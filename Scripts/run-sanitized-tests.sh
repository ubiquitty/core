#!/bin/bash
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

#
# run-sanitized-tests.sh — the CMake-registered test suite under
# AddressSanitizer and UndefinedBehaviorSanitizer.
#
# Usage:
#   ./Scripts/run-sanitized-tests.sh              build, run every registered entry, then the host pass
#   ./Scripts/run-sanitized-tests.sh -R <regex>   run the entries ctest matches, and no host pass
#   ./Scripts/run-sanitized-tests.sh --no-host    every registered entry, and no host pass
#
# One driver builds the Core and the tests that exercise it with one
# instrumentation, because a checker sees only the allocations it was compiled
# into: a Core built plain hands the test a pointer whose bounds the sanitizer
# never learned, and the overrun the suite exists to catch reads as clean.
#
# Pinned to clang. GCC's libasan on Fedora is a linker script that names a
# runtime the package does not install, so -fsanitize=address under GCC fails
# CMake's compiler test before a line of the tree is compiled; clang links the
# compiler-rt it ships with. CC/CXX are honoured so a differently named clang
# can be used, and a compiler that is not clang is refused rather than tried,
# because the refusal names the cause and the configure failure does not.
#
# The shell stanzas are not in the CMake pass. Each compiles its own link set
# through the driver's own compiler invocation, which carries no -fsanitize, so
# a green stanza there would be a plain run reported as a sanitized one. The
# CMake pass is exactly what CMake registers, which is what makes its verdict a
# statement about instrumented binaries and nothing else.
#
# The host pass is the exception that keeps that true: it builds ubiquitty-host
# itself with the same instrumentation and runs only the stanzas that drive the
# daemon as a binary, host_smoke, host_cli and host_soak, against that build
# (UBQ_HOST_TEST_BUILD, UBQ_HOST_PREBUILT). Every allocation the daemon makes is
# then one the sanitizer learned, and LeakSanitizer reads the daemon at its
# exit on Linux; macOS arm64 has no LeakSanitizer runtime. Stanzas that link
# the host's objects into a test of their own compile that test plain, so they
# stay out of it.
#

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"

# How many compilers this machine can run at once; see Scripts/build-jobs.sh.
# shellcheck source=Scripts/build-jobs.sh
. "$ROOT/Scripts/build-jobs.sh"
# Build output lives outside the checkout; see Scripts/build-root.sh.
# shellcheck source=Scripts/build-root.sh
. "$ROOT/Scripts/build-root.sh"
OUT="$UBQ_BUILD_ROOT/BuildHostTests/sanitizers"
BUILD="$OUT/build"
JOBS="${JOBS:-$(ubq_build_jobs)}"
# The compiler's fan-out is bounded by memory, because a C++23 translation unit
# holds a lot of it and four runners share one slice. A test process does not,
# so the suite is scheduled on a number of its own — the same one
# Scripts/run-tests.sh uses. Borrowing the compiler's runs the suite far
# narrower than the host allows and leaves most of its cores idle.
CTEST_JOBS="${CTEST_JOBS:-8}"

CTEST_FILTER=()
SHOULD_RUN_HOST=1
while [ $# -gt 0 ]; do
    case "$1" in
        -R) shift; CTEST_FILTER=(-R "${1:?run-sanitized-tests.sh: -R needs a regex}"); SHOULD_RUN_HOST=0 ;;
        --no-host) SHOULD_RUN_HOST=0 ;;
        *) echo "run-sanitized-tests.sh: unknown flag: $1" >&2; exit 2 ;;
    esac
    shift
done

