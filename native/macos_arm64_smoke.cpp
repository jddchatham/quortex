// Minimal bounded ARM64 numerical smoke target for macOS bring-up.
//
// This target is intentionally model-free: it performs a deterministic
// float64 dot product over LCG-generated vectors and self-checks against an
// independently computed reference constant (see tests/test_macos_smoke.py,
// which recomputes the same value in Python).  It exists to prove, before any
// large-model work, that:
//   1. the host toolchain produces a working arm64-apple-darwin binary;
//   2. the binary runs to completion under the Darwin resource supervisor
//      (quortex/autonomy/macos_inference.py) without tripping guards;
//   3. native float64 numerics agree across independent implementations.
//
// Exit status is non-zero unless every self-check passes (fail-closed).

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <sys/resource.h>

namespace {

constexpr std::uint64_t kMultiplier = 6364136223846793005ULL;
constexpr std::uint64_t kIncrement = 1442695040888963407ULL;
constexpr std::size_t kLength = 1u << 20;

// Reference produced by an independent Python float64 implementation
// (tests/test_macos_smoke.py).  Agreement within 1e-9 relative is required.
constexpr double kReferenceDot = -840.45440460774125;

struct Lcg {
    std::uint64_t state;
    explicit Lcg(std::uint64_t seed) : state(seed) {}
    double next() {
        state = state * kMultiplier + kIncrement;
        return (static_cast<double>(state >> 33) / 2147483648.0) * 2.0 - 1.0;
    }
};

}  // namespace

int main() {
    Lcg a(0x5DEECE66DULL);
    Lcg b(0x2545F4914F6CDD1DULL);
    double dot = 0.0;
    for (std::size_t i = 0; i < kLength; ++i) {
        dot += a.next() * b.next();
    }

    const double rel = (dot - kReferenceDot) / kReferenceDot;
    struct rusage usage {};
    if (getrusage(RUSAGE_SELF, &usage) != 0) {
        usage.ru_maxrss = 0;
    }

    std::printf("SMOKE_DOT=%.17g\n", dot);
    std::printf("SMOKE_REL_ERR=%.3g\n", rel);
    std::printf("SMOKE_RSS_KB=%ld\n", static_cast<long>(usage.ru_maxrss));

    const bool pass = rel < 1e-9 && rel > -1e-9;
    std::printf("SMOKE_PASS=%d\n", pass ? 1 : 0);
    return pass ? 0 : 1;
}
