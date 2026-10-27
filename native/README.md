# Native FlashNext runtime foundation

This target deliberately links the version-matched GGML source used by Cortex
without copying or modifying it. It currently exercises metadata loading only;
it does not claim layer execution or reproduce the tensor-allocation assertion.

Build:

```sh
cmake -S native -B native/build \
  -DCORTEX_GGML_SOURCE=/home/user/bitnet-cortex/cortex-stream-forge/runtime-flashnext-stock-oracle-exp/runtime-src/ggml \
  -DGGML_CPU=ON -DBUILD_SHARED_LIBS=OFF
cmake --build native/build --target quortex-gguf-diagnostic -j2
native/build/quortex-gguf-diagnostic /path/to/model.gguf

Allocation regression:

```sh
native/build/quortex-tensor-allocation
```

This exercises one normal context-owned allocation. It intentionally does not
call the allocator twice on the same tensor; the fatal assertion is therefore
not reproduced by this safe regression until a matching graph lifecycle is
available.

Real-weight operation:

```sh
cmake --build native/build --target quortex-flashnext-op -j2
native/build/quortex-flashnext-op /path/to/model.gguf
```

This is a bounded, real-weight GGML matrix-vector operation. It is a runtime
foundation milestone, not evidence of a complete FlashNext layer.

Optional Cortex llama adapter:

```sh
cmake -S native -B native/build \
  -DCORTEX_GGML_SOURCE=/home/user/bitnet-cortex/cortex-stream-forge/runtime-flashnext-stock-oracle-exp/runtime-src/ggml \
  -DCORTEX_LLAMA_BUILD=/home/user/bitnet-cortex/cortex-stream-forge/runtime-flashnext-layer-exp-build
cmake --build native/build --target quortex-flashnext-layer0 -j2
```

Memory safety gate (preparation only): `QUORTEX_MAX_AS_MB` controls the
`RLIMIT_AS` virtual-address-space ceiling; it is not an RSS limit. Physical
memory must be enforced by cgroup v2. The user service already declares
`MemoryMax=2G`; an explicit one-shot run should use:

```sh
systemd-run --user --wait --collect \
  -p MemoryMax=2G -p MemorySwapMax=0 -p TasksMax=64 \
  env QUORTEX_MAX_AS_MB=1536 \
  CORTEX_PREAD_ARENA=1 CORTEX_LOW_RESIDENCY_LAYER=0 \
  CORTEX_FLASHNEXT_LAYER_START=0 CORTEX_FLASHNEXT_LAYER_END=1 \
  CORTEX_FLASHNEXT_LAYER_RANGE=1 \
  native/build/quortex-flashnext-layer0 /path/to/model.gguf
```

Do not run this command until the loader path has been statically reviewed.
```
