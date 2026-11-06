# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

# shellcheck shell=bash
#
# Tests/shell-assertions.sh — the assertion vocabulary for shell-level tests.
#
# Harness.h gives the C++ tests one set of assertions and one way to report,
# and this gives the shell tests the same. A copy in each test drifts — into
# spellings of `contains` that differ in whether a failure prints what it
# actually saw. A test whose failure cannot be diagnosed from its own output is
# half a test, so the version that prints both sides is the one here.
#
# Source it after setting up, then call `report <name>` last:
#
#     . "$(dirname "$0")/shell-assertions.sh"
#     check "the gate refuses an unreachable arm" "2" "$status"
#     report "test-bench-baseline"
#
# FAILURES is owned here. A caller that keeps its own count will disagree with
# the one `report` reads, which is the same defect in miniature as two spellings
# of one assertion.
#

FAILURES=0

# A haystack is often a whole script's output. Printing all of it buries the
# assertion that failed; printing none of it leaves the failure undiagnosed.
UBQ_ASSERT_HAYSTACK_WIDTH="${UBQ_ASSERT_HAYSTACK_WIDTH:-400}"

# The two primitives. A case that decides for itself — a file mode, an exit
# status read three lines earlier — calls these directly instead of contorting
# itself into an equality.
pass() { # <description>
    printf '  %-52s ok\n' "$1"
}

fail() { # <description> [detail lines...]
    printf '  %-52s FAILED\n' "$1"
    shift
    while [ "$#" -gt 0 ]; do printf '    %s\n' "$1"; shift; done
    FAILURES=$((FAILURES + 1))
}

_ubq_fail() { # <lines...>  — for the built-ins, whose label is already printed
    echo FAILED
    while [ "$#" -gt 0 ]; do printf '    %s\n' "$1"; shift; done
    FAILURES=$((FAILURES + 1))
}

_ubq_excerpt() { # <text>
    printf '%s' "$1" | tr '\n' '~' | cut -c1-"$UBQ_ASSERT_HAYSTACK_WIDTH" | tr '~' '\n'
}

# Asserts two values are equal.
check() { # <description> <expected> <actual>
    printf '  %-52s ' "$1"
    if [ "$2" = "$3" ]; then
        echo ok
    else
        _ubq_fail "expected: $2" "actual:   $3"
    fi
}

# Asserts <haystack> contains <needle> as a literal substring.
contains() { # <description> <needle> <haystack>
    printf '  %-52s ' "$1"
    case "$3" in
        *"$2"*) echo ok ;;
        *) _ubq_fail "wanted to find: $2" "in:" "$(_ubq_excerpt "$3")" ;;
    esac
}

# Asserts <haystack> does not contain <needle>.
#
# Spelled out rather than left to each caller's own `case`, because that form
# prints only on failure: a negative assertion that passes leaves no line in the
# output, so a reader cannot tell it from one that was never reached. Every
# other assertion here says `ok` when it holds, and a check that is silent when
# it succeeds is indistinguishable from a check that was deleted.
absent() { # <description> <needle> <haystack>
    printf '  %-52s ' "$1"
    case "$3" in
        *"$2"*) _ubq_fail "did not want to find: $2" "in:" "$(_ubq_excerpt "$3")" ;;
        *) echo ok ;;
    esac
}

# Prints the verdict and exits. Non-zero when anything failed, so a caller that
# forgets to check is still reported by its exit status rather than by its last
# line of output.
report() { # <test name> [summary line]
    echo
    if [ "$FAILURES" -ne 0 ]; then
        echo "$1: FAILED — $FAILURES check(s)" >&2
        exit 1
    fi
    echo "$1: ${2:-every check passed}"
    exit 0
}
