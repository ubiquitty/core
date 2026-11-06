#!/usr/bin/env bash
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0
#
# check-bench-compiles.sh — every benchmark translation unit still compiles.
#
# Tests/Performance is built by scripts, not by CMake, and each script names the
# benchmarks it wants — build-perf-bench.sh a few, run-bench.sh and pgo-train.sh
# bench_stack, run-parse-scenarios.sh one more. Nothing names the rest, so
# nothing compiles them, and a tree-wide edit breaks them with every gate green.
#
# Some of the unnamed ones are cited from production comments as the measurement
# that justifies the code's shape: LineStore.cpp points at bench_cellops, VT.cpp
# at bench_ingest_corpora. A citation to a file that no longer compiles still
# reads as authoritative.
#
# Syntax-only, so this costs a parse rather than a link and can sit in the
# per-commit gate. It proves the sources are still valid against the headers
# they include; it does not run them and makes no claim about their numbers.
#
# Under the same warning flags Tests/CMakeLists.txt builds the two benchmarks it
# owns. A parse that accepts what a real build rejects is worse than no parse:
# it reports the benchmarks compile, and then the build that matters fails on a
# warning this one never asked for.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

CXX="${CXX:-clang++}"
# Every compile goes through the launcher (ccache), or this syntax-only sweep runs
# uncached on every push — gate_style calls this script, so that cost is paid by
# each one. Unquoted, so an empty launcher contributes no argument.
launcher="${UBQ_COMPILER_LAUNCHER-}"

# A benchmark whose only dependency is libc is meant to be compiled by the bare
# command line in its own header comment. Passing the repo's include path anyway
# is harmless for those and required for the rest, so one recipe covers both.
#
# Taken from the same function run-bench.sh builds with, so this check cannot go
# on passing under an include path no runner has.
# shellcheck source=Scripts/portable-core-lib.sh
source "$ROOT/Scripts/portable-core-lib.sh"
INCLUDES=()
while IFS= read -r bench_include_flag; do
    INCLUDES+=("$bench_include_flag")
done < <(ubiquitty_bench_include_flags "$ROOT")
WARNINGS=(-Wall -Wextra -Werror -Wno-deprecated-declarations -Wno-multichar -Wno-comment)

# `mapfile` is bash 4; macOS ships 3.2, where it is not a syntax error but a
# missing command — the array stays unset and the script dies on the next line.
SOURCES=""
while IFS= read -r found; do
    SOURCES="$SOURCES $found"
done < <(git ls-files 'Tests/Performance/bench_*.cpp' | sort)

count=0
for source in $SOURCES; do
    count=$((count + 1))
done

# An empty list is the failure this check is most likely to have: a renamed
# directory or a glob that stops matching leaves it passing over nothing. The
# floor below is the count that has to keep holding, and the total is printed on
# success too, so a set that shrinks is visible rather than merely non-zero.
if [ "$count" -lt 8 ]; then
    echo "check-bench-compiles: found $count benchmark sources, expected at least 8" >&2
    echo "check-bench-compiles: the glob stopped matching, or benchmarks were removed" >&2
    for source in $SOURCES; do echo "  $source" >&2; done
    exit 1
fi

failed=0
for source in $SOURCES; do
    if output="$($launcher "$CXX" -std=c++23 -fsyntax-only "${WARNINGS[@]}" ${INCLUDES[@]+"${INCLUDES[@]}"} "$source" 2>&1)"; then
        continue
    fi
    echo "check-bench-compiles: $source does not compile" >&2
    echo "$output" | head -20 >&2
    failed=1
done

if [ "$failed" -ne 0 ]; then
    exit 1
fi

echo "check-bench-compiles: ok ($count benchmark sources compile)"
