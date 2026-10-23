# Next action

State: `AUDIT`

Assignment: implement a deterministic transcript harness for the small Qwen2.5 model. Compare direct llama.cpp invocation and the QUORTEX launcher over a fixed bounded two-turn input, recording command lines, stdout, exit codes, timing, and backend performance lines. Do not accept the milestone unless the transcript semantics are explained and reproducible.

After acceptance, investigate persistent prompt/state reuse as the first actual inference-path optimization hypothesis.

