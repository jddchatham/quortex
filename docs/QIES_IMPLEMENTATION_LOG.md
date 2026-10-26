# Implementation log

## db9862e

Added the trusted-backend launcher, pre-load contextual gate, JSON decision mode, and tests. A review found and fixed an initial bug where JSON inspection still launched the backend.

## Current limitation

The launcher delegates conversation formatting to the existing llama.cpp runtime. The first integrated interactive run showed unexpected transcript behavior; no multi-turn correctness claim is accepted until a deterministic harness passes.

## 2026-10-22 — Persistent session harness bounded

Added `benchmarks/persistent_session.py`, which keeps one backend process alive,
sends a fixed turn sequence through a PTY, and records command, requested/sent
turns, exit code, timeout, elapsed time, raw combined output, and SHA-256. Its
protocol tests use a deterministic two-turn fake backend.

The real Qwen2.5/Cortex `llama-cli` probe was bounded to 8 seconds and sent both
turns, but exited with code 255 and `input is empty` before producing a reply.
This is a backend interaction blocker; no persistent transcript equivalence or
state-reuse claim is accepted.

## 2026-10-26 — M1 bounded retry

The backend loaded the local Qwen2.5 GGUF and the PTY harness sent both turns,
but bounded CPU probes did not finish prompt evaluation/generation. The record
was incomplete and no implementation change was made.

## 2026-10-26 — M1-DIAG repair

Diagnosis: `-cnv` lacked `--interactive-first`, so the first input boundary
depended on the backend's initial generation path. The harness now explicitly
waits for the first user turn, with a regression assertion. Focused tests pass
35/35; model-backed transcript equivalence remains unverified.

## 2026-10-26 — M3-EXEC-01 bounded probe

The external Cortex owned-layer executable completed a real-weight layer-1 to
layer-2 transaction chain in one process using the three-shard FlashNext GGUF:
the activation handoff was byte-exact, both state commits passed, output was
finite, and unselected-expert reads were zero. This is partial evidence only:
the probe starts at layer 1, so the required layer-0 producer feeding layer 1
remains open.

## 2026-10-26 — M3-EXEC-01 bounded retry

The available external executable was invoked with the real layer-0 chain and
a 90-second bound. It aborted during graph setup with
`GGML_ASSERT(tensor->buffer == NULL)` before emitting a layer-0 or layer-1
pass. No QUORTEX execution-plane source was changed; acceptance remains open.
