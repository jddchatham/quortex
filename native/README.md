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
```
