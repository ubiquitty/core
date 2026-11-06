#!/bin/bash
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

#
# build-tests.sh — compile every test translation unit, run none of them.
#
# No frontend build compiles Tests/. host, agent, the Linux frontend and the
# macOS app can all be green while a test file cannot compile, because none of
# them includes one.
#
# Compiling is cheap and running is not, so this is the half that belongs in a
# gate every checkout pays on every push. The suite itself stays where it is: in CI,
# and in the dedicated test runs that own its breakage.
#
# Same configure the ctest tier uses, so the two share a build directory and
# neither pays for the other's reconfigure.
#
# What a green run means. "Every test translation unit compiles" is a claim this
# cannot make: the build is incremental, so what it proves is that every TU it
# CHOSE to compile did. A header two includes away can leave a file untouched
# and the run says nothing about it, so a namespace move can be green here and
# fail against the same script on a clean tree. So the count is printed, and a
# run that compiled nothing says so plainly rather than reporting success over
# an empty set.
#
# UBQ_TESTS_CLEAN=1 wipes the directory first, for when the strong claim is the
# one wanted. It is not the default because a full build is minutes and this
# gate is paid on every push.
#
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"

# How many compilers this machine can run at once; see Scripts/build-jobs.sh.
# shellcheck source=Scripts/build-jobs.sh
. "$ROOT/Scripts/build-jobs.sh"
# Build output lives outside the checkout; see Scripts/build-root.sh.
# shellcheck source=Scripts/build-root.sh
. "$ROOT/Scripts/build-root.sh"

# The tests target is the only one carrying a precompiled header, and a PCH
# cannot be shared between build directories: its AST records the absolute path
# of the cmake_pch source, so one built elsewhere sends the compiler after a
# file that is not there. ccache keeps hash_dir false globally, which is what
# lets ubench reuse objects across the per-sha worktrees it builds in; this
# turns it on for this build alone, where the sharing is the unsafe half.
export CCACHE_HASHDIR=true
BUILD="${UBQ_TESTS_BUILD:-$UBQ_BUILD_ROOT/BuildHostTests/ctest}"
JOBS="${BUILD_JOBS:-$(ubq_build_jobs)}"

# A CMakeCache records the absolute path it was configured at, and CMake refuses
# to reuse one naming anywhere else. run-linux.sh bind-mounts this tree at /src,
# so a container run leaves a cache no host configure can touch — and the gate
# would then fail on a tree it inherited rather than on the change being pushed.
if [ -f "$BUILD/CMakeCache.txt" ]; then
    configured_at="$(sed -n 's/^CMAKE_CACHEFILE_DIR:INTERNAL=//p' "$BUILD/CMakeCache.txt" | head -1)"
    configuredResolved="$( (cd "$configured_at" 2>/dev/null && pwd -P) || printf '%s' "$configured_at")"
    if [ -n "$configured_at" ] && [ "$configuredResolved" != "$(cd "$BUILD" && pwd -P)" ]; then
        echo "build-tests: $BUILD was configured at $configured_at, reconfiguring here" >&2
        rm -rf "$BUILD"
    fi
fi

if [ "${UBQ_TESTS_CLEAN:-0}" = "1" ]; then
    rm -rf "$BUILD"
fi

# CMAKE_EXPORT_COMPILE_COMMANDS, because this is the only configuration that
# compiles Tests/. Without it no compile database in the tree lists a single test
# translation unit, and every tool driven from one — clang-tidy, clangd, any
# AST-based check — silently covers Source/ and reports a clean verdict over the
# half it could see.
cmake -S "$ROOT" -B "$BUILD" -G Ninja \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    -DUBIQUITTY_BUILD_TESTS=ON -DUBIQUITTY_FRONTEND=Core -DCMAKE_BUILD_TYPE=Debug >/dev/null

# Ninja names each compile it runs; counting them is the difference between
# "this compiled" and "this was already compiled, by some earlier run, of some
# earlier tree".
BUILD_LOG="$(mktemp -t ubq-build-tests.XXXXXX)"
trap 'rm -f "$BUILD_LOG"' EXIT
cmake --build "$BUILD" -j"$JOBS" 2>&1 | tee "$BUILD_LOG"
# PIPESTATUS, because tee's success is not the compiler's.
if [ "${PIPESTATUS[0]}" -ne 0 ]; then
    echo "build-tests: the build failed (above)" >&2
    exit 1
fi

COMPILED="$(grep -c 'Building CXX object' "$BUILD_LOG" || true)"
# The denominator matters as much as the count. "159 compiled" still asks the
# reader to work out whether that is most of the tree or a rounding error; "159
# of 445" answers it. Objects on disk are the translation units this
# configuration produces, which is the number the fraction is against.
# Counted rather than described: the number moves every time a stanza becomes
# a CMake target, and a number a reader cannot check only ever gets more wrong.
# `--cached --others --exclude-standard`, so a file being added is counted. A
# tracked-only listing has never heard of the source whose first version is the
# one outside the glob, which is the population this sentence exists to size.
# Every frontend backend, none of which this configuration compiles. Counting
# only the Tests/ half would name a smaller gap than the one that exists: a
# change to the sync decoders can leave shell stanzas BUILD-BROKEN while this
# script reports zero errors, because the files they break live under
# Source/Platform and this configures UBIQUITTY_FRONTEND=Core.
PLATFORM_SOURCES="$(git ls-files --cached --others --exclude-standard \
    'Source/Platform/*.cpp' 'Source/Platform/*.mm' | grep -c . || true)"
UBQ_TEST_SOURCES="$(git ls-files --cached --others --exclude-standard 'Tests/*.cpp')"
ALL_TEST_SOURCES="$(printf '%s\n' "$UBQ_TEST_SOURCES" | grep -c . || true)"
OUTSIDE_GLOB="$(printf '%s\n' "$UBQ_TEST_SOURCES" | grep -vcE '(^|/)test_[^/]*\.cpp$' || true)"

TOTAL="$(find "$BUILD" -name '*.o' 2>/dev/null | wc -l | tr -d ' ')"
if [ "$COMPILED" -eq 0 ]; then
    echo "build-tests: 0 of $TOTAL translation units compiled — all were up to date."
    echo "  This run proves nothing about any file it did not build."
else
    echo "build-tests: $COMPILED of $TOTAL translation units compiled ($((TOTAL - COMPILED)) up to date)."
    echo "  A file this run did not build is not a file this run checked."
fi

# Outside the branch, because the subject is the same either way: the run that
# compiled nothing proves least, and it names what it cannot see like any other.
echo "  The denominator counts what Tests/CMakeLists.txt globs — test_*.cpp."
echo "  ${OUTSIDE_GLOB} of ${ALL_TEST_SOURCES} source(s) under Tests/ are outside it: built on"
echo "  demand by a run-tests.sh stanza, or a bench or fuzz tool."
echo "  ${PLATFORM_SOURCES} source(s) under Source/Platform/ are outside it too: this"
echo "  configures UBIQUITTY_FRONTEND=Core, which compiles no frontend backend."
echo "  Green here says nothing about either set."

# The count is a claim about the run; this is the claim about the change. Ninja
# records which headers each compile actually opened, so a changed header can be
# traced to the translation units that read it and checked against the ones this
# run built. A green build that skipped them is the failure this script exists
# to prevent and cannot otherwise see.
python3 "$ROOT/Scripts/check-build-reached.py" "$BUILD" || exit 1
