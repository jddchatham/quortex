# Director log

## 2026-10-22 — QIES initialization

Decision: accept the remote reconciliation; reject any claim that the current gate is an inference optimization. Prioritize deterministic transcript handling and direct-versus-wrapper comparison.

Evidence: remote `origin/quortex` and local `quortex` both point to `db9862e9fdb5cb679033682129c4a31c45261874`. Nine unit tests pass. Direct Qwen2.5 baseline is documented, while wrapper multi-turn semantics remain uncertified.

Next assignment: create a reproducible transcript harness and compare output/token behavior with the trusted backend.

