#!/bin/bash
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

#
# run-coverage.sh — source-based line coverage for the host test suite.
#
# Usage:
#   ./Scripts/run-coverage.sh              build, run, write the report
#   HTML=1 ./Scripts/run-coverage.sh       also write the per-file HTML drill-down
#   ./Scripts/run-coverage.sh --gaps       print the least-covered files first
#   ./Scripts/run-coverage.sh --check N    exit non-zero below N% total line coverage
#   ./Scripts/run-coverage.sh --check-file-floors
#                                          exit non-zero below a per-file floor
#                                          (Scripts/check-coverage-floor.sh)
#
# One driver builds every test and the Core it exercises with the same
# instrumentation, because llvm-cov attributes a Core line to whichever test
# binary ran it: instrument only half the tree and reached lines report as
# missed.
#
# Artifacts, all under BuildHostTests/coverage/ and all consumed by something:
#   summary.txt         the per-file table; check-coverage-floor.sh reads it
#   coverage.lcov       per-line hit counts; coverage-changed-lines.py reads it
#   coverage.toolchain  llvm-cov / llvm-profdata versions, so the lcov is
#                       self-describing
#   covering-tests.tsv  which tests reach which file; check-mutants.sh reads it
#   html/index.html     per-file drill-down, with HTML=1
#
# The precompiled header is off here on purpose: force-including headers into
# TUs that never named them shifts which lines llvm-cov maps, and coverage has
# to measure the source as written.
#

set -euo pipefail

SCRIPTS="$(cd "$(dirname "$0")" && pwd)"
sourceArguments=()
while [ "$#" -gt 0 ]; do
    case "$1" in
        --source-root|--core-source-root|--kit-source-root)
            [ "$#" -ge 2 ] || { echo "run-coverage: $1 requires a source directory" >&2; exit 2; }
            sourceArguments+=("$1" "$2"); shift 2 ;;
        *) break ;;
    esac
done
ROOT="" CORE_ROOT="" KIT_ROOT=""
sourceContext="$(python3 "$SCRIPTS/ubq_roots.py" shell-context ${sourceArguments[@]+"${sourceArguments[@]}"})"
# The shared resolver emits fixed names and shell-quoted values only.
eval "$sourceContext"

# How many compilers this machine can run at once; see Scripts/build-jobs.sh.
# shellcheck source=Scripts/build-jobs.sh
. "$CORE_ROOT/Scripts/build-jobs.sh"
# Build output lives outside the checkout; see Scripts/build-root.sh.
# shellcheck source=Scripts/build-root.sh
. "$CORE_ROOT/Scripts/build-root.sh"
OUT="$UBQ_BUILD_ROOT/BuildHostTests/coverage"
BUILD="$OUT/build"
JOBS="${JOBS:-$(ubq_build_jobs)}"
# The compiler's fan-out is bounded by memory, because a C++23 translation unit
# holds a lot of it and four runners share one slice. A test process does not,
# so the suite is scheduled on a number of its own — the same one
# Scripts/run-tests.sh uses. Borrowing the compiler's runs the suite far
# narrower than the host allows and leaves most of its cores idle.
CTEST_JOBS="${CTEST_JOBS:-8}"

GAPS=0
FLOOR=""
FILE_FLOORS=0
NATIVE_ONLY=0
while [ $# -gt 0 ]; do
    case "$1" in
        --html)  HTML=1 ;;
        --gaps)  GAPS=1 ;;
        --check) shift; FLOOR="${1:-}" ;;
        --check-file-floors) FILE_FLOORS=1 ;;
        --native-only) NATIVE_ONLY=1 ;;
        *) echo "run-coverage.sh: unknown flag: $1" >&2; exit 2 ;;
    esac
    shift
done

# llvm-profdata and llvm-cov must come from the same toolchain as the compiler,
# or the profile format version mismatches and the merge fails.
PROFDATA="$(xcrun --find llvm-profdata 2>/dev/null || echo llvm-profdata)"
LLVMCOV="$(xcrun --find llvm-cov 2>/dev/null || echo llvm-cov)"

