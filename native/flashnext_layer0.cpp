#include "llama.h"
#include "quortex_streamed_moe.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <fstream>
#include <string>
#include <unistd.h>
#include <sys/resource.h>
#include <vector>
#include <sstream>
#include <sys/stat.h>
#include <cerrno>

static bool write_capture(const char * dir, const char * name, const float * data, size_t n) {
    if (!dir || !data || n == 0) return false;
    if (mkdir(dir, 0755) != 0 && errno != EEXIST) return false;
    const std::string final = std::string(dir) + "/quortex_" + name + "_p0.bin";
    const std::string temp = final + ".tmp";
    std::ofstream f(temp, std::ios::binary | std::ios::trunc);
    if (!f.write(reinterpret_cast<const char *>(data), n * sizeof(float))) return false;
    f.close();
    return std::rename(temp.c_str(), final.c_str()) == 0;
}

static uint64_t fnv_bytes(const uint8_t * p, size_t n) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

static bool read_contract(const char * dir, std::vector<float> & moe, std::vector<float> & residual, std::vector<float> & inject) {
    if (!dir) return false;
    std::ifstream manifest(std::string(dir) + "/segment_b_contract.tsv");
    std::string header; if (!std::getline(manifest, header) || header != "QUORTEX_SEGMENT_B_CONTRACT_V1") return false;
    std::string input; if (!std::getline(manifest, input) || input != "input\ttoken=248045\tposition=0\tsequence=0\tendianness=little\tdtype=f32") return false;
    struct item { const char * name; size_t count; std::vector<float> * dst; uint64_t hash = 0; bool seen = false; } items[] = {
        {"complete_moe", 2560, &moe}, {"recurrent_residual", 10240, &residual}, {"injection", 4, &inject}
    };
    std::string line;
    while (std::getline(manifest, line)) {
        std::istringstream in(line); std::string name, hash_text; size_t count = 0;
        if (!std::getline(in, name, '\t') || !(in >> count) || !(in >> hash_text)) return false;
        for (auto & item : items) if (name == item.name) {
            if (item.seen || count != item.count || hash_text.size() != 16) return false;
            item.hash = std::stoull(hash_text, nullptr, 16); item.seen = true;
            const std::string path = std::string(dir) + "/cortex_" + name + "_p0.bin";
            std::ifstream f(path, std::ios::binary | std::ios::ate); if (!f || static_cast<size_t>(f.tellg()) != count * sizeof(float)) return false;
            f.seekg(0); item.dst->resize(count); f.read(reinterpret_cast<char *>(item.dst->data()), count * sizeof(float));
            if (!f || fnv_bytes(reinterpret_cast<const uint8_t *>(item.dst->data()), count * sizeof(float)) != item.hash) return false;
            for (float v : *item.dst) if (!std::isfinite(v)) return false;
        }
    }
    return std::all_of(std::begin(items), std::end(items), [](const item & x) { return x.seen; });
}

static size_t rss() {
    std::ifstream f("/proc/self/statm"); size_t total=0,resident=0;
    long page=sysconf(_SC_PAGESIZE); f >> total >> resident; return resident*page;
}

static bool install_memory_guard() {
    const char * raw = std::getenv("QUORTEX_MAX_AS_MB");
    const unsigned long long mb = raw ? std::strtoull(raw, nullptr, 10) : 2048;
    if (mb < 512 || mb > 32768) return false;
    const rlim_t limit = static_cast<rlim_t>(mb) * 1024 * 1024;
    struct rlimit current{};
    if (getrlimit(RLIMIT_AS, &current) != 0) return false;
    if (current.rlim_cur != RLIM_INFINITY && current.rlim_cur <= limit) return true;
    struct rlimit next{limit, current.rlim_max == RLIM_INFINITY ? limit : std::min<rlim_t>(current.rlim_max, limit)};
    return setrlimit(RLIMIT_AS, &next) == 0;
}

