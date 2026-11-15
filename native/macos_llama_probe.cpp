// Deterministic bounded inference probe for macOS ARM64 qualification (M-MAC-02).
//
// Loads a GGUF model with the pinned Cortex llama library, decodes a fixed
// prompt, and emits greedy (argmax, temperature 0) generations together with
// SHA-256 fingerprints of the raw float32 logit vectors.  Everything printed
// is deterministic for a fixed build + model + prompt on the CPU backend:
//   - repeat runs must reproduce every printed field bit-for-bit;
//   - a two-token run must reproduce the one-token run's GEN0/LOGITS0
//     exactly (position-0 state continuity).
//
// Usage: macos-llama-probe MODEL.gguf [N_TOKENS]
// Exit status is non-zero on any failure (fail-closed).  No sampling
// randomness is used anywhere.

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
    if (n_gen < 1 || n_gen > 2) {
        std::fprintf(stderr, "N_TOKENS must be 1 or 2\n");
        return 2;
    }

    llama_backend_init();

    llama_model_params mparams = llama_model_default_params();
    mparams.n_gpu_layers = 0;  // CPU backend only
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
    std::printf("PROMPT_TOKENS=%d\n", n_cap);
    std::printf("BOS_ID=%d\n", static_cast<int>(llama_vocab_bos(vocab)));
    std::printf("N_VOCAB=%d\n", n_vocab);

    // Decode the full prompt in one batch; logits at the final position.
    llama_batch batch = llama_batch_get_one(prompt.data(), static_cast<int>(prompt.size()));
    if (llama_decode(ctx, batch) != 0) {
        std::fprintf(stderr, "DECODE_PROMPT_FAIL\n");
        return 1;
    }
    const float * logits = llama_get_logits_ith(ctx, -1);
    if (!logits) {
        std::fprintf(stderr, "LOGITS_NULL\n");
        return 1;
    }
    const size_t logit_bytes = static_cast<size_t>(n_vocab) * sizeof(float);
    std::printf("LOGITS0_SHA256=%s\n", sha256_hex(logits, logit_bytes).c_str());
    std::printf("LOGITS0_FIRST4=%.9g %.9g %.9g %.9g\n",
                static_cast<double>(logits[0]), static_cast<double>(logits[1]),
                static_cast<double>(logits[2]), static_cast<double>(logits[3]));

    llama_token gen0 = static_cast<llama_token>(argmax(logits, n_vocab));
    print_token_line("GEN", 0, vocab, gen0);

    if (n_gen >= 2) {
        llama_token in = gen0;
        llama_batch step = llama_batch_get_one(&in, 1);
        if (llama_decode(ctx, step) != 0) {
            std::fprintf(stderr, "DECODE_STEP_FAIL\n");
            return 1;
        }
        const float * l1 = llama_get_logits_ith(ctx, -1);
        if (!l1) {
            std::fprintf(stderr, "LOGITS1_NULL\n");
            return 1;
        }
        std::printf("LOGITS1_SHA256=%s\n", sha256_hex(l1, logit_bytes).c_str());
        llama_token gen1 = static_cast<llama_token>(argmax(l1, n_vocab));
        print_token_line("GEN", 1, vocab, gen1);
    }

    std::puts("PROBE_PASS=1");
    llama_free(ctx);
    llama_model_free(model);
    llama_backend_free();
    return 0;
}
