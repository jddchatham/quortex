// Deterministic bounded inference probe for macOS ARM64 - NO-REPACK VARIANT.
//
// Identical to macos_llama_probe.cpp (M-MAC-02) except for one load
// parameter: use_extra_bufts = false. The stock loader enables weight
// repacking (CPU_REPACK buffers), which copies repack-eligible tensors into
// anonymous RAM at load time. For small models that is fine; for the 98.42 GB
// Qwen3.8-Flash-Next (qwen4exp) model on a 16 GB host the repack copies do
// not fit - the process is killed by the kernel around blk.40 of 48 during
// load (measured 2026-11-17, peak RSS 8.25 GB before kill).
//
// With extra buffer types disabled, every tensor stays in the plain CPU
// buffer, which is file-backed (mmap) under use_mmap=true: bounded resident
// memory, no repack copies. This is a memory-safety adaptation of the
// harness, not a numerics change: standard ggml kernels replace the repacked
// tile kernels, results stay deterministic per build. This harness exists
// only for large artifacts that cannot survive repack.
//
// Same contract as the original probe: fixed prompt, greedy argmax,
// SHA-256 fingerprints of raw fp32 logits, fail-closed exit codes.

#include "llama.h"

#include <CommonCrypto/CommonDigest.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr const char * kPrompt = "The capital of France is";

std::string sha256_hex(const void * data, size_t len) {
    std::uint8_t digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(data, static_cast<CC_LONG>(len), digest);
    char out[2 * CC_SHA256_DIGEST_LENGTH + 1];
    for (size_t i = 0; i < CC_SHA256_DIGEST_LENGTH; ++i) {
        std::snprintf(out + 2 * i, 3, "%02x", digest[i]);
    }
    return std::string(out, 2 * CC_SHA256_DIGEST_LENGTH);
}

int argmax(const float * logits, int n) {
    int best = 0;
    for (int i = 1; i < n; ++i) {
        if (logits[i] > logits[best]) best = i;
    }
    return best;
}

std::string piece(const llama_vocab * vocab, llama_token token) {
    char buf[128];
    const int n = llama_token_to_piece(vocab, token, buf, sizeof(buf), 0, true);
    if (n < 0) return "<err>";
    return std::string(buf, static_cast<size_t>(n));
}

void print_token_line(const char * tag, int index, const llama_vocab * vocab, llama_token token) {
    std::printf("%s%d_ID=%d\n", tag, index, static_cast<int>(token));
    const std::string p = piece(vocab, token);
    std::printf("%s%d_PIECE=", tag, index);
    for (char c : p) {
        if (c == '\n') std::fputs("\\n", stdout);
        else if (static_cast<unsigned char>(c) < 0x20) std::printf("\\x%02x", c);
        else std::fputc(c, stdout);
    }
    std::fputc('\n', stdout);
}

}  // namespace

