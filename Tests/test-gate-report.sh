#!/usr/bin/env bash
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0
#
# test-gate-report.sh — the shell and Python verdict lines are the same line.
#
# The format lives twice because the gates do: some are shell, some are Python.
# Two spellings of one format drift the first time one is edited alone, and the
# drift is invisible — each side keeps printing something plausible. So the
# cases below are run through both and compared byte for byte.
#
# The zero-offender and over-the-limit cases matter most: they are the two
# branches, and a change to either is exactly what one author would make without
# opening the other file.

set -u

# The cases compare the verdict line, and an annotation would sit beside it.
# Unset rather than filtered: a filter that has to know the annotation's shape
# is a second copy of that shape, in the one place that exists to have one.
unset UBQ_CI_ANNOTATE

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "$ROOT/Scripts/gate-report-lib.sh"

FAILURES=0
CASES=0

# Each case: gate, what, then the offenders.
check() {
    CASES=$((CASES + 1))
    local shell_line python_line
    shell_line="$(gate_failed "$@" 2>&1 || true)"
    python_line="$(python3 "$ROOT/Scripts/gate_report.py" "$@")"
    if [ "$shell_line" != "$python_line" ]; then
        FAILURES=$((FAILURES + 1))
        echo "  shell:  $shell_line"
        echo "  python: $python_line"
    fi
}

check "check-x" "thing(s) that went wrong"
check "check-x" "thing(s) that went wrong" "one"
check "check-x" "thing(s) that went wrong" "one" "two"
check "check-x" "thing(s) that went wrong" "one" "two" "three"
check "check-x" "thing(s) that went wrong" "one" "two" "three" "four"
check "check-x" "thing(s) that went wrong" "one" "two" "three" "four" "five" "six"
check "check-x" "file(s)" "Source/a.cpp:12" "Source/b.h:40"
check "gate with spaces" "offender(s) whose name has a space" "a b" "c d" "e f" "g h"

# A verdict line that names no offender and no count is the defect this whole
# library exists to prevent; assert the shape rather than trusting the wording.
BARE="$(gate_failed "check-y" "something" 2>&1 || true)"
CASES=$((CASES + 1))
case "$BARE" in
    *FAILED*[:—]) FAILURES=$((FAILURES + 1)); echo "  a verdict line ends in a bare separator: $BARE" ;;
esac

COUNTED="$(gate_failed "check-y" "offender(s)" a b c d 2>&1 || true)"
CASES=$((CASES + 1))
case "$COUNTED" in
    *"FAILED — 4 offender(s): a, b, c (+1 more)") ;;
    *) FAILURES=$((FAILURES + 1)); echo "  the counted form is wrong: $COUNTED" ;;
esac

# The finding shape is what tools and models actually parse, so it is compared
# the same way. A finding is never sampled or truncated — only the verdict names
# a few — so every offender reaches the reader as its own parseable line.
finding_check() {
    CASES=$((CASES + 1))
    local shell_line python_line
    shell_line="$(gate_finding "$@" 2>&1)"
    python_line="$(python3 "$ROOT/Scripts/gate_report.py" --finding "$@")"
    if [ "$shell_line" != "$python_line" ]; then
        FAILURES=$((FAILURES + 1))
        echo "  shell:  $shell_line"
        echo "  python: $python_line"
    fi
}

finding_check "Source/Core/Foo.cpp:42" "check-style" "a boolean parameter takes an enum"
finding_check "Scripts/thing.sh" "check-x" "no line number is known for this one"
finding_check "a path with spaces.cpp:1" "check-x" "a message, with a comma"

DIAGNOSTIC="$(gate_finding "Source/A.cpp:9" "check-y" "something is wrong" 2>&1)"
CASES=$((CASES + 1))
[ "$DIAGNOSTIC" = "Source/A.cpp:9: check-y: something is wrong" ] || {
    FAILURES=$((FAILURES + 1)); echo "  the finding form is not the compiler shape: $DIAGNOSTIC"
}

count_check() {
    CASES=$((CASES + 1))
    local shell_line python_line
    shell_line="$(gate_failed_count "$@" 2>&1 || true)"
    python_line="$(python3 "$ROOT/Scripts/gate_report.py" --count "$@")"
    if [ "$shell_line" != "$python_line" ]; then
        FAILURES=$((FAILURES + 1))
        echo "  shell:  $shell_line"
        echo "  python: $python_line"
    fi
}

count_check "check-x" 7 "violation(s) in the added lines"
count_check "check-x" 0 "thing(s), which should never be reported as a failure"

OK_LINE="$(gate_ok "check-y" "12 files")"
CASES=$((CASES + 1))
[ "$OK_LINE" = "check-y: ok (12 files)" ] || {
    FAILURES=$((FAILURES + 1)); echo "  the passing form is wrong: $OK_LINE"
}

if [ "$FAILURES" -ne 0 ]; then
    echo "test-gate-report: FAILED — $FAILURES of $CASES case(s): the shell and Python verdict formats disagree" >&2
    exit 1
fi
echo "test-gate-report: ok ($CASES cases, shell and Python agree byte for byte)"
