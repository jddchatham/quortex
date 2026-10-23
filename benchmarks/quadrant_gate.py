"""Cheap falsifiable experiment: candidate work with and without pre-gating."""

from time import perf_counter

from quortex import Candidate, Context, Officiator


def expensive_candidate_work(candidate: Candidate) -> int:
    # A deterministic stand-in for model work. The counter is intentionally
    # reported; this benchmark does not pretend that Python arithmetic is a
    # Cortex kernel or that the result predicts model latency.
    total = 0
    for index in range(20_000):
        total = (total + index * (len(candidate.key) + len(candidate.properties))) & 0xFFFFFFFF
    return total


def run(candidates, context):
    judge = Officiator()
    start = perf_counter()
    baseline_results = [expensive_candidate_work(candidate) for candidate in candidates]
    baseline_time = perf_counter() - start

    start = perf_counter()
    gated_results = []
    admitted = 0
    for candidate in candidates:
        if judge.admit({"properties": candidate.properties}, context):
            admitted += 1
            gated_results.append(expensive_candidate_work(candidate))
    gated_time = perf_counter() - start
    return {
        "candidates": len(candidates),
        "baseline_evaluations": len(baseline_results),
        "gated_evaluations": admitted,
        "avoided_evaluations": len(candidates) - admitted,
        "baseline_seconds": baseline_time,
        "gated_seconds": gated_time,
        "results_preserved_for_admitted": all(value in baseline_results for value in gated_results),
    }


if __name__ == "__main__":
    context = Context.from_requirements({"basket", "ball", "official-2026"})
    candidates = [
        Candidate(f"candidate-{i}", "basket", "ball", frozenset(properties))
        for i, properties in enumerate(
            [
                {"basket", "ball"},
                {"basket", "ball", "official-2026"},
                {"basket", "ball", "official-2027"},
                {"basket", "ball"},
            ]
        )
    ]
    print(run(candidates, context))

