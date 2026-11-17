// macos_qwen3next_stream.cpp — M-MAC-15/16/17 streaming inference for Qwen3-Coder-Next
// (qwen3next arch: Gated DeltaNet + gated GQA attention + MoE) on Apple Silicon.
// Sequential (AR) decode; per-token weight streaming with recurrent-state carry.
// M-MAC-17: projections run quant-native (aliased K-quant mul_mat, x quantized to
// q8_K by ggml) wherever the verified token streams tolerate it; the expert gate
// projection stays fp32-dequantized because q8_K x-quantization there flips the
// preserved M-MAC-15/16 outputs. Expert/head matvecs are batched into few graphs
// per layer over a persistent 6-thread pool.
//
// usage: macos-qwen3next-stream MODEL.gguf N_GEN TOKEN_IDS
#include "ggml.h"
#include "gguf.h"
#include "ggml-cpu.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <unistd.h>
#include <vector>
#include <algorithm>
#include <chrono>
#include <arm_neon.h>

static bool pread_full(int fd, void * dst, size_t n, off_t off) {
    char * p = (char *) dst;
    while (n > 0) {
        ssize_t r = pread(fd, p, n, off);
        if (r <= 0) return false;
        p += r; n -= (size_t) r;
    }
    return true;
}

struct Model {
    int fd = -1;
    gguf_context * g = nullptr;
    size_t data_off = 0;
    // hparams
    int n_layer = 48, n_embd = 2048, n_vocab = 151936;
    int n_head = 16, n_head_kv = 2, n_hdim = 256, n_rot = 64;
    int n_exp = 512, n_used = 10, n_ff_exp = 512, n_ff_sh = 512;
    int s_state = 128, s_group = 16, s_inner = 4096, s_dtr = 32, s_hv, conv_k = 4;
    double freq_base = 5e6;
    float eps = 1e-6f;
    int64_t bytes_read = 0;
    bool rd(void * d, size_t n, off_t off) { bytes_read += (int64_t) n; return pread_full(fd, d, n, off); }
};

static struct ggml_context * g_ctx = nullptr;
static ggml_backend_t g_backend = nullptr;
static ggml_threadpool_t g_tp = nullptr;

#include <chrono>
static double g_prof_load = 0, g_prof_compute = 0, g_prof_rd = 0, g_prof_dq = 0, g_prof_rec = 0, g_prof_attn = 0, g_prof_moe = 0, g_prof_head = 0;
static bool g_prof = false;
static struct ProfTimer { std::chrono::steady_clock::time_point t0; } g_pt;
static void prof_tick() { g_pt.t0 = std::chrono::steady_clock::now(); }
static double prof_tock() { return std::chrono::duration<double>(std::chrono::steady_clock::now() - g_pt.t0).count(); }

static void ensure_backend() {
    if (g_backend) return;
    g_backend = ggml_backend_init_by_type(GGML_BACKEND_DEVICE_TYPE_CPU, nullptr);
    if (g_backend) {
        ggml_threadpool_params tp{ };
        for (int i = 0; i < GGML_MAX_N_THREADS; ++i) tp.cpumask[i] = false;
        tp.n_threads = getenv("Q3N_THREADS") ? atoi(getenv("Q3N_THREADS")) : 6;
        tp.prio = GGML_SCHED_PRIO_NORMAL;
        tp.poll = getenv("Q3N_POLL") ? atoi(getenv("Q3N_POLL")) : 0;
        tp.strict_cpu = false;
        tp.paused = false;
        g_tp = ggml_threadpool_new(&tp);
        if (g_tp) ggml_backend_cpu_set_threadpool(g_backend, g_tp);
    }
}
static bool gcompute(ggml_tensor * out) {
    ggml_cgraph * gf = ggml_new_graph(g_ctx);
    ggml_build_forward_expand(gf, out);
    ensure_backend();
    return g_backend && ggml_backend_graph_compute(g_backend, gf) == GGML_STATUS_SUCCESS;
}
static void ctx_reset() {
    ggml_free(g_ctx);
    ggml_init_params ip{ 768 * 1024 * 1024, nullptr, false };
    g_ctx = ggml_init(ip);
}

static ggml_tensor * f32t(const float * d, int64_t n) {
    ggml_tensor * t = ggml_new_tensor(g_ctx, GGML_TYPE_F32, 1, &n);
    memcpy(t->data, d, n * 4);
    return t;
}
// y = W (as [out,in] numpy) @ x ; Wf holds W_outin rows contiguous [out][in]
static bool matvec(const float * Wf, int64_t out, int64_t in, const float * x, std::vector<float> & y) {
    y.resize(out);
    ggml_tensor * w = ggml_new_tensor(g_ctx, GGML_TYPE_F32, 2, (int64_t[]){in, out});
    memcpy(w->data, Wf, out * in * 4);   // ggml [in, out]: element (i,o) at i + o*in == Wf[o*in + i] == W[o][i]
    ggml_tensor * xx = ggml_reshape_2d(g_ctx, f32t(x, in), in, 1);
    ggml_tensor * r = ggml_mul_mat(g_ctx, w, xx);
    double tload = prof_tock();
    prof_tick();
    if (!gcompute(r)) return false;
    double tc = prof_tock();
    g_prof_load += tload; g_prof_compute += tc;
    memcpy(y.data(), r->data, out * 4);
    ctx_reset();
    return true;
}

static std::vector<uint8_t> g_stage;
static std::vector<uint8_t> g_stage2;
static std::vector<uint8_t> g_stage3;
static std::vector<uint8_t> g_stage4;

// M-MAC-18 bounded prefetch: kernel-driven readahead of the NEXT layer's static
// weights via madvise(MADV_WILLNEED). No worker threads; the mapping is never
// dereferenced (no RSS); page-cache growth is bounded by the per-layer window.
static bool g_prefetch = false;
static char * g_map = nullptr;
static std::vector<std::pair<size_t, size_t>> g_layer_range;

