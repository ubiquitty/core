#!/bin/bash
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

#
# check-coverage-floor.sh — exit non-zero if host coverage drops below
# documented per-file floors.
#
# Pre-req: ./Scripts/run-coverage.sh has already produced
# BuildHostTests/coverage/summary.txt. (We don't re-run the suite
# here; CI can chain `./Scripts/run-coverage.sh` and then
# `./Scripts/check-coverage-floor.sh`.)
#
# Floors below match the targets in COVERAGE_PLAN.md. Bumping a floor
# is a one-line change here; lowering one needs a justification.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# Build output lives outside the checkout; see Scripts/build-root.sh.
# shellcheck source=Scripts/build-root.sh
. "$ROOT/Scripts/build-root.sh"
SUMMARY="$UBQ_BUILD_ROOT/BuildHostTests/coverage/summary.txt"

if [ ! -f "$SUMMARY" ]; then
    echo "check-coverage-floor: no summary.txt at $SUMMARY" >&2
    echo "  run ./Scripts/run-coverage.sh first" >&2
    exit 2
fi

# Nothing here measures. The floors are compared against a file some earlier run
# left behind, and a summary older than the code it describes reports the
# coverage of a tree nobody has. A moved file is caught downstream because its
# path stops matching; a file edited in place is not caught at all — the numbers
# are someone else's, and they pass.
NEWER=$(find "$ROOT/Source" -name '*.cpp' -newer "$SUMMARY" -print -quit 2>/dev/null || true)
if [ -n "$NEWER" ]; then
    echo "check-coverage-floor: $SUMMARY is older than the sources it scores." >&2
    echo "  newer: ${NEWER#"$ROOT/"}" >&2
    echo "  Re-run ./Scripts/run-coverage.sh. Comparing floors against this file" >&2
    echo "  reports coverage for code that is not the code in the tree." >&2
    exit 2
fi

# Per-file line-coverage floors (percent). Each entry is
#   <filename-substring>  <floor>
# where filename-substring is matched against the leading column of
# the llvm-cov report. The comparison uses the integer part, so a
# floor of N passes at N.00%+.
#
# Floor justifications (lowering a floor needs one):
#   Core/Mouse/Mouse.cpp 93 (target 95): host suite measures 93.48%; the 6 missed
#     lines are SGR-extended-mode report arms only the live frontend
#     reaches. Floor = the measured value, integer-truncated: a floor
#     above what the suite can reach fails every run.
#   Unicode/Width.cpp 69 (target 100): host suite measures 69.66%. The
#     full-plane flat table is the primary path, and the binary-search
#     range lookup + the trie-overflow fallback behind it are defensive
#     code the suite does not sweep. Floor = measured.
#   Core/CellGrid.cpp 85 (target 90): measures 85.85%. The
#     writeASCIIRun/emitASCIIRun fast path carries degenerate-size clamps +
#     snapshot pen-diff arms the host suite doesn't exercise. Floor =
#     measured, truncated.
#   Image/Sixel/Decoder.cpp 73 (target 80): measures 73.79%. The HLS
#     out-of-range clamp arms + resolver paths are ones the host suite
#     doesn't sweep. Floor = measured, truncated.
#   Kitty/Graphics.cpp + Image/TerminalImage.cpp (target 75): the protocol
#     decoder and the model lifecycle. The model half is exercised by the
#     vt100 slot/scroll/animation tests; the protocol half holds the
#     lower-covered decode/transport arms. Floors set conservatively — retighten
#     against the next clang-coverage run (this host's GCC has no llvm-cov).
read -r -d '' FLOORS <<'EOF' || true
Core/VT/VT.cpp           80
Core/CellGrid.cpp         85
Core/Mouse/Mouse.cpp     93
Core/OSC/Dispatch.cpp    80
Core/SGR.cpp             95
Core/Terminfo.cpp 95
Kitty/Graphics.cpp 70
Image/TerminalImage.cpp      75
Codec/PNGDecoder.cpp 75
Codec/ZlibInflate.cpp 80
Image/Sixel/Decoder.cpp 73
Unicode/Width.cpp        69
PAL/TaskQueue.cpp   85
EOF

FAILED=0
PASS=0
# Every floor in the table, plus the weighted TOTAL, must reach a verdict. The
# counters alone cannot say so: a run where nothing is judged reports "0 passed,
# 0 failed" and exits 0, which reads as agreement.
EXPECTED=$(printf '%s\n' "$FLOORS" | grep -c '[^[:space:]]' || true)
EXPECTED=$((EXPECTED + 1))

# Parse the line-coverage column out of summary.txt. The llvm-cov
# report format is:
#   Filename  Regions ... Lines  MissedLines  Cover%(Lines)  Branches ...
# The line-coverage % is column 10 (counting from 1) of the data
# rows. awk -F gives us whitespace splits.