int main(int argc, char ** argv) {
    if (argc != 2) { std::fprintf(stderr, "usage: %s MODEL.gguf\n", argv[0]); return 2; }
    if (!install_memory_guard()) { std::puts("MEMORY_GUARD=FAIL_CLOSED"); return 10; }
    // These are the Cortex loader controls used by the verified low-residency
    // executable.  They must be set before model construction.
    setenv("CORTEX_PREAD_ARENA", "1", 1);
    setenv("CORTEX_LOW_RESIDENCY_LAYER", "0", 1);
    setenv("CORTEX_FLASHNEXT_LAYER_START", "0", 1);
    setenv("CORTEX_FLASHNEXT_LAYER_END", "1", 1);
    setenv("CORTEX_FLASHNEXT_LAYER_RANGE", "1", 1);
    setenv("CORTEX_FLASHNEXT_PRE_MOE", "1", 1);
    setenv("CORTEX_EXTERNAL_GLOBAL_TENSORS", "1", 1);
    if (std::getenv("FLASHNEXT_COMPARE_DIR")) {
        setenv("CORTEX_FLASHNEXT_INTERNAL_TENSOR_DIAGNOSTICS", "1", 1);
    }
    if (!std::getenv("CORTEX_PREAD_ARENA") || !std::getenv("CORTEX_LOW_RESIDENCY_LAYER") ||
        !std::getenv("CORTEX_FLASHNEXT_LAYER_RANGE")) {
        std::puts("STREAMING_FLAGS=FAIL_CLOSED");
        return 11;
    }
    llama_backend_init();
    llama_model_params mp = llama_model_default_params();
    llama_model * model = llama_model_load_from_file(argv[1], mp);
    if (!model) { std::puts("LAYER0_LOAD=FAIL"); llama_backend_free(); return 1; }
    llama_context_params cp = llama_context_default_params();
    cp.n_ctx = 4; cp.n_batch = 1; cp.embeddings = true;
    const char * segment_b_dir = std::getenv("FLASHNEXT_SEGMENT_B_INPUT_DIR");
    const bool full_layer0 = std::getenv("QUORTEX_FULL_LAYER0_STREAMED") != nullptr;
    if (full_layer0 && segment_b_dir) {
        std::puts("MODE_CONFLICT=FULL_LAYER0_STREAMED_AND_EXTERNAL_SEGMENT_B");
        llama_model_free(model); llama_backend_free(); return 19;
    }
    if (full_layer0 && !std::getenv("FLASHNEXT_COMPARE_DIR")) {
        std::puts("FULL_LAYER0_STREAMED=FAIL capture_directory_required");
        llama_model_free(model); llama_backend_free(); return 20;
    }
    if (segment_b_dir) {
        setenv("CORTEX_FLASHNEXT_SEGMENT_B", "1", 1);
        setenv("CORTEX_FLASHNEXT_SEGMENT_B_INPUT_WIDTH", "12804", 1);
    }
    llama_context * ctx = llama_init_from_model(model, cp);
    if (!ctx) { llama_model_free(model); llama_backend_free(); return 1; }
    if (segment_b_dir) {
        std::vector<float> moe, residual, inject;
        if (!read_contract(segment_b_dir, moe, residual, inject)) {
            std::puts("SEGMENT_B_INPUT_CONTRACT=FAIL");
            llama_free(ctx); llama_model_free(model); llama_backend_free(); return 17;
        }
        llama_batch batch = llama_batch_init(1, 12804, 1);
        if (!batch.embd || batch.token != nullptr) { llama_batch_free(batch); llama_free(ctx); llama_model_free(model); llama_backend_free(); return 17; }
        std::copy(moe.begin(), moe.end(), batch.embd);
        std::copy(residual.begin(), residual.end(), batch.embd + moe.size());
        std::copy(inject.begin(), inject.end(), batch.embd + moe.size() + residual.size());
        batch.pos[0] = 0; batch.n_seq_id[0] = 1; batch.seq_id[0][0] = 0; batch.logits[0] = 1; batch.n_tokens = 1;
        const int rc = llama_decode(ctx, batch); llama_synchronize(ctx);
        const float * out = llama_get_embeddings_ith(ctx, 0);
        const size_t count = 2560 * 4;
        bool valid = rc == 0 && out != nullptr;
        if (valid) for (size_t i = 0; i < count; ++i) valid = valid && std::isfinite(out[i]);
        if (valid && std::getenv("FLASHNEXT_COMPARE_DIR")) write_capture(std::getenv("FLASHNEXT_COMPARE_DIR"), "post_segment_b", out, count);
        std::printf("SEGMENT_B_ISOLATED rc=%d output=%s count=%zu\n", rc, valid ? "VALID" : "INVALID", valid ? count : 0);
        llama_batch_free(batch); llama_free(ctx); llama_model_free(model); llama_backend_free(); return valid ? 0 : 18;
    }
    const int32_t token_id = 248045;
    llama_batch batch = llama_batch_init(1, llama_model_n_embd_inp(model), 1);
    // llama_batch_init allocates either token storage or embedding storage;
    // embedding-backed batches deliberately have no batch.token pointer.
    if (batch.token != nullptr || batch.embd == nullptr) {
        std::puts("BATCH_STORAGE_INVARIANT=FAIL");
        llama_batch_free(batch); llama_free(ctx); llama_model_free(model); llama_backend_free(); return 13;
    }
    batch.pos[0] = 0; batch.n_seq_id[0] = 1;
    batch.seq_id[0][0] = 0; batch.logits[0] = 1; batch.n_tokens = 1;
    std::vector<float> streamed_embedding((size_t) llama_model_n_embd_inp(model));
    if (!llama_cortex_token_embd_rows(model, &token_id, 1, streamed_embedding.data())) {
        std::puts("TOKEN_EMBEDDING_STREAM=FAIL");
        llama_batch_free(batch); llama_free(ctx); llama_model_free(model); llama_backend_free(); return 12;
    }
    std::copy(streamed_embedding.begin(), streamed_embedding.end(), batch.embd);
    const size_t before = rss();
    const int rc = llama_decode(ctx, batch);
    llama_synchronize(ctx);
    const float * out = llama_get_embeddings_ith(ctx, 0);
    size_t finite = 0; if (out) for (int i=0; i<2560; ++i) finite += std::isfinite(out[i]);
    std::printf("LAYER0_STOCK_ADAPTER rc=%d activation=%s dimension=%d finite=%zu rss=%zu\n",
        rc, out ? "present" : "missing", 2560, finite, rss() >= before ? rss()-before : 0);
    if (rc == 0 && std::getenv("FLASHNEXT_COMPARE_DIR")) {
        constexpr size_t boundary_n = 2560 + 10240 + 4;
        constexpr size_t router_n = 512;
        std::vector<float> boundary(boundary_n), router(router_n);
        const size_t boundary_bytes = llama_cortex_copy_layer0_boundary(ctx, boundary.data(), boundary.size());
        const size_t router_bytes = llama_cortex_copy_layer0_router(ctx, router.data(), router.size());
        bool finite_capture = boundary_bytes == boundary_n * sizeof(float) && router_bytes == router_n * sizeof(float);
        for (float v : boundary) finite_capture = finite_capture && std::isfinite(v);
        for (float v : router) finite_capture = finite_capture && std::isfinite(v);
        const bool written = finite_capture &&
            write_capture(std::getenv("FLASHNEXT_COMPARE_DIR"), "segment_a_boundary", boundary.data(), boundary.size()) &&
            write_capture(std::getenv("FLASHNEXT_COMPARE_DIR"), "router_logits", router.data(), router.size());
        std::printf("CAPTURE boundary_values=%zu router_values=%zu finite=%s written=%s\n",
            boundary_bytes / sizeof(float), router_bytes / sizeof(float), finite_capture ? "YES" : "NO", written ? "YES" : "NO");
        if (!written) { llama_batch_free(batch); llama_free(ctx); llama_model_free(model); llama_backend_free(); return 14; }
        if (full_layer0) {
            const char * index_path = std::getenv("FLASHNEXT_TENSOR_INDEX");
            if (!index_path) { llama_batch_free(batch); llama_free(ctx); llama_model_free(model); llama_backend_free(); return 20; }
            quortex_moe_result moe;
            if (!quortex_compute_streamed_moe(index_path, boundary.data(), router.data(), moe) ||
                moe.selected.size() != 2560 || moe.shared.size() != 2560 || moe.complete.size() != 2560) {
                std::puts("STREAMED_MOE=FAIL");
                llama_batch_free(batch); llama_free(ctx); llama_model_free(model); llama_backend_free(); return 21;
            }
            const char * dir = std::getenv("FLASHNEXT_COMPARE_DIR");
            bool moe_finite = true;
            for (float v : moe.selected) moe_finite = moe_finite && std::isfinite(v);
            for (float v : moe.shared) moe_finite = moe_finite && std::isfinite(v);
            for (float v : moe.complete) moe_finite = moe_finite && std::isfinite(v);
            const bool moe_written = moe_finite &&
                write_capture(dir, "selected_moe", moe.selected.data(), moe.selected.size()) &&
                write_capture(dir, "shared_moe", moe.shared.data(), moe.shared.size()) &&
                write_capture(dir, "complete_moe", moe.complete.data(), moe.complete.size());
            if (!moe_written) { std::puts("STREAMED_MOE_CAPTURE=FAIL"); llama_batch_free(batch); llama_free(ctx); llama_model_free(model); llama_backend_free(); return 22; }

            // Replace the normal graph context with the explicitly selected
            // isolated Segment-B graph; no attention-path fallback is allowed.
            std::vector<float> segment_b_input(moe.complete.size() + boundary_n - 2560);
            std::copy(moe.complete.begin(), moe.complete.end(), segment_b_input.begin());
            std::copy(boundary.begin() + 2560, boundary.begin() + boundary_n, segment_b_input.begin() + moe.complete.size());
            llama_batch_free(batch); llama_free(ctx);
            setenv("CORTEX_FLASHNEXT_SEGMENT_B", "1", 1);
            setenv("CORTEX_FLASHNEXT_SEGMENT_B_INPUT_WIDTH", "12804", 1);
            llama_context * segment_ctx = llama_init_from_model(model, cp);
            if (!segment_ctx) { llama_model_free(model); llama_backend_free(); return 23; }
            llama_batch segment_batch = llama_batch_init(1, 12804, 1);
            if (!segment_batch.embd || segment_batch.token != nullptr) { llama_batch_free(segment_batch); llama_free(segment_ctx); llama_model_free(model); llama_backend_free(); return 23; }
            std::copy(segment_b_input.begin(), segment_b_input.end(), segment_batch.embd);
            segment_batch.pos[0] = 0; segment_batch.n_seq_id[0] = 1; segment_batch.seq_id[0][0] = 0; segment_batch.logits[0] = 1; segment_batch.n_tokens = 1;
            const int segment_rc = llama_decode(segment_ctx, segment_batch); llama_synchronize(segment_ctx);
            const float * segment_out = llama_get_embeddings_ith(segment_ctx, 0);
            bool segment_valid = segment_rc == 0 && segment_out != nullptr;
            for (size_t i = 0; segment_valid && i < 2560 * 4; ++i) segment_valid = std::isfinite(segment_out[i]);
            const bool segment_written = segment_valid && write_capture(dir, "post_segment_b", segment_out, 2560 * 4);
            std::printf("FULL_LAYER0_STREAMED rc=%d moe=INDEPENDENT segment_b=ISOLATED output=%s\n", segment_rc, segment_written ? "VALID" : "INVALID");
            llama_batch_free(segment_batch); llama_free(segment_ctx); llama_model_free(model); llama_backend_free();
            return segment_written ? 0 : 24;
        }
        std::vector<float> post_segment_b(2560 * 4);
        const size_t post_bytes = llama_cortex_copy_layer0_post_segment_b(ctx, post_segment_b.data(), post_segment_b.size());
        bool post_finite = post_bytes == post_segment_b.size() * sizeof(float);
        for (float v : post_segment_b) post_finite = post_finite && std::isfinite(v);
        const bool post_written = post_finite && write_capture(std::getenv("FLASHNEXT_COMPARE_DIR"), "post_segment_b", post_segment_b.data(), post_segment_b.size());
        std::printf("POST_SEGMENT_B_CAPTURE values=%zu finite=%s written=%s\n",
            post_bytes / sizeof(float), post_finite ? "YES" : "NO", post_written ? "YES" : "NO");
        if (!post_written) { llama_batch_free(batch); llama_free(ctx); llama_model_free(model); llama_backend_free(); return 15; }
        std::vector<float> pre_hc(2560);
        const size_t pre_hc_bytes = llama_cortex_copy_layer0_pre_hc(ctx, pre_hc.data(), pre_hc.size());
        bool pre_hc_finite = pre_hc_bytes == pre_hc.size() * sizeof(float);
        for (float v : pre_hc) pre_hc_finite = pre_hc_finite && std::isfinite(v);
        const bool pre_hc_written = pre_hc_finite && write_capture(std::getenv("FLASHNEXT_COMPARE_DIR"), "pre_hc_block_out", pre_hc.data(), pre_hc.size());
        std::printf("PRE_HC_CAPTURE values=%zu finite=%s written=%s\n",
            pre_hc_bytes / sizeof(float), pre_hc_finite ? "YES" : "NO", pre_hc_written ? "YES" : "NO");
        if (!pre_hc_written) { llama_batch_free(batch); llama_free(ctx); llama_model_free(model); llama_backend_free(); return 16; }
    }
    llama_batch_free(batch); llama_free(ctx); llama_model_free(model); llama_backend_free();
    return rc == 0 && out && finite == 2560 ? 0 : 3;
}