int main(int argc, char ** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s MODEL.gguf [N_TOKENS]\n", argv[0]);
        return 2;
    }
    const int n_gen = argc > 2 ? std::atoi(argv[2]) : 1;
    if (n_gen < 1 || n_gen > 64) {
        std::fprintf(stderr, "N_TOKENS must be between 1 and 64\n");
        return 2;
    }

    llama_backend_init();

    llama_model_params mparams = llama_model_default_params();
    mparams.n_gpu_layers = 0;  // CPU backend only
    mparams.use_extra_bufts = false;  // keep every tensor in the mmap-backed CPU buffer (bounded RSS)
    llama_model * model = llama_model_load_from_file(argv[1], mparams);
    if (!model) {
        std::fprintf(stderr, "MODEL_LOAD_FAIL path=%s\n", argv[1]);
        llama_backend_free();
        return 1;
    }
    const llama_vocab * vocab = llama_model_get_vocab(model);
    const int n_vocab = llama_vocab_n_tokens(vocab);

    llama_context_params cparams = llama_context_default_params();
    cparams.n_ctx = 512;
    cparams.n_batch = 512;
    cparams.n_threads = 4;
    cparams.n_threads_batch = 4;
    // embeddings=true marks every input position as a graph output; on this
    // pinned build the qwen4exp graph then reads its output tensor out of
    // bounds (GGML_ASSERT "tensor read out of bounds", measured 2026-11-17).
    // We only need last-position logits, so keep the standard single-output
    // decode path.
    cparams.embeddings = false;
    // Greedy argmax is used for generation; no RNG is involved anywhere.
    llama_context * ctx = llama_init_from_model(model, cparams);
    if (!ctx) {
        std::fprintf(stderr, "CTX_INIT_FAIL\n");
        llama_model_free(model);
        llama_backend_free();
        return 1;
    }

    // Tokenize the fixed prompt (add BOS per model config).
    const int plen = static_cast<int>(std::strlen(kPrompt));
    const int n_cap = -llama_tokenize(vocab, kPrompt, plen, nullptr, 0, true, true);
    if (n_cap <= 0) {
        std::fprintf(stderr, "TOKENIZE_LEN_FAIL\n");
        return 1;
    }
    std::vector<llama_token> prompt(static_cast<size_t>(n_cap));
    if (llama_tokenize(vocab, kPrompt, plen, prompt.data(), n_cap, true, true) != n_cap) {
        std::fprintf(stderr, "TOKENIZE_FAIL\n");
        return 1;
    }
    if (const char * ids_env = std::getenv("PROBE_IDS")) {
        prompt.clear();
        for (const char * p = ids_env; *p; ) {
            int v = 0, digits = 0;
            while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); ++p; ++digits; }
            if (!digits) return 2;
            prompt.push_back((llama_token) v);
            if (*p == ',') ++p; else if (*p) return 2;
        }
    }
    std::printf("PROMPT_TOKENS=%d\n", n_cap);
    if (const char * mode_env = std::getenv("PROBE_TOKENIZE_ONLY")) {
        if (mode_env[0] == '1') {
            std::string ids, detok;
            for (int i = 0; i < n_cap; ++i) {
                ids += std::to_string(static_cast<int>(prompt[(size_t) i]));
                if (i + 1 < n_cap) ids += ",";
                detok += piece(vocab, prompt[(size_t) i]);
            }
            std::printf("PROMPT_IDS=%s\n", ids.c_str());
            std::printf("DETOK=");
            for (char c : detok) {
                if (c == '\n') std::fputs("\\n", stdout);
                else std::fputc(c, stdout);
            }
            std::fputc('\n', stdout);
            llama_free(ctx);
            llama_model_free(model);
            llama_backend_free();
            return 0;
        }
    }
    std::printf("BOS_ID=%d\n", static_cast<int>(llama_vocab_bos(vocab)));
    std::printf("N_VOCAB=%d\n", n_vocab);

    // Decode the full prompt in one batch; logits at the final position.
    llama_batch batch = llama_batch_get_one(prompt.data(), static_cast<int>(prompt.size()));
    if (llama_decode(ctx, batch) != 0) { std::fprintf(stderr, "DECODE_PROMPT_FAIL\n"); return 1; }
    const float * logits = llama_get_logits_ith(ctx, -1);
    if (!logits) {
        std::fprintf(stderr, "LOGITS_NULL\n");
        return 1;
    }
    const size_t logit_bytes = static_cast<size_t>(n_vocab) * sizeof(float);
    std::printf("LOGITS0_SHA256=%s\n", sha256_hex(logits, logit_bytes).c_str());
    if (const char * p = std::getenv("PROBE_DUMP_LOGITS")) {
        FILE * f = std::fopen(p, "wb");
        if (f) { std::fwrite(logits, sizeof(float), (size_t) n_vocab, f); std::fclose(f); }
    }
    if (const char * p = std::getenv("PROBE_DUMP_HIDDEN")) {
        const float * emb = llama_get_embeddings_ith(ctx, -1);
        if (emb) {
            FILE * f = std::fopen(p, "wb");
            if (f) { std::fwrite(emb, sizeof(float), (size_t) llama_model_n_embd_inp(model), f); std::fclose(f); }
        }
    }
    std::printf("LOGITS0_FIRST4=%.9g %.9g %.9g %.9g\n",
                static_cast<double>(logits[0]), static_cast<double>(logits[1]),
                static_cast<double>(logits[2]), static_cast<double>(logits[3]));

    llama_token prev = static_cast<llama_token>(argmax(logits, n_vocab));
    print_token_line("GEN", 0, vocab, prev);

    for (int i = 1; i < n_gen; ++i) {
        llama_batch step = llama_batch_get_one(&prev, 1);
        if (llama_decode(ctx, step) != 0) {
            std::fprintf(stderr, "DECODE_STEP_%d_FAIL\n", i);
            return 1;
        }
        const float * li = llama_get_logits_ith(ctx, -1);
        if (!li) {
            std::fprintf(stderr, "LOGITS%d_NULL\n", i);
            return 1;
        }
        std::printf("LOGITS%d_SHA256=%s\n", i, sha256_hex(li, logit_bytes).c_str());
        prev = static_cast<llama_token>(argmax(li, n_vocab));
        print_token_line("GEN", i, vocab, prev);
    }

    std::puts("PROBE_PASS=1");
    llama_free(ctx);
    llama_model_free(model);
    llama_backend_free();
    return 0;
}
