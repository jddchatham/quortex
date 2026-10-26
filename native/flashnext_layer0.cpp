#include "llama.h"
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <fstream>
#include <string>
#include <unistd.h>

static size_t rss() {
    std::ifstream f("/proc/self/statm"); size_t total=0,resident=0;
    long page=sysconf(_SC_PAGESIZE); f >> total >> resident; return resident*page;
}

int main(int argc, char ** argv) {
    if (argc != 2) { std::fprintf(stderr, "usage: %s MODEL.gguf\n", argv[0]); return 2; }
    llama_backend_init();
    llama_model_params mp = llama_model_default_params();
    llama_model * model = llama_model_load_from_file(argv[1], mp);
    if (!model) { std::puts("LAYER0_LOAD=FAIL"); llama_backend_free(); return 1; }
    llama_context_params cp = llama_context_default_params();
    cp.n_ctx = 4; cp.n_batch = 1; cp.embeddings = true;
    llama_context * ctx = llama_init_from_model(model, cp);
    if (!ctx) { llama_model_free(model); llama_backend_free(); return 1; }
    llama_batch batch = llama_batch_init(1, 0, 1);
    batch.token[0] = 248045; batch.pos[0] = 0; batch.n_seq_id[0] = 1;
    batch.seq_id[0][0] = 0; batch.logits[0] = 1; batch.n_tokens = 1;
    const size_t before = rss();
    const int rc = llama_decode(ctx, batch);
    llama_synchronize(ctx);
    const float * out = llama_get_embeddings_ith(ctx, 0);
    size_t finite = 0; if (out) for (int i=0; i<2560; ++i) finite += std::isfinite(out[i]);
    std::printf("LAYER0_STOCK_ADAPTER rc=%d activation=%s dimension=%d finite=%zu rss=%zu\n",
        rc, out ? "present" : "missing", 2560, finite, rss() >= before ? rss()-before : 0);
    llama_batch_free(batch); llama_free(ctx); llama_model_free(model); llama_backend_free();
    return rc == 0 && out && finite == 2560 ? 0 : 3;
}