// Quant-native matvec: staged quant bytes -> typed ggml tensor (aliased) -> mul_mat.
static bool qmatvec(Model & m, const char * name, int64_t out, int64_t in, const float * x, std::vector<float> & y) {
    int id = gguf_find_tensor(m.g, name);
    if (id < 0) { fprintf(stderr, "NO_TENSOR %s\n", name); return false; }
    const int64_t * ne = gguf_get_tensor_ne(m.g, id);
    enum ggml_type ty = gguf_get_tensor_type(m.g, id);
    size_t nb = ggml_row_size(ty, ne[0]);
    for (int i = 1; i < GGML_MAX_DIMS && ne[i] > 0; ++i) nb *= (size_t) ne[i];
    if (ne[0] != in || ne[1] != out) { fprintf(stderr, "SHAPE %s\n", name); return false; }
    if (g_stage.size() < nb) g_stage.resize(nb);
    if (!m.rd(g_stage.data(), nb, (off_t)(m.data_off + gguf_get_tensor_offset(m.g, id)))) { fprintf(stderr, "RD_FAIL %s\n", name); return false; }
    ggml_tensor * w = ggml_new_tensor(g_ctx, ty, 2, (int64_t[]){in, out});
    w->data = (void *) g_stage.data();
    ggml_tensor * xx = ggml_reshape_2d(g_ctx, f32t(x, in), in, 1);
    ggml_tensor * r = ggml_mul_mat(g_ctx, w, xx);
    if (!gcompute(r)) return false;
    y.resize(out);
    memcpy(y.data(), r->data, out * 4);
    ctx_reset();
    return true;
}
// Quant-native matvec over a row-range of a tensor viewed as 2-D [out, in]:
// reads rows [first_row, first_row+out) (expert e's slice == rows [e*out, (e+1)*out)).
static bool qmatvec_slice(Model & m, int id, int64_t out, int64_t in, int64_t first_row, const float * x, std::vector<float> & y) {
    enum ggml_type ty = gguf_get_tensor_type(m.g, id);
    size_t row_b = ggml_row_size(ty, in);
    size_t nb = row_b * (size_t) out;
    if (g_stage.size() < nb) g_stage.resize(nb);
    if (!m.rd(g_stage.data(), nb, (off_t)(m.data_off + gguf_get_tensor_offset(m.g, id) + (size_t) first_row * row_b))) return false;
    ggml_tensor * w = ggml_new_tensor(g_ctx, ty, 2, (int64_t[]){in, out});
    w->data = (void *) g_stage.data();
    ggml_tensor * xx = ggml_reshape_2d(g_ctx, f32t(x, in), in, 1);
    ggml_tensor * r = ggml_mul_mat(g_ctx, w, xx);
    if (!gcompute(r)) return false;
    y.resize(out);
    memcpy(y.data(), r->data, out * 4);
    ctx_reset();
    return true;
}
// M-MAC-18: bit-exact NEON Q4_K row dequant. Matches ggml dequantize_row_q4_K
// bit-for-bit: d*sc and d*n are exact in fp32 (<=21 mantissa bits), so the only
// rounding is the final subtract, performed identically (explicit mul then sub —
// no FMA contraction). Validated against the library at runtime before first use.
static inline void get_scale_min_k4_18(int j, const uint8_t * q, uint8_t * d, uint8_t * m) {
    if (j < 4) { *d = q[j] & 63; *m = q[j + 4] & 63; }
    else { *d = (q[j + 4] & 0xF) | ((q[j - 4] >> 6) << 4); *m = (q[j + 4] >> 4) | ((q[j] >> 6) << 4); }
}
static inline void dq32_neon(const uint8_t * q, bool high, float dv, float mv, float * y) {
    uint8x16_t b0 = vld1q_u8(q), b1 = vld1q_u8(q + 16);
    if (high) { b0 = vshrq_n_u8(b0, 4); b1 = vshrq_n_u8(b1, 4); }
    else      { b0 = vandq_u8(b0, vdupq_n_u8(0xF)); b1 = vandq_u8(b1, vdupq_n_u8(0xF)); }
    float32x4_t dvv = vdupq_n_f32(dv), mvv = vdupq_n_f32(mv);
    uint16x8_t w0 = vmovl_u8(vget_low_u8(b0)), w1 = vmovl_u8(vget_high_u8(b0));
    uint16x8_t w2 = vmovl_u8(vget_low_u8(b1)), w3 = vmovl_u8(vget_high_u8(b1));
    float32x4_t f[8];
    f[0] = vcvtq_f32_u32(vmovl_u16(vget_low_u16(w0)));  f[1] = vcvtq_f32_u32(vmovl_u16(vget_high_u16(w0)));
    f[2] = vcvtq_f32_u32(vmovl_u16(vget_low_u16(w1)));  f[3] = vcvtq_f32_u32(vmovl_u16(vget_high_u16(w1)));
    f[4] = vcvtq_f32_u32(vmovl_u16(vget_low_u16(w2)));  f[5] = vcvtq_f32_u32(vmovl_u16(vget_high_u16(w2)));
    f[6] = vcvtq_f32_u32(vmovl_u16(vget_low_u16(w3)));  f[7] = vcvtq_f32_u32(vmovl_u16(vget_high_u16(w3)));
    for (int i = 0; i < 8; ++i) vst1q_f32(y + 4 * i, vsubq_f32(vmulq_f32(dvv, f[i]), mvv));
}
static void dq4K_neon(const uint8_t * src, float * dst, int64_t k) {
    const int nb = (int)(k / 256);
    for (int b = 0; b < nb; ++b) {
        const uint8_t * blk = src + (size_t)b * 144;
        float d = ggml_fp16_to_fp32(((const ggml_fp16_t *) blk)[0]);
        float dmin = ggml_fp16_to_fp32(((const ggml_fp16_t *) blk)[1]);
        uint8_t sc[8], mn[8];
        for (int j = 0; j < 8; ++j) get_scale_min_k4_18(j, blk + 4, &sc[j], &mn[j]);
        const uint8_t * qs = blk + 16;
        float * y = dst + (size_t)b * 256;
        for (int g = 0; g < 4; ++g) {
            dq32_neon(qs + g * 32, false, d * sc[2 * g],     dmin * mn[2 * g],     y + g * 64);
            dq32_neon(qs + g * 32, true,  d * sc[2 * g + 1], dmin * mn[2 * g + 1], y + g * 64 + 32);
        }
    }
}
static int g_neon_dq = -1;   // -1 unknown, 1 validated bit-exact, 0 fallback
// one-time validation: full staged gate buffer, NEON vs library to_float
static void validate_neon_dq(enum ggml_type ty, const uint8_t * src, float * dst, int64_t n) {
    if (g_neon_dq >= 0) return;
    g_neon_dq = 0;
    if (ty != GGML_TYPE_Q4_K) { fprintf(stderr, "NEON_DQ disabled (type %d)\n", (int) ty); return; }
    std::vector<float> ref(n);
    ggml_get_type_traits(ty)->to_float(src, ref.data(), n);
    dq4K_neon(src, dst, n);
    if (memcmp(ref.data(), dst, (size_t) n * 4) == 0) { g_neon_dq = 1; fprintf(stderr, "NEON_DQ validated bit-exact\n"); }
    else fprintf(stderr, "NEON_DQ MISMATCH -> library fallback\n");
}

