#!/bin/bash
# Builds build-switch/SheepD3D.nro with devkitPro's Switch toolchain (switch/README.md).
set -e
cd "$(dirname "$0")/.."
: "${DEVKITPRO:=/opt/devkitpro}"
"$DEVKITPRO/portlibs/switch/bin/aarch64-none-elf-cmake" -S . -B build-switch -DCMAKE_BUILD_TYPE=Release "$@"
cmake --build build-switch -j"$(nproc)"
