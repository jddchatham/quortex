#!/bin/sh
# Build the minimal ARM64 smoke target with the host toolchain.
# No CMake, no external source trees, no downloaded dependencies.
set -eu
cd "$(dirname "$0")"
mkdir -p build
clang++ -arch arm64 -O2 -std=c++17 -Wall -Wextra \
    macos_arm64_smoke.cpp -o build/macos-arm64-smoke
file build/macos-arm64-smoke
