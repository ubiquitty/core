#!/bin/bash
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0
#
# test-snapshot-prerequisites.sh — the snapshot prerequisite check refuses a
# build root without the two snapshot binaries, names the script that builds
# each, and passes once both exist.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CHECK="$ROOT/Scripts/check-snapshot-prerequisites.py"
FIXTURE="$(mktemp -d "${TMPDIR:-/tmp}/ubiquitty-snapshot-prerequisites.XXXXXX")"
trap 'rm -rf "$FIXTURE"' EXIT

# The fixture stands in for the BUILD root, so the check looks inside it rather
# than beside the sources. It kept the pre-move `build/` segment after the
# checker dropped it, so the fixture created binaries the checker then declared
# missing -- the two were migrated in opposite directions, and the positive arm
# below could never pass.
export UBQ_BUILD_ROOT="$FIXTURE"

portableBinary="$FIXTURE/core/ubiquitty-snapshot"
macosBinary="$FIXTURE/macos/Source/Platform/macOS/Ubiquitty.app/Contents/MacOS/Ubiquitty"
mkdir -p "$(dirname "$portableBinary")" "$(dirname "$macosBinary")"

if "$CHECK" "$FIXTURE" >"$FIXTURE/missing.out" 2>&1; then
    echo "FAIL: a build root holding neither snapshot binary passed the prerequisite check" >&2
    exit 1
fi

# A bare `grep >/dev/null` under `set -e` exits 1 having printed nothing, so a
# drifted expectation is indistinguishable from the check crashing. What the
# check must name is the script that BUILDS each arm, and the two arms are built
# by different scripts — asserting one text for both would pass while the check
# pointed a reader at the wrong one.
expectSuggestion() {
    if ! grep -F "$1" "$FIXTURE/missing.out" >/dev/null; then
        echo "FAIL: the prerequisite check does not tell the reader to run: $1" >&2
        echo "  what it printed instead:" >&2
        sed 's/^/    /' "$FIXTURE/missing.out" >&2
        exit 1
    fi
}
expectSuggestion "\"$FIXTURE/Scripts/build-core-snapshot.sh\""
expectSuggestion "\"$FIXTURE/Scripts/build-macos.sh\""

# Present and executable is all the check asks; an empty file answers it.
for binary in "$portableBinary" "$macosBinary"; do
    : >"$binary"
    chmod +x "$binary"
done

if ! "$CHECK" "$FIXTURE"; then
    echo "FAIL: the check refused a build root holding both snapshot binaries" >&2
    exit 1
fi
echo "PASS: the snapshot prerequisite check names what is unbuilt and accepts what is built"
