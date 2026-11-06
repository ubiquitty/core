#!/usr/bin/env bash
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

#
# test-covering-selection.sh — check-mutants.sh runs the right tests.
#
# The covering-test index turns a whole-suite run into a handful of entries, and
# a wrong answer here is invisible in the output: running too few tests reports
# "killed" and "survived" in the same form, just from less evidence. A gate
# that quietly selects nothing reads as a fast, green gate.
#
# So this asserts what check-mutants.sh asks for rather than what it concludes.
# ninja and ctest are replaced with stubs that record their arguments, which is
# also what makes the test hermetic: no compiler, no build, no load on the host.
#

set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$(dirname "$0")/shell-assertions.sh"
cd "$ROOT" || exit 1

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT



# ----------------------------------------------------------------------
# Stubs. ctest records every invocation and always passes, so a "killed"
# verdict in this test would mean the script decided it on something other
# than a test result.
mkdir -p "$WORK/bin" "$WORK/build"
cat > "$WORK/bin/ninja" <<'STUB'
#!/usr/bin/env bash
echo "ninja $*" >> "$UBQ_STUB_LOG"
exit 0
STUB
cat > "$WORK/bin/ctest" <<'STUB'
#!/usr/bin/env bash
echo "ctest $*" >> "$UBQ_STUB_LOG"
# -N is the dry run check-mutants uses to prove a filter selects something.
for arg in "$@"; do
    if [ "$arg" = "-N" ]; then
        echo "Total Tests: ${UBQ_STUB_SELECTED:-2}"
        exit 0
    fi
done
exit 0
STUB
chmod +x "$WORK/bin/ninja" "$WORK/bin/ctest"

# A real Source file, so the mutation itself is genuine — only the build and
# the test run answer without an OS call.
VICTIM="Source/Core/Input/PointerShape.cpp"
if [ ! -f "$VICTIM" ]; then
    echo "test-covering-selection: $VICTIM is gone; point this at another Core file" >&2
    exit 1
fi
VICTIM_LINE="$(grep -n '&&' "$VICTIM" | head -1 | cut -d: -f1)"
if [ -z "$VICTIM_LINE" ]; then
    echo "test-covering-selection: no mutable line found in $VICTIM" >&2
    exit 1
fi

# The victim's modification time, restored with its contents at the end.
#
# The gate rewrites this file and writes it back, so the bytes return and the
# timestamp does not -- and a Core source newer than the artifacts built from
# it is what every staleness guard downstream reads. In a full suite run this
# one file deferred the macOS and web snapshot tiers for the rest of the pass:
# the tiers refuse a binary older than the sources it renders, correctly, and
# the source had only been touched.
#
# Restoring it here and not in check-mutants.sh, because nothing here compiles:
# ninja and ctest are stubs. A real run's mutant objects are newer than the
# restored source on purpose.
VICTIM_STAMP="$(python3 -c 'import os, sys; print(os.stat(sys.argv[1]).st_mtime)' "$VICTIM")"
restore_victim_stamp() {
    [ -n "${VICTIM_STAMP:-}" ] || return 0
    python3 -c 'import os, sys; os.utime(sys.argv[1], (float(sys.argv[2]), float(sys.argv[2])))' \
        "$VICTIM" "$VICTIM_STAMP" 2>/dev/null || true
}
# Replaces the WORK-only trap above rather than adding to it: that one covers
# the window before a victim is chosen, and there is nothing to restore yet.
trap 'rm -rf "$WORK"; restore_victim_stamp' EXIT

# Writes the gate's exit status to $WORK/status and its output to $WORK/out,
# because both are assertions here and $? would otherwise report on the pipe
# that carried the log rather than on the gate.
run_gate() { # <index contents> -> the recorded stub calls
    : > "$WORK/log"
    printf '%s' "$1" > "$WORK/index.tsv"
    PATH="$WORK/bin:$PATH" \
    UBQ_STUB_LOG="$WORK/log" \
    UBQ_STUB_SELECTED="${UBQ_STUB_SELECTED:-2}" \
    UBQ_MUTANT_BUILD="$WORK/build" \
    UBQ_COVERING_INDEX="$WORK/index.tsv" \
    UBQ_MUTANT_LIMIT=1 \
        ./Scripts/check-mutants.sh --file "$VICTIM" --lines "$VICTIM_LINE" \
        > "$WORK/out" 2>&1
    echo "$?" > "$WORK/status"
    cat "$WORK/log"
}

echo "test-covering-selection: the gate asks for the tests the index names"

