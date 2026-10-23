# Performance record

## Direct Qwen2.5 baseline

See `QUORTEX_RUNTIME_BASELINE.md`. One bounded run measured 8.822 s wall time for a 16-token request; llama.cpp reported 6.93 generated tokens/s, 1.36 s prompt evaluation, 1.10 GiB compute buffer, and 1.15 GiB KV buffer.

## Gate result

The gate avoids backend work only when the context rejects the candidate. This is not an accepted-inference acceleration result. No 100× claim exists.

The deterministic single-turn comparison measured 4.012 s direct versus 4.108 s through the QUORTEX launcher. This is a one-run wrapper-overhead observation, not a statistically established regression or improvement.