# Debug is named rather than left to the default. This directory's cache carried
# an EMPTY build type, and Scripts/cmake/UbiquittyToolchain.cmake FORCEs empty
# to Release — so the next run would have rebuilt the coverage tree at -O3
# -DNDEBUG. That deletes the assertions and moves llvm-cov's region attribution,
# which the per-file floors in check-coverage-floor.sh and the covering-test
# index are both calibrated against.
sourceOptions=("-DUBIQUITTY_CORE_SOURCE_DIR=$CORE_ROOT")
[ -z "$KIT_ROOT" ] || sourceOptions+=("-DUBIQUITTY_KIT_SOURCE_DIR=$KIT_ROOT")
FRONTEND=Core
if [ "$ROOT" = "$KIT_ROOT" ]; then
    FRONTEND=Linux
fi
if [ "$NATIVE_ONLY" = 1 ]; then
    sourceOptions+=("-DUBIQUITTY_CTEST_SHELL_REMAINDER=OFF")
else
    sourceOptions+=("-DUBIQUITTY_CTEST_SHELL_REMAINDER=ON")
fi
cmake -S "$ROOT" -B "$BUILD" -G Ninja \
    -DUBIQUITTY_FRONTEND="$FRONTEND" -DCMAKE_BUILD_TYPE=Debug \
    -DUBIQUITTY_BUILD_TESTS=ON -DUBIQUITTY_COVERAGE=ON -DUBIQUITTY_TEST_PCH=OFF \
    "${sourceOptions[@]}" \
    -DUBIQUITTY_TEST_TIMEOUT=300 \
    >/dev/null
# The timeout is the suite's, multiplied for instrumentation: counters on every
# edge cost several times the plain runtime, and the slowest entries sit close
# enough to the plain budget that the instrumented run kills them. A killed
# entry writes no profile, so its lines read as uncovered and the changed-line
# gate reports a phantom regression: uncovered lines in a file its own tests
# exercise.
#
# Not silenced. This build is the one configuration that compiles Tests/ without
# the precompiled header, so it is the only place a file missing an include of
# its own is visible — the PCH force-includes what such a file never named. A
# file that compiles everywhere else fails only here, and with the output on
# /dev/null this script would exit 1 saying nothing at all.
if ! cmake --build "$BUILD" --parallel "$JOBS" > "$OUT/build.log" 2>&1; then
    grep -E 'error:|FAILED' "$OUT/build.log" | head -20 >&2
    echo "run-coverage: the instrumented build failed (full log: $OUT/build.log)." >&2
    echo "  This configuration builds with UBIQUITTY_TEST_PCH=OFF, so a source" >&2
    echo "  that only compiles under the shared precompiled header fails here" >&2
    echo "  and nowhere else." >&2
    exit 1
fi

# CMake decides this path and gives each ctest entry a filename under it that
# starts with the entry's name; build-coverage-index.sh reads the names back.
RAW="$BUILD/coverage-raw"
rm -rf "$RAW"; mkdir -p "$RAW"
# The fallback for any process ctest did not give an environment to — the build
# fixture, a helper spawned outside a test. It still contributes its counters to
# the merged total, and its name carries no entry, so nothing attributes it to a
# test that did not run it. %p substitutes the PID, so concurrent processes
# cannot clobber one another.
export LLVM_PROFILE_FILE="$RAW/unattributed-%p.profraw"

# A failing test still contributes the lines it reached, so the report is
# produced either way and the status is reported at the end.
TESTS_OK=0
"$CORE_ROOT/Scripts/run-ctest.sh" "$BUILD" \
    -j"$CTEST_JOBS" -E 'ubq_build|shell_remainder' --output-on-failure || TESTS_OK=$?

