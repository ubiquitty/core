#!/usr/bin/env bash
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

# Build + run sgr_bench: a gprof profiling harness for Terminal::processBytes on
# the SGR-heavy scenarios (truecolor_text / sgr_churn / dense_truecolor /
# dense_sgr256), mirroring the byte patterns of the ubench generator
# (Scripts/run-parse-scenarios.sh). Not a
# correctness test; a profiling harness.
#
# The Core link set is DERIVED from the one canonical list
# (Scripts/portable-core-sources.txt) via the shared archive helper — the same
# source CMake's ubiquitty-core target and the sibling bench drivers read — so it
# can never drift behind a file split, as a hand-written g++ line does.
#
# Built with -pg; after the run it prints the gprof flat-profile head.
#
# Usage:
#   ./run.sh [scenario] [reps]        # default: truecolor_text 20
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT="$ROOT/BuildHostTests"
mkdir -p "$OUT"
CXX="${CXX:-c++}"

# shellcheck source=Scripts/portable-core-lib.sh
source "$ROOT/Scripts/portable-core-lib.sh"
# -O3 matches the shipping app's optimization level. ThinLTO stays OFF here
# alone: gprof attribution is this harness's whole purpose, and LTO folds the
# helpers it attributes into their callers (run-bench.sh is the
# shipping-flags throughput gate).
build_portable_core_archive "$OUT/sgr-core" "-O3 -DNDEBUG -pg"
ubiquitty_core_pal_stub_source "$OUT/sgr-core/pal_stub.cpp"

"$CXX" -std=c++23 -O3 -g -pg -DNDEBUG -fno-exceptions -fno-rtti -pthread \
    -Wno-deprecated-declarations -I"$ROOT/Source" \
    "$ROOT/Tools/sgr-profile/sgr_bench.cpp" "$OUT/sgr-core/pal_stub.cpp" \
    "$PORTABLE_CORE_ARCHIVE" -o "$OUT/sgr_bench"

# gprof writes gmon.out into the CWD of the profiled run.
cd "$OUT"
"$OUT/sgr_bench" "${1:-truecolor_text}" "${2:-20}"
if command -v gprof >/dev/null 2>&1; then
    gprof "$OUT/sgr_bench" "$OUT/gmon.out" | head -40
else
    echo "gprof not found (GNU binutils) — the -pg run wrote $OUT/gmon.out;" \
         "read it with 'gprof $OUT/sgr_bench $OUT/gmon.out', or profile on" \
         "macOS with Instruments/'sample' against $OUT/sgr_bench instead." >&2
fi
