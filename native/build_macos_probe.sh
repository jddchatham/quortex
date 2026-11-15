#!/bin/sh
# Build the deterministic llama probe against the pinned Cortex build.
# Requires the cortex llama library from M-MAC-02 (see M_MAC_02_REPORT.md).
set -eu
cd "$(dirname "$0")/.."
SRC=/Users/user/workspace/cortex-build-src
BUILD=/Users/user/workspace/cortex-build-mac
mkdir -p native/build-mac
clang++ -arch arm64 -O2 -std=c++17 -Wall \
    native/macos_llama_probe.cpp -o native/build-mac/macos-llama-probe \
    -I "$SRC/include" -I "$SRC/ggml/include" -L "$BUILD/bin" \
    -lllama -lggml -lggml-base -lggml-cpu \
    -Wl,-rpath,"$BUILD/bin"
file native/build-mac/macos-llama-probe
