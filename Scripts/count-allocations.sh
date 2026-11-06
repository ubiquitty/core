#!/bin/bash
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

# Count every allocation a bench lane group makes, by DYLD interposition.
#
#   Scripts/count-allocations.sh                      # sweep the usual groups
#   Scripts/count-allocations.sh --groups delta,parse
#   Scripts/count-allocations.sh --binary /tmp/bench_base --groups delta
#
# Two binaries built from two commits, same groups, is the check that a change
# removed the allocations it was meant to remove. That is a claim about the
# mechanism; a bench delta alone is a claim about the clock, and the two fail
# independently, so a change is convincing when both move together.
#
# macOS only — __DATA,__interpose is a dyld feature. Docs/measuring-a-core-change.md
# is the procedure this belongs to; Scripts/profile-lane.sh is its time-side
# counterpart and takes the same --only group names.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# Build output lives outside the checkout; see Scripts/build-root.sh.
# shellcheck source=Scripts/build-root.sh
. "$ROOT/Scripts/build-root.sh"
BINARY="$UBQ_BUILD_ROOT/BuildBench/bench_stack"
# The eight groups that allocate enough to rank. Others exist (see the --only
# comment in bench_stack.cpp); pass --groups to name them.
LANE_GROUPS="render,parse,snapshot,delta,reflow,scrollback,tmuxdeliver,kittyozstream"
OUT="${TMPDIR:-/tmp}/ubq-alloc-$$"

while [ $# -gt 0 ]; do
    case "$1" in
    --binary) BINARY="$2"; shift 2 ;;
    --groups) LANE_GROUPS="$2"; shift 2 ;;
    -h | --help) sed -n '2,20p' "${BASH_SOURCE[0]}"; exit 0 ;;
    *) echo "count-allocations.sh: unknown argument $1" >&2; exit 2 ;;
    esac
done

if [ ! -x "$BINARY" ]; then
    echo "count-allocations.sh: no bench binary at $BINARY" >&2
    echo "  build one with: UBIQUITTY_PGO=OFF ./Scripts/build-perf-bench.sh bench_stack" >&2
    exit 1
fi

mkdir -p "$OUT"
trap 'rm -rf "$OUT"' EXIT

DYLIB="$UBQ_BUILD_ROOT/BuildBench/alloc-interpose.dylib"
SOURCE="$ROOT/Scripts/alloc-interpose.c"
if [ ! -f "$DYLIB" ] || [ "$SOURCE" -nt "$DYLIB" ]; then
    mkdir -p "$(dirname "$DYLIB")"
    cc -O2 -dynamiclib "$SOURCE" -o "$DYLIB"
fi

printf '%-16s %10s %10s %10s %14s\n' group malloc calloc realloc bytes
IFS=',' read -r -a group_list <<<"$LANE_GROUPS"
for group in "${group_list[@]}"; do
    DYLD_INSERT_LIBRARIES="$DYLIB" "$BINARY" --only "$group" \
        >"$OUT/$group.json" 2>"$OUT/$group.err" || true

    # A group name that matches no lane group still runs, still exits zero and
    # still reports its startup allocations — which reads as a real measurement
    # of a very cheap group. The JSON is the discriminator: --only leaves only
    # machine_control behind when it matched nothing.
    lanes=$(grep -c '"name":' "$OUT/$group.json" 2>/dev/null || echo 0)
    if [ "$lanes" -le 1 ]; then
        echo "count-allocations.sh: --only $group matched no lane group" >&2
        echo "  names come from the group function minus 'Benches'; grep 'wanted(' Tests/Performance/bench_stack.cpp" >&2
        exit 1
    fi

    line=$(grep '^ALLOC' "$OUT/$group.err" | tail -1)
    # Cleared before the eval, which assigns these by name out of the
    # interposer's own line. A group whose run wrote no ALLOC line leaves the
    # eval with nothing to do, and the printf below would then repeat the
    # preceding group's counts under this group's name — a wrong number shaped
    # exactly like a right one. Clearing turns that into the refusal underneath.
    # It also tells shellcheck where the four names come from (SC2154).
    malloc=""
    calloc=""
    realloc=""
    bytes=""
    eval "${line#ALLOC }"
    if [ -z "$malloc" ]; then
        echo "count-allocations.sh: $group produced no ALLOC line in $OUT/$group.err" >&2
        echo "  The interposer did not load, or the run died before it reported." >&2
        exit 1
    fi
    printf '%-16s %10s %10s %10s %14s\n' "$group" "$malloc" "$calloc" "$realloc" "$bytes"
done
