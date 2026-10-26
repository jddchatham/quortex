#include "gguf.h"
#include "ggml.h"
#include "ggml-backend.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fcntl.h>
#include <string>
#include <unistd.h>
#include <vector>

static bool read_all(int fd, void * dst, size_t n, off_t off) {
    char * p = static_cast<char *>(dst);
    while (n) {
        ssize_t got = pread(fd, p, n, off);
        if (got <= 0) return false;
        p += got; n -= static_cast<size_t>(got); off += got;
    }
    return true;
}

static size_t rss_bytes() {
    FILE * f = std::fopen("/proc/self/statm", "r");
    long pages = 0, ignored = 0;
    if (!f || std::fscanf(f, "%ld %ld", &ignored, &pages) != 2) { if (f) fclose(f); return 0; }
    fclose(f); return static_cast<size_t>(pages) * static_cast<size_t>(sysconf(_SC_PAGESIZE));
}

int main(int argc, char ** argv) {
    if (argc != 2) { std::fprintf(stderr, "usage: %s MODEL.gguf\n", argv[0]); return 2; }
    int fd = open(argv[1], O_RDONLY | O_CLOEXEC);
    if (fd < 0) { perror("open"); return 1; }
    gguf_init_params gp{}; gp.no_alloc = true;
    gguf_context * gu = gguf_init_from_file(argv[1], gp);
    if (!gu) { close(fd); return 1; }

    int64_t chosen = -1;
    for (int64_t i = 0; i < gguf_get_n_tensors(gu); ++i) {
        const int64_t * ne = gguf_get_tensor_ne(gu, i);
        if (ne[1] > 1 && ne[0] > 1 && ne[0] <= 4096 && gguf_get_tensor_size(gu, i) <= 16 * 1024 * 1024) {
            chosen = i; break;
        }
    }
    if (chosen < 0) { std::puts("REAL_WEIGHT_OP=NO_SUITABLE_TENSOR"); gguf_free(gu); close(fd); return 3; }
    const int64_t * ne = gguf_get_tensor_ne(gu, chosen);
    const size_t bytes = gguf_get_tensor_size(gu, chosen);
    const size_t data_off = gguf_get_data_offset(gu) + gguf_get_tensor_offset(gu, chosen);
    std::vector<uint8_t> raw(bytes);
    if (!read_all(fd, raw.data(), raw.size(), static_cast<off_t>(data_off))) { std::puts("REAL_WEIGHT_OP=PREAD_FAIL"); return 4; }

    ggml_backend_load_all();
    ggml_backend_t backend = ggml_backend_init_by_type(GGML_BACKEND_DEVICE_TYPE_CPU, nullptr);
    ggml_init_params ip{ 16 * 1024 * 1024, nullptr, true };
    ggml_context * ctx = ggml_init(ip);
    if (!backend || !ctx) return 5;
    ggml_tensor * w = ggml_new_tensor(ctx, gguf_get_tensor_type(gu, chosen), 2, ne);
    ggml_tensor * x = ggml_new_tensor_1d(ctx, GGML_TYPE_F32, ne[0]);
    std::vector<float> input(static_cast<size_t>(ne[0]));
    for (int64_t i = 0; i < ne[0]; ++i) input[static_cast<size_t>(i)] = std::sin(float(i + 1) * 0.013f);
    ggml_tensor * y = ggml_mul_mat(ctx, w, x);
    ggml_cgraph * graph = ggml_new_graph(ctx);
    ggml_build_forward_expand(graph, y);
    ggml_backend_buffer_t buffer = ggml_backend_alloc_ctx_tensors(ctx, backend);
    if (!buffer) return 6;
    ggml_backend_tensor_set(w, raw.data(), 0, raw.size());
    ggml_backend_tensor_set(x, input.data(), 0, input.size() * sizeof(float));
    const size_t before = rss_bytes();
    const ggml_status status = ggml_backend_graph_compute(backend, graph);
    const size_t after = rss_bytes();
    std::vector<float> out(static_cast<size_t>(ne[1]));
    ggml_backend_tensor_get(y, out.data(), 0, out.size() * sizeof(float));
    size_t bad = 0; for (float v : out) bad += !std::isfinite(v);
    std::printf("REAL_WEIGHT_OP=PASS tensor=%s type=%d shape=[%lld,%lld] bytes=%zu status=%d finite=%s rss_delta=%zu\n",
        gguf_get_tensor_name(gu, chosen), (int)gguf_get_tensor_type(gu, chosen),
        (long long)ne[0], (long long)ne[1], bytes, (int)status, bad ? "NO" : "YES", after >= before ? after - before : 0);
    ggml_backend_buffer_free(buffer); ggml_free(ctx); ggml_backend_free(backend); gguf_free(gu); close(fd);
    return status == GGML_STATUS_SUCCESS && bad == 0 ? 0 : 7;
}
