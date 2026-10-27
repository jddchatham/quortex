# QIES-056 — Integrated Layer-0 Verification

Status: **PASS** for one token at position 0.

This milestone verified the complete QUORTEX path:

```text
Segment-A boundary
  -> router selection
  -> streamed selected experts
  -> streamed shared expert
  -> complete_moe accumulation
  -> isolated Segment-B HC combine
  -> post-Segment-B output
```

## Revisions and inputs

- QUORTEX revision at verification: `53b16b0967b56eea4e84fc69a6dcec8ccb729861`
- Cortex revision at verification: `8da466d955635180190945412d5560403e4ff062`
- Model: `Qwen3.8-Flash-Next-Uncensored-IQ4_XS-00001-of-00003.gguf` with automatic split loading
- Tensor index: `/home/user/bitnet-cortex/cortex-stream-forge/flashnext-tensor-index.json`
- Input token: `248045`
- Position: `0`
- Sequence: `0`
- Tensor-index SHA-256: `bc78052954dc3ab6d37ad189a2643c68cc8ad6ecfab5a6bb76361dc0b94f22c7`

## Evidence

Reference captures:

`/tmp/quortex-qies056-reference.JEYl5b`

QUORTEX captures:

`/tmp/quortex-qies056-quortex.VuCuU0`

All values were finite and every corresponding file was bitwise identical.

| Checkpoint | Elements | SHA-256 |
|---|---:|---|
| Segment-A boundary | 12,804 | `89881dc0dcd0316d108d209bb065e6aa41400bb9c71097442900cb2d4f50ddd7` |
| Router logits | 512 | `049afa8caca40ac647d2e341bcb280d33cd2657bfb7e429f706d5bcfdec9e904` |
| Selected MoE | 2,560 | `2ab9a1fd7b48f78e0e285741e39aaeafb9dcacd8bb1dca83910cd04648400d87` |
| Shared MoE | 2,560 | `4f813930b9e83b3b5260610bef3c787dd5e5514ee4ca9e88e4bf2dba8058247e` |
| Combined MoE | 2,560 | `1e692e7198c0d0dd9dff0f894128335eace3a72ddc9498f317c3023f33b266cf` |
| Post-Segment-B | 10,240 | `0ad745c2270960879b93a8d911745c3048db2a801bba69804a0335fd46132c1a` |

For every checkpoint: maximum absolute error, RMS error, and maximum relative
error were zero; bitwise equality was true. The post-Segment-B result was also
bitwise identical independently in each of its four 2,560-element streams.

## Runtime and safeguards

| Run | Runtime | Peak cgroup memory | Exit |
|---|---:|---:|---:|
| Cortex reference | 1.208 s | 398.3 MiB | 0 |
| QUORTEX | 3.061 s | 287.2 MiB | 0 |

Both runs used `MemoryMax=2G`, `MemorySwapMax=0`, `TasksMax=64`,
`RLIMIT_AS=1536 MiB`, and a 90-second timeout. Swap usage was zero and no OOM
or resource-limit event occurred.

## Reproduction outline

Build the version-matched native target:

```sh
cmake -S native -B native/debug
cmake --build native/debug --target quortex-flashnext-layer0 -j2
```

Run the reference and QUORTEX in fresh directories using the same model,
tensor index, token/state contract, low-residency flags, and transient cgroup
limits above. The QUORTEX run must set:

```sh
QUORTEX_FULL_LAYER0_STREAMED=1
FLASHNEXT_TENSOR_INDEX=/home/user/bitnet-cortex/cortex-stream-forge/flashnext-tensor-index.json
FLASHNEXT_COMPARE_DIR=/fresh/output/directory
```

The reference must set `CORTEX_REFERENCE_FULL_LAYER0_ONLY=1` and leave
`CORTEX_REFERENCE_POSITION0_ONLY` unset. Compare the six matching binary
artifacts offline after both runs complete.

## Required implementation boundary

The reproducible QUORTEX-side runtime files are:

- `native/flashnext_layer0.cpp`
- `native/quortex_streamed_moe.cpp`
- `native/quortex_streamed_moe.h`
- `native/CMakeLists.txt`

The matching Cortex-side runtime and diagnostic files are:

- `runtime-flashnext-layer-exp/examples/flashnext-owned-layer.cpp`
- `runtime-flashnext-layer-exp/src/flashnext-selected-moe.cpp`
- `runtime-flashnext-layer-exp/src/flashnext-shared-expert.cpp`
- `runtime-flashnext-layer-exp/src/flashnext-segment-b.cpp`
- `runtime-flashnext-layer-exp/src/models/qwen4exp.cpp`
- `runtime-flashnext-layer-exp/src/llama-context.cpp`
- `runtime-flashnext-layer-exp/include/llama.h`
- `runtime-flashnext-layer-exp/src/llama-context.h`

The selected/shared kernels are version-matched Cortex kernels linked into the
QUORTEX native executable. QUORTEX independently invokes them using its own
Segment-A arrays and tensor-index reads; it does not load the reference MoE
artifacts.

## Scope and limitations

This proves one deterministic position-0 integrated layer-0 computation. It
does not prove position-1 state continuity, multi-token generation, complete
48-layer execution, final logits, multi-turn behavior, or acceleration.

Generated binaries, temporary capture directories, autonomy state, model files,
and unrelated dirty-tree changes are excluded from the proposed milestone.

## Proposed commit boundaries

1. Native streamed-MoE integration and isolated Segment-B wiring.
2. Cortex diagnostic/reference capture additions.
3. Offline comparison and synthetic regression tests.
4. This reproducibility document.

Before committing, review the dirty Cortex checkout and exclude unrelated
experimental files and generated build directories.

## Next risk

The next correctness milestone is position-1 execution with recurrent-state
handoff. It requires a separate capture contract for state before and after
position 0, identical position-1 inputs, and the same bounded execution
safeguards.
