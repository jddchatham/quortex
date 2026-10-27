# QIES-041 — Real FlashNext Segment-A Verification

Status: **PASS** (2026-10-26)

## Verified result

QUORTEX and the trusted Cortex reference produced bitwise-identical real-weight
outputs for token `248045` at position `0`.

| Component | Elements | Shape/layout | Max abs | RMS | Max rel | Bitwise |
|---|---:|---|---:|---:|---:|---|
| Pre-MoE activation | 2,560 | `[2560,1]`, F32 | 0 | 0 | 0 | yes |
| Recurrent residual | 10,240 | `[2560,4,1]`, F32 | 0 | 0 | 0 | yes |
| Injection | 4 | `[4,1]`, F32 | 0 | 0 | 0 | yes |
| Router logits | 512 | `[512,1]`, F32 | 0 | 0 | 0 | yes |

The three-component Segment-A boundary contains 12,804 F32 values. Router
logits are captured separately.

## Capture evidence

Validated reference directory:

`/tmp/quortex-qies039.jD23Yv/reference`

Validated QUORTEX directory:

`/tmp/quortex-qies041.dIeWuI/quortex`

SHA-256 hashes:

| Artifact | Reference | QUORTEX |
|---|---|---|
| Segment-A, 51,216 bytes | `89881dc0dcd0316d108d209bb065e6aa41400bb9c71097442900cb2d4f50ddd7` | identical |
| Router, 2,048 bytes | `049afa8caca40ac647d2e341bcb280d33cd2657bfb7e429f706d5bcfdec9e904` | identical |

The comparison manifest is preserved at
`/tmp/quortex-qies041.dIeWuI/comparison.json`.

## Reproduction identity

- QUORTEX repository revision: `53b16b0967b56eea4e84fc69a6dcec8ccb729861`
- Cortex source revision: `8da466d955635180190945412d5560403e4ff062`
- Model: Qwen3.8 Flash Next Uncensored IQ4_XS, three GGUF shards recorded in
  `flashnext-sha256.txt`
- Tensor index: `/home/user/bitnet-cortex/cortex-stream-forge/flashnext-tensor-index.json`
- Input: token `248045`, position `0`, sequence `0`, one token
- QUORTEX flags: `CORTEX_PREAD_ARENA=1`, `CORTEX_LOW_RESIDENCY_LAYER=0`,
  `CORTEX_FLASHNEXT_LAYER_START=0`, `CORTEX_FLASHNEXT_LAYER_END=1`,
  `CORTEX_FLASHNEXT_LAYER_RANGE=1`, `CORTEX_FLASHNEXT_PRE_MOE=1`,
  `CORTEX_EXTERNAL_GLOBAL_TENSORS=1`
- QUORTEX runtime: 1.001 seconds; peak cgroup memory: 283.8 MiB
- Safeguards: cgroup v2 `MemoryMax=2G`, `MemorySwapMax=0`, `TasksMax=64`,
  `RLIMIT_AS=1536 MiB`, 90-second timeout

## Required milestone changes

QUORTEX-side files required for this milestone are:

- `native/flashnext_layer0.cpp` — streamed embedding input, memory guard, and
  atomic capture output.
- `tools/compare_captures.py` — offline validation and comparison.
- `tests/test_capture_compare.py` — synthetic capture regression.
- `native/README.md` — bounded execution documentation.

The version-matched Cortex working tree contains the corresponding runtime and
reference changes in `runtime-flashnext-layer-exp/include/llama.h`,
`src/llama-context.h`, `src/llama-context.cpp`, and
`examples/flashnext-owned-layer.cpp`. It is a separate dirty checkout and must
be preserved as an explicitly reviewed patch; unrelated Cortex changes and
generated builds are excluded.

## Scope and limitations

This proves real-weight, bitwise-equivalent Segment-A boundary and router
outputs for one position. It does not prove post-Segment-B equivalence, full
48-layer execution, final logits, coherent generation, multi-turn behavior, or
any acceleration benefit.

## Next numerical acceptance test

The next test will compare the post-Segment-B output, exactly 10,240 F32 values
with shape `[2560,4,1]`, using the same token, state, model shards, cgroup
limits, and deterministic capture protocol. Both implementations must export
the tensor atomically, validate finite values and byte length, then use the
offline comparator with component-specific hashes and absolute/RMS/relative
error. No result will be accepted from a shape-only or synthetic comparison.