echo "Coverage floor check:"
echo

while read -r entry; do
    [ -z "$entry" ] && continue
    file=$(echo "$entry" | awk '{print $1}')
    floor=$(echo "$entry" | awk '{print $2}')
    row=$(grep -F "$file " "$SUMMARY" | head -1 || true)
    if [ -z "$row" ]; then
        # A floor with no row fails. Skipping it would let a file that moves
        # silently stop being floored: the entry stays, prints a `?`, and the
        # gate goes on passing.
        if [ ! -f "$ROOT/Source/$file" ]; then
            printf '  FAIL %-32s %s\n' "$file" "[floor names a file that is gone]"
        else
            printf '  FAIL %-32s %s\n' "$file" "[no coverage data — was it built?]"
        fi
        FAILED=$((FAILED + 1))
        continue
    fi
    # Column 10 in the data row is the line-cover %, formatted "NN.NN%".
    cover=$(echo "$row" | awk '{print $10}' | tr -d '%')
    # Blank or non-numeric. A column holding a word still compares — `[` reports
    # an error and returns non-zero, which lands in the same branch as "below
    # the floor" and reads as a coverage regression. The gate would be right by
    # accident and its message would send someone to write tests.
    case "$cover" in
        '' | *[!0-9.]* | *.*.*) cover='' ;;
    esac
    if [ -z "$cover" ]; then
        # A column this cannot read is a broken instrument, not a pass. llvm-cov
        # decides the layout, so one added column moves the cover percentage out
        # from under $10 for every row at once — and skipping each in turn
        # retires every floor in the table in one step, with the exit status
        # still 0.
        printf '  FAIL %-32s %s\n' "$file" "[could not read a cover column from: $row]"
        FAILED=$((FAILED + 1))
        continue
    fi
    cover_int=$(echo "$cover" | awk -F'.' '{print $1}')
    if [ "$cover_int" -ge "$floor" ]; then
        # A file over its floor is counted, not named: the closing tally says
        # how many were judged, and a reader acts on the ones under it.
        PASS=$((PASS + 1))
    else
        printf '  FAIL %-32s %6s%% <  %s%%\n' "$file" "$cover" "$floor"
        FAILED=$((FAILED + 1))
    fi
done <<< "$FLOORS"

# Weighted total floor. 80→77: measured 77.76% when set. Part of that drop was
# Application/* — the Classic (Mac OS 9) frontend — entering the total uncovered
# because the host suite never exercised it. That frontend is gone, so this
# floor is looser than the tree warrants. Re-measure with
# ./Scripts/run-coverage.sh and raise it; do not guess the number.
# `|| true` because `set -e` kills the script at this assignment when the
# summary has no TOTAL row — which is exactly the case the "could not read a
# cover column" branch below was written for. Without it that branch is
# unreachable, and a summary with no total ends the run part-way through the
# per-file list: no total, no tally, no verdict line, and an exit status a
# reader has to interpret without any of them.
total_row=$(grep '^TOTAL' "$SUMMARY" | head -1) || true
total_cover=$(echo "$total_row" | awk '{print $10}' | tr -d '%')
case "$total_cover" in
    '' | *[!0-9.]* | *.*.*) total_cover='' ;;
esac
total_floor=77
echo
if [ -z "$total_cover" ]; then
    printf '  FAIL %-32s %s\n' "TOTAL (weighted)" "[could not read a cover column]"
    FAILED=$((FAILED + 1))
else
    total_int=$(echo "$total_cover" | awk -F'.' '{print $1}')
    if [ "$total_int" -ge "$total_floor" ]; then
        printf '  ok  %-32s %6s%% >= %s%%\n' "TOTAL (weighted)" "$total_cover" "$total_floor"
        PASS=$((PASS + 1))
    else
        printf '  FAIL %-32s %6s%% <  %s%%\n' "TOTAL (weighted)" "$total_cover" "$total_floor"
        FAILED=$((FAILED + 1))
    fi
fi

echo
echo "$PASS passed, $FAILED failed, $EXPECTED floors to judge"
if [ "$((PASS + FAILED))" -ne "$EXPECTED" ]; then
    echo "check-coverage-floor: judged $((PASS + FAILED)) of $EXPECTED floors." >&2
    echo "  The ones it did not reach are unfloored, and a floor nobody checks is" >&2
    echo "  indistinguishable from a floor that holds." >&2
    exit 1
fi
if [ "$FAILED" -gt 0 ]; then
    echo "Coverage regressed. To raise a floor: edit FLOORS in this script." >&2
    exit 1
fi
exit 0