struct QM { const char * name; int64_t out, in; std::vector<float> * y; };
// k quant matvecs sharing one input x in a SINGLE graph: one threadpool dispatch and
// one ctx_reset instead of k. Per-row math is identical to sequential qmatvec calls.
static bool qmatvec_multi(Model & m, const QM * ops, int k, const float * x) {
    size_t sz[8]; int id[8]; enum ggml_type tyv[8];
    if (k < 1 || k > 8) return false;
    size_t tot = 0;
    for (int i = 0; i < k; ++i) {
        id[i] = gguf_find_tensor(m.g, ops[i].name);
        if (id[i] < 0) { fprintf(stderr, "NO_TENSOR %s\n", ops[i].name); return false; }
        const int64_t * ne = gguf_get_tensor_ne(m.g, id[i]);
        tyv[i] = gguf_get_tensor_type(m.g, id[i]);
        size_t nb = ggml_row_size(tyv[i], ne[0]);
        for (int j = 1; j < GGML_MAX_DIMS && ne[j] > 0; ++j) nb *= (size_t) ne[j];
        if (ne[0] != ops[i].in || ne[1] != ops[i].out) { fprintf(stderr, "SHAPE %s\n", ops[i].name); return false; }
        sz[i] = nb; tot += nb;
    }
    if (g_stage4.size() < tot) g_stage4.resize(tot);
    ggml_tensor * xx = ggml_reshape_2d(g_ctx, f32t(x, ops[0].in), ops[0].in, 1);
    ggml_tensor * rs[8];
    size_t off = 0;
    for (int i = 0; i < k; ++i) {
        if (!m.rd(g_stage4.data() + off, sz[i], (off_t)(m.data_off + gguf_get_tensor_offset(m.g, id[i])))) { fprintf(stderr, "RD_FAIL %s\n", ops[i].name); return false; }
        ggml_tensor * w = ggml_new_tensor(g_ctx, tyv[i], 2, (int64_t[]){ops[i].in, ops[i].out});
        w->data = (void *)(g_stage4.data() + off);
        off += sz[i];
        rs[i] = ggml_mul_mat(g_ctx, w, xx);
    }
    ggml_cgraph * gf = ggml_new_graph(g_ctx);
    for (int i = 0; i < k; ++i) ggml_build_forward_expand(gf, rs[i]);
    ensure_backend();
    if (!g_backend || ggml_backend_graph_compute(g_backend, gf) != GGML_STATUS_SUCCESS) { fprintf(stderr, "MULTI_COMPUTE_FAIL k=%d\n", k); return false; }
    for (int i = 0; i < k; ++i) {
        ops[i].y->resize((size_t) ops[i].out);
        memcpy(ops[i].y->data(), rs[i]->data, (size_t) ops[i].out * 4);
    }
    ctx_reset();
    return true;
}
// load tensor bytes and dequantize to f32 in [out,in] row-major (numpy) order
static bool load_f32(Model & m, const char * name, std::vector<float> & out, bool outin = true) {
    int id = gguf_find_tensor(m.g, name);
    if (id < 0) { fprintf(stderr, "NO_TENSOR %s\n", name); return false; }
    const int64_t * ne = gguf_get_tensor_ne(m.g, id);
    enum ggml_type ty = gguf_get_tensor_type(m.g, id);
    int64_t n = 1, dims = 0;
    for (int i = 0; i < GGML_MAX_DIMS && ne[i] > 0; ++i) { n *= ne[i]; dims++; }
    size_t nb = ggml_row_size(ty, ne[0]);
    for (int i = 1; i < dims; ++i) nb *= (size_t) ne[i];
    prof_tick();
    g_stage.resize(nb);
    if (!m.rd(g_stage.data(), nb, (off_t)(m.data_off + gguf_get_tensor_offset(m.g, id)))) { fprintf(stderr, "RD_FAIL %s\n", name); return false; }
    g_prof_rd += prof_tock(); prof_tick();
    out.resize(n);
    if (ty == GGML_TYPE_F32) {
        memcpy(out.data(), g_stage.data(), n * 4);
    } else {
        const ggml_type_traits * tr = ggml_get_type_traits(ty);
        if (!tr || !tr->to_float) { fprintf(stderr, "NO_TO_FLOAT %s type=%d\n", name, (int) ty); return false; }
        tr->to_float(g_stage.data(), out.data(), n);
    }
    g_prof_dq += prof_tock(); prof_tick();
    return true;
}

