#!/bin/bash
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CENSUS="$ROOT/Scripts/ctest-shell-lane-census.sh"
FIXTURE="$(mktemp -d "${TMPDIR:-/tmp}/ubiquitty-shell-lane-census.XXXXXX")"
trap 'rm -rf "$FIXTURE"' EXIT

# The census reads the build directory it is run from, the way CTest runs a
# post-test command, so the fixture stands in for one.
mkdir -p "$FIXTURE/Testing/Temporary"
printf '%s\n' token_auth consent_gate message_gate_matrix > "$FIXTURE/shell-lane-stanzas.txt"

# CTest names the log it is still writing with a suffix and renames it into
# place afterwards, so the census reads the suffixed one.
inProgressLog="$FIXTURE/Testing/Temporary/LastTest.log.tmp1a2b3"

runCensus() {
    ( cd "$FIXTURE" && "$CENSUS" ) > "$FIXTURE/out" 2>&1
}

# A run that took the whole lane says nothing: the summary above it already
# covers everything there is.
{
    echo "1/3 Test: shell_token_auth"
    echo "2/3 Test: shell_consent_gate"
    echo "3/3 Test: shell_message_gate_matrix"
} > "$inProgressLog"
runCensus
if [ -s "$FIXTURE/out" ]; then
    echo "FAIL: the census spoke over a run that took the whole lane:" >&2
    sed 's/^/    /' "$FIXTURE/out" >&2
    exit 1
fi

# A run that took part of it names both numbers. One stanza short is the case a
# threshold written as "none ran" would miss.
{
    echo "1/2 Test: unit_something"
    echo "2/2 Test: shell_token_auth"
} > "$inProgressLog"
runCensus
if ! grep -qF "1 of 3 shell stanzas ran here" "$FIXTURE/out"; then
    echo "FAIL: the census did not count the stanzas this run left out:" >&2
    sed 's/^/    /' "$FIXTURE/out" >&2
    exit 1
fi

# The unit lane alone, which is the invocation the census exists for.
echo "1/1 Test: unit_something" > "$inProgressLog"
runCensus
if ! grep -qF "0 of 3 shell stanzas ran here" "$FIXTURE/out"; then
    echo "FAIL: the census did not speak over a run that took none of the lane:" >&2
    sed 's/^/    /' "$FIXTURE/out" >&2
    exit 1
fi

# No log for this invocation means no answer about it. A census drawn from the
# run before would be a claim about a run nobody made.
rm -f "$inProgressLog"
runCensus
if [ -s "$FIXTURE/out" ]; then
    echo "FAIL: the census answered without a log for the run it is about:" >&2
    sed 's/^/    /' "$FIXTURE/out" >&2
    exit 1
fi

# A directory that is not a test build carries no ledger, and the census is
# silent rather than wrong about a tree it cannot see.
rm -f "$FIXTURE/shell-lane-stanzas.txt"
echo "1/1 Test: unit_something" > "$inProgressLog"
runCensus
if [ -s "$FIXTURE/out" ]; then
    echo "FAIL: the census spoke about a directory carrying no stanza ledger:" >&2
    sed 's/^/    /' "$FIXTURE/out" >&2
    exit 1
fi

echo "shell_lane_census: the census counts what an invocation left out"
