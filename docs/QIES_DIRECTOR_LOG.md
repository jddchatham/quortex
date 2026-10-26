# Director log

## 2026-10-22 — QIES initialization

Decision: accept the remote reconciliation; reject any claim that the current gate is an inference optimization. Prioritize deterministic transcript handling and direct-versus-wrapper comparison.

Evidence: remote `origin/quortex` and local `quortex` both point to `db9862e9fdb5cb679033682129c4a31c45261874`. Nine unit tests pass. Direct Qwen2.5 baseline is documented, while wrapper multi-turn semantics remain uncertified.

Next assignment: create a reproducible transcript harness and compare output/token behavior with the trusted backend.

## 2026-10-22 — Stage A single-turn accepted

The fixed-seed direct-versus-QUORTEX single-turn harness produced identical stdout and SHA-256 output hashes. Direct wall time was 4.012 s and wrapper wall time was 4.108 s. This passes correctness for the single-turn scope and proves no optimization. Interactive multi-turn behavior remains rejected pending a persistent-session harness.

Next assignment: establish deterministic persistent multi-turn session semantics and test whether state reuse avoids repeated prompt/model setup work.

## 2026-10-22 — Persistent harness cycle rejected

The bounded harness and protocol tests are reproducible, but the available
Cortex-era backend exits with `input is empty` after both PTY writes (return
code 255). Reject the multi-turn milestone and any optimization claim. Continue
from the recoverable harness with a backend interaction diagnosis.

## 2026-10-26 — M1 bounded retry remains open

The local Cortex-era `llama-cli` loaded the available 1.79 GiB Qwen2.5 GGUF and
accepted both PTY turns, but bounded 5–45 second probes did not complete prompt
evaluation/generation. The harness recorded incomplete sessions; no transcript
equivalence or state-reuse claim is accepted. No source change was justified.
