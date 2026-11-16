#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
SRC=/Users/user/workspace/cortex-build-src
BUILD=/Users/user/workspace/cortex-build-mac
mkdir -p native/build-mac
clang++ -arch arm64 -O2 -std=c++17 -Wall \
    native/macos_stream_decode.cpp -o native/build-mac/macos-stream-decode \
    -I "$SRC/include" -I "$SRC/ggml/include" -L "$BUILD/bin" \
    -lggml -lggml-base -lggml-cpu \
    -Wl,-rpath,"$BUILD/bin"
file native/build-mac/macos-stream-decode
