# Next action

State: `NEXT_ASSIGNMENT`

Assignment: obtain a bounded-compatible llama.cpp/GGUF fixture or profiled backend configuration, then compare direct llama.cpp and QUORTEX over the fixed two-turn harness. Do not accept the milestone unless transcript semantics are explained and reproducible. The current backend loads and accepts both turns, but does not finish generation within the bounded CPU probe.

After acceptance, investigate persistent prompt/state reuse as the first actual inference-path optimization hypothesis.

The harness now uses `--interactive-first`; a bounded direct-versus-persistent
completion run remains required before M1 can be accepted.
