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
```