CC="${CC:-clang}"
CXX="${CXX:-clang++}"
export CC CXX
for compiler in "$CC" "$CXX"; do
    if ! command -v "$compiler" >/dev/null 2>&1; then
        echo "run-sanitized-tests.sh: $compiler is not on PATH; the sanitized build needs clang." >&2
        exit 2
    fi
    if ! "$compiler" --version 2>/dev/null | grep -qi clang; then
        echo "run-sanitized-tests.sh: $compiler is not clang. GCC's libasan on Fedora is a" >&2
        echo "  linker script naming a runtime the package does not install, so the configure" >&2
        echo "  fails its compiler test; set CC=clang CXX=clang++." >&2
        exit 2
    fi
done

mkdir -p "$OUT"

# Debug is named for the reason run-coverage.sh names it: an empty build type is
# FORCEd to Release, which defines NDEBUG and changes what the suite asserts.
# The timeout is the suite's, multiplied for instrumentation: shadow memory on
# every access costs several times the plain runtime, and the slowest entries
# sit close enough to the plain budget that the instrumented run kills them.
cmake -S "$ROOT" -B "$BUILD" -G Ninja \
    -DUBIQUITTY_FRONTEND=Core -DCMAKE_BUILD_TYPE=Debug \
    -DUBIQUITTY_BUILD_TESTS=ON -DUBIQUITTY_SANITIZERS=address,undefined \
    -DUBIQUITTY_TEST_TIMEOUT=300 \
    >/dev/null

if ! cmake --build "$BUILD" --parallel "$JOBS" > "$OUT/build.log" 2>&1; then
    grep -E 'error:|FAILED' "$OUT/build.log" | head -20 >&2
    echo "run-sanitized-tests.sh: the instrumented build failed (full log: $OUT/build.log)." >&2
    exit 1
fi

# The build is done above, so ubq_build is excluded rather than run twice; the
# shell stanzas are excluded because they are not instrumented (see the header).
#
# UBSan reports and continues by default, so a test whose body has undefined
# behaviour would exit zero and pass; halt_on_error makes every report a failure.
# A stack frame that outlives its function is a class ASan finds only when told
# to keep frames alive, so detect_stack_use_after_return is on.
export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_stack_use_after_return=1:strict_string_checks=1}"
export UBSAN_OPTIONS="${UBSAN_OPTIONS:-halt_on_error=1:print_stacktrace=1}"
"$ROOT/Scripts/run-ctest.sh" "$BUILD" \
    -j"$CTEST_JOBS" -E '^(ubq_build|shell_)' --output-on-failure \
    ${CTEST_FILTER[@]+"${CTEST_FILTER[@]}"}

[ "$SHOULD_RUN_HOST" = 1 ] || exit 0

HOST_BUILD="$OUT/host"
cmake -S "$ROOT" -B "$HOST_BUILD" -G Ninja \
    -DUBIQUITTY_FRONTEND=Host -DCMAKE_BUILD_TYPE=Debug \
    -DUBIQUITTY_SANITIZERS=address,undefined \
    >/dev/null
if ! cmake --build "$HOST_BUILD" --parallel "$JOBS" --target ubiquitty-host > "$OUT/host-build.log" 2>&1; then
    grep -E 'error:|FAILED' "$OUT/host-build.log" | head -20 >&2
    echo "run-sanitized-tests.sh: the instrumented host did not build (full log: $OUT/host-build.log)." >&2
    exit 1
fi
UBQ_HOST_TEST_BUILD="$HOST_BUILD" UBQ_HOST_PREBUILT=1 \
    "$ROOT/Scripts/run-tests.sh" host_smoke host_cli
# The soak bounds the daemon's resident size, and ASan's quarantine keeps every
# freed block until the quarantine is full, which reads as growth that is not
# there. Leaks are LeakSanitizer's to report, at the daemon's exit; the two
# stanzas above keep the quarantine, which is what catches a use after free.
ASAN_OPTIONS="${ASAN_OPTIONS}:quarantine_size_mb=0" \
    UBQ_HOST_TEST_BUILD="$HOST_BUILD" UBQ_HOST_PREBUILT=1 \
    "$ROOT/Scripts/run-tests.sh" host_soak