raw_files=( "$RAW"/*.profraw )
if [ ! -e "${raw_files[0]}" ]; then
    echo "run-coverage: no .profraw in $RAW — did the tests exit normally?" >&2
    exit 1
fi
"$PROFDATA" merge -sparse "${raw_files[@]}" -o "$OUT/merged.profdata"

# Only binaries this configuration still knows about. The build directory is not
# cleaned between runs, so a test deleted or renamed leaves its executable
# behind — and llvm-cov reads a stale binary's coverage mapping as readily as a
# live one, reporting files the tree no longer contains and counting them in the
# denominator. A binary from before a directory was moved will name the old
# paths forever.
OBJECTS=()
STALE=()
ubq_live_tests="$(ctest --test-dir "$BUILD" -N 2>/dev/null |
    sed -n 's/^ *Test *#[0-9]*: *//p' | sed 's/\.[0-9]*$//' | sort -u)"
for binary in "$BUILD"/Tests/*; do
    [ -x "$binary" ] && [ ! -d "$binary" ] || continue
    if printf '%s\n' "$ubq_live_tests" | grep -qxF "$(basename "$binary")"; then
        OBJECTS+=(-object "$binary")
    else
        STALE+=("$(basename "$binary")")
    fi
done
if [ ${#OBJECTS[@]} -eq 0 ]; then
    echo "run-coverage: no test binary matched a ctest entry; the report would" >&2
    echo "  cover nothing and read as 0%. Is $BUILD configured with tests on?" >&2
    exit 1
fi

# The shell tier's binaries: built by run-tests.sh during ctest, outside this
# CMake build — ad-hoc compiles under $BUILD/shell/<stanza>/ plus the two
# shared Host roots the nested cmakes fill. Only what carries a coverage map
# is passed: the knob is explicit both ways in run-tests.sh's cmake wrapper,
# so covmap presence tracks "most recently built instrumented" exactly, and a
# clean binary from a normal run filters itself out. A $BUILD/shell directory
# whose stanza no longer has a ctest entry is skipped for the same reason the
# Tests/ loop above names stale binaries: llvm-cov reads a dead binary's
# mapping as readily as a live one.
carries_coverage_map() {
    if command -v otool >/dev/null 2>&1; then
        otool -l "$1" 2>/dev/null | grep -q __llvm_covmap
    else
        "$(xcrun --find llvm-objdump 2>/dev/null || echo llvm-objdump)" -h "$1" 2>/dev/null | grep -q covmap
    fi
}
SHELL_OBJECT_COUNT=0
add_shell_objects_under() {
    local root="$1" candidate
    [ -d "$root" ] || return 0
    while IFS= read -r candidate; do
        if carries_coverage_map "$candidate"; then
            OBJECTS+=(-object "$candidate")
            SHELL_OBJECT_COUNT=$((SHELL_OBJECT_COUNT + 1))
        fi
    done < <(find "$root" -type f -perm -111 ! -name '*.a' ! -name '*.o' ! -name '*.sh' ! -name '*.py' 2>/dev/null)
}
for stanza_dir in "$BUILD"/shell/*/; do
    [ -d "$stanza_dir" ] || continue
    stanza_name="$(basename "$stanza_dir")"
    if printf '%s\n' "$ubq_live_tests" | grep -qxF "shell_$stanza_name"; then
        add_shell_objects_under "$stanza_dir"
    fi
done
add_shell_objects_under "$UBQ_BUILD_ROOT/host-tests"
add_shell_objects_under "$UBQ_BUILD_ROOT/host-quic"
# Named rather than dropped quietly: a binary with no ctest entry is usually a
# test that was deleted or renamed, and the leftover is worth removing.
if [ ${#STALE[@]} -ne 0 ]; then
    echo "run-coverage: ignoring ${#STALE[@]} binary(ies) with no ctest entry: ${STALE[*]}" >&2
fi

# Test code, generated data, and upstream we don't own stay out of our
# denominator — re2/simdutf/wuffs/yyjson internals would otherwise flood the
# report at 0% and the changed-line gate would demand coverage of vendored lines.
IGNORE='(Tests/|/compat/|BuildHostTests/|ThirdParty/)'
if [ "$ROOT" = "$KIT_ROOT" ]; then
    # Dependency coverage cannot inflate a Kit verdict. Include every built
    # Kit library's mappings, including functions no test linked or exercised.
    dependencyPattern="$(python3 -c 'import re, sys; print(re.escape(sys.argv[1]) + "/")' "$CORE_ROOT")"
    IGNORE="(Tests/|/compat/|BuildHostTests/|ThirdParty/|$dependencyPattern)"
    for name in FreeType GTK GTKPAL Software Vulkan; do
        library="$BUILD/libUbiquittyKit$name.a"
        if [ ! -f "$library" ]; then
            echo "run-coverage: missing Kit library coverage input: $library" >&2
            exit 1
        fi
        OBJECTS+=(-object "$library")
    done
fi

"$LLVMCOV" report "${OBJECTS[@]}" -instr-profile="$OUT/merged.profdata" \
    -ignore-filename-regex="$IGNORE" 2>&1 | tee "$OUT/summary.txt" >/dev/null

"$LLVMCOV" export "${OBJECTS[@]}" -instr-profile="$OUT/merged.profdata" \
    -ignore-filename-regex="$IGNORE" -format=lcov > "$OUT/coverage.lcov"

{
    "$LLVMCOV" --version | sed -n '1,2p'
    "$PROFDATA" --version | sed -n '1p'
} > "$OUT/coverage.toolchain"

# Determinism proof: the gate's input has to be reproducible. A second merge and
# export of the same profraw must be byte-identical, so a toolchain or flag
# change that made coverage.lcov order-unstable fails here rather than silently
# drifting the gate. This proves artifact generation is deterministic; test
# execution determinism is the suite's own property, upstream of the profraw.
"$PROFDATA" merge -sparse "${raw_files[@]}" -o "$OUT/merged.verify.profdata"
"$LLVMCOV" export "${OBJECTS[@]}" -instr-profile="$OUT/merged.verify.profdata" \
    -ignore-filename-regex="$IGNORE" -format=lcov > "$OUT/coverage.verify.lcov"
if ! cmp -s "$OUT/coverage.lcov" "$OUT/coverage.verify.lcov"; then
    echo "coverage: NON-DETERMINISTIC — a second merge+export differs from the first" >&2
    exit 1
fi
rm -f "$OUT/merged.verify.profdata" "$OUT/coverage.verify.lcov"

if [ "${HTML:-0}" = "1" ]; then
    rm -rf "$OUT/html"; mkdir -p "$OUT/html"
    "$LLVMCOV" show "${OBJECTS[@]}" -instr-profile="$OUT/merged.profdata" \
        -ignore-filename-regex="$IGNORE" \
        -format=html -show-line-counts-or-regions \
        -output-dir="$OUT/html" >/dev/null
fi

if [ "$GAPS" = "1" ]; then
    echo "=== least-covered files (missed lines, descending) ==="
    awk '$1 ~ /Source\// {
        lines=$(NF-5); missed=$(NF-4); cover=$(NF-3);
        if (missed+0 > 0) printf "%6d missed  %8s  %6d lines  %s\n", missed, cover, lines, $1
    }' "$OUT/summary.txt" | sort -rn | head -40
    echo
fi

# The per-test profiles are only readable while they are still on disk, and the
# next coverage run deletes them. Turning them into the index here means the
# index is never older than the lcov beside it.
if [ "$ROOT" = "$CORE_ROOT" ]; then
    "$CORE_ROOT/Scripts/build-coverage-index.sh" || \
        echo "run-coverage: the covering-test index could not be built (coverage itself is fine)" >&2
fi

# How far the committed copy had fallen behind, said by the only run that can
# know. This rebuilds the tracked file in place, so the two copies exist
# together for exactly this long; afterwards a reader can see that the index is
# old but not what adopting it would have changed.
if [ "$ROOT" = "$CORE_ROOT" ] && ! git -C "$CORE_ROOT" diff --quiet -- Tests/coverage/covering-tests.tsv 2>/dev/null; then
    echo "index : rebuilt, and the committed copy differs"
    echo "  committed: $(git -C "$CORE_ROOT" show HEAD:Tests/coverage/covering-tests.tsv 2>/dev/null | head -1)"
    echo "  rebuilt:   $(head -1 "$CORE_ROOT/Tests/coverage/covering-tests.tsv")"
    git -C "$CORE_ROOT" diff --stat -- Tests/coverage/covering-tests.tsv | sed 's/^/  /'
fi

tail -3 "$OUT/summary.txt"

LINE_PCT=$(awk '/^TOTAL/ { pct=$(NF-3); gsub("%","",pct); print pct }' "$OUT/summary.txt")
echo "line coverage: ${LINE_PCT}%"

# llvm-cov writes its warnings to stderr, and the report above folds stderr into
# the same file as the table. A reader who opens summary.txt meets a percentage,
# and the sentence naming the functions it was computed in the absence of sits
# above that percentage and reaches no console.
#
# The mismatched-function count is a DUPLICATE count, not a coverage gap, and
# saying otherwise makes a sound number look untrustworthy. Every test binary
# statically links the Core it uses, all of them are passed as -object, and the
# same function therefore arrives once per binary that kept it. llvm-cov scores
# the first record and drops the rest.
#
# Measured against this build by varying the object count and holding the
# profile set fixed:
#
#     objects    functions scored    line coverage    mismatched
#           1                  40           53.06%            53
#          16               1,526           86.63%           349
#          64               2,213           85.72%         2,000
#         304               3,826           85.21%        10,025
#
# The scored set GROWS with the object count — it is the union across binaries,
# and no single binary holds all of Core because each one drops what it never
# references. The mismatch count grows at roughly a constant rate per object,
# which is what a duplicate does and what a missing function does not.
#
# That holds for the TOTAL. It does not hold per file when the code is inline in
# a header, and the difference is large enough to invert a reading:
#
#     Source/Core/Rendering/CursorMotion.h
#       its own test's object + profile   118 lines,   6 missed   94.92%
#       this whole-tree report            152 lines, 123 missed   19.08%
#
# `test_cursor_motion` calls update() 37 times across all four modes. The record
# that survived came from an object that includes the header and never runs it —
# a different TU sees a different line set, which is why even the denominators
# disagree. llvm-cov keeps the first record for a name and drops the rest, and
# nothing in the report says which object it kept.
#
# So a per-file number for a header is a statement about one arbitrary TU. To get
# the real one for a single file, run llvm-cov against the object of a test that
# exercises it — Tests/coverage/covering-tests.tsv names those tests:
#
#     llvm-cov report -object <build>/Tests/<test> -instr-profile <that test's profile>
#
# A .cpp is not exposed the same way: its code compiles into one object, so there
# is one record and nothing to choose between.
WARNINGS=$(grep -c '^warning:' "$OUT/summary.txt" 2>/dev/null || true)
if [ "${WARNINGS:-0}" -gt 0 ]; then
    FUNCTIONS=$(awk '/^TOTAL/ { print $5 }' "$OUT/summary.txt")
    grep '^warning:' "$OUT/summary.txt" | sed 's/^/  llvm-cov: /'
    echo "  Those are duplicate records: one function per binary that linked it,"
    echo "  scored once and dropped thereafter. ${FUNCTIONS:-?} function(s) were scored,"
    echo "  which is the union across every object passed and larger than any one"
    echo "  binary holds. The count scales with the object list, not with coverage."
    echo "  The TOTAL is sound. A PER-FILE number for a header is not: the record"
    echo "  kept may come from an object that only includes it — CursorMotion.h"
    echo "  reads 19% here and 95% against its own test. See the note in this"
    echo "  script for how to attribute one file properly."
fi

# The shell tier instruments through run-tests.sh's UBQ_COVERAGE rider and
# cmake wrapper, and the wiring is held to what it promised PER ENTRY: each
# stanza that configured an instrumented build left a marker beside the
# profiles, and an entry with a marker and no profile of its own is the
# wiring regressed for that entry — a tier-level bound would pass on any one
# profile from anywhere, which bounds nothing. Entries
# that compile nothing leave no marker and owe nothing.
SHELL_ENTRIES=$(ctest --test-dir "$BUILD" -N 2>/dev/null |
    sed -n 's/^ *Test *#[0-9]*: *//p' | grep -c '^shell_' || true)
shell_profiles=( "$RAW"/shell_*.profraw )
SHELL_PROFILES=0
[ -e "${shell_profiles[0]}" ] && SHELL_PROFILES=${#shell_profiles[@]}
UNMET_ENTRIES=()
for marker in "$RAW"/shell_*.expects-profile; do
    [ -e "$marker" ] || continue
    entry="$(basename "$marker" .expects-profile)"
    entry_profiles=( "$RAW/$entry"--*.profraw )
    [ -e "${entry_profiles[0]}" ] || UNMET_ENTRIES+=("$entry")
done
if [ ${#UNMET_ENTRIES[@]} -gt 0 ]; then
    echo "run-coverage: ${#UNMET_ENTRIES[@]} shell entry/entries configured an instrumented" >&2
    echo "  build and wrote no profile — the stanza built code this run cannot see," >&2
    echo "  or built it and never ran it:" >&2
    printf '    %s\n' "${UNMET_ENTRIES[@]}" >&2
    exit 1
fi
if [ "${SHELL_ENTRIES:-0}" -gt 0 ] && [ "${SHELL_PROFILES:-0}" -eq 0 ]; then
    echo "run-coverage: ${SHELL_ENTRIES} shell_* entries ran and wrote no profile at all —" >&2
    echo "  the UBQ_COVERAGE rider is not reaching the stanzas, and the report above" >&2
    echo "  silently reverted to the pre-wired blind spot." >&2
    exit 1
fi
echo "  shell tier: ${SHELL_ENTRIES} entry/entries, ${SHELL_PROFILES} profile(s), ${SHELL_OBJECT_COUNT} instrumented binary(ies)"

# The percentage above is a fraction of what this build COMPILED, and this build
# is UBIQUITTY_FRONTEND=Core. A frontend's own sources are not in it, so they
# cannot lower the number and their absence looks like nothing rather than like a
# gap — the shape where a verdict is read as covering what it skipped.
#
# Counted from the tree rather than stated, so the figure cannot rot: an
# implementation file under a frontend that this build does not reach.
UNCOMPILED=$(git -C "$ROOT" ls-files \
    'Source/Platform/macOS/*.mm' 'Source/Platform/macOS/*.cpp' \
    'Source/Platform/Linux/*.cpp' 2>/dev/null | grep -c . || true)
if [ "$ROOT" = "$KIT_ROOT" ]; then
    echo "  subject: Kit native Linux tests and all five built Kit libraries."
    echo "  Core dependencies and Apple/Swift implementations are outside this denominator."
elif [ "${UNCOMPILED:-0}" -gt 0 ]; then
    echo "  subject: the configured Core and dependent test targets. ${UNCOMPILED} frontend"
    echo "  implementation file(s) exist under Source/Platform/{macOS,Linux}; files not"
    echo "  linked by those targets remain outside this report's denominator."
fi
echo "report: $OUT/summary.txt"
echo "lcov  : $OUT/coverage.lcov"
if [ "$ROOT" = "$CORE_ROOT" ] && [ -f "$CORE_ROOT/Tests/coverage/covering-tests.tsv" ]; then
    echo "index : $CORE_ROOT/Tests/coverage/covering-tests.tsv"
fi
[ "${HTML:-0}" = "1" ] && echo "html  : $OUT/html/index.html"

if [ -n "$FLOOR" ]; then
    if awk -v have="$LINE_PCT" -v want="$FLOOR" 'BEGIN { exit !(have + 0 < want + 0) }'; then
        echo "FAIL: line coverage ${LINE_PCT}% is below the ${FLOOR}% floor" >&2
        exit 1
    fi
    echo "ok: ${LINE_PCT}% meets the ${FLOOR}% floor"
fi

# The per-file floors Scripts/check-coverage-floor.sh holds, against the summary
# written above.
if [ "$FILE_FLOORS" = "1" ]; then
    floorArguments=(--source-root "$ROOT" --core-source-root "$CORE_ROOT")
    [ -z "$KIT_ROOT" ] || floorArguments+=(--kit-source-root "$KIT_ROOT")
    if [ "$ROOT" = "$KIT_ROOT" ]; then
        "$ROOT/Scripts/check-coverage-floor.sh" || exit 1
    else
        "$CORE_ROOT/Scripts/check-coverage-floor.sh" "${floorArguments[@]}" || exit 1
    fi
fi

exit "$TESTS_OK"
