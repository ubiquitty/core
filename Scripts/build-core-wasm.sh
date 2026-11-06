#!/usr/bin/env bash
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

# build-core-wasm.sh — build portable Core's layer libraries to wasm32.
#
# wasm32 rejects shapes a 64-bit host accepts (a pointer-width assumption, a
# missing transitive include), so this arm catches Core and Base breaks no host
# lane does. The browser page that consumes the module is a frontend's, built
# by the repository that holds it; this proves the libraries it links compile.
#
# Skips gracefully (exit 0, logged) when emscripten is absent, as the other wasm
# scripts do.
#
#   ./Scripts/build-core-wasm.sh
set -euo pipefail

ROOT="$(git rev-parse --show-toplevel 2>/dev/null || (cd "$(dirname "$0")/.." && pwd))"
SCRIPTS="$(cd "$(dirname "$0")" && pwd)"
# Build output lives outside the checkout; see Scripts/build-root.sh.
# shellcheck source=Scripts/build-root.sh
. "$SCRIPTS/build-root.sh"
BUILD="$UBQ_BUILD_ROOT/core-wasm"

if ! command -v emcmake >/dev/null 2>&1; then
    echo "build-core-wasm: emscripten (emcmake) not found — skipping" >&2
    exit 0
fi

# CMake caches the absolute Emscripten toolchain path, and a package upgrade
# removes the versioned prefix it names. Drop only that stale generated state; a
# valid cache keeps its objects.
if [[ -f "$BUILD/CMakeCache.txt" ]]; then
    while IFS='=' read -r key value; do
        case "$key" in
            CMAKE_TOOLCHAIN_FILE:* | CMAKE_C_COMPILER:* | CMAKE_CXX_COMPILER:*)
                if [[ "$value" = /* && ! -e "$value" ]]; then
                    echo "build-core-wasm: discarding stale CMake toolchain cache ($value)" >&2
                    cmake -E remove -f "$BUILD/CMakeCache.txt"
                    cmake -E remove_directory "$BUILD/CMakeFiles"
                    break
                fi
                ;;
        esac
    done <"$BUILD/CMakeCache.txt"
fi

# QUIC is off because a wasm target has neither UDP sockets nor AWS-LC.
echo "build-core-wasm: configuring for wasm32 ..." >&2
emcmake cmake -S "$ROOT" -B "$BUILD" -G Ninja \
    -DUBIQUITTY_FRONTEND=Core -DUBIQUITTY_QUIC=OFF -DCMAKE_BUILD_TYPE=Release >/dev/null

echo "build-core-wasm: building ..." >&2
cmake --build "$BUILD" --target ubiquitty-core
echo "build-core-wasm: ok (ubiquitty-core compiles for wasm32)"
