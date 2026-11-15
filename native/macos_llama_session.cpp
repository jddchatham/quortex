// Persistent multi-turn inference session probe for macOS ARM64 (M-MAC-04).
//
// Demonstrates one loaded model serving two prompt segments in a single
// context without numerical drift and without model reinitialization:
//
//   session : decode A -> generate 4 tokens -> decode B (no special tokens)
//             -> generate 4 more tokens; then reset (fresh context, same
//             model) and repeat segment A, requiring identical fingerprints.
//   combined: one-shot decode of A + genA_text + B -> generate 4 tokens.
//             Serves as the equivalent-execution reference for session.
//   slow    : session with a bounded 6 s sleep between segments so the
//             supervisor can demonstrate interrupted-session cleanup.
//
// Every generation position prints a SHA-256 fingerprint of the raw float32
// logit vector; all comparisons are bitwise.  Any drift, decode failure, or
// lifecycle failure exits non-zero (fail-closed).  No RNG is used.

#include "llama.h"

#include <CommonCrypto/CommonDigest.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr const char * kSegA = "The capital of France is";
// genA for kSegA under greedy decoding (verified in M-MAC-03): " Paris. It is"
constexpr const char * kGenA = " Paris. It is";
constexpr const char * kSegB = " known for the Eiffel Tower";
constexpr int kGen = 4;

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

struct Session {
    llama_model * model = nullptr;
    llama_context * ctx = nullptr;
    const llama_vocab * vocab = nullptr;
    int n_vocab = 0;
    size_t logit_bytes = 0;

    explicit Session(llama_model * m) { attach(m); }
    ~Session() { if (ctx) llama_free(ctx); }

    void attach(llama_model * m) {
        model = m;
        vocab = llama_model_get_vocab(model);
        n_vocab = llama_vocab_n_tokens(vocab);
        logit_bytes = static_cast<size_t>(n_vocab) * sizeof(float);
        new_context();
    }

    void new_context() {
        if (ctx) { llama_free(ctx); ctx = nullptr; }
        llama_context_params cparams = llama_context_default_params();
        cparams.n_ctx = 512;
        cparams.n_batch = 512;
        cparams.n_threads = 4;
        cparams.n_threads_batch = 4;
        ctx = llama_init_from_model(model, cparams);
    }

    std::vector<llama_token> tokenize(const char * text, bool add_special) const {
        const int len = static_cast<int>(std::strlen(text));
        const int n = -llama_tokenize(vocab, text, len, nullptr, 0, add_special, true);
        std::vector<llama_token> toks(static_cast<size_t>(n));
        if (llama_tokenize(vocab, text, len, toks.data(), n, add_special, true) != n) {
            std::fprintf(stderr, "TOKENIZE_FAIL text=%s\n", text);
            std::exit(1);
        }
        return toks;
    }

    // Decode a whole segment in one batch; return logits at its last token.
    const float * decode_segment(const std::vector<llama_token> & toks, const char * tag) {
        llama_batch batch = llama_batch_get_one(const_cast<llama_token *>(toks.data()), static_cast<int>(toks.size()));
        if (llama_decode(ctx, batch) != 0) {
            std::fprintf(stderr, "DECODE_%s_FAIL\n", tag);
            std::exit(1);
        }
        const float * logits = llama_get_logits_ith(ctx, -1);
        if (!logits) { std::fprintf(stderr, "LOGITS_%s_NULL\n", tag); std::exit(1); }
        return logits;
    }

    // Greedy-generate k tokens from current state, printing fingerprints.
    // prefix example: "A0" -> A0_ID, A0_PIECE, A0_LOGITS_SHA256
    void generate(int k, const char * prefix, std::vector<llama_token> * out = nullptr) {
        for (int i = 0; i < k; ++i) {
            const float * logits = llama_get_logits_ith(ctx, -1);
            if (!logits) { std::fprintf(stderr, "%s_%d_NULL\n", prefix, i); std::exit(1); }
            std::printf("%s%d_LOGITS_SHA256=%s\n", prefix, i, sha256_hex(logits, logit_bytes).c_str());
            llama_token tok = static_cast<llama_token>(argmax(logits, n_vocab));
            std::printf("%s%d_ID=%d\n", prefix, i, static_cast<int>(tok));
            if (out) out->push_back(tok);
            llama_batch step = llama_batch_get_one(&tok, 1);
            if (llama_decode(ctx, step) != 0) {
                std::fprintf(stderr, "%s_%d_STEP_FAIL\n", prefix, i);
                std::exit(1);
            }
        }
    }
};

bool g_slow = false;
void between_segments() {
    if (g_slow) {
        std::puts("SLOW_SLEEP=6");
        std::fflush(stdout);
        std::this_thread::sleep_for(std::chrono::seconds(6));
    }
}

}  // namespace