# ----------------------------------------------------------------------
HEAD_SHA="$(git rev-parse --short HEAD)"

# 1. A file the index covers: only those entries are built and only those run.
LOG="$(run_gate "# $HEAD_SHA, 3 tests
$VICTIM	test_mouse test_pointer_shape
Source/Core/Other.cpp	test_other
")"
contains "builds only the covering targets" \
    "ninja -C $WORK/build test_mouse test_pointer_shape" "$LOG"
contains "runs only the covering entries" \
    '-R ^(test_mouse|test_pointer_shape)$' "$LOG"
case "$LOG" in
    *test_other*) echo "  a test the index did NOT name was selected"; FAILURES=$((FAILURES + 1)) ;;
esac

# 2. Shards. Three ctest entries over one binary: the binary is built once,
#    and all three entries run — building test_reflow.0 would fail, and running
#    only test_reflow would run nothing.
LOG="$(run_gate "# $HEAD_SHA, 3 tests
$VICTIM	test_reflow.0 test_reflow.1 test_reflow.2
")"
check "a sharded entry builds its binary, not the shard" \
    "test_reflow" "$(printf '%s\n' "$LOG" | sed -n "s|^ninja -C $WORK/build ||p" | tr -d '\n')"
contains "every shard entry still runs" \
    '-R ^(test_reflow\.0|test_reflow\.1|test_reflow\.2)$' "$LOG"

# 3. A file the index does not cover, which existed when the index was built:
#    no test reaches it, so there is nothing to build and nothing to run.
LOG="$(run_gate "# $HEAD_SHA, 1 tests
Source/Core/Other.cpp	test_other
")"
# The two ninja calls that remain are the baseline build and the restore at the
# end, both whole-tree. Neither names a target, which is the assertion: nothing
# was built on behalf of a mutant no test could have noticed.
TARGETED="$(printf '%s\n' "$LOG" | sed -n "s|^ninja -C $WORK/build ||p" | tr -d '\n')"
check "an unreached file is not built for" "" "$TARGETED"
contains "and is reported as unreached" "unreached" "$(cat "$WORK/out")"

# 4. No index at all: the gate still works and says it is running everything.
: > "$WORK/log"
PATH="$WORK/bin:$PATH" UBQ_STUB_LOG="$WORK/log" \
UBQ_MUTANT_BUILD="$WORK/build" UBQ_COVERING_INDEX="$WORK/absent.tsv" \
UBQ_MUTANT_LIMIT=1 \
    ./Scripts/check-mutants.sh --file "$VICTIM" --lines "$VICTIM_LINE" \
    > "$WORK/out" 2>&1
STATUS=$?
contains "no index falls back to the whole suite" "whole suite" "$(cat "$WORK/out")"
# The message prints before the mutants run, so asserting it alone passes even
# when the script dies immediately after — which it did, on an empty array
# expanded under `set -u`. The status is what says the run finished.
check "and the run completes" "0" "$STATUS"
case "$(cat "$WORK/log")" in
    *"-R ^("*) echo "  filtered anyway with no index to filter by"; FAILURES=$((FAILURES + 1)) ;;
esac

# 5. An index naming entries this build does not have. ctest reports success
#    over an empty selection, so the gate has to refuse rather than score
#    every mutant a survivor on a run of nothing.
UBQ_STUB_SELECTED=0
LOG="$(run_gate "# $HEAD_SHA, 1 tests
$VICTIM	test_that_was_deleted
")"
check "a stale index is refused, not silently obeyed" "2" "$(cat "$WORK/status")"
contains "and says why" "selects no ctest entry" "$(cat "$WORK/out")"
unset UBQ_STUB_SELECTED

# ----------------------------------------------------------------------
# Whatever happened above, the victim must be back exactly as it was.
if ! git diff --quiet -- "$VICTIM"; then
    echo "  the mutated file was NOT restored"
    git checkout -- "$VICTIM"
    FAILURES=$((FAILURES + 1))
fi
restore_victim_stamp
NOW_STAMP="$(python3 -c 'import os, sys; print(os.stat(sys.argv[1]).st_mtime)' "$VICTIM")"
if [ "$NOW_STAMP" != "$VICTIM_STAMP" ]; then
    echo "  the mutated file's timestamp was NOT restored ($NOW_STAMP, wanted $VICTIM_STAMP)"
    FAILURES=$((FAILURES + 1))
fi

report "test-covering-selection" "the gate selects, builds and runs the right tests"
