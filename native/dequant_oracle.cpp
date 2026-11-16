// Dequant oracle: dump selected GGUF tensors as F32 via ggml identity mul_mat.
// Usage: dequant-oracle MODEL.gguf OUTDIR NAME1 NAME2 ...
// Writes OUTDIR/<name>.f32 (float32, row-major, GGUF dims order).
#include "ggml.h"
#include "ggml-backend.h"
#include "gguf.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

int main(int argc, char ** argv) {
    if (argc < 4) { std::fprintf(stderr, "usage: %s MODEL OUTDIR NAME...\n", argv[0]); return 2; }
    int fd = open(argv[1], O_RDONLY | O_CLOEXEC);
    if (fd < 0) { std::perror("open"); return 1; }
    gguf_init_params gp{}; gp.no_alloc = true;
    gguf_context * g = gguf_init_from_file(argv[1], gp);
    if (!g) { std::puts("GGUF_OPEN_FAIL"); return 1; }
    size_t data_off = gguf_get_data_offset(g);
    mkdir(argv[2], 0755);

    ggml_backend_t backend = ggml_backend_init_by_type(GGML_BACKEND_DEVICE_TYPE_CPU, nullptr);
    if (!backend) { std::puts("BACKEND_FAIL"); return 1; }

    for (int a = 3; a < argc; ++a) {
        const char * name = argv[a];
        int id = -1;
        for (int i = 0; i < gguf_get_n_tensors(g); ++i)
            if (std::strcmp(gguf_get_tensor_name(g, i), name) == 0) { id = i; break; }
        if (id < 0) { std::fprintf(stderr, "MISSING %s\n", name); return 1; }
        const int64_t * ne = gguf_get_tensor_ne(g, id);
        enum ggml_type ty = gguf_get_tensor_type(g, id);
        size_t nbytes = gguf_get_tensor_size(g, id);
        std::vector<uint8_t> raw(nbytes);
        if (pread(fd, raw.data(), nbytes, (off_t)(data_off + gguf_get_tensor_offset(g, id))) != (ssize_t) nbytes) {
            std::fprintf(stderr, "PREAD_FAIL %s\n", name); return 1;
        }
        if (ty == GGML_TYPE_F32) {
            std::string out = std::string(argv[2]) + "/" + name + ".f32";
            FILE * f = std::fopen(out.c_str(), "wb");
            std::fwrite(raw.data(), 1, nbytes, f); std::fclose(f);
            std::printf("DUMP %s dims=[%lld,%lld] f32-direct\n", name, (long long) ne[0], (long long) ne[1]);
            continue;
        }
        // dequant via identity: mul_mat(W [in,out], I [in,in]) -> [out,in] = W^T (dequant)
        // chunk the identity rows to bound memory: result [out, k] per chunk
        const int64_t n_in = ne[0], n_out = ne[1] > 0 ? ne[1] : 1;
        ggml_context * ctx = ggml_init({ 768 * 1024 * 1024, nullptr, false });
        if (!ctx) { std::puts("CTX_FAIL"); return 1; }
        std::string out = std::string(argv[2]) + "/" + name + ".f32";
        FILE * f = std::fopen(out.c_str(), "wb");
        const int64_t CH = 128;
        for (int64_t base = 0; base < n_in; base += CH) {
            int64_t rows = n_in - base < CH ? n_in - base : CH;
            ggml_tensor * w = ggml_new_tensor(ctx, ty, 2, (int64_t[]){n_in, n_out});
            std::memcpy(w->data, raw.data(), nbytes);
            ggml_tensor * ic = ggml_new_tensor(ctx, GGML_TYPE_F32, 2, (int64_t[]){n_in, rows});
            for (int64_t r = 0; r < rows; ++r) {
                float * row = (float *) ic->data + r * n_in;
                for (int64_t c = 0; c < n_in; ++c) row[c] = (c == base + r) ? 1.0f : 0.0f;
            }
            ggml_tensor * res = ggml_mul_mat(ctx, w, ic);      // [n_out, rows]
            ggml_cgraph * gf = ggml_new_graph(ctx);
            ggml_build_forward_expand(gf, res);
            if (ggml_backend_graph_compute(backend, gf) != GGML_STATUS_SUCCESS) { std::puts("COMPUTE_FAIL"); return 1; }
            // res[m=out, n] = sum_k W[k,m] I[k,n] -> dequant(W)[k, m] at I's k=base+n
            // emit transposed: for each n -> write column of res as W row (base+n)
            std::vector<float> col((size_t) n_out);
            for (int64_t n = 0; n < rows; ++n) {
                for (int64_t m = 0; m < n_out; ++m) col[(size_t) m] = ((float *) res->data)[m * rows + n];
                std::fwrite(col.data(), sizeof(float), (size_t) n_out, f);
            }
            ggml_free(ctx);
            ctx = ggml_init({ 768 * 1024 * 1024, nullptr, false });
            if (!ctx) { std::puts("CTX_REINIT_FAIL"); return 1; }
        }
        std::fclose(f);
        std::printf("DUMP %s dims=[%lld,%lld] dequant\n", name, (long long) ne[0], (long long) ne[1]);
    }
    return 0;
}
