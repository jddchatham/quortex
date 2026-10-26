# Next action

State: `NEXT_ASSIGNMENT`

Assignment: obtain a bounded-compatible llama.cpp/GGUF fixture or profiled backend configuration, then compare direct llama.cpp and QUORTEX over the fixed two-turn harness. Do not accept the milestone unless transcript semantics are explained and reproducible. The current backend loads and accepts both turns, but does not finish generation within the bounded CPU probe.

After acceptance, investigate persistent prompt/state reuse as the first actual inference-path optimization hypothesis.

The harness now uses `--interactive-first`; a bounded direct-versus-persistent
completion run remains required before M1 can be accepted.

M3-PREP is complete for this cycle: target shard fingerprints and the Cortex
provider/`pread` entry points are recorded in `docs/QIES_LARGE_MODEL_STATUS.md`.
Next M3 work must use the recorded external artifacts and remain bounded; do
not begin full generation without an explicit acceptance experiment.

M3-EXEC-01 partial result: a real-weight external Cortex layer-1-to-layer-2
chain passed activation and state handoff. Continue by exposing a bounded
layer-0 producer and an independent reference before accepting the assignment.

The bounded retry reached external graph setup but aborted on
`GGML_ASSERT(tensor->buffer == NULL)` (return code 134). The available Cortex
checkout is dirty and no compatible alternate or local QUORTEX execution seam
was found in this cycle. Obtain or rebuild a compatible executable, or
explicitly authorize exposing a local seam, before retrying.
