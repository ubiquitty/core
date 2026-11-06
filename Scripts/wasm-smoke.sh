#!/usr/bin/env bash
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

#
# wasm-smoke.sh — proves libUbiquittyCore compiles to wasm32 and runs.
#
# Configures the standalone Core with the emscripten toolchain (which swaps the
# POSIX system-services backend for Source/Platform/Wasm/SystemPAL), builds
# the core-consumer sample, runs it under node, and asserts the expected
# snapshot output. Exits non-zero if the Core stops being wasm-clean — the
# regression signal that something POSIX-bound leaked above the PAL seam.
#
# Skips gracefully (exit 0, logged) when the emscripten toolchain or node is
# absent, so the broader battery stays green on machines without them.
#
#   ./Scripts/wasm-smoke.sh
#
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# Build output lives outside the checkout; see Scripts/build-root.sh.
# shellcheck source=Scripts/build-root.sh
. "$ROOT/Scripts/build-root.sh"
BUILD="$UBQ_BUILD_ROOT/wasm"

if ! command -v emcmake >/dev/null 2>&1; then
    echo "wasm-smoke: emscripten (emcmake) not found — skipping" >&2
    exit 0
fi
if ! command -v node >/dev/null 2>&1; then
    echo "wasm-smoke: node not found — skipping" >&2
    exit 0
fi

# A build tree that outlives the run remembers the toolchain file it was
# configured with, twice: in its cache, and in the system description its
# compiler probes include. cmake cannot reconfigure it for another, and a
# reconfigure that fails rewrites the cache entry and leaves the description
# naming the old one — so both must name this run's toolchain for the tree to
# be kept. A tree that names a toolchain file that no longer exists fails
# both checks. Any other tree starts over, and says so.
toolchain="$(dirname "$(command -v emcc)")/cmake/Modules/Platform/Emscripten.cmake"
if [ -d "$BUILD" ]; then
    kept=1
    grep -qsF "$toolchain" "$BUILD/CMakeCache.txt" || kept=0
    grep -qsF "$toolchain" "$BUILD"/CMakeFiles/*/CMakeSystem.cmake || kept=0
    if [ "$kept" = 0 ]; then
        echo "wasm-smoke: the kept build tree was configured with another toolchain — starting over" >&2
        rm -rf "$BUILD"
    fi
fi

echo "wasm-smoke: configuring the Core for wasm32…"
# QUIC is on everywhere a build can carry it; wasm32 is the exception the
# platform makes, not a choice: the carrier is ngtcp2 over UDP sockets with
# AWS-LC underneath, and a wasm target has neither. A browser reaches a host
# through the page's socket, which is the web frontend's own transport.
emcmake cmake -S "$ROOT" -B "$BUILD" \
    -DUBIQUITTY_FRONTEND=Core -DUBIQUITTY_QUIC=OFF >/dev/null

echo "wasm-smoke: building core-consumer…"
cmake --build "$BUILD" --target core-consumer >/dev/null

echo "wasm-smoke: running under node…"
OUTPUT="$(node "$BUILD/core-consumer.js")"
echo "$OUTPUT"

if ! grep -q "Hello from libUbiquittyCore!" <<<"$OUTPUT"; then
    echo "wasm-smoke: FAIL — expected snapshot row missing from wasm output" >&2
    exit 1
fi

echo "wasm-smoke: PASS — libUbiquittyCore compiles to wasm32 and runs"