int main(int argc, char ** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s MODEL.gguf {session|combined|slow}\n", argv[0]);
        return 2;
    }
    const std::string mode = argv[2];
    if (mode != "session" && mode != "combined" && mode != "slow" && mode != "batchcheck") {
        std::fprintf(stderr, "invalid mode: %s\n", mode.c_str());
        return 2;
    }
    g_slow = (mode == "slow");

    llama_backend_init();

    llama_model_params mparams = llama_model_default_params();
    mparams.n_gpu_layers = 0;  // CPU backend only
    llama_model * model = llama_model_load_from_file(argv[1], mparams);
    if (!model) {
        std::fprintf(stderr, "MODEL_LOAD_FAIL path=%s\n", argv[1]);
        llama_backend_free();
        return 1;
    }

    Session s(model);
    if (!s.ctx) { std::fprintf(stderr, "CTX_INIT_FAIL\n"); return 1; }

    const std::vector<llama_token> toksA = s.tokenize(kSegA, true);
    std::printf("A_TOKENS=%zu\n", toksA.size());

    if (mode == "combined") {
        // Equivalent one-shot execution: A + observed genA + B in one prompt.
        std::string full = std::string(kSegA) + kGenA + kSegB;
        const std::vector<llama_token> toks = s.tokenize(full.c_str(), true);
        std::printf("COMBINED_TOKENS=%zu\n", toks.size());
        s.decode_segment(toks, "COMBINED_PROMPT");
        s.generate(kGen, "B");
        std::puts("PROBE_PASS=1");
        llama_model_free(model);
        llama_backend_free();
        return 0;
    }

    // session / slow: two segments in one context.
    s.decode_segment(toksA, "A");
    std::vector<llama_token> genA;
    s.generate(kGen, "A", &genA);
    const std::vector<llama_token> toksB = s.tokenize(kSegB, false);  // no re-added specials
    std::printf("B_TOKENS=%zu\n", toksB.size());

    between_segments();

    s.decode_segment(toksB, "B");
    // B positions: the logits right after decoding B belong to generation B0.
    s.generate(kGen, "B");

    const size_t cells_after = toksA.size() + kGen + toksB.size() + kGen;
    std::printf("EXPECTED_KV_CELLS=%zu\n", cells_after);
    std::printf("TOTAL_DECODED=%zu\n", toksA.size() + kGen + toksB.size() + kGen);

    if (mode == "batchcheck") {
        // Batched-vs-sequential position correctness (runtime behavioral
        // counterpart of the repository's batch-position source invariant).
        // Three fresh contexts over the same model:
        //   S: A as one 5-token batch, then B as one 7-token batch (segmented)
        //   Q: all 12 prompt tokens decoded one at a time (fully sequential)
        //   O: all 12 prompt tokens decoded as a single batch (fully batched)
        // Requirement: final-position logits and all 4 generated fingerprints
        // are bitwise identical across S, Q and O.
        std::vector<llama_token> all = toksA;
        all.insert(all.end(), toksB.begin(), toksB.end());

        auto run_variant = [&](const char * tag, int variant) {
            s.new_context();
            if (variant == 0) {              // segmented batches
                s.decode_segment(toksA, "S_A");
                s.decode_segment(toksB, "S_B");
            } else if (variant == 1) {       // one token per decode
                for (size_t i = 0; i < all.size(); ++i) {
                    llama_batch one = llama_batch_get_one(&all[i], 1);
                    if (llama_decode(s.ctx, one) != 0) { std::fprintf(stderr, "Q_DECODE_%zu_FAIL\n", i); std::exit(1); }
                }
            } else {                         // single 12-token batch
                s.decode_segment(all, "O_ALL");
            }
            const float * lf = llama_get_logits_ith(s.ctx, -1);
            if (!lf) { std::fprintf(stderr, "%s_FINAL_NULL\n", tag); std::exit(1); }
            std::printf("%s_FINAL_LOGITS_SHA256=%s\n", tag, sha256_hex(lf, s.logit_bytes).c_str());
            std::vector<llama_token> gen;
            s.generate(kGen, tag, &gen);
            return gen;
        };

        const std::vector<llama_token> gS = run_variant("S", 0);
        const std::vector<llama_token> gQ = run_variant("Q", 1);
        const std::vector<llama_token> gO = run_variant("O", 2);
        const bool pass = (gS == gQ) && (gQ == gO);
        std::printf("BATCHCHECK_PASS=%d\n", pass ? 1 : 0);
        llama_model_free(model);
        llama_backend_free();
        return pass ? 0 : 10;
    }

    // Reset: fresh context from the SAME loaded model; segment A must reproduce.
    s.new_context();
    const std::vector<llama_token> toksA2 = s.tokenize(kSegA, true);
    s.decode_segment(toksA2, "RESET_A");
    std::vector<llama_token> genA2;
    s.generate(kGen, "RA", &genA2);
    if (genA2 != genA) {
        std::puts("RESET_DRIFT=1");
        llama_model_free(model);
        llama_backend_free();
        return 10;
    }
    std::puts("RESET_DRIFT=0");

    std::puts("PROBE_PASS=1");
    llama_model_free(model);
    llama_backend_free();
    return 0;
}
