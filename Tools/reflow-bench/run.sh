#!/usr/bin/env bash
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

# Build + run reflow_bench (and throughput_bench) against the portable Core.
# Prints the Tier-2 go/no-go measurement (reflow_bench by default).
#
# The Core link set is DERIVED from the one canonical list
# (Scripts/portable-core-sources.txt) via the shared archive helper — the same
# source CMake's ubiquitty-core target and run-bench.sh read — so it can never
# drift behind a file split (a hand-maintained list silently loses the TUs a
# split creates, and stops linking).
#
# Usage:
#   ./run.sh [reflow_bench args]        build both, run reflow_bench
#   BENCH=throughput ./run.sh [args]    build both, run throughput_bench
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT="$ROOT/BuildHostTests"
mkdir -p "$OUT"
CXX="${CXX:-c++}"

# shellcheck source=Scripts/portable-core-lib.sh
source "$ROOT/Scripts/portable-core-lib.sh"
# Flags must match the shipping app (-O3 + ThinLTO) — see run-bench.sh.
build_portable_core_archive "$OUT/reflow-core" "-O3 -flto=thin -DNDEBUG"
ubiquitty_core_pal_stub_source "$OUT/reflow-core/pal_stub.cpp"

build_one() {
    "$CXX" -std=c++23 -O3 -flto=thin -DNDEBUG -fno-exceptions -fno-rtti -pthread \
        -Wno-deprecated-declarations -I"$ROOT/Source" \
        "$ROOT/Tools/reflow-bench/$1.cpp" "$OUT/reflow-core/pal_stub.cpp" \
        "$PORTABLE_CORE_ARCHIVE" -o "$OUT/$1"
}
build_one reflow_bench
build_one throughput_bench

case "${BENCH:-reflow}" in
    throughput) "$OUT/throughput_bench" "$@" ;;
    *) "$OUT/reflow_bench" "$@" ;;
esac
