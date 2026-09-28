#!/usr/bin/env bash
# Build voltage.so. Usage: ./scripts/build.sh
set -euo pipefail
cd "$(dirname "$0")/.."
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j"$(nproc)"
ls -la build/libvoltage.so
