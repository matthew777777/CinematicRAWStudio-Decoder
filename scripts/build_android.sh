#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Cross-compiles Release arm64-v8a binaries with the newest installed NDK.
#   scripts/build_android.sh [build-dir]     (default: build-android-arm64)
set -euo pipefail
cd "$(dirname "$0")/.."
SDK="${ANDROID_HOME:-$HOME/Library/Android/sdk}"
NDK="${ANDROID_NDK:-$(ls -d "$SDK"/ndk/* | sort -V | tail -1)}"
OUT="${1:-build-android-arm64}"
cmake -S . -B "$OUT" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 -DANDROID_STL=c++_static \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON \
    -DCMAKE_CXX_FLAGS_RELEASE="-O3 -DNDEBUG"
cmake --build "$OUT"
echo "built with $(basename "$NDK") -> $OUT"
