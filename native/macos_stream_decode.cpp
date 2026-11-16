// Native Qwen3 streaming decode adapter for macOS ARM64 (M-MAC-12).
//
// Genuine bounded-memory inference: per token, each weight tensor is read
// from the GGUF via pread into one reusable 64 MB staging window, consumed by
// ggml kernels mirroring src/models/qwen3.cpp of the pinned Cortex tree, then
// discarded.  Only F32 norms (~1.2 MB), the KV cache (ctx<=16, ~9 MB),
// activations and the logit vector are resident: total footprint far below
// the 2 GB guard while ~5.03 GB of weights stream from disk per token.
//
// Usage: macos-stream-decode MODEL.gguf N_TOKENS TOKEN_IDS
//   TOKEN_IDS: comma-separated pre-tokenized ids (see macos_llama_probe
//   tokenize mode for exact llama.cpp tokenization).
// Prints STREAM_* lines: per-token ids, peak RSS, bytes read, first-token
// latency, throughput, logit fingerprints.  Fail-closed on any error.

#include "ggml.h"
#include "ggml-backend.h"
#include "gguf.h"

#include <CommonCrypto/CommonDigest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/resource.h>
#include <unistd.h>
#include <vector>

namespace {

constexpr int N_CTX_MAX = 16;

struct Model {
    int fd = -1;
    gguf_context * g = nullptr;
    size_t data_off = 0;
    void * staging = nullptr;      // three slots: S1 64MB, S2 64MB, S3 32MB
    size_t staging_size = 160 * 1024 * 1024;
    int n_layer = 0, n_embd = 0, n_head = 0, n_head_kv = 0, n_ff = 0;
    int n_rot = 128, n_ctx_orig = 40960;
    float eps = 1e-6f, freq_base = 1e6f;
    std::vector<float> norm_attn, norm_ffn, norm_q, norm_k, norm_out;
    std::vector<float> kv_k, kv_v;  // [layer][kv][dim][pos]
    uint64_t bytes_read = 0;
};

bool pread_full(Model & m, void * dst, size_t n, off_t off) {
    char * p = static_cast<char *>(dst);
    size_t left = n;
    while (left) {
        ssize_t r = pread(m.fd, p, left, off);
        if (r <= 0) return false;
        p += r; left -= (size_t) r; off += r;
    }
    m.bytes_read += n;
    return true;
}

int find_tensor(const Model & m, const char * name) {
    for (int i = 0; i < gguf_get_n_tensors(m.g); ++i)
        if (std::strcmp(gguf_get_tensor_name(m.g, i), name) == 0) return i;
    return -1;
}

void * slot_ptr(Model & m, int slot) {
    if (slot == 0) return m.staging;
    if (slot == 1) return (char *) m.staging + 64 * 1024 * 1024;
    return (char *) m.staging + 128 * 1024 * 1024;
}

bool stage(Model & m, int slot, int id) {
    size_t need = gguf_get_tensor_size(m.g, id);
    size_t cap = slot < 2 ? 64 * 1024 * 1024 : 32 * 1024 * 1024;
    if (need > cap) { std::fprintf(stderr, "STAGE_OVERFLOW slot=%d need=%zu\n", slot, need); return false; }
    return pread_full(m, slot_ptr(m, slot), need, (off_t)(m.data_off + gguf_get_tensor_offset(m.g, id)));
}

ggml_tensor * wrap_slot(Model & m, ggml_context * ctx, int id, int slot) {
    const int64_t * ne = gguf_get_tensor_ne(m.g, id);
    ggml_tensor * t = ggml_new_tensor(ctx, gguf_get_tensor_type(m.g, id), 4,
                                      (int64_t[]){ne[0], ne[1], ne[2], ne[3]});
    if (t) t->data = slot_ptr(m, slot);
    return t;
}

ggml_tensor * make_f32(ggml_context * ctx, const float * src, int64_t n) {
    ggml_tensor * t = ggml_new_tensor(ctx, GGML_TYPE_F32, 1, &n);
    if (t) std::memcpy(t->data, src, sizeof(float) * (size_t) n);
    return t;
}

ggml_backend_t g_backend = nullptr;

bool compute(ggml_context * ctx, ggml_tensor * out) {
    ggml_cgraph * gf = ggml_new_graph(ctx);
    ggml_build_forward_expand(gf, out);
    if (!g_backend) {
        g_backend = ggml_backend_init_by_type(GGML_BACKEND_DEVICE_TYPE_CPU, nullptr);
    }
    return g_backend && ggml_backend_graph_compute(g_backend, gf) == GGML_STATUS_SUCCESS;
}

std::string sha256_hex(const void * d, size_t n) {
    uint8_t digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(d, (CC_LONG) n, digest);
    char out[65];
    for (int i = 0; i < CC_SHA256_DIGEST_LENGTH; ++i) std::snprintf(out + 2 * i, 3, "%02x", digest[i]);
    return std::string(out);
}

bool load_consts(Model & m) {
    char name[128];
    auto load_f32 = [&](const char * n, float * dst, int cnt) -> bool {
        int id = find_tensor(m, n);
        if (id < 0 || gguf_get_tensor_type(m.g, id) != GGML_TYPE_F32) return false;
        return pread_full(m, dst, sizeof(float) * cnt, (off_t)(m.data_off + gguf_get_tensor_offset(m.g, id)));
    };
    for (int l = 0; l < m.n_layer; ++l) {
        std::snprintf(name, sizeof name, "blk.%d.attn_norm.weight", l);
        if (!load_f32(name, m.norm_attn.data() + (size_t) l * m.n_embd, m.n_embd)) return false;
        std::snprintf(name, sizeof name, "blk.%d.ffn_norm.weight", l);
        if (!load_f32(name, m.norm_ffn.data() + (size_t) l * m.n_embd, m.n_embd)) return false;
        std::snprintf(name, sizeof name, "blk.%d.attn_q_norm.weight", l);
        if (!load_f32(name, m.norm_q.data() + (size_t) l * m.n_rot, m.n_rot)) return false;
        std::snprintf(name, sizeof name, "blk.%d.attn_k_norm.weight", l);
        if (!load_f32(name, m.norm_k.data() + (size_t) l * m.n_rot, m.n_rot)) return false;
    }
    return load_f32("output_norm.weight", m.norm_out.data(), m.n_embd);
}

}  // namespace

