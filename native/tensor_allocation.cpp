#include "ggml.h"
#include "ggml-backend.h"

#include <cstdio>

int main() {
    ggml_backend_load_all();
    ggml_backend_t backend = ggml_backend_init_by_type(GGML_BACKEND_DEVICE_TYPE_CPU, nullptr);
    if (!backend) { std::puts("BACKEND_INIT=FAIL"); return 1; }

    ggml_init_params params{ 256 * 1024, nullptr, true };
    ggml_context * ctx = ggml_init(params);
    if (!ctx) { ggml_backend_free(backend); return 2; }
    ggml_tensor * tensor = ggml_new_tensor_1d(ctx, GGML_TYPE_F32, 16);
    std::printf("BEFORE name=%s buffer=%p data=%p\n", tensor->name, (void *)tensor->buffer, tensor->data);
    ggml_backend_buffer_t buffer = ggml_backend_alloc_ctx_tensors(ctx, backend);
    if (!buffer) { std::puts("ALLOCATE=FAIL"); ggml_free(ctx); ggml_backend_free(backend); return 3; }
    std::printf("AFTER name=%s buffer=%p data=%p bytes=%zu\n", tensor->name,
                (void *)tensor->buffer, tensor->data, ggml_nbytes(tensor));
    const bool owned = tensor->buffer == buffer && tensor->data != nullptr;
    std::printf("OWNERSHIP=%s\n", owned ? "PASS" : "FAIL");
    ggml_backend_buffer_free(buffer);
    ggml_free(ctx);
    ggml_backend_free(backend);
    return owned ? 0 : 4;
}
