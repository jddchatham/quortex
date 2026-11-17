# Reproduce: FlashNext first token (M-MAC-20 / PUNDY-003)

Target: `~/Downloads/Qwen3.8-Flash-Next-Uncensored.IQ4_XS.gguf`
Verified SHA-256: `a1975fd73542230e2b344d112f29f22ff522d051cb4bf33bdb872cb3ffc5dedf`
Host: Apple M4 Mac Mini, 16 GB unified memory. Oracle: pinned Cortex llama
build `~/workspace/cortex-build-mac/bin/libllama.0.5.0.dylib` (ggml 0.25.3).

## 1. Pre-flight gates (all must hold)

```sh
sysctl hw.memsize                       # 17179869184
memory_pressure | tail -1               # expect free % >= 50
sysctl vm.swapusage                     # expect used near 0
df -h /System/Volumes/Data              # expect >= 5 GiB free
shasum -a 256 ~/Downloads/Qwen3.8-Flash-Next-Uncensored.IQ4_XS.gguf
# must print a1975fd73542230e2b344d112f29f22ff522d051cb4bf33bdb872cb3ffc5dedf
```

## 2. Build the harness (one time)

```sh
cd ~/workspace/quortex-macos
SRC=/Users/user/workspace/cortex-build-src
BUILD=/Users/user/workspace/cortex-build-mac
clang++ -arch arm64 -O2 -std=c++17 -Wall \
  native/macos_llama_probe_nr.cpp -o native/build-mac/macos-llama-probe-nr \
  -I "$SRC/include" -I "$SRC/ggml/include" \
  "$BUILD/bin/libllama.dylib" "$BUILD/bin/libggml.dylib" \
  "$BUILD/bin/libggml-base.dylib" "$BUILD/bin/libggml-cpu.dylib" \
  -Wl,-rpath,"$BUILD/bin"
```

Do NOT use the stock `macos-llama-probe` for this model: its repack phase is
kernel-killed during load (memory), and `embeddings=true` aborts in the
qwen4exp graph. The `-nr` harness disables both (see its header comment).

## 3. Run (single token, bounded)

```sh
/usr/bin/time -l env PROBE_DUMP_LOGITS="$PWD/logits0.f32" \
  ./native/build-mac/macos-llama-probe-nr \
  ~/Downloads/Qwen3.8-Flash-Next-Uncensored.IQ4_XS.gguf 1
```

Kill limits used (enforced by an external watchdog in the original run):
probe RSS > 10 GB, system memory free < 10%, swap used > 1600 MB, runtime > 900 s.

## 4. Expected output (must reproduce bit-for-bit)

```text
PROMPT_TOKENS=5
BOS_ID=248044
N_VOCAB=248320
LOGITS0_SHA256=e8effaaba61fb2cf42e3fc05c615e749b5205143d7f7047bad6202c7e70f7650
LOGITS0_FIRST4=6.33340311 4.54787254 2.25123787 2.31205535
GEN0_ID=11751
GEN0_PIECE= Paris
PROBE_PASS=1
```

Measured envelopes: cold page cache ~12 s wall, peak RSS ~7.3 GB; warm ~3 s.
`logits0.f32` (993280 bytes) must hash to the LOGITS0_SHA256 value above.
