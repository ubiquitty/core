#!/bin/bash
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

# Build the canonical bench streams from a ubench checkout (UBENCH_DIR), then time
# their parser/model cost through the portable Core. The Rust generator is the
# sole scenario owner; this script works unchanged on macOS and Linux.
#
# The bench tool lives in a separate repo so the harness stays version-stable —
# the newest bench can measure old ubiquitty builds without touching this tree.
#
# UBENCH_DIR names the checkout; the lane does not run without it.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# Build output lives outside the checkout; see Scripts/build-root.sh.
# shellcheck source=Scripts/build-root.sh
. "$ROOT/Scripts/build-root.sh"
OUT="${1:-$ROOT/parse-scenario-results.json}"
CXX="${CXX:-c++}"
# Launcher plus compiler, unquoted at the call so an absent launcher adds no
# argument. A bare "$CXX" would compile fine and skip ccache without saying so.
CXX_LAUNCHED="${UBQ_COMPILER_LAUNCHER:+$UBQ_COMPILER_LAUNCHER }$CXX"
BUILD="$UBQ_BUILD_ROOT/BuildBench"
STREAMS="$BUILD/scenario-streams"
UBENCH_DIR="${UBENCH_DIR:-}"
BENCH_DIR="$UBENCH_DIR/bench"
PLATFORM="$(uname -s)-$(uname -m)"
COMMIT="$(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo unknown)"
COLUMNS="${BENCH_COLUMNS:-196}"
ROWS="${BENCH_ROWS:-52}"
SCALE="${BENCH_SCALE:-0.05}"
ITERS="${BENCH_ITERS:-5}"

mkdir -p "$BUILD" "$STREAMS"

# The workload generator is the bench/ subdirectory of the ubench checkout.
if [ -z "$UBENCH_DIR" ] || [ ! -f "$BENCH_DIR/Cargo.toml" ]; then
    echo "error: no ubench checkout at UBENCH_DIR (${UBENCH_DIR:-unset})" >&2
    echo "  This optional lane reads its workloads from a ubench checkout;" >&2
    echo "  no build, test or contribution check requires it. Set UBENCH_DIR:" >&2
    echo "    UBENCH_DIR=/path/to/ubench $0 $*" >&2
    exit 1
fi

( cd "$BENCH_DIR" && cargo build --release )
"$BENCH_DIR/target/release/bench" \
    --columns "$COLUMNS" --rows "$ROWS" --scale "$SCALE" \
    --dump-streams-only "$STREAMS"

# shellcheck source=Scripts/portable-core-lib.sh
source "$ROOT/Scripts/portable-core-lib.sh"
# Flags must match the shipping app (-O3 + LTO) — see run-bench.sh.
UBQ_LTO_FLAG="$(ubiquitty_lto_flag)"
build_portable_core_archive "$BUILD" "-O3 $UBQ_LTO_FLAG -DNDEBUG"
ubiquitty_core_pal_stub_source "$BUILD/bench_pal_stub.cpp"
$CXX_LAUNCHED -std=c++23 -O3 $UBQ_LTO_FLAG -DNDEBUG -fno-exceptions -fno-rtti -pthread \
    -I"$ROOT/Source" \
    "$ROOT/Tests/Performance/bench_parse_scenarios.cpp" "$BUILD/bench_pal_stub.cpp" \
    "$PORTABLE_CORE_ARCHIVE" -o "$BUILD/bench_parse_scenarios"

"$BUILD/bench_parse_scenarios" --stream-dir "$STREAMS" \
    --platform "$PLATFORM" --commit "$COMMIT" --columns "$COLUMNS" --rows "$ROWS" \
    --iters "$ITERS" | tee "$OUT"
