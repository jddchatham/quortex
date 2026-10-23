# QUORTEX runtime baseline

Date: 2026-10-22

## Hardware and model

- Target hardware: AMD Athlon Silver 3050U
- Model: `/home/user/models/qwen/Qwen2.5-Coder-3B-Instruct-abliterated-Q4_K_M.gguf`
- File size: 1,929,903,488 bytes
- Runtime: existing Cortex/llama.cpp build `/home/user/bitnet-cortex/phase8-build/bin/llama-cli`
- Threads: 2; GPU layers: 0

## Direct baseline

Command shape:

```text
llama-cli -m MODEL -p "Write one short sentence about quadrants." -n 16 -t 2 -ngl 0 --no-display-prompt --no-warmup
```

Observed output began:

```text
Quadrants are four regions formed by two perpendicular lines that intersect at the origin,
```

Measured wall time was 8.822 seconds. Runtime diagnostics reported:

- prompt evaluation: 1,356.86 ms for 8 tokens / 5.90 tokens/s
- generation: 2,163.85 ms for 15 tokens / 6.93 tokens/s
- model load: 6,438.92 ms
- CPU compute buffer: 1,104.01 MiB
- CPU KV buffer: 1,152.00 MiB

This is the direct reference, not a QUORTEX optimization result.

## QUORTEX gate

The `quortex.cli` launcher performs contextual officiation before invoking the backend. A satisfied context returns quadrant `1/1` and launches the unchanged backend; an unsatisfied context returns exit code `3` and does not load the GGUF. `--json-decision` inspects either result without launching the backend.

The gate has therefore demonstrated avoided backend invocation, not reduced work for an accepted generation. No speedup claim is made.

## First integrated run

An admitted run launched the real backend and produced interactive text under a bounded timeout. Its chat-template/input behavior did not match the simple direct-prompt baseline cleanly, so multi-turn correctness is **not certified**. The next test must use a deterministic prompt/input transcript and compare wrapper versus direct backend output before any optimization is accepted.

