#include "gguf.h"
#include "ggml.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

int main(int argc, char ** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s MODEL.gguf\n", argv[0]);
        return 2;
    }
    gguf_init_params params{};
    params.no_alloc = true;
    params.ctx = nullptr;
    gguf_context * ctx = gguf_init_from_file(argv[1], params);
    if (!ctx) {
        std::fprintf(stderr, "GGUF_OPEN_FAIL path=%s\n", argv[1]);
        return 1;
    }
    const int64_t n = gguf_get_n_tensors(ctx);
    // The source revision is reported by CMake (GGML 0.25.3, Cortex 8da466d).
    std::puts("GGML_VERSION=0.25.3");
    std::printf("GGUF_VERSION=%u TENSORS=%lld DATA_OFFSET=%zu ALIGNMENT=%zu\n",
                gguf_get_version(ctx), (long long)n,
                gguf_get_data_offset(ctx), gguf_get_alignment(ctx));
    const int64_t id = n > 0 ? 0 : -1;
    if (id >= 0) {
        const int64_t * ne = gguf_get_tensor_ne(ctx, id);
        std::printf("TENSOR_0 name=%s type=%d bytes=%zu shape=[%lld,%lld,%lld,%lld]\n",
                    gguf_get_tensor_name(ctx, id), (int)gguf_get_tensor_type(ctx, id),
                    gguf_get_tensor_size(ctx, id), (long long)ne[0], (long long)ne[1],
                    (long long)ne[2], (long long)ne[3]);
    }
    gguf_free(ctx);
    std::puts("ALLOCATION_DIAGNOSTIC=METADATA_ONLY_NO_TENSOR_ALLOC");
    return 0;
}
