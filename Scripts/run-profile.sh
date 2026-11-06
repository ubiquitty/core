#!/bin/bash
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

# run-profile.sh — build + run the portable latency profiler and emit one
# JSON object of per-operation distributions (p50/p90/p95/p99/max, µs/op) plus a
# human-readable table on stderr.
#
#   ./Scripts/run-profile.sh [out.json] [-- extra profile_stack args]
#
# Profiles the VT protocol parser, the Sync snapshot path, and the tmux Session
# deliver/render path. Pure CPU — no GUI, no PTY — so the distributions compare
# across platforms and across commits. Scripts/compare-profile.py diffs two.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# Build output lives outside the checkout; see Scripts/build-root.sh.
# shellcheck source=Scripts/build-root.sh
. "$ROOT/Scripts/build-root.sh"
OUT="${1:-$ROOT/profile-results.json}"
shift || true
[ "${1:-}" = "--" ] && shift
CXX="${CXX:-c++}"
# Launcher plus compiler, unquoted at the call so an absent launcher adds no
# argument. A bare "$CXX" would compile fine and skip ccache without saying so.
CXX_LAUNCHED="${UBQ_COMPILER_LAUNCHER:+$UBQ_COMPILER_LAUNCHER }$CXX"
BUILD="$UBQ_BUILD_ROOT/BuildBench"
mkdir -p "$BUILD"

PLATFORM="$(uname -s)-$(uname -m)"
COMMIT="$(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo unknown)"

# The portable parse / Sync-snapshot / tmux Session path. The link set is
# DERIVED from the one canonical libUbiquittyCore source list
# (Scripts/portable-core-sources.txt) — built into a static archive so only the
# TUs profile_stack references are pulled. Same single source CMake's
# `ubiquitty-core` target reads; no hand-maintained list to drift.
# shellcheck source=Scripts/portable-core-lib.sh
source "$ROOT/Scripts/portable-core-lib.sh"

# Flags must match the shipping app (-O3 + ThinLTO) — see run-bench.sh; a
# profile taken at -O2 attributes time to helpers LTO folds away in the
# shipping binary.
UBQ_LTO_FLAG="$(ubiquitty_lto_flag)"
echo "Building libUbiquittyCore archive + profile_stack ($CXX, -O3 $UBQ_LTO_FLAG) ..." >&2
build_portable_core_archive "$BUILD" "-O3 $UBQ_LTO_FLAG -DNDEBUG"
ubiquitty_core_pal_stub_source "$BUILD/profile_pal_stub.cpp"
$CXX_LAUNCHED -std=c++23 -O3 $UBQ_LTO_FLAG -DNDEBUG -fno-exceptions -fno-rtti -pthread \
    -I"$ROOT/Source" \
    "$ROOT/Tests/Performance/profile_stack.cpp" "$BUILD/profile_pal_stub.cpp" \
    "$PORTABLE_CORE_ARCHIVE" -o "$BUILD/profile_stack"

echo "Profiling (platform=$PLATFORM commit=$COMMIT) ..." >&2
"$BUILD/profile_stack" --platform "$PLATFORM" --commit "$COMMIT" "$@" > "$OUT"
echo "wrote $OUT" >&2