int main(int argc, char ** argv) {
    if (argc != 4) { fprintf(stderr, "usage: %s MODEL N_GEN IDS\n", argv[0]); return 2; }
    const int n_gen = atoi(argv[2]);
    std::vector<int> ids;
    for (const char * p = argv[3]; *p;) {
        int v = 0, dg = 0;
        while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); ++p; ++dg; }
        if (!dg) return 2;
        ids.push_back(v);
        if (*p == ',') ++p; else if (*p) return 2;
    }
    if (ids.empty() || n_gen < 1) return 2;

    gguf_init_params gp { true, nullptr };
    Model m;
    m.fd = open(argv[1], O_RDONLY | O_CLOEXEC);
    if (m.fd < 0) { perror("open"); return 1; }
    m.g = gguf_init_from_file(argv[1], gp);
    if (!m.g) { fprintf(stderr, "GGUF_FAIL\n"); return 1; }
    m.data_off = gguf_get_data_offset(m.g);
    // bounded prefetch setup (Q3N_PREFETCH=1): mmap file for madvise + per-layer ranges
    g_prefetch = getenv("Q3N_PREFETCH") != nullptr;
    if (g_prefetch) {
        struct stat fst;
        if (fstat(m.fd, &fst) == 0 && fst.st_size > 0) {
            g_map = (char *) mmap(nullptr, (size_t) fst.st_size, PROT_READ, MAP_PRIVATE, m.fd, 0);
            if (g_map == MAP_FAILED) { g_map = nullptr; g_prefetch = false; fprintf(stderr, "PREFETCH_DISABLED\n"); }
        } else { g_prefetch = false; fprintf(stderr, "PREFETCH_DISABLED\n"); }
    }
    if (g_prefetch) {
        static const char * stat_names[] = {
            "attn_norm.weight", "attn_qkv.weight", "attn_gate.weight", "ssm_ba.weight",
            "ssm_conv1d.weight", "ssm_dt.bias", "ssm_a", "ssm_norm.weight", "ssm_out.weight",
            "attn_q.weight", "attn_k.weight", "attn_v.weight", "attn_output.weight",
            "attn_q_norm.weight", "attn_k_norm.weight", "post_attention_norm.weight",
            "ffn_gate_inp.weight", "ffn_gate_shexp.weight", "ffn_up_shexp.weight",
            "ffn_down_shexp.weight", "ffn_gate_inp_shexp.weight", nullptr };
        g_layer_range.assign(m.n_layer, {0, 0});
        for (int l = 0; l < m.n_layer; ++l) {
            size_t lo = (size_t) -1, hi = 0;
            char tn[160];
            for (int s = 0; stat_names[s]; ++s) {
                snprintf(tn, sizeof tn, "blk.%d.%s", l, stat_names[s]);
                int id = gguf_find_tensor(m.g, tn);
                if (id < 0) continue;
                const int64_t * ne = gguf_get_tensor_ne(m.g, id);
                size_t off = (size_t) gguf_get_tensor_offset(m.g, id);
                size_t nb = ggml_row_size(gguf_get_tensor_type(m.g, id), ne[0]);
                for (int j = 1; j < GGML_MAX_DIMS && ne[j] > 0; ++j) nb *= (size_t) ne[j];
                lo = std::min(lo, off); hi = std::max(hi, off + nb);
            }
            if (l == m.n_layer - 1) {
                int oid2 = gguf_find_tensor(m.g, "output.weight");
                if (oid2 >= 0) {
                    const int64_t * ne = gguf_get_tensor_ne(m.g, oid2);
                    size_t off = (size_t) gguf_get_tensor_offset(m.g, oid2);
                    size_t nb = ggml_row_size(gguf_get_tensor_type(m.g, oid2), ne[0]);
                    for (int j = 1; j < GGML_MAX_DIMS && ne[j] > 0; ++j) nb *= (size_t) ne[j];
                    lo = std::min(lo, off); hi = std::max(hi, off + nb);
                }
            }
            if (lo != (size_t) -1) g_layer_range[l] = {lo, hi - lo};
        }
    }
    auto gi = [&](const char * k) -> int64_t {
        int i = gguf_find_key(m.g, k);
        if (i < 0) return 0;
        switch (gguf_get_kv_type(m.g, i)) {
            case GGUF_TYPE_UINT32: return (int64_t) gguf_get_val_u32(m.g, i);
            case GGUF_TYPE_INT32:  return (int64_t) gguf_get_val_i32(m.g, i);
            case GGUF_TYPE_UINT64: return (int64_t) gguf_get_val_u64(m.g, i);
            case GGUF_TYPE_INT64:  return gguf_get_val_i64(m.g, i);
            default: return 0;
        }
    };
    m.n_layer = (int) gi("qwen3next.block_count");
    m.n_embd = (int) gi("qwen3next.embedding_length");
    m.n_vocab = (int) gi("qwen3next.vocab_size"); if (!m.n_vocab) m.n_vocab = 151936;
    m.n_head = (int) gi("qwen3next.attention.head_count");
    m.n_head_kv = (int) gi("qwen3next.attention.head_count_kv");
    m.n_hdim = (int) gi("qwen3next.attention.key_length");
    m.n_rot = (int) gi("qwen3next.rope.dimension_count");
    m.n_exp = (int) gi("qwen3next.expert_count");
    m.n_used = (int) gi("qwen3next.expert_used_count");
    m.n_ff_exp = (int) gi("qwen3next.expert_feed_forward_length");
    m.n_ff_sh = (int) gi("qwen3next.expert_shared_feed_forward_length");
    m.s_state = (int) gi("qwen3next.ssm.state_size");
    m.s_group = (int) gi("qwen3next.ssm.group_count");
    m.s_inner = (int) gi("qwen3next.ssm.inner_size");
    m.s_dtr = (int) gi("qwen3next.ssm.time_step_rank");
    m.s_hv = m.s_inner / m.s_dtr;
    m.conv_k = (int) gi("qwen3next.ssm.conv_kernel");
    { int fb = gguf_find_key(m.g, "qwen3next.rope.freq_base");
      m.freq_base = fb >= 0 ? (gguf_get_kv_type(m.g, fb) == GGUF_TYPE_FLOAT32 ? (double) gguf_get_val_f32(m.g, fb) : 5e6) : 5e6; }
    m.eps = 1e-6f;
    fprintf(stderr, "Q3N layer=%d embd=%d head=%d/%d hdim=%d rot=%d exp=%d/%d ff=%d/%d ssm=%d/%d/%d/%d conv=%d\n",
        m.n_layer, m.n_embd, m.n_head, m.n_head_kv, m.n_hdim, m.n_rot, m.n_exp, m.n_used,
        m.n_ff_exp, m.n_ff_sh, m.s_state, m.s_group, m.s_inner, m.s_dtr, m.conv_k);

    ggml_init_params ip{ 512 * 1024 * 1024, nullptr, false };
    g_ctx = ggml_init(ip);

    const int E = m.n_embd, HD = m.n_hdim;
    // recurrent + kv state
    const int NREC = (m.n_layer / 4) * 3 + (m.n_layer % 4 ? m.n_layer % 4 - 1 : 0); // not used; per-block flag below
    std::vector<char> is_rec(m.n_layer);
    std::vector<float> conv_state, ssm_state;   // per recr block: [conv_k-1, convdim] + [dtr, s_state, s_hv]
    std::vector<float> kv_k, kv_v;              // per attn block: [n_head_kv, HD, CTX]
    const int CTX = 8192;
    int convdim = 0, n_rec = 0, n_attn = 0;
    for (int l = 0; l < m.n_layer; ++l) {
        char nm[64]; snprintf(nm, sizeof nm, "blk.%d.ssm_a", l);
        is_rec[l] = gguf_find_tensor(m.g, nm) >= 0;
        if (is_rec[l]) n_rec++; else n_attn++;
    }
    convdim = m.s_state * m.s_group * 2 + m.s_inner;
    conv_state.resize((size_t) n_rec * (m.conv_k - 1) * convdim, 0.0f);
    ssm_state.resize((size_t) n_rec * m.s_dtr * m.s_state * m.s_hv, 0.0f);
    kv_k.resize((size_t) n_attn * m.n_head_kv * HD * CTX, 0.0f);
    kv_v.resize((size_t) n_attn * m.n_head_kv * HD * CTX, 0.0f);
    fprintf(stderr, "rec=%d attn=%d convdim=%d state_mb=%.1f kv_mb=%.1f\n", n_rec, n_attn, convdim,
        (conv_state.size() + ssm_state.size()) * 4 / 1e6, (kv_k.size() + kv_v.size()) * 4 / 1e6);

    // embedding row via 256-blocks
    int eid = gguf_find_tensor(m.g, "token_embd.weight");
    const int64_t * ene = gguf_get_tensor_ne(m.g, eid);
    enum ggml_type ety = gguf_get_tensor_type(m.g, eid);
    const ggml_type_traits * etr = ggml_get_type_traits(ety);
    size_t erow_b = ggml_row_size(ety, ene[0]);
    size_t emb_base = m.data_off + gguf_get_tensor_offset(m.g, eid);
    auto emb_row = [&](int tok, float * x) {
        const int blocks = (int)(ene[0] / 256);
        std::vector<uint8_t> blk(ggml_row_size(ety, 256));
        std::vector<float> bv(256);
        for (int b = 0; b < blocks; ++b) {
            if (!m.rd(blk.data(), blk.size(), (off_t)(emb_base + ((size_t) tok * blocks + b) * ggml_row_size(ety, 256)))) { fprintf(stderr, "EMB_FAIL\n"); exit(1); }
            if (ety == GGML_TYPE_F32) memcpy(bv.data(), blk.data(), 256 * 4);
            else etr->to_float(blk.data(), bv.data(), 256);
            memcpy(x + (size_t) b * 256, bv.data(), 256 * 4);
        }
    };


    std::vector<float> hidden(E), cur(E), normed(E), tmp1, tmp2;
    std::vector<float> logits(m.n_vocab);
    std::vector<float> wbuf, xbuf, router, wg, wu, wd, wq, wk, wv, wo;
    std::vector<float> qf, kf, vf, ba, gate, beta, a_qkv, a_gate, ssmout, convw, dtb, sa, ssnorm;
    int rec_slot = 0, attn_slot = 0;

    auto rms = [&](const float * x, const float * w, float * y, int n) {
        double s = 0; for (int i = 0; i < n; ++i) s += (double) x[i] * x[i];
        float inv = 1.0f / sqrtf((float)(s / n) + m.eps);
        for (int i = 0; i < n; ++i) y[i] = x[i] * inv * w[i];
    };
    auto sigmoid = [](float x) { return 1.0f / (1.0f + expf(-x)); };

    auto forward = [&](int tok, int pos) -> bool {
        rec_slot = 0; attn_slot = 0;
        emb_row(tok, hidden.data());
        (void) 0;
        if (getenv("Q3N_TRACE0") && pos == 0) fwrite(hidden.data(), 4, E, fopen("/tmp/ad_emb0.f32", "wb"));
        for (int l = 0; l < m.n_layer; ++l) {
            if (g_prefetch && l + 1 < m.n_layer) {
                size_t lo = g_layer_range[l + 1].first, len = g_layer_range[l + 1].second;
                if (len) {
                    size_t a = (m.data_off + lo) & ~((size_t) 4095);
                    size_t e2 = ((m.data_off + lo + len) + 4095) & ~((size_t) 4095);
                    if (e2 > a && e2 <= (size_t) -1) madvise(g_map + a, e2 - a, MADV_WILLNEED);
                }
            }
            if (getenv("Q3N_TRACE")) fprintf(stderr, "L%d %s\n", l, is_rec[l] ? "rec" : "attn");
            char nm[128];
            auto LD = [&](const char * suffix, std::vector<float> & dst, bool outin = true) {
                snprintf(nm, sizeof nm, "blk.%d.%s", l, suffix);
                return load_f32(m, nm, dst, outin);
            };
            if (!LD("attn_norm.weight", wbuf, false)) return false;
            rms(hidden.data(), wbuf.data(), normed.data(), E);
            if (is_rec[l]) {
                const int kd = m.s_state * m.s_group, vd = m.s_inner, cd = 2 * kd + vd;
                if (getenv("Q3N_TRACE")) fprintf(stderr, "  L%d load qkv\n", l);
                if (getenv("Q3N_TRACE0") && l == 0) {
                    fwrite(normed.data(), 4, E, fopen("/tmp/ad_normed0.f32", "wb"));
                }
                if (!LD("ssm_ba.weight", ba)) return false;            // [E, 2*dtr]
                if (!LD("ssm_conv1d.weight", convw)) return false; // -> [cd][conv_k] rows, index [c*conv_k + j]
                if (!LD("ssm_dt.bias", dtb, false)) return false;
                if (!LD("ssm_a", sa, false)) return false;
                if (!LD("ssm_norm.weight", ssnorm, false)) return false;
                // projections (quant-native, one graph)
                {
                    char nmq[128], nmg[128];
                    snprintf(nmq, sizeof nmq, "blk.%d.attn_qkv.weight", l);
                    snprintf(nmg, sizeof nmg, "blk.%d.attn_gate.weight", l);
                    QM ops[2] = { {nmq, cd, E, &tmp1}, {nmg, vd, E, &tmp2} };
                    if (!qmatvec_multi(m, ops, 2, normed.data())) return false;   // qkv[c], z
                }
                std::vector<float> & z = tmp2;
                if (getenv("Q3N_TRACE")) fprintf(stderr, "  L%d proj done\n", l);
                if (!matvec(ba.data(), 2 * m.s_dtr, E, normed.data(), qf)) return false;
                if (getenv("Q3N_TRACE")) fprintf(stderr, "  L%d ba done\n", l);
                // conv: state carry
                prof_tick();
                float * cst = conv_state.data() + (size_t) rec_slot * (m.conv_k - 1) * cd;
                // window: [state(ck-1); current]
                std::vector<float> conv_in((m.conv_k - 1) + 1);
                std::vector<float> convout(cd);
                for (int c = 0; c < cd; ++c) {
                    float acc = 0;
                    for (int j = 0; j < m.conv_k - 1; ++j) acc += convw[(size_t) c * m.conv_k + j] * cst[(size_t) j * cd + c];
                    acc += convw[(size_t) c * m.conv_k + (m.conv_k - 1)] * tmp1[c];
                    convout[c] = acc * sigmoid(acc);
                }
                // shift state
                memmove(cst, cst + cd, (size_t)(m.conv_k - 2) * cd * 4);
                memcpy(cst + (size_t)(m.conv_k - 2) * cd, tmp1.data(), (size_t) cd * 4);
                if (getenv("Q3N_TRACE0") && l == 0) {
                    fwrite(tmp1.data(), 4, cd, fopen("/tmp/ad_qkv0.f32", "wb"));
                    fwrite(convout.data(), 4, cd, fopen("/tmp/ad_conv0.f32", "wb"));
                }
                // split + L2 norm q,k (per head over s_state)
                const int SS = m.s_state, SG = m.s_group, DTR = m.s_dtr, HV = m.s_hv;
                // convout channels: [q kd | k kd | v vd], head-minor within each
                auto l2 = [&](const float * seg, int heads, int dim, float * dstt) {
                    for (int g = 0; g < heads; ++g) {
                        double s = 0;
                        for (int i = 0; i < dim; ++i) { float v = seg[(size_t) g * dim + i]; s += (double) v * v; }
                        float inv = 1.0f / sqrtf((float) s + m.eps);
                        for (int i = 0; i < dim; ++i) dstt[(size_t) g * dim + i] = seg[(size_t) g * dim + i] * inv;
                    }
                };
                std::vector<float> qq((size_t) kd), kk((size_t) kd), vv((size_t) vd);
                l2(convout.data(), SG, SS, qq.data());
                l2(convout.data() + kd, SG, SS, kk.data());
                memcpy(vv.data(), convout.data() + 2 * kd, (size_t) vd * 4);
                // beta/alpha per v-head
                std::vector<float> dbg_b(DTR), dbg_g(DTR);
                float * S = ssm_state.data() + (size_t) rec_slot * DTR * SS * HV;
                const float scale = 1.0f / sqrtf((float) SS);
                std::vector<float> o((size_t) vd, 0.0f);
                for (int h = 0; h < DTR; ++h) {
                    int g = h / (DTR / SG);      // v-head h uses k-head h // (DTR/SG)
                    const float * qh = qq.data() + (size_t) g * SS;
                    const float * kh = kk.data() + (size_t) g * SS;
                    const float * vh = vv.data() + (size_t) h * HV;
                    float b = sigmoid(qf[h]);
                    float al = logf(1.0f + expf(qf[DTR + h] + dtb[h]));
                    float gg2 = al * sa[h];
                    dbg_b[h] = b; dbg_g[h] = gg2;
                    float * Sh = S + (size_t) h * SS * HV;   // [SS i][HV j]
                    float dec = expf(gg2);
                    std::vector<float> u(HV);
                    for (int j = 0; j < HV; ++j) {
                        float s = 0;
                        for (int i = 0; i < SS; ++i) s += Sh[(size_t) i * HV + j] * kh[i];
                        u[j] = (vh[j] - s) * b;
                    }
                    for (int i = 0; i < SS; ++i)
                        for (int j = 0; j < HV; ++j)
                            Sh[(size_t) i * HV + j] = Sh[(size_t) i * HV + j] * dec + kh[i] * u[j];
                    for (int j = 0; j < HV; ++j) {
                        float s = 0;
                        for (int i = 0; i < SS; ++i) s += Sh[(size_t) i * HV + j] * qh[i];
                        o[(size_t) h * HV + j] = s * scale;
                    }
                }
                if (getenv("Q3N_TRACE0") && l == 0) {
                    fwrite(o.data(), 4, vd, fopen("/tmp/ad_oraw0.f32", "wb"));
                    fwrite(tmp2.data(), 4, vd, fopen("/tmp/ad_z0.f32", "wb"));
                    fwrite(qq.data(), 4, kd, fopen("/tmp/ad_ql2.f32", "wb"));
                    fwrite(kk.data(), 4, kd, fopen("/tmp/ad_kl2.f32", "wb"));
                    fwrite(vv.data(), 4, vd, fopen("/tmp/ad_v.f32", "wb"));
                    fwrite(dbg_b.data(), 4, DTR, fopen("/tmp/ad_beta.f32", "wb"));
                    fwrite(dbg_g.data(), 4, DTR, fopen("/tmp/ad_gate.f32", "wb"));
                }
                rec_slot++;
                if (getenv("Q3N_TRACE")) fprintf(stderr, "  L%d gdn done\n", l);
                // gated norm: rms(o per HV) * ssnorm, then * silu(z); z channels (i + h*HV)
                for (int h = 0; h < DTR; ++h) {
                    double s = 0;
                    for (int i = 0; i < HV; ++i) { float v = o[(size_t) h * HV + i]; s += (double) v * v; }
                    float inv = 1.0f / sqrtf((float)(s / HV) + m.eps);
                    for (int i = 0; i < HV; ++i) {
                        float zz = z[(size_t) h * HV + i];
                        o[(size_t) h * HV + i] = o[(size_t) h * HV + i] * inv * ssnorm[i] * (zz / (1.0f + expf(-zz)));
                    }
                }
                if (getenv("Q3N_TRACE0") && l == 0) {
                    fwrite(o.data(), 4, vd, fopen("/tmp/ad_o0.f32", "wb"));
                    fwrite(cur.data(), 4, E, fopen("/tmp/ad_fo0.f32", "wb"));
                }
                snprintf(nm, sizeof nm, "blk.%d.ssm_out.weight", l);
                if (!qmatvec(m, nm, E, vd, o.data(), cur)) return false;
                g_prof_rec += prof_tock();
            } else {
                // full gated attention
                if (!LD("attn_q_norm.weight", wbuf, false)) return false;
                std::vector<float> qn = wbuf;
                if (!LD("attn_k_norm.weight", wbuf, false)) return false;
                std::vector<float> kn = wbuf;
                const int NH = m.n_head, NKV = m.n_head_kv, G = NH / NKV;
                std::vector<float> qfull, kf2, vf2;
                {
                    char nm1[128], nm2[128], nm3[128];
                    snprintf(nm1, sizeof nm1, "blk.%d.attn_q.weight", l);
                    snprintf(nm2, sizeof nm2, "blk.%d.attn_k.weight", l);
                    snprintf(nm3, sizeof nm3, "blk.%d.attn_v.weight", l);
                    QM ops[3] = { {nm1, NH * 2 * HD, E, &qfull}, {nm2, NKV * HD, E, &kf2}, {nm3, NKV * HD, E, &vf2} };
                    if (!qmatvec_multi(m, ops, 3, normed.data())) return false;
                }
                float * ka = kv_k.data() + (size_t) attn_slot * NKV * HD * CTX;
                float * va = kv_v.data() + (size_t) attn_slot * NKV * HD * CTX;
                // per kv-head: norm k, partial rope on q/k, store k/v
                auto rope = [&](float * v, int pos2) {
                    for (int i = 0; i < m.n_rot / 2; ++i) {
                        float freq = (float)(1.0 / pow(m.freq_base, (double) i / (m.n_rot / 2)));
                        float ang = pos2 * freq;
                        float c = cosf(ang), sn = sinf(ang);
                        float a = v[i], b = v[i + m.n_rot / 2];
                        v[i] = a * c - b * sn;
                        v[i + m.n_rot / 2] = b * c + a * sn;
                    }
                };
                prof_tick();
                std::vector<float> knr(HD);
                for (int kh2 = 0; kh2 < NKV; ++kh2) {
                    float * krow = ka + (size_t) kh2 * HD * CTX;   // [i][t]
                    float * vrow = va + (size_t) kh2 * HD * CTX;   // [i][t]
                    float * k = kf2.data() + (size_t) kh2 * HD;
                    rms(k, kn.data(), knr.data(), HD);
                    rope(knr.data(), pos);
                    for (int i = 0; i < HD; ++i) krow[(size_t) i * CTX + pos] = knr[i];
                    for (int i = 0; i < HD; ++i) vrow[(size_t) i * CTX + pos] = vf2[(size_t) kh2 * HD + i];
                }
                std::vector<float> attnout((size_t) NH * HD, 0.0f);
                for (int h = 0; h < NH; ++h) {
                    int kh2 = h / G;
                    float * q = qfull.data() + (size_t) h * 2 * HD;
                    float * gate = q + HD;
                    rms(q, qn.data(), q, HD);   // in-place
                    rope(q, pos);
                    float * krow = ka + (size_t) kh2 * HD * CTX;
                    float * vrow = va + (size_t) kh2 * HD * CTX;
                    std::vector<float> scores(pos + 1);
                    for (int t = 0; t <= pos; ++t) {
                        float s = 0;
                        for (int i = 0; i < HD; ++i) s += krow[(size_t) i * CTX + t] * q[i];
                        scores[t] = s / sqrtf((float) HD);
                    }
                    float mx = *std::max_element(scores.begin(), scores.end());
                    double sum = 0;
                    for (int t = 0; t <= pos; ++t) { scores[t] = expf(scores[t] - mx); sum += scores[t]; }
                    for (int j = 0; j < HD; ++j) {
                        float acc = 0;
                        for (int t = 0; t <= pos; ++t) acc += scores[t] * vrow[(size_t) j * CTX + t];
                        attnout[(size_t) h * HD + j] = (float)(acc / sum) * sigmoid(gate[j]);
                    }
                }
                snprintf(nm, sizeof nm, "blk.%d.attn_output.weight", l);
                if (!qmatvec(m, nm, E, NH * HD, attnout.data(), cur)) return false;
                attn_slot++;
                g_prof_attn += prof_tock();
            }
            // residual
            for (int i = 0; i < E; ++i) hidden[i] += cur[i];
            prof_tick();
            if (!LD("post_attention_norm.weight", wbuf, false)) return false;
            rms(hidden.data(), wbuf.data(), normed.data(), E);
            // MoE: router + shared-expert gate/up in ONE graph (all consume normed)
            std::vector<float> rl, sg1, su1;
            {
                char nm1[128], nm2[128], nm3[128];
                snprintf(nm1, sizeof nm1, "blk.%d.ffn_gate_inp.weight", l);
                snprintf(nm2, sizeof nm2, "blk.%d.ffn_gate_shexp.weight", l);
                snprintf(nm3, sizeof nm3, "blk.%d.ffn_up_shexp.weight", l);
                QM ops[3] = { {nm1, m.n_exp, E, &rl}, {nm2, m.n_ff_sh, E, &sg1}, {nm3, m.n_ff_sh, E, &su1} };
                if (!qmatvec_multi(m, ops, 3, normed.data())) return false;
            }
            std::vector<int> sel(m.n_used);
            { std::vector<int> idx(m.n_exp); for (int i = 0; i < m.n_exp; ++i) idx[i] = i;
              std::partial_sort(idx.begin(), idx.begin() + m.n_used, idx.end(), [&](int a, int b){ return rl[a] > rl[b]; });
              for (int i = 0; i < m.n_used; ++i) sel[i] = idx[i]; }
            if (getenv("Q3N_TRACE0") && l == 0) {
                fprintf(stderr, "Q3N selected experts:");
                for (int i2 = 0; i2 < m.n_used; ++i2) fprintf(stderr, " %d", sel[i2]);
                fprintf(stderr, "\n");
            }
            if (getenv("Q3N_TRACE0") && l == 0) {
                fwrite(normed.data(), 4, E, fopen("/tmp/ad_fnorm0.f32", "wb"));
                fprintf(stderr, "Q3N router logits[:8]:");
                for (int i2 = 0; i2 < 8; ++i2) fprintf(stderr, " %.4f", rl[i2]);
                fprintf(stderr, "\n");
            }
            double mx = -1e30; for (float v : rl) mx = std::max(mx, (double) v);
            double dsum = 0; std::vector<double> pr(m.n_exp);
            for (int e = 0; e < m.n_exp; ++e) { pr[e] = exp((double) rl[e] - mx); dsum += pr[e]; }
            std::vector<float> mw(m.n_used);
            double wsum = 0;
            for (int i = 0; i < m.n_used; ++i) { mw[i] = (float)(pr[sel[i]] / dsum); wsum += mw[i]; }
            for (int i = 0; i < m.n_used; ++i) mw[i] = (float)(mw[i] / wsum);
            // expert slices from 3-D tensors: gate fp32-dequant (exactness — q8_K x-quant
            // there flips verified streams), up/down quant-native. Batched into two
            // graphs per layer: phase A = gate+up (shared x=normed), phase B = downs.
            snprintf(nm, sizeof nm, "blk.%d.ffn_gate_exps.weight", l);
            int idg = gguf_find_tensor(m.g, nm);
            snprintf(nm, sizeof nm, "blk.%d.ffn_up_exps.weight", l);
            int idu = gguf_find_tensor(m.g, nm);
            snprintf(nm, sizeof nm, "blk.%d.ffn_down_exps.weight", l);
            int idd = gguf_find_tensor(m.g, nm);
            if (idg < 0 || idu < 0 || idd < 0) return false;
            const int64_t FFE = m.n_ff_exp;
            enum ggml_type tyg = gguf_get_tensor_type(m.g, idg);
            enum ggml_type tyu = gguf_get_tensor_type(m.g, idu);
            enum ggml_type tyd = gguf_get_tensor_type(m.g, idd);
            size_t slice_gu = ggml_row_size(tyg, E) * (size_t) FFE;
            size_t slice_dn = ggml_row_size(tyd, FFE) * (size_t) E;
            if (g_stage2.size() < slice_gu * m.n_used) g_stage2.resize(slice_gu * m.n_used);
            if (g_stage3.size() < slice_gu * m.n_used) g_stage3.resize(slice_gu * m.n_used);
            for (int i = 0; i < m.n_used; ++i) {
                int e = sel[i];
                if (!m.rd(g_stage2.data() + (size_t)i * slice_gu, slice_gu, (off_t)(m.data_off + gguf_get_tensor_offset(m.g, idg) + (size_t)e * slice_gu))) return false;
                if (!m.rd(g_stage3.data() + (size_t)i * slice_gu, slice_gu, (off_t)(m.data_off + gguf_get_tensor_offset(m.g, idu) + (size_t)e * slice_gu))) return false;
            }
            prof_tick();
            // phase A: one graph computes all gate (fp32) and up (quant) projections
            ggml_tensor * wg_all = ggml_new_tensor(g_ctx, GGML_TYPE_F32, 2, (int64_t[]){E, FFE * m.n_used});
            if (g_neon_dq < 0) validate_neon_dq(tyg, g_stage2.data(), (float *) wg_all->data, FFE * m.n_used * E);
            else if (g_neon_dq) dq4K_neon(g_stage2.data(), (float *) wg_all->data, FFE * m.n_used * E);
            else ggml_get_type_traits(tyg)->to_float(g_stage2.data(), (float *) wg_all->data, FFE * m.n_used * E);
            ggml_tensor * wu_all = ggml_new_tensor(g_ctx, tyu, 2, (int64_t[]){E, FFE * m.n_used});
            wu_all->data = (void *) g_stage3.data();
            ggml_tensor * xx = ggml_reshape_2d(g_ctx, f32t(normed.data(), E), E, 1);
            ggml_tensor * g1all = ggml_mul_mat(g_ctx, wg_all, xx);
            ggml_tensor * u1all = ggml_mul_mat(g_ctx, wu_all, xx);
            {
                ggml_cgraph * gf = ggml_new_graph(g_ctx);
                ggml_build_forward_expand(gf, g1all);
                ggml_build_forward_expand(gf, u1all);
                if (!g_backend || ggml_backend_graph_compute(g_backend, gf) != GGML_STATUS_SUCCESS) return false;
            }
            const float * g1p = (const float *) g1all->data;
            const float * u1p = (const float *) u1all->data;
            std::vector<std::vector<float>> g1s(m.n_used);
            for (int i = 0; i < m.n_used; ++i) {
                std::vector<float> & g1 = g1s[i];
                g1.resize(FFE);
                for (int j = 0; j < FFE; ++j) { float gv = g1p[(size_t)i * FFE + j]; g1[j] = gv / (1.0f + expf(-gv)) * u1p[(size_t)i * FFE + j]; }
            }
            // phase B: one graph computes all down projections
            if (g_stage.size() < slice_dn * m.n_used) g_stage.resize(slice_dn * m.n_used);
            for (int i = 0; i < m.n_used; ++i) {
                int e = sel[i];
                if (!m.rd(g_stage.data() + (size_t)i * slice_dn, slice_dn, (off_t)(m.data_off + gguf_get_tensor_offset(m.g, idd) + (size_t)e * slice_dn))) return false;
            }
            std::vector<ggml_tensor *> douts(m.n_used);
            {
                ggml_cgraph * gf = ggml_new_graph(g_ctx);
                for (int i = 0; i < m.n_used; ++i) {
                    ggml_tensor * wd = ggml_new_tensor(g_ctx, tyd, 2, (int64_t[]){FFE, E});
                    wd->data = (void *)(g_stage.data() + (size_t)i * slice_dn);
                    ggml_tensor * xv = ggml_reshape_2d(g_ctx, f32t(g1s[i].data(), FFE), FFE, 1);
                    douts[i] = ggml_mul_mat(g_ctx, wd, xv);
                    ggml_build_forward_expand(gf, douts[i]);
                }
                if (!g_backend || ggml_backend_graph_compute(g_backend, gf) != GGML_STATUS_SUCCESS) return false;
            }
            std::vector<float> moe(E, 0.0f);
            for (int i = 0; i < m.n_used; ++i) {
                const float * d1v = (const float *) douts[i]->data;
                for (int j = 0; j < E; ++j) moe[j] += mw[i] * d1v[j];
            }
            ctx_reset();
            // shared expert (gate/up computed with router; down quant-native; fp32 gate scalar)
            if (!LD("ffn_gate_inp_shexp.weight", wbuf, false)) return false;
            {
                std::vector<float> d1v;
                for (int j = 0; j < m.n_ff_sh; ++j) { float gv = sg1[j]; sg1[j] = gv / (1.0f + expf(-gv)) * su1[j]; }
                snprintf(nm, sizeof nm, "blk.%d.ffn_down_shexp.weight", l);
                if (!qmatvec(m, nm, E, m.n_ff_sh, sg1.data(), d1v)) return false;
                double sg = 0;
                for (int i2 = 0; i2 < E; ++i2) sg += (double) wbuf[i2] * normed[i2];
                float sgf = sigmoid((float) sg);
                for (int j = 0; j < E; ++j) moe[j] += sgf * d1v[j];
            }
            for (int i = 0; i < E; ++i) hidden[i] += moe[i];
            g_prof_moe += prof_tock();
            if (getenv("Q3N_DUMP_HIDDEN")) {
                char hp[256]; snprintf(hp, sizeof hp, "%s/blk%02d.f32", getenv("Q3N_DUMP_HIDDEN"), l);
                FILE * hf = fopen(hp, "wb");
                if (hf) { fwrite(hidden.data(), 4, E, hf); fclose(hf); }
            }
        }
        // head
        prof_tick();
        if (!load_f32(m, "output_norm.weight", wbuf, false)) return false;
        rms(hidden.data(), wbuf.data(), normed.data(), E);
        int oid = gguf_find_tensor(m.g, "output.weight");
        const int64_t * one = gguf_get_tensor_ne(m.g, oid);
        enum ggml_type oty = gguf_get_tensor_type(m.g, oid);
        size_t orow = ggml_row_size(oty, one[0]);
        size_t obase = m.data_off + gguf_get_tensor_offset(m.g, oid);
        const int64_t nv = one[1];
        for (int64_t base = 0; base < nv; base += 8192) {
            int64_t rows = std::min<int64_t>(8192, nv - base);
            std::vector<float> lg2;
            if (!qmatvec_slice(m, oid, rows, one[0], base, normed.data(), lg2)) return false;
            memcpy(logits.data() + base, lg2.data(), (size_t) rows * 4);
        }
        g_prof_head += prof_tock();
        return true;
    };

    g_prof = getenv("Q3N_PROFILE") != nullptr;
    ensure_backend();
    struct rusage ru0; getrusage(RUSAGE_SELF, &ru0);
    auto t0 = std::chrono::steady_clock::now();
    int gen = 0, fed = 0;
    std::vector<int> fedids = ids;
    for (int step = 0; step < n_gen; ++step) {
        int tok = (step < (int) fedids.size()) ? fedids[step] : gen;
        if (!forward(tok, step)) { fprintf(stderr, "FWD_FAIL step=%d\n", step); return 1; }
        int best = 0;
        for (int64_t i = 1; i < m.n_vocab; ++i) if (logits[i] > logits[best]) best = (int) i;
        gen = best;
        fprintf(stderr, "Q3N step=%d fed=%d gen=%d max=%.4f\n", step, tok, gen, logits[best]);
        if (getenv("Q3N_DUMP_LOGITS")) {
            char lp[256]; snprintf(lp, sizeof lp, "%s.step%d", getenv("Q3N_DUMP_LOGITS"), step);
            FILE * lf = fopen(lp, "wb");
            if (lf) { fwrite(logits.data(), 4, logits.size(), lf); fclose(lf); }
        }
        fedids.push_back(gen);
    }
    auto t1 = std::chrono::steady_clock::now();
    struct rusage ru1; getrusage(RUSAGE_SELF, &ru1);
    double wall = std::chrono::duration<double>(t1 - t0).count();
    if (getenv("Q3N_PROFILE")) {
        double tot = g_prof_load + g_prof_compute + g_prof_rec + g_prof_attn + g_prof_moe + g_prof_head;
        fprintf(stderr, "Q3N_PROF rd=%.2fs dequant=%.2fs compute=%.2fs gdn=%.2fs attn=%.2fs moe=%.2fs head=%.2fs accounted=%.2f/%ddom\n",
            g_prof_rd, g_prof_dq, g_prof_compute, g_prof_rec, g_prof_attn, g_prof_moe, g_prof_head, tot, n_gen);
    }
    fprintf(stderr, "Q3N_PASS=1 gens=%d wall_s=%.2f tok_per_s=%.3f bytes_read=%lld peak_rss_kb=%ld\n",
        n_gen, wall, n_gen / wall, (long long) m.bytes_read, ru1.ru_maxrss / 1024);
    (void) fed;
    return 0;
}
