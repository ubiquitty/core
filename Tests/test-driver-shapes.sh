#!/usr/bin/env bash
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

# Exercise one stanza through each build route: inline compilation, the
# standalone archive fixture, a shared CMake target, and an external script.
# CTest may supply a built fixture; the stanza still executes its assertions.

set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT" || exit 1

FAILURES=0

# An awk program reopened through fd 0 can vanish under parallel CTest.
stdin_source="$(printf '/dev/%s' stdin)"
if grep -nF -- "$stdin_source" Scripts/run-tests.sh Tests/test-driver-shapes.sh; then
    echo "test-driver-shapes: FAILED — awk programs must live in source files" >&2
    exit 1
fi

shape() { # <description> <stanza>
    local what="$1" stanza="$2"
    printf '  %-14s %-26s ' "$what" "$stanza"
    if ./Scripts/run-tests.sh "$stanza" >/dev/null 2>&1; then
        echo ok
    else
        echo FAILED
        FAILURES=$((FAILURES + 1))
    fi
}

# The stanza for each shape is derived from the driver rather than named here.
#
# A name is a thing that leaves. The inline representative was migrated to a
# CMake target, and this check then failed reporting a broken inline path — true
# of nothing except its own example, and phrased as though the driver were
# broken. Every stanza this file names is a stanza somebody may move.
#
# The cost of deriving: whichever stanza comes first may have dependencies of
# its own, so a red row can mean that stanza rather than the driver. Each row
# prints the stanza it used, which is what lets a reader tell the two apart —
# and a representative that rots produces a red row about nothing at all.
#
# `stanzas_of` reports every stanza matching a shape, in declaration order,
# tracking the enclosing stanza name so a match inside a body is attributed to
# the stanza it is in rather than to the last one declared.
stanzas_of() { # <awk pattern matching a line inside the body>
    local declared=(Scripts/run-tests.sh) file
    for file in Scripts/run-tests-stanzas-*.sh; do
        [ -f "$file" ] && declared+=("$file")
    done
    awk -v mode=shape -v want="$1" -v platform="$(uname -s)" \
        -f Scripts/stanza-declarations.awk -f Scripts/stanza-inventories.awk \
        "${declared[@]}"
}

# The stanzas the suite runs under the timing lock, read from the registration
# itself so the two cannot drift: a test named in a set_tests_properties call
# whose RESOURCE_LOCK holds `timing`, and a stanza whose
# shell_<stanza>_RESOURCE_LOCK does. Such a stanza bounds the wall clock and
# runs alone in the suite; run here beside the rest, a red row would be about
# its timing and not its route, so it is never a route's example.
timing_locked_stanzas() {
    local sources=() file
    while IFS= read -r file; do
        sources+=("$file")
    done < <(find Tests Scripts/cmake \( -name CMakeLists.txt -o -name '*.cmake' \) -type f 2>/dev/null | sort)
    [ "${#sources[@]}" -gt 0 ] || return 0
    cat "${sources[@]}" | tr '\n' ' ' \
        | grep -oE 'set_tests_properties\([^)]*\)|(set|list)\([^)]*shell_[A-Za-z0-9_]+_RESOURCE_LOCK[^)]*\)' \
        | awk -f Scripts/stanza-timing-locks.awk | sort -u
}

LIST_ROUTES=0
[ "${1:-}" = "--routes" ] && LIST_ROUTES=1
TIMING_LOCKED=" $(timing_locked_stanzas | tr '\n' ' ') "

if [ "$LIST_ROUTES" -eq 0 ]; then
    echo "test-driver-shapes: one stanza per build route"
fi
SHAPES=0

# A shape with no stanza left is an error, not a skip. Deriving from an empty
# set removes the row from the output entirely, and a path that stopped being
# checked looks exactly like a path with nothing to say.
# CTest supplies the standalone build fixture to both this probe and the
# consumer stanza. The probe still executes the consumer's assertions.
for pair in \
        "inline:@inline" \
        "shared-lib:ubq_libcore_consumer" \
        "shared-target:ubq_build_test " \
        "standalone:ROOT/Tests/test-[a-z-]+[.]sh"; do
    SHAPES=$((SHAPES + 1))
    what="${pair%%:*}"
    pattern="${pair#*:}"
    stanza=""
    locked=""
    while IFS= read -r candidate; do
        [ -n "$candidate" ] || continue
        if [[ "$TIMING_LOCKED" == *" $candidate "* ]]; then
            locked+="${locked:+ }$candidate"
            continue
        fi
        stanza="$candidate"
        break
    done < <(stanzas_of "$pattern")
    # The inline route is the driver's helper for a stanza that names its sources
    # on one line. Its stanzas are the other repositories' (their own files under
    # Scripts/run-tests-stanzas-*.sh), so a checkout without them has no example
    # of the route to build, and says so rather than passing it.
    if [ -z "$stanza" ] && [ "$what" = "inline" ] && ! ls Scripts/run-tests-stanzas-*.sh >/dev/null 2>&1; then
        echo "test-driver-shapes: SKIPPED — no stanza of this checkout builds inline; the route's examples are in the stanza files of the repositories that hold them" >&2
        continue
    fi
    if [ -z "$stanza" ] && [ -n "$locked" ]; then
        if [ "$LIST_ROUTES" -eq 1 ]; then
            echo "$what UNPROVEN"
        else
            echo "  FAILED   the $what route is unproven: every stanza with its shape" >&2
            echo "           runs under the timing lock ($locked), and none of them" >&2
            echo "           can prove the route beside the rest of the suite." >&2
        fi
        FAILURES=$((FAILURES + 1))
        continue
    fi
    if [ -z "$stanza" ]; then
        echo "  FAILED   no stanza left with the $what shape" >&2
        echo "           Either the driver stopped building that way, in which" >&2
        echo "           case delete the row, or every example migrated and this" >&2
        echo "           path is now unchecked." >&2
        FAILURES=$((FAILURES + 1))
        continue
    fi
    if [ "$LIST_ROUTES" -eq 1 ]; then
        echo "$what $stanza"
        continue
    fi
    shape "$what" "$stanza"
done

# --routes names each route's example, or UNPROVEN, without running any.
if [ "$LIST_ROUTES" -eq 1 ]; then
    [ "$FAILURES" -eq 0 ]
    exit
fi

echo
if [ "$FAILURES" -ne 0 ]; then
    echo "test-driver-shapes: FAILED — $FAILURES of $SHAPES build routes are broken" >&2
    echo "  A stanza passing proves its own route." >&2
    exit 1
fi
echo "test-driver-shapes: all $SHAPES build routes passed"
