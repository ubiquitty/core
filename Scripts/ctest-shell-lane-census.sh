#!/usr/bin/env bash
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

#
# ctest-shell-lane-census.sh — says how much of the suite an invocation of
# ctest left out.
#
# The shell stanzas register as shell_<name>, and `ctest -E '^shell_'` is the
# command this repository's people and agents reach for. It runs the C++ unit
# binaries and none of the stanzas, so "100% tests passed out of 516" is a true
# sentence about a fraction of the suite, and reads as a sentence about all of
# it. A confinement change once shipped alongside that line with five checks red
# in message_gate_matrix.
#
# CTest runs this through CTEST_CUSTOM_POST_TEST after every invocation,
# filtered or not, and prints whatever it writes below the summary. It says
# nothing when the whole lane ran, so the line appears exactly where the claim
# would otherwise be larger than the run.
#
# CTEST_CUSTOM_POST_TEST is a list of commands, not a command and its arguments,
# so the count arrives through the file the test build writes rather than an
# argument.
#
# What ran comes from the log CTest is still writing. LastTest.log is renamed
# into place after the post-test commands, so at this point it still holds the
# invocation before this one; the LastTest.log.tmp<suffix> beside it is the one
# being written now. Where that file is not found, this says nothing, because a
# census drawn from the wrong run is worse than no census.

set -uo pipefail

# CTest runs a post-test command from the build directory.
build="${PWD}"
stanzas="$build/shell-lane-stanzas.txt"

[ -r "$stanzas" ] || exit 0
registered="$(wc -l < "$stanzas" | tr -d ' ')"
[ "$registered" -gt 0 ] || exit 0

log=""
for candidate in "$build"/Testing/Temporary/LastTest.log.tmp*; do
    [ -r "$candidate" ] || continue
    if [ -z "$log" ] || [ "$candidate" -nt "$log" ]; then
        log="$candidate"
    fi
done
[ -n "$log" ] || exit 0

# One "N/M Test: <name>" line per test this invocation ran.
ran="$(grep -cE '^[0-9]+/[0-9]+ Test: shell_' "$log" || true)"
[ "$ran" -lt "$registered" ] || exit 0

echo "SHELL-LANE: $ran of $registered shell stanzas ran here — the rest of the suite is untouched by this result."
echo "  They hold the daemon, the gates, the transfers and the projections; a green"
echo "  line above covers the C++ unit binaries and says nothing about them."
echo "  ctest -R '^shell_' runs the lane; ./Scripts/run-tests.sh <stanza> runs one."
exit 0
