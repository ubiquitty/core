#!/usr/bin/env bash
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

# bench-one-binary-ab.sh — interleaved A/B of two code paths inside ONE binary,
# selected by an environment variable the change reads once.
#
#   bench-one-binary-ab.sh PAIRS ENV_SELECTING_OLD_PATH BENCH_BINARY [-- ARGS...]
#   bench-one-binary-ab.sh 11 UBQ_OLD_PATH BuildBench/bench_core \
#       -- --file corpus.vte --rows 24 --columns 80 --iters 20
#
# Why one binary. Two separately linked binaries differ everywhere, not only
# where the source differs: every function lands at a new address, and code
# placement alone can move a lane the change cannot reach — see
# Docs/measuring-a-core-change.md, "Compare with controls". Holding the binary
# fixed makes the executed path the only variable.
#
# The change under test therefore has to be selectable at run time:
#
#     // Read once. A getenv inside the loop measures the instrumentation.
#     static const bool takesNewPath = std::getenv("UBQ_OLD_PATH") == nullptr;
#
# That rules the method out for anything the linker decides — a type width, a
# struct layout, an inlining choice. Those stay cross-binary, and are only
# resolvable when the effect clears the layout noise.
#
# Carry a control. A lane the change cannot reach must read unchanged; when it
# does not, the run is measuring something else and there is no result to report.

set -u

if [ "$#" -lt 3 ]; then
    echo "usage: bench-one-binary-ab.sh PAIRS ENV_SELECTING_OLD_PATH BENCH_BINARY [-- ARGS...]" >&2
    exit 2
fi

pairs="$1"
selector="$2"
binary="$3"
shift 3
[ "${1:-}" = "--" ] && shift

[ -x "$binary" ] || { echo "bench-one-binary-ab: not executable: $binary" >&2; exit 2; }

# Another job on the machine moves both arms, but not equally: a build or a test
# suite alongside the run can invert a ratio.
loadAverage=$(uptime | sed 's/.*load averages*: *//' | awk '{print $1}')
if [ "$(python3 -c "print(1 if float('$loadAverage') > ${UBQ_AB_MAX_LOAD:-4.0} else 0)")" = 1 ]; then
    echo "bench-one-binary-ab: load average $loadAverage is above the gate; wait for the machine" >&2
    exit 3
fi

samples=$(mktemp)
trap 'rm -f "$samples"' EXIT

for index in $(seq 1 "$pairs"); do
    # Lead-swapped: a machine drifting under the battery moves both arms equally.
    if [ $((index % 2)) -eq 1 ]; then
        old=$(env "$selector=1" "$binary" "$@" 2>/dev/null | grep -oE '\(([0-9.]+) MiB/s best\)' | grep -oE '[0-9.]+')
        new=$("$binary" "$@" 2>/dev/null | grep -oE '\(([0-9.]+) MiB/s best\)' | grep -oE '[0-9.]+')
    else
        new=$("$binary" "$@" 2>/dev/null | grep -oE '\(([0-9.]+) MiB/s best\)' | grep -oE '[0-9.]+')
        old=$(env "$selector=1" "$binary" "$@" 2>/dev/null | grep -oE '\(([0-9.]+) MiB/s best\)' | grep -oE '[0-9.]+')
    fi
    if [ -z "$old" ] || [ -z "$new" ]; then
        echo "bench-one-binary-ab: a run produced no throughput line; check the binary and its arguments" >&2
        exit 4
    fi
    printf '%s\t%s\n' "$old" "$new" >> "$samples"
done

python3 - "$samples" <<'PY'
import statistics
import sys

old, new = [], []
for line in open(sys.argv[1]):
    left, right = line.split()
    old.append(float(left))
    new.append(float(right))

medianOld, medianNew = statistics.median(old), statistics.median(new)
wins = sum(1 for a, b in zip(old, new) if b > a)
print(f"old {medianOld:9.1f}   new {medianNew:9.1f}   new/old {100 * (medianNew / medianOld - 1):+7.2f}%   new wins {wins}/{len(old)}")
print("a lane the change cannot reach must read about zero; if it does not, this is not a result")
PY