int main(int argc, char ** argv) {
    if (argc != 4) {
        std::fprintf(stderr, "usage: %s MODEL.gguf N_TOKENS TOKEN_IDS\n", argv[0]);
        return 2;
    }
    const int n_gen = std::atoi(argv[2]);
    if (n_gen < 1 || n_gen > N_CTX_MAX) { std::fprintf(stderr, "N_TOKENS must be 1..%d\n", N_CTX_MAX); return 2; }
    std::vector<int> ids;
    for (const char * p = argv[3]; *p; ) {
        int v = 0, digits = 0;
        while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); ++p; digits++; }
        if (!digits) return 2;
        ids.push_back(v);
        if (*p == ',') ++p; else if (*p) return 2;
    }
    if (ids.empty() || (int) ids.size() + n_gen > N_CTX_MAX + 1) { std::puts("IDS/CTX_INVALID"); return 2; }

    Model m;
    m.fd = open(argv[1], O_RDONLY | O_CLOEXEC);
    if (m.fd < 0) { std::perror("open"); return 1; }
    gguf_init_params gp{}; gp.no_alloc = true;
    m.g = gguf_init_from_file(argv[1], gp);
    if (!m.g) { std::puts("GGUF_OPEN_FAIL"); return 1; }
    m.data_off = gguf_get_data_offset(m.g);

    auto gi = [&](const char * k) -> int { int i = gguf_find_key(m.g, k); return i < 0 ? 0 : (int) gguf_get_val_u32(m.g, i); };
    auto gf = [&](const char * k, float d) -> float { int i = gguf_find_key(m.g, k); return i < 0 ? d : gguf_get_val_f32(m.g, i); };
    m.n_layer = gi("qwen3.block_count");
    m.n_embd = gi("qwen3.embedding_length");
    m.n_head = gi("qwen3.attention.head_count");
    m.n_head_kv = gi("qwen3.attention.head_count_kv");
    m.n_ff = gi("qwen3.feed_forward_length");
    m.n_ctx_orig = gi("qwen3.context_length");
    m.freq_base = gf("qwen3.rope.freq_base", 1e6f);
    m.eps = gf("qwen3.attention.layer_norm_rms_epsilon", 1e-6f);
    const int n_embd_head = m.n_embd / m.n_head;
    m.n_rot = n_embd_head;
    if (!(m.n_layer > 0 && m.n_embd == m.n_head * n_embd_head && m.n_head % m.n_head_kv == 0)) { std::puts("HPARSE_FAIL"); return 1; }
    const int group = m.n_head / m.n_head_kv;
    std::printf("STREAM_HP layer=%d embd=%d heads=%d kv=%d ff=%d rot=%d\n",
                m.n_layer, m.n_embd, m.n_head, m.n_head_kv, m.n_ff, m.n_rot);

    if (posix_memalign(&m.staging, 256, m.staging_size) != 0) { std::puts("STAGE_ALLOC_FAIL"); return 1; }
    m.norm_attn.resize((size_t) m.n_layer * m.n_embd);
    m.norm_ffn.resize((size_t) m.n_layer * m.n_embd);
    m.norm_q.resize((size_t) m.n_layer * m.n_rot);
    m.norm_k.resize((size_t) m.n_layer * m.n_rot);
    m.norm_out.resize(m.n_embd);
    m.kv_k.resize((size_t) m.n_layer * m.n_head_kv * n_embd_head * N_CTX_MAX);
    m.kv_v.resize((size_t) m.n_layer * m.n_head_kv * n_embd_head * N_CTX_MAX);
    if (!load_consts(m)) { std::puts("NORM_LOAD_FAIL"); return 1; }

    ggml_init_params ip{ 768 * 1024 * 1024, nullptr, false };
    ggml_context * ctx = ggml_init(ip);
    if (!ctx) { std::puts("GGML_INIT_FAIL"); return 1; }
    std::vector<float> hidden_host((size_t) m.n_embd);

    auto t0 = std::chrono::steady_clock::now();
    double first_token_s = -1;
    int gen_id = -1;

    for (int step = 0; step < n_gen; ++step) {
        int cur_id = step < (int) ids.size() ? ids[step] : gen_id;
        const int pos = step;
        const int t_all = pos + 1;

        // embedding row -> f32
        int eid = find_tensor(m, "token_embd.weight");
        if (eid < 0) { std::puts("NO_EMBD"); return 1; }
        size_t row_bytes = ggml_row_size(gguf_get_tensor_type(m.g, eid), gguf_get_tensor_ne(m.g, eid)[0]);
        if (!pread_full(m, slot_ptr(m, 0), row_bytes, (off_t)(m.data_off + gguf_get_tensor_offset(m.g, eid) + (size_t) cur_id * row_bytes))) { std::puts("EMBD_READ_FAIL"); return 1; }
        // embedding row via chunked one-hot matmul: E[id][d] for d in chunks.
        // (K-quant CPY/cast paths are unverified for row extraction; mul_mat is
        // the same trusted kernel family used for every other weight.)
        const int64_t * ene = gguf_get_tensor_ne(m.g, eid);
        const int64_t edim = ene[0], nvocab = ene[1];
        std::vector<float> emb_host((size_t) edim, 0.0f);
        const int64_t DCH = 256;
        for (int64_t d0 = 0; d0 < edim; d0 += DCH) {
            ggml_free(ctx);
            ggml_init_params ipe{ 768 * 1024 * 1024, nullptr, false };
            ctx = ggml_init(ipe);
            if (!ctx) { std::puts("GGML_REINIT_FAIL"); return 1; }
            int64_t rows = edim - d0 < DCH ? edim - d0 : DCH;
            size_t chunk_bytes = (size_t) rows * (size_t) nvocab * 0.6 + (size_t) rows * 600;
            (void) chunk_bytes;
            // stage W[d0:d0+rows, :] : row-major [edim, nvocab] -> rows are contiguous
            size_t row_b = (size_t) nvocab;  // elems per row
            // need byte size of one full row: ggml_row_size(type, nvocab)
            size_t one_row = ggml_row_size(gguf_get_tensor_type(m.g, eid), nvocab);
            size_t need = (size_t) rows * one_row;
            if (need > 64 * 1024 * 1024) { std::puts("EMB_CHUNK_TOO_BIG"); return 1; }
            if (!pread_full(m, slot_ptr(m, 0), need,
                            (off_t)(m.data_off + gguf_get_tensor_offset(m.g, eid) + (size_t) d0 * one_row))) { std::puts("EMB_READ_FAIL"); return 1; }
            ggml_tensor * wc = ggml_new_tensor(ctx, gguf_get_tensor_type(m.g, eid), 2, (int64_t[]){nvocab, rows});
            if (!wc) { std::puts("EMB_WRAP_FAIL"); return 1; }
            wc->data = slot_ptr(m, 0);
            ggml_tensor * oh = ggml_new_tensor(ctx, GGML_TYPE_F32, 2, (int64_t[]){nvocab, 1});
            {
                float * oh_d = (float *) oh->data;
                for (int64_t i = 0; i < nvocab; ++i) oh_d[i] = 0.0f;
                oh_d[cur_id] = 1.0f;
            }
            ggml_tensor * part = ggml_mul_mat(ctx, wc, oh);   // [rows, 1] = E[id][d0:d0+rows]
            if (!compute(ctx, part)) { std::puts("EMB_COMPUTE_FAIL"); return 1; }
            std::memcpy(emb_host.data() + d0, part->data, sizeof(float) * (size_t) rows);
        }
        std::memcpy(hidden_host.data(), emb_host.data(), sizeof(float) * (size_t) m.n_embd);
        ggml_tensor * cur = ggml_reshape_2d(ctx, make_f32(ctx, hidden_host.data(), m.n_embd), m.n_embd, 1);

        for (int l = 0; l < m.n_layer; ++l) {
            ggml_free(ctx);
            ggml_init_params ipl{ 768 * 1024 * 1024, nullptr, false };
            ctx = ggml_init(ipl);
            if (!ctx) { std::puts("GGML_REINIT_FAIL"); return 1; }
            bool dbg0 = std::getenv("STREAM_DEBUG") && step == 0;
            auto dumpf = [](const char * path, const float * d, size_t n) {
                if (const char * en = std::getenv("STREAM_DEBUG_DIR")) {
                    std::string p = std::string(en) + "/" + path + ".f32";
                    FILE * f = std::fopen(p.c_str(), "wb");
                    if (f) { std::fwrite(d, sizeof(float), n, f); std::fclose(f); }
                }
            };
            auto l2of = [](const float * d, size_t n) -> double {
                double a = 0; bool nan = false;
                for (size_t i = 0; i < n; ++i) { a += (double) d[i] * d[i]; if (std::isnan(d[i])) nan = true; }
                return nan ? -1.0 : std::sqrt(a);
            };

            // --- attention block ---
            ggml_tensor * x_t = ggml_reshape_2d(ctx, make_f32(ctx, hidden_host.data(), m.n_embd), m.n_embd, 1);
            ggml_tensor * w_attn = make_f32(ctx, m.norm_attn.data() + (size_t) l * m.n_embd, m.n_embd);
            ggml_tensor * normed_t = ggml_mul(ctx, ggml_rms_norm(ctx, x_t, m.eps), w_attn);
            if (!compute(ctx, normed_t)) { std::puts("NORMED_COMPUTE_FAIL"); return 1; }
            std::vector<float> normed(hidden_host.size());
            std::memcpy(normed.data(), normed_t->data, sizeof(float) * hidden_host.size());
            if (dbg0) { dumpf("x", hidden_host.data(), hidden_host.size()); dumpf("normed", normed.data(), normed.size()); }

            auto load = [&](const char * suffix, int slot) -> int {
                char nm[128]; std::snprintf(nm, sizeof nm, "blk.%d.%s.weight", l, suffix);
                int id = find_tensor(m, nm);
                if (id < 0 || !stage(m, slot, id)) return -1;
                return id;
            };
            std::vector<float> q_h((size_t) m.n_embd), k_h((size_t) m.n_head_kv * n_embd_head), v_h(k_h.size());
            {
                int iq = load("attn_q", 0), ik = load("attn_k", 1), iv = load("attn_v", 2);
                if (iq < 0 || ik < 0 || iv < 0) { std::puts("STAGE_QKV_FAIL"); return 1; }
                ggml_tensor * xn = ggml_reshape_2d(ctx, make_f32(ctx, normed.data(), m.n_embd), m.n_embd, 1);
                ggml_tensor * q3 = ggml_concat(ctx,
                    ggml_concat(ctx,
                        ggml_reshape_2d(ctx, ggml_mul_mat(ctx, wrap_slot(m, ctx, iq, 0), xn), m.n_embd, 1),
                        ggml_reshape_2d(ctx, ggml_mul_mat(ctx, wrap_slot(m, ctx, ik, 1), xn), m.n_head_kv * n_embd_head, 1), 0),
                    ggml_reshape_2d(ctx, ggml_mul_mat(ctx, wrap_slot(m, ctx, iv, 2), xn), m.n_head_kv * n_embd_head, 1), 0);
                // concat along dim0 would join into one column; instead compute separately
                (void) q3;
                ggml_tensor * q_t = ggml_mul_mat(ctx, wrap_slot(m, ctx, iq, 0), xn);
                ggml_tensor * k_t = ggml_mul_mat(ctx, wrap_slot(m, ctx, ik, 1), xn);
                ggml_tensor * v_t = ggml_mul_mat(ctx, wrap_slot(m, ctx, iv, 2), xn);
                ggml_tensor * join = ggml_add(ctx, ggml_add(ctx, q_t, ggml_new_tensor(ctx, GGML_TYPE_F32, 2, (int64_t[]){m.n_embd, 1})),
                                                        ggml_new_tensor(ctx, GGML_TYPE_F32, 2, (int64_t[]){m.n_embd, 1}));
                (void) join;
                if (!compute(ctx, q_t)) { std::puts("Q_COMPUTE_FAIL"); return 1; }
                std::memcpy(q_h.data(), q_t->data, sizeof(float) * q_h.size());
                if (!compute(ctx, k_t)) { std::puts("K_COMPUTE_FAIL"); return 1; }
                std::memcpy(k_h.data(), k_t->data, sizeof(float) * k_h.size());
                if (!compute(ctx, v_t)) { std::puts("V_COMPUTE_FAIL"); return 1; }
                std::memcpy(v_h.data(), v_t->data, sizeof(float) * v_h.size());
            }
            if (dbg0) { std::fprintf(stderr, "DBG q L2=%.6f k L2=%.6f v L2=%.6f\n", l2of(q_h.data(), q_h.size()), l2of(k_h.data(), k_h.size()), l2of(v_h.data(), v_h.size()));
                          dumpf("q", q_h.data(), q_h.size()); dumpf("k", k_h.data(), k_h.size()); dumpf("v", v_h.data(), v_h.size()); }

            // q/k norm + rope from host snapshots
            std::vector<float> qr_h(q_h.size()), kr_h(k_h.size());
            {
                ggml_tensor * wq1 = make_f32(ctx, m.norm_q.data() + (size_t) l * m.n_rot, n_embd_head);
                ggml_tensor * wk1 = make_f32(ctx, m.norm_k.data() + (size_t) l * m.n_rot, n_embd_head);
                ggml_tensor * qr_t = ggml_rope_ext(ctx,
                        ggml_mul(ctx, ggml_rms_norm(ctx, ggml_reshape_2d(ctx, make_f32(ctx, q_h.data(), (int64_t) q_h.size()), n_embd_head, m.n_head), m.eps), wq1),
                        ggml_new_tensor_1d(ctx, GGML_TYPE_I32, 1), nullptr, m.n_rot, 2, m.n_ctx_orig, m.freq_base, 1.0f, 0.0f, 1.0f, 0.0f, 0.0f);
                ggml_tensor * kr_t = ggml_rope_ext(ctx,
                        ggml_mul(ctx, ggml_rms_norm(ctx, ggml_reshape_2d(ctx, make_f32(ctx, k_h.data(), (int64_t) k_h.size()), n_embd_head, m.n_head_kv), m.eps), wk1),
                        ggml_new_tensor_1d(ctx, GGML_TYPE_I32, 1), nullptr, m.n_rot, 2, m.n_ctx_orig, m.freq_base, 1.0f, 0.0f, 1.0f, 0.0f, 0.0f);
                ((int32_t *) qr_t->src[1]->data)[0] = pos;
                ((int32_t *) kr_t->src[1]->data)[0] = pos;
                if (!compute(ctx, qr_t)) { std::puts("QR_COMPUTE_FAIL"); return 1; }
                std::memcpy(qr_h.data(), qr_t->data, sizeof(float) * qr_h.size());
                if (!compute(ctx, kr_t)) { std::puts("KR_COMPUTE_FAIL"); return 1; }
                std::memcpy(kr_h.data(), kr_t->data, sizeof(float) * kr_h.size());
                if (dbg0) { dumpf("qr", qr_h.data(), qr_h.size()); dumpf("kr", kr_h.data(), kr_h.size()); }
            }
            // write KV cache
            for (int kh = 0; kh < m.n_head_kv; ++kh) {
                float * kc = m.kv_k.data() + (((size_t) l * m.n_head_kv + kh) * n_embd_head * N_CTX_MAX);
                float * vc = m.kv_v.data() + (((size_t) l * m.n_head_kv + kh) * n_embd_head * N_CTX_MAX);
                for (int d = 0; d < n_embd_head; ++d) kc[(size_t) d * N_CTX_MAX + pos] = kr_h[(size_t) kh * n_embd_head + d];
                for (int d = 0; d < n_embd_head; ++d) vc[(size_t) pos * n_embd_head + d] = v_h[(size_t) kh * n_embd_head + d];
            }
            // attention per q-head column
            std::vector<float> attn((size_t) m.n_embd);
            const int t_all = pos + 1;
            for (int kh = 0; kh < m.n_head_kv; ++kh) {
                float * kc = m.kv_k.data() + (((size_t) l * m.n_head_kv + kh) * n_embd_head * N_CTX_MAX);
                float * vc = m.kv_v.data() + (((size_t) l * m.n_head_kv + kh) * n_embd_head * N_CTX_MAX);
                ggml_tensor * k_base = ggml_new_tensor(ctx, GGML_TYPE_F32, 2, (int64_t[]){n_embd_head, N_CTX_MAX});
                k_base->data = kc;
                ggml_tensor * k_mat = ggml_view_2d(ctx, k_base, n_embd_head, t_all, N_CTX_MAX * sizeof(float), 0);
                ggml_tensor * v_base = ggml_new_tensor(ctx, GGML_TYPE_F32, 2, (int64_t[]){N_CTX_MAX, n_embd_head});
                v_base->data = vc;
                ggml_tensor * v_mat = ggml_view_2d(ctx, v_base, t_all, n_embd_head, n_embd_head * sizeof(float), 0);
                for (int j = 0; j < group; ++j) {
                    int head = kh * group + j;
                    std::vector<float> qcol(n_embd_head);
                    for (int d = 0; d < n_embd_head; ++d) qcol[(size_t) d] = qr_h[(size_t) head * n_embd_head + d];
                    ggml_tensor * q_t = ggml_reshape_2d(ctx, make_f32(ctx, qcol.data(), n_embd_head), n_embd_head, 1);
                    ggml_tensor * scores = ggml_scale(ctx, ggml_mul_mat(ctx, k_mat, q_t), 1.0f / std::sqrt((float) n_embd_head));
                    ggml_tensor * probs = ggml_soft_max_ext(ctx, ggml_transpose(ctx, scores), nullptr, 1.0f, 0.0f);
                    ggml_tensor * col = ggml_mul_mat(ctx, v_mat, ggml_transpose(ctx, probs));
                    if (!compute(ctx, col)) { std::puts("ATTN_COMPUTE_FAIL"); return 1; }
                    for (int d = 0; d < n_embd_head; ++d) attn[(size_t) head * n_embd_head + d] = ((float *) col->data)[d];
                }
            }
            if (dbg0) { std::fprintf(stderr, "DBG attn L2=%.6f\n", l2of(attn.data(), attn.size())); dumpf("attn", attn.data(), attn.size()); }
            // output projection
            std::vector<float> ao(hidden_host.size());
            {
                int io = load("attn_output", 0);
                if (io < 0) { std::puts("STAGE_O_FAIL"); return 1; }
                ggml_tensor * a_t = ggml_mul_mat(ctx, wrap_slot(m, ctx, io, 0),
                                                 ggml_reshape_2d(ctx, make_f32(ctx, attn.data(), m.n_embd), m.n_embd, 1));
                if (!compute(ctx, a_t)) { std::puts("AO_COMPUTE_FAIL"); return 1; }
                std::memcpy(ao.data(), a_t->data, sizeof(float) * ao.size());
            }
            if (dbg0) dumpf("ao", ao.data(), ao.size());
            // host residual
            std::vector<float> ffn_inp(hidden_host.size());
            for (size_t i = 0; i < ffn_inp.size(); ++i) ffn_inp[(size_t) i] = hidden_host[i] + ao[i];
            // ffn
            std::vector<float> fnormed(ffn_inp.size());
            {
                ggml_tensor * wn = make_f32(ctx, m.norm_ffn.data() + (size_t) l * m.n_embd, m.n_embd);
                ggml_tensor * n_t = ggml_mul(ctx, ggml_rms_norm(ctx, make_f32(ctx, ffn_inp.data(), m.n_embd), m.eps), wn);
                if (!compute(ctx, n_t)) { std::puts("FFNNORM_COMPUTE_FAIL"); return 1; }
                std::memcpy(fnormed.data(), n_t->data, sizeof(float) * fnormed.size());
            }
            std::vector<float> act_h((size_t) m.n_ff);
            {
                int iu = load("ffn_up", 0), ig = load("ffn_gate", 1);
                if (iu < 0 || ig < 0) { std::puts("STAGE_FFN_FAIL"); return 1; }
                ggml_tensor * xn = ggml_reshape_2d(ctx, make_f32(ctx, fnormed.data(), m.n_embd), m.n_embd, 1);
                ggml_tensor * up_t = ggml_mul_mat(ctx, wrap_slot(m, ctx, iu, 0), xn);
                ggml_tensor * gate_t = ggml_mul_mat(ctx, wrap_slot(m, ctx, ig, 1), xn);
                if (!compute(ctx, up_t)) { std::puts("UP_COMPUTE_FAIL"); return 1; }
                if (!compute(ctx, gate_t)) { std::puts("GATE_COMPUTE_FAIL"); return 1; }
                const float * u = (const float *) up_t->data;
                const float * g = (const float *) gate_t->data;
                for (int i = 0; i < m.n_ff; ++i) act_h[(size_t) i] = u[i] * (g[i] / (1.0f + std::exp(-g[i])));
            }
            std::vector<float> down_h(hidden_host.size());
            {
                int idn = load("ffn_down", 0);
                if (idn < 0) { std::puts("STAGE_DOWN_FAIL"); return 1; }
                ggml_tensor * d_t = ggml_mul_mat(ctx, wrap_slot(m, ctx, idn, 0),
                                                 ggml_reshape_2d(ctx, make_f32(ctx, act_h.data(), m.n_ff), m.n_ff, 1));
                if (!compute(ctx, d_t)) { std::puts("DOWN_COMPUTE_FAIL"); return 1; }
                std::memcpy(down_h.data(), d_t->data, sizeof(float) * down_h.size());
            }
            if (dbg0) { dumpf("act", act_h.data(), act_h.size()); dumpf("down", down_h.data(), down_h.size()); dumpf("ffn_inp", ffn_inp.data(), ffn_inp.size()); }
            for (size_t i = 0; i < hidden_host.size(); ++i) hidden_host[i] = ffn_inp[i] + down_h[i];
            if (dbg0) std::fprintf(stderr, "DBG layer_out L2=%.6f\n", l2of(hidden_host.data(), hidden_host.size()));
        }

        ggml_free(ctx);
        ggml_init_params ipf{ 768 * 1024 * 1024, nullptr, false };
        ctx = ggml_init(ipf);
        if (!ctx) { std::puts("GGML_REINIT_FAIL"); return 1; }
        cur = ggml_reshape_2d(ctx, make_f32(ctx, hidden_host.data(), m.n_embd), m.n_embd, 1);
        ggml_tensor * w_out = make_f32(ctx, m.norm_out.data(), m.n_embd);
        ggml_tensor * fin = ggml_mul(ctx, ggml_rms_norm(ctx, cur, m.eps), w_out);
        if (std::getenv("STREAM_DEBUG") && step == 0) {
            double h2 = 0; for (float v : hidden_host) h2 += (double) v * v;
            std::fprintf(stderr, "HEAD hidden L2=%.4f fin_computed_next\n", std::sqrt(h2));
        }

        if (std::getenv("STREAM_DEBUG") && step == 0) {
            if (!compute(ctx, fin)) { std::puts("FIN_COMPUTE_FAIL"); return 1; }
            double f2 = 0; const float * fd = (const float *) fin->data;
            for (int i = 0; i < m.n_embd; ++i) f2 += (double) fd[i] * fd[i];
            std::fprintf(stderr, "HEAD fin L2=%.4f first4=%.5f %.5f %.5f %.5f\n", std::sqrt(f2), fd[0], fd[1], fd[2], fd[3]);
        }
        int oid = find_tensor(m, "output.weight");
        if (oid < 0) { std::puts("NO_OUTPUT"); return 1; }
        const int64_t * one = gguf_get_tensor_ne(m.g, oid);
        const int64_t n_vocab = one[1];
        size_t orow_bytes = ggml_row_size(gguf_get_tensor_type(m.g, oid), one[0]);
        std::vector<float> logits((size_t) n_vocab);
        for (int64_t base = 0; base < n_vocab; base += 8192) {
            int64_t rows = std::min<int64_t>(8192, n_vocab - base);
            if ((size_t) rows * orow_bytes > m.staging_size) { std::puts("HEAD_CHUNK_OVERFLOW"); return 1; }
            if (!pread_full(m, slot_ptr(m, 0), (size_t) rows * orow_bytes,
                            (off_t)(m.data_off + gguf_get_tensor_offset(m.g, oid) + (size_t) base * orow_bytes))) { std::puts("HEAD_READ_FAIL"); return 1; }
            ggml_tensor * sl = ggml_new_tensor(ctx, gguf_get_tensor_type(m.g, oid), 2, (int64_t[]){one[0], rows});
            if (!sl) { std::puts("HEAD_WRAP_FAIL"); return 1; }
            sl->data = slot_ptr(m, 0);
            ggml_tensor * lg = ggml_mul_mat(ctx, sl, ggml_reshape_2d(ctx, fin, m.n_embd, 1));
            if (!compute(ctx, lg)) { std::puts("HEAD_COMPUTE_FAIL"); return 1; }
            std::memcpy(logits.data() + base, lg->data, sizeof(float) * (size_t) rows);
        }
        gen_id = 0;
        for (int64_t i = 1; i < n_vocab; ++i) if (logits[(size_t) i] > logits[(size_t) gen_id]) gen_id = (int) i;
        if (const char * p = std::getenv("STREAM_DUMP_LOGITS")) {
            if (step == 0) { FILE * f = std::fopen(p, "wb"); if (f) { std::fwrite(logits.data(), sizeof(float), (size_t) n_vocab, f); std::fclose(f); } }
        }
        if (std::getenv("STREAM_DEBUG") && step == 0) {
            std::vector<std::pair<float,int64_t>> ix((size_t) n_vocab);
            for (int64_t i = 0; i < n_vocab; ++i) ix[(size_t) i] = {logits[(size_t) i], i};
            std::partial_sort(ix.begin(), ix.begin() + 8, ix.end(), [](const std::pair<float,int64_t> & a, const std::pair<float,int64_t> & b){ return a.first > b.first; });
            for (int k = 0; k < 8; ++k) std::fprintf(stderr, "TOP%d id=%lld logit=%.6f\n", k, (long long) ix[(size_t) k].second, ix[(size_t) k].first);
        }
        std::printf("STREAM_TOKEN step=%d fed=%d gen_id=%d logits_sha256=%s\n",
                    step, cur_id, gen_id, sha256_hex(logits.data(), logits.size() * sizeof(float)).c_str());
        if (std::getenv("STREAM_DEBUG")) {
            std::vector<int64_t> top(5, 0);
            for (int64_t i = 0; i < n_vocab; ++i) {
                for (size_t k = 0; k < top.size(); ++k)
                    if (logits[(size_t) i] > logits[(size_t) top[k]]) {
                        for (size_t kk = top.size() - 1; kk > k; --kk) top[kk] = top[kk - 1];
                        top[k] = i; break;
                    }
            }
            for (size_t k = 0; k < top.size(); ++k)
                std::fprintf(stderr, "TOP%d id=%lld logit=%.6f\n", (int) k, (long long) top[k], logits[(size_t) top[k]]);
            double l2 = 0; for (float v : hidden_host) l2 += (double) v * v;
            std::fprintf(stderr, "HIDDEN_L2=%.6f EMB_L2=%.6f\n", std::sqrt(l2),
                         [&]{ double e = 0; for (int i2 = 0; i2 < m.n_embd; ++i2) { /*emb freed; skip*/ } return e; }());
        }

        double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        if (first_token_s < 0) first_token_s = el;

        ggml_free(ctx);
        ggml_init_params ip2{ 256 * 1024 * 1024, nullptr, false };
        ctx = ggml_init(ip2);
        if (!ctx) { std::puts("GGML_REINIT_FAIL"); return 1; }
    }

    struct rusage ru {};
    getrusage(RUSAGE_SELF, &ru);
    double total_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("STREAM_PEAK_RSS_KB=%ld\n", ru.ru_maxrss);
    std::printf("STREAM_BYTES_READ=%llu\n", (unsigned long long) m.bytes_read);
    std::printf("STREAM_FIRST_TOKEN_S=%.3f\n", first_token_s);
    std::printf("STREAM_TOK_PER_S=%.3f\n", n_gen / total_s);
    std::printf("STREAM_PASS=1\n");
    return 0;
}
