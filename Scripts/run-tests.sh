#!/bin/bash
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

#
# run-tests.sh — the host suite's single entry point.
#
# Most of the suite lives in Tests/CMakeLists.txt, which shares Core builds.
# This script hands off to CTest and supplies the shell stanzas for process
# setup, external clients, standalone build contracts, snapshots and fuzzers.
#
# Adding a test means dropping a `test_*.cpp` under Tests/<Subsystem>/. It is
# discovered and registered automatically; nothing here needs an entry. A stanza
# below owns setup or a build contract outside an ordinary unit-test invocation.
#
# Usage:
#   ./Scripts/run-tests.sh                 full CTest suite, then remaining stanzas
#   ./Scripts/run-tests.sh --list-stanzas   list the shell recipe names accepted here
#   ./Scripts/run-tests.sh test_host_tools  run one shell recipe, without CTest
#   ./Scripts/run-tests.sh test_host_tools gate_report
#                                          run a chosen set of shell recipes
#   ./Scripts/run-tests.sh --with-fuzz      full suite plus compile/run fuzzers
#   ./Scripts/run-tests.sh --fuzz-only      compile/run fuzzers only
#   FUZZ_REQUIRED=1 ./Scripts/run-tests.sh --fuzz-only
#                                          fail if a required fuzzer cannot run
#   ./Scripts/run-tests.sh --help           show this help
#
# Shell stanza names and individual CTest case names are different namespaces.
# Filters here select only names printed by --list-stanzas; they never select
# portable Core cases such as test_selection. Configure an explicit CTest tree
# to discover or run those cases (the build fixture builds their binaries):
#
#   CTEST_BUILD_DIR="$(python3 Scripts/build_root.py .)/BuildHostTests/ctest"
#   cmake -S . -B "$CTEST_BUILD_DIR" -G Ninja -DUBIQUITTY_FRONTEND=Core -DUBIQUITTY_BUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Debug
#   ctest --test-dir "$CTEST_BUILD_DIR" -N
#   ctest --test-dir "$CTEST_BUILD_DIR" -R '^test_selection$' --output-on-failure
#
# Set CTEST_BUILD_DIR to reuse a custom test tree. Otherwise the driver uses
# UBQ_OUT/ctest, with UBQ_OUT defaulting to the checkout's external build root
# plus BuildHostTests. Unavailable optional fuzz toolchains/runtimes print SKIP
# with reasons; FUZZ_REQUIRED=1 makes a skipped required harness fail the run.
# A harness that does not compile or link is BUILD-BROKEN and fails the run
# everywhere.
#
# Coverage is not here. Scripts/run-coverage.sh drives it through the CMake
# driver, which instruments the Core and the tests together — the only way
# llvm-cov attributes a Core line to the test that ran it.
#
# Filter arguments match bare shell stanza names (no path or source suffix).
# The default runs the CTest scheduler before the shell remainder; optional
# fuzzers require --with-fuzz or --fuzz-only.

set -euo pipefail
source "$(dirname "$0")/gate-report-lib.sh"

# An attach bridge reads SSH_CONNECTION to learn that it relays a client from
# SSH, and gates that client's sessions as a remote client's. A runner launched
# over ssh would otherwise make every bridge a test spawns look remote; a test
# that models an SSH attach sets the variable itself.
unset SSH_CONNECTION SSH_CLIENT SSH_TTY

# This driver's checkout, and the checkout whose stanzas it runs. They are one tree,
# except where this is ubiquitty-core's copy in another repository's Libraries/ and
# the driver is run from that repository: its stanzas run, written against this
# driver's helpers, and ubiquitty-core's own are not. UBQ_TESTS_ROOT names the
# checkout explicitly.
CORE_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ROOT="${UBQ_TESTS_ROOT:-}"
if [ -z "$ROOT" ]; then
    ROOT="$CORE_ROOT"
    ubq_toplevel="$(git rev-parse --show-toplevel 2>/dev/null || true)"
    if [ -n "$ubq_toplevel" ] && [ "$ubq_toplevel" != "$CORE_ROOT" ] \
        && [ "$(cd "$ubq_toplevel/Libraries/ubiquitty-core" 2>/dev/null && pwd -P)" = "$(cd "$CORE_ROOT" && pwd -P)" ]; then
        ROOT="$ubq_toplevel"
    fi
fi
# The libraries the stanzas of the checkout under test name files in; each is the
# checkout itself where it holds them.
KIT_ROOT="${UBQ_KIT_ROOT:-$ROOT}"
if [ -z "${UBQ_KIT_ROOT:-}" ] && [ -e "$ROOT/Libraries/ubiquitty-kit" ]; then
    KIT_ROOT="$ROOT/Libraries/ubiquitty-kit"
fi

# How many compilers this machine can run at once; see Scripts/build-jobs.sh.
# shellcheck source=Scripts/build-jobs.sh
. "$CORE_ROOT/Scripts/build-jobs.sh"
# Build output lives outside the checkout; see Scripts/build-root.sh.
# shellcheck source=Scripts/build-root.sh
. "$CORE_ROOT/Scripts/build-root.sh"
# The helpers a stanza calls: wanted, ubq_skip, ubq_build_test.
# shellcheck source=Scripts/run-tests-lib.sh
. "$CORE_ROOT/Scripts/run-tests-lib.sh"

WITH_FUZZ=0
FUZZ_ONLY=0
FILTERS=()
# Which filters a stanza answered, space-delimited with sentinels so a `case`
# match cannot hit a substring. A string rather than an associative array
# because macOS ships bash 3.2, where `declare -A` is a syntax error — and this
# script runs there more than anywhere else.
FILTERS_MATCHED=" "

usage() {
    sed -n '/^#!/d; /^[^#]/q; s/^# \{0,1\}//; p' "$0"
}

# This driver and the stanza files of the repositories the checkout holds, which
# are the places a stanza is declared. A repository's own stanzas are
# Scripts/run-tests-stanzas-<repository>.sh.
UBQ_STANZA_FILES=()
[ "$ROOT" = "$CORE_ROOT" ] && UBQ_STANZA_FILES=("$0")
for ubq_stanza_file in "$ROOT"/Scripts/run-tests-stanzas-*.sh; do
    [ -f "$ubq_stanza_file" ] && UBQ_STANZA_FILES+=("$ubq_stanza_file")
done

ubq_list_stanzas() {
    # Only host-supported names become CTest entries or accepted filters.
    awk -v mode=list -v platform="$(uname -s)" \
        -f "$CORE_ROOT/Scripts/stanza-declarations.awk" \
        -f "$CORE_ROOT/Scripts/stanza-inventories.awk" ${UBQ_STANZA_FILES[@]+"${UBQ_STANZA_FILES[@]}"} </dev/null | LC_ALL=C sort -u
}

for arg in "$@"; do
    case "$arg" in
        --help|-h) usage; exit 0 ;;
        --with-fuzz) WITH_FUZZ=1 ;;
        --fuzz-only) WITH_FUZZ=1; FUZZ_ONLY=1 ;;
        # Every name this script can be filtered to, one per line. CMake reads
        # it to give each stanza its own ctest entry, so the list has one owner
        # — the script that dispatches on it — rather than a copy in the build
        # that goes stale the first time a stanza is added.
        # The stanzas that configure or build into a shared CMake directory —
        # build/host, build/host-quic and friends. Two of them running at once
        # race on the same object tree, and adopt_session derives its socket
        # name from the binary they both write, so the loser talks to the
        # winner's daemon. ctest serializes them against each other by RESOURCE
        # LOCK while the other 48 stay parallel.
        --list-serial-stanzas)
            serial_list=$(mktemp)
            # `<stanza> <directory>` for every stanza that configures or builds
            # into a tree under build/. The caller locks per directory, so
            # stanzas on different trees still run beside each other.
            #
            # Any directory, not a list of known ones. An enumeration has to be
            # maintained by whoever adds the next tree, and nothing tells them:
            # a stanza using an unlisted directory is silently left unserialized,
            # races on compiler inspection under CMakeFiles/, and surfaces as an
            # occasional red that reads as flakiness, however heavily the
            # tree is used.
            #
            # The stanza's WHOLE body is scanned, bounded by the next stanza
            # rather than by a line count, for the same reason: a fixed window
            # decides the answer by how long the stanza happens to be.
            # A tree named through a variable is the same tree. Matching the
            # literal path alone sees `$UBQ_BUILD_ROOT/host-quic` and misses
            # `$HOST_TEST_BUILD`, which is assigned once at the top of this file
            # and used by many stanzas — so they would be left unserialized while
            # reading as covered, which is this list's own documented failure
            # mode arriving through the spelling rather than through the
            # directory. The assignments are collected first and the references
            # resolved against them.
            awk -v mode=serial -f "$CORE_ROOT/Scripts/stanza-declarations.awk" \
                -f "$CORE_ROOT/Scripts/stanza-inventories.awk" ${UBQ_STANZA_FILES[@]+"${UBQ_STANZA_FILES[@]}"} </dev/null | LC_ALL=C sort -u >"$serial_list"
            # A safety list that silently empties is worse than no list: every
            # lock disappears and everything downstream still reports success.
            # A detector that goes blind when build output moves says nothing.
            # This file plainly contains stanzas that share a tree, so finding
            # none means the detector stopped seeing them, not that they stopped
            # existing.
            # Only ubiquitty-core's own stanzas are known to share trees; another
            # repository's may share none.
            if [ ! -s "$serial_list" ] && [ "$ROOT" = "$CORE_ROOT" ]; then
                rm -f "$serial_list"
                echo "run-tests.sh --list-serial-stanzas: found NO shared build trees." >&2
                echo "  The detector matches assignments of the form VAR=\"\$<something>ROOT/[build/]<dir>\"." >&2
                echo "  Either this file no longer has any, or the build-root spelling moved again" >&2
                echo "  and the detector did not follow. Serializing nothing lets stanzas race on a" >&2
                echo "  shared object tree, which surfaces as flakiness anywhere in the suite." >&2
                exit 1
            fi
            cat "$serial_list"
            rm -f "$serial_list"
            exit 0 ;;
        --list-host-binary-stanzas)
            # Stanzas that run against the built Host frontend. The caller makes
            # them require the fixture that builds it, and tells them it is
            # already there.
            #
            # Derived from the call rather than listed, so a stanza that starts
            # using the binary is covered by the act of using it. A list here
            # would be a second place to update, and the stanza that forgot it
            # would build the tree underneath the ones reading it. An agent
            # stanza's daemon is this binary unless its caller names another.
            #
            # The call counts where it is written: from a stanza's opening to the
            # next one. A stanza handed the binary by an enclosing block or a
            # variable another stanza set is not ordered after the fixture, and
            # under ctest ubq_refuse_unordered_build stops it building one.
            awk -v mode=host -f "$CORE_ROOT/Scripts/stanza-declarations.awk" \
                -f "$CORE_ROOT/Scripts/stanza-inventories.awk" ${UBQ_STANZA_FILES[@]+"${UBQ_STANZA_FILES[@]}"} </dev/null | LC_ALL=C sort -u
            exit 0 ;;
        --list-agent-binary-stanzas)
            # Stanzas that run the built agent, derived from the call exactly
            # as the Host list above is, and bounded the same way.
            awk -v mode=agent -f "$CORE_ROOT/Scripts/stanza-declarations.awk" \
                -f "$CORE_ROOT/Scripts/stanza-inventories.awk" ${UBQ_STANZA_FILES[@]+"${UBQ_STANZA_FILES[@]}"} </dev/null | LC_ALL=C sort -u
            exit 0 ;;
        --list-quic-dependency-stanzas)
            # Stanzas that decide whether they can run by inspecting the shared
            # QUIC archives. CTest makes them wait for the dependency fixture;
            # deriving the list from the probe keeps a newly added consumer
            # from racing the cold-cache build.
            awk -v mode=quic -f "$CORE_ROOT/Scripts/stanza-declarations.awk" \
                -f "$CORE_ROOT/Scripts/stanza-inventories.awk" ${UBQ_STANZA_FILES[@]+"${UBQ_STANZA_FILES[@]}"} </dev/null | LC_ALL=C sort -u
            exit 0 ;;
        --list-stanzas)
            ubq_list_stanzas
            exit 0 ;;
        --*) echo "run-tests.sh: unknown flag: $arg" >&2; exit 2 ;;
        *) FILTERS+=("$arg") ;;
    esac
done

# Report — never refuse — whether this clone's git hooks will actually gate it.
# A clone whose hooks are absent, stale, or point at a directory with no
# pre-push pushes with nothing checking it, and the gate it skips is the one
# that would have caught the mistake. This preamble is the one home that runs
# even for a fully ungated clone, where a hook-based check by definition cannot.
# It reports and continues rather than refusing: a contributor who finds it is ungated
# is usually mid-task, and stopping it there trades a silent problem for a
# blocked one without making the fix any likelier.
# One report per process tree, not one per nested invocation: ctest re-enters
# this script once per shell_* entry, and a second identical line says nothing
# the first did not. Exported, so the children
# inherit the answer rather than each asking again.
if [ -z "${UBQ_HOOKS_REPORTED:-}" ]; then
    export UBQ_HOOKS_REPORTED=1
    python3 "$CORE_ROOT/Scripts/check-hooks-installed.py" >&2 || true
fi

# Where a stanza puts its objects and its binary. Concurrent runs need separate
# directories: two stanzas compiling the same Core source write the same object
# name, and the loser links a half-written file. UBQ_OUT lets ctest give each
# entry its own — the driver is one process per stanza there, so a shared
# directory is a race by construction.
#
# The default stays BuildHostTests so a hand-run keeps its incremental objects
# and its logs where the failure messages say they are.
OUT="${UBQ_OUT:-$UBQ_BUILD_ROOT/BuildHostTests}"
CTEST_BUILD_DIR="${CTEST_BUILD_DIR:-$OUT/ctest}"

# What this run may leave behind: nothing. A stanza's daemon that outlives it,
# and a file in the state directory of whoever runs the suite, are both found
# and both fail the run; Scripts/test-run-hygiene.sh says how and why.
# shellcheck source=Scripts/test-run-hygiene.sh
. "$CORE_ROOT/Scripts/test-run-hygiene.sh"
ubq_hygiene_begin

CTEST_FAILED=0
# The ctest verdict on every exit path, not only the one that reaches the end.
# `set -e` means the first failing stanza leaves this script, and the report
# below the stanzas then never runs — which is how a job with six failing ctest
# entries ended on a line about an unrelated gate and named none of them. The
# entries themselves are printed by ubq_run_ctest as ctest returns. Defined
# ahead of the trap that calls it, because an unknown stanza name exits the
# script before the stanzas are reached.
ubq_report_ctest_verdict() {
    [ "${CTEST_FAILED:-0}" -eq 0 ] && return 0
    echo "run-tests.sh: ctest reported failures (the entries are named above)" >&2
}

# ubq_end_run — the run's last act on every path that reaches a trap.
#
# A completed run (EXIT) is judged: what it left running is named and fails it,
# as does an entry the real state directory gained. An interrupted one is only
# cleaned up; its status already says it did not finish.
ubq_end_run() {
    local status=$? how="$1"
    trap - EXIT INT TERM HUP
    if [ "$how" = completed ]; then
        ubq_report_ctest_verdict || true
        ubq_hygiene_judge run-tests.sh || status=1
    fi
    ubq_hygiene_reap || status=1
    [ "$how" = completed ] || status=$((128 + how))
    exit "$status"
}
trap 'ubq_end_run completed' EXIT
trap 'ubq_end_run 2' INT
trap 'ubq_end_run 15' TERM
trap 'ubq_end_run 1' HUP

ubq_report_unknown_filters() {
    {
        printf 'run-tests.sh: not a stanza name:'
        printf ' %q' "$@"
        printf '\n  Shell recipe names: ./Scripts/run-tests.sh --list-stanzas\n'
        printf '  Individual CTest cases use a different namespace. Configure the test tree explicitly:\n'
        printf '    cmake -S %q -B %q -G Ninja -DUBIQUITTY_FRONTEND=Core -DUBIQUITTY_BUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Debug\n' "$ROOT" "$CTEST_BUILD_DIR"
        printf '  Discover its cases, then select an exact case name:\n'
        printf '    ctest --test-dir %q -N\n' "$CTEST_BUILD_DIR"
        printf "    ctest --test-dir %q -R '^<ctest-name>\$' --output-on-failure\n" "$CTEST_BUILD_DIR"
    } >&2
}

# Validate the whole request before cache cleanup or any selected recipe runs.
# A valid name beside an invalid one must not trigger a partial execution.
if [ "${#FILTERS[@]}" -gt 0 ]; then
    ubq_stanzas="$(ubq_list_stanzas)"
    ubq_unmatched=()
    for ubq_filter in "${FILTERS[@]}"; do
        case "$ubq_filter" in
            *[!a-zA-Z_0-9-]*|'') ubq_unmatched+=("$ubq_filter") ;;
            *)
                # Drain the inventory so pipefail cannot mistake SIGPIPE for a missing name.
                if ! printf '%s\n' "$ubq_stanzas" | grep -xF -- "$ubq_filter" >/dev/null; then
                    ubq_unmatched+=("$ubq_filter")
                fi
                ;;
        esac
    done
    if [ "${#ubq_unmatched[@]}" -gt 0 ]; then
        ubq_report_unknown_filters "${ubq_unmatched[@]}"
        exit 1
    fi
fi

# The host daemon the stanzas below build and drive. It is a separate directory
# from build/host because build-host.sh configures that path Release while these
# stanzas need Debug for the asserts, and one CMake cache holds one build type:
# sharing the path means whichever ran last decides what build/host/ubiquitty-host
# is, and nothing at the point of use announces which one you got. A -O0 daemon
# is indistinguishable from a slow one across the wire, so the cost surfaces as
# frame rate rather than as a wrong binary. build/host-noquic and build/host-quic
# separate the same cache over a different configure flag.
#
# Stanzas assign HOST_BUILD from this rather than using it directly, because the
# variant stanzas point HOST_BUILD at their own directory and a later stanza
# reading a leaked value would drive the wrong daemon.
#
# UBQ_HOST_TEST_BUILD names another tree, for a driver that built the daemon
# itself: Scripts/run-sanitized-tests.sh points the stanzas that run only the
# binary at an instrumented one this way, with UBQ_HOST_PREBUILT=1.
HOST_TEST_BUILD="${UBQ_HOST_TEST_BUILD:-$UBQ_BUILD_ROOT/host-tests}"

# Remaining standalone compiles use the same compiler launcher as CMake.
#
# pch_defines,time_macros because the tests define __DATE__-adjacent macros the
# default sloppiness refuses to cache; without it the hit rate on this tree is
# near zero and ccache costs more than it saves.
# How many compilations one stanza may run at once.
#
# `cmake --build -j` with no number hands ninja its default, which is cores + 2.
# ctest runs up to eight stanzas at a time, so on a fifteen-core machine the
# suite would ask for 136 concurrent clang processes and the machine stops
# responding. Concurrent builds on a shared machine multiply it again.
#
# One generator for every directory this script configures — host-tests, host-quic,
# host-noquic — and the same one Scripts/build-host.sh picks for build/host. A
# CMake cache records the generator that created it and refuses a different one
# with "generator Ninja does not match Unix Makefiles", which is a configure
# failure rather than a regenerate; ubq_reset_on_generator_change wipes the
# directory instead of letting a run die on it.
#
# Set before the branch below rather than inside it. Placed in one arm it is unset
# in the other two — which is every path that matters, since ctest takes the UBQ_OUT
# arm and a hand run takes the else — and under `set -u` the first stanza to reach a
# cmake line aborts on an unbound variable instead of defaulting.
UBQ_GENERATOR="Unix Makefiles"
command -v ninja >/dev/null 2>&1 && UBQ_GENERATOR="Ninja"
# Asserted here, where the value is promised, rather than discovered at the first of
# ~37 use sites. A stanza aborting at its cmake line names the symptom; this names
# the cause, and it fails before any stanza has done work.
: "${UBQ_GENERATOR:?run-tests.sh: the generator was not resolved}"

# A build directory configured with a different generator cannot be reused: CMake
# refuses with "does not match the generator used previously" and the stanza fails
# at configure, naming a build error for a tree it did not break. A host tree
# configured with Unix Makefiles before a stanza passed -G is such a directory,
# and without this the clash would need everyone to delete it by hand.
#
# Removing a build directory is safe in a way that keeping a stale one is not:
# the only thing lost is compile time, and the only thing kept is a tree nothing
# can configure.
ubq_reset_on_generator_change() { # <build directory>
    local dir="$1" cache="$1/CMakeCache.txt" previous
    [ -f "$cache" ] || return 0

    # A cross build is not this script's to reset. emcmake configures
    # $UBQ_BUILD_ROOT/wasm and $UBQ_BUILD_ROOT/web through a toolchain file,
    # with em++ and without -G, so both the compiler and the generator differ
    # from anything a host run wants -- and neither directory is configured by
    # a stanza here. Removing one would delete a build this run cannot rebuild,
    # from a line that promises the owner rebuilds it, which is the disappearance
    # the note above this sweep already records. A toolchain file is the mark of
    # a build configured for another target, so it is left to its owner.
    if grep -q "^CMAKE_TOOLCHAIN_FILE:" "$cache" 2>/dev/null; then
        return 0
    fi

    # A cache records the ABSOLUTE path it was configured at, and CMake refuses
    # to reuse one that names anywhere else. run-linux.sh bind-mounts the working
    # tree at /src, so every shared tree the host filled reaches the container
    # under a different path and every stanza reusing it fails at configure —
    # naming the test rather than the tree it inherited.
    previous="$(sed -n 's/^CMAKE_CACHEFILE_DIR:INTERNAL=//p' "$cache" | head -1)"
    # Both sides resolved: a tree configured through a symlinked spelling
    # (/tmp for /private/tmp) is the same tree, not a moved one.
    previousResolved="$( (cd "$previous" 2>/dev/null && pwd -P) || printf '%s' "$previous")"
    if [ -n "$previous" ] && [ "$previousResolved" != "$(cd "$dir" && pwd -P)" ]; then
        echo "run-tests.sh: $dir was configured at $previous, reconfiguring it here" >&2
        rm -rf "$dir"
        return 0
    fi

    # CMake reads CC and CXX on the first configure and records the answer; every
    # later configure of the same directory uses what it recorded and ignores the
    # environment. A runner that keeps its build directories between runs then
    # compiles a coverage build with whichever compiler happened to configure it
    # first, and clang-only flags reach GCC: "unrecognized command-line option
    # -fprofile-instr-generate". That reads as a broken stanza rather than as a
    # directory inherited from another run.
    # The type is matched as a class rather than an alternation: \| is a GNU sed
    # extension, and the BSD sed on macOS matches nothing at all through it --
    # which is a check that never fires rather than one that fails.
    previous="$(sed -n 's/^CMAKE_CXX_COMPILER:[A-Z]*=//p' "$cache" | head -1)"
    if [ -n "$previous" ] && [ -n "${CXX:-}" ]; then
        ubq_wanted_cxx="$(command -v "$CXX" 2>/dev/null || printf '%s' "$CXX")"
        if [ "$previous" != "$ubq_wanted_cxx" ]; then
            echo "run-tests.sh: removed $dir — configured with $previous, and this run" >&2
            echo "  compiles with $ubq_wanted_cxx. Whichever script owns it reconfigures it." >&2
            rm -rf "$dir"
            return 0
        fi
    fi

    previous="$(sed -n 's/^CMAKE_GENERATOR:INTERNAL=//p' "$cache" | head -1)"
    [ -n "$previous" ] || return 0
    [ "$previous" = "$UBQ_GENERATOR" ] && return 0
    # Says removed, because removed is what happens. The rebuild belongs to
    # whichever stanza owns the directory, and a directory this sweep reaches
    # that no stanza owns is never rebuilt at all — a line promising it back
    # would be false on every run.
    echo "run-tests.sh: removed $dir — configured with $previous, and every build" >&2
    echo "  directory here is $UBQ_GENERATOR. Whichever script owns it reconfigures it;" >&2
    echo "  if none does, it is gone until someone runs that script by hand." >&2
    rm -rf "$dir"
}
# Every configured tree under build/, not a list of them. A list is a second
# place to remember: a tree every Host stanza links against can be absent from
# it, and those stanzas then report BUILD-BROKEN with the cache error in a log
# the summary does not print. A stanza that adds a build
# directory inherits the check instead of needing an edit here.
# Both locations are swept. Build output is keyed to the checkout and kept
# outside it, which is where every directory a stanza configures now lands; the
# in-tree path stays because a tree bind-mounted at /src by run-linux.sh builds
# inside itself, and that is the case the cache-path check above exists for.
if [ "${UBQ_CACHE_SWEEP_COMPLETE:-0}" != "1" ]; then
    for ubq_cache in "$UBQ_BUILD_ROOT"/*/CMakeCache.txt "$ROOT"/build/*/CMakeCache.txt; do
        [ -f "$ubq_cache" ] || continue
        ubq_reset_on_generator_change "${ubq_cache%/CMakeCache.txt}"
    done
    export UBQ_CACHE_SWEEP_COMPLETE=1
fi

# Under ctest the scheduler is already the thing spreading work across cores, so
# each stanza takes a small slice; run by hand there is nothing else competing
# and the full machine is right. UBQ_BUILD_JOBS overrides either way.
if [ -n "${UBQ_BUILD_JOBS:-}" ]; then
    BUILD_JOBS="$UBQ_BUILD_JOBS"
elif [ -n "${UBQ_OUT:-}" ]; then
    BUILD_JOBS=2
else
    BUILD_JOBS="$(ubq_build_jobs)"
fi

# CC and CXX name a compiler, and every consumer invokes them as one word — the
# quoting that is right for a path with a space is wrong for two words, so a
# wrapper folded into them resolves as a binary called "ccache c++". ccache
# rides in UBQ_COMPILER_LAUNCHER instead, expanded unquoted beside the compiler,
# which is the split CMAKE_CXX_COMPILER_LAUNCHER already uses in
# Scripts/cmake/UbiquittyPreProject.cmake.
if command -v ccache >/dev/null 2>&1; then
    export CCACHE_SLOPPINESS="${CCACHE_SLOPPINESS:-pch_defines,time_macros}"
    UBQ_COMPILER_LAUNCHER="${UBQ_COMPILER_LAUNCHER-ccache}"
else
    UBQ_COMPILER_LAUNCHER="${UBQ_COMPILER_LAUNCHER-}"
fi
export UBQ_COMPILER_LAUNCHER
CC="${CC:-cc}"
CXX="${CXX:-c++}"
# Launcher plus compiler, ready to invoke. Two words when ccache is present, so
# every use is unquoted; CC and CXX stay one word for the callers that quote
# them. Compile through these, never through CC/CXX directly — a bare $CXX_TEST
# still runs, just uncached, and nothing reports the loss.
CC_LAUNCHED="${UBQ_COMPILER_LAUNCHER:+$UBQ_COMPILER_LAUNCHER }$CC"
CXX_TEST="${UBQ_COMPILER_LAUNCHER:+$UBQ_COMPILER_LAUNCHER }$CXX -I$CORE_ROOT/Source/ThirdParty/simdutf"

# The simdutf include path rides with the compiler rather than with each stanza's
# own flag list. Core/Codec/Base64.cpp and Core/Unicode/UTF8Decoder.cpp include
# `simdutf.h` unconditionally. Keeping the include with the compiler covers
# every standalone and instrumented recipe that uses those sources.

# Under the coverage driver every stanza's compile instruments, or the shell_*
# entries run and write nothing. The flag rides with the compiler for the reason
# the simdutf include
# does: one place here, not one place per stanza to forget. The same switch
# covers the link, because clang's -fprofile-instr-generate is both.
# -runtime-counter-relocation rides with them for the reason
# Scripts/cmake/UbiquittyToolchain.cmake states at its own copy: the shell tier's LLVM_PROFILE_FILE carries %c, and on
# ELF continuous mode needs the bias symbol this flag emits. A stanza compiled
# here without it prints "LLVM Profile Error: __llvm_profile_counter_bias is
# undefined" from every instrumented process it starts — including the ones
# whose output a test reads.
#
# -fprofile-update=atomic rides with them because a relocated counter is one
# ThreadSanitizer cannot recognise. TSan exempts a profile counter by matching
# the __llvm_prf_cnts section on the global an instrumented store addresses;
# addressed instead through __llvm_profile_counter_bias, it presents no such
# global, the exemption misses, and the plain load-add-store of a counter in any
# function two threads enter reads as an eight-byte data race. An atomic update
# is one TSan models, and it never reports a race between two atomic accesses.
UBQ_COVERAGE_LINK_FLAGS=""
if [ "${UBQ_COVERAGE:-0}" = "1" ]; then
    UBQ_COVERAGE_LINK_FLAGS="-fprofile-instr-generate"
    UBQ_COVERAGE_FLAGS="$UBQ_COVERAGE_LINK_FLAGS -fcoverage-mapping -fprofile-update=atomic -mllvm -runtime-counter-relocation"
    CXX_TEST="$CXX_TEST $UBQ_COVERAGE_FLAGS"
    CC_LAUNCHED="$CC_LAUNCHED $UBQ_COVERAGE_FLAGS"
fi

# The compiler for an invocation whose inputs are all objects. It compiles
# nothing, so a backend flag on it is claimed by no compilation: clang raises
# -Wunused-command-line-argument, and a stanza that also passes -Werror gets a
# link failure reading `argument unused during compilation: '-mllvm
# -runtime-counter-relocation'`. This form carries only what a link needs --
# -fprofile-instr-generate, which selects the profile runtime.
# Scripts/cmake/UbiquittyToolchain.cmake draws the same line between
# add_compile_options and add_link_options.
CXX_LINK="${UBQ_COMPILER_LAUNCHER:+$UBQ_COMPILER_LAUNCHER }$CXX $UBQ_COVERAGE_LINK_FLAGS"

# Records that this entry configured an instrumented build, so run-coverage.sh
# can hold it to a profile PER ENTRY. The tier-level form of that assertion is
# satisfiable by any one profile from anywhere — the vacuous shape — and a
# static expectation list rots; a marker written at the configure that creates
# the obligation is derived from what actually happened. The name comes off
# LLVM_PROFILE_FILE's entry prefix, so writer and reader agree by construction.
ubq_note_expected_profile() {
    # A build fixture compiles for other entries and runs nothing itself; its
    # obligation is discharged by the profiles of the stanzas that consume it.
    [ "${UBQ_COVERAGE_BUILD_FIXTURE:-0}" = "1" ] && return 0
    case "${LLVM_PROFILE_FILE:-}" in
    *--%c%p.profraw | *--%p.profraw) : >"${LLVM_PROFILE_FILE%--*}.expects-profile" ;;
    esac
}

# The prefix the QUIC stanzas link and every nested configure is given. Assigned
# here rather than beside the archive list it heads, because the cmake() wrapper
# below reads it and the ctest section calls that wrapper long before the list is
# reached: under `set -u` the script would die on the first configure it ran. The
# reasoning for the value, and what goes wrong when the link and the configure
# disagree about it, is with the archive list.
QUIC_PREFIX="${UBIQUITTY_QUIC_PREFIX:-$UBQ_BUILD_ROOT/quic-deps}"

# Every nested configure in this script funnels through here, so the coverage
# knob reaches the ~31 `cmake -B` sites without each having to remember it —
# including a stanza not yet written. The knob is passed explicitly in
# both directions because the Host build roots (build/host-tests, build/host-quic) are
# shared caches outside UBQ_OUT: a coverage run flips them ON, and a normal
# run that stayed silent would inherit instrumented binaries from the cache
# and never say so. `cmake --build` and `cmake -E` take no -D and pass
# through untouched.
#
# The QUIC prefix rides along for the same reason and one more.
# Scripts/cmake/UbiquittyToolchain.cmake declares UBIQUITTY_QUIC_PREFIX as a
# cache path, and cmake does not seed a cache entry from the environment
# variable of the same name: a configure that does not name it takes the build
# root's prefix whatever the environment says. Every stanza then links
# $QUIC_PREFIX, the environment's, against a tree configured for the default —
# the drift QUIC_PREFIX's own comment below claims the environment prevents.
#
# On a box whose build root sits outside the checkout it is worse than drift.
# Finding the default prefix empty, each configure runs fetch-quic-deps.sh to
# build AWS-LC there, and ctest locks a stanza only against the others sharing
# its build directory. These trees differ, so a dozen of them fetch into one
# default prefix at once and tear each other apart: a compiler that
# stops being detected mid-configure, an aws-lc/build directory that will not
# delete because another writer is in it, a dependency file that vanishes
# between opening and reading. Twenty stanzas reported a broken build, which
# asserts nothing. Passing the prefix here leaves configure and link naming one
# prefix by construction.
ubq_prebuilt_cmake() {
    local tree="" previous="" argument expected_quic="" enabled=""
    for argument in "$@"; do
        if [ "$previous" = "-B" ] || [ "$previous" = "--build" ]; then
            tree="$argument"
            break
        fi
        case "$argument" in
            -B?*) tree="${argument#-B}"; break ;;
        esac
        previous="$argument"
    done
    case "$tree" in
        "$UBQ_BUILD_ROOT/host-noquic")
            enabled="${UBQ_HOST_NOQUIC_PREBUILT:-0}"
            expected_quic=ON
            ;;
        "$UBQ_BUILD_ROOT/host-quic")
            enabled="${UBQ_HOST_QUIC_PREBUILT:-0}"
            expected_quic=ON
            ;;
        *) return 1 ;;
    esac
    [ "$enabled" = "1" ] || return 1
    if [ ! -x "$tree/ubiquitty-host" ] \
            || [ ! -d "$tree/CMakeFiles/ubiquitty-host.dir" ] \
            || ! grep -q '^UBIQUITTY_FRONTEND:STRING=Host$' "$tree/CMakeCache.txt" 2>/dev/null \
            || ! grep -q "^UBIQUITTY_QUIC:BOOL=$expected_quic$" "$tree/CMakeCache.txt" 2>/dev/null; then
        echo "run-tests.sh: prebuilt Host fixture is incomplete or has the wrong configuration: $tree" >&2
        return 2
    fi
    return 0
}

cmake() {
    local prebuilt_status
    if ubq_prebuilt_cmake "$@"; then
        case "${1:-}" in
        --build | -E) ;;
        *) ubq_note_expected_profile ;;
        esac
        return 0
    else
        prebuilt_status=$?
        [ "$prebuilt_status" -eq 1 ] || return "$prebuilt_status"
    fi
    case "${1:-}" in
    --build | -E)
        command cmake "$@"
        ;;
    *)
        if [ "${UBQ_COVERAGE:-0}" = "1" ]; then
            ubq_note_expected_profile
            command cmake "$@" -DUBIQUITTY_COVERAGE=ON \
                -DUBIQUITTY_QUIC_PREFIX="$QUIC_PREFIX"
        else
            command cmake "$@" -DUBIQUITTY_COVERAGE=OFF \
                -DUBIQUITTY_QUIC_PREFIX="$QUIC_PREFIX"
        fi
        ;;
    esac
}

# A compile through CC/CXX directly still succeeds — it just skips the launcher,
# so the only symptom is a slower run nobody attributes. Fail loudly instead:
# past this point the compilers are the launched forms, and the raw names are
# left readable so a message can still print one.
compile_through_launched_form_only() {
    echo "run-tests.sh: compile through \$CXX_TEST / \$CC_LAUNCHED, not \$CXX / \$CC." >&2
    echo "  The raw names carry no launcher, so ccache is skipped silently." >&2
    exit 1
}
CFLAGS_BASE="-std=c99 -Wall -Wextra -Werror -Wno-comment"

# Portable code takes its scalar types from Base/Types.h on every host.
# Every stanza searches Tests/ as well, as the CMake targets do, so a test that
# includes "Harness.h" gets the same file whichever of the two drivers builds it.
HOST_COMPAT="-I$ROOT/Tests${CORE_ROOT:+ -I$CORE_ROOT/Tests}"

mkdir -p "$OUT"

# Built-in themes ship as TOML (Source/Core/Themes/builtin-themes.toml) compiled in
# via this generated TU; every stanza that links Themes.cpp links it too.
# Mirrors the CMake add_custom_command so host tests and the app share one
# source of truth.
THEMES_GEN="$OUT/BuiltinThemesData.gen.cpp"
python3 "$CORE_ROOT/Scripts/embed-builtin-themes.py" \
    "$CORE_ROOT/Source/Core/Themes/builtin-themes.toml" "$THEMES_GEN"

# The CTest handoff. Tests/CMakeLists.txt compiles the portable Core once and
# links every test it registers against that copy, where a stanza here compiles
# its own ~47 Core sources. Across a whole run the stanzas would repeat some
# 2,500 compilations, about 25 minutes against ctest's 8 seconds or so.
#
# A full run therefore gives ctest everything it registers, and a stanza runs
# only for what has not moved over: the frontend tests and gates such as
# libcore_consumer, the snapshot harness and the fuzzers. The configure step
# writes the registered names to ctest-covered.txt and nobody edits that file,
# so no test is run by both drivers or dropped by both.
# shellcheck disable=SC2034  # read by wanted in run-tests-lib.sh
CTEST_COVERED="$CTEST_BUILD_DIR/ctest-covered.txt"
: "${CTEST_JOBS:=8}"
# Whether the covered list decided anything this run. SKIP_CTEST_COVERED reads
# as protection; on a filtered run it is inert, because an explicit filter
# answers before the list is opened.
CTEST_COVERED_CONSULTED=0

# The stanzas of the checkout under test, which `wanted` holds a driver run from
# a repository carrying this one to.
# shellcheck disable=SC2034  # wanted, in run-tests-lib.sh, reads it
UBQ_OWN_STANZAS="$(ubq_list_stanzas)"

# Every source path this script names, checked before the first compile.
#
# The link sets here are maintained by hand, so a source that moves is invisible
# until the one stanza naming it reaches clang and reports "no such file or
# directory" a minute into the run, without saying which list is stale. A stanza
# that CTest covers is skipped entirely, so its copy of the list can rot for
# weeks before anyone runs the shell driver alone. One stat per path names the
# file instead.
#
# Scanning this script rather than the arrays catches the per-stanza lists too,
# which is where most of the drift lands.
#
# A stanza whose subject lives in a repository that depends on this one names a
# source under one of that repository's trees. Those trees are listed here, and
# a path under a listed tree that this checkout does not carry is that
# repository's to verify; every other missing path is stale.
verify_source_paths() {
    local isMissing=0 path tree isDependent named=()
    # This driver's own text names ubiquitty-core's sources, which are the
    # checkout under test's to verify only when it is ubiquitty-core.
    [ "$ROOT" = "${CORE_ROOT:-$ROOT}" ] && named+=("$0")
    local dependentRepositoryTrees=(Source/Platform/macOS/App Source/UbiquittyRenderKit)
    while IFS= read -r path; do
        [ -f "$ROOT/$path" ] && continue
        [ -f "${CORE_ROOT:-$ROOT}/$path" ] && continue
        [ -f "${KIT_ROOT:-$ROOT}/$path" ] && continue
        isDependent=0
        for tree in "${dependentRepositoryTrees[@]}"; do
            case "$path" in "$tree"/*) [ -d "$ROOT/$tree" ] || isDependent=1 ;; esac
        done
        [ "$isDependent" -eq 1 ] && continue
        echo "run-tests.sh: names a source that does not exist: $path" >&2
        isMissing=1
    # Both spellings: the link sets write "$ROOT/Source/...", and the CMake
    # EXCLUDE mirrors write the repo-relative path. A pattern matching only the
    # first reads past a stale entry in the second, and a stale entry in an
    # EXCLUDE list is worse than one in a link set — the excluded file quietly
    # comes back, so the fuzz link stops matching the daemon it stands in for.
    done < <(grep -ohE '(\$(CORE_|KIT_)?ROOT/)?Source/[A-Za-z0-9_/.-]+\.(cpp|mm|swift|cc|c)' \
                 ${named[@]+"${named[@]}"} ${UBQ_STANZA_FILES+"${UBQ_STANZA_FILES[@]}"} \
             | sed -E 's#^\$(CORE_|KIT_)?ROOT/##' | sort -u)
    [ "$isMissing" -eq 0 ] || {
        echo "run-tests.sh: a source moved and a link set did not follow it." >&2
        echo "  git knows where each went; ask it, and let it rewrite them:" >&2
        echo "    ./Scripts/where-did-it-go.sh --fix Scripts/run-tests.sh" >&2
        exit 2
    }
}
verify_source_paths

# ubq_configure_ctest_tree DIR — configures DIR as the CMake-covered tests' tree.
#
# The build type is named because an unnamed one is not neutral:
# Scripts/cmake/UbiquittyToolchain.cmake FORCEs an empty CMAKE_BUILD_TYPE to
# Release, and Release defines NDEBUG, which changes sizeof(CellGrid) (CellGrid
# declares `m_paintInvariantBroken` inside #ifndef NDEBUG). Unnamed, whichever
# driver reaches a wiped directory first decides whether the suite asserts.
ubq_configure_ctest_tree() {
    cmake -S "$ROOT" -B "$1" -G Ninja \
        -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
        -DUBIQUITTY_FRONTEND=Core -DUBIQUITTY_BUILD_TESTS=ON \
        -DCMAKE_BUILD_TYPE=Debug
}

# Configures the CMake test tree and hands it to ctest, which builds and runs
# every test registered there.
run_ctest_suite() {
    echo "=== ctest (CMake-covered tests) ==="
    ubq_reset_on_generator_change "$CTEST_BUILD_DIR"
    ubq_configure_ctest_tree "$CTEST_BUILD_DIR" >/dev/null || { echo "ctest handoff: configure failed" >&2; return 1; }
    # No build step here: the ubq_build fixture compiles before any entry runs
    # and fails the whole run when the compile fails, so a binary left over from
    # an earlier build cannot pass in place of the current one.
    "$CORE_ROOT/Scripts/run-ctest.sh" "$CTEST_BUILD_DIR" \
        -j"$CTEST_JOBS" --output-on-failure
}

# Whether this run hands the registered tests to ctest. NO_CTEST=1 says no, and
# so does any filter: `run-tests.sh <name>` asks for one stanza, and a whole
# ctest run beside it would report failures in tests nobody named.
USE_CTEST=1
if [ "${NO_CTEST:-0}" = "1" ] || [ ${#FILTERS[@]} -gt 0 ]; then
    USE_CTEST=0
fi
# --fuzz-only declines every stanza through wanted(), and the ctest battery is
# not a stanza: it is gated here, on the filter list. Passing the flag leaves
# that list empty, so without this line the suite it exists to skip runs in
# full and the flag saves nothing.
[ "$FUZZ_ONLY" = "1" ] && USE_CTEST=0

# Two independent questions: whether to RUN the ctest suite (USE_CTEST) and
# whether to SKIP the stanzas it covers (SKIP_CTEST_COVERED). shell_remainder
# needs NO_CTEST=1 with the skip still on; one flag answering both makes it
# recompile the Core sources per stanza and re-run the whole suite.
SKIP_CTEST_COVERED="${SKIP_CTEST_COVERED:-$USE_CTEST}"

if [ "$USE_CTEST" = "1" ]; then
    if ! run_ctest_suite; then
        CTEST_FAILED=1
        if [ "${UBQ_TEST_FAIL_FAST:-0}" = "1" ]; then
            echo "run-tests.sh: fail-fast — shell stanzas remain unexecuted after CTest failed" >&2
            exit 1
        fi
    fi
fi

RAN=0
# Stanzas that answered by declining. RAN counts both, so without this a run on
# a machine with no sanitizer runtime, no fuzzer and no QUIC archives reports the
# same RAN as a full one and exits 0 — which is how snapshots reported Passed
# without running for as long as no app build existed.
SKIPPED=0

# What each decline was about, so the summary can name the capabilities this run
# does not speak for. A count answers "how much did I lose" and not "what",
# which are different questions when the answer is one absent sanitizer runtime
# versus every fuzz lane — and the SKIP lines that would tell you are forty
# stanzas up by the time the verdict prints.
SKIPPED_REASONS=()

# Why this machine's nvim cannot run the Neovim client, or nothing.
#
# The client needs Neovim 0.10 or later for
# `vim.uv`, `vim.system` and `nvim_open_term`. An older nvim starts the check
# and fails at the first of those, which reads as a broken client rather than
# as the machine it is, so an old nvim declines as a missing one does.
#
# - Returns: 0, printing the reason, when the stanza must not run.
ubq_neovim_unusable_reason() {
    if ! command -v nvim >/dev/null 2>&1; then
        echo "nvim not installed"
        return 0
    fi
    if ! nvim --headless -u NONE -i NONE -n -c 'if !has("nvim-0.10") | cquit | endif' -c 'quit' </dev/null >/dev/null 2>&1; then
        echo "$(nvim --version 2>/dev/null | head -n 1) is older than the Neovim client's 0.10 floor"
        return 0
    fi
    return 1
}

# A compile/link failure of a test the suite already DECIDED to run is a broken
# tree, not a missing capability — fail the run loudly instead of printing SKIP,
# so coverage can't silently vanish (a stanza whose build a refactor breaks would
# rot behind a SKIP). Environment gates — absent QUIC deps,
# no libFuzzer, no working sanitizer runtime — still SKIP, but they are all
# probed before any test build is attempted.
# The prefix CMake configured, never a second guess at the same path.
#
# QUIC dependencies share the external build root with the CMake trees and the
# Actions cache. The cmake() wrapper hands the same prefix to every nested
# configure, so configured targets and hand-written links cannot drift to the
# default Scripts/cmake/UbiquittyToolchain.cmake chooses. An explicit
# UBIQUITTY_QUIC_PREFIX still moves both together. QUIC_PREFIX is assigned
# beside the wrapper because the CTest section calls it before this archive list
# is reached.

# The archives a hand-rolled link of the host objects must name.
#
# Every stanza that links "@…objs.txt" is linking ubiquitty-host's objects, and
# those carry HostQUIC.cpp. QUIC builds unconditionally, so that translation unit
# calls into BoringSSL and ngtcp2 and a link that names neither dies on an
# undefined BIO_* — which the harness reports as a broken build, meaning the
# stanza asserts nothing and still does not fail the run.
#
# Every such stanza must also compile its own translation unit with
# -DUBIQUITTY_QUIC=1, because `Daemon` declares a member under that macro:
# a test compiled without it sees a different layout from the objects it links
# against, reads `m_clients` and `m_sessions` at the wrong offsets, and spins on
# the garbage it finds. The archives alone would turn a link error that names
# itself into undefined behaviour that does not.
HOST_LINK_ARCHIVES=(
    "$QUIC_PREFIX/lib/libngtcp2_crypto_boringssl.a"
    "$QUIC_PREFIX/lib/libngtcp2.a"
    "$QUIC_PREFIX/lib/libssl.a"
    "$QUIC_PREFIX/lib/libcrypto.a"
)
[ "$(uname)" = "Darwin" ] && HOST_LINK_ARCHIVES+=(-framework CoreFoundation -framework Security)

# quic_deps_present — true when every archive the QUIC stanzas link is there.
#
# Each of them links four, so testing one lets a partial tree past the gate and
# into the linker, where a missing dependency is indistinguishable from a real
# break. Names what is absent the first time it is asked, once per run: a
# silently skipped test and a passing one look identical from the summary.
QUIC_DEPS_REPORTED=0
quic_deps_present() {
    local missing=()
    local archive
    local report_missing=1
    [ "${1:-}" = "--quiet" ] && report_missing=0
    for archive in libngtcp2.a libngtcp2_crypto_boringssl.a libssl.a libcrypto.a; do
        [ -f "$QUIC_PREFIX/lib/$archive" ] || missing+=("$archive")
    done
    if [ "${#missing[@]}" -eq 0 ]; then
        if "$CORE_ROOT/Scripts/ensure-quic-deps.sh" --check "$QUIC_PREFIX"; then
            return 0
        fi
        if [ "$report_missing" -eq 1 ] && [ "$QUIC_DEPS_REPORTED" -eq 0 ]; then
            QUIC_DEPS_REPORTED=1
            ubq_skip "QUIC tests — $QUIC_PREFIX/lib holds archives for another platform."
            SKIPPED=$((SKIPPED + 1))
            echo "      They belong to whichever host filled build/, and $(uname -s) cannot link them."
            echo "      Rebuild them here with ./Scripts/ensure-quic-deps.sh"
        fi
        return 1
    fi
    if [ "$report_missing" -eq 1 ] && [ "$QUIC_DEPS_REPORTED" -eq 0 ]; then
        QUIC_DEPS_REPORTED=1
        ubq_skip "QUIC tests — absent from $QUIC_PREFIX/lib: ${missing[*]}"
        SKIPPED=$((SKIPPED + 1))
        echo "      Build them with ./Scripts/ensure-quic-deps.sh"
    fi
    return 1
}

# ubq_run_bounded SECONDS BINARY [ARGUMENT…] — see Scripts/run-bounded.sh.
# shellcheck source=Scripts/run-bounded.sh
. "$CORE_ROOT/Scripts/run-bounded.sh"

# A stanza whose binary would not build, as distinct from one whose assertions
# failed. Were both FAIL with exit 1, a link error and a real defect would be the
# same event to anyone reading the output or the status — a Core file split that
# leaves a stanza linking a caller without its callee would read as a test
# failure and hide real diffs behind it.
#
# BUILD-BROKEN, and exit 2. The word says the code never ran; the code lets a
# caller branch on it without parsing the message.
build_broken() {
    echo "BUILD-BROKEN: $1"
    echo "  The stanza's binary did not build, so nothing was asserted. This is not"
    echo "  a test failure and the tests it contains have no verdict."
    # Callers spell the log as "(see <path>)". A path is something a developer
    # can open and a CI reader cannot: the runner's filesystem is gone by the
    # time anyone reads the job, so a message naming a file says nothing.
    # The compiler's own lines go on the output, which is the only place a
    # reader has. Same shape check-verdict-lines.py requires of a verdict.
    local build_broken_log
    build_broken_log="$(printf '%s' "$1" | sed -n 's/.*(see \([^)]*\)).*/\1/p')"
    if [ -n "$build_broken_log" ] && [ -r "$build_broken_log" ]; then
        local build_broken_lines
        build_broken_lines="$(grep -E 'error:|Error|undefined (symbol|reference)|ld: ' "$build_broken_log" | head -5)"
        [ -n "$build_broken_lines" ] || build_broken_lines="$(tail -5 "$build_broken_log")"
        if [ -n "$build_broken_lines" ]; then
            echo "  What the build said:"
            printf '%s\n' "$build_broken_lines" | sed 's/^/    /'
        fi
    fi
    exit 2
}

build_and_run() {
    local name="$1"; shift
    if ! wanted "$name"; then
        return 0
    fi
    echo "=== $name ==="
    # If any source file ends in .cpp, route the build through c++ as
    # the link driver (so libstdc++ symbols resolve). C source files
    # are compiled with -xc to preserve C semantics, then linked
    # together by the C++ frontend.
    local has_cxx=0
    for a in "$@"; do
        case "$a" in
            *.cpp|*.cc) has_cxx=1; break ;;
        esac
    done
    if [ "$has_cxx" = "1" ]; then
        # Compile each source separately so we can flag .c with -xc.
        # Collect any -I / -D / -O flags up front so they apply to every TU.
        local incflags="$HOST_COMPAT"
        for a in "$@"; do
            case "$a" in
                -I*|-D*|-O*) incflags="$incflags $a" ;;
            esac
        done
        local objs=""
        local args=()
        for a in "$@"; do
            case "$a" in
                *.c)
                    local obj
                    obj="$OUT/${name}_$(basename "$(dirname "$a")")_$(basename "$a" .c).c.o"
                    # shellcheck disable=SC2086
                    $CC_LAUNCHED $CFLAGS_BASE $incflags -c "$a" -o "$obj" || return 1
                    objs="$objs $obj"
                    ;;
                *.cpp|*.cc)
                    local obj
                    obj="$OUT/${name}_$(basename "$(dirname "$a")")_$(basename "$a").o"
                    local clog="$OUT/${name}.compile.log"
                    # ThirdParty/ is vendored: we don't own its warnings and can't
                    # patch around every toolchain's opinion of them (a newer clang
                    # flagging an unused helper in simdutf's amalgamation shouldn't
                    # fail our build). Vendored sources compile with -w so their
                    # diagnostics are suppressed entirely — they are noise in our
                    # build; only our sources carry -Wall -Wextra -Werror.
                    local werror="-Werror"
                    case "$a" in
                        */ThirdParty/*) werror="-w" ;; # vendored: suppress all diagnostics
                    esac
                    # Skipped when nothing it was compiled from has changed.
                    # Without this every stanza recompiles its whole list on
                    # every run, which the cache hides until it goes cold.
                    local dep="${obj%.o}.d"
                    # shellcheck disable=SC2086
                    if ! ubq_object_is_current "$obj" "$dep" \
                        && ! $CXX_TEST -std=c++23 -I"$ROOT/Tests" -I"$CORE_ROOT/Tests" -Wall -Wextra $werror \
                        -Wno-deprecated-declarations -Wno-multichar -Wno-comment \
                        -fno-exceptions -fno-rtti \
                        $incflags -MD -MF "$dep" -c "$a" -o "$obj" 2>"$clog"; then
                        cat "$clog" >&2
                        return 1
                    fi
                    objs="$objs $obj"
                    ;;
                -I*|-D*|-O*)
                    : # already collected into $incflags
                    ;;
                *)
                    args+=("$a")
                    ;;
            esac
        done
        # shellcheck disable=SC2086
        $CXX_LINK ${args[@]+"${args[@]}"} $objs -o "$OUT/$name" || return 1
    else
        # shellcheck disable=SC2086
        $CC_LAUNCHED $CFLAGS_BASE $HOST_COMPAT "$@" -o "$OUT/$name"
    fi
    "$OUT/$name"
    RAN=$((RAN + 1))
}


# CORE_SRCS — the live-Terminal Core dependency set every parser/render/runtime
# stanza links: a Terminal that processes bytes pulls the full VT100 + OSC + SGR +
# Tek + Kitty/image + Unicode + tmux-filter + PNG/Sixel decode surface plus the
# PAL contract. Defined once here and expanded via "${CORE_SRCS[@]}" so a Core
# file split touches one list, not many copy-pasted stanzas. Every
# path is a subset of the canonical Scripts/portable-core-sources.txt; stanzas
# that need more (Sync/Preferences/Runtime/software-render/tmux-session) append those
# extras after the expansion.
CORE_SRCS=(
    # simdutf: not Core, but Core/Codec/Base64.cpp and Core/Unicode/UTF8Decoder.cpp
    # call into it, so a stanza linking either one links this. It is here rather
    # than in each stanza for the reason the rest of this list is.
    "$ROOT/Source/ThirdParty/simdutf/simdutf.cpp"
    "$ROOT/Source/Core/BuildVersion.cpp"
    "$ROOT/Source/Core/VT/VT.cpp"
    "$ROOT/Source/Core/VT/CharacterSets.cpp"
    "$ROOT/Source/Core/VT/Init.cpp"
    "$ROOT/Source/Core/Screen/Addressing.cpp"
    "$ROOT/Source/Core/Screen/CommandBlocks.cpp"
    "$ROOT/Source/Core/Screen/Scrollback.cpp"
    "$ROOT/Source/Core/Screen/HistoryLines.cpp"
    "$ROOT/Source/Core/Screen/LineIdentity.cpp"
    "$ROOT/Source/Core/Modes/SyncOutput.cpp"
    "$ROOT/Source/Core/Modes/DisplayIdle.cpp"
    # The Terminal -> ModeSnapshot sampler, so a test can encode a key
    # against a live Terminal the way the web, wscons and GTK arms do.
    "$ROOT/Source/Core/Runtime/ModeSnapshot.cpp"
    "$ROOT/Source/Core/Modes/HostPolicy.cpp"
    "$ROOT/Source/Core/Attributes/Hyperlinks.cpp"
    # VT.cpp and CSI.cpp call ensureStatusRow, so the status row's store comes
    # with them: DECSSDT allocates it on first write rather than per Terminal.
    "$ROOT/Source/Core/Rendering/StatusRow.cpp"
    "$ROOT/Source/Core/VT/ParseState.cpp"
    "$ROOT/Source/Core/Modes/Emulation.cpp"
    "$ROOT/Source/Core/Modes/Focus.cpp"
    "$ROOT/Source/Core/Modes/Scroll.cpp"
    "$ROOT/Source/Core/Triggers/Triggers.cpp"
    "$ROOT/Source/Core/Mouse/Locator.cpp"
    "$ROOT/Source/Core/Cell.cpp"
    "$ROOT/Source/Core/Screen/Marks.cpp"
    "$ROOT/Source/Core/Mouse/Hilite.cpp"
    "$ROOT/Source/Core/Screen/Reflow.cpp"
    "$ROOT/Source/Core/TerminalOperations.cpp"
    "$ROOT/Source/Core/VT/Modes.cpp"
    "$ROOT/Source/Core/VT/Reports.cpp"
    "$ROOT/Source/Core/Screen/Scroll.cpp"
    "$ROOT/Source/Core/Screen/Cursor.cpp"
    "$ROOT/Source/Core/Screen/Erase.cpp"
    "$ROOT/Source/Core/Screen/SizedBlock.cpp"
    "$ROOT/Source/Core/Rendering/ProportionalRun.cpp"
    "$ROOT/Source/Core/Screen/Edit.cpp"
    "$ROOT/Source/Core/Screen/Rect.cpp"
    "$ROOT/Source/Core/VT/CSI.cpp"
    "$ROOT/Source/Core/VT/Prefixed.cpp"
    "$ROOT/Source/Core/VT/Sequences/Protection.cpp"
    "$ROOT/Source/Core/VT/Cancel.cpp"
    "$ROOT/Source/Core/VT/Escape.cpp"
    "$ROOT/Source/Core/Triggers/Engine.cpp"
    "$ROOT/Source/Core/LineStore.cpp"
    "$ROOT/Source/Core/Diagnostics/DiagLog.cpp"
    "$ROOT/Source/PAL/PAL.cpp"
    # The POSIX PAL backend: OSC/State.cpp asks it for the host name, so a
    # Terminal that parses OSC 7 cannot link without it.
    "$ROOT/Source/Platform/POSIX/SystemPAL.cpp"
    "$ROOT/Source/Core/SGR.cpp"
    "$ROOT/Source/Core/OSC/Activity.cpp"
    "$ROOT/Source/Core/DragDrop/Client.cpp"
    "$ROOT/Source/Core/DragDrop/Protocol.cpp"
    "$ROOT/Source/Core/DragDrop/Session.cpp"
    "$ROOT/Source/Core/OSC/Dispatch.cpp"
    "$ROOT/Source/Core/OSC/TextSizing.cpp"
    "$ROOT/Source/Core/RichText/Document.cpp"
    "$ROOT/Source/Core/RichText/Drawing.cpp"
    "$ROOT/Source/Core/RichText/Layout.cpp"
    "$ROOT/Source/Core/RichText/RTFReader.cpp"
    "$ROOT/Source/Core/RichText/BlockStore.cpp"
    "$ROOT/Source/Core/RichText/Blocks.cpp"
    "$ROOT/Source/Core/OSC/RichTextBlocks.cpp"
    "$ROOT/Source/Core/OSC/Color.cpp"
    "$ROOT/Source/Core/OSC/MousePointerShape.cpp"
    "$ROOT/Source/Core/OSC/XParseColor.cpp"
    "$ROOT/Source/Core/OSC/Clipboard.cpp"
    "$ROOT/Source/Core/OSC/ExtendedClipboard.cpp"
    "$ROOT/Source/Core/Codec/Base64.cpp"
    "$ROOT/Source/Core/OSC/State.cpp"
    "$ROOT/Source/Core/OSC/Notify.cpp"
    "$ROOT/Source/Core/OSC/ShellIntegration.cpp"
    "$ROOT/Source/Core/OSC/ITerm2.cpp"
    "$ROOT/Source/Core/Tek/Tek4014.cpp"
    # Beside Tek4014.cpp, not optional next to it: the graphic-input report
    # builds its projection inside the portable machine, so every stanza that
    # links the one links the other or the link fails on `makeProjection`.
    "$ROOT/Source/Core/Tek/Project.cpp"
    "$ROOT/Source/Core/Terminfo.cpp"
    "$ROOT/Source/Core/Mouse/Mouse.cpp"
    "$ROOT/Source/Core/Kitty/Graphics.cpp" \
    "$ROOT/Source/Core/Kitty/KeyboardState.cpp"
    # XTGETTCAP answers a key capability from the key encoder whenever a
    # keyboard type other than the default is selected, so VT.cpp calls
    # Keymap::encodeKeyEvent, and the encoder's CSI u form is KittyKeyboard's.
    "$ROOT/Source/Core/Keys/Keymap.cpp"
    "$ROOT/Source/Core/Keys/KittyKeyboard.cpp"
    "$ROOT/Source/Core/Image/Inline/Images.cpp"
    "$ROOT/Source/Core/Image/TerminalImage.cpp"
    "$ROOT/Source/Core/Image/TIFFDecoder.cpp"
    "$ROOT/Source/PAL/TaskQueue.cpp"
    "$ROOT/Source/Core/CellGrid.cpp"
    "$ROOT/Source/Core/Accessibility/TextProjection.cpp"
    "$ROOT/Source/Core/Unicode/Width.cpp"
    "$ROOT/Source/Core/Unicode/UTF8Decoder.cpp"
    "$ROOT/Source/Core/Unicode/Encodings.cpp"
    "$ROOT/Source/Core/Unicode/GraphemeBreak.cpp"
    "$ROOT/Source/Core/Unicode/Joining.cpp"
    "$ROOT/Source/Core/Protocol/Tmux/Tmux.cpp"
    "$ROOT/Source/Core/Codec/PNGDecoder.cpp"
    "$ROOT/Source/Core/Image/Sixel/Decoder.cpp"
    "$ROOT/Source/Core/Codec/ZlibInflate.cpp"
    "$ROOT/Source/Core/Codec/WuffsCodec.cpp"
)

# SYNC_SRCS — the screen plane every Sync client and the daemon carry beside
# NativeAttachment.cpp: a stanza that links a Sync client links these, because the
# screen arrives on no other wire. Every path is in portable-core-sources.txt.
# Connection.cpp sends and reads the attachments and consent-status Notifies, so
# their codecs and the JSON codec under them ride here, and every consumer links
# the yyjson object (ubq_yyjson_object, or ubq_yyjson_fuzz_object for a fuzzer).
SYNC_SRCS=(
    "$ROOT/Source/Core/Protocol/Sync/Effects.cpp"
    "$ROOT/Source/Core/Protocol/Sync/EffectCodec.cpp"
    "$ROOT/Source/Core/Protocol/Sync/EffectDelivery.cpp"
    "$ROOT/Source/Core/Protocol/Sync/Attachment.cpp"
    "$ROOT/Source/Core/Protocol/Sync/ClientAttachment.cpp"
    "$ROOT/Source/Core/Protocol/Sync/Connection.cpp"
    # Connection.cpp carries native drag events and commands.
    "$ROOT/Source/Core/Protocol/Sync/NativeDrag.cpp"
    "$ROOT/Source/Core/Protocol/Sync/Envelope.cpp"
    "$ROOT/Source/Core/Protocol/Sync/Geometry.cpp"
    "$ROOT/Source/Core/Protocol/Sync/Hash.cpp"
    "$ROOT/Source/Core/Protocol/Sync/HistoryCache.cpp"
    "$ROOT/Source/Core/Protocol/Sync/HistoryPlane.cpp"
    "$ROOT/Source/Core/Protocol/Sync/HistoryWants.cpp"
    "$ROOT/Source/Core/Protocol/Sync/Images.cpp"
    "$ROOT/Source/Core/Protocol/Sync/Samples.cpp"
    "$ROOT/Source/Core/Protocol/Sync/Input.cpp"
    "$ROOT/Source/Core/Protocol/Sync/Lanes.cpp"
    "$ROOT/Source/Core/Protocol/Sync/Liveness.cpp"
    "$ROOT/Source/Core/Protocol/Sync/Messages.cpp"
    "$ROOT/Source/Core/Protocol/Sync/Pacer.cpp"
    "$ROOT/Source/Core/Protocol/Sync/Records.cpp"
    "$ROOT/Source/Core/Protocol/Sync/ResumeProof.cpp"
    "$ROOT/Source/Core/Protocol/Sync/ResumeRecord.cpp"
    "$ROOT/Source/Core/Protocol/Sync/RichTextForm.cpp"
    "$ROOT/Source/Core/Protocol/Sync/RowForm.cpp"
    "$ROOT/Source/Core/Protocol/Sync/ScreenPlane.cpp"
    "$ROOT/Source/Core/Protocol/Sync/Topology.cpp"
    "$ROOT/Source/Core/Protocol/Sync/WireVersion.cpp"
    "$ROOT/Source/Core/Protocol/Sync/Notify.cpp"
    "$ROOT/Source/Core/Protocol/Sync/ConsentStatus.cpp"
    "$ROOT/Source/Core/Codec/JSON.cpp"
)

# host_objects OBJDIR OUTFILE [extra-basenames-to-skip...]
#
# The objects CMake ACTUALLY links into the host target, minus main.cpp.o.
# Derived from CMake's own link line, not from `find OBJDIR -name '*.o'`.
#
# find walks the filesystem, so a source trimmed from the target leaves its stale
# .o behind and every stanza keeps linking it — forever, and silently, because the
# symptom is an undefined symbol from a TU that is no longer in the build: a
# source trimmed from the host subset leaves a .o that re-introduces its
# dependencies until the build dir is deleted by hand. A link line is
# regenerated by the build system,
# so it cannot carry an object the target no longer has.
#
# Where that line lives depends on the generator: Unix Makefiles writes link.txt,
# Ninja writes none and answers `-t commands` instead. Both are the same
# authority — what the target is actually linked from — so the invariant above
# holds either way, and neither is a filesystem walk.
# Compiles each source to its own object, at the machine's width, then links.
#
# `c++ a.cpp b.cpp c.cpp -o bin` compiles every unit inside one process, one
# after another, and ccache declines a multi-source command outright — so a
# stanza that names the whole of Core pays a full serial uncached build on every
# run, however warm the cache is. Per-unit compilation restores both properties:
# the units go out in parallel, and each one is a cache key of its own.
#
# The objects are named after the source path with the separators folded, so two
# sources cannot land on one object and a failure names the file that produced
# it.
#
# - Parameter binary: Where the linked result goes.
# - Parameter objectDirectory: Scratch for the objects and the compile log.
# - Parameter errorLog: Collects every unit's diagnostics.
# The rest is the compile flags, then `--`, then the sources.
# ubq_sanitized_binary — the binary under test carries the sanitizer runtime.
#
# The probe every sanitizer stanza runs before building compiles a two-line
# program with the flag and runs it. That answers whether the TOOLCHAIN has a
# runtime; it says nothing about the binary the stanza then produces, because it
# is a different compile of a different file. A stanza whose own flag went
# missing passes the probe, links an uninstrumented binary, runs it, reports
# nothing and exits 0 — which is the verdict a clean ASan run gives, spelled the
# same way.
#
# Decided by the runtime's own symbol, which the instrumented binary references
# and nothing else does. `undefined` is not decidable this way: UBSan emits a
# handler reference only where it inserts a check, so an absence is a fact about
# the code rather than about the build. Reporting it as uninstrumented would be
# an answer this cannot support, so it is named as undecided instead.
# The Host frontend, built once, printed as a path.
#
# Eleven stanzas run against this binary. Each configuring and building the tree
# itself would configure one directory eleven times so that one file could be
# read, and would make them hold a lock against each other. Reading a binary
# needs no lock; building it does.
#
# UBQ_HOST_PREBUILT says a fixture has already built it, which only ctest knows.
# Without that this builds on demand, because a stanza named on the command line
# has no fixture behind it and has to stay runnable on its own. Under ctest the
# fixture builds it whether or not a stanza was told so, and a stanza that was
# not refuses rather than build it a second time.
# A content fingerprint of the daemon's source. The daemon (ubiquitty-host) links
# Core, Base and Platform/Host; this hashes their working-tree content, so an edit
# to any of them changes the fingerprint. Content rather than mtime: in the Linux
# container cmake's incremental check can keep a daemon built before a Core
# change — measured: a from-scratch build carries a new outbound log string that
# the reused binary does not — under bind-mounted source and build trees whose
# mtimes it compares to the wrong conclusion. A stale daemon there is a
# silently wrong test: a Core fix reads as unverified, or a bug it removed reads as
# still present.
ubq_daemon_source_fingerprint() {
    local blob; blob="$(mktemp)"
    find "$ROOT/Source/Core" "$ROOT/Source/Base" "$ROOT/Source/Platform/Host" \
        -type f \( -name '*.cpp' -o -name '*.h' -o -name '*.mm' \) -print0 2>/dev/null \
        | LC_ALL=C sort -z | xargs -0 cat >"$blob" 2>/dev/null
    cmake -E sha256sum "$blob" 2>/dev/null | cut -d' ' -f1
    rm -f "$blob"
}

# ubq_refuse_unordered_build <helper> <fixture> <binary> — fails when ctest's
# <fixture> builds <binary> for this run and the calling stanza was not ordered
# after it. Tests/CMakeLists.txt sets
# UBQ_SHARED_BINARIES_FROM_FIXTURES for every stanza but the fixtures, and
# orders after <fixture> exactly the stanzas whose own body calls <helper>. Any
# other caller building here would relink the binary under the stanzas
# executing it.
ubq_refuse_unordered_build() { # <helper> <fixture> <binary>
    [ "${UBQ_SHARED_BINARIES_FROM_FIXTURES:-0}" = "1" ] || return 0
    echo "$1: the $2 fixture builds $3 for this run, and this stanza is not ordered after it." >&2
    echo "  Building it here would relink it under the stanzas executing it. Call $1" >&2
    echo "  in the stanza's own body, which is where the fixture's inventory looks." >&2
    return 1
}

ubq_host_binary() { # <build log>
    local log="$1" binary="$HOST_TEST_BUILD/ubiquitty-host"
    if [ "${UBQ_HOST_PREBUILT:-0}" = "1" ]; then
        # Loudly, because the alternative is building it here — which is the
        # concurrent write the fixture exists to prevent, and it would happen
        # exactly when several stanzas are already running.
        if [ ! -x "$binary" ]; then
            echo "ubq_host_binary: UBQ_HOST_PREBUILT is set and $binary is not there." >&2
            echo "  The fixture that builds it did not run, or did not produce it." >&2
            return 1
        fi
        ubq_note_expected_profile
        printf '%s' "$binary"
        return 0
    fi
    ubq_refuse_unordered_build ubq_host_binary host_binary "$binary" || return 1
    # Only in the Linux container (run-linux.sh's docker, marked by /.dockerenv):
    # there cmake's incremental check has reused a daemon built before a Core
    # change, where the native macOS build detects the change and rebuilds, so
    # forcing it on macOS would only cost a clean rebuild it does not need. Force a
    # clean build when the daemon's source no longer hashes to the cached binary's
    # — an empty fingerprint (find or cmake unavailable) forces it too, over
    # trusting a check that could not run.
    if [ -f /.dockerenv ]; then
        local fp_file="$HOST_TEST_BUILD/.daemon-source-sha256" fingerprint
        fingerprint="$(ubq_daemon_source_fingerprint)"
        if [ ! -x "$binary" ] || [ -z "$fingerprint" ] || [ "$fingerprint" != "$(cat "$fp_file" 2>/dev/null)" ]; then
            rm -rf "$HOST_TEST_BUILD"
        fi
    fi
    cmake -B "$HOST_TEST_BUILD" -S "$ROOT" -G "$UBQ_GENERATOR" \
            -DUBIQUITTY_FRONTEND=Host -DCMAKE_BUILD_TYPE=Debug >"$log" 2>&1 \
        && cmake --build "$HOST_TEST_BUILD" -j"$BUILD_JOBS" >>"$log" 2>&1 || return 1
    [ -f /.dockerenv ] && ubq_daemon_source_fingerprint >"$HOST_TEST_BUILD/.daemon-source-sha256"
    printf '%s' "$binary"
}

# ubq_agent_binary <build log> — the agent the agent stanzas run, as
# Scripts/build-agent.sh builds it, so a stanza runs the ubq of this checkout
# rather than whichever one an earlier build left behind. UBQ_BIN names another
# outright. Under ctest the agent_binary fixture builds it once and the stanzas
# ordered after it only read it, as the Host fixture's do.
ubq_agent_binary() { # <build log>
    local log="$1" binary="$UBQ_BUILD_ROOT/agent/ubq"
    if [ -n "${UBQ_BIN:-}" ]; then
        if [ ! -x "$UBQ_BIN" ]; then
            echo "ubq_agent_binary: UBQ_BIN names $UBQ_BIN, which is not an executable." >&2
            return 1
        fi
        printf '%s' "$UBQ_BIN"
        return 0
    fi
    if [ "${UBQ_AGENT_PREBUILT:-0}" = "1" ]; then
        if [ ! -x "$binary" ]; then
            echo "ubq_agent_binary: UBQ_AGENT_PREBUILT is set and $binary is not there." >&2
            echo "  The fixture that builds it did not run, or did not produce it." >&2
            return 1
        fi
        printf '%s' "$binary"
        return 0
    fi
    ubq_refuse_unordered_build ubq_agent_binary agent_binary "$binary" || return 1
    UBQ_BUILD_JOBS="$BUILD_JOBS" "$ROOT/Scripts/build-agent.sh" >"$log" 2>&1 || return 1
    [ -x "$binary" ] || return 1
    printf '%s' "$binary"
}

# ubq_agent_host_binary <build log> — the daemon an agent stanza drives:
# UBIQUITTY_HOST_BIN when the caller names one, and otherwise the Host
# frontend the host stanzas run.
ubq_agent_host_binary() { # <build log>
    if [ -n "${UBIQUITTY_HOST_BIN:-}" ]; then
        if [ ! -x "$UBIQUITTY_HOST_BIN" ]; then
            echo "ubq_agent_host_binary: UBIQUITTY_HOST_BIN names $UBIQUITTY_HOST_BIN, which is not an executable." >&2
            return 1
        fi
        printf '%s' "$UBIQUITTY_HOST_BIN"
        return 0
    fi
    ubq_host_binary "$1"
}

ubq_sanitized_binary() { # <sanitizer> <binary>
    local sanitizer="$1" binary="$2" token
    case "$sanitizer" in
        address) token="__asan_init" ;;
        thread) token="__tsan_init" ;;
        *) return 0 ;; # undecidable from symbols; the caller says so
    esac
    # A binary that is not there is a different fact from one built without the
    # flag, and reporting the second for the first sends the reader to the
    # compile line of a stanza whose compile never produced a file. Separated by
    # exit code: 2 for absent, 1 for present and uninstrumented.
    [ -f "$binary" ] || return 2

    # Read the symbol table into a variable, then match it in the shell. An
    # `nm | grep` pipeline can report a binary as uninstrumented while that same
    # binary, checked immediately afterwards, carries the token — so its verdict
    # is not a reading of the file. A pipeline's status answers for every
    # process in it, and only one of them is looking at the binary.
    #
    # Neither obvious cause accounts for it: nm does not fail under CPU
    # contention, and `grep -q` exiting early does not cost nm its status here,
    # even matching the first symbol. This narrows what the answer can be drawn
    # from rather than claiming to name the mechanism.
    local symbols status
    symbols=$(nm "$binary" 2>/dev/null)
    status=$?
    # nm's own failure is a fact about the instrument. Answering it with "built
    # without the flag" points the reader at a -fsanitize that is right there in
    # the compile line, so it is separated: 3 for could not read.
    [ "$status" -eq 0 ] || return 3
    # A control with a known answer in the same output, because a status of zero
    # over an empty listing would read as a missing token. Every binary this is
    # asked about links a main; if that is absent too, this is not a symbol table
    # a conclusion can be drawn from.
    case "$symbols" in
        *" main"*|*" _main"*) ;;
        *) return 3 ;;
    esac

    # Matched by the shell, not by a second pipeline: feeding the listing back
    # into `grep -q` reproduces the SIGPIPE above with printf as the writer.
    case "$symbols" in
        *"$token"*) return 0 ;;
        *) return 1 ;;
    esac
}

# ubq_require_sanitized — refuse to read a clean run from an uninstrumented binary.
ubq_require_sanitized() { # <sanitizer> <binary> <stanza>
    local sanitizer="$1" binary="$2" stanza="$3"
    case "$sanitizer" in
        address|thread) ;;
        *) return 0 ;;
    esac
    ubq_sanitized_binary "$sanitizer" "$binary"
    local verdict=$?
    if [ "$verdict" -eq 2 ]; then
        echo "$stanza: no binary at $binary to check — the compile reported success" >&2
        echo "  and produced nothing at the path this runs, so what follows would" >&2
        echo "  have been a clean run of a file that is not there." >&2
        return 1
    fi
    if [ "$verdict" -eq 3 ]; then
        echo "$stanza: could not read a symbol table from $binary, so whether it" >&2
        echo "  carries the $sanitizer runtime is undecided here. This is a fact" >&2
        echo "  about nm, not about the compile line — check the binary by hand" >&2
        echo "  before touching a -fsanitize flag." >&2
        return 1
    fi
    if [ "$verdict" -ne 0 ]; then
        echo "$stanza: built without the $sanitizer runtime — the binary carries no" >&2
        echo "  ${sanitizer}_init reference, so a clean run here reports nothing about" >&2
        echo "  memory at all. The stanza's own -fsanitize flag went missing; the" >&2
        echo "  toolchain probe cannot see that, because it compiles a different file." >&2
        return 1
    fi
    return 0
}

# Whether an object is newer than everything it was compiled from.
#
# The prerequisite list is the compiler's own, written with -MD, so editing a
# header invalidates the objects that included it. Comparing against the source
# alone would not, and would hand back an object compiled against a header that
# has since changed — which links, runs, and is wrong.
#
# One definition: build_and_run calls it, and the generated per-unit worker gets
# it injected with `declare -f`. Two copies of a staleness rule drift, and the
# copy that drifts is the one that stops rebuilding.
ubq_object_is_current() { # <object> <dependency file>
    local object="$1" dependencies="$2" prerequisite
    [ -f "$object" ] || return 1
    [ -f "$dependencies" ] || return 1
    # "obj: a b \" over several lines: drop the target and the continuations. A
    # prerequisite that has been deleted counts as stale, because the object was
    # compiled against something that is no longer there.
    for prerequisite in $(sed -e 's/^[^:]*://' -e 's/\\$//' "$dependencies"); do
        if [ ! -e "$prerequisite" ] || [ "$prerequisite" -nt "$object" ]; then
            return 1
        fi
    done
    return 0
}

# The vendored JSON parser as an object a stanza links. yyjson is C and wants
# -std=c99, so it cannot ride the C++ command line ubq_compile_parallel gives
# every source; Core/Codec/JSON.cpp calls into it. Rebuilt only when the
# source is newer than the object.
ubq_yyjson_object() { # <object>, printed on success
    local object="$1" source="$ROOT/Source/ThirdParty/yyjson/yyjson.c"
    if [ ! -f "$object" ] || [ "$source" -nt "$object" ]; then
        mkdir -p "$(dirname "$object")" || return 1
        $CC_LAUNCHED -std=c99 -w -O1 -g -c "$source" -o "$object" || return 1
    fi
    printf '%s\n' "$object"
}

ubq_compile_parallel() { # <binary> <object directory> <error log> <flag…> -- <source…> [-- <link input…>]
    local binary="$1" objdir="$2" errorlog="$3"
    shift 3
    # The suite's compiler unless the caller names another: the fuzz stanzas
    # build with the libFuzzer-capable clang rather than CXX, and the launcher
    # rides with whichever it is.
    local compiler="${UBQ_COMPILE_CXX:-$CXX_TEST}" linker="${UBQ_COMPILE_LINK:-$CXX_LINK}"
    # Three sections. Flags reach every compile and the link; sources compile
    # one object each; what follows a second `--` reaches the link alone — an
    # archive, an @objects response file, a framework. A stanza that handed the
    # compiler its sources and its archives in one command compiled and linked
    # in one step, which the cache launcher cannot cache: it caches a `-c`
    # compile of one file and passes anything else through, so every such
    # stanza recompiled its test from scratch on every run, on every runner.
    local flags=() sources=() links=() section=0 arg
    for arg in "$@"; do
        if [ "$arg" = "--" ] && [ "$section" -lt 2 ]; then
            section=$((section + 1))
            continue
        fi
        case "$section" in
        0) flags+=("$arg") ;;
        1) sources+=("$arg") ;;
        *) links+=("$arg") ;;
        esac
    done
    if [ "${#sources[@]}" -eq 0 ]; then
        echo "ubq_compile_parallel: no sources after --" >&2
        return 1
    fi

    # The objects live under a directory keyed on the compiler and the flags,
    # so two stanzas that name the same object directory with different flags
    # — one with a sanitizer, one without — never take each other's objects:
    # the staleness check reads timestamps, not the command that made them.
    objdir="$objdir/$(printf '%s\n' "$compiler" ${flags[@]+"${flags[@]}"} | cksum | cut -d' ' -f1)"
    mkdir -p "$objdir" || return 1
    : > "$errorlog"

    # The flags are quoted into the worker so a path with a space, or a
    # multi-word ccache launcher, survives the trip through xargs intact.
    local quoted_flags
    quoted_flags=$(printf ' %q' ${flags[@]+"${flags[@]}"})
    # The object is rebuilt when it is older than something it was compiled
    # from, and skipped otherwise. Without this every stanza recompiles its whole
    # source list on every run: 2,284 compiler invocations across the suite, kept
    # survivable only by the cache, which makes a full rebuild read as an
    # incremental one until the cache is cold.
    #
    # The prerequisite list is the compiler's own (-MD), so editing a header
    # invalidates the objects that included it. A timestamp against the source
    # alone would not, and would hand back a stale object that links and runs —
    # trading a correctness bug for the speed this is after.
    cat > "$objdir/compile-one.sh" <<WORKER
#!/bin/bash
source="\$1"
object="$objdir/\$(printf '%s' "\${source#$ROOT/}" | tr '/' '_').o"
dependencies="\${object%.o}.d"
$(declare -f ubq_object_is_current)
if ubq_object_is_current "\$object" "\$dependencies"; then
    exit 0
fi
# ThirdParty/ is vendored: we do not own its warnings, and a newer compiler
# finding one in an amalgamation must not fail this build. The sibling
# builder above suppresses them per file; this one compiles every unit with
# one flag set, so the suppression is appended for a vendored path.
vendored=""
case "\$source" in
    */ThirdParty/*) vendored="-w" ;;
esac
exec $compiler$quoted_flags \$vendored -MD -MF "\$dependencies" -c "\$source" -o "\$object"
WORKER
    chmod +x "$objdir/compile-one.sh" || return 1

    # -0 so a path is one argument whatever it contains; -n 1 so one unit is one
    # process and a failure is attributable.
    if ! printf '%s\0' "${sources[@]}" \
            | xargs -0 -P "$BUILD_JOBS" -n 1 "$objdir/compile-one.sh" 2>>"$errorlog"; then
        return 1
    fi

    local objects=() source
    for source in "${sources[@]}"; do
        local object
        object="$objdir/$(printf '%s' "${source#$ROOT/}" | tr '/' '_').o"
        # A unit that produced no object with xargs reporting success is the
        # silent case: the link would then be missing a definition and blame the
        # subject rather than the build.
        if [ ! -f "$object" ]; then
            echo "ubq_compile_parallel: $source compiled without producing $object" >&2
            return 1
        fi
        objects+=("$object")
    done

    # shellcheck disable=SC2086
    $linker ${flags[@]+"${flags[@]}"} ${objects[@]+"${objects[@]}"} ${links[@]+"${links[@]}"} -o "$binary" 2>>"$errorlog"
}

host_objects() {
    local objdir="$1" outfile="$2"
    shift 2
    local skip='/main\.cpp\.o$'
    local extra
    for extra in "$@"; do skip="$skip|/$extra\$"; done
    local builddir="${objdir%/CMakeFiles/*}"
    local linkline
    if [ -f "$objdir/link.txt" ]; then
        linkline=$(cat "$objdir/link.txt")
    else
        local target="${objdir##*/}"; target="${target%.dir}"
        linkline=$(ninja -C "$builddir" -t commands "$target" | tail -1)
    fi
    # Neither source answered. An empty object list links the test against
    # nothing, which surfaces as undefined symbols in the subject under test
    # rather than as a harness that could not find the build.
    if [ -z "$linkline" ]; then
        echo "host_objects: no link line for $objdir (neither link.txt nor ninja)" >&2
        return 1
    fi
    printf '%s\n' "$linkline" \
        | grep -oE '[^" ]+\.o\b' \
        | grep -vE "$skip" \
        | sed "s|^|$builddir/|" > "$outfile"

    # A link line that matched no object is the same empty list by another route:
    # a changed quoting style, a response file, a target that links only archives.
    # Checking the input and not the result leaves that case returning success.
    if [ ! -s "$outfile" ]; then
        echo "host_objects: link line for $objdir named no object files" >&2
        return 1
    fi
}

# ubq_consent_person <daemon binary> <source> — sets CONSENT_PERSON to the
# person the ubq smokes answer consent prompts with: UBQ_CONSENT_PERSON_BIN when
# the caller names one, and otherwise <source> linked against the objects of the
# daemon it is run beside, so the two agree on the wire. A failed link is
# BUILD-BROKEN for the calling stanza. The stanza spells the source because a
# recipe names its inputs: Scripts/check-tests-reaching-the-diff.py selects the
# stanzas whose own body names a changed file.
ubq_consent_person() { # <daemon binary> <source>
    CONSENT_PERSON="${UBQ_CONSENT_PERSON_BIN:-$OUT/consent_person}"
    [ -z "${UBQ_CONSENT_PERSON_BIN:-}" ] || return 0
    host_objects "$(dirname "$1")/CMakeFiles/ubiquitty-host.dir" "$OUT/consent-person-objs.txt" \
        || build_broken "consent_person requires the current host build's object list"
    if ! ubq_compile_parallel "$CONSENT_PERSON" "$OUT/objects/consent_person" "$OUT/consent-person-build.log" \
            -std=c++23 -I"$ROOT/Tests" -I"$ROOT/Source" -- "$2" \
            -- "@$OUT/consent-person-objs.txt" "${HOST_LINK_ARCHIVES[@]}"; then
        build_broken "consent_person link failed (see $OUT/consent-person-build.log)"
    fi
}


# The standalone archive and consumer are built without CMake. CTest supplies
# one fixture to both the consumer stanza and the driver's shared-library probe.
ubq_libcore_consumer() { # <build log>, prints the consumer path
    local log="$1"
    local build="${UBQ_HOST_TEST_TREE:-$CTEST_BUILD_DIR}/standalone-core"
    if [ "${UBQ_LIBCORE_PREBUILT:-0}" != 1 ]; then
        mkdir -p "$build" || return 1
        # shellcheck source=Scripts/portable-core-lib.sh
        source "$ROOT/Scripts/portable-core-lib.sh"
        "$ROOT/Scripts/gen-portable-core-sources.sh" --check >"$log" 2>&1 || return 1
        build_portable_core_archive "$build" "-O0 -g" >>"$log" 2>&1 || return 1
        # The consumer supplies its own PAL; the link remains independent of
        # any frontend or CMake target.
        ubq_compile_parallel "$build/core-consumer" "$build/consumer-objects" "$build/consumer-build.log" \
                -std=c++23 -I"$ROOT/Tests" -Wall -Wextra -Werror -fno-exceptions -fno-rtti \
                -Wno-deprecated-declarations -Wno-multichar -Wno-comment -I"$ROOT/Source" -- \
                "$ROOT/Samples/core-consumer/main.cpp" -- "$PORTABLE_CORE_ARCHIVE" >>"$log" 2>&1 || {
            cat "$build/consumer-build.log" >>"$log"
            return 1
        }
    fi
    if [ ! -f "$build/libubiquitty-core.a" ] || [ ! -x "$build/core-consumer" ]; then
        printf 'standalone Core fixture is missing: %s\n' "$build" >>"$log"
        return 1
    fi
    printf '%s\n' "$build/core-consumer"
}

# The tree Scripts/build-core-snapshot.sh builds the portable snapshot renderer
# in. Assigned at the top level, so the build-lock inventory resolves a stanza
# spelling the variable to this tree.
PORTABLE_SNAPSHOT_BUILD="$UBQ_BUILD_ROOT/core"

# ubq_portable_snapshot_renderer <build log> — brings the portable snapshot
# renderer up to date with this tree.
#
# run.py refuses a verdict from a renderer older than the Core sources it renders
# with, and rightly: a diff against a renderer nobody is running reads as a golden
# regression. Rebuilding here keeps a commit under portable Core from turning
# every checkout's run red until someone rebuilds by hand. Only when stale: the
# incremental build still costs eleven seconds with nothing to do.
#
# That rebuild compiles the whole portable Core, which no test's timeout is sized
# for. Under ctest the snapshot_renderer_fixture entry runs it with a build's
# width and budget, and UBQ_SNAPSHOT_RENDERER_PREBUILT tells the snapshots entry
# ordered after it that the renderer is current. There a stale renderer is
# refused rather than rebuilt: a source changed after the fixture, or the fixture
# did not finish, and rendering with it is the wrong verdict either way.
#
# - Returns: 0 when current, 1 when the build failed (the log says why), and 2
#   when the promise above stands and the renderer is stale.
ubq_portable_snapshot_renderer() { # <build log>
    local log="$1" binary="$PORTABLE_SNAPSHOT_BUILD/ubiquitty-snapshot" stale
    if stale="$(python3 "$ROOT/Scripts/snapshot-binary-is-current.py" \
            "$PORTABLE_SNAPSHOT_BUILD" "$binary" 2>&1)"; then
        return 0
    fi
    if [ "${UBQ_SNAPSHOT_RENDERER_PREBUILT:-0}" = "1" ]; then
        echo "ubq_portable_snapshot_renderer: $binary is out of date, and this entry may not rebuild it." >&2
        [ -z "$stale" ] || echo "  ${stale%%$'\n'*}" >&2
        echo "  CTest orders this entry after snapshot_renderer_fixture, which brings the renderer" >&2
        echo "  current for the run; a source changed since, or the fixture did not finish." >&2
        echo "  ./Scripts/run-tests.sh snapshots rebuilds it outside any test's timeout." >&2
        return 2
    fi
    echo "  the portable snapshot renderer is out of date; rebuilding it"
    "$ROOT/Scripts/build-core-snapshot.sh" >"$log" 2>&1 || return 1
}

if wanted libcore_fixture; then
    echo "=== libcore_fixture ==="
    if ! UBQ_LIBCORE_PREBUILT=0 ubq_libcore_consumer "$OUT/libcore-fixture-build.log" >/dev/null; then
        cat "$OUT/libcore-fixture-build.log" >&2
        build_broken "standalone Core fixture build failed"
    fi
    export UBQ_LIBCORE_PREBUILT=1
    RAN=$((RAN + 1))
fi

# The source-list guard and the consumer's stub-PAL assertions remain live on
# every invocation, including when a build fixture supplied the executable.
if wanted libcore_consumer; then
    echo "=== libcore_consumer ==="
    "$ROOT/Scripts/gen-portable-core-sources.sh" --check || {
        echo "libcore_consumer: portable-core-sources.txt is stale" >&2; exit 1; }
    if LC_BINARY="$(ubq_libcore_consumer "$OUT/libcore-consumer-build.log")"; then
        "$LC_BINARY" || exit 1
        echo "libcore_consumer: PASSED"
        export UBQ_LIBCORE_PREBUILT=1
        RAN=$((RAN + 1))
    else
        cat "$OUT/libcore-consumer-build.log" >&2
        build_broken "libcore_consumer: sample did not build"
    fi
fi

# test_v8_characterset_oob — AddressSanitizer negative for the saved-cursor
# character-set bounds check. A hostile checkpoint plants savedCharacterSetActive/GR
# outside [0,3]; restoreCursor (DECRC, ESC 8) applies the slot raw and the translate
# path indexes the 4-element characterSetG[] with it. The checkpoint decode refuses
# that screen state before anything is applied. Built + run under ASan: it encodes a
# crafted host checkpoint, applies it to a passive client, then drives ESC 8 + a
# graphic byte through Terminal::processBytes so the indexed characterSetG[] read runs.
# With the refusal the read is in-bounds (ASan-clean); without it ASan reports a
# heap-buffer-overflow READ in putChar (the GL and GR translate paths). Gated:
# skipped where the toolchain lacks a working -fsanitize=address runtime.
# Keep this narrow address-only build; sanitized_sync uses different sanitizer flags.
if wanted test_v8_characterset_oob; then
    echo "=== test_v8_characterset_oob (address) ==="
    bin="$OUT/test_v8_characterset_oob"
    slog="$OUT/v8-charset-oob-build.log"
    # Real link+run probe: some toolchains accept -fsanitize= at compile but
    # ship no runtime to link/run against. Probe with a tiny program that runs.
    probe="$OUT/santest_v8oob"
    printf 'int main(){return 0;}\n' > "$OUT/santest.cpp"
    # shellcheck disable=SC2086
    if ! $CXX_TEST -fsanitize=address "$OUT/santest.cpp" -o "$probe" 2>/dev/null \
            || ! "$probe" >/dev/null 2>&1; then
        ubq_skip "${CXX:-c++} has no working -fsanitize=address runtime"
        SKIPPED=$((SKIPPED + 1))
    elif YYJSON_OBJECT="$(ubq_yyjson_object "$OUT/objects/yyjson.o" 2>>"$slog")" \
            && ubq_compile_parallel "$bin" "$OUT/objects/v8_charset_oob" "$slog" \
            -std=c++23 -I"$ROOT/Tests" -Wall -Wextra -Werror \
            -Wno-deprecated-declarations -Wno-multichar -Wno-comment \
            -fno-exceptions -fno-rtti -g -O1 -fsanitize=address \
            -I"$ROOT/Source" -- \
            "$ROOT/Tests/Core/test_v8_characterset_oob.cpp" \
            "$ROOT/Source/Core/Protocol/Sync/NativeAttachment.cpp" \
            "${SYNC_SRCS[@]}" \
            "$ROOT/Source/Core/Protocol/Sync/SyncClientCore.cpp" \
            "$ROOT/Source/Core/Preferences/Preferences.cpp" \
            "$ROOT/Source/Core/Preferences/PreferencesPaths.cpp" \
            "$ROOT/Source/Core/Preferences/PreferencesWriter.cpp" \
            "$ROOT/Source/Platform/POSIX/Preferences.cpp" \
            "$ROOT/Source/Core/Preferences/Profile.cpp" \
            "${CORE_SRCS[@]}" -- "$YYJSON_OBJECT"; then
        ubq_require_sanitized address "$bin" test_v8_characterset_oob || exit 1
        "$bin" || exit 1
        RAN=$((RAN + 1))
    else
        cat "$slog" >&2
        build_broken "test_v8_characterset_oob build failed (see $slog)"
    fi
fi

# test_term_lifecycle — AddressSanitizer guard for the bare Terminal lifecycle.
# Display::init's grow path frees every buffer before the first alloc, so a
# Display buffer pointer the ctor leaves uninitialized turns a plain `Terminal t;`
# into delete[] of a garbage pointer — UB that only ASan surfaces (release
# masks it). Gated: skipped where the toolchain lacks a working ASan runtime.
# Keep this narrow address-only build; sanitized_sync uses different sanitizer flags.
if wanted test_term_lifecycle; then
    echo "=== test_term_lifecycle (address) ==="
    bin="$OUT/test_term_lifecycle"
    slog="$OUT/term-lifecycle-build.log"
    probe="$OUT/santest_lifecycle"
    printf 'int main(){return 0;}\n' > "$OUT/santest.cpp"
    # shellcheck disable=SC2086
    if ! $CXX_TEST -fsanitize=address "$OUT/santest.cpp" -o "$probe" 2>/dev/null \
            || ! "$probe" >/dev/null 2>&1; then
        ubq_skip "${CXX:-c++} has no working -fsanitize=address runtime"
        SKIPPED=$((SKIPPED + 1))
    elif ubq_compile_parallel "$bin" "$OUT/objects/term_lifecycle" "$slog" \
            -std=c++23 -I"$ROOT/Tests" -Wall -Wextra -Werror \
            -Wno-deprecated-declarations -Wno-multichar -Wno-comment \
            -fno-exceptions -fno-rtti -g -O1 -fsanitize=address \
            -I"$ROOT/Source" -- \
            "$ROOT/Tests/Core/test_term_lifecycle.cpp" \
            "${CORE_SRCS[@]}"; then
        ubq_require_sanitized address "$bin" test_term_lifecycle || exit 1
        "$bin" || exit 1
        RAN=$((RAN + 1))
    else
        cat "$slog" >&2
        build_broken "test_term_lifecycle build failed (see $slog)"
    fi
fi

# test_themes_uaf — AddressSanitizer negative for the custom-theme
# registry's pointer stability. The registry hands out a const Theme*
# (customGetInternal) that callers hold across a later registerCustom. It is a
# std::deque, whose elements never move on push_back, so a held pointer stays
# valid across registerCustom/replaceCustom (only clearCustom invalidates it);
# a std::vector's geometric growth would reallocate the backing buffer and free
# the old one, dangling every held Theme*.
# The test registers a theme, captures its internal pointer, registers 256 more
# to force what would be several vector reallocations, then reads a field
# through the held pointer. With the deque this is ASan-clean; reverting the
# registry to std::vector makes ASan report a heap-use-after-free READ at the
# held-pointer read (freed by registerCustom's realloc). ThemesCustom.cpp's only
# cross-TU dependency is builtInCount(), which the test stubs, so the registry
# resolves without the built-in theme registry. Skipped without an ASan runtime.
# This registry-only link keeps its stub builtInCount and address-only instrumentation.
if wanted test_themes_uaf; then
    echo "=== test_themes_uaf (address) ==="
    bin="$OUT/test_themes_uaf"
    slog="$OUT/themes-uaf-build.log"
    probe="$OUT/santest_themesuaf"
    printf 'int main(){return 0;}\n' > "$OUT/santest.cpp"
    # shellcheck disable=SC2086
    if ! $CXX_TEST -fsanitize=address "$OUT/santest.cpp" -o "$probe" 2>/dev/null \
            || ! "$probe" >/dev/null 2>&1; then
        ubq_skip "${CXX:-c++} has no working -fsanitize=address runtime"
        SKIPPED=$((SKIPPED + 1))
    elif ubq_compile_parallel "$bin" "$OUT/objects/test_themes_uaf" "$slog" \
            -std=c++23 -I"$ROOT/Tests" -Wall -Wextra -Werror -Wno-deprecated-declarations -Wno-multichar -Wno-comment -fno-exceptions -fno-rtti -g -O1 -fsanitize=address -I"$ROOT/Source" -DUBIQUITTY_TEST_WITHOUT_CORE -- \
            "$ROOT/Tests/Core/Themes/test_themes_uaf.cpp" \
            "$ROOT/Source/Core/Themes/ThemesCustom.cpp"; then
        ubq_require_sanitized address "$bin" test_themes_uaf || exit 1
        "$bin" || exit 1
        RAN=$((RAN + 1))
    else
        cat "$slog" >&2
        build_broken "test_themes_uaf build failed (see $slog)"
    fi
fi







# PTY backend smoke (forkpty + non-blocking master + TIOCSWINSZ +
# child reap). Drives the real macOS Stream backend against
# /bin/echo, /bin/cat, /usr/bin/false and /bin/stty. Requires a Unix
# PTY subsystem with <util.h>::forkpty, so we run it only on Darwin;
# Linux gets its own variant when that backend lands.
#
# The shared test target links Core because the interrupt-under-flood cases
# drain the PTY through a live Terminal that answers the program's queries.
if [ "$(uname -s)" = "Darwin" ] && wanted test_pty; then
    NAME=test_pty
    OUT_BIN="$OUT/$NAME"
    mkdir -p "$OUT"
    echo "=== $NAME ==="
    ubq_build_test test_pty "$OUT_BIN" "$OUT/test_pty-build.log" || { cat "$OUT/test_pty-build.log" >&2; exit 1; }
    "$OUT_BIN" || exit 1
    RAN=$((RAN + 1))
fi

# Serial / RS-232 Stream backend (SerialStream.cpp): opens a real tty (a forkpty'd
# PTY slave stands in for a serial line) in raw 8N1 non-blocking mode and round-
# trips bytes against the master. Needs <util.h>::openpty + termios, so Darwin-only
# (gated alongside test_pty); Linux gets its own variant when that backend lands.
if [ "$(uname -s)" = "Darwin" ] && wanted test_serial; then
    NAME=test_serial
    OUT_BIN="$OUT/$NAME"
    mkdir -p "$OUT"
    echo "=== $NAME ==="
    # No Preferences on this link line -- the stream backend and the output
    # queue neither read config nor pull in Core -- so the harness must not pin
    # it, or the pin is an undefined symbol.
    ubq_compile_parallel "$OUT_BIN" "$OUT/objects/test_serial" "$OUT/test_serial-build.log" \
            -std=c++23 -I"$ROOT/Tests" -Wall -Wextra -Werror -fno-exceptions -fno-rtti -DUBIQUITTY_TEST_WITHOUT_CORE -I"$ROOT/Source" -- \
            "$ROOT/Tests/Platform/test_serial.cpp" \
            "$ROOT/Source/Platform/POSIX/SerialStream.cpp" \
            "$ROOT/Source/PAL/OutputQueue.cpp" || { cat "$OUT/test_serial-build.log" >&2; exit 1; }
    "$OUT_BIN" || exit 1
    RAN=$((RAN + 1))
fi




if wanted test_display; then
    NAME=test_display
    OUT_BIN="$OUT/$NAME"
    mkdir -p "$OUT"
    echo "=== $NAME ==="
    ubq_build_test test_display "$OUT_BIN" "$OUT/test_display-build.log" || { cat "$OUT/test_display-build.log" >&2; exit 1; }
    "$OUT_BIN" || exit 1
    RAN=$((RAN + 1))
fi

# Public BB-shadow surface used by the PAL's scrollback view-offset
# painter (readShadowCell / writeShadowCell / markShadowValid /
# invalidateRowShadow / shiftShadowForViewOffset). Kept separate from
# test_display so the per-test signal stays narrow.
if wanted test_display_shadow; then
    NAME=test_display_shadow
    OUT_BIN="$OUT/$NAME"
    mkdir -p "$OUT"
    echo "=== $NAME ==="
    ubq_build_test test_display_shadow "$OUT_BIN" "$OUT/test_display_shadow-build.log" || { cat "$OUT/test_display_shadow-build.log" >&2; exit 1; }
    "$OUT_BIN" || exit 1
    RAN=$((RAN + 1))
fi


# test_session_stress — the tmux-session concurrency contract: an ingest
# thread (deliver / split-unsplit / kill-pane-while-writing / window-close /
# %exit) racing a render thread (lookup + read-under-pane-lock + reclaim-on-
# unbind) over one Tmux::Session. Built + run under ThreadSanitizer (a missing
# lock / bad lock order) and AddressSanitizer+LeakSanitizer (a pane-Terminal UAF /
# double-free / leak) — the failure classes the GUI ASan soak would catch, but
# here headless and deterministic. Skipped where the toolchain lacks a sanitizer.
# Keep each sanitizer variant on this terminal-only link set.
if wanted test_session_stress; then
    TMUX_STRESS_SRCS=(
        "$ROOT/Tests/Core/Protocol/Tmux/test_session_stress.cpp"
        "$ROOT/Source/Core/Protocol/Tmux/Session.cpp"
        "$ROOT/Source/Core/Protocol/Tmux/Commands.cpp"
        "$ROOT/Source/Core/Protocol/Tmux/PaneLayout.cpp"
        "$ROOT/Source/Core/Panes/PaneTree.cpp"
        "${CORE_SRCS[@]}"
    )
    for san in thread address; do
        echo "=== test_session_stress ($san) ==="
        RAN=$((RAN + 1))
        bin="$OUT/test_session_stress_$san"
        slog="$OUT/tmux-stress-$san.log"
        # Real link+run probe: some toolchains accept -fsanitize= at compile but
        # ship no runtime .so (e.g. Fedora g++), so a syntax-only check passes yet
        # the link fails. Probe with a tiny program that must also RUN.
        probe="$OUT/santest_$san"
        printf 'int main(){return 0;}\n' > "$OUT/santest.cpp"
        # shellcheck disable=SC2086
        if ! $CXX_TEST -fsanitize="$san" "$OUT/santest.cpp" -o "$probe" 2>/dev/null \
                || ! "$probe" >/dev/null 2>&1; then
            ubq_skip "${CXX:-c++} has no working -fsanitize=$san runtime"
            SKIPPED=$((SKIPPED + 1))
            continue
        fi
        # Per-unit rather than one command over the whole list: this stanza names
        # every Core source and builds them twice over, which is nearly all of
        # its wall time and none of what it tests.
        if ubq_compile_parallel "$bin" "$OUT/objects/session_stress_$san" "$slog" \
                -std=c++23 -I"$ROOT/Tests" -Wall -Wextra -Wno-deprecated-declarations \
                -fno-rtti -g -O1 -pthread -fsanitize="$san" \
                -I"$ROOT/Source" -- "${TMUX_STRESS_SRCS[@]}"; then
            # ASan run (LeakSanitizer is on by default on Linux; macOS ASan has no
            # LSan, and forcing detect_leaks there aborts — so don't force it).
            # TSan: halt on the first race.
            ubq_require_sanitized "$san" "$bin" "test_session_stress ($san)" || exit 1
            if [ "$san" = "address" ]; then
                "$bin" || exit 1
            else
                TSAN_OPTIONS=halt_on_error=1 "$bin" || exit 1
            fi
        else
            cat "$slog" >&2
            build_broken "test_session_stress ($san) build failed (see $slog)"
        fi
    done
fi

# fuzz_harnesses_compile — every libFuzzer harness still compiles, on a machine
# that cannot link one.
#
# The harnesses are built under --with-fuzz, which defaults off, and that block
# carries no stanza name — so --list-stanzas does not report it, CMake registers
# no ctest entry, and the only run that reaches it is an unfiltered
# run-tests.sh --with-fuzz. libFuzzer being installed changes none of that.
# A harness can stop compiling while the suite stays green: harnesses reach
# Terminal's fields and the grid bounds through headers they do not include,
# and a header moving out of a folder takes those away silently.
#
# Syntax-only, so it runs everywhere the fuzz stanza does not. The list comes
# from the tree rather than from a table here — a harness added tomorrow is
# covered without anyone remembering this file, and a table would be one more
# thing that can be narrower than its subject.
if wanted fuzz_harnesses_compile; then
    echo "=== fuzz_harnesses_compile ==="
    RAN=$((RAN + 1))
    fuzz_sources=$(find "$ROOT/Tests" -name 'fuzz_*.cpp' | sort)
    if [ -z "$fuzz_sources" ]; then
        echo "fuzz_harnesses_compile: no fuzz_*.cpp under Tests — the naming" >&2
        echo "  convention changed and nothing was compiled." >&2
        exit 1
    fi
    fuzz_broken=0
    fuzz_checked=0
    for fuzz_source in $fuzz_sources; do
        fuzz_checked=$((fuzz_checked + 1))
        if ! $CXX_TEST -std=c++23 -fsyntax-only \
                -I"$ROOT/Source" -I"$ROOT/Tests" -I"$ROOT/ThirdParty" \
                "$fuzz_source" 2>"$OUT/fuzz-compile.log"; then
            echo "  FAILED   ${fuzz_source#$ROOT/}" >&2
            grep -E 'error:' "$OUT/fuzz-compile.log" | head -3 >&2
            fuzz_broken=$((fuzz_broken + 1))
            if [ "${UBQ_TEST_FAIL_FAST:-0}" = "1" ]; then
                echo "fuzz_harnesses_compile: fail-fast — remaining harnesses were not compiled" >&2
                exit 1
            fi
        fi
    done
    if [ "$fuzz_broken" -ne 0 ]; then
        echo "fuzz_harnesses_compile: $fuzz_broken of $fuzz_checked harness(es) do not" >&2
        echo "  compile. A harness that does not build is a corpus nothing replays," >&2
        echo "  and the fuzz stanza reports SKIP rather than saying so." >&2
        exit 1
    fi
    echo "  ok       $fuzz_checked harness(es) compile"
fi

# host_binary — build the Host frontend, so the stanzas that run against it do
# not each build it themselves.
#
# Registered as a ctest fixture, which is what makes it run first and once. On
# its own it is an ordinary stanza and builds the tree; the stanzas that need
# the binary then find it and only read.
if wanted host_binary; then
    echo "=== host_binary ==="
    RAN=$((RAN + 1))
    if ! command -v cmake >/dev/null 2>&1; then
        ubq_skip "cmake not found"
        SKIPPED=$((SKIPPED + 1))
    else
        # Built by this stanza whatever the caller says. This is the fixture, so
        # honouring UBQ_HOST_PREBUILT would make it assert that somebody else had
        # already done its job.
        UBQ_HOST_PREBUILT=0 host_binary_path="$(ubq_host_binary "$OUT/host-binary-build.log")" \
            || build_broken "host_binary: the Host frontend did not build (see $OUT/host-binary-build.log)"
        [ -x "$host_binary_path" ] \
            || build_broken "host_binary: cmake reported success and produced no $host_binary_path"
        echo "  ok       $host_binary_path"
    fi
fi

# agent_binary — build ubq once, so the stanzas that run it do not each build
# it, as host_binary does for the Host frontend.
if wanted agent_binary; then
    echo "=== agent_binary ==="
    RAN=$((RAN + 1))
    if ! command -v cmake >/dev/null 2>&1; then
        ubq_skip "cmake not found"
        SKIPPED=$((SKIPPED + 1))
    else
        UBQ_AGENT_PREBUILT=0 agent_binary_path="$(ubq_agent_binary "$OUT/agent-binary-build.log")" \
            || build_broken "agent_binary: ubq did not build (see $OUT/agent-binary-build.log)"
        echo "  ok       $agent_binary_path"
    fi
fi

# CTest orders this no-op after the process-level sweep and before every other
# shell stanza. Direct and filtered invocations keep the sweep above, while a
# scheduled suite performs it once instead of letting concurrent consumers
# inspect and remove each other's shared CMake trees.
if wanted cache_sweep_fixture; then
    echo "=== cache_sweep_fixture ==="
    RAN=$((RAN + 1))
fi

# The three Host build fixtures configure against one dependency prefix. This
# fixture establishes it once before any of those independent CMake trees can
# trigger the dependency builder concurrently.
if wanted quic_deps_fixture; then
    echo "=== quic_deps_fixture ==="
    RAN=$((RAN + 1))
    "$ROOT/Scripts/ensure-quic-deps.sh" "$QUIC_PREFIX" \
        || build_broken "quic_deps_fixture build failed"
    quic_deps_present --quiet || build_broken "quic_deps_fixture produced incomplete archives"
fi

# The QUIC shell targets are excluded from the ordinary Core build. Their
# dependency fixture supplies the TLS archives before this target build runs.
if wanted quic_test_fixture && quic_deps_present; then
    echo "=== quic_test_fixture ==="
    if ! ubq_build_test_targets "$OUT/quic-test-fixture.log" \
            ubq-shell-quic_channel_lifecycle ubq-shell-quic_daemon_accept ubq-shell-quic_bound_socket; then
        cat "$OUT/quic-test-fixture.log" >&2
        build_broken "QUIC test fixture build failed"
    fi
    RAN=$((RAN + 1))
fi

# These fixture stanzas build the two shared Host object trees once. CTest sets
# the matching PREBUILT promise only for consumers ordered after the fixture;
# a directly filtered stanza keeps its on-demand configure/build behavior.
if wanted host_noquic_fixture; then
    echo "=== host_noquic_fixture ==="
    RAN=$((RAN + 1))
    host_noquic_build="$UBQ_BUILD_ROOT/host-noquic"
    UBQ_HOST_NOQUIC_PREBUILT=0 cmake -B "$host_noquic_build" -S "$ROOT" -G "$UBQ_GENERATOR" \
        -DUBIQUITTY_FRONTEND=Host -DUBIQUITTY_QUIC=ON -DCMAKE_BUILD_TYPE=Debug \
        >"$OUT/host-noquic-fixture.log" 2>&1 \
        && UBQ_HOST_NOQUIC_PREBUILT=0 cmake --build "$host_noquic_build" -j"$BUILD_JOBS" \
            >>"$OUT/host-noquic-fixture.log" 2>&1 \
        || build_broken "host_noquic_fixture build failed (see $OUT/host-noquic-fixture.log)"
fi

if wanted host_quic_fixture && quic_deps_present; then
    echo "=== host_quic_fixture ==="
    RAN=$((RAN + 1))
    host_quic_build="$UBQ_BUILD_ROOT/host-quic"
    UBQ_HOST_QUIC_PREBUILT=0 cmake -B "$host_quic_build" -S "$ROOT" -G "$UBQ_GENERATOR" \
        -DUBIQUITTY_FRONTEND=Host -DUBIQUITTY_QUIC=ON -DCMAKE_BUILD_TYPE=Debug \
        >"$OUT/host-quic-fixture.log" 2>&1 \
        && UBQ_HOST_QUIC_PREBUILT=0 cmake --build "$host_quic_build" -j"$BUILD_JOBS" \
            >>"$OUT/host-quic-fixture.log" 2>&1 \
        || build_broken "host_quic_fixture build failed (see $OUT/host-quic-fixture.log)"
fi

# sanitizer_guard — the guard that refuses a clean run from an uninstrumented
# binary, checked against binaries whose answer is known.
#
# ubq_require_sanitized stands between a stanza whose -fsanitize flag went
# missing and a green tick, and nothing exercised it. It reported a binary as
# uninstrumented that carried the runtime, which is the verdict that costs the
# most: the reader is sent to a compile line that is correct.
#
# So the verdicts are produced from binaries rather than asserted about, and all
# four are asked for in one run. A function answering the same way for every
# input satisfies any single row, and the row it would hide — built without the
# flag — is the one the guard exists for.
if wanted sanitizer_guard; then
    echo "=== sanitizer_guard ==="
    RAN=$((RAN + 1))
    guarddir="$OUT/objects/sanitizer_guard"
    mkdir -p "$guarddir"
    printf 'int main(){return 0;}\n' > "$guarddir/probe.cpp"
    printf 'not an object file\n' > "$guarddir/junk"
    rm -f "$guarddir/absent"
    if $CXX_TEST -fsanitize=thread "$guarddir/probe.cpp" -o "$guarddir/instrumented" 2>/dev/null \
            && "$guarddir/instrumented" >/dev/null 2>&1 \
            && $CXX_TEST "$guarddir/probe.cpp" -o "$guarddir/plain" 2>/dev/null; then
        guard_failures=0
        guard_verdicts=""
        for expectation in \
                "instrumented:0:carries the runtime" \
                "plain:1:built without the flag" \
                "absent:2:no file at the path" \
                "junk:3:no symbol table to read from"; do
            name="${expectation%%:*}"; rest="${expectation#*:}"
            want="${rest%%:*}"; what="${rest#*:}"
            set +e
            ubq_sanitized_binary thread "$guarddir/$name"
            got=$?
            set -e
            guard_verdicts="$guard_verdicts $got"
            if [ "$got" = "$want" ]; then
                echo "  ok       $what -> $got"
            else
                echo "  FAILED   $what -> $got (expected $want)" >&2
                guard_failures=$((guard_failures + 1))
            fi
        done
        if [ "$guard_failures" -ne 0 ]; then
            echo "sanitizer_guard: ubq_sanitized_binary does not separate its verdicts;" >&2
            echo "  a stanza built without -fsanitize would run clean and be believed." >&2
            exit 1
        fi
        # The four have to be four. Equal expectations passing individually while
        # the function collapses them is not reachable from the rows above.
        if [ "$(printf '%s\n' $guard_verdicts | sort -u | wc -l)" -ne 4 ]; then
            echo "sanitizer_guard: the four verdicts are not distinct:$guard_verdicts" >&2
            exit 1
        fi
    else
        ubq_skip "${CXX:-c++} has no working -fsanitize=thread runtime"
        SKIPPED=$((SKIPPED + 1))
    fi
fi

# Concurrent runtime publication must be race-free as well as coherent.
if wanted tsan_runtimelink; then
    echo "=== tsan_runtimelink (thread) ==="
    RAN=$((RAN + 1))
    bin="$OUT/tsan_runtimelink"
    slog="$OUT/tsan_runtimelink.log"
    probe="$OUT/santest_thread"
    printf 'int main(){return 0;}\n' > "$OUT/santest.cpp"
    if ! $CXX_TEST -fsanitize=thread "$OUT/santest.cpp" -o "$probe" 2>/dev/null \
            || ! "$probe" >/dev/null 2>&1; then
        ubq_skip "${CXX:-c++} has no working -fsanitize=thread runtime"
        SKIPPED=$((SKIPPED + 1))
    elif ubq_compile_parallel "$bin" "$OUT/objects/tsan_runtimelink" "$slog" \
            -std=c++23 -I"$ROOT/Tests" -Wall -Wextra -fno-rtti -g -O1 -pthread -fsanitize=thread \
            -DUBIQUITTY_TEST_WITHOUT_CORE -I"$ROOT/Source" -- \
            "$ROOT/Tests/Core/Runtime/test_runtimelink.cpp"; then
        ubq_require_sanitized thread "$bin" tsan_runtimelink || exit 1
        TSAN_OPTIONS=halt_on_error=1 "$bin" || exit 1
    else
        cat "$slog" >&2
        build_broken "tsan_runtimelink build failed (see $slog)"
    fi
fi

# tsan_trie_race — the Unicode width/grapheme-break trie lazy-init contract
# The first build of each trie is gated by std::call_once, so two
# parse threads (two panes) first-touching a trie at once can't race the build.
# 4 threads concurrently first-touch both tries via trie-forcing code points
# (combining mark / wide CJK / emoji / ZWJ — never an ASCII fast path); under
# ThreadSanitizer the call_once-gated build is serialized + published-before-read
# so TSAN is clean. The test compiles the real Width.cpp + GraphemeBreak.cpp with
# UBIQUITTY_UNICODE_NO_EAGER_PROPS_INIT so the eager startup build doesn't pre-fire
# the call_once gates single-threaded — the worker threads reach the lazy gate
# first-touch, exactly the two-pane race the gate exists for. (Replacing the
# call_once with `if (!ready) { build; ready = true; }` makes this TSAN report a
# data race on the trie globals — the non-vacuity check.) Skipped where the
# toolchain lacks TSan.
# The lazy-init seam needs its own thread-instrumented translation unit.
if wanted tsan_trie_race; then
    echo "=== tsan_trie_race (thread) ==="
    RAN=$((RAN + 1))
    bin="$OUT/tsan_trie_race"
    slog="$OUT/tsan_trie_race.log"
    # Real link+run probe: some toolchains accept -fsanitize=thread at compile but
    # ship no runtime, so probe with a tiny program that must also RUN.
    probe="$OUT/santest_thread"
    printf 'int main(){return 0;}\n' > "$OUT/santest.cpp"
    if ! $CXX_TEST -fsanitize=thread "$OUT/santest.cpp" -o "$probe" 2>/dev/null \
            || ! "$probe" >/dev/null 2>&1; then
        ubq_skip "${CXX:-c++} has no working -fsanitize=thread runtime"
        SKIPPED=$((SKIPPED + 1))
    elif ubq_compile_parallel "$bin" "$OUT/objects/tsan_trie_race" "$slog" \
            -std=c++23 -I"$ROOT/Tests" -Wall -Wextra -Wno-deprecated-declarations -fno-rtti -g -O1 -pthread -fsanitize=thread -DUBIQUITTY_UNICODE_NO_EAGER_PROPS_INIT -I"$ROOT/Source" -- \
            "$ROOT/Tests/Core/Unicode/tsan_trie_race.cpp"; then
        ubq_require_sanitized thread "$bin" tsan_trie_race || exit 1
        TSAN_OPTIONS=halt_on_error=1 "$bin" || exit 1
    else
        cat "$slog" >&2
        build_broken "tsan_trie_race build failed (see $slog)"
    fi
fi






# Whether $FUZZ_CC can actually BUILD a libFuzzer target, which is a different
# question from whether it accepts the flag.
#
# `-fsanitize=fuzzer -E` stops after preprocessing. Apple's clang accepts the
# flag there and then fails at link with
#
#     ld: library '…/lib/darwin/libclang_rt.fuzzer_osx.a' not found
#
# which is the failure the comment below already describes. Probed by
# preprocessing, that arrives as `ubq_skip "does not support -fsanitize=fuzzer"`
# — so on a stock Xcode toolchain every fuzz lane reports as unavailable rather
# than as broken, and the two are indistinguishable in the summary.
#
# Runs once and caches, because thirteen lanes ask.
# libFuzzer writes newly-discovered units into the FIRST corpus directory it is
# given and reads the rest. Handed only the tracked corpus, a run adds thousands
# of files to it and prunes what it considers redundant — a single run can
# delete a tracked seed, invisible in `git status` behind thousands of untracked
# additions. A seed is in the corpus because something needed it to be reachable,
# so a run that removes one silently removes coverage.
#
# Every run writes to a scratch directory under the build tree and reads the
# tracked corpus, which stays an input.
ubq_fuzz_scratch() {
    mkdir -p "$OUT/$1-corpus"
}

ubq_fuzz_run() {
    python3 "$ROOT/Scripts/run-fuzzer.py" --log-dir "$OUT/fuzz-logs" -- "$@"
}

# The tracked corpora, and whether a run left them as it found them.
#
# Redirecting output keeps the corpus an input; this is the check that it stays
# one. A run
# that adds or removes tracked seeds is not a failing test — it is a passing one
# that quietly rewrote its own inputs, so nothing downstream reports it and the
# next reader sees a corpus nobody chose.
ubq_fuzz_corpus_state() {
    git -C "$ROOT" status --porcelain --untracked-files=all -- \
        "*fuzz-corpus*" "*kitty-corpus*" "*sixel-corpus*" "*tiff-corpus*" \
        "*png-corpus*" "*bootstrap-corpus*" "Tests/Corpus/sync/fuzz_sync_screen/*" "Tests/Corpus/sync/fuzz_sync_catalog/*" "Tests/Corpus/sync/fuzz_sync_connection/*" 2>/dev/null | sort
}

# A tracked corpus entry whose name starts with a dot is a filesystem artefact,
# not a seed anyone chose — a partially-written unit such as `.!73677!<sha>`,
# swept in by a bulk `git add`. libFuzzer replays dotfiles like any other unit,
# so one looks like working coverage under a name that says the opposite.
#
# Checked here rather than in a self-test, because the entries arrive from runs
# and refactors rather than from anyone editing this file.
# Where a crash lands. Without -artifact_prefix, libFuzzer writes crash-, leak-
# and timeout- artefacts into the CURRENT directory, which for this suite is the
# repository root: untracked files beside the source tree, and one route by
# which a partially-written unit reaches the tracked corpus under a `.!PID!`
# name. Every run above writes them under the build tree instead.
#
# The artefacts are written, not suppressed: a crash unit names the input the
# run was on when it stopped, which is where an investigation starts. Only the
# destination changes.
ubq_fuzz_corpus_names() {
    ubq_dotted="$(git -C "$ROOT" ls-files 'Tests/**corpus*/.*' 2>/dev/null)"
    if [ -n "$ubq_dotted" ]; then
        echo "FAIL: tracked corpus entries named as filesystem artefacts:" >&2
        printf '%s\n' "$ubq_dotted" | head -10 >&2
        echo "  Rename to the unit's content hash — libFuzzer replays these, so the" >&2
        echo "  name is wrong and the coverage is real." >&2
        return 1
    fi
    return 0
}

ubq_fuzz_links() {
    if [ -n "${UBQ_FUZZ_LINK_STATUS:-}" ]; then
        return "$UBQ_FUZZ_LINK_STATUS"
    fi
    ubq_fuzz_probe="$OUT/fuzz-link-probe"
    printf '#include <cstddef>\n#include <cstdint>\nextern "C" int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n) { (void)d; (void)n; return 0; }\n' \
        > "$ubq_fuzz_probe.cpp"
    if "$FUZZ_CC" -fsanitize=fuzzer -std=c++17 -o "$ubq_fuzz_probe" "$ubq_fuzz_probe.cpp" \
            > "$ubq_fuzz_probe.log" 2>&1; then
        UBQ_FUZZ_LINK_STATUS=0
    else
        UBQ_FUZZ_LINK_STATUS=1
    fi
    rm -f "$ubq_fuzz_probe" "$ubq_fuzz_probe.cpp"
    return "$UBQ_FUZZ_LINK_STATUS"
}

# Optional fuzzer. Requires Clang with libFuzzer (most stock Apple Clang
# builds have it via -fsanitize=fuzzer). On systems without it, skip
# silently — fuzz is an opt-in extra, not a gate.
if [ "$WITH_FUZZ" -eq 1 ]; then
    UBQ_FUZZ_CORPUS_BEFORE="$(ubq_fuzz_corpus_state)"
    ubq_fuzz_corpus_names || exit 1
    # Removed before anything is built, because the gate below reads "the binary
    # is there" as "this run built and ran it". The output directory sits
    # outside the checkout, under DerivedData, where a runner's workspace clean
    # does not reach — so a harness that starts skipping leaves an earlier run's
    # binary behind to answer for a tree it never saw.
    #
    # Named one at a time from the same list the gate requires. A `fuzz_*` glob
    # also takes the per-harness `fuzz_X-corpus` directories beside the
    # binaries: `rm -f` refuses a directory, which ends the run under this
    # file's errexit, and a glob that did remove them would throw away the seeds
    # the run is supposed to start from. No guard on existence, because `rm -f`
    # is satisfied by a name that is not there and none of these names is ever a
    # directory.
    for fuzzBinary in $(find "$ROOT/Tests" -name 'fuzz_*.cpp' -exec basename {} .cpp \; | sort); do
        rm -f "$OUT/$fuzzBinary"
    done
    echo "=== fuzz_vt100 ==="
    # Fuzzer compiler. Apple's clang++ accepts -fsanitize=fuzzer but ships no
    # libclang_rt.fuzzer_osx.a to link against; Homebrew LLVM bundles the
    # runtime. Prefer it when present (keg-only, not on PATH) so --with-fuzz
    # works out of the box; an explicit FUZZ_CC always wins.
    if [ -z "${FUZZ_CC:-}" ] && [ -x /opt/homebrew/opt/llvm/bin/clang++ ]; then
        FUZZ_CC="/opt/homebrew/opt/llvm/bin/clang++"
    else
        FUZZ_CC="${FUZZ_CC:-clang++}"
    fi
    # The same rider CXX_TEST carries, for the same reason: every harness here
    # expands CORE_SRCS, and Core/Codec/Base64.cpp and Core/Unicode/UTF8Decoder.cpp
    # include `simdutf.h` unconditionally. Riding with the compiler is one place
    # to get it right instead of one per harness.
    #
    # FUZZ_CC stays the bare program, because `command -v` and the
    # -fsanitize=fuzzer probe below test the compiler and not a command line.
    #
    # The launcher rides with the compiler here for the reason it rides with
    # every other compile in this file. Unquoted beside the compiler, since
    # folding it in would resolve as a binary called "ccache clang++", and empty
    # when there is no ccache.
    FUZZ_CXX="$UBQ_COMPILER_LAUNCHER $FUZZ_CC -I$ROOT/Source/ThirdParty/simdutf"
    FUZZ_LOG="$OUT/fuzz-build.log"
    # One flag set for every harness, so one object serves them all: the twelve
    # harnesses below link the same instrumented Core, and an object compiled
    # for fuzz_vt100 is the object fuzz_kitty links. A compile-and-link of a
    # whole source list per harness would never be cached (the launcher caches
    # a `-c` compile of one file), so each harness would compile the same sixty
    # Core files uncached, one after another. The object directory is keyed on
    # the compiler and the flags, so a change to either compiles afresh rather
    # than linking objects built another way. The RE2 root is on every
    # harness's path because ScrollSearch and the vendored RE2 units it matches
    # with include from it.
    FUZZ_FLAGS=(-std=c++23 -I"$ROOT/Tests" -fno-exceptions -fno-rtti -O1 -g
        "-fsanitize=fuzzer,address,undefined" -fno-sanitize-recover=undefined
        -I"$ROOT/Source" -isystem "$ROOT/Source/ThirdParty/re2")
    FUZZ_OBJDIR="$OUT/fuzz-objects-$(printf '%s\n' "$FUZZ_CXX" "${FUZZ_FLAGS[@]}" | cksum | cut -d' ' -f1)"
    # Compiles a harness's sources BUILD_JOBS wide into the shared objects and
    # links the harness, or ends the run BUILD-BROKEN. It is reached only
    # past the two probes that decline: a compiler that is absent, and one
    # that cannot link a libFuzzer target at all. Past them the toolchain
    # builds fuzzers, so a harness that does not compile or link is a broken
    # tree, on every machine and whether or not the run requires its
    # fuzzers: a decline here exits 0 with the harness's coverage gone.
    # The vendored JSON parser as a fuzz object: C, so compiled apart from the
    # C++ command line, and instrumented as everything it is linked with is.
    # -no-link so the object carries coverage without libFuzzer's main.
    ubq_yyjson_fuzz_object() { # <object>
        "${FUZZ_CC%++}" -std=c99 -w -O1 -g -fsanitize=fuzzer-no-link,address,undefined \
            -c "$ROOT/Source/ThirdParty/yyjson/yyjson.c" -o "$1"
    }
    ubq_fuzz_build() { # <harness> <error log> <source…> [-- <link input…>]
        local harness="$1" errorlog="$2"
        shift 2
        if UBQ_COMPILE_CXX="$FUZZ_CXX" UBQ_COMPILE_LINK="$FUZZ_CXX" \
                ubq_compile_parallel "$OUT/$harness" "$FUZZ_OBJDIR" "$errorlog" "${FUZZ_FLAGS[@]}" -- "$@"; then
            return 0
        fi
        echo "$harness: did not build — first errors from $errorlog:" >&2
        grep -m 5 -E 'error:|undefined reference|Undefined symbols' "$errorlog" >&2 || true
        build_broken "$harness build failed (see $errorlog)"
    }
    # The consent model fuzz_sync and fuzz_consent share (ConsentFuzz.h) drives
    # the typed Call decoder, the structure and question Notifies, the schema
    # check behind them, the adopt carrier and the object IDs they name. The
    # Call decoder's TU also answers pane searches, which reach ScrollSearch and
    # the vendored RE2 it matches with.
    CONSENT_FUZZ_SRCS=(
        "$ROOT/Source/Core/Protocol/Local/AdoptWire.cpp"
        "$ROOT/Source/Core/Protocol/Sync/HandoverBody.cpp"
        "$ROOT/Source/Core/Protocol/Sync/HandoverChannel.cpp"
        "$ROOT/Source/Core/Protocol/Sync/HandoverGround.cpp"
        "$ROOT/Source/PAL/OutputQueue.cpp"
        "$ROOT/Source/Core/Protocol/Sync/Control.cpp"
        "$ROOT/Source/Core/Protocol/ControlRPC.cpp"
        "$ROOT/Source/Core/Screen/ScrollSearch.cpp"
        "$ROOT/Source/Core/Screen/ScrollSearchResults.cpp"
        "$ROOT/Source/Core/Unicode/Normalize.cpp"
        "$ROOT"/Source/ThirdParty/re2/re2/*.cc
        "$ROOT/Source/ThirdParty/re2/util/rune.cc"
        "$ROOT/Source/ThirdParty/re2/util/strutil.cc"
        "$ROOT/Source/Core/Protocol/Control/ConsentQuestion.cpp"
        "$ROOT/Source/Core/Protocol/Control/StructureConsent.cpp"
        "$ROOT/Source/Core/Protocol/Control/JSONSchema.cpp"
        "$ROOT/Source/Core/Protocol/Control/MethodRegistry.cpp"
        "$ROOT/Source/Core/Protocol/Local/Handoff.cpp"
        "$ROOT/Source/Core/Session/Identity.cpp"
    )
    if ! command -v "$FUZZ_CC" >/dev/null 2>&1; then
        ubq_skip "$FUZZ_CC not found"
        SKIPPED=$((SKIPPED + 1))
    elif ! ubq_fuzz_links; then
        ubq_skip "$FUZZ_CC cannot build a libFuzzer target (see $OUT/fuzz-link-probe.log) — it may accept -fsanitize=fuzzer and ship no runtime to link"
        SKIPPED=$((SKIPPED + 1))
    else
        ubq_fuzz_build fuzz_vt100 "$FUZZ_LOG" \
            "$ROOT/Tests/Core/fuzz_vt100.cpp" \
            "${CORE_SRCS[@]}"
        FUZZ_CORPUS="$ROOT/Tests/Core/fuzz-corpus"
        if [ -d "$FUZZ_CORPUS" ]; then
            # Replay every checked-in seed first (each is run as a single
            # input — exit code != 0 means an assertion fired inside
            # check_term_sanity, i.e. the parser produced a Terminal state
            # that violates an invariant). Then continue exploration.
            ubq_fuzz_scratch fuzz_vt100
            ubq_fuzz_run "$OUT/fuzz_vt100" -artifact_prefix="$OUT/" -runs=0 "$OUT/fuzz_vt100-corpus" "$FUZZ_CORPUS"
            ubq_fuzz_run "$OUT/fuzz_vt100" -artifact_prefix="$OUT/" -runs=10000 "$OUT/fuzz_vt100-corpus" "$FUZZ_CORPUS"
        else
            ubq_fuzz_run "$OUT/fuzz_vt100" -artifact_prefix="$OUT/" -runs=10000
        fi
    fi

    # fuzz_kitty — the Kitty graphics WIRE PROTOCOL parser (Source/Core/Kitty/
    # Graphics.cpp, linked via CORE_SRCS). PNG/zlib/TIFF have their own
    # fuzzers; this covers the APC control-key grammar, chunked-transfer assembly,
    # and base64 accumulator in front of them — reached only through ESC _ G … ESC \,
    # which fuzz_kitty frames every input as. Drives the real VT100 parser.
    echo "=== fuzz_kitty ==="
    FUZZ_KITTY_LOG="$OUT/fuzz-kitty-build.log"
    if ! command -v "$FUZZ_CC" >/dev/null 2>&1; then
        ubq_skip "$FUZZ_CC not found"
        SKIPPED=$((SKIPPED + 1))
    elif ! ubq_fuzz_links; then
        ubq_skip "$FUZZ_CC cannot build a libFuzzer target (see $OUT/fuzz-link-probe.log) — it may accept -fsanitize=fuzzer and ship no runtime to link"
        SKIPPED=$((SKIPPED + 1))
    else
        ubq_fuzz_build fuzz_kitty "$FUZZ_KITTY_LOG" \
            "$ROOT/Tests/Core/fuzz_kitty.cpp" \
            "${CORE_SRCS[@]}"
        KITTY_CORPUS="$ROOT/Tests/Core/kitty-corpus"
        if [ -d "$KITTY_CORPUS" ]; then
            ubq_fuzz_scratch fuzz_kitty
            ubq_fuzz_run "$OUT/fuzz_kitty" -artifact_prefix="$OUT/" -runs=0 "$OUT/fuzz_kitty-corpus" "$KITTY_CORPUS"
            ubq_fuzz_run "$OUT/fuzz_kitty" -artifact_prefix="$OUT/" -runs=20000 "$OUT/fuzz_kitty-corpus" "$KITTY_CORPUS"
        else
            ubq_fuzz_run "$OUT/fuzz_kitty" -artifact_prefix="$OUT/" -runs=20000
        fi
    fi

    # fuzz_osc1337 — the iTerm2 OSC 1337 inline-image chain end-to-end: handleOSC1337
    # -> handleITermInlineImage -> base64 decode -> container sniff
    # (decodeContainerToRGBA -> PNG/TIFF/zlib). The codecs have their own fuzzers,
    # but nothing exercised the PARSER-through-base64-through-dispatch path; this
    # frames every input as `ESC ] 1337 ; File=<fuzz> ST` so the File= argument
    # parser, the base64 accumulator, and the container dispatch all run on the
    # real VT100 parser. Drives CORE_SRCS (parser + OSC1337 + codecs linked in).
    echo "=== fuzz_osc1337 ==="
    FUZZ_OSC1337_LOG="$OUT/fuzz-osc1337-build.log"
    if ! command -v "$FUZZ_CC" >/dev/null 2>&1; then
        ubq_skip "$FUZZ_CC not found"
        SKIPPED=$((SKIPPED + 1))
    elif ! ubq_fuzz_links; then
        ubq_skip "$FUZZ_CC cannot build a libFuzzer target (see $OUT/fuzz-link-probe.log) — it may accept -fsanitize=fuzzer and ship no runtime to link"
        SKIPPED=$((SKIPPED + 1))
    else
        ubq_fuzz_build fuzz_osc1337 "$FUZZ_OSC1337_LOG" \
            "$ROOT/Tests/Core/fuzz_osc1337.cpp" \
            "${CORE_SRCS[@]}"
        OSC1337_CORPUS="$ROOT/Tests/Core/osc1337-corpus"
        if [ -d "$OSC1337_CORPUS" ]; then
            ubq_fuzz_scratch fuzz_osc1337
            ubq_fuzz_run "$OUT/fuzz_osc1337" -artifact_prefix="$OUT/" -runs=0 "$OUT/fuzz_osc1337-corpus" "$OSC1337_CORPUS"
            ubq_fuzz_run "$OUT/fuzz_osc1337" -artifact_prefix="$OUT/" -runs=20000 "$OUT/fuzz_osc1337-corpus" "$OSC1337_CORPUS"
        else
            ubq_fuzz_run "$OUT/fuzz_osc1337" -artifact_prefix="$OUT/" -runs=20000
        fi
    fi

    echo "=== fuzz_sixel ==="
    FUZZ_SIXEL_LOG="$OUT/fuzz-sixel-build.log"
    if ! command -v "$FUZZ_CC" >/dev/null 2>&1; then
        ubq_skip "$FUZZ_CC not found"
        SKIPPED=$((SKIPPED + 1))
    elif ! ubq_fuzz_links; then
        ubq_skip "$FUZZ_CC cannot build a libFuzzer target (see $OUT/fuzz-link-probe.log) — it may accept -fsanitize=fuzzer and ship no runtime to link"
        SKIPPED=$((SKIPPED + 1))
    else
        ubq_fuzz_build fuzz_sixel "$FUZZ_SIXEL_LOG" \
            "$ROOT/Tests/Core/Rendering/fuzz_sixel.cpp" \
            "$ROOT/Source/Core/Image/Sixel/Decoder.cpp"
        SIXEL_CORPUS="$ROOT/Tests/Core/Rendering/sixel-corpus"
        if [ -d "$SIXEL_CORPUS" ]; then
            ubq_fuzz_scratch fuzz_sixel
            ubq_fuzz_run "$OUT/fuzz_sixel" -artifact_prefix="$OUT/" -runs=0 "$OUT/fuzz_sixel-corpus" "$SIXEL_CORPUS"
            ubq_fuzz_run "$OUT/fuzz_sixel" -artifact_prefix="$OUT/" -runs=10000 "$OUT/fuzz_sixel-corpus" "$SIXEL_CORPUS"
        else
            ubq_fuzz_run "$OUT/fuzz_sixel" -artifact_prefix="$OUT/" -runs=10000
        fi
    fi

    # fuzz_sync — the untrusted host<->client wire decoder (Source/Core/Protocol/Sync).
    # The remote-Core/SSH split parses frames off a socket, so the frame
    # decoder + every typed decode/apply path (Hello/HelloAck/Resize,
    # applySnapshot, applyConfig) is exactly what should be fuzzed.
    echo "=== fuzz_sync ==="
    FUZZ_SYNC_LOG="$OUT/fuzz-sync-build.log"
    if ! command -v "$FUZZ_CC" >/dev/null 2>&1; then
        ubq_skip "$FUZZ_CC not found"
        SKIPPED=$((SKIPPED + 1))
    elif ! ubq_fuzz_links; then
        ubq_skip "$FUZZ_CC cannot build a libFuzzer target (see $OUT/fuzz-link-probe.log) — it may accept -fsanitize=fuzzer and ship no runtime to link"
        SKIPPED=$((SKIPPED + 1))
    else
        if ! ubq_yyjson_fuzz_object "$OUT/yyjson.fuzz.o" 2>>"$FUZZ_SYNC_LOG"; then
            build_broken "yyjson did not compile for fuzz_sync (see $FUZZ_SYNC_LOG)"
        fi
        ubq_fuzz_build fuzz_sync "$FUZZ_SYNC_LOG" \
            "$ROOT/Tests/Core/Protocol/Sync/fuzz_sync.cpp" \
            "$ROOT/Source/Core/Protocol/Sync/NativeAttachment.cpp" \
            "${SYNC_SRCS[@]}" \
            "${CONSENT_FUZZ_SRCS[@]}" \
            "$ROOT/Source/Core/Protocol/Sync/SyncClientCore.cpp" \
            "$ROOT/Source/Core/Preferences/Preferences.cpp" \
            "$ROOT/Source/Core/Preferences/PreferencesPaths.cpp" \
            "$ROOT/Source/Core/Preferences/PreferencesWriter.cpp" \
            "$ROOT/Source/Platform/POSIX/Preferences.cpp" \
            "$ROOT/Source/Core/Preferences/Profile.cpp" \
            "${CORE_SRCS[@]}" -- "$OUT/yyjson.fuzz.o"
        SYNC_CORPUS="$ROOT/Tests/Core/Protocol/Sync/fuzz-corpus"
        if [ -d "$SYNC_CORPUS" ]; then
            ubq_fuzz_scratch fuzz_sync
            ubq_fuzz_run "$OUT/fuzz_sync" -artifact_prefix="$OUT/" -runs=0 "$OUT/fuzz_sync-corpus" "$SYNC_CORPUS"
            ubq_fuzz_run "$OUT/fuzz_sync" -artifact_prefix="$OUT/" -runs=10000 "$OUT/fuzz_sync-corpus" "$SYNC_CORPUS"
        else
            ubq_fuzz_run "$OUT/fuzz_sync" -artifact_prefix="$OUT/" -runs=10000
        fi
    fi

    # fuzz_sync_connection — both ends of a Sync connection, and a host of four panes run by a script
    # whose invariants it asserts (Tests/Core/Protocol/Sync/fuzz_sync_connection.cpp).
    # Seeded from Tests/Corpus/sync/fuzz_sync_connection, which stays an input.
    echo "=== fuzz_sync_connection ==="
    FUZZ_SYNC_CONNECTION_LOG="$OUT/fuzz-sync-connection-build.log"
    if ! command -v "$FUZZ_CC" >/dev/null 2>&1; then
        ubq_skip "$FUZZ_CC not found"
        SKIPPED=$((SKIPPED + 1))
    elif ! ubq_fuzz_links; then
        ubq_skip "$FUZZ_CC cannot build a libFuzzer target (see $OUT/fuzz-link-probe.log) — it may accept -fsanitize=fuzzer and ship no runtime to link"
        SKIPPED=$((SKIPPED + 1))
    else
        if ! ubq_yyjson_fuzz_object "$OUT/yyjson.fuzz.o" 2>>"$FUZZ_SYNC_CONNECTION_LOG"; then
            build_broken "yyjson did not compile for fuzz_sync_connection (see $FUZZ_SYNC_CONNECTION_LOG)"
        fi
        ubq_fuzz_build fuzz_sync_connection "$FUZZ_SYNC_CONNECTION_LOG" \
            "$ROOT/Tests/Core/Protocol/Sync/fuzz_sync_connection.cpp" \
            "${SYNC_SRCS[@]}" \
            "${CORE_SRCS[@]}" -- "$OUT/yyjson.fuzz.o"
        ubq_fuzz_scratch fuzz_sync_connection
        ubq_fuzz_run "$OUT/fuzz_sync_connection" -artifact_prefix="$OUT/" -runs=0 "$OUT/fuzz_sync_connection-corpus" "$ROOT/Tests/Corpus/sync/fuzz_sync_connection"
        ubq_fuzz_run "$OUT/fuzz_sync_connection" -artifact_prefix="$OUT/" -runs=10000 "$OUT/fuzz_sync_connection-corpus" "$ROOT/Tests/Corpus/sync/fuzz_sync_connection"
    fi

    # fuzz_sync_screen — the screen-plane and history-plane decoders a client applies a host's bodies
    # with (Tests/Core/Protocol/Sync/fuzz_sync_screen.cpp). It builds its own seeds for the decoders;
    # Tests/Corpus/sync/fuzz_sync_screen seeds its chain-update differential, which stays an input.
    echo "=== fuzz_sync_screen ==="
    FUZZ_SYNC_LOG="$OUT/fuzz-sync-screen-build.log"
    if ! command -v "$FUZZ_CC" >/dev/null 2>&1; then
        ubq_skip "$FUZZ_CC not found"
        SKIPPED=$((SKIPPED + 1))
    elif ! ubq_fuzz_links; then
        ubq_skip "$FUZZ_CC cannot build a libFuzzer target (see $OUT/fuzz-link-probe.log) — it may accept -fsanitize=fuzzer and ship no runtime to link"
        SKIPPED=$((SKIPPED + 1))
    else
        if ! ubq_yyjson_fuzz_object "$OUT/yyjson.fuzz.o" 2>>"$FUZZ_SYNC_LOG"; then
            build_broken "yyjson did not compile for fuzz_sync_screen (see $FUZZ_SYNC_LOG)"
        fi
        ubq_fuzz_build fuzz_sync_screen "$FUZZ_SYNC_LOG" \
            "$ROOT/Tests/Core/Protocol/Sync/fuzz_sync_screen.cpp" \
            "${SYNC_SRCS[@]}" \
            "${CORE_SRCS[@]}" -- "$OUT/yyjson.fuzz.o"
        ubq_fuzz_scratch fuzz_sync_screen
        ubq_fuzz_run "$OUT/fuzz_sync_screen" -artifact_prefix="$OUT/" -runs=0 "$OUT/fuzz_sync_screen-corpus" "$ROOT/Tests/Corpus/sync/fuzz_sync_screen"
        ubq_fuzz_run "$OUT/fuzz_sync_screen" -artifact_prefix="$OUT/" -runs=10000 "$OUT/fuzz_sync_screen-corpus" "$ROOT/Tests/Corpus/sync/fuzz_sync_screen"
    fi

    # The local handover control decoder never allocates announced state bodies,
    # and a pane record it accepts is canonical.
    echo "=== fuzz_sync_handover ==="
    FUZZ_SYNC_HANDOVER_LOG="$OUT/fuzz-sync-handover-build.log"
    if ! command -v "$FUZZ_CC" >/dev/null 2>&1; then
        ubq_skip "$FUZZ_CC not found"
        SKIPPED=$((SKIPPED + 1))
    elif ! ubq_fuzz_links; then
        ubq_skip "$FUZZ_CC cannot build a libFuzzer target (see $OUT/fuzz-link-probe.log)"
        SKIPPED=$((SKIPPED + 1))
    else
        if ! ubq_yyjson_fuzz_object "$OUT/yyjson.fuzz.o" 2>>"$FUZZ_SYNC_HANDOVER_LOG"; then
            build_broken "yyjson did not compile for fuzz_sync_handover (see $FUZZ_SYNC_HANDOVER_LOG)"
        fi
        # HandoverBody.cpp captures and restores a whole Terminal through the
        # screen plane, and a pane's unwritten input as an OutputQueue, so the
        # decoder links the Core, the Sync screen plane and the queue.
        ubq_fuzz_build fuzz_sync_handover "$FUZZ_SYNC_HANDOVER_LOG" \
            "$ROOT/Tests/Core/Protocol/Sync/fuzz_sync_handover.cpp" \
            "$ROOT/Source/Core/Protocol/Sync/HandoverBody.cpp" \
            "$ROOT/Source/Core/Protocol/Sync/HandoverChannel.cpp" \
            "$ROOT/Source/Core/Protocol/Sync/HandoverGround.cpp" \
            "$ROOT/Source/PAL/OutputQueue.cpp" \
            "${SYNC_SRCS[@]}" \
            "${CORE_SRCS[@]}" -- "$OUT/yyjson.fuzz.o"
        # Seeded with the frozen pane record of the current version, whose length
        # sets the bound: a mutation of the whole record reaches every section
        # decoder.
        ubq_fuzz_scratch fuzz_sync_handover
        ubq_fuzz_run "$OUT/fuzz_sync_handover" -artifact_prefix="$OUT/" -runs=0 "$OUT/fuzz_sync_handover-corpus" "$ROOT/Tests/Corpus/sync/fuzz_sync_handover"
        ubq_fuzz_run "$OUT/fuzz_sync_handover" -artifact_prefix="$OUT/" -runs=10000 -max_len=65536 "$OUT/fuzz_sync_handover-corpus" "$ROOT/Tests/Corpus/sync/fuzz_sync_handover"
    fi

    # fuzz_resume_record — the persisted resume record a relaunched client reads
    # back from its own storage (Source/Core/Session/ResumeRecordFormat.cpp). The
    # harness edits a valid record with a picture and drafts, so a run with no
    # seeds reaches every pane and draft section.
    echo "=== fuzz_resume_record ==="
    FUZZ_RESUME_RECORD_LOG="$OUT/fuzz-resume-record-build.log"
    if ! command -v "$FUZZ_CC" >/dev/null 2>&1; then
        ubq_skip "$FUZZ_CC not found"
        SKIPPED=$((SKIPPED + 1))
    elif ! ubq_fuzz_links; then
        ubq_skip "$FUZZ_CC cannot build a libFuzzer target (see $OUT/fuzz-link-probe.log)"
        SKIPPED=$((SKIPPED + 1))
    else
        if ! ubq_yyjson_fuzz_object "$OUT/yyjson.fuzz.o" 2>>"$FUZZ_RESUME_RECORD_LOG"; then
            build_broken "yyjson did not compile for fuzz_resume_record (see $FUZZ_RESUME_RECORD_LOG)"
        fi
        ubq_fuzz_build fuzz_resume_record "$FUZZ_RESUME_RECORD_LOG" \
            "$ROOT/Tests/Core/Session/fuzz_resume_record.cpp" \
            "$ROOT/Source/Core/Session/ResumeRecordFormat.cpp" \
            "${SYNC_SRCS[@]}" \
            "${CORE_SRCS[@]}" -- "$OUT/yyjson.fuzz.o"
        ubq_fuzz_run "$OUT/fuzz_resume_record" -artifact_prefix="$OUT/" -runs=20000 -max_len=8192
    fi

    # fuzz_sync_catalog — the Sync frame catalog a pane's handle carries, both ways, checked
    # frame for frame against what was sent (Tests/Core/Protocol/Sync/fuzz_sync_catalog.cpp).
    # Seeded from Tests/Corpus/sync/fuzz_sync_catalog, which stays an input.
    echo "=== fuzz_sync_catalog ==="
    FUZZ_SYNC_CATALOG_LOG="$OUT/fuzz-sync-catalog-build.log"
    if ! command -v "$FUZZ_CC" >/dev/null 2>&1; then
        ubq_skip "$FUZZ_CC not found"
        SKIPPED=$((SKIPPED + 1))
    elif ! ubq_fuzz_links; then
        ubq_skip "$FUZZ_CC cannot build a libFuzzer target (see $OUT/fuzz-link-probe.log) — it may accept -fsanitize=fuzzer and ship no runtime to link"
        SKIPPED=$((SKIPPED + 1))
    else
        if ! ubq_yyjson_fuzz_object "$OUT/yyjson.fuzz.o" 2>>"$FUZZ_SYNC_CATALOG_LOG"; then
            build_broken "yyjson did not compile for fuzz_sync_catalog (see $FUZZ_SYNC_CATALOG_LOG)"
        fi
        ubq_fuzz_build fuzz_sync_catalog "$FUZZ_SYNC_CATALOG_LOG" \
            "$ROOT/Tests/Core/Protocol/Sync/fuzz_sync_catalog.cpp" \
            "${SYNC_SRCS[@]}" \
            "${CORE_SRCS[@]}" -- "$OUT/yyjson.fuzz.o"
        ubq_fuzz_scratch fuzz_sync_catalog
        ubq_fuzz_run "$OUT/fuzz_sync_catalog" -artifact_prefix="$OUT/" -runs=0 "$OUT/fuzz_sync_catalog-corpus" "$ROOT/Tests/Corpus/sync/fuzz_sync_catalog"
        ubq_fuzz_run "$OUT/fuzz_sync_catalog" -artifact_prefix="$OUT/" -runs=10000 "$OUT/fuzz_sync_catalog-corpus" "$ROOT/Tests/Corpus/sync/fuzz_sync_catalog"
    fi

    # fuzz_consent — canonical consent Calls, own/human Notifies and roster
    # bodies, plus the local adopt snapshot carrier. Uses production typed
    # ingress and codecs with ASAN+UBSAN; it never reinstates retired raw tags.
    echo "=== fuzz_consent ==="
    FUZZ_CONSENT_LOG="$OUT/fuzz-consent-build.log"
    if ! command -v "$FUZZ_CC" >/dev/null 2>&1; then
        ubq_skip "$FUZZ_CC not found"
        SKIPPED=$((SKIPPED + 1))
    elif ! ubq_fuzz_links; then
        ubq_skip "$FUZZ_CC cannot build a libFuzzer target (see $OUT/fuzz-link-probe.log) — it may accept -fsanitize=fuzzer and ship no runtime to link"
        SKIPPED=$((SKIPPED + 1))
    else
        if ! ubq_yyjson_fuzz_object "$OUT/yyjson.fuzz.o" 2>>"$FUZZ_CONSENT_LOG"; then
            build_broken "yyjson did not compile for fuzz_consent (see $FUZZ_CONSENT_LOG)"
        fi
        ubq_fuzz_build fuzz_consent "$FUZZ_CONSENT_LOG" \
            "$ROOT/Tests/Core/Protocol/Sync/fuzz_consent.cpp" \
            "$ROOT/Source/Core/Protocol/Sync/NativeAttachment.cpp" \
            "${SYNC_SRCS[@]}" \
            "${CONSENT_FUZZ_SRCS[@]}" \
            "$ROOT/Source/Core/Preferences/Preferences.cpp" \
            "$ROOT/Source/Core/Preferences/PreferencesPaths.cpp" \
            "$ROOT/Source/Core/Preferences/PreferencesWriter.cpp" \
            "$ROOT/Source/Platform/POSIX/Preferences.cpp" \
            "$ROOT/Source/Core/Preferences/Profile.cpp" \
            "${CORE_SRCS[@]}" -- "$OUT/yyjson.fuzz.o"
        CONSENT_CORPUS="$ROOT/Tests/Core/Protocol/Sync/fuzz-corpus"
        if [ -d "$CONSENT_CORPUS" ]; then
            ubq_fuzz_scratch fuzz_consent
            ubq_fuzz_run "$OUT/fuzz_consent" -artifact_prefix="$OUT/" -runs=0 "$OUT/fuzz_consent-corpus" "$CONSENT_CORPUS"
            ubq_fuzz_run "$OUT/fuzz_consent" -artifact_prefix="$OUT/" -max_total_time=30 "$OUT/fuzz_consent-corpus" "$CONSENT_CORPUS"
        else
            ubq_fuzz_run "$OUT/fuzz_consent" -artifact_prefix="$OUT/" -max_total_time=30
        fi
    fi

    # fuzz_quic_preamble — the daemon's PRE-AUTH QUIC preamble state machine
    # (Daemon::onQUICStreamData, #ifdef UBIQUITTY_QUIC). A brand-new QUIC peer is
    # unauthenticated: the daemon reassembles its leading bytes into a
    # [u8 tokenLength][token] preamble, authenticates via consumeQUICToken, then
    # slices the remainder off as Sync frames. Every byte here is untrusted and
    # pre-auth, so the reassembly, the pre-insert OOM cap, the tokenLength parse,
    # and the rest-slice are the surface. This drives the REAL daemon (a live
    # ephemeral listener + a handshake-less QUICConnection via a friend seam) with
    # one pending token seeded so successful auth is reachable, and asserts the
    # pre-auth buffer never exceeds 1 + maxQUICTokenLength while unauthenticated.
    # Compiled from source with ASAN+UBSAN over our daemon TUs (the ngtcp2/AWS-LC
    # archives are a trusted dependency, linked but not the instrumentation target).
    echo "=== fuzz_quic_preamble ==="
    FUZZ_QUIC_PREAMBLE_LOG="$OUT/fuzz-quic-preamble-build.log"
    if ! quic_deps_present; then
        :
    elif ! command -v "$FUZZ_CC" >/dev/null 2>&1; then
        ubq_skip "$FUZZ_CC not found"
        SKIPPED=$((SKIPPED + 1))
    elif ! ubq_fuzz_links; then
        ubq_skip "$FUZZ_CC cannot build a libFuzzer target (see $OUT/fuzz-link-probe.log) — it may accept -fsanitize=fuzzer and ship no runtime to link"
        SKIPPED=$((SKIPPED + 1))
    else
        FUZZ_QUIC_FRAMEWORKS=()
        [ "$(uname)" = "Darwin" ] && FUZZ_QUIC_FRAMEWORKS=(-framework CoreFoundation -framework Security)
        # The full portable manifest includes renderer-to-search references;
        # retaining its callers requires ScrollSearch and its vendored RE2 units.
        # The sources this harness names itself. They are listed once, and the
        # manifest walk below skips them, because a file compiled from both
        # lists is 33 duplicate symbols rather than a diagnosable mistake.
        HOST_FUZZ_EXPLICIT=(
            "Source/Platform/Host/ClipboardTool.cpp"
            "Source/Platform/Host/Daemon.cpp"
            "Source/Platform/Host/HostQUIC.cpp"
            "Source/Platform/POSIX/DatagramBatch.cpp"
            "Source/Core/Session/ControlPaths.cpp"
            "Source/Platform/POSIX/SocketPath.cpp"
            "Source/Platform/POSIX/SystemPAL.cpp"
            "Source/Platform/POSIX/PTYFacts.cpp"
            "Source/Platform/Host/HostPaths.cpp"
            "Source/Platform/Host/HostPAL.cpp"
            "Source/Platform/POSIX/PTYStream.cpp"
            "Source/Platform/POSIX/Socket.cpp"
            "Source/PAL/OutputQueue.cpp"
        )
        # Every daemon TU the host target compiles, read from that target's
        # own list, so a TU the daemon comes to reach is never missing here.
        # The entry point and the scripts it embeds are the binary's alone. The
        # list spells each under Core's source root, and a list read as empty
        # links a daemon short of its own translation units, so it is refused.
        host_target_sources=0
        while IFS= read -r host_source; do
            host_target_sources=$((host_target_sources + 1))
            case " ${HOST_FUZZ_EXPLICIT[*]} " in *" $host_source "*) continue ;; esac
            HOST_FUZZ_EXPLICIT+=("$host_source")
        done < <(awk '/add_executable\(ubiquitty-host/ { inside = 1; next }
            inside && /\$\{UBIQUITTY_HOST_CORE\}/ { exit }
            inside { for (i = 1; i <= NF; i++) { source = $i; sub(/^\$\{UBIQUITTY_CORE_SOURCE_DIR\}\//, "", source)
                if (source ~ /^Source\/.*\.cpp$/ && source !~ /\/(main|ShellIntegrationFiles)\.cpp$/) print source } }' "$ROOT/CMakeLists.txt")
        [ "$host_target_sources" -gt 0 ] \
            || build_broken "fuzz_quic_preamble read no source from ubiquitty-host's list in CMakeLists.txt"
        HOST_FUZZ_EXPLICIT_PATHS=()
        for explicit_source in "${HOST_FUZZ_EXPLICIT[@]}"; do
            HOST_FUZZ_EXPLICIT_PATHS+=("$ROOT/$explicit_source")
        done
        HOST_CORE_EXCLUDE=" ${HOST_FUZZ_EXPLICIT[*]} "
        # simdutf is not Core, so portable-core-sources.txt does not list it, but
        # Core/Codec/Base64.cpp and Core/Unicode/UTF8Decoder.cpp are in that list
        # and call into it. CORE_SRCS names it separately for the same reason;
        # a list built from the manifest alone links neither.
        HOST_FUZZ_CORE=(
            "$ROOT/Source/ThirdParty/simdutf/simdutf.cpp"
            "$ROOT"/Source/ThirdParty/re2/re2/*.cc
            "$ROOT/Source/ThirdParty/re2/util/rune.cc"
            "$ROOT/Source/ThirdParty/re2/util/strutil.cc"
        )
        # yyjson is C and wants -std=c99, so it cannot ride the C++ command line
        # the rest of the link uses. Core/Codec/JSON.cpp is in the manifest and
        # calls into it; CMake compiles it with its own source properties for the
        # same reason. The sanitizer flags match the link's, or the object is
        # instrumented differently from everything it is linked with.
        # The C driver, because clang++ refuses -std=c99 outright. -no-link so
        # the object carries coverage instrumentation without libFuzzer's main.
        YYJSON_FUZZ_OBJ="$OUT/yyjson.fuzz.o"
        if ! ubq_yyjson_fuzz_object "$YYJSON_FUZZ_OBJ" 2>>"$FUZZ_QUIC_PREAMBLE_LOG"; then
            build_broken "yyjson did not compile for fuzz_quic_preamble (see $FUZZ_QUIC_PREAMBLE_LOG)"
            YYJSON_FUZZ_OBJ=""
        fi
        while IFS= read -r core_line; do
            case "$core_line" in ''|\#*) continue ;; esac
            case "$HOST_CORE_EXCLUDE" in *" $core_line "*) continue ;; esac
            HOST_FUZZ_CORE+=("$ROOT/$core_line")
        done < "$ROOT/Scripts/portable-core-sources.txt"
        if UBQ_COMPILE_CXX="$FUZZ_CXX" UBQ_COMPILE_LINK="$FUZZ_CXX" \
                ubq_compile_parallel "$OUT/fuzz_quic_preamble" "$FUZZ_OBJDIR" "$FUZZ_QUIC_PREAMBLE_LOG" \
                "${FUZZ_FLAGS[@]}" -pthread -DUBIQUITTY_QUIC=1 \
                -isystem "$ROOT/Source/ThirdParty/re2" -I"$QUIC_PREFIX/include" -- \
                "$ROOT/Tests/Host/fuzz_quic_preamble.cpp" \
                ${HOST_FUZZ_EXPLICIT_PATHS[@]+"${HOST_FUZZ_EXPLICIT_PATHS[@]}"} \
                ${HOST_FUZZ_CORE[@]+"${HOST_FUZZ_CORE[@]}"} \
                "$THEMES_GEN" -- \
                ${YYJSON_FUZZ_OBJ:+"$YYJSON_FUZZ_OBJ"} \
                "$QUIC_PREFIX/lib/libngtcp2_crypto_boringssl.a" \
                "$QUIC_PREFIX/lib/libngtcp2.a" \
                "$QUIC_PREFIX/lib/libssl.a" "$QUIC_PREFIX/lib/libcrypto.a" \
                ${FUZZ_QUIC_FRAMEWORKS[@]+"${FUZZ_QUIC_FRAMEWORKS[@]}"}; then
            QUIC_PREAMBLE_CORPUS="$ROOT/Tests/Host/fuzz-corpus-quic-preamble"
            mkdir -p "$QUIC_PREAMBLE_CORPUS"
            ubq_fuzz_scratch fuzz_quic_preamble
            ubq_fuzz_run "$OUT/fuzz_quic_preamble" -artifact_prefix="$OUT/" -max_total_time=30 "$OUT/fuzz_quic_preamble-corpus" "$QUIC_PREAMBLE_CORPUS"
        else
            echo "fuzz_quic_preamble: did not build — first errors from $FUZZ_QUIC_PREAMBLE_LOG:" >&2
            grep -m 5 -E 'error:|undefined reference|Undefined symbols' "$FUZZ_QUIC_PREAMBLE_LOG" >&2 || true
            build_broken "fuzz_quic_preamble build failed (see $FUZZ_QUIC_PREAMBLE_LOG)"
            echo "----- $FUZZ_QUIC_PREAMBLE_LOG -----" >&2; cat "$FUZZ_QUIC_PREAMBLE_LOG" >&2; echo "----- end -----" >&2
        fi
    fi

    # fuzz_workspace — the untrusted .workspace decoder (Source/Core/Session/Workspace).
    # A restored workspace file is arbitrary bytes; deserializeWorkspace must never
    # read OOB / over-allocate, and an accepted result must round-trip exactly.
    # Workspace.cpp encodes and decodes Base64, which calls into simdutf; this
    # stanza names its sources, so it names both.
    echo "=== fuzz_workspace ==="
    FUZZ_WS_LOG="$OUT/fuzz-workspace-build.log"
    if ! command -v "$FUZZ_CC" >/dev/null 2>&1; then
        ubq_skip "$FUZZ_CC not found"
        SKIPPED=$((SKIPPED + 1))
    elif ! ubq_fuzz_links; then
        ubq_skip "$FUZZ_CC cannot build a libFuzzer target (see $OUT/fuzz-link-probe.log) — it may accept -fsanitize=fuzzer and ship no runtime to link"
        SKIPPED=$((SKIPPED + 1))
    else
        ubq_fuzz_build fuzz_workspace "$FUZZ_WS_LOG" \
            "$ROOT/Tests/Core/Session/fuzz_workspace.cpp" \
            "$ROOT/Source/Core/Session/Workspace.cpp" \
            "$ROOT/Source/Core/Codec/Base64.cpp" \
            "$ROOT/Source/ThirdParty/simdutf/simdutf.cpp"
        ubq_fuzz_run "$OUT/fuzz_workspace" -artifact_prefix="$OUT/" -runs=20000
    fi

    # fuzz_session_tree — the decoder of a workspace's structure, which reads a host's record of a
    # workspace's windows, tabs, splits and leaves (Tests/Core/Session/fuzz_session_tree.cpp). It
    # builds its own seed body, so a run reaches the splits and the leaves without a corpus.
    echo "=== fuzz_session_tree ==="
    FUZZ_SESSION_TREE_LOG="$OUT/fuzz-session-tree-build.log"
    if ! command -v "$FUZZ_CC" >/dev/null 2>&1; then
        ubq_skip "$FUZZ_CC not found"
        SKIPPED=$((SKIPPED + 1))
    elif ! ubq_fuzz_links; then
        ubq_skip "$FUZZ_CC cannot build a libFuzzer target (see $OUT/fuzz-link-probe.log) — it may accept -fsanitize=fuzzer and ship no runtime to link"
        SKIPPED=$((SKIPPED + 1))
    else
        if ! ubq_yyjson_fuzz_object "$OUT/yyjson.fuzz.o" 2>>"$FUZZ_SESSION_TREE_LOG"; then
            build_broken "yyjson did not compile for fuzz_session_tree (see $FUZZ_SESSION_TREE_LOG)"
        fi
        ubq_fuzz_build fuzz_session_tree "$FUZZ_SESSION_TREE_LOG" \
            "$ROOT/Tests/Core/Session/fuzz_session_tree.cpp" \
            "$ROOT/Source/Core/Session/SessionTreeWire.cpp" \
            "$ROOT/Source/Core/Session/Identity.cpp" \
            "$ROOT/Source/Core/Session/ProcessState.cpp" \
            "$ROOT/Source/PAL/RemoteTarget.cpp" \
            "$ROOT/Source/Core/Preferences/Preferences.cpp" \
            "$ROOT/Source/Core/Preferences/PreferencesPaths.cpp" \
            "$ROOT/Source/Core/Preferences/PreferencesWriter.cpp" \
            "$ROOT/Source/Core/Preferences/Profile.cpp" \
            "$ROOT/Source/Platform/POSIX/Preferences.cpp" \
            "${SYNC_SRCS[@]}" \
            "${CORE_SRCS[@]}" -- "$OUT/yyjson.fuzz.o"
        ubq_fuzz_run "$OUT/fuzz_session_tree" -artifact_prefix="$OUT/" -runs=20000
    fi

    # fuzz_quic_bootstrap — the QUIC bootstrap-LINE parser
    # (Source/Core/Protocol/Sync/QUICBootstrap.h: parseQUICBootstrapLine +
    # decodeHexBytes). This line is the cert-PIN carrier over the trusted
    # (ssh/local) channel; the hex-decode + length math is the fuzz surface,
    # and a half-decode that returns SUCCESS with a wrong-length pin weakens the
    # MITM gate. The harness asserts the parser's own contract — a SUCCESS must
    # carry a 32-byte pin and a <=64-byte token — so that half-decode is caught
    # even without a sanitizer trip; ASAN+UBSAN cover the read/UB side.
    # Header-only target, so it links standalone with no Sync / socket deps.
    echo "=== fuzz_quic_bootstrap ==="
    FUZZ_QB_LOG="$OUT/fuzz-quic-bootstrap-build.log"
    if ! command -v "$FUZZ_CC" >/dev/null 2>&1; then
        ubq_skip "$FUZZ_CC not found"
        SKIPPED=$((SKIPPED + 1))
    elif ! ubq_fuzz_links; then
        ubq_skip "$FUZZ_CC cannot build a libFuzzer target (see $OUT/fuzz-link-probe.log) — it may accept -fsanitize=fuzzer and ship no runtime to link"
        SKIPPED=$((SKIPPED + 1))
    else
        ubq_fuzz_build fuzz_quic_bootstrap "$FUZZ_QB_LOG" \
            "$ROOT/Tests/Core/Protocol/Sync/fuzz_quic_bootstrap.cpp"
        QB_CORPUS="$ROOT/Tests/Core/Protocol/Sync/quic-bootstrap-corpus"
        if [ -d "$QB_CORPUS" ]; then
            ubq_fuzz_scratch fuzz_quic_bootstrap
            ubq_fuzz_run "$OUT/fuzz_quic_bootstrap" -artifact_prefix="$OUT/" -runs=0 "$OUT/fuzz_quic_bootstrap-corpus" "$QB_CORPUS"
            ubq_fuzz_run "$OUT/fuzz_quic_bootstrap" -artifact_prefix="$OUT/" -max_total_time=60 "$OUT/fuzz_quic_bootstrap-corpus" "$QB_CORPUS"
        else
            ubq_fuzz_run "$OUT/fuzz_quic_bootstrap" -artifact_prefix="$OUT/" -max_total_time=60
        fi
    fi

    # fuzz_png — the PNG decoder (Source/Core/Codec/PNGDecoder). Inline-image payloads
    # (Kitty t=d / iTerm 1337) are untrusted; the IHDR/chunk/scanline math is
    # classic integer-overflow / OOB-write territory. Links the inflater it uses.
    echo "=== fuzz_png ==="
    FUZZ_PNG_LOG="$OUT/fuzz-png-build.log"
    if ! command -v "$FUZZ_CC" >/dev/null 2>&1; then
        ubq_skip "$FUZZ_CC not found"
        SKIPPED=$((SKIPPED + 1))
    elif ! ubq_fuzz_links; then
        ubq_skip "$FUZZ_CC cannot build a libFuzzer target (see $OUT/fuzz-link-probe.log) — it may accept -fsanitize=fuzzer and ship no runtime to link"
        SKIPPED=$((SKIPPED + 1))
    else
        ubq_fuzz_build fuzz_png "$FUZZ_PNG_LOG" \
            "$ROOT/Tests/Core/Rendering/fuzz_png.cpp" \
            "$ROOT/Source/Core/Codec/PNGDecoder.cpp" \
            "$ROOT/Source/Core/Codec/ZlibInflate.cpp" \
            "$ROOT/Source/Core/Codec/WuffsCodec.cpp"
        PNG_CORPUS="$ROOT/Tests/Core/Rendering/png-corpus"
        if [ -d "$PNG_CORPUS" ]; then
            ubq_fuzz_scratch fuzz_png
            ubq_fuzz_run "$OUT/fuzz_png" -artifact_prefix="$OUT/" -runs=0 "$OUT/fuzz_png-corpus" "$PNG_CORPUS"
            ubq_fuzz_run "$OUT/fuzz_png" -artifact_prefix="$OUT/" -runs=20000 "$OUT/fuzz_png-corpus" "$PNG_CORPUS"
        else
            ubq_fuzz_run "$OUT/fuzz_png" -artifact_prefix="$OUT/" -runs=20000
        fi
    fi

    # fuzz_tiff — the TIFF decoder (Source/Core/Image/TIFFDecoder). chafa's `-f iterm`
    # output is baseline uncompressed TIFF, delivered as an untrusted OSC 1337
    # inline-image payload (Source/Core/OSC/ITerm2.cpp decodeContainerToRGBA). The
    # IFD width/height × SamplesPerPixel × RowsPerStrip strip-offset product math
    # is the integer-overflow / OOB-write surface, so build with UBSan on
    # top of ASan to flag the overflow itself, not just the resulting OOB.
    echo "=== fuzz_tiff ==="
    FUZZ_TIFF_LOG="$OUT/fuzz-tiff-build.log"
    if ! command -v "$FUZZ_CC" >/dev/null 2>&1; then
        ubq_skip "$FUZZ_CC not found"
        SKIPPED=$((SKIPPED + 1))
    elif ! ubq_fuzz_links; then
        ubq_skip "$FUZZ_CC cannot build a libFuzzer target (see $OUT/fuzz-link-probe.log) — it may accept -fsanitize=fuzzer and ship no runtime to link"
        SKIPPED=$((SKIPPED + 1))
    else
        ubq_fuzz_build fuzz_tiff "$FUZZ_TIFF_LOG" \
            "$ROOT/Tests/Core/Rendering/fuzz_tiff.cpp" \
            "$ROOT/Source/Core/Image/TIFFDecoder.cpp"
        TIFF_CORPUS="$ROOT/Tests/Core/Rendering/tiff-corpus"
        if [ -d "$TIFF_CORPUS" ]; then
            ubq_fuzz_scratch fuzz_tiff
            ubq_fuzz_run "$OUT/fuzz_tiff" -artifact_prefix="$OUT/" -runs=0 "$OUT/fuzz_tiff-corpus" "$TIFF_CORPUS"
            ubq_fuzz_run "$OUT/fuzz_tiff" -artifact_prefix="$OUT/" -runs=20000 "$OUT/fuzz_tiff-corpus" "$TIFF_CORPUS"
        else
            ubq_fuzz_run "$OUT/fuzz_tiff" -artifact_prefix="$OUT/" -runs=20000
        fi
    fi

    # fuzz_zlib — the inflate decoder (Source/Core/Codec/ZlibInflate), the
    # decompressor under PNG IDAT + Kitty o=z. Huffman/window/output OOB.
    echo "=== fuzz_zlib ==="
    FUZZ_ZLIB_LOG="$OUT/fuzz-zlib-build.log"
    if ! command -v "$FUZZ_CC" >/dev/null 2>&1; then
        ubq_skip "$FUZZ_CC not found"
        SKIPPED=$((SKIPPED + 1))
    elif ! ubq_fuzz_links; then
        ubq_skip "$FUZZ_CC cannot build a libFuzzer target (see $OUT/fuzz-link-probe.log) — it may accept -fsanitize=fuzzer and ship no runtime to link"
        SKIPPED=$((SKIPPED + 1))
    else
        ubq_fuzz_build fuzz_zlib "$FUZZ_ZLIB_LOG" \
            "$ROOT/Tests/Core/Rendering/fuzz_zlib.cpp" \
            "$ROOT/Source/Core/Codec/ZlibInflate.cpp" \
            "$ROOT/Source/Core/Codec/WuffsCodec.cpp"
        ubq_fuzz_run "$OUT/fuzz_zlib" -artifact_prefix="$OUT/" -runs=20000
    fi

    # fuzz_tmux — the tmux Control Mode (`tmux -CC`) parser (Source/Core/Protocol/Tmux/
    # Tmux.cpp + Commands.cpp). Every byte comes from a tmux -CC server reached
    # over SSH — attacker-controlled if the peer is hostile. feedInput is a
    # hand-rolled byte machine; parseLayout walks a RECURSIVE cell tree off a
    # %layout-change field; parseDimension/parseUint drive pane geometry; the
    # %output path length-decodes an octal-escaped body. Links the REAL parser and
    # drives feedInput in fuzzer-chosen chunks (partial-frame reassembly) plus the
    # public parseLayout + decodePaneModeReport; UBSan on top of ASan flags the
    # integer-overflow in the dimension / octal / pane-id accumulators, the asserts
    # pin the pane-geometry / pane-id / window-count bounds.
    echo "=== fuzz_tmux ==="
    FUZZ_TMUX_LOG="$OUT/fuzz-tmux-build.log"
    if ! command -v "$FUZZ_CC" >/dev/null 2>&1; then
        ubq_skip "$FUZZ_CC not found"
        SKIPPED=$((SKIPPED + 1))
    elif ! ubq_fuzz_links; then
        ubq_skip "$FUZZ_CC cannot build a libFuzzer target (see $OUT/fuzz-link-probe.log) — it may accept -fsanitize=fuzzer and ship no runtime to link"
        SKIPPED=$((SKIPPED + 1))
    else
        ubq_fuzz_build fuzz_tmux "$FUZZ_TMUX_LOG" \
            "$ROOT/Tests/Core/Protocol/Tmux/fuzz_tmux.cpp" \
            "$ROOT/Source/Core/Protocol/Tmux/Tmux.cpp" \
            "$ROOT/Source/Core/Protocol/Tmux/Commands.cpp" \
            "$ROOT/Source/Core/Panes/PaneTree.cpp"
        TMUX_CORPUS="$ROOT/Tests/Core/Protocol/Tmux/fuzz-corpus"
        if [ -d "$TMUX_CORPUS" ]; then
            ubq_fuzz_scratch fuzz_tmux
            ubq_fuzz_run "$OUT/fuzz_tmux" -artifact_prefix="$OUT/" -runs=0 "$OUT/fuzz_tmux-corpus" "$TMUX_CORPUS"
            ubq_fuzz_run "$OUT/fuzz_tmux" -artifact_prefix="$OUT/" -runs=20000 "$OUT/fuzz_tmux-corpus" "$TMUX_CORPUS"
        else
            ubq_fuzz_run "$OUT/fuzz_tmux" -artifact_prefix="$OUT/" -runs=20000
        fi
    fi

    # fuzz_richtext — the RTF reader (Source/Core/RichText/RTFReader). An OSC
    # 7404 body is untrusted bytes off the terminal stream; the harness checks
    # the document invariants on every body the reader accepts, seeded from
    # Tests/Corpus/richtext, which test_richtext_corpus_replay also replays.
    echo "=== fuzz_richtext ==="
    FUZZ_RICHTEXT_LOG="$OUT/fuzz-richtext-build.log"
    if ! command -v "$FUZZ_CC" >/dev/null 2>&1; then
        ubq_skip "$FUZZ_CC not found"
        SKIPPED=$((SKIPPED + 1))
    elif ! ubq_fuzz_links; then
        ubq_skip "$FUZZ_CC cannot build a libFuzzer target (see $OUT/fuzz-link-probe.log) — it may accept -fsanitize=fuzzer and ship no runtime to link"
        SKIPPED=$((SKIPPED + 1))
    else
        ubq_fuzz_build fuzz_richtext "$FUZZ_RICHTEXT_LOG" \
            "$ROOT/Tests/Core/RichText/fuzz_richtext.cpp" \
            "$ROOT/Source/Core/RichText/RTFReader.cpp" \
            "$ROOT/Source/Core/Unicode/Encodings.cpp" \
            "$ROOT/Source/Core/Unicode/UTF8Decoder.cpp" \
            "$ROOT/Source/ThirdParty/simdutf/simdutf.cpp"
        RICHTEXT_CORPUS="$ROOT/Tests/Corpus/richtext"
        ubq_fuzz_scratch fuzz_richtext
        ubq_fuzz_run "$OUT/fuzz_richtext" -artifact_prefix="$OUT/" -runs=0 "$OUT/fuzz_richtext-corpus" "$RICHTEXT_CORPUS"
        ubq_fuzz_run "$OUT/fuzz_richtext" -artifact_prefix="$OUT/" -runs=20000 "$OUT/fuzz_richtext-corpus" "$RICHTEXT_CORPUS"
    fi

    # CI sets FUZZ_REQUIRED=1 so a silently-skipped fuzzer (toolchain drift —
    # no clang, no -fsanitize=fuzzer, unlinkable runtime) fails the job instead
    # of passing green having fuzzed nothing. Locally the default leaves the
    # opt-in skip intact. A produced binary is the proxy for "built + ran"; any
    # SKIP short-circuits before `-o "$OUT/fuzz_*"`, and the removal at the top
    # of this section is what stops an older run's binary standing in.
    #
    # The set comes from the tree, for the reason fuzz_harnesses_compile takes
    # its list from the tree: a written-out list is narrower than its subject the
    # moment a harness is added, and every harness it omits can skip with the
    # gate still green.
    if [ "${FUZZ_REQUIRED:-0}" = "1" ]; then
        FUZZ_MISSING=0
        FUZZ_DECLARED=0
        FUZZ_EXEMPT=""
        # A dependency absent from the MACHINE rather than from the tree. The
        # fuzz job installs clang and nothing else, so the QUIC archives are not
        # there and the harness that links them cannot build; that is a fact
        # about the runner, and the name leaves the required set with its reason
        # printed rather than by being left out of a list.
        if ! quic_deps_present; then
            FUZZ_EXEMPT=" fuzz_quic_preamble "
        fi
        for fb in $(find "$ROOT/Tests" -name 'fuzz_*.cpp' -exec basename {} .cpp \; | sort); do
            FUZZ_DECLARED=$((FUZZ_DECLARED + 1))
            # Both sides delimited, so a name cannot be excused by being the
            # prefix of an exempt one.
            case "$FUZZ_EXEMPT" in *" $fb "*) continue ;; esac
            if [ ! -x "$OUT/$fb" ]; then
                echo "FUZZ_REQUIRED: $fb was skipped (not built/run)" >&2
                FUZZ_MISSING=1
            fi
        done
        # A declared set of zero passes the loop above having checked nothing,
        # and the list is a glob over a tree a rename can empty.
        if [ "$FUZZ_DECLARED" -eq 0 ]; then
            echo "FAIL: FUZZ_REQUIRED set and no fuzz_*.cpp harness exists under Tests" >&2
            exit 1
        fi
        if [ "$FUZZ_MISSING" -eq 1 ]; then
            echo "FAIL: FUZZ_REQUIRED set but a fuzzer was skipped" >&2
            exit 1
        fi
        echo "run-tests.sh: FUZZ_REQUIRED — $FUZZ_DECLARED harness(es) declared, exempt:${FUZZ_EXEMPT:- none }"
    fi

    if [ "$(ubq_fuzz_corpus_state)" != "$UBQ_FUZZ_CORPUS_BEFORE" ]; then
        echo "FAIL: a fuzz run changed a tracked corpus" >&2
        git -C "$ROOT" status --short --untracked-files=all -- \
            "*fuzz-corpus*" "*kitty-corpus*" "*sixel-corpus*" "*tiff-corpus*" \
            "*png-corpus*" "*bootstrap-corpus*" 2>/dev/null | head -10 >&2
        echo "  A seed is tracked because something needed it to be reachable, and" >&2
        echo "  libFuzzer prunes what it judges redundant. Every run writes to a" >&2
        echo "  scratch corpus under the build tree; a change here means one of" >&2
        echo "  them was handed the tracked directory first." >&2
        exit 1
    fi
    echo "run-tests.sh: tracked fuzz corpora unchanged by this run"
fi

# The catalog/PNG unit test is host-portable and prevents missing baselines,
# orphaned images, or showcase entries without executable snapshot coverage.
# The showcase's key decoder rides along: a reply it misreads as a keypress
# dismisses the page that turned the reply on. So does its menu layout: a
# character past the bottom row's last column scrolls the double-height
# banner's top half off the screen.
if wanted test_snapshot_harness; then
    echo "=== test_snapshot_harness ==="
    RAN=$((RAN + 1))
    python3 "$ROOT/Tests/Snapshots/test_harness.py" || exit 1
    python3 "$ROOT/Tools/showcase/test_tui_escape_decoding.py" || exit 1
    python3 "$ROOT/Tools/showcase/test_first_screen_fits.py" || exit 1
fi

# Host-tool and test-driver contracts use isolated fixtures and fixed subprocess
# responses. Discovery and invalid requests must not build or execute targets.
# The shell-integration scripts run in each installed shell; the macOS
# installer runs against fixture bundles and stand-in tools.
# The tool tests whose subjects ship with the app are the app's stanza.
if wanted test_host_tools; then
    echo "=== test_host_tools ==="
    RAN=$((RAN + 1))
    python3 "$ROOT/Tests/Tools/test_host_tools.py" || exit 1
    python3 "$ROOT/Tests/Tools/test_test_driver.py" || exit 1
    python3 "$ROOT/Tests/Tools/test_coverage_index_freshness.py" || exit 1
    python3 "$ROOT/Tests/Tools/test_sync_memory_gate.py" || exit 1
    python3 "$ROOT/Tests/Tools/test_profile_lane.py" || exit 1
    python3 "$ROOT/Tests/Tools/test_draft_workflows.py" || exit 1
    python3 "$ROOT/Tests/Tools/test_shell_integration_scripts.py" || exit 1
    python3 "$ROOT/Tests/Tools/test_linux_host_artifact.py" || exit 1
    python3 "$ROOT/Tests/Tools/test_stale_landing.py" || exit 1
fi

# run_hygiene — a run that leaves a process running, or a file in the state
# directory of whoever runs the suite, fails and says what it left; and the
# process is ended on a pass, a failure, an interrupt and an outright kill.
# With UBQ_HYGIENE_PLANT the stanza is the leak itself, which is how the test
# drives this script against one: Tests/Tools/test_run_hygiene.py.
if wanted run_hygiene; then
    echo "=== run_hygiene ==="
    RAN=$((RAN + 1))
    if [ -n "${UBQ_HYGIENE_PLANT:-}" ]; then
        python3 "$ROOT/Scripts/test-run-hygiene.py" plant "$UBQ_HYGIENE_PLANT" "$UBQ_REAL_STATE_DIR" || exit 1
    else
        python3 "$ROOT/Tests/Tools/test_run_hygiene.py" || exit 1
    fi
fi

# snapshot_renderer_fixture — the portable snapshot renderer, brought current
# before the snapshots stanza renders with it; ubq_portable_snapshot_renderer
# says why the build is a fixture of its own under ctest. Declared ahead of
# snapshots so a full run without ctest builds first as well.
if wanted snapshot_renderer_fixture; then
    echo "=== snapshot_renderer_fixture ==="
    if [ "${SNAPSHOT:-}" = "skip" ]; then
        ubq_skip "snapshot_renderer_fixture: snapshots explicitly disabled with SNAPSHOT=skip"
        SKIPPED=$((SKIPPED + 1))
    else
        RAN=$((RAN + 1))
        UBQ_SNAPSHOT_RENDERER_PREBUILT=0 ubq_portable_snapshot_renderer "$OUT/snapshot-renderer-fixture.log" || {
            cat "$OUT/snapshot-renderer-fixture.log" >&2
            build_broken "snapshot_renderer_fixture: the portable renderer did not build (see $OUT/snapshot-renderer-fixture.log)"
        }
        echo "  ok       $PORTABLE_SNAPSHOT_BUILD/ubiquitty-snapshot"
    fi
fi

# App-driven regression suite. A dedicated Linux job runs every portable case
# and showcase entry through ubiquitty-snapshot; macOS additionally runs its
# native CoreText/Metal catalog. This local convenience entry runs both macOS
# tiers. SNAPSHOT=skip is explicit opt-out.
#
# Both binaries come from full app builds, which a fresh worktree does not have —
# the same standing as the QUIC deps below, so an absent build skips rather than
# fails. Naming the stanza asks for it specifically, and then a missing build is
# the answer to the question and fails with the commands that produce it.
if wanted snapshots && [ "${SNAPSHOT:-}" = "skip" ]; then
    ubq_skip "snapshots: explicitly disabled with SNAPSHOT=skip"
    SKIPPED=$((SKIPPED + 1))
fi
if wanted snapshots && [ "${SNAPSHOT:-}" != "skip" ]; then
    PORTABLE_SNAPSHOT_BIN="$PORTABLE_SNAPSHOT_BUILD/ubiquitty-snapshot"
    MACOS_SNAPSHOT_BIN="$UBQ_BUILD_ROOT/macos/Source/Platform/macOS/Ubiquitty.app/Contents/MacOS/Ubiquitty"
    # SNAPSHOT=demand is what asks for the failure. Keying the skip on "no
    # filter was given" would let the run's shape decide, and every stanza is its
    # own ctest entry — which passes a filter — so such a guard would never fire.
    echo "=== snapshots ==="
    RAN=$((RAN + 1))
    # Asked per tier, because one prerequisite check over both is the same
    # coupling one level up: it would demand the macOS app before either tier
    # ran.
    # Bring the renderer up to date before asking it anything. Under ctest the
    # fixture already has, and a renderer stale here is refused, not rebuilt
    # inside a timeout sized for rendering. The prerequisite check below stays as
    # the backstop: a rebuild that produces nothing current is still refused.
    PORTABLE_SNAPSHOT_STATUS=0
    ubq_portable_snapshot_renderer "$OUT/snapshot-rebuild.log" || PORTABLE_SNAPSHOT_STATUS=$?
    if [ "$PORTABLE_SNAPSHOT_STATUS" = "2" ]; then
        build_broken "snapshots: the portable renderer is out of date after the fixture that builds it"
    elif [ "$PORTABLE_SNAPSHOT_STATUS" != "0" ]; then
        cat "$OUT/snapshot-rebuild.log" >&2
        build_broken "snapshots: the portable renderer did not build (see $OUT/snapshot-rebuild.log)"
    fi

    "$ROOT/Scripts/check-snapshot-prerequisites.py" "$ROOT" portable || exit 1

    # The two tiers have different prerequisites and are gated separately. One
    # condition over both would skip the tier needing nothing native for want of
    # a native build — and the skip prints inside a Passed entry, so the one test
    # that catches a portable render regression would report success without
    # running wherever no app build exists.
    #
    # ubiquitty-snapshot links Core and SoftwareRenderer: no app, no platform,
    # and the same bytes on macOS and Linux, which is what makes this tier the
    # cross-platform check rather than a macOS one.
    # No skip branch here, and the absence is the point: the prerequisite check
    # above already fails on a missing portable binary and prints the two
    # commands that build it. A branch below it could never run.
    #
    # Failing is also the right answer. ubiquitty-snapshot needs no app and no
    # platform, so "it is not built" is a thing to fix rather than a condition
    # to tolerate, and tolerating it hides regressions.
    python3 "$ROOT/Tests/Snapshots/run.py" --backend portable --quiet \
        --portable-binary "$PORTABLE_SNAPSHOT_BIN" || exit 1

    # The macOS tier genuinely needs a full app build, which a fresh worktree
    # does not have.
    # A stale app is the same situation as an absent one and gets the same
    # answer. The tier already skips when a fresh worktree has no app build;
    # an app older than the Core it renders with is equally "not the app for
    # this tree", and the portable tier's rebuild is not available here — a
    # full macOS app is not something a test run should build.
    #
    # Refusing instead would make every checkout red for a reason none of them
    # introduced.
    # SNAPSHOT=demand still turns it back into a failure for anyone who wants it.
    # The checker's one line of output is the prerequisite it found newer than
    # the app, and it is kept: a skip that says only "predates" sends the reader
    # to the whole tree, and a tier that skips on every run without naming why
    # leaves nobody able to say what it compares.
    MACOS_SNAPSHOT_STALE=0
    MACOS_SNAPSHOT_STALE_WHY=""
    if [ -x "$MACOS_SNAPSHOT_BIN" ] \
            && ! MACOS_SNAPSHOT_STALE_WHY="$(python3 "$ROOT/Scripts/snapshot-binary-is-current.py" \
                "$UBQ_BUILD_ROOT/macos" "$MACOS_SNAPSHOT_BIN" 2>&1)"; then
        MACOS_SNAPSHOT_STALE=1
    fi
    if [ "$(uname)" = "Darwin" ] && [ "$MACOS_SNAPSHOT_STALE" = "1" ]; then
        ubq_skip "snapshots: macOS tier — the app build predates the Core sources it renders"
        [ -n "$MACOS_SNAPSHOT_STALE_WHY" ] && echo "  ${MACOS_SNAPSHOT_STALE_WHY%%$'\n'*}"
        echo "  rebuild with Scripts/build-macos.sh — SNAPSHOT=demand to require it"
        SKIPPED=$((SKIPPED + 1))
        [ "${SNAPSHOT:-}" = "demand" ] && exit 1
    elif [ "$(uname)" = "Darwin" ] && [ -x "$MACOS_SNAPSHOT_BIN" ]; then
        python3 "$ROOT/Tests/Snapshots/run.py" --backend macos \
            --macos-binary "$MACOS_SNAPSHOT_BIN" || exit 1

        # The swift arm renders through TerminalSurface and SurfaceRenderer, out
        # of the app this branch has already built and checked for staleness.
        # A backend reachable by --backend swift and invoked by nothing renders
        # its cases for no gate, and a golden it disagrees with stands unread.
        python3 "$ROOT/Tests/Snapshots/run.py" --backend swift \
            --macos-binary "$MACOS_SNAPSHOT_BIN" || exit 1
    elif [ "$(uname)" = "Darwin" ]; then
        ubq_skip "snapshots: macOS tier — no app build"
        echo "  build with Scripts/build-macos.sh — SNAPSHOT=demand to require it"
        SKIPPED=$((SKIPPED + 1))
        [ "${SNAPSHOT:-}" = "demand" ] && exit 1
    fi

    # The web arm renders Core compiled to wasm, so it is the one tier that runs
    # the same source through a different toolchain. Skipped rather than failed
    # when the module is absent, matching the macOS branch above: a wasm build is
    # not something a test run should produce. run.py refuses a module older than
    # Core on its own, so staleness needs no branch here.
    # build/web-dist is what build-web.sh stages to and what the arm resolves.
    # A worktree can hold build/web as well, so guarding on that one tests a
    # file the arm does not read.
    WEB_SNAPSHOT_MODULE="$UBQ_BUILD_ROOT/web-dist/ubiquitty.wasm"
    if [ -f "$WEB_SNAPSHOT_MODULE" ]; then
        # The module is the prerequisite; the arm's launcher is a shim run.py
        # already defaults to. --web-binary names that launcher rather than the
        # module, and passing the module to it fails all 35 cases.
        #
        # run.py exits 2 when it refuses to render, which a module older than
        # Core is: the same situation as the stale app above, and it gets the
        # same answer, because a wasm build is not something a test run should
        # produce. Only a real disagreement (exit 1) fails the stanza.
        # Captured through || because this script runs under set -e, which
        # aborts at the failing command and never reaches a bare $?.
        WEB_SNAPSHOT_STATUS=0
        python3 "$ROOT/Tests/Snapshots/run.py" --backend web || WEB_SNAPSHOT_STATUS=$?
        if [ "$WEB_SNAPSHOT_STATUS" = "2" ]; then
            ubq_skip "snapshots: web tier — the wasm module predates the Core sources it renders"
            echo "  rebuild with Scripts/build-web.sh — SNAPSHOT=demand to require it"
            SKIPPED=$((SKIPPED + 1))
            [ "${SNAPSHOT:-}" = "demand" ] && exit 1
        elif [ "$WEB_SNAPSHOT_STATUS" != "0" ]; then
            exit 1
        fi
    else
        ubq_skip "snapshots: web tier — no wasm module"
        echo "  build with Scripts/build-web.sh — SNAPSHOT=demand to require it"
        SKIPPED=$((SKIPPED + 1))
        [ "${SNAPSHOT:-}" = "demand" ] && exit 1
    fi
fi

# host_smoke — a session outlives its client: the built daemon is attached,
# typed into, left and attached again through Tests/Host/smoke_client.cpp, and
# a command's line rides its typed completion. The smoke builds its client in the
# unit tests' tree, which is configured as it configures it. Skipped where
# cmake is absent or the host cannot be built.
if wanted host_smoke && command -v cmake >/dev/null 2>&1; then
    echo "=== host_smoke ==="
    if [ ! -f "$ROOT/Tests/Host/smoke.sh" ] || [ ! -f "$ROOT/Tests/Host/smoke_client.cpp" ]; then
        build_broken "host_smoke: the smoke or its client is missing from Tests/Host"
    elif HOST_BINARY="$(ubq_host_binary "$OUT/host-smoke-host-build.log")"; then
        RAN=$((RAN + 1))
        ubq_run_bounded 600 env UBIQUITTY_HOST_BIN="$HOST_BINARY" UBIQUITTY_SMOKE_BUILD="$CTEST_BUILD_DIR" UBQ_BUILD_JOBS="$BUILD_JOBS" \
            bash "$ROOT/Tests/Host/smoke.sh" || exit 1
    else
        build_broken "host_smoke Host build failed (see $OUT/host-smoke-host-build.log)"
    fi
fi

# host_soak — one daemon, a thousand attaches of one session through the
# --attach relay: its resident size stays within a bound once warm, and when
# the session ends it exits cleanly with no sanitizer report in its log.
# Tests/Host/soak.sh says what each bound means for a sanitized daemon.
if wanted host_soak && command -v cmake >/dev/null 2>&1; then
    echo "=== host_soak ==="
    if [ ! -f "$ROOT/Tests/Host/soak.sh" ] || [ ! -f "$ROOT/Tests/Host/smoke_client.cpp" ]; then
        build_broken "host_soak: the soak or its client is missing from Tests/Host"
    elif HOST_BINARY="$(ubq_host_binary "$OUT/host-soak-host-build.log")"; then
        RAN=$((RAN + 1))
        ubq_run_bounded 900 env UBIQUITTY_HOST_BIN="$HOST_BINARY" UBIQUITTY_SMOKE_BUILD="$CTEST_BUILD_DIR" UBQ_BUILD_JOBS="$BUILD_JOBS" \
            bash "$ROOT/Tests/Host/soak.sh" || exit 1
    else
        build_broken "host_soak Host build failed (see $OUT/host-soak-host-build.log)"
    fi
fi

if wanted agent_smoke; then
    echo "=== agent_smoke ==="
    RAN=$((RAN + 1))
    AGENT_BINARY="$(ubq_agent_binary "$OUT/agent_smoke-agent-build.log")" \
        || build_broken "agent_smoke: ubq did not build (see $OUT/agent_smoke-agent-build.log)"
    AGENT_HOST="$(ubq_agent_host_binary "$OUT/agent_smoke-host-build.log")" \
        || build_broken "agent_smoke: ubiquitty-host did not build (see $OUT/agent_smoke-host-build.log)"
    ubq_run_private env UBQ_BIN="$AGENT_BINARY" UBIQUITTY_HOST_BIN="$AGENT_HOST" bash "$ROOT/Tests/Agent/smoke.sh" || exit 1
fi

if wanted agent_auth_smoke; then
    echo "=== agent_auth_smoke ==="
    RAN=$((RAN + 1))
    AGENT_BINARY="$(ubq_agent_binary "$OUT/agent_auth_smoke-agent-build.log")" \
        || build_broken "agent_auth_smoke: ubq did not build (see $OUT/agent_auth_smoke-agent-build.log)"
    AGENT_HOST="$(ubq_agent_host_binary "$OUT/agent_auth_smoke-host-build.log")" \
        || build_broken "agent_auth_smoke: ubiquitty-host did not build (see $OUT/agent_auth_smoke-host-build.log)"
    ubq_run_private env UBQ_BIN="$AGENT_BINARY" UBIQUITTY_HOST_BIN="$AGENT_HOST" bash "$ROOT/Tests/Agent/auth_smoke.sh" || exit 1
fi

if wanted agent_mcp_smoke; then
    echo "=== agent_mcp_smoke ==="
    RAN=$((RAN + 1))
    AGENT_BINARY="$(ubq_agent_binary "$OUT/agent_mcp_smoke-agent-build.log")" \
        || build_broken "agent_mcp_smoke: ubq did not build (see $OUT/agent_mcp_smoke-agent-build.log)"
    AGENT_HOST="$(ubq_agent_host_binary "$OUT/agent_mcp_smoke-host-build.log")" \
        || build_broken "agent_mcp_smoke: ubiquitty-host did not build (see $OUT/agent_mcp_smoke-host-build.log)"
    ubq_run_private env UBQ_BIN="$AGENT_BINARY" UBIQUITTY_HOST_BIN="$AGENT_HOST" bash "$ROOT/Tests/Agent/mcp_smoke.sh" || exit 1
fi

if wanted agent_completions_smoke; then
    echo "=== agent_completions_smoke ==="
    RAN=$((RAN + 1))
    AGENT_BINARY="$(ubq_agent_binary "$OUT/agent_completions_smoke-agent-build.log")" \
        || build_broken "agent_completions_smoke: ubq did not build (see $OUT/agent_completions_smoke-agent-build.log)"
    ubq_run_private env UBQ_BIN="$AGENT_BINARY" bash "$ROOT/Tests/Agent/completions_smoke.sh" || exit 1
fi

# ubq_malformed_sweep — discover every current command-line verb and refuse
# malformed arguments before any daemon can answer.
if wanted ubq_malformed_sweep; then
    echo "=== ubq_malformed_sweep ==="
    RAN=$((RAN + 1))
    AGENT_BINARY="$(ubq_agent_binary "$OUT/ubq_malformed_sweep-agent-build.log")" \
        || build_broken "ubq_malformed_sweep: ubq did not build (see $OUT/ubq_malformed_sweep-agent-build.log)"
    ubq_run_bounded 180 env UBQ_BIN="$AGENT_BINARY" python3 "$ROOT/Tests/Agent/ubq_malformed_sweep.py" || exit 1
fi

# ubq_smoke — the command line against a live, isolated daemon: every verb is
# a registry operation with a schema and a bad parameter is refused with the
# schema's sentence; run returns the command's status; wait --for exit wakes
# on the event; events --since resumes without a gap; status shows an OSC
# 7501 record; targets resolve by label and here; -H reaches a daemon through
# the ssh bootstrap, with a stand-in ssh on PATH.
#
# It and ubq_smoke_cleanup each ask for their own binaries: the fixture
# inventories (--list-host-binary-stanzas) read only a stanza's own body.
if wanted ubq_smoke; then
    echo "=== ubq_smoke ==="
    RAN=$((RAN + 1))
    AGENT_BINARY="$(ubq_agent_binary "$OUT/ubq_smoke-agent-build.log")" \
        || build_broken "ubq_smoke: ubq did not build (see $OUT/ubq_smoke-agent-build.log)"
    AGENT_HOST="$(ubq_agent_host_binary "$OUT/ubq_smoke-host-build.log")" \
        || build_broken "ubq_smoke: ubiquitty-host did not build (see $OUT/ubq_smoke-host-build.log)"
    ubq_consent_person "$AGENT_HOST" "$ROOT/Tests/Agent/consent_person.cpp"
    # Bounded above the smoke's own deadline (UBQ_SMOKE_DEADLINE_SECONDS,
    # 1200), so the smoke's trap stops its daemons before this ever
    # signals it.
    ubq_run_bounded 1500 env UBQ_BIN="$AGENT_BINARY" UBIQUITTY_HOST_BIN="$AGENT_HOST" UBQ_CONSENT_PERSON_BIN="$CONSENT_PERSON" \
        bash "$ROOT/Tests/Agent/ubq_smoke.sh" || exit 1
fi

# ubq_smoke_cleanup — a smoke ended by the runner's limit leaves no daemon
# behind: one is started, killed the way a timed-out stanza is, and no
# ubiquitty-host may remain whose home is under its temporary directory.
if wanted ubq_smoke_cleanup; then
    echo "=== ubq_smoke_cleanup ==="
    RAN=$((RAN + 1))
    AGENT_BINARY="$(ubq_agent_binary "$OUT/ubq_smoke_cleanup-agent-build.log")" \
        || build_broken "ubq_smoke_cleanup: ubq did not build (see $OUT/ubq_smoke_cleanup-agent-build.log)"
    AGENT_HOST="$(ubq_agent_host_binary "$OUT/ubq_smoke_cleanup-host-build.log")" \
        || build_broken "ubq_smoke_cleanup: ubiquitty-host did not build (see $OUT/ubq_smoke_cleanup-host-build.log)"
    ubq_consent_person "$AGENT_HOST" "$ROOT/Tests/Agent/consent_person.cpp"
    UBQ_BIN="$AGENT_BINARY" UBIQUITTY_HOST_BIN="$AGENT_HOST" UBQ_CONSENT_PERSON_BIN="$CONSENT_PERSON" \
        UBQ_SMOKE_SCRIPT="$ROOT/Tests/Agent/ubq_smoke.sh" bash "$ROOT/Tests/Agent/ubq_smoke_cleanup.sh" || exit 1
fi

# agent_screen_diff — computeScreenDiff, the pure projection→projection diff behind
# the agent's read_changes tool. Needs no daemon and no QUIC: it links only
# AgentSession.cpp and drops the rest of that TU at link (dead_strip / --gc-sections),
# so no Sync/PAL/Terminal sources ride along.
if wanted agent_screen_diff; then
    echo "=== agent_screen_diff ==="
    RAN=$((RAN + 1))
    ASD_STRIP="-Wl,-dead_strip"
    [ "$(uname)" != "Darwin" ] && ASD_STRIP="-ffunction-sections -fdata-sections -Wl,--gc-sections"
    if ubq_compile_parallel "$OUT/agent_screen_diff" "$OUT/objects/agent_screen_diff" "$OUT/agent-screen-diff-build.log" \
            -std=c++23 -I"$ROOT/Source" $ASD_STRIP -- \
            "$ROOT/Tests/Agent/agent_screen_diff.cpp" \
            "$ROOT/Source/Agent/AgentSession.cpp"; then
        "$OUT/agent_screen_diff" || exit 1
    else
        build_broken "agent_screen_diff build failed (see $OUT/agent-screen-diff-build.log)"
    fi
fi

# agent_auth_text — authDeniedReasonText maps every refusal ByeReason to a specific
# message, not the generic fallback. Same minimal link as agent_screen_diff: only
# AgentSession.cpp, the rest dropped at link.
if wanted agent_auth_text; then
    echo "=== agent_auth_text ==="
    RAN=$((RAN + 1))
    AAT_STRIP="-Wl,-dead_strip"
    [ "$(uname)" != "Darwin" ] && AAT_STRIP="-ffunction-sections -fdata-sections -Wl,--gc-sections"
    if ubq_compile_parallel "$OUT/agent_auth_text" "$OUT/objects/agent_auth_text" "$OUT/agent-auth-text-build.log" \
            -std=c++23 -I"$ROOT/Source" $AAT_STRIP -- \
            "$ROOT/Tests/Agent/agent_auth_text.cpp" \
            "$ROOT/Source/Agent/AgentSession.cpp"; then
        "$OUT/agent_auth_text" || exit 1
    else
        build_broken "agent_auth_text build failed (see $OUT/agent-auth-text-build.log)"
    fi
fi

# QUIC loopback — only when the QUIC stack is built (Scripts/fetch-quic-deps.sh).
# Brings up an ngtcp2 + AWS-LC server and client over localhost UDP and drives a
# real TLS 1.3 handshake to completion, confirming the vendored stack works end
# to end. Skipped (not failed) when the deps aren't present.
if wanted quic_loopback && quic_deps_present; then
    echo "=== quic_loopback ==="
    RAN=$((RAN + 1))
    QUIC_FRAMEWORKS=""
    [ "$(uname)" = "Darwin" ] && QUIC_FRAMEWORKS="-framework CoreFoundation -framework Security"
    if ubq_compile_parallel "$OUT/quic_loopback" "$OUT/objects/quic_loopback" "$OUT/quic-loopback-build.log" \
            -std=c++23 -I"$ROOT/Tests" -I"$QUIC_PREFIX/include" -- \
            "$ROOT/Tests/Host/quic_loopback.cpp" \
            -- "$QUIC_PREFIX/lib/libngtcp2_crypto_boringssl.a" "$QUIC_PREFIX/lib/libngtcp2.a" "$QUIC_PREFIX/lib/libssl.a" "$QUIC_PREFIX/lib/libcrypto.a" $QUIC_FRAMEWORKS; then
        "$OUT/quic_loopback" || exit 1
    else
        build_broken "quic_loopback build failed (see $OUT/quic-loopback-build.log)"
    fi
fi

# quic_channel_lifecycle — QUICChannel's client-wrapper lifetime seam, driven with
# no daemon: the drop-when-no-client guards and the cross-thread ref-swap accessors
# that no integration test reaches. Links only QUICChannel + QUICClient against the
# vendored stack (start() dials a host and stays with the loopback/accept stanzas).
if wanted quic_channel_lifecycle && quic_deps_present; then
    echo "=== quic_channel_lifecycle ==="
    RAN=$((RAN + 1))
    if ubq_build_test ubq-shell-quic_channel_lifecycle "$OUT/quic_channel_lifecycle" "$OUT/quic-channel-lifecycle-build.log"; then
        "$OUT/quic_channel_lifecycle" || exit 1
    else
        build_broken "quic_channel_lifecycle build failed (see $OUT/quic-channel-lifecycle-build.log)"
    fi
fi

# Authenticated local handover admission; isolated Unix sockets and child peers,
# with no daemon, user host, or QUIC dependency.
if wanted handover_admission; then
    echo "=== handover_admission ==="
    RAN=$((RAN + 1))
    if ubq_build_test ubq-shell-handover_admission "$OUT/handover_admission" "$OUT/handover-admission-build.log"; then
        ubq_run_private "$OUT/handover_admission" || exit 1
    else
        build_broken "handover_admission build failed (see $OUT/handover-admission-build.log)"
    fi
fi

# Nonblocking first-record handover routing; isolated sockets and owned descriptors.
if wanted handover_request; then
    echo "=== handover_request ==="
    RAN=$((RAN + 1))
    if ubq_build_test ubq-shell-handover_request "$OUT/handover_request" "$OUT/handover-request-build.log"; then
        ubq_run_bounded 60 "$OUT/handover_request" || exit 1
    else
        build_broken "handover_request build failed (see $OUT/handover-request-build.log)"
    fi
fi

# Process-associated handover lease; real forked SCM_RIGHTS peers, no Core archive.
if wanted handover_lease; then
    echo "=== handover_lease ==="
    RAN=$((RAN + 1))
    if ubq_compile_parallel "$OUT/handover_lease" "$OUT/objects/handover_lease" "$OUT/handover-lease-build.log" \
            -std=c++23 -DUBIQUITTY_TEST_WITHOUT_CORE -I"$ROOT/Source" -I"$ROOT/Tests" -- \
            "$ROOT/Tests/Host/handover_lease.cpp" \
            "$ROOT/Source/Platform/POSIX/HandoverLease.cpp" \
            "$ROOT/Source/Platform/POSIX/HandoverTransport.cpp" \
            --; then
        ubq_run_bounded 60 "$OUT/handover_lease" || exit 1
    else
        build_broken "handover_lease build failed (see $OUT/handover-lease-build.log)"
    fi
fi

# QUIC daemon-accept — drives the daemon's real HostQUIC.cpp QUICListener through
# a handshake (a client vs the actual per-connection server, not an inline one).
if wanted quic_daemon_accept && quic_deps_present; then
    echo "=== quic_daemon_accept ==="
    RAN=$((RAN + 1))
    if ubq_build_test ubq-shell-quic_daemon_accept "$OUT/quic_daemon_accept" "$OUT/quic-daemon-accept-build.log"; then
        "$OUT/quic_daemon_accept" || exit 1
        "$OUT/quic_daemon_accept" --transferred-socket || exit 1
    else
        build_broken "quic_daemon_accept build failed (see $OUT/quic-daemon-accept-build.log)"
    fi
fi

# Bound QUIC socket ownership, with no running daemon or user control directory.
if wanted quic_bound_socket && quic_deps_present; then
    echo "=== quic_bound_socket ==="
    RAN=$((RAN + 1))
    if ubq_build_test ubq-shell-quic_bound_socket "$OUT/quic_bound_socket" "$OUT/quic-bound-socket-build.log"; then
        ubq_run_bounded 60 "$OUT/quic_bound_socket" || exit 1
    else
        build_broken "quic_bound_socket build failed (see $OUT/quic-bound-socket-build.log)"
    fi
fi

# QUIC session-relay — the full path: spawn the REAL `ubiquitty-host --daemon
# --quic-port N`, dial it with an ngtcp2 client, and speak Sync over the stream
# — one Greet must yield a Granted Welcome and an applied checkpoint/update.
# The production client reader links the same-tree Host Core objects. Needs a
# QUIC-enabled daemon, so it cmake-builds one under build/host-quic first.
if wanted quic_session_relay && quic_deps_present && command -v cmake >/dev/null 2>&1; then
    echo "=== quic_session_relay ==="
    RAN=$((RAN + 1))
    HOSTQ_BUILD="$UBQ_BUILD_ROOT/host-quic"
    HOSTQ_OBJDIR="$HOSTQ_BUILD/CMakeFiles/ubiquitty-host.dir"
    QUIC_RELAY_LINK_INPUTS=("${HOST_LINK_ARCHIVES[@]}")
    [ "$(uname)" = "Linux" ] && QUIC_RELAY_LINK_INPUTS+=(-lutil)
    if cmake -B "$HOSTQ_BUILD" -S "$ROOT" -G "$UBQ_GENERATOR" -DUBIQUITTY_FRONTEND=Host -DCMAKE_BUILD_TYPE=Debug \
                -DUBIQUITTY_QUIC=ON >"$OUT/quic-relay-daemon-build.log" 2>&1 \
            && cmake --build "$HOSTQ_BUILD" -j"$BUILD_JOBS" >>"$OUT/quic-relay-daemon-build.log" 2>&1 \
            && host_objects "$HOSTQ_OBJDIR" "$OUT/quic-relay-objs.txt" \
            && ubq_compile_parallel "$OUT/quic_session_relay" "$OUT/objects/quic_session_relay" "$OUT/quic-relay-build.log" \
                    -std=c++23 -DUBIQUITTY_QUIC=1 -I"$ROOT/Tests" -I"$ROOT/Source" -I"$QUIC_PREFIX/include" -- \
                    "$ROOT/Tests/Host/quic_session_relay.cpp" \
                    -- "@$OUT/quic-relay-objs.txt" "${QUIC_RELAY_LINK_INPUTS[@]}"; then
        ubq_run_private "$OUT/quic_session_relay" "$HOSTQ_BUILD/ubiquitty-host" || exit 1
    else
        build_broken "quic_session_relay build failed (see $OUT/quic-relay-build.log + $OUT/quic-relay-daemon-build.log)"
    fi
fi

# quic_user_identity — the person dialling in from a phone is a person to the
# consent gate, and nothing else arriving over QUIC is. originOf answers Remote
# for any client holding a quicConnection and isAgentClient is origin != Local, so
# an owner attaching the session they left running is a third-party agent:
# AUTH_DENIED(Consent) on an unattended session, and skipped by broadcastPresence.
# The bootstrap token a uid-gated local client mints already means "acts as that
# authenticated user"; it carries that identity too, and the mint is unreachable
# from QUIC. This drives the real daemon over ngtcp2 for the attach, the snapshot
# and the presence row, and at the friend seam pins that a labelled bearer, a
# persisted-token client and a tokenless remote peer all stay agents — deriving
# identity from origin rather than from the absence of a token, so a peer that
# presents none cannot read as a person — that a SESSION_LIST row names an
# OSC 7-silent shell's real cwd, that a 4096-byte workspace name survives intact,
# and that an oversized workspace name is refused before creating a session.
# Links the QUIC-ON daemon objects for the seam and spawns the same binary for the
# wire, so it needs both builds the sections around it need.
if wanted quic_user_identity && quic_deps_present && command -v cmake >/dev/null 2>&1; then
    echo "=== quic_user_identity ==="
    RAN=$((RAN + 1))
    HOSTQ_BUILD="$UBQ_BUILD_ROOT/host-quic"
    HOSTQ_OBJDIR="$HOSTQ_BUILD/CMakeFiles/ubiquitty-host.dir"
    if cmake -B "$HOSTQ_BUILD" -S "$ROOT" -G "$UBQ_GENERATOR" -DUBIQUITTY_FRONTEND=Host -DCMAKE_BUILD_TYPE=Debug \
                -DUBIQUITTY_QUIC=ON >"$OUT/quic-useridentity-host-build.log" 2>&1 \
            && cmake --build "$HOSTQ_BUILD" -j"$BUILD_JOBS" >>"$OUT/quic-useridentity-host-build.log" 2>&1; then
        # Link against every daemon object except the binary's own main().
        host_objects "$HOSTQ_OBJDIR" "$OUT/quic-useridentity-objs.txt" \
            || build_broken "host object list is empty (the harness, not the test under it)"
        if ubq_compile_parallel "$OUT/quic_user_identity" "$OUT/objects/quic_user_identity" "$OUT/quic-useridentity-build.log" \
                -std=c++23 -DUBIQUITTY_QUIC=1 -I"$QUIC_PREFIX/include" -I"$ROOT/Tests" -I"$ROOT/Source" -- \
                "$ROOT/Tests/Host/quic_user_identity.cpp" \
                -- "@$OUT/quic-useridentity-objs.txt" "${HOST_LINK_ARCHIVES[@]}"; then
            ubq_run_private "$OUT/quic_user_identity" "$HOSTQ_BUILD/ubiquitty-host" || exit 1
        else
            build_broken "quic_user_identity link failed (see $OUT/quic-useridentity-build.log)"
        fi
    else
        build_broken "quic_user_identity host build failed (see $OUT/quic-useridentity-host-build.log)"
    fi
fi

# QUIC client-relay — drives the PRODUCTION portable Source/Core/Transport/QUICClient (the
# class the macOS + Linux frontends wrap) against the real daemon: start()s to
# `--daemon --quic-port`, sends a Sync HELLO on onConnect, asserts HELLO_ACK +
# a checkpoint arrive via onData. Reuses the QUIC daemon built by the section above.
if wanted quic_client_relay && quic_deps_present && command -v cmake >/dev/null 2>&1; then
    echo "=== quic_client_relay ==="
    RAN=$((RAN + 1))
    QUIC_FRAMEWORKS=""
    [ "$(uname)" = "Darwin" ] && QUIC_FRAMEWORKS="-framework CoreFoundation -framework Security"
    HOSTQ_BUILD="$UBQ_BUILD_ROOT/host-quic"
    HOSTQ_OBJDIR="$HOSTQ_BUILD/CMakeFiles/ubiquitty-host.dir"
    if cmake -B "$HOSTQ_BUILD" -S "$ROOT" -G "$UBQ_GENERATOR" -DUBIQUITTY_FRONTEND=Host -DCMAKE_BUILD_TYPE=Debug \
                -DUBIQUITTY_QUIC=ON >"$OUT/quic-relay-daemon-build.log" 2>&1 \
            && cmake --build "$HOSTQ_BUILD" -j"$BUILD_JOBS" >>"$OUT/quic-relay-daemon-build.log" 2>&1; then
        host_objects "$HOSTQ_OBJDIR" "$OUT/quic-relay-objs.txt" \
            || build_broken "host object list is empty (the harness, not the test under it)"
        # The test speaks canonical Sync frames, so it links the daemon's own
        # objects for the codec, as quic_resume_proof does.
        if ubq_compile_parallel "$OUT/quic_client_relay" "$OUT/objects/quic_client_relay" "$OUT/quic-client-build.log" \
                -std=c++23 -I"$ROOT/Tests" -DUBIQUITTY_QUIC=1 -DUBIQUITTY_ENABLE_TEST_HOOKS=1 -pthread -I"$ROOT/Source" -I"$QUIC_PREFIX/include" -- \
                "$ROOT/Tests/Host/quic_client_relay.cpp" \
                "$ROOT/Source/Platform/POSIX/QUICClient.cpp" \
                -- "@$OUT/quic-relay-objs.txt" "$QUIC_PREFIX/lib/libngtcp2_crypto_boringssl.a" "$QUIC_PREFIX/lib/libngtcp2.a" "$QUIC_PREFIX/lib/libssl.a" "$QUIC_PREFIX/lib/libcrypto.a" "${HOST_LINK_ARCHIVES[@]}" $QUIC_FRAMEWORKS; then
            ubq_run_private "$OUT/quic_client_relay" "$HOSTQ_BUILD/ubiquitty-host" || exit 1
        else
            build_broken "quic_client_relay build failed (see $OUT/quic-client-build.log + $OUT/quic-relay-daemon-build.log)"
        fi
    else
        build_broken "quic_client_relay host build failed (see $OUT/quic-relay-daemon-build.log)"
    fi
fi

# quic_resume_proof — a QUIC connection resumes its attachment by a proof in
# its preamble: the production QUICClient attaches to a spawned host with a
# token, drops, and dials again leading with [2][proof]; the pane continues
# with a diff, and a replayed proof and a proof for a detached attachment are
# refused. Links the daemon objects for the Sync client and QUICClient.cpp.
if wanted quic_resume_proof && quic_deps_present && command -v cmake >/dev/null 2>&1; then
    echo "=== quic_resume_proof ==="
    RAN=$((RAN + 1))
    QUIC_FRAMEWORKS=""
    [ "$(uname)" = "Darwin" ] && QUIC_FRAMEWORKS="-framework CoreFoundation -framework Security"
    HOSTQ_BUILD="$UBQ_BUILD_ROOT/host-quic"
    HOSTQ_OBJDIR="$HOSTQ_BUILD/CMakeFiles/ubiquitty-host.dir"
    if cmake -B "$HOSTQ_BUILD" -S "$ROOT" -G "$UBQ_GENERATOR" -DUBIQUITTY_FRONTEND=Host -DCMAKE_BUILD_TYPE=Debug \
                -DUBIQUITTY_QUIC=ON >"$OUT/quic-resume-proof-host-build.log" 2>&1 \
            && cmake --build "$HOSTQ_BUILD" -j"$BUILD_JOBS" >>"$OUT/quic-resume-proof-host-build.log" 2>&1; then
        host_objects "$HOSTQ_OBJDIR" "$OUT/quic-resume-proof-objs.txt" \
            || build_broken "host object list is empty (the harness, not the test under it)"
        if ubq_compile_parallel "$OUT/quic_resume_proof" "$OUT/objects/quic_resume_proof" "$OUT/quic-resume-proof-build.log" \
                -std=c++23 -I"$ROOT/Tests" -DUBIQUITTY_QUIC=1 -pthread -I"$ROOT/Source" -I"$QUIC_PREFIX/include" -- \
                "$ROOT/Tests/Host/quic_resume_proof.cpp" \
                "$ROOT/Source/Platform/POSIX/QUICClient.cpp" \
                -- "@$OUT/quic-resume-proof-objs.txt" "$QUIC_PREFIX/lib/libngtcp2_crypto_boringssl.a" "$QUIC_PREFIX/lib/libngtcp2.a" "$QUIC_PREFIX/lib/libssl.a" "$QUIC_PREFIX/lib/libcrypto.a" "${HOST_LINK_ARCHIVES[@]}" $QUIC_FRAMEWORKS; then
            ubq_run_bounded 300 "$OUT/quic_resume_proof" "$HOSTQ_BUILD/ubiquitty-host" || exit 1
        else
            build_broken "quic_resume_proof build failed (see $OUT/quic-resume-proof-build.log)"
        fi
    else
        build_broken "quic_resume_proof host build failed (see $OUT/quic-resume-proof-host-build.log)"
    fi
fi

# quic_backlog_close — a QUIC client pushed past its outbound high-water mark
# is closed with Bye{OutputBacklog} carried in its application CONNECTION_CLOSE,
# which the production QUICClient reads over real loopback QUIC. Links the
# daemon objects and QUICClient.cpp; the daemon runs in process.
if wanted quic_backlog_close && quic_deps_present && command -v cmake >/dev/null 2>&1; then
    echo "=== quic_backlog_close ==="
    RAN=$((RAN + 1))
    QUIC_FRAMEWORKS=""
    [ "$(uname)" = "Darwin" ] && QUIC_FRAMEWORKS="-framework CoreFoundation -framework Security"
    HOSTQ_BUILD="$UBQ_BUILD_ROOT/host-quic"
    HOSTQ_OBJDIR="$HOSTQ_BUILD/CMakeFiles/ubiquitty-host.dir"
    if cmake -B "$HOSTQ_BUILD" -S "$ROOT" -G "$UBQ_GENERATOR" -DUBIQUITTY_FRONTEND=Host -DCMAKE_BUILD_TYPE=Debug \
                -DUBIQUITTY_QUIC=ON >"$OUT/quic-backlog-close-host-build.log" 2>&1 \
            && cmake --build "$HOSTQ_BUILD" -j"$BUILD_JOBS" >>"$OUT/quic-backlog-close-host-build.log" 2>&1; then
        host_objects "$HOSTQ_OBJDIR" "$OUT/quic-backlog-close-objs.txt" \
            || build_broken "host object list is empty (the harness, not the test under it)"
        if ubq_compile_parallel "$OUT/quic_backlog_close" "$OUT/objects/quic_backlog_close" "$OUT/quic-backlog-close-build.log" \
                -std=c++23 -I"$ROOT/Tests" -DUBIQUITTY_QUIC=1 -pthread -I"$ROOT/Source" -I"$QUIC_PREFIX/include" -- \
                "$ROOT/Tests/Host/quic_backlog_close.cpp" \
                "$ROOT/Source/Platform/POSIX/QUICClient.cpp" \
                -- "@$OUT/quic-backlog-close-objs.txt" "$QUIC_PREFIX/lib/libngtcp2_crypto_boringssl.a" "$QUIC_PREFIX/lib/libngtcp2.a" "$QUIC_PREFIX/lib/libssl.a" "$QUIC_PREFIX/lib/libcrypto.a" "${HOST_LINK_ARCHIVES[@]}" $QUIC_FRAMEWORKS; then
            ubq_run_bounded 300 "$OUT/quic_backlog_close" || exit 1
        else
            build_broken "quic_backlog_close build failed (see $OUT/quic-backlog-close-build.log)"
        fi
    else
        build_broken "quic_backlog_close host build failed (see $OUT/quic-backlog-close-host-build.log)"
    fi
fi

# quic_client_migration — a QUIC client whose UDP socket dies moves its
# connection to a fresh socket and keeps it: the production QUICClient
# attaches to a spawned host, its socket is closed under it, and input typed
# after the move is written on the same attachment; told its interface is
# gone, it moves again. Links the shared Core and a QUIC client carrier
# compiled with the same test hooks as the harness.
# UBIQUITTY_QUIC changes SHA256Hasher's layout; the portable archive is built without it.
if wanted quic_client_migration && quic_deps_present && command -v cmake >/dev/null 2>&1; then
    echo "=== quic_client_migration ==="
    RAN=$((RAN + 1))
    QUIC_FRAMEWORKS=""
    [ "$(uname)" = "Darwin" ] && QUIC_FRAMEWORKS="-framework CoreFoundation -framework Security"
    HOSTQ_BUILD="$UBQ_BUILD_ROOT/host-quic"
    HOSTQ_OBJDIR="$HOSTQ_BUILD/CMakeFiles/ubiquitty-host.dir"
    if cmake -B "$HOSTQ_BUILD" -S "$ROOT" -G "$UBQ_GENERATOR" -DUBIQUITTY_FRONTEND=Host -DCMAKE_BUILD_TYPE=Debug \
                -DUBIQUITTY_QUIC=ON >"$OUT/quic-client-migration-host-build.log" 2>&1 \
            && cmake --build "$HOSTQ_BUILD" -j"$BUILD_JOBS" >>"$OUT/quic-client-migration-host-build.log" 2>&1; then
        host_objects "$HOSTQ_OBJDIR" "$OUT/quic-client-migration-objs.txt" \
            || build_broken "host object list is empty (the harness, not the test under it)"
        if ubq_compile_parallel "$OUT/quic_client_migration" "$OUT/objects/quic_client_migration" "$OUT/quic-client-migration-build.log" \
                -std=c++23 -I"$ROOT/Tests" -DUBIQUITTY_QUIC=1 -DUBIQUITTY_ENABLE_TEST_HOOKS=1 -pthread -I"$ROOT/Source" -I"$QUIC_PREFIX/include" -- \
                "$ROOT/Tests/Host/quic_client_migration.cpp" \
                "$ROOT/Source/Platform/POSIX/QUICClient.cpp" \
                "$ROOT/Source/Core/Transport/QUICChannel.cpp" \
                -- "@$OUT/quic-client-migration-objs.txt" "$QUIC_PREFIX/lib/libngtcp2_crypto_boringssl.a" "$QUIC_PREFIX/lib/libngtcp2.a" "$QUIC_PREFIX/lib/libssl.a" "$QUIC_PREFIX/lib/libcrypto.a" "${HOST_LINK_ARCHIVES[@]}" $QUIC_FRAMEWORKS; then
            ubq_run_bounded 300 "$OUT/quic_client_migration" "$HOSTQ_BUILD/ubiquitty-host" || exit 1
        else
            build_broken "quic_client_migration build failed (see $OUT/quic-client-migration-build.log)"
        fi
    else
        build_broken "quic_client_migration host build failed (see $OUT/quic-client-migration-host-build.log)"
    fi
fi

# quic_workspace_isolation — a person on another machine changes only the
# workspace their connection is attached to (Docs/sync.md §7.3): two people
# dial a spawned host over real loopback QUIC, each for a workspace of their
# own; the first is refused workspace.split, addTab, addWindow, closePane and
# window.close on the second's workspace, which keeps its revision, and is
# served all five on its own. Links the daemon objects for the Sync client and
# QUICClient.cpp.
if wanted quic_workspace_isolation && quic_deps_present && command -v cmake >/dev/null 2>&1; then
    echo "=== quic_workspace_isolation ==="
    RAN=$((RAN + 1))
    QUIC_FRAMEWORKS=""
    [ "$(uname)" = "Darwin" ] && QUIC_FRAMEWORKS="-framework CoreFoundation -framework Security"
    HOSTQ_BUILD="$UBQ_BUILD_ROOT/host-quic"
    HOSTQ_OBJDIR="$HOSTQ_BUILD/CMakeFiles/ubiquitty-host.dir"
    if cmake -B "$HOSTQ_BUILD" -S "$ROOT" -G "$UBQ_GENERATOR" -DUBIQUITTY_FRONTEND=Host -DCMAKE_BUILD_TYPE=Debug \
                -DUBIQUITTY_QUIC=ON >"$OUT/quic-workspace-isolation-host-build.log" 2>&1 \
            && cmake --build "$HOSTQ_BUILD" -j"$BUILD_JOBS" >>"$OUT/quic-workspace-isolation-host-build.log" 2>&1; then
        host_objects "$HOSTQ_OBJDIR" "$OUT/quic-workspace-isolation-objs.txt" \
            || build_broken "host object list is empty (the harness, not the test under it)"
        if ubq_compile_parallel "$OUT/quic_workspace_isolation" "$OUT/objects/quic_workspace_isolation" "$OUT/quic-workspace-isolation-build.log" \
                -std=c++23 -I"$ROOT/Tests" -DUBIQUITTY_QUIC=1 -pthread -I"$ROOT/Source" -I"$QUIC_PREFIX/include" -- \
                "$ROOT/Tests/Host/quic_workspace_isolation.cpp" \
                "$ROOT/Source/Platform/POSIX/QUICClient.cpp" \
                -- "@$OUT/quic-workspace-isolation-objs.txt" "$QUIC_PREFIX/lib/libngtcp2_crypto_boringssl.a" "$QUIC_PREFIX/lib/libngtcp2.a" "$QUIC_PREFIX/lib/libssl.a" "$QUIC_PREFIX/lib/libcrypto.a" "${HOST_LINK_ARCHIVES[@]}" $QUIC_FRAMEWORKS; then
            ubq_run_bounded 300 "$OUT/quic_workspace_isolation" "$HOSTQ_BUILD/ubiquitty-host" || exit 1
        else
            build_broken "quic_workspace_isolation build failed (see $OUT/quic-workspace-isolation-build.log)"
        fi
    else
        build_broken "quic_workspace_isolation host build failed (see $OUT/quic-workspace-isolation-host-build.log)"
    fi
fi

# sanitized_sync — the Sync, Session and Runtime unit tests under
# AddressSanitizer and UndefinedBehaviorSanitizer. A connection released from
# inside its own read, or a pane core released by the observer it has just
# told, reads freed memory and passes every plain run while the allocator has
# not reused the block. Built in a tree of its own under the build root with
# UBIQUITTY_SANITIZERS, from the sources the globs below name; a source with no
# CMake target is said, and so is a toolchain with no sanitizer runtime, which
# declines rather than passing. A report of either sanitizer ends the test that
# drew it: UndefinedBehaviorSanitizer is told to halt, since its default is to
# print and carry on to a green exit.
#
# macOS alone. The tree g++ builds for these tests is 15 GB a checkout, for
# code the macOS lane already runs under both sanitizers, so anywhere else the
# stanza says so and declines, and the selector leaves it out of what it runs
# there (Scripts/check-tests-reaching-the-diff.py reads this opening line).
if wanted sanitized_sync && [ "$(uname -s)" != "Darwin" ]; then
    echo "=== sanitized_sync (address, undefined) ==="
    ubq_skip "sanitized_sync is not run on $(uname -s): the macOS lane runs these tests under AddressSanitizer and UndefinedBehaviorSanitizer, and the tree g++ builds for them is 15 GB a checkout"
    SKIPPED=$((SKIPPED + 1))
elif wanted sanitized_sync && command -v cmake >/dev/null 2>&1; then
    echo "=== sanitized_sync (address, undefined) ==="
    printf 'int main(){return 0;}\n' > "$OUT/santest-sync.cpp"
    # shellcheck disable=SC2086
    if ! $CXX_TEST -fsanitize=address,undefined "$OUT/santest-sync.cpp" -o "$OUT/santest_sync" 2>/dev/null \
            || ! "$OUT/santest_sync" >/dev/null 2>&1; then
        ubq_skip "${CXX:-c++} has no working -fsanitize=address,undefined runtime"
        SKIPPED=$((SKIPPED + 1))
    else
        RAN=$((RAN + 1))
        SANITIZED_BUILD="$UBQ_BUILD_ROOT/sanitized-sync"
        SANITIZED_LOG="$OUT/sanitized-sync-build.log"
        SANITIZED_QUIC=OFF
        quic_deps_present && SANITIZED_QUIC=ON
        SANITIZED_GLOBS=(
            "$ROOT/Tests/Core/Protocol/Sync/test_sync_abort.cpp"
            "$ROOT/Tests/Core/Protocol/Sync/test_sync_admission.cpp"
            "$ROOT/Tests/Core/Protocol/Sync/test_sync_app_version.cpp"
            "$ROOT/Tests/Core/Protocol/Sync/test_sync_attachments.cpp"
            "$ROOT/Tests/Core/Protocol/Sync/test_sync_cadence.cpp"
            "$ROOT/Tests/Core/Protocol/Sync/test_sync_caps.cpp"
            "$ROOT/Tests/Core/Protocol/Sync/test_sync_catalog.cpp"
            "$ROOT/Tests/Core/Protocol/Sync/test_sync_checkpoint.cpp"
            "$ROOT/Tests/Core/Protocol/Sync/test_sync_checkpoint_ledger.cpp"
            "$ROOT/Tests/Core/Protocol/Sync/test_sync_checkpoint_storage_admission.cpp"
            "$ROOT/Tests/Core/Protocol/Sync/test_sync_client_attachment.cpp"
            "$ROOT/Tests/Core/Protocol/Sync/test_sync_clipboard_delivery.cpp"
            "$ROOT/Tests/Core/Protocol/Sync/test_sync_compression.cpp"
            "$ROOT/Tests/Core/Protocol/Sync/test_sync_connection.cpp"
            "$ROOT/Tests/Core/Protocol/Sync/test_sync_control.cpp"
            "$ROOT/Tests/Core/Protocol/Sync/test_sync_corpus.cpp"
            "$ROOT/Tests/Core/Protocol/Sync/test_sync_divergence.cpp"
            "$ROOT/Tests/Core/Protocol/Sync/test_sync_echo_context.cpp"
            "$ROOT/Tests/Core/Protocol/Sync/test_sync_effect_codec.cpp"
            "$ROOT/Tests/Core/Protocol/Sync/test_sync_effect_delivery.cpp"
            "$ROOT/Tests/Core/Protocol/Sync/test_sync_effects.cpp"
            "$ROOT/Tests/Core/Protocol/Sync/test_sync_envelope.cpp"
            "$ROOT/Tests/Core/Protocol/Sync/test_sync_facts_palette.cpp"
            "$ROOT/Tests/Core/Protocol/Sync/test_sync_frame_ingress.cpp"
            "$ROOT/Tests/Core/Protocol/Sync/test_sync_geometry.cpp"
            "$ROOT/Tests/Core/Protocol/Sync/test_sync_handles.cpp"
            "$ROOT/Tests/Core/Protocol/Sync/test_sync_handover_body.cpp"
            "$ROOT/Tests/Core/Protocol/Sync/test_sync_handover_channel.cpp"
            "$ROOT/Tests/Core/Protocol/Sync/test_sync_handover_continuation.cpp"
            "$ROOT/Tests/Core/Protocol/Sync/test_sync_handover_ground.cpp"
            "$ROOT/Tests/Core/Protocol/Sync/test_sync_handover_ledger.cpp"
            "$ROOT/Tests/Core/Protocol/Sync/test_sync_history.cpp"
            "$ROOT/Tests/Core/Protocol/Sync/test_sync_history_cache.cpp"
            "$ROOT/Tests/Core/Protocol/Sync/test_sync_history_plane.cpp"
            "$ROOT/Tests/Core/Protocol/Sync/test_sync_history_reset.cpp"
            "$ROOT/Tests/Core/Protocol/Sync/test_sync_history_runs.cpp"
            "$ROOT/Tests/Core/Protocol/Sync/test_sync_history_wants.cpp"
            "$ROOT/Tests/Core/Protocol/Sync/test_sync_history_wire.cpp"
            "$ROOT/Tests/Core/Protocol/Sync/test_sync_images.cpp"
            "$ROOT/Tests/Core/Protocol/Sync/test_sync_input.cpp"
            "$ROOT/Tests/Core/Protocol/Sync/test_sync_input_continuation.cpp"
            "$ROOT/Tests/Core/Protocol/Sync/test_sync_lag.cpp"
            "$ROOT/Tests/Core/Protocol/Sync/test_sync_lanes.cpp"
            "$ROOT/Tests/Core/Protocol/Sync/test_sync_listing.cpp"
            "$ROOT/Tests/Core/Protocol/Sync/test_sync_live_ingress.cpp"
            "$ROOT/Tests/Core/Protocol/Sync/test_sync_liveness.cpp"
            "$ROOT/Tests/Core/Protocol/Sync/test_sync_measured_policy.cpp"
            "$ROOT/Tests/Core/Protocol/Sync/test_sync_non_sizing_client.cpp"
            "$ROOT/Tests/Core/Protocol/Sync/test_sync_notify.cpp"
            "$ROOT/Tests/Core/Protocol/Sync/test_sync_owed.cpp"
            "$ROOT/Tests/Core/Protocol/Sync/test_sync_pacer.cpp"
            "$ROOT/Tests/Core/Protocol/Sync/test_sync_paste.cpp"
            "$ROOT/Tests/Core/Protocol/Sync/test_sync_pending_consent.cpp"
            "$ROOT/Tests/Core/Protocol/Sync/test_sync_probe.cpp"
            "$ROOT/Tests/Core/Protocol/Sync/test_sync_records.cpp"
            "$ROOT/Tests/Core/Protocol/Sync/test_sync_refusal.cpp"
            "$ROOT/Tests/Core/Protocol/Sync/test_sync_resume.cpp"
            "$ROOT/Tests/Core/Protocol/Sync/test_sync_resume_proof.cpp"
            "$ROOT/Tests/Core/Protocol/Sync/test_sync_resume_record.cpp"
            "$ROOT/Tests/Core/Protocol/Sync/test_sync_rich_text.cpp"
            "$ROOT/Tests/Core/Protocol/Sync/test_sync_roles.cpp"
            "$ROOT/Tests/Core/Protocol/Sync/test_sync_row_form.cpp"
            "$ROOT/Tests/Core/Protocol/Sync/test_sync_row_ops.cpp"
            "$ROOT/Tests/Core/Protocol/Sync/test_sync_sample_delivery.cpp"
            "$ROOT/Tests/Core/Protocol/Sync/test_sync_samples.cpp"
            "$ROOT/Tests/Core/Protocol/Sync/test_sync_shaped_link.cpp"
            "$ROOT/Tests/Core/Protocol/Sync/test_sync_signal.cpp"
            "$ROOT/Tests/Core/Protocol/Sync/test_sync_topology.cpp"
            "$ROOT/Tests/Core/Protocol/Sync/test_sync_view.cpp"
            "$ROOT/Tests/Core/Protocol/Sync/test_sync_wire_version.cpp"
            "$ROOT/Tests/Core/Session/test_*.cpp"
            "$ROOT/Tests/Core/Runtime/test_*.cpp"
        )
        if ! cmake -B "$SANITIZED_BUILD" -S "$ROOT" -G "$UBQ_GENERATOR" -DCMAKE_BUILD_TYPE=Debug \
                -DUBIQUITTY_FRONTEND=Core -DUBIQUITTY_BUILD_TESTS=ON -DUBIQUITTY_QUIC="$SANITIZED_QUIC" \
                -DUBIQUITTY_QUIC_PREFIX="$QUIC_PREFIX" -DUBIQUITTY_SANITIZERS=address,undefined >"$SANITIZED_LOG" 2>&1; then
            build_broken "sanitized_sync configure failed (see $SANITIZED_LOG)"
        fi
        SANITIZED_KNOWN="$(cmake --build "$SANITIZED_BUILD" --target help 2>/dev/null)"
        SANITIZED_TESTS=()
        SANITIZED_ABSENT=()
        for SANITIZED_GLOB in "${SANITIZED_GLOBS[@]}"; do
            while IFS= read -r SANITIZED_SOURCE; do
                [ -n "$SANITIZED_SOURCE" ] || continue
                SANITIZED_NAME="$(basename "$SANITIZED_SOURCE" .cpp)"
                case "$SANITIZED_KNOWN" in
                    *"$SANITIZED_NAME: phony"*|*"... $SANITIZED_NAME"*) SANITIZED_TESTS+=("$SANITIZED_NAME") ;;
                    *) SANITIZED_ABSENT+=("$SANITIZED_NAME") ;;
                esac
            done < <(compgen -G "$SANITIZED_GLOB")
        done
        if [ "${#SANITIZED_TESTS[@]}" -lt 12 ]; then
            echo "sanitized_sync: FAILED — ${#SANITIZED_TESTS[@]} test(s) named by the globs have a target in $SANITIZED_BUILD; the set is over a hundred" >&2
            exit 1
        fi
        [ "${#SANITIZED_ABSENT[@]}" -eq 0 ] || echo "sanitized_sync: ${#SANITIZED_ABSENT[@]} source(s) with no CMake target are not run: ${SANITIZED_ABSENT[*]}"
        if ! cmake --build "$SANITIZED_BUILD" -j"$BUILD_JOBS" --target "${SANITIZED_TESTS[@]}" >>"$SANITIZED_LOG" 2>&1; then
            build_broken "sanitized_sync build failed (see $SANITIZED_LOG)"
        fi
        ubq_require_sanitized address "$SANITIZED_BUILD/Tests/${SANITIZED_TESTS[0]}" sanitized_sync || exit 1
        SANITIZED_RED=()
        for SANITIZED_NAME in "${SANITIZED_TESTS[@]}"; do
            if ! UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 ubq_run_bounded 300 "$SANITIZED_BUILD/Tests/$SANITIZED_NAME" >"$OUT/sanitized-$SANITIZED_NAME.log" 2>&1; then
                SANITIZED_RED+=("$SANITIZED_NAME")
                grep -m1 -E 'ERROR: AddressSanitizer|runtime error:|FAILED|FATAL' "$OUT/sanitized-$SANITIZED_NAME.log" | sed "s/^/sanitized_sync: $SANITIZED_NAME: /" >&2
                echo "  (the whole run is in $OUT/sanitized-$SANITIZED_NAME.log)" >&2
            fi
        done
        if [ "${#SANITIZED_RED[@]}" -ne 0 ]; then
            echo "sanitized_sync: FAILED — ${#SANITIZED_RED[@]} of ${#SANITIZED_TESTS[@]} test(s) under the sanitizers: ${SANITIZED_RED[*]}" >&2
            exit 1
        fi
        echo "sanitized_sync: ${#SANITIZED_TESTS[@]} test(s) ran clean under AddressSanitizer and UndefinedBehaviorSanitizer"
    fi
fi

# remote_session_quic_resume — a remote session redials its QUIC carrier by
# a resume proof: a RemoteSessionCore attaches to a spawned host through a
# factory whose bootstrap mints a token, a long suspension replaces the
# carrier with one that presents a resume key and runs no bootstrap, the host
# continues the attachment with a diff, an announced suspension resumes the
# same way, and a forged proof falls back to the bootstrap within the core's
# deadline. Links the shared Core and the QUIC client carrier.
# UBIQUITTY_QUIC changes SHA256Hasher's layout; the portable archive is built without it.
if wanted remote_session_quic_resume && quic_deps_present && command -v cmake >/dev/null 2>&1; then
    echo "=== remote_session_quic_resume ==="
    RAN=$((RAN + 1))
    QUIC_FRAMEWORKS=""
    [ "$(uname)" = "Darwin" ] && QUIC_FRAMEWORKS="-framework CoreFoundation -framework Security"
    HOSTQ_BUILD="$UBQ_BUILD_ROOT/host-quic"
    HOSTQ_OBJDIR="$HOSTQ_BUILD/CMakeFiles/ubiquitty-host.dir"
    if cmake -B "$HOSTQ_BUILD" -S "$ROOT" -G "$UBQ_GENERATOR" -DUBIQUITTY_FRONTEND=Host -DCMAKE_BUILD_TYPE=Debug \
                -DUBIQUITTY_QUIC=ON >"$OUT/remote-session-quic-resume-host-build.log" 2>&1 \
            && cmake --build "$HOSTQ_BUILD" -j"$BUILD_JOBS" >>"$OUT/remote-session-quic-resume-host-build.log" 2>&1; then
        host_objects "$HOSTQ_OBJDIR" "$OUT/remote-session-quic-resume-objs.txt" \
            || build_broken "host object list is empty (the harness, not the test under it)"
        if ubq_compile_parallel "$OUT/remote_session_quic_resume" "$OUT/objects/remote_session_quic_resume" "$OUT/remote-session-quic-resume-build.log" \
                -std=c++23 -I"$ROOT/Tests" -DUBIQUITTY_QUIC=1 -pthread -I"$ROOT/Source" -I"$QUIC_PREFIX/include" -- \
                "$ROOT/Tests/Host/remote_session_quic_resume.cpp" \
                "$ROOT/Source/Platform/POSIX/QUICClient.cpp" \
                "$ROOT/Source/Core/Transport/QUICChannel.cpp" \
                -- "@$OUT/remote-session-quic-resume-objs.txt" "$QUIC_PREFIX/lib/libngtcp2_crypto_boringssl.a" "$QUIC_PREFIX/lib/libngtcp2.a" "$QUIC_PREFIX/lib/libssl.a" "$QUIC_PREFIX/lib/libcrypto.a" "${HOST_LINK_ARCHIVES[@]}" $QUIC_FRAMEWORKS; then
            ubq_run_bounded 300 "$OUT/remote_session_quic_resume" "$HOSTQ_BUILD/ubiquitty-host" || exit 1
        else
            build_broken "remote_session_quic_resume build failed (see $OUT/remote-session-quic-resume-build.log)"
        fi
    else
        build_broken "remote_session_quic_resume host build failed (see $OUT/remote-session-quic-resume-host-build.log)"
    fi
fi

# workspace_reference_restore — a client that saved a reference to a named
# workspace (daemon instance, workspace id, pane session) gets it back after
# the host restarts: the first daemon is killed outright, the next lists the
# workspace under the id it was recorded with, and a reopened window — a
# WorkspaceConnection pinned to the saved instance, as TerminalSession reopens
# one on macOS and iOS — is answered with the workspace recreated from its
# record, its tabs, split, title and directories. A reference naming another
# instance is refused and starts nothing. Runs over the `--attach` bridge an
# ssh connection runs and over QUIC. Links the daemon objects, which carry the
# client stack, and QUICClient.cpp.
# UBIQUITTY_QUIC changes SHA256Hasher's layout; the portable archive is built without it.
if wanted workspace_reference_restore && quic_deps_present && command -v cmake >/dev/null 2>&1; then
    echo "=== workspace_reference_restore ==="
    RAN=$((RAN + 1))
    QUIC_FRAMEWORKS=""
    [ "$(uname)" = "Darwin" ] && QUIC_FRAMEWORKS="-framework CoreFoundation -framework Security"
    HOSTQ_BUILD="$UBQ_BUILD_ROOT/host-quic"
    HOSTQ_OBJDIR="$HOSTQ_BUILD/CMakeFiles/ubiquitty-host.dir"
    if cmake -B "$HOSTQ_BUILD" -S "$ROOT" -G "$UBQ_GENERATOR" -DUBIQUITTY_FRONTEND=Host -DCMAKE_BUILD_TYPE=Debug \
                -DUBIQUITTY_QUIC=ON >"$OUT/workspace-reference-restore-host-build.log" 2>&1 \
            && cmake --build "$HOSTQ_BUILD" -j"$BUILD_JOBS" >>"$OUT/workspace-reference-restore-host-build.log" 2>&1; then
        host_objects "$HOSTQ_OBJDIR" "$OUT/workspace-reference-restore-objs.txt" \
            || build_broken "host object list is empty (the harness, not the test under it)"
        if ubq_compile_parallel "$OUT/workspace_reference_restore" "$OUT/objects/workspace_reference_restore" "$OUT/workspace-reference-restore-build.log" \
                -std=c++23 -I"$ROOT/Tests" -DUBIQUITTY_QUIC=1 -pthread -I"$ROOT/Source" -I"$QUIC_PREFIX/include" -- \
                "$ROOT/Tests/Host/workspace_reference_restore.cpp" \
                "$ROOT/Source/Platform/POSIX/QUICClient.cpp" \
                "$ROOT/Source/Core/Transport/QUICChannel.cpp" \
                -- "@$OUT/workspace-reference-restore-objs.txt" "$QUIC_PREFIX/lib/libngtcp2_crypto_boringssl.a" "$QUIC_PREFIX/lib/libngtcp2.a" "$QUIC_PREFIX/lib/libssl.a" "$QUIC_PREFIX/lib/libcrypto.a" "${HOST_LINK_ARCHIVES[@]}" $QUIC_FRAMEWORKS; then
            ubq_run_bounded 300 "$OUT/workspace_reference_restore" "$HOSTQ_BUILD/ubiquitty-host" || exit 1
        else
            build_broken "workspace_reference_restore build failed (see $OUT/workspace-reference-restore-build.log)"
        fi
    else
        build_broken "workspace_reference_restore host build failed (see $OUT/workspace-reference-restore-host-build.log)"
    fi
fi

# vanished_instance_reresolve — a window pinned to a daemon that stopped
# without handing over gets its workspace back from the host's current daemon.
# The first daemon runs on a socket of its own and is stopped by SIGTERM, which
# keeps its record and removes its socket and PID file; the current daemon
# takes the record in. A window left open across the stop keeps its pane, which
# is the restored workspace's pane under its new session from then on. The
# reopened window's real pinned bootstrap is told the daemon is gone, drops the
# pin and is answered with the restored workspace; a reference with no record
# fails plainly and is not dialled again. Also checks
# the host's `--probe-daemon` and pinned `--quic-bootstrap` on a stale socket.
# Links the daemon objects, as workspace_reference_restore does.
if wanted vanished_instance_reresolve && quic_deps_present && command -v cmake >/dev/null 2>&1; then
    echo "=== vanished_instance_reresolve ==="
    RAN=$((RAN + 1))
    QUIC_FRAMEWORKS=""
    [ "$(uname)" = "Darwin" ] && QUIC_FRAMEWORKS="-framework CoreFoundation -framework Security"
    HOSTQ_BUILD="$UBQ_BUILD_ROOT/host-quic"
    HOSTQ_OBJDIR="$HOSTQ_BUILD/CMakeFiles/ubiquitty-host.dir"
    if cmake -B "$HOSTQ_BUILD" -S "$ROOT" -G "$UBQ_GENERATOR" -DUBIQUITTY_FRONTEND=Host -DCMAKE_BUILD_TYPE=Debug \
                -DUBIQUITTY_QUIC=ON >"$OUT/vanished-instance-reresolve-host-build.log" 2>&1 \
            && cmake --build "$HOSTQ_BUILD" -j"$BUILD_JOBS" >>"$OUT/vanished-instance-reresolve-host-build.log" 2>&1; then
        host_objects "$HOSTQ_OBJDIR" "$OUT/vanished-instance-reresolve-objs.txt" \
            || build_broken "host object list is empty (the harness, not the test under it)"
        if ubq_compile_parallel "$OUT/vanished_instance_reresolve" "$OUT/objects/vanished_instance_reresolve" "$OUT/vanished-instance-reresolve-build.log" \
                -std=c++23 -I"$ROOT/Tests" -DUBIQUITTY_QUIC=1 -pthread -I"$ROOT/Source" -I"$QUIC_PREFIX/include" -- \
                "$ROOT/Tests/Host/vanished_instance_reresolve.cpp" \
                "$ROOT/Source/Platform/POSIX/QUICClient.cpp" \
                "$ROOT/Source/Core/Transport/QUICChannel.cpp" \
                -- "@$OUT/vanished-instance-reresolve-objs.txt" "$QUIC_PREFIX/lib/libngtcp2_crypto_boringssl.a" "$QUIC_PREFIX/lib/libngtcp2.a" "$QUIC_PREFIX/lib/libssl.a" "$QUIC_PREFIX/lib/libcrypto.a" "${HOST_LINK_ARCHIVES[@]}" $QUIC_FRAMEWORKS; then
            ubq_run_bounded 300 "$OUT/vanished_instance_reresolve" "$HOSTQ_BUILD/ubiquitty-host" || exit 1
        else
            build_broken "vanished_instance_reresolve build failed (see $OUT/vanished-instance-reresolve-build.log)"
        fi
    else
        build_broken "vanished_instance_reresolve host build failed (see $OUT/vanished-instance-reresolve-host-build.log)"
    fi
fi

# restore_renumbered_workspace — a window's saved reference to a workspace on a
# daemon that was killed is answered by the host's current daemon, which took
# the record in under another id because both daemons numbered their first
# workspace 1; and a daemon killed after the current one started has its
# record taken in when an open window asks, the window's pane staying open on
# the restored workspace. Links the daemon objects, as
# workspace_reference_restore does.
if wanted restore_renumbered_workspace && quic_deps_present && command -v cmake >/dev/null 2>&1; then
    echo "=== restore_renumbered_workspace ==="
    RAN=$((RAN + 1))
    QUIC_FRAMEWORKS=""
    [ "$(uname)" = "Darwin" ] && QUIC_FRAMEWORKS="-framework CoreFoundation -framework Security"
    HOSTQ_BUILD="$UBQ_BUILD_ROOT/host-quic"
    HOSTQ_OBJDIR="$HOSTQ_BUILD/CMakeFiles/ubiquitty-host.dir"
    if cmake -B "$HOSTQ_BUILD" -S "$ROOT" -G "$UBQ_GENERATOR" -DUBIQUITTY_FRONTEND=Host -DCMAKE_BUILD_TYPE=Debug \
                -DUBIQUITTY_QUIC=ON >"$OUT/restore-renumbered-workspace-host-build.log" 2>&1 \
            && cmake --build "$HOSTQ_BUILD" -j"$BUILD_JOBS" >>"$OUT/restore-renumbered-workspace-host-build.log" 2>&1; then
        host_objects "$HOSTQ_OBJDIR" "$OUT/restore-renumbered-workspace-objs.txt" \
            || build_broken "host object list is empty (the harness, not the test under it)"
        if ubq_compile_parallel "$OUT/restore_renumbered_workspace" "$OUT/objects/restore_renumbered_workspace" "$OUT/restore-renumbered-workspace-build.log" \
                -std=c++23 -I"$ROOT/Tests" -DUBIQUITTY_QUIC=1 -pthread -I"$ROOT/Source" -I"$QUIC_PREFIX/include" -- \
                "$ROOT/Tests/Host/restore_renumbered_workspace.cpp" \
                "$ROOT/Source/Platform/POSIX/QUICClient.cpp" \
                "$ROOT/Source/Core/Transport/QUICChannel.cpp" \
                -- "@$OUT/restore-renumbered-workspace-objs.txt" "$QUIC_PREFIX/lib/libngtcp2_crypto_boringssl.a" "$QUIC_PREFIX/lib/libngtcp2.a" "$QUIC_PREFIX/lib/libssl.a" "$QUIC_PREFIX/lib/libcrypto.a" "${HOST_LINK_ARCHIVES[@]}" $QUIC_FRAMEWORKS; then
            ubq_run_bounded 300 "$OUT/restore_renumbered_workspace" "$HOSTQ_BUILD/ubiquitty-host" || exit 1
        else
            build_broken "restore_renumbered_workspace build failed (see $OUT/restore-renumbered-workspace-build.log)"
        fi
    else
        build_broken "restore_renumbered_workspace host build failed (see $OUT/restore-renumbered-workspace-host-build.log)"
    fi
fi

# quic_retry_validation — security negative for pre-auth QUIC retry-token validation: a
# Initial flood can't allocate connection state (stateless address validation,
# RFC 9000 §8.1, before any ngtcp2/TLS allocation), and a real client still
# completes its handshake despite the flood. Drives the REAL QUICListener
# in-process against 100 real (started-then-immediately-stopped) QUICClient
# instances over real loopback UDP, then one real completing QUICClient;
# asserts testQUICConnectionCount stays 0 through the flood and settles at
# exactly 1. Links the daemon objects (QUICListener) + QUICClient.cpp directly.
if wanted quic_retry_validation && quic_deps_present && command -v cmake >/dev/null 2>&1; then
    echo "=== quic_retry_validation ==="
    RAN=$((RAN + 1))
    QUIC_FRAMEWORKS=""
    [ "$(uname)" = "Darwin" ] && QUIC_FRAMEWORKS="-framework CoreFoundation -framework Security"
    HOSTQ_BUILD="$UBQ_BUILD_ROOT/host-quic"
    HOSTQ_OBJDIR="$HOSTQ_BUILD/CMakeFiles/ubiquitty-host.dir"
    if cmake -B "$HOSTQ_BUILD" -S "$ROOT" -G "$UBQ_GENERATOR" -DUBIQUITTY_FRONTEND=Host -DCMAKE_BUILD_TYPE=Debug \
                -DUBIQUITTY_QUIC=ON >"$OUT/quic-retry-host-build.log" 2>&1 \
            && cmake --build "$HOSTQ_BUILD" -j"$BUILD_JOBS" >>"$OUT/quic-retry-host-build.log" 2>&1; then
        host_objects "$HOSTQ_OBJDIR" "$OUT/quic-retry-objs.txt" \
            || build_broken "host object list is empty (the harness, not the test under it)"
        # DatagramBatch is not named here: it arrives with the host objects,
        # because the write pump sends through it and the host target compiles it.
        if ubq_compile_parallel "$OUT/quic_retry_validation" "$OUT/objects/quic_retry_validation" "$OUT/quic-retry-build.log" \
                -std=c++23 -I"$ROOT/Tests" -DUBIQUITTY_QUIC=1 -pthread -I"$ROOT/Source" -I"$QUIC_PREFIX/include" -- \
                "$ROOT/Tests/Host/quic_retry_validation.cpp" \
                "$ROOT/Source/Platform/POSIX/QUICClient.cpp" \
                -- "@$OUT/quic-retry-objs.txt" "$QUIC_PREFIX/lib/libngtcp2_crypto_boringssl.a" "$QUIC_PREFIX/lib/libngtcp2.a" "$QUIC_PREFIX/lib/libssl.a" "$QUIC_PREFIX/lib/libcrypto.a" $QUIC_FRAMEWORKS; then
            "$OUT/quic_retry_validation" || exit 1
        else
            build_broken "quic_retry_validation link failed (see $OUT/quic-retry-build.log)"
        fi
    else
        build_broken "quic_retry_validation host build failed (see $OUT/quic-retry-host-build.log)"
    fi
fi

# quic_client_datagram_upward — the production QUICClient sends a datagram the
# daemon's listener receives: the path a screen plane's Echo takes when the
# host probes by datagram (Docs/sync.md §6.4). Drives the REAL QUICListener
# in-process against the real QUICClient on loopback, linked as
# quic_retry_validation is.
if wanted quic_client_datagram_upward && quic_deps_present && command -v cmake >/dev/null 2>&1; then
    echo "=== quic_client_datagram_upward ==="
    RAN=$((RAN + 1))
    QUIC_FRAMEWORKS=""
    [ "$(uname)" = "Darwin" ] && QUIC_FRAMEWORKS="-framework CoreFoundation -framework Security"
    HOSTQ_BUILD="$UBQ_BUILD_ROOT/host-quic"
    HOSTQ_OBJDIR="$HOSTQ_BUILD/CMakeFiles/ubiquitty-host.dir"
    if cmake -B "$HOSTQ_BUILD" -S "$ROOT" -G "$UBQ_GENERATOR" -DUBIQUITTY_FRONTEND=Host -DCMAKE_BUILD_TYPE=Debug \
                -DUBIQUITTY_QUIC=ON >"$OUT/quic-upward-host-build.log" 2>&1 \
            && cmake --build "$HOSTQ_BUILD" -j"$BUILD_JOBS" >>"$OUT/quic-upward-host-build.log" 2>&1; then
        host_objects "$HOSTQ_OBJDIR" "$OUT/quic-upward-objs.txt" \
            || build_broken "host object list is empty (the harness, not the test under it)"
        if ubq_compile_parallel "$OUT/quic_client_datagram_upward" "$OUT/objects/quic_client_datagram_upward" "$OUT/quic-upward-build.log" \
                -std=c++23 -I"$ROOT/Tests" -DUBIQUITTY_QUIC=1 -pthread -I"$ROOT/Source" -I"$QUIC_PREFIX/include" -- \
                "$ROOT/Tests/Host/quic_client_datagram_upward.cpp" \
                "$ROOT/Source/Platform/POSIX/QUICClient.cpp" \
                -- "@$OUT/quic-upward-objs.txt" "$QUIC_PREFIX/lib/libngtcp2_crypto_boringssl.a" "$QUIC_PREFIX/lib/libngtcp2.a" "$QUIC_PREFIX/lib/libssl.a" "$QUIC_PREFIX/lib/libcrypto.a" $QUIC_FRAMEWORKS; then
            "$OUT/quic_client_datagram_upward" || exit 1
        else
            build_broken "quic_client_datagram_upward link failed (see $OUT/quic-upward-build.log)"
        fi
    else
        build_broken "quic_client_datagram_upward host build failed (see $OUT/quic-upward-host-build.log)"
    fi
fi

# quic_echo_one_packet — a datagram and the stream bytes queued with it leave
# the listener in one packet, so a keystroke echo's superset update and the
# frames beside it are one packet (Docs/sync.md §6.5). Drives the REAL
# QUICListener in-process against the real QUICClient on loopback, linked as
# quic_retry_validation is.
if wanted quic_echo_one_packet && quic_deps_present && command -v cmake >/dev/null 2>&1; then
    echo "=== quic_echo_one_packet ==="
    RAN=$((RAN + 1))
    QUIC_FRAMEWORKS=""
    [ "$(uname)" = "Darwin" ] && QUIC_FRAMEWORKS="-framework CoreFoundation -framework Security"
    HOSTQ_BUILD="$UBQ_BUILD_ROOT/host-quic"
    HOSTQ_OBJDIR="$HOSTQ_BUILD/CMakeFiles/ubiquitty-host.dir"
    if cmake -B "$HOSTQ_BUILD" -S "$ROOT" -G "$UBQ_GENERATOR" -DUBIQUITTY_FRONTEND=Host -DCMAKE_BUILD_TYPE=Debug \
                -DUBIQUITTY_QUIC=ON >"$OUT/quic-echo-packet-host-build.log" 2>&1 \
            && cmake --build "$HOSTQ_BUILD" -j"$BUILD_JOBS" >>"$OUT/quic-echo-packet-host-build.log" 2>&1; then
        host_objects "$HOSTQ_OBJDIR" "$OUT/quic-echo-packet-objs.txt" \
            || build_broken "host object list is empty (the harness, not the test under it)"
        if ubq_compile_parallel "$OUT/quic_echo_one_packet" "$OUT/objects/quic_echo_one_packet" "$OUT/quic-echo-packet-build.log" \
                -std=c++23 -I"$ROOT/Tests" -DUBIQUITTY_QUIC=1 -pthread -I"$ROOT/Source" -I"$QUIC_PREFIX/include" -- \
                "$ROOT/Tests/Host/quic_echo_one_packet.cpp" \
                "$ROOT/Source/Platform/POSIX/QUICClient.cpp" \
                -- "@$OUT/quic-echo-packet-objs.txt" "$QUIC_PREFIX/lib/libngtcp2_crypto_boringssl.a" "$QUIC_PREFIX/lib/libngtcp2.a" "$QUIC_PREFIX/lib/libssl.a" "$QUIC_PREFIX/lib/libcrypto.a" $QUIC_FRAMEWORKS; then
            "$OUT/quic_echo_one_packet" || exit 1
        else
            build_broken "quic_echo_one_packet link failed (see $OUT/quic-echo-packet-build.log)"
        fi
    else
        build_broken "quic_echo_one_packet host build failed (see $OUT/quic-echo-packet-host-build.log)"
    fi
fi

# quic_pin_gate — SSL_VERIFY_NONE means the SHA-256 cert pin is the only daemon
# authentication, so the pin is checked synchronously inside
# handshakeCompletedHandler (before any 1-RTT receive handler can fire in the
# same read_pkt call — a QUIC protocol guarantee) and onData/onDatagram are
# gated on it. Receive handlers that deliver to the Sync decoder before a pin
# check runs let an active MITM completing the handshake with its own cert +
# coalescing a 1-RTT frame with the Finished get a spoofed Sync frame decoded.
# Drives the REAL (file-scope)
# ngtcp2 callbacks directly via a friend seam — no live network needed.
if wanted quic_pin_gate && quic_deps_present; then
    echo "=== quic_pin_gate ==="
    RAN=$((RAN + 1))
    QUIC_FRAMEWORKS=""
    [ "$(uname)" = "Darwin" ] && QUIC_FRAMEWORKS="-framework CoreFoundation -framework Security"
    if ubq_compile_parallel "$OUT/quic_pin_gate" "$OUT/objects/quic_pin_gate" "$OUT/quic-pingate-build.log" \
            -std=c++23 -I"$ROOT/Tests" -DUBIQUITTY_QUIC=1 -DUBIQUITTY_ENABLE_TEST_HOOKS=1 -pthread -I"$ROOT/Source" -I"$QUIC_PREFIX/include" -- \
            "$ROOT/Tests/Host/quic_pin_gate.cpp" \
            "$ROOT/Source/Platform/POSIX/QUICClient.cpp" \
            "$ROOT/Source/Platform/POSIX/DatagramBatch.cpp" \
            -- "$QUIC_PREFIX/lib/libngtcp2_crypto_boringssl.a" "$QUIC_PREFIX/lib/libngtcp2.a" "$QUIC_PREFIX/lib/libssl.a" "$QUIC_PREFIX/lib/libcrypto.a" $QUIC_FRAMEWORKS; then
        "$OUT/quic_pin_gate" || exit 1
    else
        build_broken "quic_pin_gate build failed (see $OUT/quic-pingate-build.log)"
    fi
fi

# quic_outbound_capacity — the client's outbound send() queue is bounded at
# maxOutboundBytes (4 MiB) and fails the connection closed on overflow, so a
# stalled/malicious daemon (receive window never opens) can't OOM the frontend
# Drives the REAL send() past the limit and checks the overflow-safe
# (INT30-C) predicate against near-SIZE_MAX lengths. Compiles QUICClient.cpp
# directly (ngtcp2 + AWS-LC); no daemon needed.
if wanted quic_outbound_capacity && quic_deps_present; then
    echo "=== quic_outbound_capacity ==="
    RAN=$((RAN + 1))
    QUIC_FRAMEWORKS=""
    [ "$(uname)" = "Darwin" ] && QUIC_FRAMEWORKS="-framework CoreFoundation -framework Security"
    if ubq_compile_parallel "$OUT/quic_outbound_capacity" "$OUT/objects/quic_outbound_capacity" "$OUT/quic-outbound-capacity-build.log" \
            -std=c++23 -I"$ROOT/Tests" -DUBIQUITTY_QUIC=1 -DUBIQUITTY_ENABLE_TEST_HOOKS=1 -pthread -I"$ROOT/Source" -I"$QUIC_PREFIX/include" -- \
            "$ROOT/Tests/Host/quic_outbound_capacity.cpp" \
            "$ROOT/Source/Platform/POSIX/QUICClient.cpp" \
            "$ROOT/Source/Platform/POSIX/DatagramBatch.cpp" \
            -- "$QUIC_PREFIX/lib/libngtcp2_crypto_boringssl.a" "$QUIC_PREFIX/lib/libngtcp2.a" "$QUIC_PREFIX/lib/libssl.a" "$QUIC_PREFIX/lib/libcrypto.a" $QUIC_FRAMEWORKS; then
        "$OUT/quic_outbound_capacity" || exit 1
    else
        build_broken "quic_outbound_capacity build failed (see $OUT/quic-outbound-capacity-build.log)"
    fi
fi

# quic_token_uid_local — only a uid-LOCAL client mints a QUIC bootstrap token; a
# QUIC NETWORK peer (quicConnection set) mints nothing. The mint is
# #ifdef UBIQUITTY_QUIC, so this links the QUIC-ON daemon objects (the build above)
# and a real (handshake-less) QUICListener + QUICConnection via a friend seam to
# drive local-control token admission for a local vs a network client, asserting the
# token is minted for the former and not for the latter.
if wanted quic_token_uid_local && quic_deps_present && command -v cmake >/dev/null 2>&1; then
    echo "=== quic_token_uid_local ==="
    RAN=$((RAN + 1))
    QUIC_FRAMEWORKS=""
    [ "$(uname)" = "Darwin" ] && QUIC_FRAMEWORKS="-framework CoreFoundation -framework Security"
    HOSTQ_BUILD="$UBQ_BUILD_ROOT/host-quic"
    HOSTQ_OBJDIR="$HOSTQ_BUILD/CMakeFiles/ubiquitty-host.dir"
    if cmake -B "$HOSTQ_BUILD" -S "$ROOT" -G "$UBQ_GENERATOR" -DUBIQUITTY_FRONTEND=Host -DCMAKE_BUILD_TYPE=Debug \
                -DUBIQUITTY_QUIC=ON >"$OUT/quic-uidlocal-host-build.log" 2>&1 \
            && cmake --build "$HOSTQ_BUILD" -j"$BUILD_JOBS" >>"$OUT/quic-uidlocal-host-build.log" 2>&1; then
        # Link against every daemon object except the binary's own main().
        host_objects "$HOSTQ_OBJDIR" "$OUT/quic-uidlocal-objs.txt" \
            || build_broken "host object list is empty (the harness, not the test under it)"
        if ubq_compile_parallel "$OUT/quic_token_uid_local" "$OUT/objects/quic_token_uid_local" "$OUT/quic-uidlocal-build.log" \
                -std=c++23 -I"$ROOT/Tests" -DUBIQUITTY_QUIC=1 -I"$ROOT/Source" -I"$QUIC_PREFIX/include" -- \
                "$ROOT/Tests/Host/quic_token_uid_local.cpp" \
                -- "@$OUT/quic-uidlocal-objs.txt" "$QUIC_PREFIX/lib/libngtcp2_crypto_boringssl.a" "$QUIC_PREFIX/lib/libngtcp2.a" "$QUIC_PREFIX/lib/libssl.a" "$QUIC_PREFIX/lib/libcrypto.a" $QUIC_FRAMEWORKS; then
            "$OUT/quic_token_uid_local" || exit 1
        else
            build_broken "quic_token_uid_local link failed (see $OUT/quic-uidlocal-build.log)"
        fi
    else
        build_broken "quic_token_uid_local host build failed (see $OUT/quic-uidlocal-host-build.log)"
    fi
fi

# quic_output_capacity — a stalled/desynced QUIC reader's queued-but-unsent bytes are
# bounded by the per-client output high-water cap. queueToClient counts the
# connection's pendingBytes toward the cap, and pendingBytes SATURATES a desync
# (toSendConsumed > toSend.size()) to 0 so a corrupted accounting can't underflow
# and wrap the cap below its limit (an unbounded-memory DoS). The count is #ifdef
# UBIQUITTY_QUIC, so this links the QUIC-ON daemon objects (the build above) and a
# real (handshake-less) QUICConnection via a friend seam to drive the private
# queueToClient and assert c.overflowed for (a) a near-cap stall and (b) a desync.
if wanted quic_output_capacity && quic_deps_present && command -v cmake >/dev/null 2>&1; then
    echo "=== quic_output_capacity ==="
    RAN=$((RAN + 1))
    QUIC_FRAMEWORKS=""
    [ "$(uname)" = "Darwin" ] && QUIC_FRAMEWORKS="-framework CoreFoundation -framework Security"
    HOSTQ_BUILD="$UBQ_BUILD_ROOT/host-quic"
    HOSTQ_OBJDIR="$HOSTQ_BUILD/CMakeFiles/ubiquitty-host.dir"
    if cmake -B "$HOSTQ_BUILD" -S "$ROOT" -G "$UBQ_GENERATOR" -DUBIQUITTY_FRONTEND=Host -DCMAKE_BUILD_TYPE=Debug \
                -DUBIQUITTY_QUIC=ON >"$OUT/quic-outputcap-host-build.log" 2>&1 \
            && cmake --build "$HOSTQ_BUILD" -j"$BUILD_JOBS" >>"$OUT/quic-outputcap-host-build.log" 2>&1; then
        # Link against every daemon object except the binary's own main().
        host_objects "$HOSTQ_OBJDIR" "$OUT/quic-outputcap-objs.txt" \
            || build_broken "host object list is empty (the harness, not the test under it)"
        if ubq_compile_parallel "$OUT/quic_output_capacity" "$OUT/objects/quic_output_capacity" "$OUT/quic-outputcap-build.log" \
                -std=c++23 -I"$ROOT/Tests" -DUBIQUITTY_QUIC=1 -I"$ROOT/Source" -I"$QUIC_PREFIX/include" -- \
                "$ROOT/Tests/Host/quic_output_capacity.cpp" \
                -- "@$OUT/quic-outputcap-objs.txt" "$QUIC_PREFIX/lib/libngtcp2_crypto_boringssl.a" "$QUIC_PREFIX/lib/libngtcp2.a" "$QUIC_PREFIX/lib/libssl.a" "$QUIC_PREFIX/lib/libcrypto.a" $QUIC_FRAMEWORKS; then
            "$OUT/quic_output_capacity" || exit 1
        else
            build_broken "quic_output_capacity link failed (see $OUT/quic-outputcap-build.log)"
        fi
    else
        build_broken "quic_output_capacity host build failed (see $OUT/quic-outputcap-host-build.log)"
    fi
fi

# quic_ack_release — ngtcp2 does not copy stream data: writev_stream's contract is
# that the caller keeps the written bytes intact until acked_stream_data_offset
# reports them acknowledged, because a lost packet is rebuilt from that buffer.
# Released on the WRITE, a retransmission re-reads bytes the next append has
# overwritten. A retransmission needs loss and the loopback harness has none,
# so this drives the write and the acknowledgement directly and asserts what the
# buffer retains between them.
if wanted quic_ack_release && quic_deps_present && command -v cmake >/dev/null 2>&1; then
    echo "=== quic_ack_release ==="
    RAN=$((RAN + 1))
    QUIC_FRAMEWORKS=""
    [ "$(uname)" = "Darwin" ] && QUIC_FRAMEWORKS="-framework CoreFoundation -framework Security"
    HOSTQ_BUILD="$UBQ_BUILD_ROOT/host-quic"
    HOSTQ_OBJDIR="$HOSTQ_BUILD/CMakeFiles/ubiquitty-host.dir"
    if cmake -B "$HOSTQ_BUILD" -S "$ROOT" -G "$UBQ_GENERATOR" -DUBIQUITTY_FRONTEND=Host -DCMAKE_BUILD_TYPE=Debug \
                -DUBIQUITTY_QUIC=ON >"$OUT/quic-ackrelease-host-build.log" 2>&1 \
            && cmake --build "$HOSTQ_BUILD" -j"$BUILD_JOBS" >>"$OUT/quic-ackrelease-host-build.log" 2>&1; then
        host_objects "$HOSTQ_OBJDIR" "$OUT/quic-ackrelease-objs.txt" \
            || build_broken "host object list is empty (the harness, not the test under it)"
        if ubq_compile_parallel "$OUT/quic_ack_release" "$OUT/objects/quic_ack_release" "$OUT/quic-ackrelease-build.log" \
                -std=c++23 -I"$ROOT/Tests" -DUBIQUITTY_QUIC=1 -I"$ROOT/Source" -I"$QUIC_PREFIX/include" -- \
                "$ROOT/Tests/Host/quic_ack_release.cpp" \
                -- "@$OUT/quic-ackrelease-objs.txt" "$QUIC_PREFIX/lib/libngtcp2_crypto_boringssl.a" "$QUIC_PREFIX/lib/libngtcp2.a" "$QUIC_PREFIX/lib/libssl.a" "$QUIC_PREFIX/lib/libcrypto.a" $QUIC_FRAMEWORKS; then
            "$OUT/quic_ack_release" || exit 1
        else
            build_broken "quic_ack_release link failed (see $OUT/quic-ackrelease-build.log)"
        fi
    else
        build_broken "quic_ack_release host build failed (see $OUT/quic-ackrelease-host-build.log)"
    fi
fi

# quic_flow_credit — a QUIC receiver grants an initial flow-control window and
# must extend it as it consumes, or every transfer wedges at the window edge
# with no error anywhere: the sender queues, the receiver waits, and both are
# healthy. Drives the production client against the production listener over
# loopback and pushes half again the connection window in each direction; the
# assertion is bytes delivered under a deadline a wedged transfer cannot meet.
if wanted quic_flow_credit && quic_deps_present && command -v cmake >/dev/null 2>&1; then
    echo "=== quic_flow_credit ==="
    RAN=$((RAN + 1))
    QUIC_FRAMEWORKS=""
    [ "$(uname)" = "Darwin" ] && QUIC_FRAMEWORKS="-framework CoreFoundation -framework Security"
    HOSTQ_BUILD="$UBQ_BUILD_ROOT/host-quic"
    HOSTQ_OBJDIR="$HOSTQ_BUILD/CMakeFiles/ubiquitty-host.dir"
    if cmake -B "$HOSTQ_BUILD" -S "$ROOT" -G "$UBQ_GENERATOR" -DUBIQUITTY_FRONTEND=Host -DCMAKE_BUILD_TYPE=Debug \
                -DUBIQUITTY_QUIC=ON >"$OUT/quic-flowcredit-host-build.log" 2>&1 \
            && cmake --build "$HOSTQ_BUILD" -j"$BUILD_JOBS" >>"$OUT/quic-flowcredit-host-build.log" 2>&1; then
        host_objects "$HOSTQ_OBJDIR" "$OUT/quic-flowcredit-objs.txt" \
            || build_broken "host object list is empty (the harness, not the test under it)"
        if ubq_compile_parallel "$OUT/quic_flow_credit" "$OUT/objects/quic_flow_credit" "$OUT/quic-flowcredit-build.log" \
                -std=c++23 -I"$ROOT/Tests" -DUBIQUITTY_QUIC=1 -I"$ROOT/Source" -I"$QUIC_PREFIX/include" -- \
                "$ROOT/Tests/Host/quic_flow_credit.cpp" \
                "$ROOT/Source/Platform/POSIX/QUICClient.cpp" \
                -- "@$OUT/quic-flowcredit-objs.txt" "$QUIC_PREFIX/lib/libngtcp2_crypto_boringssl.a" "$QUIC_PREFIX/lib/libngtcp2.a" "$QUIC_PREFIX/lib/libssl.a" "$QUIC_PREFIX/lib/libcrypto.a" $QUIC_FRAMEWORKS; then
            "$OUT/quic_flow_credit" || exit 1
        else
            build_broken "quic_flow_credit link failed (see $OUT/quic-flowcredit-build.log)"
        fi
    else
        build_broken "quic_flow_credit host build failed (see $OUT/quic-flowcredit-host-build.log)"
    fi
fi

# quic_lossy_transfer — ngtcp2 rebuilds a lost packet from the pointers it was
# given, so the sender's stream bytes must stay at their addresses until they are
# acknowledged. Drives the production client against the production listener
# through an in-process relay that drops and delays datagrams, both directions,
# and checks every delivered byte against its stream offset.
if wanted quic_lossy_transfer && quic_deps_present && command -v cmake >/dev/null 2>&1; then
    echo "=== quic_lossy_transfer ==="
    RAN=$((RAN + 1))
    QUIC_FRAMEWORKS=""
    [ "$(uname)" = "Darwin" ] && QUIC_FRAMEWORKS="-framework CoreFoundation -framework Security"
    HOSTQ_BUILD="$UBQ_BUILD_ROOT/host-quic"
    HOSTQ_OBJDIR="$HOSTQ_BUILD/CMakeFiles/ubiquitty-host.dir"
    if cmake -B "$HOSTQ_BUILD" -S "$ROOT" -G "$UBQ_GENERATOR" -DUBIQUITTY_FRONTEND=Host -DCMAKE_BUILD_TYPE=Debug \
                -DUBIQUITTY_QUIC=ON >"$OUT/quic-lossy-host-build.log" 2>&1 \
            && cmake --build "$HOSTQ_BUILD" -j"$BUILD_JOBS" >>"$OUT/quic-lossy-host-build.log" 2>&1; then
        host_objects "$HOSTQ_OBJDIR" "$OUT/quic-lossy-objs.txt" \
            || build_broken "host object list is empty (the harness, not the test under it)"
        if ubq_compile_parallel "$OUT/quic_lossy_transfer" "$OUT/objects/quic_lossy_transfer" "$OUT/quic-lossy-build.log" \
                -std=c++23 -I"$ROOT/Tests" -DUBIQUITTY_QUIC=1 -I"$ROOT/Source" -I"$QUIC_PREFIX/include" -- \
                "$ROOT/Tests/Host/quic_lossy_transfer.cpp" \
                "$ROOT/Source/Platform/POSIX/QUICClient.cpp" \
                -- "@$OUT/quic-lossy-objs.txt" "$QUIC_PREFIX/lib/libngtcp2_crypto_boringssl.a" "$QUIC_PREFIX/lib/libngtcp2.a" "$QUIC_PREFIX/lib/libssl.a" "$QUIC_PREFIX/lib/libcrypto.a" $QUIC_FRAMEWORKS; then
            "$OUT/quic_lossy_transfer" || exit 1
        else
            build_broken "quic_lossy_transfer link failed (see $OUT/quic-lossy-build.log)"
        fi
    else
        build_broken "quic_lossy_transfer host build failed (see $OUT/quic-lossy-host-build.log)"
    fi
fi

# quic_timestamp_clamp — ngtcp2 aborts the process on a timestamp lower than the
# connection's last, and the daemon holds every session in one process. Runs the
# production client and listener over loopback with both ends reading a clock
# that steps backwards, through a handshake, transfers and idle timers.
if wanted quic_timestamp_clamp && quic_deps_present && command -v cmake >/dev/null 2>&1; then
    echo "=== quic_timestamp_clamp ==="
    RAN=$((RAN + 1))
    QUIC_FRAMEWORKS=""
    [ "$(uname)" = "Darwin" ] && QUIC_FRAMEWORKS="-framework CoreFoundation -framework Security"
    HOSTQ_BUILD="$UBQ_BUILD_ROOT/host-quic"
    HOSTQ_OBJDIR="$HOSTQ_BUILD/CMakeFiles/ubiquitty-host.dir"
    if cmake -B "$HOSTQ_BUILD" -S "$ROOT" -G "$UBQ_GENERATOR" -DUBIQUITTY_FRONTEND=Host -DCMAKE_BUILD_TYPE=Debug \
                -DUBIQUITTY_QUIC=ON >"$OUT/quic-clamp-host-build.log" 2>&1 \
            && cmake --build "$HOSTQ_BUILD" -j"$BUILD_JOBS" >>"$OUT/quic-clamp-host-build.log" 2>&1; then
        host_objects "$HOSTQ_OBJDIR" "$OUT/quic-clamp-objs.txt" \
            || build_broken "host object list is empty (the harness, not the test under it)"
        if ubq_compile_parallel "$OUT/quic_timestamp_clamp" "$OUT/objects/quic_timestamp_clamp" "$OUT/quic-clamp-build.log" \
                -std=c++23 -I"$ROOT/Tests" -DUBIQUITTY_QUIC=1 -DUBIQUITTY_ENABLE_TEST_HOOKS=1 -pthread -I"$ROOT/Source" -I"$QUIC_PREFIX/include" -- \
                "$ROOT/Tests/Host/quic_timestamp_clamp.cpp" \
                "$ROOT/Source/Platform/POSIX/QUICClient.cpp" \
                -- "@$OUT/quic-clamp-objs.txt" "$QUIC_PREFIX/lib/libngtcp2_crypto_boringssl.a" "$QUIC_PREFIX/lib/libngtcp2.a" "$QUIC_PREFIX/lib/libssl.a" "$QUIC_PREFIX/lib/libcrypto.a" $QUIC_FRAMEWORKS; then
            "$OUT/quic_timestamp_clamp" || exit 1
        else
            build_broken "quic_timestamp_clamp link failed (see $OUT/quic-clamp-build.log)"
        fi
    else
        build_broken "quic_timestamp_clamp host build failed (see $OUT/quic-clamp-host-build.log)"
    fi
fi

# quic_preamble_capacity — an UNAUTHENTICATED QUIC peer can't grow the daemon's pre-auth
# preamble buffer without bound (pre-auth remote-OOM). onQUICStreamData drops
# a new connection before buffering more than 1 + maxQUICTokenLength pre-auth bytes,
# so a token-less byte flood is refused instead of accumulated toward the 4 MiB
# stream window (x maxQUICConnections = a multi-hundred-MiB OOM). The path is #ifdef
# UBIQUITTY_QUIC, so this links the QUIC-ON daemon objects (the build above) and a
# real (handshake-less) QUICConnection via a friend seam to drive onQUICStreamData
# and assert the peer is dropped and its quicAuthBuffer stayed bounded.
if wanted quic_preamble_capacity && quic_deps_present && command -v cmake >/dev/null 2>&1; then
    echo "=== quic_preamble_capacity ==="
    RAN=$((RAN + 1))
    QUIC_FRAMEWORKS=""
    [ "$(uname)" = "Darwin" ] && QUIC_FRAMEWORKS="-framework CoreFoundation -framework Security"
    HOSTQ_BUILD="$UBQ_BUILD_ROOT/host-quic"
    HOSTQ_OBJDIR="$HOSTQ_BUILD/CMakeFiles/ubiquitty-host.dir"
    if cmake -B "$HOSTQ_BUILD" -S "$ROOT" -G "$UBQ_GENERATOR" -DUBIQUITTY_FRONTEND=Host -DCMAKE_BUILD_TYPE=Debug \
                -DUBIQUITTY_QUIC=ON >"$OUT/quic-preamblecap-host-build.log" 2>&1 \
            && cmake --build "$HOSTQ_BUILD" -j"$BUILD_JOBS" >>"$OUT/quic-preamblecap-host-build.log" 2>&1; then
        # Link against every daemon object except the binary's own main().
        host_objects "$HOSTQ_OBJDIR" "$OUT/quic-preamblecap-objs.txt" \
            || build_broken "host object list is empty (the harness, not the test under it)"
        if ubq_compile_parallel "$OUT/quic_preamble_capacity" "$OUT/objects/quic_preamble_capacity" "$OUT/quic-preamblecap-build.log" \
                -std=c++23 -I"$ROOT/Tests" -DUBIQUITTY_QUIC=1 -I"$ROOT/Source" -I"$QUIC_PREFIX/include" -- \
                "$ROOT/Tests/Host/quic_preamble_capacity.cpp" \
                -- "@$OUT/quic-preamblecap-objs.txt" "$QUIC_PREFIX/lib/libngtcp2_crypto_boringssl.a" "$QUIC_PREFIX/lib/libngtcp2.a" "$QUIC_PREFIX/lib/libssl.a" "$QUIC_PREFIX/lib/libcrypto.a" $QUIC_FRAMEWORKS; then
            "$OUT/quic_preamble_capacity" || exit 1
        else
            build_broken "quic_preamble_capacity link failed (see $OUT/quic-preamblecap-build.log)"
        fi
    else
        build_broken "quic_preamble_capacity host build failed (see $OUT/quic-preamblecap-host-build.log)"
    fi
fi

# quic_close_on_reap — reapClient frees a dropped QUIC client's connection from the
# listener table IMMEDIATELY, not at the 90 s idle timeout. reapClient calls
# m_quic->closeConnection(quicConnection), which erases the owning unique_ptr from
# m_connections (freeing the connection + its unbounded toSend/toSendDatagrams now).
# closeConnection + m_connections are #ifdef UBIQUITTY_QUIC, so this links the QUIC-ON
# daemon objects and a friend seam that adopts a (handshake-less) connection into the
# live table, drives the private reapClient, and asserts the table count drops 1 -> 0.
if wanted quic_close_on_reap && quic_deps_present && command -v cmake >/dev/null 2>&1; then
    echo "=== quic_close_on_reap ==="
    RAN=$((RAN + 1))
    QUIC_FRAMEWORKS=""
    [ "$(uname)" = "Darwin" ] && QUIC_FRAMEWORKS="-framework CoreFoundation -framework Security"
    HOSTQ_BUILD="$UBQ_BUILD_ROOT/host-quic"
    HOSTQ_OBJDIR="$HOSTQ_BUILD/CMakeFiles/ubiquitty-host.dir"
    if cmake -B "$HOSTQ_BUILD" -S "$ROOT" -G "$UBQ_GENERATOR" -DUBIQUITTY_FRONTEND=Host -DCMAKE_BUILD_TYPE=Debug \
                -DUBIQUITTY_QUIC=ON >"$OUT/quic-closereap-host-build.log" 2>&1 \
            && cmake --build "$HOSTQ_BUILD" -j"$BUILD_JOBS" >>"$OUT/quic-closereap-host-build.log" 2>&1; then
        # Link against every daemon object except the binary's own main().
        host_objects "$HOSTQ_OBJDIR" "$OUT/quic-closereap-objs.txt" \
            || build_broken "host object list is empty (the harness, not the test under it)"
        if ubq_compile_parallel "$OUT/quic_close_on_reap" "$OUT/objects/quic_close_on_reap" "$OUT/quic-closereap-build.log" \
                -std=c++23 -I"$ROOT/Tests" -DUBIQUITTY_QUIC=1 -I"$ROOT/Source" -I"$QUIC_PREFIX/include" -- \
                "$ROOT/Tests/Host/quic_close_on_reap.cpp" \
                -- "@$OUT/quic-closereap-objs.txt" "$QUIC_PREFIX/lib/libngtcp2_crypto_boringssl.a" "$QUIC_PREFIX/lib/libngtcp2.a" "$QUIC_PREFIX/lib/libssl.a" "$QUIC_PREFIX/lib/libcrypto.a" $QUIC_FRAMEWORKS; then
            "$OUT/quic_close_on_reap" || exit 1
        else
            build_broken "quic_close_on_reap link failed (see $OUT/quic-closereap-build.log)"
        fi
    else
        build_broken "quic_close_on_reap host build failed (see $OUT/quic-closereap-host-build.log)"
    fi
fi

# quic_cid_clamp — the QUIC accept path clamps a connection-id length to
# NGTCP2_MAX_CIDLEN before the memcpy into the fixed-size 20-byte ngtcp2_cid.
# ngtcp2's wire decode never yields an overlong CID on the accept path, so a friend
# seam (testAcceptServerConnectionRejectsCID) injects a synthetic version-cid with
# scidlen/dcidlen > NGTCP2_MAX_CIDLEN and calls acceptServerConnection directly.
# BUILT WITH AddressSanitizer: with the clamp removed the memcpy overruns the 20-byte
# stack ngtcp2_cid.data and ASan reports it; with the clamp the function rejects
# (nullptr) clean. HostQUIC.cpp is compiled here with -fsanitize=address (the memcpy
# must be instrumented); the rest of the daemon objects link from the QUIC-ON host
# build (the cmake build above) MINUS HostQUIC.cpp.o (replaced by the ASan one) and
# main.cpp.o — so the ASan-instrumented acceptServerConnection's stack redzone is
# what catches an unclamped overrun.
if wanted quic_cid_clamp && quic_deps_present && command -v cmake >/dev/null 2>&1; then
    echo "=== quic_cid_clamp (address) ==="
    RAN=$((RAN + 1))
    QUIC_FRAMEWORKS=""
    [ "$(uname)" = "Darwin" ] && QUIC_FRAMEWORKS="-framework CoreFoundation -framework Security"
    HOSTQ_BUILD="$UBQ_BUILD_ROOT/host-quic"
    HOSTQ_OBJDIR="$HOSTQ_BUILD/CMakeFiles/ubiquitty-host.dir"
    # Real link+run ASan probe: some toolchains accept -fsanitize=address at compile
    # but ship no runtime, so a syntax-only check passes yet link/run fails.
    probe="$OUT/santest_cidclamp"
    printf 'int main(){return 0;}\n' > "$OUT/santest_cidclamp.cpp"
    if ! $CXX_TEST -fsanitize=address "$OUT/santest_cidclamp.cpp" -o "$probe" 2>/dev/null \
            || ! "$probe" >/dev/null 2>&1; then
        ubq_skip "$CXX_TEST has no working -fsanitize=address runtime"
        SKIPPED=$((SKIPPED + 1))
    elif cmake -B "$HOSTQ_BUILD" -S "$ROOT" -G "$UBQ_GENERATOR" -DUBIQUITTY_FRONTEND=Host -DCMAKE_BUILD_TYPE=Debug \
                -DUBIQUITTY_QUIC=ON >"$OUT/quic-cidclamp-host-build.log" 2>&1 \
            && cmake --build "$HOSTQ_BUILD" -j"$BUILD_JOBS" >>"$OUT/quic-cidclamp-host-build.log" 2>&1; then
        # Every daemon object except the binary's main() and HostQUIC.cpp.o (we link
        # an ASan-instrumented HostQUIC.cpp instead, so the memcpy is checked).
        host_objects "$HOSTQ_OBJDIR" "$OUT/quic-cidclamp-objs.txt" "HostQUIC.cpp.o" \
            || build_broken "host object list is empty (the harness, not the test under it)"
        # DatagramBatch is not named here: it arrives with the host objects,
        # because the write pump sends through it and the host target compiles it.
        if ubq_compile_parallel "$OUT/quic_cid_clamp" "$OUT/objects/quic_cid_clamp" "$OUT/quic-cidclamp-build.log" \
                -std=c++23 -I"$ROOT/Tests" -DUBIQUITTY_QUIC=1 -g -O1 -fsanitize=address -I"$ROOT/Source" -I"$QUIC_PREFIX/include" -- \
                "$ROOT/Tests/Host/quic_cid_clamp.cpp" \
                "$ROOT/Source/Platform/Host/HostQUIC.cpp" \
                -- "@$OUT/quic-cidclamp-objs.txt" "$QUIC_PREFIX/lib/libngtcp2_crypto_boringssl.a" "$QUIC_PREFIX/lib/libngtcp2.a" "$QUIC_PREFIX/lib/libssl.a" "$QUIC_PREFIX/lib/libcrypto.a" $QUIC_FRAMEWORKS; then
            "$OUT/quic_cid_clamp" || exit 1
        else
            build_broken "quic_cid_clamp link failed (see $OUT/quic-cidclamp-build.log)"
        fi
    else
        build_broken "quic_cid_clamp host build failed (see $OUT/quic-cidclamp-host-build.log)"
    fi
fi

# Private daemons exercise workspace carrier isolation and explicit bootstrap routing.
if wanted workspace_mux; then
    echo "=== workspace_mux ==="
    RAN=$((RAN + 1))
    if HOST_BINARY="$(ubq_host_binary "$OUT/workspace-mux-host-build.log")"; then
        ubq_run_private python3 "$ROOT/Tests/Host/workspace_mux.py" "$HOST_BINARY" || exit 1
    else
        build_broken "workspace_mux Host build failed (see $OUT/workspace-mux-host-build.log)"
    fi
fi

if wanted ensure_preserves_owner; then
    echo "=== ensure_preserves_owner ==="
    RAN=$((RAN + 1))
    if HOST_BINARY="$(ubq_host_binary "$OUT/ensure-preserves-owner-host-build.log")"; then
        ubq_run_private python3 "$ROOT/Tests/Host/ensure_preserves_owner.py" "$HOST_BINARY" || exit 1
    else
        build_broken "ensure_preserves_owner Host build failed (see $OUT/ensure-preserves-owner-host-build.log)"
    fi
fi

if wanted pinned_quic_bootstrap; then
    echo "=== pinned_quic_bootstrap ==="
    RAN=$((RAN + 1))
    if HOST_BINARY="$(ubq_host_binary "$OUT/pinned-quic-bootstrap-host-build.log")"; then
        ubq_run_private python3 "$ROOT/Tests/Host/pinned_quic_bootstrap.py" "$HOST_BINARY" || exit 1
    else
        build_broken "pinned_quic_bootstrap Host build failed (see $OUT/pinned-quic-bootstrap-host-build.log)"
    fi
fi

# Private daemon and synthetic clipboard fixtures exercise the installed helper aliases.
if wanted clipboard_tool_ipc; then
    echo "=== clipboard_tool_ipc ==="
    RAN=$((RAN + 1))
    if HOST_BINARY="$(ubq_host_binary "$OUT/clipboard-tool-host-build.log")"; then
        ubq_run_private python3 "$ROOT/Tests/Host/clipboard_tool_ipc.py" "$HOST_BINARY" || exit 1
    else
        build_broken "clipboard_tool_ipc Host build failed (see $OUT/clipboard-tool-host-build.log)"
    fi
fi

if wanted clipboard_tool_no_rcs; then
    echo "=== clipboard_tool_no_rcs ==="
    RAN=$((RAN + 1))
    if [ ! -x /bin/zsh ]; then
        ubq_skip "clipboard_tool_no_rcs requires zsh"
        SKIPPED=$((SKIPPED + 1))
    elif HOST_BINARY="$(ubq_host_binary "$OUT/clipboard-tool-no-rcs-host-build.log")"; then
        ubq_run_private python3 "$ROOT/Tests/Host/clipboard_tool_ipc.py" "$HOST_BINARY" --no-rcs || exit 1
    else
        build_broken "clipboard_tool_no_rcs Host build failed (see $OUT/clipboard-tool-no-rcs-host-build.log)"
    fi
fi

# Consent gate — spawn the real daemon, drive a human + an agent client
# over the unix socket: the agent's attach to the human's session is HELD
# (AwaitingConsent Welcome and Pending status, no checkpoint) until the human
# allows or denies through consent.respond. Needs a built host and its codecs.
if wanted consent_gate && command -v cmake >/dev/null 2>&1; then
    echo "=== consent_gate ==="
    RAN=$((RAN + 1))
    if HOST_BINARY="$(ubq_host_binary "$OUT/consent-host-build.log")" \
            && host_objects "$HOST_TEST_BUILD/CMakeFiles/ubiquitty-host.dir" "$OUT/consent-objs.txt" \
            && ubq_compile_parallel "$OUT/consent_gate" "$OUT/objects/consent_gate" "$OUT/consent-build.log" \
                    -std=c++23 -DUBIQUITTY_QUIC=1 -I"$QUIC_PREFIX/include" -I"$ROOT/Tests" -I"$ROOT/Source" -- \
                    "$ROOT/Tests/Host/consent_gate.cpp" -- "@$OUT/consent-objs.txt" "${HOST_LINK_ARCHIVES[@]}"; then
        ubq_run_private "$OUT/consent_gate" "$HOST_BINARY" || exit 1
    else
        build_broken "consent_gate build failed (see $OUT/consent-build.log + $OUT/consent-host-build.log)"
    fi
fi

# Install-time grant — the real CLI issues a token with --allow-always, the
# real daemon admits the agent presenting it to a session nobody is attached
# to, holds an ungranted token at its exact pending question, takes a grant
# issued while it runs through authenticated local reload, and drops it with the
# token on revoke. Needs only a built ubiquitty-host (no QUIC).
if wanted consent_install_grant && command -v cmake >/dev/null 2>&1; then
    echo "=== consent_install_grant ==="
    RAN=$((RAN + 1))
    if HOST_BINARY="$(ubq_host_binary "$OUT/consent-install-grant-host-build.log")" \
            && host_objects "$HOST_TEST_BUILD/CMakeFiles/ubiquitty-host.dir" "$OUT/consent-install-grant-objs.txt" \
            && ubq_compile_parallel "$OUT/consent_install_grant" "$OUT/objects/consent_install_grant" "$OUT/consent-install-grant-build.log" \
                    -std=c++23 -DUBIQUITTY_QUIC=1 -I"$QUIC_PREFIX/include" -I"$ROOT/Tests" -I"$ROOT/Source" -- \
                    "$ROOT/Tests/Host/consent_install_grant.cpp" -- "@$OUT/consent-install-grant-objs.txt" "${HOST_LINK_ARCHIVES[@]}"; then
        ubq_run_private "$OUT/consent_install_grant" "$HOST_BINARY" || exit 1
    else
        build_broken "consent_install_grant build failed (see $OUT/consent-install-grant-build.log + $OUT/consent-install-grant-host-build.log)"
    fi
fi

if wanted token_auth && command -v cmake >/dev/null 2>&1; then
    echo "=== token_auth ==="
    RAN=$((RAN + 1))
    if HOST_BINARY="$(ubq_host_binary "$OUT/token-host-build.log")" \
            && host_objects "$HOST_TEST_BUILD/CMakeFiles/ubiquitty-host.dir" "$OUT/token-objs.txt" \
            && ubq_compile_parallel "$OUT/token_auth" "$OUT/objects/token_auth" "$OUT/token-build.log" \
                    -std=c++23 -DUBIQUITTY_QUIC=1 -I"$QUIC_PREFIX/include" -I"$ROOT/Tests" -I"$ROOT/Source" -- \
                    "$ROOT/Tests/Host/token_auth.cpp" -- "@$OUT/token-objs.txt" ${HOST_LINK_ARCHIVES[@]+"${HOST_LINK_ARCHIVES[@]}"}; then
        ubq_run_private "$OUT/token_auth" "$HOST_BINARY" || exit 1
    else
        build_broken "token_auth build failed (see $OUT/token-build.log + $OUT/token-host-build.log)"
    fi
fi

# token_scope_parse — `ubiquitty-host token issue --scope ...` fails closed
# on a misspelled / empty scope instead of over-granting the observe|drive
# default. Drives the REAL host binary; needs no daemon.
if wanted token_scope_parse && command -v cmake >/dev/null 2>&1; then
    echo "=== token_scope_parse ==="
    RAN=$((RAN + 1))
    if HOST_BINARY="$(ubq_host_binary "$OUT/scope-host-build.log")" \
            && ubq_compile_parallel "$OUT/token_scope_parse" "$OUT/objects/token_scope_parse" "$OUT/scope-build.log" \
                    -std=c++23 -I"$ROOT/Tests" -I"$ROOT/Source" -- \
                    "$ROOT/Tests/Host/token_scope_parse.cpp"; then
        ubq_run_private "$OUT/token_scope_parse" "$HOST_BINARY" || exit 1
    else
        build_broken "token_scope_parse build failed (see $OUT/scope-build.log + $OUT/scope-host-build.log)"
    fi
fi

# control_token — a Control-only token (no observe) connects as a
# control-only client (agent ls/kill) but is scope-denied when it tries to
# attach a REAL session. Spawns the real daemon (require_token on).
if wanted control_token && command -v cmake >/dev/null 2>&1; then
    echo "=== control_token ==="
    RAN=$((RAN + 1))
    if HOST_BINARY="$(ubq_host_binary "$OUT/ctrl-host-build.log")" \
            && host_objects "$HOST_TEST_BUILD/CMakeFiles/ubiquitty-host.dir" "$OUT/ctrl-objs.txt" \
            && ubq_compile_parallel "$OUT/control_token" "$OUT/objects/control_token" "$OUT/ctrl-build.log" \
                    -std=c++23 -DUBIQUITTY_QUIC=1 -I"$QUIC_PREFIX/include" -I"$ROOT/Tests" -I"$ROOT/Source" -- \
                    "$ROOT/Tests/Host/control_token.cpp" -- "@$OUT/ctrl-objs.txt" ${HOST_LINK_ARCHIVES[@]+"${HOST_LINK_ARCHIVES[@]}"}; then
        ubq_run_private "$OUT/control_token" "$HOST_BINARY" || exit 1
    else
        build_broken "control_token build failed (see $OUT/ctrl-build.log + $OUT/ctrl-host-build.log)"
    fi
fi

# tmux_real_e2e — the TmuxClient parser against a real tmux -C on a private
# socket: our wrapKeyForPane drives the live pane, and the real %output,
# %session-renamed, %layout-change and list-panes rows feed our decoders.
# Skips (with the reason) where no tmux binary exists.
if wanted tmux_real_e2e; then
    if command -v tmux >/dev/null 2>&1; then
        echo "=== tmux_real_e2e ==="
        RAN=$((RAN + 1))
        if ubq_build_test ubq-shell-tmux_real_e2e "$OUT/tmux_real_e2e" "$OUT/tmux-e2e-build.log"; then
            ubq_run_private "$OUT/tmux_real_e2e" || exit 1
        else
            build_broken "tmux_real_e2e build failed (see $OUT/tmux-e2e-build.log)"
        fi
    else
        ubq_skip "tmux not installed"
        SKIPPED=$((SKIPPED + 1)) # a sole-filter skip must count, or the run reads as "no tests matched"
    fi
fi

# tmux_control_stress — tmux control mode under load against a real tmux
# server on its own socket and HOME, hosted as a frontend hosts it: a flood
# beside typing, window and pane churn, a resize storm, the control stream
# re-read in every chunking, the server or the client killed under a live
# session, and a detach and reattach, each screen held against tmux's own
# capture-pane. Built in the Core test tree; skips where no tmux exists.
if wanted tmux_control_stress && command -v cmake >/dev/null 2>&1; then
    if command -v tmux >/dev/null 2>&1; then
        echo "=== tmux_control_stress ==="
        RAN=$((RAN + 1))
        if ubq_build_test ubq-tmux-control-stress "$OUT/tmux_control_stress" "$OUT/tmux-stress-build.log"; then
            ubq_run_bounded 900 "$OUT/tmux_control_stress" || exit 1
        else
            build_broken "tmux_control_stress build failed (see $OUT/tmux-stress-build.log)"
        fi
    else
        ubq_skip "tmux not installed"
        SKIPPED=$((SKIPPED + 1)) # a sole-filter skip must count, or the run reads as "no tests matched"
    fi
fi

# host_cli — the binary's own command line: usage, --version, token
# list/revoke, --ensure-daemon's double-fork detach and --quic-bootstrap.
# Detached daemons are reaped from their pid files inside the sandbox home.
if wanted host_cli && command -v cmake >/dev/null 2>&1; then
    echo "=== host_cli ==="
    RAN=$((RAN + 1))
    if HOST_BINARY="$(ubq_host_binary "$OUT/cli-host-build.log")" \
            && ubq_compile_parallel "$OUT/host_cli" "$OUT/objects/host_cli" "$OUT/cli-build.log" \
                    -std=c++23 -I"$ROOT/Tests" -I"$ROOT/Source" -- \
                    "$ROOT/Tests/Host/host_cli.cpp"; then
        ubq_run_private "$OUT/host_cli" "$HOST_BINARY" || exit 1
    else
        build_broken "host_cli build failed (see $OUT/cli-build.log + $OUT/cli-host-build.log)"
    fi
fi

# control_surface — typed session directory, retired read-tag refusal, EXIT's
# detach-not-destroy, and the attached owner's typed termination exemption,
# all against the real daemon over the unix socket.
if wanted control_surface && command -v cmake >/dev/null 2>&1; then
    echo "=== control_surface ==="
    RAN=$((RAN + 1))
    if HOST_BINARY="$(ubq_host_binary "$OUT/surface-host-build.log")" \
            && host_objects "$HOST_TEST_BUILD/CMakeFiles/ubiquitty-host.dir" "$OUT/surface-objs.txt" \
            && ubq_compile_parallel "$OUT/control_surface" "$OUT/objects/control_surface" "$OUT/surface-build.log" \
                    -std=c++23 -DUBIQUITTY_QUIC=1 -I"$QUIC_PREFIX/include" -I"$ROOT/Tests" -I"$ROOT/Source" -- \
                    "$ROOT/Tests/Host/control_surface.cpp" -- "@$OUT/surface-objs.txt" "${HOST_LINK_ARCHIVES[@]}"; then
        ubq_run_private "$OUT/control_surface" "$HOST_BINARY" || exit 1
    else
        build_broken "control_surface build failed (see $OUT/surface-build.log + $OUT/surface-host-build.log)"
    fi
fi

# host_handover — a running daemon hands every live pane to the binary that
# replaced it at its install path: real processes, PTYs and --attach relays.
# The holder exits 0 after Bye{HandingOver}; each client resumes on the
# successor with its pane's id and scrollback; a copy at another path is refused.
if wanted host_handover && command -v cmake >/dev/null 2>&1; then
    echo "=== host_handover ==="
    RAN=$((RAN + 1))
    if HOST_BINARY="$(ubq_host_binary "$OUT/host-handover-host-build.log")" \
            && host_objects "$HOST_TEST_BUILD/CMakeFiles/ubiquitty-host.dir" "$OUT/host-handover-objs.txt" \
            && ubq_compile_parallel "$OUT/host_handover" "$OUT/objects/host_handover" "$OUT/host-handover-build.log" \
                    -std=c++23 -DUBIQUITTY_QUIC=1 -I"$QUIC_PREFIX/include" -I"$ROOT/Tests" -I"$ROOT/Source" -- \
                    "$ROOT/Tests/Host/host_handover.cpp" -- "@$OUT/host-handover-objs.txt" "${HOST_LINK_ARCHIVES[@]}"; then
        ubq_run_bounded 180 "$OUT/host_handover" "$HOST_BINARY" || exit 1
    else
        build_broken "host_handover build failed (see $OUT/host-handover-build.log + $OUT/host-handover-host-build.log)"
    fi
fi

# local_host_upgrade — this machine's host upgraded as the macOS app upgrades
# it after an update: a holder installed in the control directory refuses the
# bundled host started where it lies, and hands every session to it once it is
# linked over the install; a saved reference to the holder's workspace then
# reattaches on the successor. A host started from an earlier bundle hands over
# to the updated bundle's. UBQ_OLDER_HOST and UBQ_OLDER_AGENT, set together,
# name an older release's ubiquitty-host and ubq to run as the holder. On
# macOS, UBQ_TEAM_SIGN_IDENTITY names a code-signing identity of the trusted
# team, under which a successor the team signed takes over wherever it lies;
# UBQ_TEAM_SIGN_KEYCHAIN names the keychain that holds it, since the test's
# private machines replace HOME.
if wanted local_host_upgrade && command -v cmake >/dev/null 2>&1; then
    echo "=== local_host_upgrade ==="
    RAN=$((RAN + 1))
    if HOST_BINARY="$(ubq_host_binary "$OUT/local-host-upgrade-host-build.log")" \
            && host_objects "$HOST_TEST_BUILD/CMakeFiles/ubiquitty-host.dir" "$OUT/local-host-upgrade-objs.txt" \
            && ubq_compile_parallel "$OUT/local_host_upgrade" "$OUT/objects/local_host_upgrade" "$OUT/local-host-upgrade-build.log" \
                    -std=c++23 -DUBIQUITTY_QUIC=1 -I"$QUIC_PREFIX/include" -I"$ROOT/Tests" -I"$ROOT/Source" -- \
                    "$ROOT/Tests/Host/local_host_upgrade.cpp" \
                    -- "@$OUT/local-host-upgrade-objs.txt" "${HOST_LINK_ARCHIVES[@]}"; then
        ubq_run_bounded 300 "$OUT/local_host_upgrade" "$HOST_BINARY" || exit 1
        if [ -n "${UBQ_OLDER_HOST:-}" ] && [ -n "${UBQ_OLDER_AGENT:-}" ]; then
            ubq_run_bounded 300 "$OUT/local_host_upgrade" "$HOST_BINARY" "$UBQ_OLDER_HOST" "$UBQ_OLDER_AGENT" || exit 1
        fi
    else
        build_broken "local_host_upgrade build failed (see $OUT/local-host-upgrade-build.log + $OUT/local-host-upgrade-host-build.log)"
    fi
fi

# quic_handover_resume — a QUIC client follows a whole-host handover: the
# holder closes its connection saying HandingOver, the successor serves the
# holder's port and certificate, and a resume proof bound to the holder's
# instance continues the attachment there.
if wanted quic_handover_resume && quic_deps_present && command -v cmake >/dev/null 2>&1; then
    echo "=== quic_handover_resume ==="
    RAN=$((RAN + 1))
    QUIC_FRAMEWORKS=""
    [ "$(uname)" = "Darwin" ] && QUIC_FRAMEWORKS="-framework CoreFoundation -framework Security"
    if HOST_BINARY="$(ubq_host_binary "$OUT/quic-handover-host-build.log")" \
            && host_objects "$HOST_TEST_BUILD/CMakeFiles/ubiquitty-host.dir" "$OUT/quic-handover-objs.txt" \
            && ubq_compile_parallel "$OUT/quic_handover_resume" "$OUT/objects/quic_handover_resume" "$OUT/quic-handover-build.log" \
                    -std=c++23 -DUBIQUITTY_QUIC=1 -pthread -I"$QUIC_PREFIX/include" -I"$ROOT/Tests" -I"$ROOT/Source" -- \
                    "$ROOT/Tests/Host/quic_handover_resume.cpp" \
                    "$ROOT/Source/Platform/POSIX/QUICClient.cpp" \
                    -- "@$OUT/quic-handover-objs.txt" "$QUIC_PREFIX/lib/libngtcp2_crypto_boringssl.a" "$QUIC_PREFIX/lib/libngtcp2.a" "$QUIC_PREFIX/lib/libssl.a" "$QUIC_PREFIX/lib/libcrypto.a" "${HOST_LINK_ARCHIVES[@]}" $QUIC_FRAMEWORKS; then
        ubq_run_bounded 180 "$OUT/quic_handover_resume" "$HOST_BINARY" || exit 1
    else
        build_broken "quic_handover_resume build failed (see $OUT/quic-handover-build.log + $OUT/quic-handover-host-build.log)"
    fi
fi

# handover_input_once — 300 numbered lines typed into a slow PTY reader while
# the host hands over: the client resumes on the successor, sends again what it
# reports unwritten, and every line reaches the program once, in order.
if wanted handover_input_once && command -v cmake >/dev/null 2>&1; then
    echo "=== handover_input_once ==="
    RAN=$((RAN + 1))
    if HOST_BINARY="$(ubq_host_binary "$OUT/handover-input-once-host-build.log")" \
            && host_objects "$HOST_TEST_BUILD/CMakeFiles/ubiquitty-host.dir" "$OUT/handover-input-once-objs.txt" \
            && ubq_compile_parallel "$OUT/handover_input_once" "$OUT/objects/handover_input_once" "$OUT/handover-input-once-build.log" \
                    -std=c++23 -DUBIQUITTY_QUIC=1 -I"$QUIC_PREFIX/include" -I"$ROOT/Tests" -I"$ROOT/Source" -- \
                    "$ROOT/Tests/Host/handover_input_once.cpp" -- "@$OUT/handover-input-once-objs.txt" "${HOST_LINK_ARCHIVES[@]}"; then
        ubq_run_bounded 240 "$OUT/handover_input_once" "$HOST_BINARY" || exit 1
    else
        build_broken "handover_input_once build failed (see $OUT/handover-input-once-build.log + $OUT/handover-input-once-host-build.log)"
    fi
fi

# handover_diffed_resume — a client that held its pane when its host handed
# over resumes the same attachment on the successor and is continued with a
# diff, no checkpoint, between real daemons.
if wanted handover_diffed_resume && command -v cmake >/dev/null 2>&1; then
    echo "=== handover_diffed_resume ==="
    RAN=$((RAN + 1))
    if HOST_BINARY="$(ubq_host_binary "$OUT/handover-diffed-resume-host-build.log")" \
            && host_objects "$HOST_TEST_BUILD/CMakeFiles/ubiquitty-host.dir" "$OUT/handover-diffed-resume-objs.txt" \
            && ubq_compile_parallel "$OUT/handover_diffed_resume" "$OUT/objects/handover_diffed_resume" "$OUT/handover-diffed-resume-build.log" \
                    -std=c++23 -DUBIQUITTY_QUIC=1 -I"$QUIC_PREFIX/include" -I"$ROOT/Tests" -I"$ROOT/Source" -- \
                    "$ROOT/Tests/Host/handover_diffed_resume.cpp" -- "@$OUT/handover-diffed-resume-objs.txt" "${HOST_LINK_ARCHIVES[@]}"; then
        ubq_run_bounded 240 "$OUT/handover_diffed_resume" "$HOST_BINARY" || exit 1
    else
        build_broken "handover_diffed_resume build failed (see $OUT/handover-diffed-resume-build.log + $OUT/handover-diffed-resume-host-build.log)"
    fi
fi

# handover_refusal_keeps_serving — an install never ends a session: a holder
# with a pane that cannot reach ground refuses, keeps every pane and serves on,
# and the successor beside it logs the reason it was given.
if wanted handover_refusal_keeps_serving && command -v cmake >/dev/null 2>&1; then
    echo "=== handover_refusal_keeps_serving ==="
    RAN=$((RAN + 1))
    if HOST_BINARY="$(ubq_host_binary "$OUT/handover-refusal-host-build.log")" \
            && host_objects "$HOST_TEST_BUILD/CMakeFiles/ubiquitty-host.dir" "$OUT/handover-refusal-objs.txt" \
            && ubq_compile_parallel "$OUT/handover_refusal_keeps_serving" "$OUT/objects/handover_refusal_keeps_serving" "$OUT/handover-refusal-build.log" \
                    -std=c++23 -DUBIQUITTY_QUIC=1 -I"$QUIC_PREFIX/include" -I"$ROOT/Tests" -I"$ROOT/Source" -- \
                    "$ROOT/Tests/Host/handover_refusal_keeps_serving.cpp" -- "@$OUT/handover-refusal-objs.txt" "${HOST_LINK_ARCHIVES[@]}"; then
        ubq_run_bounded 240 "$OUT/handover_refusal_keeps_serving" "$HOST_BINARY" || exit 1
    else
        build_broken "handover_refusal_keeps_serving build failed (see $OUT/handover-refusal-build.log + $OUT/handover-refusal-host-build.log)"
    fi
fi

# session_directory — the portable SessionDirectoryCore over a real
# LocalSocketChannel to the real daemon: the session list a person is shown
# before any attach, a terminate settling as Terminated, the next list no longer
# carrying the ended session, and a token-requiring host refusing a directory
# that presents none.
if wanted session_directory && command -v cmake >/dev/null 2>&1; then
    echo "=== session_directory ==="
    RAN=$((RAN + 1))
    if HOST_BINARY="$(ubq_host_binary "$OUT/sessdir-host-build.log")" \
            && ubq_build_test ubq-shell-session_directory "$OUT/session_directory" "$OUT/sessdir-build.log"; then
        # The directory's outline is compared with what `ubq tree` prints of
        # the same daemon, so the agent's binary is part of the case.
        SESSDIR_UBQ="$(ubq_agent_binary "$OUT/sessdir-agent-build.log")" \
            || build_broken "session_directory: ubq did not build (see $OUT/sessdir-agent-build.log)"
        ubq_run_private "$OUT/session_directory" "$HOST_BINARY" --ubq "$SESSDIR_UBQ" || exit 1
    else
        build_broken "session_directory build failed (see $OUT/sessdir-build.log + $OUT/sessdir-host-build.log)"
    fi
fi
# directory_watch_silence — the same binary's one slow case: a watch
# (SessionDirectoryCore::watch) held against the real daemon through 70 seconds
# in which nothing on the host changes, more than twice the daemon's silence
# bound, and still following afterwards on the connection it held. Seventy
# seconds of wall time by construction; the suite has no slow class to put it
# in, so it is a stanza of its own that says so.
if wanted directory_watch_silence && command -v cmake >/dev/null 2>&1; then
    echo "=== directory_watch_silence ==="
    RAN=$((RAN + 1))
    if HOST_BINARY="$(ubq_host_binary "$OUT/sessdir-host-build.log")" \
            && ubq_build_test ubq-shell-session_directory "$OUT/session_directory" "$OUT/sessdir-build.log"; then
        ubq_run_private "$OUT/session_directory" "$HOST_BINARY" --watch-silence || exit 1
    else
        build_broken "directory_watch_silence build failed (see $OUT/sessdir-build.log + $OUT/sessdir-host-build.log)"
    fi
fi

# resize_clamp — over-limit HELLO and Viewport dims are clamped to the Core
# ceiling for both the Core model (read via SESSION_LIST) and the
# child's PTY (read via `stty size`). Spawns the real daemon over the unix socket.
if wanted resize_clamp && command -v cmake >/dev/null 2>&1; then
    echo "=== resize_clamp ==="
    RAN=$((RAN + 1))
    if HOST_BINARY="$(ubq_host_binary "$OUT/resize-host-build.log")" \
            && host_objects "$HOST_TEST_BUILD/CMakeFiles/ubiquitty-host.dir" "$OUT/resize-objs.txt" \
            && ubq_compile_parallel "$OUT/resize_clamp" "$OUT/objects/resize_clamp" "$OUT/resize-build.log" \
                    -std=c++23 -DUBIQUITTY_QUIC=1 -I"$QUIC_PREFIX/include" -I"$ROOT/Tests" -I"$ROOT/Source" -- \
                    "$ROOT/Tests/Host/resize_clamp.cpp" -- "@$OUT/resize-objs.txt" ${HOST_LINK_ARCHIVES[@]+"${HOST_LINK_ARCHIVES[@]}"}; then
        ubq_run_private "$OUT/resize_clamp" "$HOST_BINARY" || exit 1
    else
        build_broken "resize_clamp build failed (see $OUT/resize-build.log + $OUT/resize-host-build.log)"
    fi
fi

# file_transfer_confinement — FILE_REQUEST (click-to-open) confines to the
# session's cwd subtree with no escape hatch: an in-cwd file is served, an
# absolute path with a literal ".." resolving outside cwd is denied
# (OutsideConfinement) even though it exists and is perfectly readable by the
# daemon's own uid, a directory is rejected (NotRegular), and a missing in-cwd
# path reports NotFound (distinguishing it from the confinement denial).
# Spawns the real daemon over the unix socket, same family as resize_clamp.
if wanted file_transfer_confinement && command -v cmake >/dev/null 2>&1; then
    echo "=== file_transfer_confinement ==="
    RAN=$((RAN + 1))
    if HOST_BINARY="$(ubq_host_binary "$OUT/filexfer-host-build.log")" \
            && host_objects "$HOST_TEST_BUILD/CMakeFiles/ubiquitty-host.dir" "$OUT/filexfer-objs.txt" \
            && ubq_compile_parallel "$OUT/file_transfer_confinement" "$OUT/objects/file_transfer_confinement" "$OUT/filexfer-build.log" \
                    -std=c++23 -DUBIQUITTY_QUIC=1 -I"$QUIC_PREFIX/include" -I"$ROOT/Tests" -I"$ROOT/Source" -- \
                    "$ROOT/Tests/Host/file_transfer_confinement.cpp" -- "@$OUT/filexfer-objs.txt" ${HOST_LINK_ARCHIVES[@]+"${HOST_LINK_ARCHIVES[@]}"}; then
        ubq_run_private "$OUT/file_transfer_confinement" "$HOST_BINARY" || exit 1
    else
        build_broken "file_transfer_confinement build failed (see $OUT/filexfer-build.log + $OUT/filexfer-host-build.log)"
    fi
fi

# host_ptyfacts_cwd — ubiquitty-host's processCurrentDirectory (Source/Platform/
# POSIX/PTYFacts.cpp) reports a REAL cwd on Darwin instead of the always-empty
# empty stub: drives a bare OSC-7-silent shell through a `cd` and asserts
# PaneFacts' fallback (foregroundCurrentDirectory -> processCurrentDirectory)
# reports it. Spawns the real daemon over the unix socket.
if wanted host_ptyfacts_cwd && command -v cmake >/dev/null 2>&1; then
    echo "=== host_ptyfacts_cwd ==="
    RAN=$((RAN + 1))
    if HOST_BINARY="$(ubq_host_binary "$OUT/ptyfacts-host-build.log")" \
            && host_objects "$HOST_TEST_BUILD/CMakeFiles/ubiquitty-host.dir" "$OUT/ptyfacts-objs.txt" \
            && ubq_compile_parallel "$OUT/host_ptyfacts_cwd" "$OUT/objects/host_ptyfacts_cwd" "$OUT/ptyfacts-build.log" \
                    -std=c++23 -DUBIQUITTY_QUIC=1 -I"$QUIC_PREFIX/include" -I"$ROOT/Tests" -I"$ROOT/Source" -- \
                    "$ROOT/Tests/Host/host_ptyfacts_cwd.cpp" -- "@$OUT/ptyfacts-objs.txt" ${HOST_LINK_ARCHIVES[@]+"${HOST_LINK_ARCHIVES[@]}"}; then
        ubq_run_private "$OUT/host_ptyfacts_cwd" "$HOST_BINARY" || exit 1
    else
        build_broken "host_ptyfacts_cwd build failed (see $OUT/ptyfacts-build.log + $OUT/ptyfacts-host-build.log)"
    fi
fi

# session_shell_environment — what a shell started by an --ensure-daemon
# daemon inherits: UBIQUITTY_INTEGRATION_DIR naming the shell-integration
# scripts the host carries, written into its control directory. Spawns the
# real daemon over the unix socket.
if wanted session_shell_environment && command -v cmake >/dev/null 2>&1; then
    echo "=== session_shell_environment ==="
    RAN=$((RAN + 1))
    if HOST_BINARY="$(ubq_host_binary "$OUT/shellenv-host-build.log")" \
            && host_objects "$HOST_TEST_BUILD/CMakeFiles/ubiquitty-host.dir" "$OUT/shellenv-objs.txt" \
            && ubq_compile_parallel "$OUT/session_shell_environment" "$OUT/objects/session_shell_environment" "$OUT/shellenv-build.log" \
                    -std=c++23 -DUBIQUITTY_QUIC=1 -I"$QUIC_PREFIX/include" -I"$ROOT/Tests" -I"$ROOT/Source" -- \
                    "$ROOT/Tests/Host/session_shell_environment.cpp" -- "@$OUT/shellenv-objs.txt" ${HOST_LINK_ARCHIVES[@]+"${HOST_LINK_ARCHIVES[@]}"}; then
        ubq_run_private "$OUT/session_shell_environment" "$HOST_BINARY" "$ROOT" || exit 1
    else
        build_broken "session_shell_environment build failed (see $OUT/shellenv-build.log + $OUT/shellenv-host-build.log)"
    fi
fi

# partial_frame_budget — the inbound-frame budget reconciled with the
# longest frame a client may send: one declared a byte longer is refused at
# its header, a stalled partial frame is dropped on its 10 s deadline, many
# under-cap partials summing past the aggregate cap drop newest-first, and a
# valid near-ceiling Sync stream is received whole and its final Probe echoed
# (not dropped mid-body).
# Spawns the real daemon over the unix socket. The decoder's retained-capacity
# bound is unit-tested in test_sync.
if wanted partial_frame_budget && command -v cmake >/dev/null 2>&1; then
    echo "=== partial_frame_budget ==="
    RAN=$((RAN + 1))
    if HOST_BINARY="$(ubq_host_binary "$OUT/partial-host-build.log")" \
            && host_objects "$HOST_TEST_BUILD/CMakeFiles/ubiquitty-host.dir" "$OUT/partial-objs.txt" \
            && ubq_compile_parallel "$OUT/partial_frame_budget" "$OUT/objects/partial_frame_budget" "$OUT/partial-build.log" \
                    -std=c++23 -DUBIQUITTY_QUIC=1 -I"$QUIC_PREFIX/include" -I"$ROOT/Tests" -I"$ROOT/Source" -- \
                    "$ROOT/Tests/Host/partial_frame_budget.cpp" -- "@$OUT/partial-objs.txt" ${HOST_LINK_ARCHIVES[@]+"${HOST_LINK_ARCHIVES[@]}"}; then
        ubq_run_private "$OUT/partial_frame_budget" "$HOST_BINARY" || exit 1
    else
        build_broken "partial_frame_budget build failed (see $OUT/partial-build.log + $OUT/partial-host-build.log)"
    fi
fi

# quic_token_ttl — QUIC bootstrap tokens expire: mint stamps a deadline,
# consume rejects + GCs expired entries, prune runs even below the cap. The
# token store lives behind the #ifdef UBIQUITTY_QUIC seam, so this links the
# QUIC-ON daemon objects (built under build/host-quic) and the boringssl/AWS-LC
# libs, then unit-tests the token-store TTL logic via a friend seam.
if wanted quic_token_ttl && quic_deps_present && command -v cmake >/dev/null 2>&1; then
    echo "=== quic_token_ttl ==="
    RAN=$((RAN + 1))
    QUIC_FRAMEWORKS=""
    [ "$(uname)" = "Darwin" ] && QUIC_FRAMEWORKS="-framework CoreFoundation -framework Security"
    HOSTQ_BUILD="$UBQ_BUILD_ROOT/host-quic"
    HOSTQ_OBJDIR="$HOSTQ_BUILD/CMakeFiles/ubiquitty-host.dir"
    if cmake -B "$HOSTQ_BUILD" -S "$ROOT" -G "$UBQ_GENERATOR" -DUBIQUITTY_FRONTEND=Host -DCMAKE_BUILD_TYPE=Debug \
                -DUBIQUITTY_QUIC=ON >"$OUT/quicttl-host-build.log" 2>&1 \
            && cmake --build "$HOSTQ_BUILD" -j"$BUILD_JOBS" >>"$OUT/quicttl-host-build.log" 2>&1; then
        # Link against every daemon object except the binary's own main().
        host_objects "$HOSTQ_OBJDIR" "$OUT/quicttl-objs.txt" \
            || build_broken "host object list is empty (the harness, not the test under it)"
        if ubq_compile_parallel "$OUT/quic_token_ttl" "$OUT/objects/quic_token_ttl" "$OUT/quicttl-build.log" \
                -std=c++23 -I"$ROOT/Tests" -DUBIQUITTY_QUIC=1 -I"$ROOT/Source" -I"$QUIC_PREFIX/include" -- \
                "$ROOT/Tests/Host/quic_token_ttl.cpp" \
                -- "@$OUT/quicttl-objs.txt" "$QUIC_PREFIX/lib/libngtcp2_crypto_boringssl.a" "$QUIC_PREFIX/lib/libngtcp2.a" "$QUIC_PREFIX/lib/libssl.a" "$QUIC_PREFIX/lib/libcrypto.a" $QUIC_FRAMEWORKS; then
            "$OUT/quic_token_ttl" || exit 1
        else
            build_broken "quic_token_ttl link failed (see $OUT/quicttl-build.log)"
        fi
    else
        build_broken "quic_token_ttl host build failed (see $OUT/quicttl-host-build.log)"
    fi
fi

# quic_token_mint_flood — a same-uid mint flood must not evict ANOTHER
# client's pending QUIC bootstrap token: mintQUICToken's cap
# eviction is self-only now (drops the MINTING client's own oldest pending
# token; a client with none of its own fails the mint closed). Same
# QUIC-ON build/link as quic_token_ttl — the token store lives behind the
# #ifdef UBIQUITTY_QUIC seam.
if wanted quic_token_mint_flood && quic_deps_present && command -v cmake >/dev/null 2>&1; then
    echo "=== quic_token_mint_flood ==="
    RAN=$((RAN + 1))
    QUIC_FRAMEWORKS=""
    [ "$(uname)" = "Darwin" ] && QUIC_FRAMEWORKS="-framework CoreFoundation -framework Security"
    HOSTQ_BUILD="$UBQ_BUILD_ROOT/host-quic"
    HOSTQ_OBJDIR="$HOSTQ_BUILD/CMakeFiles/ubiquitty-host.dir"
    if cmake -B "$HOSTQ_BUILD" -S "$ROOT" -G "$UBQ_GENERATOR" -DUBIQUITTY_FRONTEND=Host -DCMAKE_BUILD_TYPE=Debug \
                -DUBIQUITTY_QUIC=ON >"$OUT/mintflood-host-build.log" 2>&1 \
            && cmake --build "$HOSTQ_BUILD" -j"$BUILD_JOBS" >>"$OUT/mintflood-host-build.log" 2>&1; then
        host_objects "$HOSTQ_OBJDIR" "$OUT/mintflood-objs.txt" \
            || build_broken "host object list is empty (the harness, not the test under it)"
        if ubq_compile_parallel "$OUT/quic_token_mint_flood" "$OUT/objects/quic_token_mint_flood" "$OUT/mintflood-build.log" \
                -std=c++23 -I"$ROOT/Tests" -DUBIQUITTY_QUIC=1 -I"$ROOT/Source" -I"$QUIC_PREFIX/include" -- \
                "$ROOT/Tests/Host/quic_token_mint_flood.cpp" \
                -- "@$OUT/mintflood-objs.txt" "$QUIC_PREFIX/lib/libngtcp2_crypto_boringssl.a" "$QUIC_PREFIX/lib/libngtcp2.a" "$QUIC_PREFIX/lib/libssl.a" "$QUIC_PREFIX/lib/libcrypto.a" $QUIC_FRAMEWORKS; then
            "$OUT/quic_token_mint_flood" || exit 1
        else
            build_broken "quic_token_mint_flood link failed (see $OUT/mintflood-build.log)"
        fi
    else
        build_broken "quic_token_mint_flood host build failed (see $OUT/mintflood-host-build.log)"
    fi
fi

# drop_defer — dropClient never erases m_clients inline. The live-wire
# wrong-client-reap race can't be driven black-box on Darwin (SHUT_RD no-EPIPE,
# first-write-to-closed-peer succeeds, read-EOF reaped by the index-safe service
# loop first), so this links the daemon objects and unit-tests the deferred-drop
# invariant directly via a friend seam.
if wanted drop_defer && command -v cmake >/dev/null 2>&1; then
    echo "=== drop_defer ==="
    RAN=$((RAN + 1))
    # Dedicated QUIC-OFF host build: the deferred-drop logic is QUIC-agnostic
    # (Daemon.cpp), and QUIC-OFF compiles HostQUIC.cpp as an empty TU so the
    # loose-object link needs no ngtcp2/AWS-LC. A separate dir avoids clobbering
    # build/host's config.
    HOST_BUILD="$UBQ_BUILD_ROOT/host-noquic"
    HOST_OBJDIR="$HOST_BUILD/CMakeFiles/ubiquitty-host.dir"
    if cmake -B "$HOST_BUILD" -S "$ROOT" -G "$UBQ_GENERATOR" -DUBIQUITTY_FRONTEND=Host -DCMAKE_BUILD_TYPE=Debug \
                \
                >"$OUT/dropdefer-host-build.log" 2>&1 \
            && cmake --build "$HOST_BUILD" -j"$BUILD_JOBS" >>"$OUT/dropdefer-host-build.log" 2>&1; then
        host_objects "$HOST_OBJDIR" "$OUT/dropdefer-objs.txt" \
            || build_broken "host object list is empty (the harness, not the test under it)"
        if ubq_compile_parallel "$OUT/drop_defer" "$OUT/objects/drop_defer" "$OUT/dropdefer-build.log" \
                -std=c++23 -DUBIQUITTY_QUIC=1 -I"$QUIC_PREFIX/include" -I"$ROOT/Tests" -I"$ROOT/Source" -- \
                "$ROOT/Tests/Host/drop_defer.cpp" \
                -- "@$OUT/dropdefer-objs.txt" "${HOST_LINK_ARCHIVES[@]}"; then
            "$OUT/drop_defer" || exit 1
        else
            build_broken "drop_defer link failed (see $OUT/dropdefer-build.log)"
        fi
    else
        build_broken "drop_defer host build failed (see $OUT/dropdefer-host-build.log)"
    fi
fi

# grant_persistence — grants that outlive the answer, and the pass that takes
# them back. A human answering "always" writes a grant key to disk; a token
# withdrawn from the store must disconnect the clients already holding it. The
# reverification pass walks the client table BACKWARDS on purpose, because
# denying a client can drop it and shift the rest down — asserted with several
# revoked clients at once, since one cannot tell the two directions apart.
# Friend seam, same QUIC-OFF host build as drop_defer.
if wanted grant_persistence && command -v cmake >/dev/null 2>&1; then
    echo "=== grant_persistence ==="
    RAN=$((RAN + 1))
    HOST_BUILD="$UBQ_BUILD_ROOT/host-noquic"
    HOST_OBJDIR="$HOST_BUILD/CMakeFiles/ubiquitty-host.dir"
    if cmake -B "$HOST_BUILD" -S "$ROOT" -G "$UBQ_GENERATOR" -DUBIQUITTY_FRONTEND=Host -DCMAKE_BUILD_TYPE=Debug \
                \
                >"$OUT/grants-host-build.log" 2>&1 \
            && cmake --build "$HOST_BUILD" -j"$BUILD_JOBS" >>"$OUT/grants-host-build.log" 2>&1; then
        host_objects "$HOST_OBJDIR" "$OUT/grants-objs.txt" \
            || build_broken "host object list is empty (the harness, not the test under it)"
        if ubq_compile_parallel "$OUT/grant_persistence" "$OUT/objects/grant_persistence" "$OUT/grants-build.log" \
                -std=c++23 -DUBIQUITTY_QUIC=1 -I"$QUIC_PREFIX/include" -I"$ROOT/Tests" -I"$ROOT/Source" -- \
                "$ROOT/Tests/Host/grant_persistence.cpp" \
                -- "@$OUT/grants-objs.txt" "${HOST_LINK_ARCHIVES[@]}"; then
            "$OUT/grant_persistence" || exit 1
        else
            build_broken "grant_persistence link failed (see $OUT/grants-build.log)"
        fi
    else
        build_broken "grant_persistence host build failed (see $OUT/grants-host-build.log)"
    fi
fi

# consent_answer_attach — canonical pending Welcome/status, exact requested
# recipients and consent.respond decisions. Standing grants bind the approved
# principal; missing Observe cannot be supplied by consent. The mutable-path
# non-persistence guard is explicitly policy-only, while token-bound persistence
# and write-failure fallback use actual pending promotion. Friend seam.
if wanted consent_answer_attach && command -v cmake >/dev/null 2>&1; then
    echo "=== consent_answer_attach ==="
    RAN=$((RAN + 1))
    HOST_BUILD="$UBQ_BUILD_ROOT/host-noquic"
    HOST_OBJDIR="$HOST_BUILD/CMakeFiles/ubiquitty-host.dir"
    if cmake -B "$HOST_BUILD" -S "$ROOT" -G "$UBQ_GENERATOR" -DUBIQUITTY_FRONTEND=Host -DCMAKE_BUILD_TYPE=Debug \
                \
                >"$OUT/consentanswer-host-build.log" 2>&1 \
            && cmake --build "$HOST_BUILD" -j"$BUILD_JOBS" >>"$OUT/consentanswer-host-build.log" 2>&1; then
        host_objects "$HOST_OBJDIR" "$OUT/consentanswer-objs.txt" \
            || build_broken "host object list is empty (the harness, not the test under it)"
        if ubq_compile_parallel "$OUT/consent_answer_attach" "$OUT/objects/consent_answer_attach" "$OUT/consentanswer-build.log" \
                -std=c++23 -DUBIQUITTY_QUIC=1 -I"$QUIC_PREFIX/include" -I"$ROOT/Tests" -I"$ROOT/Source" -- \
                "$ROOT/Tests/Host/consent_answer_attach.cpp" \
                -- "@$OUT/consentanswer-objs.txt" "${HOST_LINK_ARCHIVES[@]}"; then
            "$OUT/consent_answer_attach" || exit 1
        else
            build_broken "consent_answer_attach link failed (see $OUT/consentanswer-build.log)"
        fi
    else
        build_broken "consent_answer_attach host build failed (see $OUT/consentanswer-host-build.log)"
    fi
fi

# daemon_startup — the socket a daemon claims, and the sessions it agrees to
# make. One socket path names the daemon for every client on the machine, and
# two things have to be true at once: a socket file left by a daemon that
# crashed must not block the next start, and a socket a LIVE daemon is serving
# must not be taken from it. The oracle is a second daemon — a test asserting
# only the refusal would pass against one that refused after unlinking, which is
# the case that matters. Second
# half is the session registry's refusals: geometry off the wire, the 64-session
# cap, the sentinel id, an adopted descriptor that is not a pty, and a remote
# launch with no program. Friend seam, same QUIC-OFF host build as drop_defer.
if wanted daemon_startup && command -v cmake >/dev/null 2>&1; then
    echo "=== daemon_startup ==="
    RAN=$((RAN + 1))
    HOST_BUILD="$UBQ_BUILD_ROOT/host-noquic"
    HOST_OBJDIR="$HOST_BUILD/CMakeFiles/ubiquitty-host.dir"
    if cmake -B "$HOST_BUILD" -S "$ROOT" -G "$UBQ_GENERATOR" -DUBIQUITTY_FRONTEND=Host -DCMAKE_BUILD_TYPE=Debug \
                \
                >"$OUT/startup-host-build.log" 2>&1 \
            && cmake --build "$HOST_BUILD" -j"$BUILD_JOBS" >>"$OUT/startup-host-build.log" 2>&1; then
        host_objects "$HOST_OBJDIR" "$OUT/startup-objs.txt" \
            || build_broken "host object list is empty (the harness, not the test under it)"
        if ubq_compile_parallel "$OUT/daemon_startup" "$OUT/objects/daemon_startup" "$OUT/startup-build.log" \
                -std=c++23 -DUBIQUITTY_QUIC=1 -I"$QUIC_PREFIX/include" -I"$ROOT/Tests" -I"$ROOT/Source" -- \
                "$ROOT/Tests/Host/daemon_startup.cpp" \
                -- "@$OUT/startup-objs.txt" "${HOST_LINK_ARCHIVES[@]}"; then
            "$OUT/daemon_startup" || exit 1
        else
            build_broken "daemon_startup link failed (see $OUT/startup-build.log)"
        fi
    else
        build_broken "daemon_startup host build failed (see $OUT/startup-host-build.log)"
    fi
fi

# create_session_refusal — a session whose structure cannot be allocated is
# refused, and the daemon goes on. The Session record, the Terminal and the
# grid's buffers are nothrow allocations; the test replaces the nothrow operator
# new with one that grants a budget and refuses the rest, walks the budget up
# from zero, and checks every refusal leaves the registry empty until the first
# session made holds its geometry. Friend seam, same QUIC-OFF host build as
# daemon_startup.
if wanted create_session_refusal && command -v cmake >/dev/null 2>&1; then
    echo "=== create_session_refusal ==="
    RAN=$((RAN + 1))
    HOST_BUILD="$UBQ_BUILD_ROOT/host-noquic"
    HOST_OBJDIR="$HOST_BUILD/CMakeFiles/ubiquitty-host.dir"
    if cmake -B "$HOST_BUILD" -S "$ROOT" -G "$UBQ_GENERATOR" -DUBIQUITTY_FRONTEND=Host -DCMAKE_BUILD_TYPE=Debug \
                >"$OUT/create-session-refusal-host-build.log" 2>&1 \
            && cmake --build "$HOST_BUILD" -j"$BUILD_JOBS" >>"$OUT/create-session-refusal-host-build.log" 2>&1; then
        host_objects "$HOST_OBJDIR" "$OUT/create-session-refusal-objs.txt" \
            || build_broken "host object list is empty (the harness, not the test under it)"
        if ubq_compile_parallel "$OUT/create_session_refusal" "$OUT/objects/create_session_refusal" "$OUT/create-session-refusal-build.log" \
                -std=c++23 -DUBIQUITTY_QUIC=1 -I"$QUIC_PREFIX/include" -I"$ROOT/Tests" -I"$ROOT/Source" -- \
                "$ROOT/Tests/Host/create_session_refusal.cpp" \
                -- "@$OUT/create-session-refusal-objs.txt" "${HOST_LINK_ARCHIVES[@]}"; then
            "$OUT/create_session_refusal" || exit 1
        else
            build_broken "create_session_refusal link failed (see $OUT/create-session-refusal-build.log)"
        fi
    else
        build_broken "create_session_refusal host build failed (see $OUT/create-session-refusal-host-build.log)"
    fi
fi

# session_sized_text — the daemon's terminal draws sized text, so a
# proportional run and a sized block reach a mirror as the model rather than
# as plain text. Friend seam, same QUIC-OFF host build as create_session_refusal.
if wanted session_sized_text && command -v cmake >/dev/null 2>&1; then
    echo "=== session_sized_text ==="
    RAN=$((RAN + 1))
    HOST_BUILD="$UBQ_BUILD_ROOT/host-noquic"
    HOST_OBJDIR="$HOST_BUILD/CMakeFiles/ubiquitty-host.dir"
    if cmake -B "$HOST_BUILD" -S "$ROOT" -G "$UBQ_GENERATOR" -DUBIQUITTY_FRONTEND=Host -DCMAKE_BUILD_TYPE=Debug \
                >"$OUT/session-sized-text-host-build.log" 2>&1 \
            && cmake --build "$HOST_BUILD" -j"$BUILD_JOBS" >>"$OUT/session-sized-text-host-build.log" 2>&1; then
        host_objects "$HOST_OBJDIR" "$OUT/session-sized-text-objs.txt" \
            || build_broken "host object list is empty (the harness, not the test under it)"
        if ubq_compile_parallel "$OUT/session_sized_text" "$OUT/objects/session_sized_text" "$OUT/session-sized-text-build.log" \
                -std=c++23 -DUBIQUITTY_QUIC=1 -I"$QUIC_PREFIX/include" -I"$ROOT/Tests" -I"$ROOT/Source" -- \
                "$ROOT/Tests/Host/session_sized_text.cpp" \
                -- "@$OUT/session-sized-text-objs.txt" "${HOST_LINK_ARCHIVES[@]}"; then
            "$OUT/session_sized_text" || exit 1
        else
            build_broken "session_sized_text link failed (see $OUT/session-sized-text-build.log)"
        fi
    else
        build_broken "session_sized_text host build failed (see $OUT/session-sized-text-host-build.log)"
    fi
fi

# slow_client_withhold — a client that cannot drain the session's frames is
# queued none rather than dropped: while its output holds a frame its screen
# connection encodes nothing, and the first service after it drains brings it
# to the newest frame in one update. Friend seam, same QUIC-OFF host build as
# create_session_refusal.
if wanted slow_client_withhold && command -v cmake >/dev/null 2>&1; then
    echo "=== slow_client_withhold ==="
    RAN=$((RAN + 1))
    HOST_BUILD="$UBQ_BUILD_ROOT/host-noquic"
    HOST_OBJDIR="$HOST_BUILD/CMakeFiles/ubiquitty-host.dir"
    if cmake -B "$HOST_BUILD" -S "$ROOT" -G "$UBQ_GENERATOR" -DUBIQUITTY_FRONTEND=Host -DCMAKE_BUILD_TYPE=Debug \
                >"$OUT/slow-client-withhold-host-build.log" 2>&1 \
            && cmake --build "$HOST_BUILD" -j"$BUILD_JOBS" >>"$OUT/slow-client-withhold-host-build.log" 2>&1; then
        host_objects "$HOST_OBJDIR" "$OUT/slow-client-withhold-objs.txt" \
            || build_broken "host object list is empty (the harness, not the test under it)"
        if ubq_compile_parallel "$OUT/slow_client_withhold" "$OUT/objects/slow_client_withhold" "$OUT/slow-client-withhold-build.log" \
                -std=c++23 -DUBIQUITTY_QUIC=1 -I"$QUIC_PREFIX/include" -I"$ROOT/Tests" -I"$ROOT/Source" -- \
                "$ROOT/Tests/Host/slow_client_withhold.cpp" \
                -- "@$OUT/slow-client-withhold-objs.txt" "${HOST_LINK_ARCHIVES[@]}"; then
            ubq_run_bounded 300 "$OUT/slow_client_withhold" || exit 1
        else
            build_broken "slow_client_withhold link failed (see $OUT/slow-client-withhold-build.log)"
        fi
    else
        build_broken "slow_client_withhold host build failed (see $OUT/slow-client-withhold-host-build.log)"
    fi
fi

# handover_quiesce — the daemon holds client input while every pane reaches a
# fresh coherent parser boundary; any refusal resumes the complete host.
if wanted handover_quiesce && command -v cmake >/dev/null 2>&1; then
    echo "=== handover_quiesce ==="
    RAN=$((RAN + 1))
    if ubq_host_binary "$OUT/handover-quiesce-host-build.log" >/dev/null; then
        host_objects "$HOST_TEST_BUILD/CMakeFiles/ubiquitty-host.dir" "$OUT/handover-quiesce-objs.txt" \
            || build_broken "host object list is empty (the harness, not the test under it)"
        if ubq_compile_parallel "$OUT/handover_quiesce" "$OUT/objects/handover_quiesce" "$OUT/handover-quiesce-build.log" \
                -std=c++23 -DUBIQUITTY_QUIC=1 -I"$QUIC_PREFIX/include" -I"$ROOT/Tests" -I"$ROOT/Source" -- \
                "$ROOT/Tests/Host/handover_quiesce.cpp" \
                -- "@$OUT/handover-quiesce-objs.txt" "${HOST_LINK_ARCHIVES[@]}"; then
            ubq_run_bounded 300 "$OUT/handover_quiesce" || exit 1
        else
            build_broken "handover_quiesce link failed (see $OUT/handover-quiesce-build.log)"
        fi
    else
        build_broken "handover_quiesce host build failed (see $OUT/handover-quiesce-host-build.log)"
    fi
fi

# Local handover routing must reject unadmitted peers before the physical
# ownership transaction. The test supplies refusing transaction sentinels.
if wanted handover_startup && command -v cmake >/dev/null 2>&1; then
    echo "=== handover_startup ==="
    RAN=$((RAN + 1))
    if ubq_host_binary "$OUT/handover-startup-host-build.log" >/dev/null; then
        host_objects "$HOST_TEST_BUILD/CMakeFiles/ubiquitty-host.dir" "$OUT/handover-startup-objs.txt" 'HostHandover\.cpp\.o' 'HandoverStartup\.cpp\.o' \
            || build_broken "host object list is empty (the harness, not the test under it)"
        if ubq_compile_parallel "$OUT/handover_startup" "$OUT/objects/handover_startup" "$OUT/handover-startup-build.log" \
                -std=c++23 -DUBIQUITTY_QUIC=1 -I"$QUIC_PREFIX/include" -I"$ROOT/Tests" -I"$ROOT/Source" -- \
                "$ROOT/Tests/Host/handover_startup.cpp" \
                "$ROOT/Source/Platform/Host/HandoverStartup.cpp" \
                -- "@$OUT/handover-startup-objs.txt" "${HOST_LINK_ARCHIVES[@]}"; then
            ubq_run_bounded 60 "$OUT/handover_startup" || exit 1
        else
            build_broken "handover_startup link failed (see $OUT/handover-startup-build.log)"
        fi
    else
        build_broken "handover_startup host build failed (see $OUT/handover-startup-host-build.log)"
    fi
fi

# two_client_pacing — a slow client never stalls a fast one. Two clients on
# one session, each with its own Sync connection: the one that reads holds
# the host's screen on every frame while the one that does not is queued at
# most one frame and owed the rest, and reaches the newest frame in one update
# once it reads. Friend seam, same QUIC-OFF host build as slow_client_withhold.
if wanted two_client_pacing && command -v cmake >/dev/null 2>&1; then
    echo "=== two_client_pacing ==="
    RAN=$((RAN + 1))
    HOST_BUILD="$UBQ_BUILD_ROOT/host-noquic"
    HOST_OBJDIR="$HOST_BUILD/CMakeFiles/ubiquitty-host.dir"
    if cmake -B "$HOST_BUILD" -S "$ROOT" -G "$UBQ_GENERATOR" -DUBIQUITTY_FRONTEND=Host -DCMAKE_BUILD_TYPE=Debug \
                >"$OUT/two-client-pacing-host-build.log" 2>&1 \
            && cmake --build "$HOST_BUILD" -j"$BUILD_JOBS" >>"$OUT/two-client-pacing-host-build.log" 2>&1; then
        host_objects "$HOST_OBJDIR" "$OUT/two-client-pacing-objs.txt" \
            || build_broken "host object list is empty (the harness, not the test under it)"
        if ubq_compile_parallel "$OUT/two_client_pacing" "$OUT/objects/two_client_pacing" "$OUT/two-client-pacing-build.log" \
                -std=c++23 -DUBIQUITTY_QUIC=1 -I"$QUIC_PREFIX/include" -I"$ROOT/Tests" -I"$ROOT/Source" -- \
                "$ROOT/Tests/Host/two_client_pacing.cpp" \
                -- "@$OUT/two-client-pacing-objs.txt" "${HOST_LINK_ARCHIVES[@]}"; then
            "$OUT/two_client_pacing" || exit 1
        else
            build_broken "two_client_pacing link failed (see $OUT/two-client-pacing-build.log)"
        fi
    else
        build_broken "two_client_pacing host build failed (see $OUT/two-client-pacing-host-build.log)"
    fi
fi

# sync_stress_mix — seeded attach/resume, geometry, image replacement, sized
# text, dormant palette and input churn through the daemon's Sync harness.
if wanted sync_stress_mix && command -v cmake >/dev/null 2>&1; then
    echo "=== sync_stress_mix ==="
    RAN=$((RAN + 1))
    HOST_BUILD="$UBQ_BUILD_ROOT/host-noquic"
    HOST_OBJDIR="$HOST_BUILD/CMakeFiles/ubiquitty-host.dir"
    if cmake -B "$HOST_BUILD" -S "$ROOT" -G "$UBQ_GENERATOR" -DUBIQUITTY_FRONTEND=Host -DCMAKE_BUILD_TYPE=Debug \
                >"$OUT/sync-stress-mix-host-build.log" 2>&1 \
            && cmake --build "$HOST_BUILD" -j"$BUILD_JOBS" >>"$OUT/sync-stress-mix-host-build.log" 2>&1; then
        host_objects "$HOST_OBJDIR" "$OUT/sync-stress-mix-objs.txt" \
            || build_broken "host object list is empty (the harness, not the test under it)"
        if ubq_compile_parallel "$OUT/sync_stress_mix" "$OUT/objects/sync_stress_mix" "$OUT/sync-stress-mix-build.log" \
                -std=c++23 -Wall -Wextra -Werror -DUBIQUITTY_QUIC=1 -I"$QUIC_PREFIX/include" -I"$ROOT/Tests" -I"$ROOT/Source" -- \
                "$ROOT/Tests/Host/sync_stress_mix.cpp" \
                -- "@$OUT/sync-stress-mix-objs.txt" "${HOST_LINK_ARCHIVES[@]}"; then
            ubq_run_bounded 360 "$OUT/sync_stress_mix" \
                --seconds "${UBQ_SYNC_STRESS_SECONDS:-60}" --seed "${UBQ_SYNC_STRESS_SEED:-1}" \
                --clients "${UBQ_SYNC_STRESS_CLIENTS:-4}" --rss-mib "${UBQ_SYNC_STRESS_RSS_MIB:-64}" || exit 1
        else
            build_broken "sync_stress_mix link failed (see $OUT/sync-stress-mix-build.log)"
        fi
    else
        build_broken "sync_stress_mix host build failed (see $OUT/sync-stress-mix-host-build.log)"
    fi
fi

# history_plane — the daemon serves the history plane: a resize through its own
# path sends no history byte, the clear-buffer verb and ED 3 each send one
# HistoryReset and purge the client, ten thousand wants leave its memory and
# queued bulk flat, and a metered client is pushed one viewport. Then a live
# daemon carries a want up and the history and the reset down its socket.
# Friend seam, same QUIC-OFF host build as two_client_pacing.
if wanted history_plane && command -v cmake >/dev/null 2>&1; then
    echo "=== history_plane ==="
    RAN=$((RAN + 1))
    HOST_BUILD="$UBQ_BUILD_ROOT/host-noquic"
    HOST_OBJDIR="$HOST_BUILD/CMakeFiles/ubiquitty-host.dir"
    if cmake -B "$HOST_BUILD" -S "$ROOT" -G "$UBQ_GENERATOR" -DUBIQUITTY_FRONTEND=Host -DCMAKE_BUILD_TYPE=Debug \
                >"$OUT/history-plane-host-build.log" 2>&1 \
            && cmake --build "$HOST_BUILD" -j"$BUILD_JOBS" >>"$OUT/history-plane-host-build.log" 2>&1; then
        host_objects "$HOST_OBJDIR" "$OUT/history-plane-objs.txt" \
            || build_broken "host object list is empty (the harness, not the test under it)"
        if ubq_compile_parallel "$OUT/history_plane" "$OUT/objects/history_plane" "$OUT/history-plane-build.log" \
                -std=c++23 -DUBIQUITTY_QUIC=1 -I"$QUIC_PREFIX/include" -I"$ROOT/Tests" -I"$ROOT/Source" -- \
                "$ROOT/Tests/Host/history_plane.cpp" \
                -- "@$OUT/history-plane-objs.txt" "${HOST_LINK_ARCHIVES[@]}"; then
            ubq_run_private "$OUT/history_plane" "$HOST_BUILD/ubiquitty-host" || exit 1
        else
            build_broken "history_plane link failed (see $OUT/history-plane-build.log)"
        fi
    else
        build_broken "history_plane host build failed (see $OUT/history-plane-host-build.log)"
    fi
fi

# interrupt_under_flood — a client types lines ahead into a pane that floods
# and reads nothing, falls three seconds behind, sends ^C and reads on slowly;
# the flooding program has ended within two seconds of the key. A live daemon
# over its socket, where the terminal's full input queue and the daemon's hold
# on the client's input each kept the key from the program. Friend-seam link,
# same QUIC-OFF host build as history_plane.
if wanted interrupt_under_flood && command -v cmake >/dev/null 2>&1; then
    echo "=== interrupt_under_flood ==="
    RAN=$((RAN + 1))
    HOST_BUILD="$UBQ_BUILD_ROOT/host-noquic"
    HOST_OBJDIR="$HOST_BUILD/CMakeFiles/ubiquitty-host.dir"
    if cmake -B "$HOST_BUILD" -S "$ROOT" -G "$UBQ_GENERATOR" -DUBIQUITTY_FRONTEND=Host -DCMAKE_BUILD_TYPE=Debug \
                >"$OUT/interrupt-under-flood-host-build.log" 2>&1 \
            && cmake --build "$HOST_BUILD" -j"$BUILD_JOBS" >>"$OUT/interrupt-under-flood-host-build.log" 2>&1; then
        host_objects "$HOST_OBJDIR" "$OUT/interrupt-under-flood-objs.txt" \
            || build_broken "host object list is empty (the harness, not the test under it)"
        if ubq_compile_parallel "$OUT/interrupt_under_flood" "$OUT/objects/interrupt_under_flood" "$OUT/interrupt-under-flood-build.log" \
                -std=c++23 -DUBIQUITTY_QUIC=1 -I"$QUIC_PREFIX/include" -I"$ROOT/Tests" -I"$ROOT/Source" -- \
                "$ROOT/Tests/Host/interrupt_under_flood.cpp" \
                -- "@$OUT/interrupt-under-flood-objs.txt" "${HOST_LINK_ARCHIVES[@]}"; then
            ubq_run_private "$OUT/interrupt_under_flood" "$HOST_BUILD/ubiquitty-host" || exit 1
        else
            build_broken "interrupt_under_flood link failed (see $OUT/interrupt-under-flood-build.log)"
        fi
    else
        build_broken "interrupt_under_flood host build failed (see $OUT/interrupt-under-flood-host-build.log)"
    fi
fi

# capture_flood_busy — a client that floods screen then whole-buffer captures,
# and reads nothing back draws its screen plane's call limits' worth of answers
# and busy for the rest,
# the connection holds under its queued-bulk limit, and the peak RSS stays flat
# as the count grows. Friend seam, same QUIC-OFF host build as two_client_pacing.
if wanted capture_flood_busy && command -v cmake >/dev/null 2>&1; then
    echo "=== capture_flood_busy ==="
    RAN=$((RAN + 1))
    HOST_BUILD="$UBQ_BUILD_ROOT/host-noquic"
    HOST_OBJDIR="$HOST_BUILD/CMakeFiles/ubiquitty-host.dir"
    if cmake -B "$HOST_BUILD" -S "$ROOT" -G "$UBQ_GENERATOR" -DUBIQUITTY_FRONTEND=Host -DCMAKE_BUILD_TYPE=Debug \
                >"$OUT/capture-flood-busy-host-build.log" 2>&1 \
            && cmake --build "$HOST_BUILD" -j"$BUILD_JOBS" >>"$OUT/capture-flood-busy-host-build.log" 2>&1; then
        host_objects "$HOST_OBJDIR" "$OUT/capture-flood-busy-objs.txt" \
            || build_broken "host object list is empty (the harness, not the test under it)"
        if ubq_compile_parallel "$OUT/capture_flood_busy" "$OUT/objects/capture_flood_busy" "$OUT/capture-flood-busy-build.log" \
                -std=c++23 -DUBIQUITTY_QUIC=1 -I"$QUIC_PREFIX/include" -I"$ROOT/Tests" -I"$ROOT/Source" -- \
                "$ROOT/Tests/Host/capture_flood_busy.cpp" \
                -- "@$OUT/capture-flood-busy-objs.txt" "${HOST_LINK_ARCHIVES[@]}"; then
            ubq_run_private "$OUT/capture_flood_busy" || exit 1
        else
            build_broken "capture_flood_busy link failed (see $OUT/capture-flood-busy-build.log)"
        fi
    else
        build_broken "capture_flood_busy host build failed (see $OUT/capture-flood-busy-host-build.log)"
    fi
fi

# screen_lost_silence — a client silent for 30 s with bytes outstanding is
# dropped and its session kept, an idle client in the background survives ten
# minutes, and the client after the dropped one is serviced in the same pass.
# Friend seam, same QUIC-OFF host build as two_client_pacing.
if wanted screen_lost_silence && command -v cmake >/dev/null 2>&1; then
    echo "=== screen_lost_silence ==="
    RAN=$((RAN + 1))
    HOST_BUILD="$UBQ_BUILD_ROOT/host-noquic"
    HOST_OBJDIR="$HOST_BUILD/CMakeFiles/ubiquitty-host.dir"
    if cmake -B "$HOST_BUILD" -S "$ROOT" -G "$UBQ_GENERATOR" -DUBIQUITTY_FRONTEND=Host -DCMAKE_BUILD_TYPE=Debug \
                >"$OUT/screen-lost-silence-host-build.log" 2>&1 \
            && cmake --build "$HOST_BUILD" -j"$BUILD_JOBS" >>"$OUT/screen-lost-silence-host-build.log" 2>&1; then
        host_objects "$HOST_OBJDIR" "$OUT/screen-lost-silence-objs.txt" \
            || build_broken "host object list is empty (the harness, not the test under it)"
        if ubq_compile_parallel "$OUT/screen_lost_silence" "$OUT/objects/screen_lost_silence" "$OUT/screen-lost-silence-build.log" \
                -std=c++23 -DUBIQUITTY_QUIC=1 -I"$QUIC_PREFIX/include" -I"$ROOT/Tests" -I"$ROOT/Source" -- \
                "$ROOT/Tests/Host/screen_lost_silence.cpp" \
                -- "@$OUT/screen-lost-silence-objs.txt" "${HOST_LINK_ARCHIVES[@]}"; then
            ubq_run_bounded 300 "$OUT/screen_lost_silence" || exit 1
        else
            build_broken "screen_lost_silence link failed (see $OUT/screen-lost-silence-build.log)"
        fi
    else
        build_broken "screen_lost_silence host build failed (see $OUT/screen-lost-silence-host-build.log)"
    fi
fi

# resize_storm — a storm of Viewports in one read pass costs one reflow, one
# SIGWINCH and one checkpoint, and input behind a Viewport reaches a program
# that was told of it. Friend seam, same QUIC-OFF host build as two_client_pacing.
if wanted resize_storm && command -v cmake >/dev/null 2>&1; then
    echo "=== resize_storm ==="
    RAN=$((RAN + 1))
    HOST_BUILD="$UBQ_BUILD_ROOT/host-noquic"
    HOST_OBJDIR="$HOST_BUILD/CMakeFiles/ubiquitty-host.dir"
    if cmake -B "$HOST_BUILD" -S "$ROOT" -G "$UBQ_GENERATOR" -DUBIQUITTY_FRONTEND=Host -DCMAKE_BUILD_TYPE=Debug \
                >"$OUT/resize-storm-host-build.log" 2>&1 \
            && cmake --build "$HOST_BUILD" -j"$BUILD_JOBS" >>"$OUT/resize-storm-host-build.log" 2>&1; then
        host_objects "$HOST_OBJDIR" "$OUT/resize-storm-objs.txt" \
            || build_broken "host object list is empty (the harness, not the test under it)"
        if ubq_compile_parallel "$OUT/resize_storm" "$OUT/objects/resize_storm" "$OUT/resize-storm-build.log" \
                -std=c++23 -DUBIQUITTY_QUIC=1 -I"$QUIC_PREFIX/include" -I"$ROOT/Tests" -I"$ROOT/Source" -- \
                "$ROOT/Tests/Host/resize_storm.cpp" \
                -- "@$OUT/resize-storm-objs.txt" "${HOST_LINK_ARCHIVES[@]}"; then
            "$OUT/resize_storm" || exit 1
        else
            build_broken "resize_storm link failed (see $OUT/resize-storm-build.log)"
        fi
    else
        build_broken "resize_storm host build failed (see $OUT/resize-storm-host-build.log)"
    fi
fi

# echo_delay_measure — the write-to-echo delay of real programs on a PTY, which
# sets the echo timer E (Docs/sync-measurements.md, "Input and liveness").
# Here it is a smoke, the kernel's echo answering cat's PTY; with
# UBQ_SYNC_CORPUS_REPORT=1 it loads the machine and prints the recorded table.
if wanted echo_delay_measure; then
    echo "=== echo_delay_measure ==="
    ECHO_DELAY_LINK=()
    # forkpty lives in libutil on Linux; macOS has it in libSystem.
    [ "$(uname -s)" = Linux ] && ECHO_DELAY_LINK=(-lutil)
    if ubq_compile_parallel "$OUT/echo_delay_measure" "$OUT/objects/echo_delay_measure" "$OUT/echo-delay-measure-build.log" \
            -std=c++23 -I"$ROOT/Tests" -I"$ROOT/Source" -Wall -Wextra -Werror -fno-exceptions -fno-rtti -O1 -- \
            "$ROOT/Tests/Host/echo_delay_measure.cpp" \
            -- ${ECHO_DELAY_LINK[@]+"${ECHO_DELAY_LINK[@]}"}; then
        ubq_run_private "$OUT/echo_delay_measure" || exit 1
        RAN=$((RAN + 1))
    else
        cat "$OUT/echo-delay-measure-build.log" >&2
        build_broken "echo_delay_measure build failed (see $OUT/echo-delay-measure-build.log)"
    fi
fi

# input_mode_flip — a remote client's key, refused while the program does not
# read, is encoded under the modes live when the daemon writes it: DECCKM set
# in between makes it ESC O A. A client without the Drive scope sending a key
# is closed. Friend seam, same QUIC-OFF host build as two_client_pacing.
if wanted input_mode_flip && command -v cmake >/dev/null 2>&1; then
    echo "=== input_mode_flip ==="
    RAN=$((RAN + 1))
    HOST_BUILD="$UBQ_BUILD_ROOT/host-noquic"
    HOST_OBJDIR="$HOST_BUILD/CMakeFiles/ubiquitty-host.dir"
    if cmake -B "$HOST_BUILD" -S "$ROOT" -G "$UBQ_GENERATOR" -DUBIQUITTY_FRONTEND=Host -DCMAKE_BUILD_TYPE=Debug \
                >"$OUT/input-mode-flip-host-build.log" 2>&1 \
            && cmake --build "$HOST_BUILD" -j"$BUILD_JOBS" >>"$OUT/input-mode-flip-host-build.log" 2>&1; then
        host_objects "$HOST_OBJDIR" "$OUT/input-mode-flip-objs.txt" \
            || build_broken "host object list is empty (the harness, not the test under it)"
        if ubq_compile_parallel "$OUT/input_mode_flip" "$OUT/objects/input_mode_flip" "$OUT/input-mode-flip-build.log" \
                -std=c++23 -DUBIQUITTY_QUIC=1 -I"$QUIC_PREFIX/include" -I"$ROOT/Tests" -I"$ROOT/Source" -- \
                "$ROOT/Tests/Host/input_mode_flip.cpp" \
                -- "@$OUT/input-mode-flip-objs.txt" "${HOST_LINK_ARCHIVES[@]}"; then
            "$OUT/input_mode_flip" || exit 1
        else
            build_broken "input_mode_flip link failed (see $OUT/input-mode-flip-build.log)"
        fi
    else
        build_broken "input_mode_flip host build failed (see $OUT/input-mode-flip-host-build.log)"
    fi
fi

# input_settled_echo — a real program reads a key, sleeps 30 ms and echoes it;
# no frame the daemon sends reports the key settled before it holds the echo,
# and the key does settle (Docs/sync.md §6.7). Friend seam, same QUIC-OFF host
# build as two_client_pacing.
if wanted input_settled_echo && command -v cmake >/dev/null 2>&1; then
    echo "=== input_settled_echo ==="
    RAN=$((RAN + 1))
    HOST_BUILD="$UBQ_BUILD_ROOT/host-noquic"
    HOST_OBJDIR="$HOST_BUILD/CMakeFiles/ubiquitty-host.dir"
    if cmake -B "$HOST_BUILD" -S "$ROOT" -G "$UBQ_GENERATOR" -DUBIQUITTY_FRONTEND=Host -DCMAKE_BUILD_TYPE=Debug \
                >"$OUT/input-settled-echo-host-build.log" 2>&1 \
            && cmake --build "$HOST_BUILD" -j"$BUILD_JOBS" >>"$OUT/input-settled-echo-host-build.log" 2>&1; then
        host_objects "$HOST_OBJDIR" "$OUT/input-settled-echo-objs.txt" \
            || build_broken "host object list is empty (the harness, not the test under it)"
        if ubq_compile_parallel "$OUT/input_settled_echo" "$OUT/objects/input_settled_echo" "$OUT/input-settled-echo-build.log" \
                -std=c++23 -DUBIQUITTY_QUIC=1 -I"$QUIC_PREFIX/include" -I"$ROOT/Tests" -I"$ROOT/Source" -- \
                "$ROOT/Tests/Host/input_settled_echo.cpp" \
                -- "@$OUT/input-settled-echo-objs.txt" "${HOST_LINK_ARCHIVES[@]}"; then
            "$OUT/input_settled_echo" || exit 1
        else
            build_broken "input_settled_echo link failed (see $OUT/input-settled-echo-build.log)"
        fi
    else
        build_broken "input_settled_echo host build failed (see $OUT/input-settled-echo-host-build.log)"
    fi
fi

# resume_swap — a resume on a second connection continues the attachment with
# a diff and closes the first with Superseded; another token's resume is
# refused; a lost attachment is forgotten past its grace. Friend seam, same
# QUIC-OFF host build as screen_lost_silence.
if wanted resume_swap && command -v cmake >/dev/null 2>&1; then
    echo "=== resume_swap ==="
    RAN=$((RAN + 1))
    HOST_BUILD="$UBQ_BUILD_ROOT/host-noquic"
    HOST_OBJDIR="$HOST_BUILD/CMakeFiles/ubiquitty-host.dir"
    if cmake -B "$HOST_BUILD" -S "$ROOT" -G "$UBQ_GENERATOR" -DUBIQUITTY_FRONTEND=Host -DCMAKE_BUILD_TYPE=Debug \
                >"$OUT/resume-swap-host-build.log" 2>&1 \
            && cmake --build "$HOST_BUILD" -j"$BUILD_JOBS" >>"$OUT/resume-swap-host-build.log" 2>&1; then
        host_objects "$HOST_OBJDIR" "$OUT/resume-swap-objs.txt" \
            || build_broken "host object list is empty (the harness, not the test under it)"
        if ubq_compile_parallel "$OUT/resume_swap" "$OUT/objects/resume_swap" "$OUT/resume-swap-build.log" \
                -std=c++23 -DUBIQUITTY_QUIC=1 -I"$QUIC_PREFIX/include" -I"$ROOT/Tests" -I"$ROOT/Source" -- \
                "$ROOT/Tests/Host/resume_swap.cpp" \
                -- "@$OUT/resume-swap-objs.txt" "${HOST_LINK_ARCHIVES[@]}"; then
            ubq_run_bounded 300 "$OUT/resume_swap" || exit 1
        else
            build_broken "resume_swap link failed (see $OUT/resume-swap-build.log)"
        fi
    else
        build_broken "resume_swap host build failed (see $OUT/resume-swap-host-build.log)"
    fi
fi

# move_reconnect — a client whose pane another client moved into a window of
# its own while its link was down resumes the same session, pane and
# workspace, and is continued with a diff into the host's picture. Friend
# seam, same QUIC-OFF host build as resume_swap.
if wanted move_reconnect && command -v cmake >/dev/null 2>&1; then
    echo "=== move_reconnect ==="
    RAN=$((RAN + 1))
    HOST_BUILD="$UBQ_BUILD_ROOT/host-noquic"
    HOST_OBJDIR="$HOST_BUILD/CMakeFiles/ubiquitty-host.dir"
    if cmake -B "$HOST_BUILD" -S "$ROOT" -G "$UBQ_GENERATOR" -DUBIQUITTY_FRONTEND=Host -DCMAKE_BUILD_TYPE=Debug \
                >"$OUT/move-reconnect-host-build.log" 2>&1 \
            && cmake --build "$HOST_BUILD" -j"$BUILD_JOBS" >>"$OUT/move-reconnect-host-build.log" 2>&1; then
        host_objects "$HOST_OBJDIR" "$OUT/move-reconnect-objs.txt" \
            || build_broken "host object list is empty (the harness, not the test under it)"
        if ubq_compile_parallel "$OUT/move_reconnect" "$OUT/objects/move_reconnect" "$OUT/move-reconnect-build.log" \
                -std=c++23 -DUBIQUITTY_QUIC=1 -I"$QUIC_PREFIX/include" -I"$ROOT/Tests" -I"$ROOT/Source" -- \
                "$ROOT/Tests/Host/move_reconnect.cpp" \
                -- "@$OUT/move-reconnect-objs.txt" "${HOST_LINK_ARCHIVES[@]}"; then
            ubq_run_bounded 300 "$OUT/move_reconnect" || exit 1
        else
            build_broken "move_reconnect link failed (see $OUT/move-reconnect-build.log)"
        fi
    else
        build_broken "move_reconnect host build failed (see $OUT/move-reconnect-host-build.log)"
    fi
fi

# drop_under_load — a client dropped mid-flood resumes into the host's
# picture, resized or not, and a person dropped while an agent's change waits
# for their approval is asked again on the connection they resume. Friend
# seam, same QUIC-OFF host build as resume_swap.
if wanted drop_under_load && command -v cmake >/dev/null 2>&1; then
    echo "=== drop_under_load ==="
    RAN=$((RAN + 1))
    HOST_BUILD="$UBQ_BUILD_ROOT/host-noquic"
    HOST_OBJDIR="$HOST_BUILD/CMakeFiles/ubiquitty-host.dir"
    if cmake -B "$HOST_BUILD" -S "$ROOT" -G "$UBQ_GENERATOR" -DUBIQUITTY_FRONTEND=Host -DCMAKE_BUILD_TYPE=Debug \
                >"$OUT/drop-under-load-host-build.log" 2>&1 \
            && cmake --build "$HOST_BUILD" -j"$BUILD_JOBS" >>"$OUT/drop-under-load-host-build.log" 2>&1; then
        host_objects "$HOST_OBJDIR" "$OUT/drop-under-load-objs.txt" \
            || build_broken "host object list is empty (the harness, not the test under it)"
        if ubq_compile_parallel "$OUT/drop_under_load" "$OUT/objects/drop_under_load" "$OUT/drop-under-load-build.log" \
                -std=c++23 -DUBIQUITTY_QUIC=1 -I"$QUIC_PREFIX/include" -I"$ROOT/Tests" -I"$ROOT/Source" -- \
                "$ROOT/Tests/Host/drop_under_load.cpp" \
                -- "@$OUT/drop-under-load-objs.txt" "${HOST_LINK_ARCHIVES[@]}"; then
            ubq_run_bounded 300 "$OUT/drop_under_load" || exit 1
        else
            build_broken "drop_under_load link failed (see $OUT/drop-under-load-build.log)"
        fi
    else
        build_broken "drop_under_load host build failed (see $OUT/drop-under-load-host-build.log)"
    fi
fi

# several_clients — four clients reading at their own pace through a flood,
# one dropped and resumed, all hold the host's picture; of two answers to one
# tree question one is served and decides; a client that may only observe is
# never asked. Friend seam, same QUIC-OFF host build as resume_swap.
if wanted several_clients && command -v cmake >/dev/null 2>&1; then
    echo "=== several_clients ==="
    RAN=$((RAN + 1))
    HOST_BUILD="$UBQ_BUILD_ROOT/host-noquic"
    HOST_OBJDIR="$HOST_BUILD/CMakeFiles/ubiquitty-host.dir"
    if cmake -B "$HOST_BUILD" -S "$ROOT" -G "$UBQ_GENERATOR" -DUBIQUITTY_FRONTEND=Host -DCMAKE_BUILD_TYPE=Debug \
                >"$OUT/several-clients-host-build.log" 2>&1 \
            && cmake --build "$HOST_BUILD" -j"$BUILD_JOBS" >>"$OUT/several-clients-host-build.log" 2>&1; then
        host_objects "$HOST_OBJDIR" "$OUT/several-clients-objs.txt" \
            || build_broken "host object list is empty (the harness, not the test under it)"
        if ubq_compile_parallel "$OUT/several_clients" "$OUT/objects/several_clients" "$OUT/several-clients-build.log" \
                -std=c++23 -DUBIQUITTY_QUIC=1 -I"$QUIC_PREFIX/include" -I"$ROOT/Tests" -I"$ROOT/Source" -- \
                "$ROOT/Tests/Host/several_clients.cpp" \
                -- "@$OUT/several-clients-objs.txt" "${HOST_LINK_ARCHIVES[@]}"; then
            ubq_run_bounded 300 "$OUT/several_clients" || exit 1
        else
            build_broken "several_clients link failed (see $OUT/several-clients-build.log)"
        fi
    else
        build_broken "several_clients host build failed (see $OUT/several-clients-host-build.log)"
    fi
fi

# hostile_clients — clients that break the protocol's manners on purpose:
# input past the pane's bounds, calls whose answers are never read, and a
# resume naming the most panes a greeting carries. Friend seam, same QUIC-OFF
# host build as resume_swap.
if wanted hostile_clients && command -v cmake >/dev/null 2>&1; then
    echo "=== hostile_clients ==="
    RAN=$((RAN + 1))
    HOST_BUILD="$UBQ_BUILD_ROOT/host-noquic"
    HOST_OBJDIR="$HOST_BUILD/CMakeFiles/ubiquitty-host.dir"
    if cmake -B "$HOST_BUILD" -S "$ROOT" -G "$UBQ_GENERATOR" -DUBIQUITTY_FRONTEND=Host -DCMAKE_BUILD_TYPE=Debug \
                >"$OUT/hostile-clients-host-build.log" 2>&1 \
            && cmake --build "$HOST_BUILD" -j"$BUILD_JOBS" >>"$OUT/hostile-clients-host-build.log" 2>&1; then
        host_objects "$HOST_OBJDIR" "$OUT/hostile-clients-objs.txt" \
            || build_broken "host object list is empty (the harness, not the test under it)"
        if ubq_compile_parallel "$OUT/hostile_clients" "$OUT/objects/hostile_clients" "$OUT/hostile-clients-build.log" \
                -std=c++23 -DUBIQUITTY_QUIC=1 -I"$QUIC_PREFIX/include" -I"$ROOT/Tests" -I"$ROOT/Source" -- \
                "$ROOT/Tests/Host/hostile_clients.cpp" \
                -- "@$OUT/hostile-clients-objs.txt" "${HOST_LINK_ARCHIVES[@]}"; then
            ubq_run_bounded 300 "$OUT/hostile_clients" || exit 1
        else
            build_broken "hostile_clients link failed (see $OUT/hostile-clients-build.log)"
        fi
    else
        build_broken "hostile_clients host build failed (see $OUT/hostile-clients-host-build.log)"
    fi
fi

# geometry_follows_hands — on a real PTY the newest window applies until
# someone types, the typist owns the size and pointer motion does not take
# it, a driver's pin holds and a caller without Drive is forbidden, two
# typists in turn resize the program at most once per coalesce window, an
# agent's pane.overrideGeometry holds until a driver types, and the grid in
# a HELLO sizes a session for a person attaching alone, never for an agent
# and never past a pin or an override.
# Friend seam, same QUIC-OFF host build as screen_lost_silence.
if wanted geometry_follows_hands && command -v cmake >/dev/null 2>&1; then
    echo "=== geometry_follows_hands ==="
    RAN=$((RAN + 1))
    HOST_BUILD="$UBQ_BUILD_ROOT/host-noquic"
    HOST_OBJDIR="$HOST_BUILD/CMakeFiles/ubiquitty-host.dir"
    if cmake -B "$HOST_BUILD" -S "$ROOT" -G "$UBQ_GENERATOR" -DUBIQUITTY_FRONTEND=Host -DCMAKE_BUILD_TYPE=Debug \
                >"$OUT/geometry-follows-hands-host-build.log" 2>&1 \
            && cmake --build "$HOST_BUILD" -j"$BUILD_JOBS" >>"$OUT/geometry-follows-hands-host-build.log" 2>&1; then
        host_objects "$HOST_OBJDIR" "$OUT/geometry-follows-hands-objs.txt" \
            || build_broken "host object list is empty (the harness, not the test under it)"
        if ubq_compile_parallel "$OUT/geometry_follows_hands" "$OUT/objects/geometry_follows_hands" "$OUT/geometry-follows-hands-build.log" \
                -std=c++23 -DUBIQUITTY_QUIC=1 -I"$QUIC_PREFIX/include" -I"$ROOT/Tests" -I"$ROOT/Source" -- \
                "$ROOT/Tests/Host/geometry_follows_hands.cpp" \
                -- "@$OUT/geometry-follows-hands-objs.txt" "${HOST_LINK_ARCHIVES[@]}"; then
            ubq_run_bounded 300 "$OUT/geometry_follows_hands" || exit 1
        else
            build_broken "geometry_follows_hands link failed (see $OUT/geometry-follows-hands-build.log)"
        fi
    else
        build_broken "geometry_follows_hands host build failed (see $OUT/geometry-follows-hands-host-build.log)"
    fi
fi
# sync_host_calls — a Sync call that names no pane reaches the host, and
# every call passes one check first: the caller's role, where it stands, its
# scope and its parameters, each refusal with its reason and the caller left
# attached. Four principals of one daemon. Friend seam, same QUIC-OFF host
# build as screen_lost_silence.
if wanted sync_host_calls && command -v cmake >/dev/null 2>&1; then
    echo "=== sync_host_calls ==="
    RAN=$((RAN + 1))
    HOST_BUILD="$UBQ_BUILD_ROOT/host-noquic"
    HOST_OBJDIR="$HOST_BUILD/CMakeFiles/ubiquitty-host.dir"
    if cmake -B "$HOST_BUILD" -S "$ROOT" -G "$UBQ_GENERATOR" -DUBIQUITTY_FRONTEND=Host -DCMAKE_BUILD_TYPE=Debug \
                >"$OUT/sync-host-calls-host-build.log" 2>&1 \
            && cmake --build "$HOST_BUILD" -j"$BUILD_JOBS" >>"$OUT/sync-host-calls-host-build.log" 2>&1; then
        host_objects "$HOST_OBJDIR" "$OUT/sync-host-calls-objs.txt" \
            || build_broken "host object list is empty (the harness, not the test under it)"
        if ubq_compile_parallel "$OUT/sync_host_calls" "$OUT/objects/sync_host_calls" "$OUT/sync-host-calls-build.log" \
                -std=c++23 -DUBIQUITTY_QUIC=1 -I"$QUIC_PREFIX/include" -I"$ROOT/Tests" -I"$ROOT/Source" -- \
                "$ROOT/Tests/Host/sync_host_calls.cpp" \
                -- "@$OUT/sync-host-calls-objs.txt" "${HOST_LINK_ARCHIVES[@]}"; then
            ubq_run_bounded 300 "$OUT/sync_host_calls" || exit 1
        else
            build_broken "sync_host_calls link failed (see $OUT/sync-host-calls-build.log)"
        fi
    else
        build_broken "sync_host_calls host build failed (see $OUT/sync-host-calls-host-build.log)"
    fi
fi
# background_refresh_storage — real daemon startup refuses malformed or unsafe
# journals without replacing them. A failed durable write exposes no cursor;
# repairing its private parent retries the same event identity. The fixture
# links the host objects because journal storage belongs to the daemon.
if wanted background_refresh_storage && command -v cmake >/dev/null 2>&1; then
    echo "=== background_refresh_storage ==="
    RAN=$((RAN + 1))
    HOST_BUILD="$UBQ_BUILD_ROOT/host-noquic"
    HOST_OBJDIR="$HOST_BUILD/CMakeFiles/ubiquitty-host.dir"
    if cmake -B "$HOST_BUILD" -S "$ROOT" -G "$UBQ_GENERATOR" -DUBIQUITTY_FRONTEND=Host -DCMAKE_BUILD_TYPE=Debug \
                >"$OUT/background-refresh-storage-host-build.log" 2>&1 \
            && cmake --build "$HOST_BUILD" -j"$BUILD_JOBS" >>"$OUT/background-refresh-storage-host-build.log" 2>&1; then
        host_objects "$HOST_OBJDIR" "$OUT/background-refresh-storage-objs.txt" \
            || build_broken "host object list is empty (the harness, not the test under it)"
        if ubq_compile_parallel "$OUT/background_refresh_storage" "$OUT/objects/background_refresh_storage" "$OUT/background-refresh-storage-build.log" \
                -std=c++23 -DUBIQUITTY_QUIC=1 -I"$QUIC_PREFIX/include" -I"$ROOT/Tests" -I"$ROOT/Source" -- \
                "$ROOT/Tests/Host/background_refresh_storage.cpp" \
                -- "@$OUT/background-refresh-storage-objs.txt" "${HOST_LINK_ARCHIVES[@]}"; then
            ubq_run_bounded 300 "$OUT/background_refresh_storage" || exit 1
        else
            build_broken "background_refresh_storage link failed (see $OUT/background-refresh-storage-build.log)"
        fi
    else
        build_broken "background_refresh_storage host build failed (see $OUT/background-refresh-storage-host-build.log)"
    fi
fi
# structure_calls — the calls that change what a workspace's structure says:
# a label and tags at every level, a workspace's and a tab's name, a tab's
# place and a closed window, each served to whoever may change the workspace,
# refused with its reason otherwise, and seen by a watching listing at a new
# revision. Friend seam, same QUIC-OFF host build as screen_lost_silence.
if wanted structure_calls && command -v cmake >/dev/null 2>&1; then
    echo "=== structure_calls ==="
    RAN=$((RAN + 1))
    HOST_BUILD="$UBQ_BUILD_ROOT/host-noquic"
    HOST_OBJDIR="$HOST_BUILD/CMakeFiles/ubiquitty-host.dir"
    if cmake -B "$HOST_BUILD" -S "$ROOT" -G "$UBQ_GENERATOR" -DUBIQUITTY_FRONTEND=Host -DCMAKE_BUILD_TYPE=Debug \
                >"$OUT/structure-calls-host-build.log" 2>&1 \
            && cmake --build "$HOST_BUILD" -j"$BUILD_JOBS" >>"$OUT/structure-calls-host-build.log" 2>&1; then
        host_objects "$HOST_OBJDIR" "$OUT/structure-calls-objs.txt" \
            || build_broken "host object list is empty (the harness, not the test under it)"
        if ubq_compile_parallel "$OUT/structure_calls" "$OUT/objects/structure_calls" "$OUT/structure-calls-build.log" \
                -std=c++23 -DUBIQUITTY_QUIC=1 -I"$QUIC_PREFIX/include" -I"$ROOT/Tests" -I"$ROOT/Source" -- \
                "$ROOT/Tests/Host/structure_calls.cpp" \
                -- "@$OUT/structure-calls-objs.txt" "${HOST_LINK_ARCHIVES[@]}"; then
            ubq_run_bounded 300 "$OUT/structure_calls" || exit 1
        else
            build_broken "structure_calls link failed (see $OUT/structure-calls-build.log)"
        fi
    else
        build_broken "structure_calls host build failed (see $OUT/structure-calls-host-build.log)"
    fi
fi
# session_structure_listing — the daemon describes each workspace in one
# record, sent to whoever reads the workspace and again only when it changes;
# a connection that subscribes is sent every workspace, the one the host
# could restore among them, and each unbound pane's facts and status with no
# screen, from which Core assembles the host's tree. Friend seam, same
# QUIC-OFF host build as screen_lost_silence.
if wanted session_structure_listing && command -v cmake >/dev/null 2>&1; then
    echo "=== session_structure_listing ==="
    RAN=$((RAN + 1))
    HOST_BUILD="$UBQ_BUILD_ROOT/host-noquic"
    HOST_OBJDIR="$HOST_BUILD/CMakeFiles/ubiquitty-host.dir"
    if cmake -B "$HOST_BUILD" -S "$ROOT" -G "$UBQ_GENERATOR" -DUBIQUITTY_FRONTEND=Host -DCMAKE_BUILD_TYPE=Debug \
                >"$OUT/session-structure-listing-host-build.log" 2>&1 \
            && cmake --build "$HOST_BUILD" -j"$BUILD_JOBS" >>"$OUT/session-structure-listing-host-build.log" 2>&1; then
        host_objects "$HOST_OBJDIR" "$OUT/session-structure-listing-objs.txt" \
            || build_broken "host object list is empty (the harness, not the test under it)"
        if ubq_compile_parallel "$OUT/session_structure_listing" "$OUT/objects/session_structure_listing" "$OUT/session-structure-listing-build.log" \
                -std=c++23 -DUBIQUITTY_QUIC=1 -I"$QUIC_PREFIX/include" -I"$ROOT/Tests" -I"$ROOT/Source" -- \
                "$ROOT/Tests/Host/session_structure_listing.cpp" \
                -- "@$OUT/session-structure-listing-objs.txt" "${HOST_LINK_ARCHIVES[@]}"; then
            ubq_run_bounded 300 "$OUT/session_structure_listing" || exit 1
        else
            build_broken "session_structure_listing link failed (see $OUT/session-structure-listing-build.log)"
        fi
    else
        build_broken "session_structure_listing host build failed (see $OUT/session-structure-listing-host-build.log)"
    fi
fi
# viewport_structure_cells — the size a driver's Viewport gives a pane is the
# size its workspace's structure reports to a second client: a pane laid out
# beside another takes its driver's Viewport and nothing from its window, a
# pane that is all its tab shows takes the window, and the leaf's and the
# split's cells follow under a later revision. Friend seam, same QUIC-OFF
# host build as screen_lost_silence.
if wanted viewport_structure_cells && command -v cmake >/dev/null 2>&1; then
    echo "=== viewport_structure_cells ==="
    RAN=$((RAN + 1))
    HOST_BUILD="$UBQ_BUILD_ROOT/host-noquic"
    HOST_OBJDIR="$HOST_BUILD/CMakeFiles/ubiquitty-host.dir"
    if cmake -B "$HOST_BUILD" -S "$ROOT" -G "$UBQ_GENERATOR" -DUBIQUITTY_FRONTEND=Host -DCMAKE_BUILD_TYPE=Debug \
                >"$OUT/viewport-structure-cells-host-build.log" 2>&1 \
            && cmake --build "$HOST_BUILD" -j"$BUILD_JOBS" >>"$OUT/viewport-structure-cells-host-build.log" 2>&1; then
        host_objects "$HOST_OBJDIR" "$OUT/viewport-structure-cells-objs.txt" \
            || build_broken "host object list is empty (the harness, not the test under it)"
        if ubq_compile_parallel "$OUT/viewport_structure_cells" "$OUT/objects/viewport_structure_cells" "$OUT/viewport-structure-cells-build.log" \
                -std=c++23 -DUBIQUITTY_QUIC=1 -I"$QUIC_PREFIX/include" -I"$ROOT/Tests" -I"$ROOT/Source" -- \
                "$ROOT/Tests/Host/viewport_structure_cells.cpp" \
                -- "@$OUT/viewport-structure-cells-objs.txt" "${HOST_LINK_ARCHIVES[@]}"; then
            ubq_run_bounded 300 "$OUT/viewport_structure_cells" || exit 1
        else
            build_broken "viewport_structure_cells link failed (see $OUT/viewport-structure-cells-build.log)"
        fi
    else
        build_broken "viewport_structure_cells host build failed (see $OUT/viewport-structure-cells-host-build.log)"
    fi
fi
# pane_records_published — the daemon reads every pane's PaneFacts and
# StatusRecords from its terminal, attached or not, on a budget: a shown
# pane's status rides its screen, attention is seen by a person looking and
# nobody else, and the activity stamps move the facts once a second at most.
# Friend seam, same QUIC-OFF host build as screen_lost_silence.
if wanted pane_records_published && command -v cmake >/dev/null 2>&1; then
    echo "=== pane_records_published ==="
    RAN=$((RAN + 1))
    HOST_BUILD="$UBQ_BUILD_ROOT/host-noquic"
    HOST_OBJDIR="$HOST_BUILD/CMakeFiles/ubiquitty-host.dir"
    if cmake -B "$HOST_BUILD" -S "$ROOT" -G "$UBQ_GENERATOR" -DUBIQUITTY_FRONTEND=Host -DCMAKE_BUILD_TYPE=Debug \
                >"$OUT/pane-records-published-host-build.log" 2>&1 \
            && cmake --build "$HOST_BUILD" -j"$BUILD_JOBS" >>"$OUT/pane-records-published-host-build.log" 2>&1; then
        host_objects "$HOST_OBJDIR" "$OUT/pane-records-published-objs.txt" \
            || build_broken "host object list is empty (the harness, not the test under it)"
        if ubq_compile_parallel "$OUT/pane_records_published" "$OUT/objects/pane_records_published" "$OUT/pane-records-published-build.log" \
                -std=c++23 -DUBIQUITTY_QUIC=1 -I"$QUIC_PREFIX/include" -I"$ROOT/Tests" -I"$ROOT/Source" -- \
                "$ROOT/Tests/Host/pane_records_published.cpp" \
                -- "@$OUT/pane-records-published-objs.txt" "${HOST_LINK_ARCHIVES[@]}"; then
            ubq_run_bounded 300 "$OUT/pane_records_published" || exit 1
        else
            build_broken "pane_records_published link failed (see $OUT/pane-records-published-build.log)"
        fi
    else
        build_broken "pane_records_published host build failed (see $OUT/pane-records-published-host-build.log)"
    fi
fi
# pane_activity_stamps — the host stamps a pane's last output once per drain
# pass and its last input once per pass of typing, on the wall clock, with an
# injected clock: output and input move apart, typing stamps at the end of
# the pass that carried it, pointer motion and a focus change do not, and a pass reads the
# clock once however much it carried. Friend seam, same QUIC-OFF host build
# as screen_lost_silence.
if wanted pane_activity_stamps && command -v cmake >/dev/null 2>&1; then
    echo "=== pane_activity_stamps ==="
    RAN=$((RAN + 1))
    HOST_BUILD="$UBQ_BUILD_ROOT/host-noquic"
    HOST_OBJDIR="$HOST_BUILD/CMakeFiles/ubiquitty-host.dir"
    if cmake -B "$HOST_BUILD" -S "$ROOT" -G "$UBQ_GENERATOR" -DUBIQUITTY_FRONTEND=Host -DCMAKE_BUILD_TYPE=Debug \
                >"$OUT/pane-activity-stamps-host-build.log" 2>&1 \
            && cmake --build "$HOST_BUILD" -j"$BUILD_JOBS" >>"$OUT/pane-activity-stamps-host-build.log" 2>&1; then
        host_objects "$HOST_OBJDIR" "$OUT/pane-activity-stamps-objs.txt" \
            || build_broken "host object list is empty (the harness, not the test under it)"
        if ubq_compile_parallel "$OUT/pane_activity_stamps" "$OUT/objects/pane_activity_stamps" "$OUT/pane-activity-stamps-build.log" \
                -std=c++23 -DUBIQUITTY_QUIC=1 -I"$QUIC_PREFIX/include" -I"$ROOT/Tests" -I"$ROOT/Source" -- \
                "$ROOT/Tests/Host/pane_activity_stamps.cpp" \
                -- "@$OUT/pane-activity-stamps-objs.txt" "${HOST_LINK_ARCHIVES[@]}"; then
            "$OUT/pane_activity_stamps" || exit 1
        else
            build_broken "pane_activity_stamps link failed (see $OUT/pane-activity-stamps-build.log)"
        fi
    else
        build_broken "pane_activity_stamps host build failed (see $OUT/pane-activity-stamps-host-build.log)"
    fi
fi

# attention_seen_on_focus — a pane's latched bell and notification are cleared
# when a driver the host verified as a person gains the focus on it, and stay
# when an agent's driver does. A real daemon object, driven as
# pane_activity_stamps is.
if wanted attention_seen_on_focus && command -v cmake >/dev/null 2>&1; then
    echo "=== attention_seen_on_focus ==="
    RAN=$((RAN + 1))
    HOST_BUILD="$UBQ_BUILD_ROOT/host-noquic"
    HOST_OBJDIR="$HOST_BUILD/CMakeFiles/ubiquitty-host.dir"
    if cmake -B "$HOST_BUILD" -S "$ROOT" -G "$UBQ_GENERATOR" -DUBIQUITTY_FRONTEND=Host -DCMAKE_BUILD_TYPE=Debug \
                >"$OUT/attention-seen-on-focus-host-build.log" 2>&1 \
            && cmake --build "$HOST_BUILD" -j"$BUILD_JOBS" >>"$OUT/attention-seen-on-focus-host-build.log" 2>&1; then
        host_objects "$HOST_OBJDIR" "$OUT/attention-seen-on-focus-objs.txt" \
            || build_broken "host object list is empty (the harness, not the test under it)"
        if ubq_compile_parallel "$OUT/attention_seen_on_focus" "$OUT/objects/attention_seen_on_focus" "$OUT/attention-seen-on-focus-build.log" \
                -std=c++23 -DUBIQUITTY_QUIC=1 -I"$QUIC_PREFIX/include" -I"$ROOT/Tests" -I"$ROOT/Source" -- \
                "$ROOT/Tests/Host/attention_seen_on_focus.cpp" \
                -- "@$OUT/attention-seen-on-focus-objs.txt" "${HOST_LINK_ARCHIVES[@]}"; then
            "$OUT/attention_seen_on_focus" || exit 1
        else
            build_broken "attention_seen_on_focus link failed (see $OUT/attention-seen-on-focus-build.log)"
        fi
    else
        build_broken "attention_seen_on_focus host build failed (see $OUT/attention-seen-on-focus-host-build.log)"
    fi
fi

# pane_activity_live_typing — a daemon running its own loop stamps the last
# input of a pane a Sync client types into: the test is the client on a
# socket pair, the daemon's loop runs on its own thread, and the injected
# clock reads the stamp back. Friend seam, same QUIC-OFF host build as
# pane_activity_stamps.
if wanted pane_activity_live_typing && command -v cmake >/dev/null 2>&1; then
    echo "=== pane_activity_live_typing ==="
    RAN=$((RAN + 1))
    HOST_BUILD="$UBQ_BUILD_ROOT/host-noquic"
    HOST_OBJDIR="$HOST_BUILD/CMakeFiles/ubiquitty-host.dir"
    if cmake -B "$HOST_BUILD" -S "$ROOT" -G "$UBQ_GENERATOR" -DUBIQUITTY_FRONTEND=Host -DCMAKE_BUILD_TYPE=Debug \
                >"$OUT/pane-activity-live-typing-host-build.log" 2>&1 \
            && cmake --build "$HOST_BUILD" -j"$BUILD_JOBS" >>"$OUT/pane-activity-live-typing-host-build.log" 2>&1; then
        host_objects "$HOST_OBJDIR" "$OUT/pane-activity-live-typing-objs.txt" \
            || build_broken "host object list is empty (the harness, not the test under it)"
        if ubq_compile_parallel "$OUT/pane_activity_live_typing" "$OUT/objects/pane_activity_live_typing" "$OUT/pane-activity-live-typing-build.log" \
                -std=c++23 -DUBIQUITTY_QUIC=1 -I"$QUIC_PREFIX/include" -I"$ROOT/Tests" -I"$ROOT/Source" -- \
                "$ROOT/Tests/Host/pane_activity_live_typing.cpp" \
                -- "@$OUT/pane-activity-live-typing-objs.txt" "${HOST_LINK_ARCHIVES[@]}"; then
            "$OUT/pane_activity_live_typing" || exit 1
        else
            build_broken "pane_activity_live_typing link failed (see $OUT/pane-activity-live-typing-build.log)"
        fi
    else
        build_broken "pane_activity_live_typing host build failed (see $OUT/pane-activity-live-typing-host-build.log)"
    fi
fi

# pane_end_facts — how a session ended is in the last facts its readers are
# sent: a daemon running its own loop reaps a program that exited with a
# status and one a signal killed, and the test is the client on a socket
# pair. A pane bound by a reader is sent its facts before it writes anything.
# Friend seam, same QUIC-OFF host build as pane_activity_stamps.
if wanted pane_end_facts && command -v cmake >/dev/null 2>&1; then
    echo "=== pane_end_facts ==="
    RAN=$((RAN + 1))
    HOST_BUILD="$UBQ_BUILD_ROOT/host-noquic"
    HOST_OBJDIR="$HOST_BUILD/CMakeFiles/ubiquitty-host.dir"
    if cmake -B "$HOST_BUILD" -S "$ROOT" -G "$UBQ_GENERATOR" -DUBIQUITTY_FRONTEND=Host -DCMAKE_BUILD_TYPE=Debug \
                >"$OUT/pane-end-facts-host-build.log" 2>&1 \
            && cmake --build "$HOST_BUILD" -j"$BUILD_JOBS" >>"$OUT/pane-end-facts-host-build.log" 2>&1; then
        host_objects "$HOST_OBJDIR" "$OUT/pane-end-facts-objs.txt" \
            || build_broken "host object list is empty (the harness, not the test under it)"
        if ubq_compile_parallel "$OUT/pane_end_facts" "$OUT/objects/pane_end_facts" "$OUT/pane-end-facts-build.log" \
                -std=c++23 -DUBIQUITTY_QUIC=1 -I"$QUIC_PREFIX/include" -I"$ROOT/Tests" -I"$ROOT/Source" -- \
                "$ROOT/Tests/Host/pane_end_facts.cpp" \
                -- "@$OUT/pane-end-facts-objs.txt" "${HOST_LINK_ARCHIVES[@]}"; then
            "$OUT/pane_end_facts" || exit 1
        else
            build_broken "pane_end_facts link failed (see $OUT/pane-end-facts-build.log)"
        fi
    else
        build_broken "pane_end_facts host build failed (see $OUT/pane-end-facts-host-build.log)"
    fi
fi

# label_record_persistence — the labels of a workspace, its tabs and windows,
# a pane and the session behind it are kept by the host's one label book under
# its bounds, written to the workspace's record, and set again on the
# workspace a second daemon restores from the record's bytes. Friend seam, same QUIC-OFF host build as
# pane_activity_stamps.
if wanted label_record_persistence && command -v cmake >/dev/null 2>&1; then
    echo "=== label_record_persistence ==="
    RAN=$((RAN + 1))
    HOST_BUILD="$UBQ_BUILD_ROOT/host-noquic"
    HOST_OBJDIR="$HOST_BUILD/CMakeFiles/ubiquitty-host.dir"
    if cmake -B "$HOST_BUILD" -S "$ROOT" -G "$UBQ_GENERATOR" -DUBIQUITTY_FRONTEND=Host -DCMAKE_BUILD_TYPE=Debug \
                >"$OUT/label-record-persistence-host-build.log" 2>&1 \
            && cmake --build "$HOST_BUILD" -j"$BUILD_JOBS" >>"$OUT/label-record-persistence-host-build.log" 2>&1; then
        host_objects "$HOST_OBJDIR" "$OUT/label-record-persistence-objs.txt" \
            || build_broken "host object list is empty (the harness, not the test under it)"
        if ubq_compile_parallel "$OUT/label_record_persistence" "$OUT/objects/label_record_persistence" "$OUT/label-record-persistence-build.log" \
                -std=c++23 -DUBIQUITTY_QUIC=1 -I"$QUIC_PREFIX/include" -I"$ROOT/Tests" -I"$ROOT/Source" -- \
                "$ROOT/Tests/Host/label_record_persistence.cpp" \
                -- "@$OUT/label-record-persistence-objs.txt" "${HOST_LINK_ARCHIVES[@]}"; then
            "$OUT/label_record_persistence" || exit 1
        else
            build_broken "label_record_persistence link failed (see $OUT/label-record-persistence-build.log)"
        fi
    else
        build_broken "label_record_persistence host build failed (see $OUT/label-record-persistence-host-build.log)"
    fi
fi

# layout_record_typed_line — a line typed to a recorded workspace while a
# record write is already owed is owed a write of its own a whole debounce
# interval after it, so the directory the shell moves to reaches the record.
# Friend seam, same QUIC-OFF host build as label_record_persistence.
if wanted layout_record_typed_line && command -v cmake >/dev/null 2>&1; then
    echo "=== layout_record_typed_line ==="
    RAN=$((RAN + 1))
    HOST_BUILD="$UBQ_BUILD_ROOT/host-noquic"
    HOST_OBJDIR="$HOST_BUILD/CMakeFiles/ubiquitty-host.dir"
    if cmake -B "$HOST_BUILD" -S "$ROOT" -G "$UBQ_GENERATOR" -DUBIQUITTY_FRONTEND=Host -DCMAKE_BUILD_TYPE=Debug \
                >"$OUT/layout-record-typed-line-host-build.log" 2>&1 \
            && cmake --build "$HOST_BUILD" -j"$BUILD_JOBS" >>"$OUT/layout-record-typed-line-host-build.log" 2>&1; then
        host_objects "$HOST_OBJDIR" "$OUT/layout-record-typed-line-objs.txt" \
            || build_broken "host object list is empty (the harness, not the test under it)"
        if ubq_compile_parallel "$OUT/layout_record_typed_line" "$OUT/objects/layout_record_typed_line" "$OUT/layout-record-typed-line-build.log" \
                -std=c++23 -DUBIQUITTY_QUIC=1 -I"$QUIC_PREFIX/include" -I"$ROOT/Tests" -I"$ROOT/Source" -- \
                "$ROOT/Tests/Host/layout_record_typed_line.cpp" \
                -- "@$OUT/layout-record-typed-line-objs.txt" "${HOST_LINK_ARCHIVES[@]}"; then
            "$OUT/layout_record_typed_line" || exit 1
        else
            build_broken "layout_record_typed_line link failed (see $OUT/layout-record-typed-line-build.log)"
        fi
    else
        build_broken "layout_record_typed_line host build failed (see $OUT/layout-record-typed-line-host-build.log)"
    fi
fi

# owner_presents — the program's pixel-size and colour replies are the
# geometry owner's: a client that has not typed changes neither, and the next
# owner's apply once its window passes. Friend seam, same QUIC-OFF host build
# as screen_lost_silence.
if wanted owner_presents && command -v cmake >/dev/null 2>&1; then
    echo "=== owner_presents ==="
    RAN=$((RAN + 1))
    HOST_BUILD="$UBQ_BUILD_ROOT/host-noquic"
    HOST_OBJDIR="$HOST_BUILD/CMakeFiles/ubiquitty-host.dir"
    if cmake -B "$HOST_BUILD" -S "$ROOT" -G "$UBQ_GENERATOR" -DUBIQUITTY_FRONTEND=Host -DCMAKE_BUILD_TYPE=Debug \
                >"$OUT/owner-presents-host-build.log" 2>&1 \
            && cmake --build "$HOST_BUILD" -j"$BUILD_JOBS" >>"$OUT/owner-presents-host-build.log" 2>&1; then
        host_objects "$HOST_OBJDIR" "$OUT/owner-presents-objs.txt" \
            || build_broken "host object list is empty (the harness, not the test under it)"
        if ubq_compile_parallel "$OUT/owner_presents" "$OUT/objects/owner_presents" "$OUT/owner-presents-build.log" \
                -std=c++23 -DUBIQUITTY_QUIC=1 -I"$QUIC_PREFIX/include" -I"$ROOT/Tests" -I"$ROOT/Source" -- \
                "$ROOT/Tests/Host/owner_presents.cpp" \
                -- "@$OUT/owner-presents-objs.txt" "${HOST_LINK_ARCHIVES[@]}"; then
            ubq_run_bounded 300 "$OUT/owner_presents" || exit 1
        else
            build_broken "owner_presents link failed (see $OUT/owner-presents-build.log)"
        fi
    else
        build_broken "owner_presents host build failed (see $OUT/owner-presents-host-build.log)"
    fi
fi

# rich_text_fit_attached — a host-attached pane answers an OSC 7404 fit query
# in the typography table its client published, and takes a changed one.
# Friend seam, same QUIC-OFF host build as owner_presents.
if wanted rich_text_fit_attached && command -v cmake >/dev/null 2>&1; then
    echo "=== rich_text_fit_attached ==="
    RAN=$((RAN + 1))
    HOST_BUILD="$UBQ_BUILD_ROOT/host-noquic"
    HOST_OBJDIR="$HOST_BUILD/CMakeFiles/ubiquitty-host.dir"
    if cmake -B "$HOST_BUILD" -S "$ROOT" -G "$UBQ_GENERATOR" -DUBIQUITTY_FRONTEND=Host -DCMAKE_BUILD_TYPE=Debug \
                >"$OUT/rich-text-fit-attached-host-build.log" 2>&1 \
            && cmake --build "$HOST_BUILD" -j"$BUILD_JOBS" >>"$OUT/rich-text-fit-attached-host-build.log" 2>&1; then
        host_objects "$HOST_OBJDIR" "$OUT/rich-text-fit-attached-objs.txt" \
            || build_broken "host object list is empty (the harness, not the test under it)"
        if ubq_compile_parallel "$OUT/rich_text_fit_attached" "$OUT/objects/rich_text_fit_attached" "$OUT/rich-text-fit-attached-build.log" \
                -std=c++23 -DUBIQUITTY_QUIC=1 -I"$QUIC_PREFIX/include" -I"$ROOT/Tests" -I"$ROOT/Source" -- \
                "$ROOT/Tests/Host/rich_text_fit_attached.cpp" \
                -- "@$OUT/rich-text-fit-attached-objs.txt" "${HOST_LINK_ARCHIVES[@]}"; then
            ubq_run_bounded 300 "$OUT/rich_text_fit_attached" || exit 1
        else
            build_broken "rich_text_fit_attached link failed (see $OUT/rich-text-fit-attached-build.log)"
        fi
    else
        build_broken "rich_text_fit_attached host build failed (see $OUT/rich-text-fit-attached-host-build.log)"
    fi
fi

# paste_bracket_owed — a paste whose client is gone keeps its bracket open
# through the grace and has it closed past it, so the program is not left in
# paste mode. Friend seam, same QUIC-OFF host build as screen_lost_silence.
if wanted paste_bracket_owed && command -v cmake >/dev/null 2>&1; then
    echo "=== paste_bracket_owed ==="
    RAN=$((RAN + 1))
    HOST_BUILD="$UBQ_BUILD_ROOT/host-noquic"
    HOST_OBJDIR="$HOST_BUILD/CMakeFiles/ubiquitty-host.dir"
    if cmake -B "$HOST_BUILD" -S "$ROOT" -G "$UBQ_GENERATOR" -DUBIQUITTY_FRONTEND=Host -DCMAKE_BUILD_TYPE=Debug \
                >"$OUT/paste-bracket-owed-host-build.log" 2>&1 \
            && cmake --build "$HOST_BUILD" -j"$BUILD_JOBS" >>"$OUT/paste-bracket-owed-host-build.log" 2>&1; then
        host_objects "$HOST_OBJDIR" "$OUT/paste-bracket-owed-objs.txt" \
            || build_broken "host object list is empty (the harness, not the test under it)"
        if ubq_compile_parallel "$OUT/paste_bracket_owed" "$OUT/objects/paste_bracket_owed" "$OUT/paste-bracket-owed-build.log" \
                -std=c++23 -DUBIQUITTY_QUIC=1 -I"$QUIC_PREFIX/include" -I"$ROOT/Tests" -I"$ROOT/Source" -- \
                "$ROOT/Tests/Host/paste_bracket_owed.cpp" \
                -- "@$OUT/paste-bracket-owed-objs.txt" "${HOST_LINK_ARCHIVES[@]}"; then
            ubq_run_bounded 300 "$OUT/paste_bracket_owed" || exit 1
        else
            build_broken "paste_bracket_owed link failed (see $OUT/paste-bracket-owed-build.log)"
        fi
    else
        build_broken "paste_bracket_owed host build failed (see $OUT/paste-bracket-owed-host-build.log)"
    fi
fi

# echo_context_facts — each frame's echo context carries the PTY's echo and
# signal characters and the shell's prompt. Friend seam, same QUIC-OFF host
# build as screen_lost_silence.
if wanted echo_context_facts && command -v cmake >/dev/null 2>&1; then
    echo "=== echo_context_facts ==="
    RAN=$((RAN + 1))
    HOST_BUILD="$UBQ_BUILD_ROOT/host-noquic"
    HOST_OBJDIR="$HOST_BUILD/CMakeFiles/ubiquitty-host.dir"
    if cmake -B "$HOST_BUILD" -S "$ROOT" -G "$UBQ_GENERATOR" -DUBIQUITTY_FRONTEND=Host -DCMAKE_BUILD_TYPE=Debug \
                >"$OUT/echo-context-facts-host-build.log" 2>&1 \
            && cmake --build "$HOST_BUILD" -j"$BUILD_JOBS" >>"$OUT/echo-context-facts-host-build.log" 2>&1; then
        host_objects "$HOST_OBJDIR" "$OUT/echo-context-facts-objs.txt" \
            || build_broken "host object list is empty (the harness, not the test under it)"
        if ubq_compile_parallel "$OUT/echo_context_facts" "$OUT/objects/echo_context_facts" "$OUT/echo-context-facts-build.log" \
                -std=c++23 -DUBIQUITTY_QUIC=1 -I"$QUIC_PREFIX/include" -I"$ROOT/Tests" -I"$ROOT/Source" -- \
                "$ROOT/Tests/Host/echo_context_facts.cpp" \
                -- "@$OUT/echo-context-facts-objs.txt" "${HOST_LINK_ARCHIVES[@]}"; then
            ubq_run_bounded 300 "$OUT/echo_context_facts" || exit 1
        else
            build_broken "echo_context_facts link failed (see $OUT/echo-context-facts-build.log)"
        fi
    else
        build_broken "echo_context_facts host build failed (see $OUT/echo-context-facts-host-build.log)"
    fi
fi

# paste_abort_sigint — Ctrl-C during a paste closes the bracket and signals
# a program under a real PTY. Friend seam, same QUIC-OFF host build as
# screen_lost_silence.
if wanted paste_abort_sigint && command -v cmake >/dev/null 2>&1; then
    echo "=== paste_abort_sigint ==="
    RAN=$((RAN + 1))
    HOST_BUILD="$UBQ_BUILD_ROOT/host-noquic"
    HOST_OBJDIR="$HOST_BUILD/CMakeFiles/ubiquitty-host.dir"
    if cmake -B "$HOST_BUILD" -S "$ROOT" -G "$UBQ_GENERATOR" -DUBIQUITTY_FRONTEND=Host -DCMAKE_BUILD_TYPE=Debug \
                >"$OUT/paste-abort-sigint-host-build.log" 2>&1 \
            && cmake --build "$HOST_BUILD" -j"$BUILD_JOBS" >>"$OUT/paste-abort-sigint-host-build.log" 2>&1; then
        host_objects "$HOST_OBJDIR" "$OUT/paste-abort-sigint-objs.txt" \
            || build_broken "host object list is empty (the harness, not the test under it)"
        if ubq_compile_parallel "$OUT/paste_abort_sigint" "$OUT/objects/paste_abort_sigint" "$OUT/paste-abort-sigint-build.log" \
                -std=c++23 -DUBIQUITTY_QUIC=1 -I"$QUIC_PREFIX/include" -I"$ROOT/Tests" -I"$ROOT/Source" -- \
                "$ROOT/Tests/Host/paste_abort_sigint.cpp" \
                -- "@$OUT/paste-abort-sigint-objs.txt" "${HOST_LINK_ARCHIVES[@]}"; then
            ubq_run_bounded 300 "$OUT/paste_abort_sigint" || exit 1
        else
            build_broken "paste_abort_sigint link failed (see $OUT/paste-abort-sigint-build.log)"
        fi
    else
        build_broken "paste_abort_sigint host build failed (see $OUT/paste-abort-sigint-host-build.log)"
    fi
fi

# suspending_keeps_geometry — a client that says Bye{Suspending} is Lost at
# once and the program keeps its size under a real PTY. Friend seam, same
# QUIC-OFF host build as screen_lost_silence.
if wanted suspending_keeps_geometry && command -v cmake >/dev/null 2>&1; then
    echo "=== suspending_keeps_geometry ==="
    RAN=$((RAN + 1))
    HOST_BUILD="$UBQ_BUILD_ROOT/host-noquic"
    HOST_OBJDIR="$HOST_BUILD/CMakeFiles/ubiquitty-host.dir"
    if cmake -B "$HOST_BUILD" -S "$ROOT" -G "$UBQ_GENERATOR" -DUBIQUITTY_FRONTEND=Host -DCMAKE_BUILD_TYPE=Debug \
                >"$OUT/suspending-keeps-geometry-host-build.log" 2>&1 \
            && cmake --build "$HOST_BUILD" -j"$BUILD_JOBS" >>"$OUT/suspending-keeps-geometry-host-build.log" 2>&1; then
        host_objects "$HOST_OBJDIR" "$OUT/suspending-keeps-geometry-objs.txt" \
            || build_broken "host object list is empty (the harness, not the test under it)"
        if ubq_compile_parallel "$OUT/suspending_keeps_geometry" "$OUT/objects/suspending_keeps_geometry" "$OUT/suspending-keeps-geometry-build.log" \
                -std=c++23 -DUBIQUITTY_QUIC=1 -I"$QUIC_PREFIX/include" -I"$ROOT/Tests" -I"$ROOT/Source" -- \
                "$ROOT/Tests/Host/suspending_keeps_geometry.cpp" \
                -- "@$OUT/suspending-keeps-geometry-objs.txt" "${HOST_LINK_ARCHIVES[@]}"; then
            ubq_run_bounded 300 "$OUT/suspending_keeps_geometry" || exit 1
        else
            build_broken "suspending_keeps_geometry link failed (see $OUT/suspending-keeps-geometry-build.log)"
        fi
    else
        build_broken "suspending_keeps_geometry host build failed (see $OUT/suspending-keeps-geometry-host-build.log)"
    fi
fi

# attachments_listed — the daemon tells each client who shows its pane, who
# owns its size and who is its primary, and names no principal. Friend seam,
# same QUIC-OFF host build as screen_lost_silence.
if wanted attachments_listed && command -v cmake >/dev/null 2>&1; then
    echo "=== attachments_listed ==="
    RAN=$((RAN + 1))
    HOST_BUILD="$UBQ_BUILD_ROOT/host-noquic"
    HOST_OBJDIR="$HOST_BUILD/CMakeFiles/ubiquitty-host.dir"
    if cmake -B "$HOST_BUILD" -S "$ROOT" -G "$UBQ_GENERATOR" -DUBIQUITTY_FRONTEND=Host -DCMAKE_BUILD_TYPE=Debug \
                >"$OUT/attachments-listed-host-build.log" 2>&1 \
            && cmake --build "$HOST_BUILD" -j"$BUILD_JOBS" >>"$OUT/attachments-listed-host-build.log" 2>&1; then
        host_objects "$HOST_OBJDIR" "$OUT/attachments-listed-objs.txt" \
            || build_broken "host object list is empty (the harness, not the test under it)"
        if ubq_compile_parallel "$OUT/attachments_listed" "$OUT/objects/attachments_listed" "$OUT/attachments-listed-build.log" \
                -std=c++23 -DUBIQUITTY_QUIC=1 -I"$QUIC_PREFIX/include" -I"$ROOT/Tests" -I"$ROOT/Source" -- \
                "$ROOT/Tests/Host/attachments_listed.cpp" \
                -- "@$OUT/attachments-listed-objs.txt" "${HOST_LINK_ARCHIVES[@]}"; then
            ubq_run_bounded 300 "$OUT/attachments_listed" || exit 1
        else
            build_broken "attachments_listed link failed (see $OUT/attachments-listed-build.log)"
        fi
    else
        build_broken "attachments_listed host build failed (see $OUT/attachments-listed-host-build.log)"
    fi
fi

# paste_window_never_holds — every client is read through a paste a program
# is not reading, and the pane holds a window of it. Friend seam, same QUIC-OFF host build as
# screen_lost_silence.
if wanted paste_window_never_holds && command -v cmake >/dev/null 2>&1; then
    echo "=== paste_window_never_holds ==="
    RAN=$((RAN + 1))
    HOST_BUILD="$UBQ_BUILD_ROOT/host-noquic"
    HOST_OBJDIR="$HOST_BUILD/CMakeFiles/ubiquitty-host.dir"
    if cmake -B "$HOST_BUILD" -S "$ROOT" -G "$UBQ_GENERATOR" -DUBIQUITTY_FRONTEND=Host -DCMAKE_BUILD_TYPE=Debug \
                >"$OUT/paste-window-never-holds-host-build.log" 2>&1 \
            && cmake --build "$HOST_BUILD" -j"$BUILD_JOBS" >>"$OUT/paste-window-never-holds-host-build.log" 2>&1; then
        host_objects "$HOST_OBJDIR" "$OUT/paste-window-never-holds-objs.txt" \
            || build_broken "host object list is empty (the harness, not the test under it)"
        if ubq_compile_parallel "$OUT/paste_window_never_holds" "$OUT/objects/paste_window_never_holds" "$OUT/paste-window-never-holds-build.log" \
                -std=c++23 -DUBIQUITTY_QUIC=1 -I"$QUIC_PREFIX/include" -I"$ROOT/Tests" -I"$ROOT/Source" -- \
                "$ROOT/Tests/Host/paste_window_never_holds.cpp" \
                -- "@$OUT/paste-window-never-holds-objs.txt" "${HOST_LINK_ARCHIVES[@]}"; then
            ubq_run_bounded 300 "$OUT/paste_window_never_holds" || exit 1
        else
            build_broken "paste_window_never_holds link failed (see $OUT/paste-window-never-holds-build.log)"
        fi
    else
        build_broken "paste_window_never_holds host build failed (see $OUT/paste-window-never-holds-host-build.log)"
    fi
fi

# attachment_grant_lifetime — what a resume proof is served under is its
# token's scope, kept only for an attachment a QUIC client made, and goes with
# the attachment; a proof for a freed attachment is refused, and an ungreeted
# proof connection is dropped when a newer proof replaces it. Friend seam,
# same QUIC-OFF host build as screen_lost_silence.
if wanted attachment_grant_lifetime && command -v cmake >/dev/null 2>&1; then
    echo "=== attachment_grant_lifetime ==="
    RAN=$((RAN + 1))
    HOST_BUILD="$UBQ_BUILD_ROOT/host-noquic"
    HOST_OBJDIR="$HOST_BUILD/CMakeFiles/ubiquitty-host.dir"
    if cmake -B "$HOST_BUILD" -S "$ROOT" -G "$UBQ_GENERATOR" -DUBIQUITTY_FRONTEND=Host -DCMAKE_BUILD_TYPE=Debug \
                >"$OUT/attachment-grant-lifetime-host-build.log" 2>&1 \
            && cmake --build "$HOST_BUILD" -j"$BUILD_JOBS" >>"$OUT/attachment-grant-lifetime-host-build.log" 2>&1; then
        host_objects "$HOST_OBJDIR" "$OUT/attachment-grant-lifetime-objs.txt" \
            || build_broken "host object list is empty (the harness, not the test under it)"
        if ubq_compile_parallel "$OUT/attachment_grant_lifetime" "$OUT/objects/attachment_grant_lifetime" "$OUT/attachment-grant-lifetime-build.log" \
                -std=c++23 -DUBIQUITTY_QUIC=1 -I"$QUIC_PREFIX/include" -I"$ROOT/Tests" -I"$ROOT/Source" -- \
                "$ROOT/Tests/Host/attachment_grant_lifetime.cpp" \
                -- "@$OUT/attachment-grant-lifetime-objs.txt" "${HOST_LINK_ARCHIVES[@]}"; then
            ubq_run_bounded 300 "$OUT/attachment_grant_lifetime" || exit 1
        else
            build_broken "attachment_grant_lifetime link failed (see $OUT/attachment-grant-lifetime-build.log)"
        fi
    else
        build_broken "attachment_grant_lifetime host build failed (see $OUT/attachment-grant-lifetime-host-build.log)"
    fi
fi

# session_handover_ledger — every direct session member has a preservation
# requirement. A closed stream double supplies the reflection fixture; no
# host listener, user session, or PTY is opened.
if wanted session_handover_ledger && command -v cmake >/dev/null 2>&1; then
    echo "=== session_handover_ledger ==="
    RAN=$((RAN + 1))
    HOST_BUILD="$UBQ_BUILD_ROOT/host-noquic"
    HOST_OBJDIR="$HOST_BUILD/CMakeFiles/ubiquitty-host.dir"
    if cmake -B "$HOST_BUILD" -S "$ROOT" -G "$UBQ_GENERATOR" -DUBIQUITTY_FRONTEND=Host -DCMAKE_BUILD_TYPE=Debug \
                >"$OUT/session-handover-ledger-host-build.log" 2>&1 \
            && cmake --build "$HOST_BUILD" -j"$BUILD_JOBS" >>"$OUT/session-handover-ledger-host-build.log" 2>&1; then
        host_objects "$HOST_OBJDIR" "$OUT/session-handover-ledger-objs.txt" \
            || build_broken "host object list is empty (the harness, not the test under it)"
        if ubq_compile_parallel "$OUT/session_handover_ledger" "$OUT/objects/session_handover_ledger" "$OUT/session-handover-ledger-build.log" \
                -std=c++23 -DUBIQUITTY_QUIC=1 -I"$QUIC_PREFIX/include" -I"$ROOT/Tests" -I"$ROOT/Source" -- \
                "$ROOT/Tests/Host/session_handover_ledger.cpp" \
                -- "@$OUT/session-handover-ledger-objs.txt" "${HOST_LINK_ARCHIVES[@]}"; then
            ubq_run_bounded 300 "$OUT/session_handover_ledger" || exit 1
        else
            build_broken "session_handover_ledger link failed (see $OUT/session-handover-ledger-build.log)"
        fi
    else
        build_broken "session_handover_ledger host build failed (see $OUT/session-handover-ledger-host-build.log)"
    fi
fi

# session_handover_facts — the real host Session preserves all portable facts;
# discarding an unpublished successor cannot unlink the current holder's files.
if wanted session_handover_facts && command -v cmake >/dev/null 2>&1; then
    echo "=== session_handover_facts ==="
    RAN=$((RAN + 1))
    HOST_BUILD="$UBQ_BUILD_ROOT/host-noquic"
    HOST_OBJDIR="$HOST_BUILD/CMakeFiles/ubiquitty-host.dir"
    if cmake -B "$HOST_BUILD" -S "$ROOT" -G "$UBQ_GENERATOR" -DUBIQUITTY_FRONTEND=Host -DCMAKE_BUILD_TYPE=Debug \
                >"$OUT/session-handover-facts-host-build.log" 2>&1 \
            && cmake --build "$HOST_BUILD" -j"$BUILD_JOBS" >>"$OUT/session-handover-facts-host-build.log" 2>&1; then
        host_objects "$HOST_OBJDIR" "$OUT/session-handover-facts-objs.txt" \
            || build_broken "host object list is empty (the harness, not the test under it)"
        if ubq_compile_parallel "$OUT/session_handover_facts" "$OUT/objects/session_handover_facts" "$OUT/session-handover-facts-build.log" \
                -std=c++23 -DUBIQUITTY_QUIC=1 -I"$QUIC_PREFIX/include" -I"$ROOT/Tests" -I"$ROOT/Source" -- \
                "$ROOT/Tests/Host/session_handover_facts.cpp" \
                -- "@$OUT/session-handover-facts-objs.txt" "${HOST_LINK_ARCHIVES[@]}"; then
            ubq_run_bounded 300 "$OUT/session_handover_facts" || exit 1
        else
            build_broken "session_handover_facts link failed (see $OUT/session-handover-facts-build.log)"
        fi
    else
        build_broken "session_handover_facts host build failed (see $OUT/session-handover-facts-host-build.log)"
    fi
fi

# session_handover — the complete host Session is captured, rebound privately,
# and resumed with original input ownership, configuration and held effects.
if wanted session_handover && command -v cmake >/dev/null 2>&1; then
    echo "=== session_handover ==="
    RAN=$((RAN + 1))
    HOST_BUILD="$UBQ_BUILD_ROOT/host-noquic"
    HOST_OBJDIR="$HOST_BUILD/CMakeFiles/ubiquitty-host.dir"
    if cmake -B "$HOST_BUILD" -S "$ROOT" -G "$UBQ_GENERATOR" -DUBIQUITTY_FRONTEND=Host -DCMAKE_BUILD_TYPE=Debug \
                >"$OUT/session-handover-host-build.log" 2>&1 \
            && cmake --build "$HOST_BUILD" -j"$BUILD_JOBS" >>"$OUT/session-handover-host-build.log" 2>&1; then
        host_objects "$HOST_OBJDIR" "$OUT/session-handover-objs.txt" \
            || build_broken "host object list is empty (the harness, not the test under it)"
        if ubq_compile_parallel "$OUT/session_handover" "$OUT/objects/session_handover" "$OUT/session-handover-build.log" \
                -std=c++23 -DUBIQUITTY_QUIC=1 -I"$QUIC_PREFIX/include" -I"$ROOT/Tests" -I"$ROOT/Source" -- \
                "$ROOT/Tests/Host/session_handover.cpp" \
                -- "@$OUT/session-handover-objs.txt" "${HOST_LINK_ARCHIVES[@]}"; then
            ubq_run_bounded 300 "$OUT/session_handover" || exit 1
        else
            build_broken "session_handover link failed (see $OUT/session-handover-build.log)"
        fi
    else
        build_broken "session_handover host build failed (see $OUT/session-handover-host-build.log)"
    fi
fi

# host_table_handover — actual host tables are staged privately and cross-bound
# to every carried session before the whole-host owner may publish them.
if wanted host_table_handover && command -v cmake >/dev/null 2>&1; then
    echo "=== host_table_handover ==="
    RAN=$((RAN + 1))
    HOST_BUILD="$UBQ_BUILD_ROOT/host-noquic"
    HOST_OBJDIR="$HOST_BUILD/CMakeFiles/ubiquitty-host.dir"
    if cmake -B "$HOST_BUILD" -S "$ROOT" -G "$UBQ_GENERATOR" -DUBIQUITTY_FRONTEND=Host -DCMAKE_BUILD_TYPE=Debug \
                >"$OUT/host-table-handover-host-build.log" 2>&1 \
            && cmake --build "$HOST_BUILD" -j"$BUILD_JOBS" >>"$OUT/host-table-handover-host-build.log" 2>&1; then
        host_objects "$HOST_OBJDIR" "$OUT/host-table-handover-objs.txt" \
            || build_broken "host object list is empty (the harness, not the test under it)"
        if ubq_compile_parallel "$OUT/host_table_handover" "$OUT/objects/host_table_handover" "$OUT/host-table-handover-build.log" \
                -std=c++23 -DUBIQUITTY_QUIC=1 -I"$QUIC_PREFIX/include" -I"$ROOT/Tests" -I"$ROOT/Source" -- \
                "$ROOT/Tests/Host/host_table_handover.cpp" \
                -- "@$OUT/host-table-handover-objs.txt" "${HOST_LINK_ARCHIVES[@]}"; then
            ubq_run_bounded 300 "$OUT/host_table_handover" || exit 1
        else
            build_broken "host_table_handover link failed (see $OUT/host-table-handover-build.log)"
        fi
    else
        build_broken "host_table_handover host build failed (see $OUT/host-table-handover-host-build.log)"
    fi
fi

# host_handover_candidate — all pane and host owners are validated and kept
# private until the whole-host service transaction can commit.
if wanted host_handover_candidate && command -v cmake >/dev/null 2>&1; then
    echo "=== host_handover_candidate ==="
    RAN=$((RAN + 1))
    HOST_BUILD="$UBQ_BUILD_ROOT/host-noquic"
    HOST_OBJDIR="$HOST_BUILD/CMakeFiles/ubiquitty-host.dir"
    if cmake -B "$HOST_BUILD" -S "$ROOT" -G "$UBQ_GENERATOR" -DUBIQUITTY_FRONTEND=Host -DCMAKE_BUILD_TYPE=Debug \
                >"$OUT/host-handover-candidate-host-build.log" 2>&1 \
            && cmake --build "$HOST_BUILD" -j"$BUILD_JOBS" >>"$OUT/host-handover-candidate-host-build.log" 2>&1; then
        host_objects "$HOST_OBJDIR" "$OUT/host-handover-candidate-objs.txt" \
            || build_broken "host object list is empty (the harness, not the test under it)"
        if ubq_compile_parallel "$OUT/host_handover_candidate" "$OUT/objects/host_handover_candidate" "$OUT/host-handover-candidate-build.log" \
                -std=c++23 -DUBIQUITTY_QUIC=1 -I"$QUIC_PREFIX/include" -I"$ROOT/Tests" -I"$ROOT/Source" -- \
                "$ROOT/Tests/Host/host_handover_candidate.cpp" \
                -- "@$OUT/host-handover-candidate-objs.txt" "${HOST_LINK_ARCHIVES[@]}"; then
            ubq_run_bounded 300 "$OUT/host_handover_candidate" || exit 1
        else
            build_broken "host_handover_candidate link failed (see $OUT/host-handover-candidate-build.log)"
        fi
    else
        build_broken "host_handover_candidate host build failed (see $OUT/host-handover-candidate-host-build.log)"
    fi
fi

# pane_focus_event — the daemon records a pane gaining the focus when a
# client's screen plane says so: once each time the pane goes from no driver
# holding it to one, by a Focus on its channel, its client's Facts or a
# holder leaving, and not while it changes hands or an observer claims it.
# The host's terminal holds the pane's focus as its drivers report it, which
# a bell's notification reads. Friend seam, same QUIC-OFF host build as
# screen_lost_silence.
if wanted pane_focus_event && command -v cmake >/dev/null 2>&1; then
    echo "=== pane_focus_event ==="
    RAN=$((RAN + 1))
    HOST_BUILD="$UBQ_BUILD_ROOT/host-noquic"
    HOST_OBJDIR="$HOST_BUILD/CMakeFiles/ubiquitty-host.dir"
    if cmake -B "$HOST_BUILD" -S "$ROOT" -G "$UBQ_GENERATOR" -DUBIQUITTY_FRONTEND=Host -DCMAKE_BUILD_TYPE=Debug \
                >"$OUT/pane-focus-event-host-build.log" 2>&1 \
            && cmake --build "$HOST_BUILD" -j"$BUILD_JOBS" >>"$OUT/pane-focus-event-host-build.log" 2>&1; then
        host_objects "$HOST_OBJDIR" "$OUT/pane-focus-event-objs.txt" \
            || build_broken "host object list is empty (the harness, not the test under it)"
        if ubq_compile_parallel "$OUT/pane_focus_event" "$OUT/objects/pane_focus_event" "$OUT/pane-focus-event-build.log" \
                -std=c++23 -DUBIQUITTY_QUIC=1 -I"$QUIC_PREFIX/include" -I"$ROOT/Tests" -I"$ROOT/Source" -- \
                "$ROOT/Tests/Host/pane_focus_event.cpp" \
                -- "@$OUT/pane-focus-event-objs.txt" "${HOST_LINK_ARCHIVES[@]}"; then
            ubq_run_bounded 300 "$OUT/pane_focus_event" || exit 1
        else
            build_broken "pane_focus_event link failed (see $OUT/pane-focus-event-build.log)"
        fi
    else
        build_broken "pane_focus_event host build failed (see $OUT/pane-focus-event-host-build.log)"
    fi
fi

# interrupt_drops_queued_input — the daemon reads what a signal character does
# from the pane's line discipline: queued input is dropped and its sender told
# where the terminal discards its own, kept under NOFLSH, and the key is data
# with signals off. Friend seam, same QUIC-OFF host build as
# screen_lost_silence.
if wanted interrupt_drops_queued_input && command -v cmake >/dev/null 2>&1; then
    echo "=== interrupt_drops_queued_input ==="
    RAN=$((RAN + 1))
    HOST_BUILD="$UBQ_BUILD_ROOT/host-noquic"
    HOST_OBJDIR="$HOST_BUILD/CMakeFiles/ubiquitty-host.dir"
    if cmake -B "$HOST_BUILD" -S "$ROOT" -G "$UBQ_GENERATOR" -DUBIQUITTY_FRONTEND=Host -DCMAKE_BUILD_TYPE=Debug \
                >"$OUT/interrupt-drops-queued-input-host-build.log" 2>&1 \
            && cmake --build "$HOST_BUILD" -j"$BUILD_JOBS" >>"$OUT/interrupt-drops-queued-input-host-build.log" 2>&1; then
        host_objects "$HOST_OBJDIR" "$OUT/interrupt-drops-queued-input-objs.txt" \
            || build_broken "host object list is empty (the harness, not the test under it)"
        if ubq_compile_parallel "$OUT/interrupt_drops_queued_input" "$OUT/objects/interrupt_drops_queued_input" "$OUT/interrupt-drops-queued-input-build.log" \
                -std=c++23 -DUBIQUITTY_QUIC=1 -I"$QUIC_PREFIX/include" -I"$ROOT/Tests" -I"$ROOT/Source" -- \
                "$ROOT/Tests/Host/interrupt_drops_queued_input.cpp" \
                -- "@$OUT/interrupt-drops-queued-input-objs.txt" "${HOST_LINK_ARCHIVES[@]}"; then
            ubq_run_bounded 300 "$OUT/interrupt_drops_queued_input" || exit 1
        else
            build_broken "interrupt_drops_queued_input link failed (see $OUT/interrupt-drops-queued-input-build.log)"
        fi
    else
        build_broken "interrupt_drops_queued_input host build failed (see $OUT/interrupt-drops-queued-input-host-build.log)"
    fi
fi

# interrupt_past_unread_paste — an 8 MiB paste into a program that reads
# nothing under a real PTY holds a window of itself on the host, in the pane
# and not in the stream's queue, and Ctrl-C then reaches the program within
# a bound with the client still attached. Friend seam, same QUIC-OFF host
# build as screen_lost_silence.
if wanted interrupt_past_unread_paste && command -v cmake >/dev/null 2>&1; then
    echo "=== interrupt_past_unread_paste ==="
    RAN=$((RAN + 1))
    HOST_BUILD="$UBQ_BUILD_ROOT/host-noquic"
    HOST_OBJDIR="$HOST_BUILD/CMakeFiles/ubiquitty-host.dir"
    if cmake -B "$HOST_BUILD" -S "$ROOT" -G "$UBQ_GENERATOR" -DUBIQUITTY_FRONTEND=Host -DCMAKE_BUILD_TYPE=Debug \
                >"$OUT/interrupt-past-unread-paste-host-build.log" 2>&1 \
            && cmake --build "$HOST_BUILD" -j"$BUILD_JOBS" >>"$OUT/interrupt-past-unread-paste-host-build.log" 2>&1; then
        host_objects "$HOST_OBJDIR" "$OUT/interrupt-past-unread-paste-objs.txt" \
            || build_broken "host object list is empty (the harness, not the test under it)"
        if ubq_compile_parallel "$OUT/interrupt_past_unread_paste" "$OUT/objects/interrupt_past_unread_paste" "$OUT/interrupt-past-unread-paste-build.log" \
                -std=c++23 -DUBIQUITTY_QUIC=1 -I"$QUIC_PREFIX/include" -I"$ROOT/Tests" -I"$ROOT/Source" -- \
                "$ROOT/Tests/Host/interrupt_past_unread_paste.cpp" \
                -- "@$OUT/interrupt-past-unread-paste-objs.txt" "${HOST_LINK_ARCHIVES[@]}"; then
            ubq_run_bounded 300 "$OUT/interrupt_past_unread_paste" || exit 1
        else
            build_broken "interrupt_past_unread_paste link failed (see $OUT/interrupt-past-unread-paste-build.log)"
        fi
    else
        build_broken "interrupt_past_unread_paste host build failed (see $OUT/interrupt-past-unread-paste-host-build.log)"
    fi
fi

# echo_one_write — a keystroke's echo, an update and the title the same output
# set, leaves the daemon in one write; and one publish of two sessions leaves
# in one write for each of two clients attached to both. Friend seam, same
# QUIC-OFF host build as two_client_pacing.
if wanted echo_one_write && command -v cmake >/dev/null 2>&1; then
    echo "=== echo_one_write ==="
    RAN=$((RAN + 1))
    HOST_BUILD="$UBQ_BUILD_ROOT/host-noquic"
    HOST_OBJDIR="$HOST_BUILD/CMakeFiles/ubiquitty-host.dir"
    if cmake -B "$HOST_BUILD" -S "$ROOT" -G "$UBQ_GENERATOR" -DUBIQUITTY_FRONTEND=Host -DCMAKE_BUILD_TYPE=Debug \
                >"$OUT/echo-one-write-host-build.log" 2>&1 \
            && cmake --build "$HOST_BUILD" -j"$BUILD_JOBS" >>"$OUT/echo-one-write-host-build.log" 2>&1; then
        host_objects "$HOST_OBJDIR" "$OUT/echo-one-write-objs.txt" \
            || build_broken "host object list is empty (the harness, not the test under it)"
        if ubq_compile_parallel "$OUT/echo_one_write" "$OUT/objects/echo_one_write" "$OUT/echo-one-write-build.log" \
                -std=c++23 -DUBIQUITTY_QUIC=1 -I"$QUIC_PREFIX/include" -I"$ROOT/Tests" -I"$ROOT/Source" -- \
                "$ROOT/Tests/Host/echo_one_write.cpp" \
                -- "@$OUT/echo-one-write-objs.txt" "${HOST_LINK_ARCHIVES[@]}"; then
            "$OUT/echo_one_write" || exit 1
        else
            build_broken "echo_one_write link failed (see $OUT/echo-one-write-build.log)"
        fi
    else
        build_broken "echo_one_write host build failed (see $OUT/echo-one-write-host-build.log)"
    fi
fi

# suspended_reader_ten_panes — a client attached to ten panes that stops
# reading holds at most one window: its socket queue stays under two capped
# frames, what a relay is handed while it acknowledges nothing stops at the
# window, what the connection retains stays within one window and each pane's
# newest records, and every pane reaches its newest picture and title once it
# reads. Friend seam, same QUIC-OFF host build as two_client_pacing.
if wanted suspended_reader_ten_panes && command -v cmake >/dev/null 2>&1; then
    echo "=== suspended_reader_ten_panes ==="
    RAN=$((RAN + 1))
    HOST_BUILD="$UBQ_BUILD_ROOT/host-noquic"
    HOST_OBJDIR="$HOST_BUILD/CMakeFiles/ubiquitty-host.dir"
    if cmake -B "$HOST_BUILD" -S "$ROOT" -G "$UBQ_GENERATOR" -DUBIQUITTY_FRONTEND=Host -DCMAKE_BUILD_TYPE=Debug \
                >"$OUT/suspended-reader-host-build.log" 2>&1 \
            && cmake --build "$HOST_BUILD" -j"$BUILD_JOBS" >>"$OUT/suspended-reader-host-build.log" 2>&1; then
        host_objects "$HOST_OBJDIR" "$OUT/suspended-reader-objs.txt" \
            || build_broken "host object list is empty (the harness, not the test under it)"
        if ubq_compile_parallel "$OUT/suspended_reader_ten_panes" "$OUT/objects/suspended_reader_ten_panes" "$OUT/suspended-reader-build.log" \
                -std=c++23 -DUBIQUITTY_QUIC=1 -I"$QUIC_PREFIX/include" -I"$ROOT/Tests" -I"$ROOT/Source" -- \
                "$ROOT/Tests/Host/suspended_reader_ten_panes.cpp" \
                -- "@$OUT/suspended-reader-objs.txt" "${HOST_LINK_ARCHIVES[@]}"; then
            ubq_run_bounded 300 "$OUT/suspended_reader_ten_panes" || exit 1
        else
            build_broken "suspended_reader_ten_panes link failed (see $OUT/suspended-reader-build.log)"
        fi
    else
        build_broken "suspended_reader_ten_panes host build failed (see $OUT/suspended-reader-host-build.log)"
    fi
fi

# flood_and_echo — a keystroke's echo leaves ahead of a flood that filled the
# client's window: input first, then deficit round-robin. Beside a flooding
# pane and inside one bound after another, the typed pane's update is the first
# the host sends from the echo's frame on, and the client holds the echo once it
# reads the first service after its acknowledgement. Friend seam, same QUIC-OFF
# host build as two_client_pacing.
if wanted flood_and_echo && command -v cmake >/dev/null 2>&1; then
    echo "=== flood_and_echo ==="
    RAN=$((RAN + 1))
    HOST_BUILD="$UBQ_BUILD_ROOT/host-noquic"
    HOST_OBJDIR="$HOST_BUILD/CMakeFiles/ubiquitty-host.dir"
    if cmake -B "$HOST_BUILD" -S "$ROOT" -G "$UBQ_GENERATOR" -DUBIQUITTY_FRONTEND=Host -DCMAKE_BUILD_TYPE=Debug \
                >"$OUT/flood-and-echo-host-build.log" 2>&1 \
            && cmake --build "$HOST_BUILD" -j"$BUILD_JOBS" >>"$OUT/flood-and-echo-host-build.log" 2>&1; then
        host_objects "$HOST_OBJDIR" "$OUT/flood-and-echo-objs.txt" \
            || build_broken "host object list is empty (the harness, not the test under it)"
        if ubq_compile_parallel "$OUT/flood_and_echo" "$OUT/objects/flood_and_echo" "$OUT/flood-and-echo-build.log" \
                -std=c++23 -DUBIQUITTY_QUIC=1 -I"$QUIC_PREFIX/include" -I"$ROOT/Tests" -I"$ROOT/Source" -- \
                "$ROOT/Tests/Host/flood_and_echo.cpp" \
                -- "@$OUT/flood-and-echo-objs.txt" "${HOST_LINK_ARCHIVES[@]}"; then
            ubq_run_bounded 300 "$OUT/flood_and_echo" || exit 1
        else
            build_broken "flood_and_echo link failed (see $OUT/flood-and-echo-build.log)"
        fi
    else
        build_broken "flood_and_echo host build failed (see $OUT/flood-and-echo-host-build.log)"
    fi
fi

# pty_writeback — the reply the program is still waiting for. Core answers a
# program's queries into the Terminal's own output buffer and the daemon writes
# them back up the pty, where the write can be refused: the child may not be
# reading (Again), may be gone (Closed/Fatal), or may take some bytes and refuse
# the rest. A dropped reply hangs the program on its own timeout with no
# diagnostic anywhere. StreamDouble::acceptBytesThenRefuse is what lets the
# shared stream double refuse a write. The load-
# bearing assertion is that the bytes arrive exactly once and in order across
# the refusal — reinjecting the whole payload rather than its undrained tail
# passes any branch-counting test and delivers the accepted prefix twice.
# Friend seam, same QUIC-OFF host build as drop_defer.
if wanted pty_writeback && command -v cmake >/dev/null 2>&1; then
    echo "=== pty_writeback ==="
    RAN=$((RAN + 1))
    HOST_BUILD="$UBQ_BUILD_ROOT/host-noquic"
    HOST_OBJDIR="$HOST_BUILD/CMakeFiles/ubiquitty-host.dir"
    if cmake -B "$HOST_BUILD" -S "$ROOT" -G "$UBQ_GENERATOR" -DUBIQUITTY_FRONTEND=Host -DCMAKE_BUILD_TYPE=Debug \
                \
                >"$OUT/writeback-host-build.log" 2>&1 \
            && cmake --build "$HOST_BUILD" -j"$BUILD_JOBS" >>"$OUT/writeback-host-build.log" 2>&1; then
        host_objects "$HOST_OBJDIR" "$OUT/writeback-objs.txt" \
            || build_broken "host object list is empty (the harness, not the test under it)"
        if ubq_compile_parallel "$OUT/pty_writeback" "$OUT/objects/pty_writeback" "$OUT/writeback-build.log" \
                -std=c++23 -DUBIQUITTY_QUIC=1 -I"$QUIC_PREFIX/include" -I"$ROOT/Tests" -I"$ROOT/Source" -- \
                "$ROOT/Tests/Host/pty_writeback.cpp" \
                -- "@$OUT/writeback-objs.txt" "${HOST_LINK_ARCHIVES[@]}"; then
            "$OUT/pty_writeback" || exit 1
        else
            build_broken "pty_writeback link failed (see $OUT/writeback-build.log)"
        fi
    else
        build_broken "pty_writeback host build failed (see $OUT/writeback-host-build.log)"
    fi
fi

# scrollback_backfill — handing a client the history it was not there for: the
# whole ring in the attach checkpoint, live history as appends named by
# absolute line, a ring that turned over or was cleared sent whole. The failure
# mode reports nothing — a mirror holding the wrong history, assembled from
# well-formed frames — so this drives the real consumer: the daemon's bytes go
# into the client end of a Sync connection and the assertion compares its
# replica's scrollback against the host's. Friend seam, same QUIC-OFF host
# build as drop_defer.
if wanted scrollback_backfill && command -v cmake >/dev/null 2>&1; then
    echo "=== scrollback_backfill ==="
    RAN=$((RAN + 1))
    HOST_BUILD="$UBQ_BUILD_ROOT/host-noquic"
    HOST_OBJDIR="$HOST_BUILD/CMakeFiles/ubiquitty-host.dir"
    if cmake -B "$HOST_BUILD" -S "$ROOT" -G "$UBQ_GENERATOR" -DUBIQUITTY_FRONTEND=Host -DCMAKE_BUILD_TYPE=Debug \
                \
                >"$OUT/backfill-host-build.log" 2>&1 \
            && cmake --build "$HOST_BUILD" -j"$BUILD_JOBS" >>"$OUT/backfill-host-build.log" 2>&1; then
        host_objects "$HOST_OBJDIR" "$OUT/backfill-objs.txt" \
            || build_broken "host object list is empty (the harness, not the test under it)"
        if ubq_compile_parallel "$OUT/scrollback_backfill" "$OUT/objects/scrollback_backfill" "$OUT/backfill-build.log" \
                -std=c++23 -DUBIQUITTY_QUIC=1 -I"$QUIC_PREFIX/include" -I"$ROOT/Tests" -I"$ROOT/Source" -- \
                "$ROOT/Tests/Host/scrollback_backfill.cpp" \
                -- "@$OUT/backfill-objs.txt" "${HOST_LINK_ARCHIVES[@]}"; then
            "$OUT/scrollback_backfill" || exit 1
        else
            build_broken "scrollback_backfill link failed (see $OUT/backfill-build.log)"
        fi
    else
        build_broken "scrollback_backfill host build failed (see $OUT/backfill-host-build.log)"
    fi
fi

# sync_mirror_clear_and_reflow — the window's mirror after Cmd-K and after a
# narrowing resize, both reported wrong over Sync. The daemon's frames go into
# the client end of a Sync connection, the host alone clears and rewraps, and
# every row, history included, and the cursor are compared against the host's
# Terminal. Same friend seam and QUIC-OFF host build as scrollback_backfill.
if wanted sync_mirror_clear_and_reflow && command -v cmake >/dev/null 2>&1; then
    echo "=== sync_mirror_clear_and_reflow ==="
    RAN=$((RAN + 1))
    HOST_BUILD="$UBQ_BUILD_ROOT/host-noquic"
    HOST_OBJDIR="$HOST_BUILD/CMakeFiles/ubiquitty-host.dir"
    if cmake -B "$HOST_BUILD" -S "$ROOT" -G "$UBQ_GENERATOR" -DUBIQUITTY_FRONTEND=Host -DCMAKE_BUILD_TYPE=Debug \
                \
                >"$OUT/syncmirror-host-build.log" 2>&1 \
            && cmake --build "$HOST_BUILD" -j"$BUILD_JOBS" >>"$OUT/syncmirror-host-build.log" 2>&1; then
        host_objects "$HOST_OBJDIR" "$OUT/syncmirror-objs.txt" \
            || build_broken "host object list is empty (the harness, not the test under it)"
        if ubq_compile_parallel "$OUT/sync_mirror_clear_and_reflow" "$OUT/objects/sync_mirror_clear_and_reflow" "$OUT/syncmirror-build.log" \
                -std=c++23 -DUBIQUITTY_QUIC=1 -I"$QUIC_PREFIX/include" -I"$ROOT/Tests" -I"$ROOT/Source" -- \
                "$ROOT/Tests/Host/sync_mirror_clear_and_reflow.cpp" \
                -- "@$OUT/syncmirror-objs.txt" "${HOST_LINK_ARCHIVES[@]}"; then
            ubq_run_bounded 300 "$OUT/sync_mirror_clear_and_reflow" || exit 1
        else
            build_broken "sync_mirror_clear_and_reflow link failed (see $OUT/syncmirror-build.log)"
        fi
    else
        build_broken "sync_mirror_clear_and_reflow host build failed (see $OUT/syncmirror-host-build.log)"
    fi
fi

# service_pty_turn — one turn of the PTY pump. servicePTY turns a program's
# output into everything else, including the consent-prompt ASK side. The answer
# side has a coverer (capability_consent.cpp injects a pending record by hand),
# but an injected record tests what the daemon does with a human's ruling and
# skips the code that mints the request id, records what was actually asked,
# and picks whom to ask. Pins that, plus the read
# loop's four exits and the stale-mouse-tracking guard. Friend seam, same
# QUIC-OFF host build as drop_defer.
if wanted service_pty_turn && command -v cmake >/dev/null 2>&1; then
    echo "=== service_pty_turn ==="
    RAN=$((RAN + 1))
    HOST_BUILD="$UBQ_BUILD_ROOT/host-noquic"
    HOST_OBJDIR="$HOST_BUILD/CMakeFiles/ubiquitty-host.dir"
    if cmake -B "$HOST_BUILD" -S "$ROOT" -G "$UBQ_GENERATOR" -DUBIQUITTY_FRONTEND=Host -DCMAKE_BUILD_TYPE=Debug \
                \
                >"$OUT/servicepty-host-build.log" 2>&1 \
            && cmake --build "$HOST_BUILD" -j"$BUILD_JOBS" >>"$OUT/servicepty-host-build.log" 2>&1; then
        host_objects "$HOST_OBJDIR" "$OUT/servicepty-objs.txt" \
            || build_broken "host object list is empty (the harness, not the test under it)"
        if ubq_compile_parallel "$OUT/service_pty_turn" "$OUT/objects/service_pty_turn" "$OUT/servicepty-build.log" \
                -std=c++23 -DUBIQUITTY_QUIC=1 -I"$QUIC_PREFIX/include" -I"$ROOT/Tests" -I"$ROOT/Source" -- \
                "$ROOT/Tests/Host/service_pty_turn.cpp" \
                -- "@$OUT/servicepty-objs.txt" "${HOST_LINK_ARCHIVES[@]}"; then
            "$OUT/service_pty_turn" || exit 1
        else
            build_broken "service_pty_turn link failed (see $OUT/servicepty-build.log)"
        fi
    else
        build_broken "service_pty_turn host build failed (see $OUT/servicepty-host-build.log)"
    fi
fi

# consent_remote_clipboard — the owner's phone copies from a remote program
# with OSC 52. A remote clipboard write defaults to Prompt rather than the
# host-escaping tier's Deny, and this closes the loop on the daemon: the write
# is held and the phone (the human on the session) is asked once, its allow
# delivers that exact write to the Terminal's clipboard sink without a retry.
# Denial, expiry and revocation discard it. Friend seam, same host build as
# service_pty_turn.
if wanted consent_remote_clipboard && command -v cmake >/dev/null 2>&1; then
    echo "=== consent_remote_clipboard ==="
    RAN=$((RAN + 1))
    HOST_BUILD="$UBQ_BUILD_ROOT/host-noquic"
    HOST_OBJDIR="$HOST_BUILD/CMakeFiles/ubiquitty-host.dir"
    if cmake -B "$HOST_BUILD" -S "$ROOT" -G "$UBQ_GENERATOR" -DUBIQUITTY_FRONTEND=Host -DCMAKE_BUILD_TYPE=Debug \
                \
                >"$OUT/remoteclipboard-host-build.log" 2>&1 \
            && cmake --build "$HOST_BUILD" -j"$BUILD_JOBS" >>"$OUT/remoteclipboard-host-build.log" 2>&1; then
        host_objects "$HOST_OBJDIR" "$OUT/remoteclipboard-objs.txt" \
            || build_broken "host object list is empty (the harness, not the test under it)"
        if ubq_compile_parallel "$OUT/consent_remote_clipboard" "$OUT/objects/consent_remote_clipboard" "$OUT/remoteclipboard-build.log" \
                -std=c++23 -DUBIQUITTY_QUIC=1 -I"$QUIC_PREFIX/include" -I"$ROOT/Tests" -I"$ROOT/Source" -- \
                "$ROOT/Tests/Host/consent_remote_clipboard.cpp" \
                -- "@$OUT/remoteclipboard-objs.txt" "${HOST_LINK_ARCHIVES[@]}"; then
            "$OUT/consent_remote_clipboard" || exit 1
        else
            build_broken "consent_remote_clipboard link failed (see $OUT/remoteclipboard-build.log)"
        fi
    else
        build_broken "consent_remote_clipboard host build failed (see $OUT/remoteclipboard-host-build.log)"
    fi
fi

# consent_remote_effects — what a remote program copies and announces reaches
# the person who runs it. The host has no pasteboard, so an allowed OSC 52 write
# or OSC 9 banner leaves the daemon as CLIPBOARD_WRITE / NOTIFICATION to the
# session's primary human and to no agent; the phone's own "Always" for those
# two is kept, and every other host-escaping grant stays single-use. Friend
# seam, same QUIC-OFF host build as service_pty_turn.
if wanted consent_remote_effects && command -v cmake >/dev/null 2>&1; then
    echo "=== consent_remote_effects ==="
    RAN=$((RAN + 1))
    HOST_BUILD="$UBQ_BUILD_ROOT/host-noquic"
    HOST_OBJDIR="$HOST_BUILD/CMakeFiles/ubiquitty-host.dir"
    if cmake -B "$HOST_BUILD" -S "$ROOT" -G "$UBQ_GENERATOR" -DUBIQUITTY_FRONTEND=Host -DCMAKE_BUILD_TYPE=Debug \
                \
                >"$OUT/remoteeffects-host-build.log" 2>&1 \
            && cmake --build "$HOST_BUILD" -j"$BUILD_JOBS" >>"$OUT/remoteeffects-host-build.log" 2>&1; then
        host_objects "$HOST_OBJDIR" "$OUT/remoteeffects-objs.txt" \
            || build_broken "host object list is empty (the harness, not the test under it)"
        if ubq_compile_parallel "$OUT/consent_remote_effects" "$OUT/objects/consent_remote_effects" "$OUT/remoteeffects-build.log" \
                -std=c++23 -DUBIQUITTY_QUIC=1 -I"$QUIC_PREFIX/include" -I"$ROOT/Tests" -I"$ROOT/Source" -- \
                "$ROOT/Tests/Host/consent_remote_effects.cpp" \
                -- "@$OUT/remoteeffects-objs.txt" "${HOST_LINK_ARCHIVES[@]}"; then
            "$OUT/consent_remote_effects" || exit 1
        else
            build_broken "consent_remote_effects link failed (see $OUT/remoteeffects-build.log)"
        fi
    else
        build_broken "consent_remote_effects host build failed (see $OUT/remoteeffects-host-build.log)"
    fi
fi

# resize_focus_frames — the two frames a viewer sends about its own window.
# Viewport and Focus are the messages where a client reports something the
# host cannot see for itself, and the host acts on the report by DRIVING the
# session, which is why both are a driver's alone. Pins the immediate CSI I/O
# drain (an idle session never reads, so a deferred report announces stale
# focus), the single clamp feeding the PTY and Core alike, a watcher's and a
# truncated viewport sizing nothing, and the checkpoint every client of a
# resized session takes, so none holds a history it rewrapped itself. Friend
# seam, same QUIC-OFF host build as drop_defer.
if wanted resize_focus_frames && command -v cmake >/dev/null 2>&1; then
    echo "=== resize_focus_frames ==="
    RAN=$((RAN + 1))
    HOST_BUILD="$UBQ_BUILD_ROOT/host-noquic"
    HOST_OBJDIR="$HOST_BUILD/CMakeFiles/ubiquitty-host.dir"
    if cmake -B "$HOST_BUILD" -S "$ROOT" -G "$UBQ_GENERATOR" -DUBIQUITTY_FRONTEND=Host -DCMAKE_BUILD_TYPE=Debug \
                \
                >"$OUT/resizefocus-host-build.log" 2>&1 \
            && cmake --build "$HOST_BUILD" -j"$BUILD_JOBS" >>"$OUT/resizefocus-host-build.log" 2>&1; then
        host_objects "$HOST_OBJDIR" "$OUT/resizefocus-objs.txt" \
            || build_broken "host object list is empty (the harness, not the test under it)"
        if ubq_compile_parallel "$OUT/resize_focus_frames" "$OUT/objects/resize_focus_frames" "$OUT/resizefocus-build.log" \
                -std=c++23 -DUBIQUITTY_QUIC=1 -I"$QUIC_PREFIX/include" -I"$ROOT/Tests" -I"$ROOT/Source" -- \
                "$ROOT/Tests/Host/resize_focus_frames.cpp" \
                -- "@$OUT/resizefocus-objs.txt" "${HOST_LINK_ARCHIVES[@]}"; then
            ubq_run_bounded 300 "$OUT/resize_focus_frames" || exit 1
        else
            build_broken "resize_focus_frames link failed (see $OUT/resizefocus-build.log)"
        fi
    else
        build_broken "resize_focus_frames host build failed (see $OUT/resizefocus-host-build.log)"
    fi
fi

# region_read_text — what an agent reads back through typed pane.capture.
# Pins the extracted text (combining marks with their base, a wide glyph once,
# trailing blanks
# trimmed, scrollback addressed absolutely), the OSC-133 walk behind
# last-command capture, and the rule that the range a reply NAMES is the range it
# delivered — including when the 1 MiB cap cuts it short, which is silent data
# loss for a paging agent otherwise. Calls cross the real inner transport.
if wanted region_read_text && command -v cmake >/dev/null 2>&1; then
    echo "=== region_read_text ==="
    RAN=$((RAN + 1))
    HOST_BUILD="$UBQ_BUILD_ROOT/host-noquic"
    HOST_OBJDIR="$HOST_BUILD/CMakeFiles/ubiquitty-host.dir"
    if cmake -B "$HOST_BUILD" -S "$ROOT" -G "$UBQ_GENERATOR" -DUBIQUITTY_FRONTEND=Host -DCMAKE_BUILD_TYPE=Debug \
                \
                >"$OUT/regionread-host-build.log" 2>&1 \
            && cmake --build "$HOST_BUILD" -j"$BUILD_JOBS" >>"$OUT/regionread-host-build.log" 2>&1; then
        host_objects "$HOST_OBJDIR" "$OUT/regionread-objs.txt" \
            || build_broken "host object list is empty (the harness, not the test under it)"
        if ubq_compile_parallel "$OUT/region_read_text" "$OUT/objects/region_read_text" "$OUT/regionread-build.log" \
                -std=c++23 -DUBIQUITTY_QUIC=1 -I"$QUIC_PREFIX/include" -I"$ROOT/Tests" -I"$ROOT/Source" -- \
                "$ROOT/Tests/Host/region_read_text.cpp" \
                -- "@$OUT/regionread-objs.txt" "${HOST_LINK_ARCHIVES[@]}"; then
            "$OUT/region_read_text" || exit 1
        else
            build_broken "region_read_text link failed (see $OUT/regionread-build.log)"
        fi
    else
        build_broken "region_read_text host build failed (see $OUT/regionread-host-build.log)"
    fi
fi

# event_stream — the host's event stream as a subscriber sees it: one
# SUBSCRIBE_EVENTS, then a pushed EVENT per record. The daemon counts every
# delivery, so the cases pin that nothing is delivered while nothing happens
# (a waiter never polls), that a resumed subscriber gets exactly what followed
# its cursor, that one the bounded log outran is told how many it lost, that an
# OSC 7501 report is a status event, and that a client without Control is
# streamed nothing. Friend seam, same QUIC-OFF host build as region_read_text.
if wanted event_stream && command -v cmake >/dev/null 2>&1; then
    echo "=== event_stream ==="
    RAN=$((RAN + 1))
    HOST_BUILD="$UBQ_BUILD_ROOT/host-noquic"
    HOST_OBJDIR="$HOST_BUILD/CMakeFiles/ubiquitty-host.dir"
    if cmake -B "$HOST_BUILD" -S "$ROOT" -G "$UBQ_GENERATOR" -DUBIQUITTY_FRONTEND=Host -DCMAKE_BUILD_TYPE=Debug \
                >"$OUT/eventstream-host-build.log" 2>&1 \
            && cmake --build "$HOST_BUILD" -j"$BUILD_JOBS" >>"$OUT/eventstream-host-build.log" 2>&1; then
        host_objects "$HOST_OBJDIR" "$OUT/eventstream-objs.txt" \
            || build_broken "host object list is empty (the harness, not the test under it)"
        if ubq_compile_parallel "$OUT/event_stream" "$OUT/objects/event_stream" "$OUT/eventstream-build.log" \
                -std=c++23 -DUBIQUITTY_QUIC=1 -I"$QUIC_PREFIX/include" -I"$ROOT/Tests" -I"$ROOT/Source" -- \
                "$ROOT/Tests/Host/event_stream.cpp" \
                -- "@$OUT/eventstream-objs.txt" "${HOST_LINK_ARCHIVES[@]}"; then
            "$OUT/event_stream" || exit 1
        else
            build_broken "event_stream link failed (see $OUT/eventstream-build.log)"
        fi
    else
        build_broken "event_stream host build failed (see $OUT/eventstream-host-build.log)"
    fi
fi

# hello_auth_gate — the front door. HELLO is where a client becomes a
# principal, and everything downstream reads the scope this handshake stamped.
# Three arms must agree on one rule: a client leaves HELLO with no more
# authority than it proved — a verified token's scope, a QUIC preamble's
# CARRIED scope (never widened), or nothing at all, which only the
# trusted local bridge may present under require_token. Friend seam,
# same QUIC-OFF host build as drop_defer.
if wanted hello_auth_gate && command -v cmake >/dev/null 2>&1; then
    echo "=== hello_auth_gate ==="
    RAN=$((RAN + 1))
    HOST_BUILD="$UBQ_BUILD_ROOT/host-noquic"
    HOST_OBJDIR="$HOST_BUILD/CMakeFiles/ubiquitty-host.dir"
    if cmake -B "$HOST_BUILD" -S "$ROOT" -G "$UBQ_GENERATOR" -DUBIQUITTY_FRONTEND=Host -DCMAKE_BUILD_TYPE=Debug \
                \
                >"$OUT/helloauth-host-build.log" 2>&1 \
            && cmake --build "$HOST_BUILD" -j"$BUILD_JOBS" >>"$OUT/helloauth-host-build.log" 2>&1; then
        host_objects "$HOST_OBJDIR" "$OUT/helloauth-objs.txt" \
            || build_broken "host object list is empty (the harness, not the test under it)"
        if ubq_compile_parallel "$OUT/hello_auth_gate" "$OUT/objects/hello_auth_gate" "$OUT/helloauth-build.log" \
                -std=c++23 -DUBIQUITTY_QUIC=1 -I"$QUIC_PREFIX/include" -I"$ROOT/Tests" -I"$ROOT/Source" -- \
                "$ROOT/Tests/Host/hello_auth_gate.cpp" \
                -- "@$OUT/helloauth-objs.txt" "${HOST_LINK_ARCHIVES[@]}"; then
            "$OUT/hello_auth_gate" || exit 1
        else
            build_broken "hello_auth_gate link failed (see $OUT/helloauth-build.log)"
        fi
    else
        build_broken "hello_auth_gate host build failed (see $OUT/helloauth-host-build.log)"
    fi
fi

# revoke_consent_actions — the human's side of the consent system. Four revoke
# actions, each of which must do only its own work: a revoke that kicks the
# wrong client disconnects an innocent one, and one that forgets the wrong
# grant leaves the named agent allowed. The gates in front matter as much — the
# sender must be the human (by capability origin, never a self-asserted flag),
# must hold Presence, and must name its own session, or one session's human
# reaches into another's. Friend seam, same QUIC-OFF host build as drop_defer.
if wanted revoke_consent_actions && command -v cmake >/dev/null 2>&1; then
    echo "=== revoke_consent_actions ==="
    RAN=$((RAN + 1))
    HOST_BUILD="$UBQ_BUILD_ROOT/host-noquic"
    HOST_OBJDIR="$HOST_BUILD/CMakeFiles/ubiquitty-host.dir"
    if cmake -B "$HOST_BUILD" -S "$ROOT" -G "$UBQ_GENERATOR" -DUBIQUITTY_FRONTEND=Host -DCMAKE_BUILD_TYPE=Debug \
                \
                >"$OUT/revoke-host-build.log" 2>&1 \
            && cmake --build "$HOST_BUILD" -j"$BUILD_JOBS" >>"$OUT/revoke-host-build.log" 2>&1; then
        host_objects "$HOST_OBJDIR" "$OUT/revoke-objs.txt" \
            || build_broken "host object list is empty (the harness, not the test under it)"
        if ubq_compile_parallel "$OUT/revoke_consent_actions" "$OUT/objects/revoke_consent_actions" "$OUT/revoke-build.log" \
                -std=c++23 -DUBIQUITTY_QUIC=1 -I"$QUIC_PREFIX/include" -I"$ROOT/Tests" -I"$ROOT/Source" -- \
                "$ROOT/Tests/Host/revoke_consent_actions.cpp" \
                -- "@$OUT/revoke-objs.txt" "${HOST_LINK_ARCHIVES[@]}"; then
            "$OUT/revoke_consent_actions" || exit 1
        else
            build_broken "revoke_consent_actions link failed (see $OUT/revoke-build.log)"
        fi
    else
        build_broken "revoke_consent_actions host build failed (see $OUT/revoke-host-build.log)"
    fi
fi

# message_gate_matrix — every gated message type, against every principal. What
# keeps one client out of another's session is a HELLO capability bit plus a
# token scope, and the scope check has an origin escape hatch (a Local client
# passes without a token). Both halves fail OPEN when wrong — a bare-HELLO
# remote peer passing every scope gate, an unlabeled local peer classified
# Local — and neither is visible black-box, because a black-box driver
# connects as exactly one principal. This
# links the daemon objects and drives the whole matrix through a friend seam.
# Same QUIC-OFF host build as drop_defer.
if wanted message_gate_matrix && command -v cmake >/dev/null 2>&1; then
    echo "=== message_gate_matrix ==="
    RAN=$((RAN + 1))
    HOST_BUILD="$UBQ_BUILD_ROOT/host-noquic"
    HOST_OBJDIR="$HOST_BUILD/CMakeFiles/ubiquitty-host.dir"
    if cmake -B "$HOST_BUILD" -S "$ROOT" -G "$UBQ_GENERATOR" -DUBIQUITTY_FRONTEND=Host -DCMAKE_BUILD_TYPE=Debug \
                \
                >"$OUT/gatematrix-host-build.log" 2>&1 \
            && cmake --build "$HOST_BUILD" -j"$BUILD_JOBS" >>"$OUT/gatematrix-host-build.log" 2>&1; then
        host_objects "$HOST_OBJDIR" "$OUT/gatematrix-objs.txt" \
            || build_broken "host object list is empty (the harness, not the test under it)"
        if ubq_compile_parallel "$OUT/message_gate_matrix" "$OUT/objects/message_gate_matrix" "$OUT/gatematrix-build.log" \
                -std=c++23 -DUBIQUITTY_QUIC=1 -I"$QUIC_PREFIX/include" -I"$ROOT/Tests" -I"$ROOT/Source" -- \
                "$ROOT/Tests/Host/message_gate_matrix.cpp" \
                -- "@$OUT/gatematrix-objs.txt" "${HOST_LINK_ARCHIVES[@]}"; then
            "$OUT/message_gate_matrix" || exit 1
        else
            build_broken "message_gate_matrix link failed (see $OUT/gatematrix-build.log)"
        fi
    else
        build_broken "message_gate_matrix host build failed (see $OUT/gatematrix-host-build.log)"
    fi
fi

# paste_push_landing — credited paste.push lands the payload as a temp file and
# types its path into the session's PTY, bracketed exactly when the program
# asked. Friend-seam driven: the temp directory and the PTY side of the
# injection are both invisible to a black-box driver. Same QUIC-OFF host
# build as the gate matrix.
if wanted paste_push_landing && command -v cmake >/dev/null 2>&1; then
    echo "=== paste_push_landing ==="
    RAN=$((RAN + 1))
    HOST_BUILD="$UBQ_BUILD_ROOT/host-noquic"
    HOST_OBJDIR="$HOST_BUILD/CMakeFiles/ubiquitty-host.dir"
    if cmake -B "$HOST_BUILD" -S "$ROOT" -G "$UBQ_GENERATOR" -DUBIQUITTY_FRONTEND=Host -DCMAKE_BUILD_TYPE=Debug \
                \
                >"$OUT/pastepush-host-build.log" 2>&1 \
            && cmake --build "$HOST_BUILD" -j"$BUILD_JOBS" >>"$OUT/pastepush-host-build.log" 2>&1; then
        host_objects "$HOST_OBJDIR" "$OUT/pastepush-objs.txt" \
            || build_broken "host object list is empty (the harness, not the test under it)"
        if ubq_compile_parallel "$OUT/paste_push_landing" "$OUT/objects/paste_push_landing" "$OUT/pastepush-build.log" \
                -std=c++23 -DUBIQUITTY_QUIC=1 -I"$QUIC_PREFIX/include" -I"$ROOT/Tests" -I"$ROOT/Source" -- \
                "$ROOT/Tests/Host/paste_push_landing.cpp" \
                -- "@$OUT/pastepush-objs.txt" "${HOST_LINK_ARCHIVES[@]}"; then
        "$OUT/paste_push_landing" || exit 1
        else
            build_broken "paste_push_landing link failed (see $OUT/pastepush-build.log)"
        fi
    else
        build_broken "paste_push_landing host build failed (see $OUT/pastepush-host-build.log)"
    fi
fi

# origin_staleness — program OUTPUT is gated at the attached drivers' origin, not
# the stale last-input origin / the Local default. servicePTY recomputes
# the most-restrictive attached origin (Remote>Agent>Local) and stamps the Terminal
# before the drain. Driving the live wire would need a real remote attach + a
# program emitting DECUDK autonomously mid-drain, so this links the daemon objects
# and unit-tests the recompute-then-stamp via a friend seam, then feeds a real
# DECUDK through Core to read the gate's verdict. Same QUIC-OFF host build as
# drop_defer (Daemon.cpp is QUIC-agnostic; HostQUIC.cpp is an empty TU at QUIC-OFF).
if wanted origin_staleness && command -v cmake >/dev/null 2>&1; then
    echo "=== origin_staleness ==="
    RAN=$((RAN + 1))
    HOST_BUILD="$UBQ_BUILD_ROOT/host-noquic"
    HOST_OBJDIR="$HOST_BUILD/CMakeFiles/ubiquitty-host.dir"
    if cmake -B "$HOST_BUILD" -S "$ROOT" -G "$UBQ_GENERATOR" -DUBIQUITTY_FRONTEND=Host -DCMAKE_BUILD_TYPE=Debug \
                \
                >"$OUT/origin-host-build.log" 2>&1 \
            && cmake --build "$HOST_BUILD" -j"$BUILD_JOBS" >>"$OUT/origin-host-build.log" 2>&1; then
        host_objects "$HOST_OBJDIR" "$OUT/origin-objs.txt" \
            || build_broken "host object list is empty (the harness, not the test under it)"
        if ubq_compile_parallel "$OUT/origin_staleness" "$OUT/objects/origin_staleness" "$OUT/origin-build.log" \
                -std=c++23 -DUBIQUITTY_QUIC=1 -I"$QUIC_PREFIX/include" -I"$ROOT/Tests" -I"$ROOT/Source" -- \
                "$ROOT/Tests/Host/origin_staleness.cpp" \
                -- "@$OUT/origin-objs.txt" "${HOST_LINK_ARCHIVES[@]}"; then
            "$OUT/origin_staleness" || exit 1
        else
            build_broken "origin_staleness link failed (see $OUT/origin-build.log)"
        fi
    else
        build_broken "origin_staleness host build failed (see $OUT/origin-host-build.log)"
    fi
fi

# revoke_disconnect — consent.revoke (forgetAlways + forgetSession) disconnects a live
# agent, not just block a future reconnect. An agent attached under a now-revoked grant
# keeps acting unless the revoke handler kicks it. A reverse kick loop fires after
# each grant clear; this negative proves both paths via a friend seam: a human+agent
# fixture on a shared session, the grant seeded (always-allow or session-grant), a real
# canonical Call driven through receiveScreenBytes/service, then the agent observed overflowed
# (kickClient → deferred reap) while the human is untouched. Same QUIC-OFF host build
# as drop_defer (revoke is QUIC-agnostic; HostQUIC.cpp is an empty TU at QUIC-OFF).
if wanted revoke_disconnect && command -v cmake >/dev/null 2>&1; then
    echo "=== revoke_disconnect ==="
    RAN=$((RAN + 1))
    HOST_BUILD="$UBQ_BUILD_ROOT/host-noquic"
    HOST_OBJDIR="$HOST_BUILD/CMakeFiles/ubiquitty-host.dir"
    if cmake -B "$HOST_BUILD" -S "$ROOT" -G "$UBQ_GENERATOR" -DUBIQUITTY_FRONTEND=Host -DCMAKE_BUILD_TYPE=Debug \
                \
                >"$OUT/revokedisc-host-build.log" 2>&1 \
            && cmake --build "$HOST_BUILD" -j"$BUILD_JOBS" >>"$OUT/revokedisc-host-build.log" 2>&1; then
        host_objects "$HOST_OBJDIR" "$OUT/revokedisc-objs.txt" \
            || build_broken "host object list is empty (the harness, not the test under it)"
        if ubq_compile_parallel "$OUT/revoke_disconnect" "$OUT/objects/revoke_disconnect" "$OUT/revokedisc-build.log" \
                -std=c++23 -DUBIQUITTY_QUIC=1 -I"$QUIC_PREFIX/include" -I"$ROOT/Tests" -I"$ROOT/Source" -- \
                "$ROOT/Tests/Host/revoke_disconnect.cpp" \
                -- "@$OUT/revokedisc-objs.txt" "${HOST_LINK_ARCHIVES[@]}"; then
            "$OUT/revoke_disconnect" || exit 1
        else
            build_broken "revoke_disconnect link failed (see $OUT/revokedisc-build.log)"
        fi
    else
        build_broken "revoke_disconnect host build failed (see $OUT/revokedisc-host-build.log)"
    fi
fi

# hello_reentry_guard — any HELLO after the first on one connection is rejected
# outright (AUTH_DENIED Duplicate + deferred reap). Accepted, a second HELLO
# would escalate a narrow (Control-only) token to full session access via
# sticky hasToken/tokenScope + a scope gate that only runs when the message
# itself carries a token. This drives two real HELLOs through handleFrame
# on one client: #1 with a genuine Control-only token (control-only session,
# succeeds); #2 tokenless, targeting a pre-seeded UNATTENDED session by its real
# id (no live PTY — a stub EOF stream, mirroring origin_staleness's fixture) —
# proving the client is never bound to that session and gets AUTH_DENIED, not the
# screen. Same QUIC-OFF host build as origin_staleness/revoke_disconnect.
if wanted hello_reentry_guard && command -v cmake >/dev/null 2>&1; then
    echo "=== hello_reentry_guard ==="
    RAN=$((RAN + 1))
    HOST_BUILD="$UBQ_BUILD_ROOT/host-noquic"
    HOST_OBJDIR="$HOST_BUILD/CMakeFiles/ubiquitty-host.dir"
    if cmake -B "$HOST_BUILD" -S "$ROOT" -G "$UBQ_GENERATOR" -DUBIQUITTY_FRONTEND=Host -DCMAKE_BUILD_TYPE=Debug \
                \
                >"$OUT/helloreentry-host-build.log" 2>&1 \
            && cmake --build "$HOST_BUILD" -j"$BUILD_JOBS" >>"$OUT/helloreentry-host-build.log" 2>&1; then
        host_objects "$HOST_OBJDIR" "$OUT/helloreentry-objs.txt" \
            || build_broken "host object list is empty (the harness, not the test under it)"
        if ubq_compile_parallel "$OUT/hello_reentry_guard" "$OUT/objects/hello_reentry_guard" "$OUT/helloreentry-build.log" \
                -std=c++23 -DUBIQUITTY_QUIC=1 -I"$QUIC_PREFIX/include" -I"$ROOT/Tests" -I"$ROOT/Source" -- \
                "$ROOT/Tests/Host/hello_reentry_guard.cpp" \
                -- "@$OUT/helloreentry-objs.txt" "${HOST_LINK_ARCHIVES[@]}"; then
            "$OUT/hello_reentry_guard" || exit 1
        else
            build_broken "hello_reentry_guard link failed (see $OUT/helloreentry-build.log)"
        fi
    else
        build_broken "hello_reentry_guard host build failed (see $OUT/helloreentry-host-build.log)"
    fi
fi

# hello_wire_version_skew — version ranges, not equality: a HELLO below the
# host's minimum, or ahead with a minimum past the host, is refused with
# Version, naming the host's range, app version and platform, never bound, and
# marked for reap; a HELLO in range is admitted at the lower version, and a host
# one additive version ahead never sends an older client the frame kind that
# version introduced. Real HELLOs through the production handleFrame. Same
# QUIC-OFF host build as hello_reentry_guard.
if wanted hello_wire_version_skew && command -v cmake >/dev/null 2>&1; then
    echo "=== hello_wire_version_skew ==="
    RAN=$((RAN + 1))
    HOST_BUILD="$UBQ_BUILD_ROOT/host-noquic"
    HOST_OBJDIR="$HOST_BUILD/CMakeFiles/ubiquitty-host.dir"
    if cmake -B "$HOST_BUILD" -S "$ROOT" -G "$UBQ_GENERATOR" -DUBIQUITTY_FRONTEND=Host -DCMAKE_BUILD_TYPE=Debug \
                \
                >"$OUT/hellowireversion-host-build.log" 2>&1 \
            && cmake --build "$HOST_BUILD" -j"$BUILD_JOBS" >>"$OUT/hellowireversion-host-build.log" 2>&1; then
        host_objects "$HOST_OBJDIR" "$OUT/hellowireversion-objs.txt" \
            || build_broken "host object list is empty (the harness, not the test under it)"
        if ubq_compile_parallel "$OUT/hello_wire_version_skew" "$OUT/objects/hello_wire_version_skew" "$OUT/hellowireversion-build.log" \
                -std=c++23 -DUBIQUITTY_QUIC=1 -I"$QUIC_PREFIX/include" -I"$ROOT/Tests" -I"$ROOT/Source" -- \
                "$ROOT/Tests/Host/hello_wire_version_skew.cpp" \
                -- "@$OUT/hellowireversion-objs.txt" "${HOST_LINK_ARCHIVES[@]}"; then
            "$OUT/hello_wire_version_skew" || exit 1
        else
            build_broken "hello_wire_version_skew link failed (see $OUT/hellowireversion-build.log)"
        fi
    else
        build_broken "hello_wire_version_skew host build failed (see $OUT/hellowireversion-host-build.log)"
    fi
fi

# hello_attach_only — a non-zero sessionID means reattach-or-create, which is
# what a window wants and what an errand about an existing session must never
# get: a terminate that binds its HELLO to the session it means to end CREATES
# a session that has already exited and then ends it, and the errand reports
# success for it.
if wanted hello_attach_only && command -v cmake >/dev/null 2>&1; then
    echo "=== hello_attach_only ==="
    RAN=$((RAN + 1))
    HOST_BUILD="$UBQ_BUILD_ROOT/host-noquic"
    HOST_OBJDIR="$HOST_BUILD/CMakeFiles/ubiquitty-host.dir"
    if cmake -B "$HOST_BUILD" -S "$ROOT" -G "$UBQ_GENERATOR" -DUBIQUITTY_FRONTEND=Host -DCMAKE_BUILD_TYPE=Debug \
                \
                >"$OUT/helloattachonly-host-build.log" 2>&1 \
            && cmake --build "$HOST_BUILD" -j"$BUILD_JOBS" >>"$OUT/helloattachonly-host-build.log" 2>&1; then
        host_objects "$HOST_OBJDIR" "$OUT/helloattachonly-objs.txt" \
            || build_broken "host object list is empty (the harness, not the test under it)"
        if ubq_compile_parallel "$OUT/hello_attach_only" "$OUT/objects/hello_attach_only" "$OUT/helloattachonly-build.log" \
                -std=c++23 -DUBIQUITTY_QUIC=1 -I"$QUIC_PREFIX/include" -I"$ROOT/Tests" -I"$ROOT/Source" -- \
                "$ROOT/Tests/Host/hello_attach_only.cpp" \
                -- "@$OUT/helloattachonly-objs.txt" "${HOST_LINK_ARCHIVES[@]}"; then
            "$OUT/hello_attach_only" || exit 1
        else
            build_broken "hello_attach_only link failed (see $OUT/helloattachonly-build.log)"
        fi
    else
        build_broken "hello_attach_only host build failed (see $OUT/helloattachonly-host-build.log)"
    fi
fi

# workspace_layout_projection — a hosted session is a workspace of one pane,
# described to a client that mirrors pane arrangements by the workspace's
# structure and answered on every layout verb: twenty verbs leave the host's
# structure what a model given the same verbs holds. The structure that first
# names a pane leaves before the pane's PaneBind and frames, and the one that
# retires it after its last, on a carrier whose socket was full as on one
# that read at once.
if wanted workspace_layout_projection && command -v cmake >/dev/null 2>&1; then
    echo "=== workspace_layout_projection ==="
    RAN=$((RAN + 1))
    HOST_BUILD="$UBQ_BUILD_ROOT/host-noquic"
    HOST_OBJDIR="$HOST_BUILD/CMakeFiles/ubiquitty-host.dir"
    if cmake -B "$HOST_BUILD" -S "$ROOT" -G "$UBQ_GENERATOR" -DUBIQUITTY_FRONTEND=Host -DCMAKE_BUILD_TYPE=Debug \
                \
                >"$OUT/workspacelayoutprojection-host-build.log" 2>&1 \
            && cmake --build "$HOST_BUILD" -j"$BUILD_JOBS" >>"$OUT/workspacelayoutprojection-host-build.log" 2>&1; then
        host_objects "$HOST_OBJDIR" "$OUT/workspacelayoutprojection-objs.txt" \
            || build_broken "host object list is empty (the harness, not the test under it)"
        if ubq_compile_parallel "$OUT/workspace_layout_projection" "$OUT/objects/workspace_layout_projection" "$OUT/workspacelayoutprojection-build.log" \
                -std=c++23 -DUBIQUITTY_QUIC=1 -I"$QUIC_PREFIX/include" -I"$ROOT/Tests" -I"$ROOT/Source" -- \
                "$ROOT/Tests/Host/workspace_layout_projection.cpp" \
                -- "@$OUT/workspacelayoutprojection-objs.txt" "${HOST_LINK_ARCHIVES[@]}"; then
            ubq_run_bounded 300 "$OUT/workspace_layout_projection" || exit 1
        else
            build_broken "workspace_layout_projection link failed (see $OUT/workspacelayoutprojection-build.log)"
        fi
    else
        build_broken "workspace_layout_projection host build failed (see $OUT/workspacelayoutprojection-host-build.log)"
    fi
fi

# hello_ssh_host — a FRESH HELLO may name the machine its session runs on, and
# the daemon turns that name into `ssh [user@]host` itself: the wire carries a
# host string, never an argv, so the grant is bounded by validateSSHHost and no
# shell stands between the field and execve. Four arms through the production
# handleFrame: an argv-injecting host is refused AUTH_DENIED(RemoteHost) with no
# session spawned; a valid host launches with the assembled argv (asserted from
# the session PTY, where a fake `ssh` first on $PATH echoes what it was given);
# and neither an unclaimed RemoteLaunch bit nor a HELLO naming a session id
# launches anything. Same QUIC-OFF host build as hello_reentry_guard.
if wanted hello_ssh_host && command -v cmake >/dev/null 2>&1; then
    echo "=== hello_ssh_host ==="
    RAN=$((RAN + 1))
    HOST_BUILD="$UBQ_BUILD_ROOT/host-noquic"
    HOST_OBJDIR="$HOST_BUILD/CMakeFiles/ubiquitty-host.dir"
    if cmake -B "$HOST_BUILD" -S "$ROOT" -G "$UBQ_GENERATOR" -DUBIQUITTY_FRONTEND=Host -DCMAKE_BUILD_TYPE=Debug \
                \
                >"$OUT/hellosshhost-host-build.log" 2>&1 \
            && cmake --build "$HOST_BUILD" -j"$BUILD_JOBS" >>"$OUT/hellosshhost-host-build.log" 2>&1; then
        host_objects "$HOST_OBJDIR" "$OUT/hellosshhost-objs.txt" \
            || build_broken "host object list is empty (the harness, not the test under it)"
        if ubq_compile_parallel "$OUT/hello_ssh_host" "$OUT/objects/hello_ssh_host" "$OUT/hellosshhost-build.log" \
                -std=c++23 -DUBIQUITTY_QUIC=1 -I"$QUIC_PREFIX/include" -I"$ROOT/Tests" -I"$ROOT/Source" -- \
                "$ROOT/Tests/Host/hello_ssh_host.cpp" \
                -- "@$OUT/hellosshhost-objs.txt" "${HOST_LINK_ARCHIVES[@]}"; then
            "$OUT/hello_ssh_host" || exit 1
        else
            build_broken "hello_ssh_host link failed (see $OUT/hellosshhost-build.log)"
        fi
    else
        build_broken "hello_ssh_host host build failed (see $OUT/hellosshhost-host-build.log)"
    fi
fi

# adopt_in_parts — a shared window's screen and whole history reach the daemon
# as one AdoptWire Prepare record, however many reads it arrives in, and adopt
# on Commit. A window under the ordinary frame bound and one past it are each
# adopted with every history line, past the daemon's own scrollback limit; a
# share past its total is refused whole; a record that stops short adopts
# nothing and is dropped at the partial-frame deadline with nothing held; the
# record counts in the aggregate inbound budget, which admits one whole adopt
# and drops the newest of three holders; and a Prepare whose body is no
# prepared record, a malformed or empty Commit, and a kind the lane does not
# define each drop the client with nothing adopted. The shell is a child
# leading a session on a PTY, as
# the ownership check asks. QUIC-off host build.
if wanted adopt_in_parts && command -v cmake >/dev/null 2>&1; then
    echo "=== adopt_in_parts ==="
    RAN=$((RAN + 1))
    HOST_BUILD="$UBQ_BUILD_ROOT/host-noquic"
    HOST_OBJDIR="$HOST_BUILD/CMakeFiles/ubiquitty-host.dir"
    if cmake -B "$HOST_BUILD" -S "$ROOT" -G "$UBQ_GENERATOR" -DUBIQUITTY_FRONTEND=Host -DCMAKE_BUILD_TYPE=Debug \
                \
                >"$OUT/adoptinparts-host-build.log" 2>&1 \
            && cmake --build "$HOST_BUILD" -j"$BUILD_JOBS" >>"$OUT/adoptinparts-host-build.log" 2>&1; then
        host_objects "$HOST_OBJDIR" "$OUT/adoptinparts-objs.txt" \
            || build_broken "host object list is empty (the harness, not the test under it)"
        if ubq_compile_parallel "$OUT/adopt_in_parts" "$OUT/objects/adopt_in_parts" "$OUT/adoptinparts-build.log" \
                -std=c++23 -DUBIQUITTY_QUIC=1 -I"$QUIC_PREFIX/include" -I"$ROOT/Tests" -I"$ROOT/Source" -- \
                "$ROOT/Tests/Host/adopt_in_parts.cpp" \
                -- "@$OUT/adoptinparts-objs.txt" "${HOST_LINK_ARCHIVES[@]}"; then
            ubq_run_bounded 300 "$OUT/adopt_in_parts" || exit 1
        else
            build_broken "adopt_in_parts link failed (see $OUT/adoptinparts-build.log)"
        fi
    else
        build_broken "adopt_in_parts host build failed (see $OUT/adoptinparts-host-build.log)"
    fi
fi

# inbound_holds — what a client's Sync stream can leave the daemon holding
# is released or counted: a Notify a client sends is held no longer than the
# pass that read it; a pane frame part-way through its Catalog run is under
# the partial-frame deadline, so a client that stops mid-run is dropped at it;
# and those bytes count in the aggregate inbound budget, which drops the
# newest of clients that together pass it. QUIC-off host build.
if wanted inbound_holds && command -v cmake >/dev/null 2>&1; then
    echo "=== inbound_holds ==="
    RAN=$((RAN + 1))
    HOST_BUILD="$UBQ_BUILD_ROOT/host-noquic"
    HOST_OBJDIR="$HOST_BUILD/CMakeFiles/ubiquitty-host.dir"
    if cmake -B "$HOST_BUILD" -S "$ROOT" -G "$UBQ_GENERATOR" -DUBIQUITTY_FRONTEND=Host -DCMAKE_BUILD_TYPE=Debug \
                \
                >"$OUT/inboundholds-host-build.log" 2>&1 \
            && cmake --build "$HOST_BUILD" -j"$BUILD_JOBS" >>"$OUT/inboundholds-host-build.log" 2>&1; then
        host_objects "$HOST_OBJDIR" "$OUT/inboundholds-objs.txt" \
            || build_broken "host object list is empty (the harness, not the test under it)"
        if ubq_compile_parallel "$OUT/inbound_holds" "$OUT/objects/inbound_holds" "$OUT/inboundholds-build.log" \
                -std=c++23 -DUBIQUITTY_QUIC=1 -I"$QUIC_PREFIX/include" -I"$ROOT/Tests" -I"$ROOT/Source" -- \
                "$ROOT/Tests/Host/inbound_holds.cpp" \
                -- "@$OUT/inboundholds-objs.txt" "${HOST_LINK_ARCHIVES[@]}"; then
            ubq_run_bounded 300 "$OUT/inbound_holds" || exit 1
        else
            build_broken "inbound_holds link failed (see $OUT/inboundholds-build.log)"
        fi
    else
        build_broken "inbound_holds host build failed (see $OUT/inboundholds-host-build.log)"
    fi
fi

# hello_field_bounds — a HELLO's strings sit behind 16-bit lengths, and the
# daemon holds each to less: an agent label past maxAgentLabelLength is kept
# to that many bytes at a UTF-8 boundary, so the list of who is attached
# still encodes; a token past maxAuthTokenLength is refused as unrecognised
# though the store holds its digest, and one at the length is taken; an ssh
# host past maxSSHHostLength is refused AUTH_DENIED(RemoteHost) and starts no
# session. Real HELLO frames through handleFrame, on the QUIC-off host build.
if wanted hello_field_bounds && command -v cmake >/dev/null 2>&1; then
    echo "=== hello_field_bounds ==="
    RAN=$((RAN + 1))
    HOST_BUILD="$UBQ_BUILD_ROOT/host-noquic"
    HOST_OBJDIR="$HOST_BUILD/CMakeFiles/ubiquitty-host.dir"
    if cmake -B "$HOST_BUILD" -S "$ROOT" -G "$UBQ_GENERATOR" -DUBIQUITTY_FRONTEND=Host -DCMAKE_BUILD_TYPE=Debug \
                \
                >"$OUT/hellofieldbounds-host-build.log" 2>&1 \
            && cmake --build "$HOST_BUILD" -j"$BUILD_JOBS" >>"$OUT/hellofieldbounds-host-build.log" 2>&1; then
        host_objects "$HOST_OBJDIR" "$OUT/hellofieldbounds-objs.txt" \
            || build_broken "host object list is empty (the harness, not the test under it)"
        if ubq_compile_parallel "$OUT/hello_field_bounds" "$OUT/objects/hello_field_bounds" "$OUT/hellofieldbounds-build.log" \
                -std=c++23 -DUBIQUITTY_QUIC=1 -I"$QUIC_PREFIX/include" -I"$ROOT/Tests" -I"$ROOT/Source" -- \
                "$ROOT/Tests/Host/hello_field_bounds.cpp" \
                -- "@$OUT/hellofieldbounds-objs.txt" "${HOST_LINK_ARCHIVES[@]}"; then
            ubq_run_bounded 300 "$OUT/hello_field_bounds" || exit 1
        else
            build_broken "hello_field_bounds link failed (see $OUT/hellofieldbounds-build.log)"
        fi
    else
        build_broken "hello_field_bounds host build failed (see $OUT/hellofieldbounds-host-build.log)"
    fi
fi

# inbound_kick_gate — serviceClient's dispatch stops the instant overflowed
# flips. overflowed is a deferred reap-mark the run loop's top-of-pass reap
# sweep consults; consulted there alone, a client kicked MID-PASS (its own
# frame handling set overflowed) would keep driving through every frame still
# sitting in that same already-decoded batch. This drives a real
# [duplicate HELLO][palette Facts] batch (one write, one recvmsg) through the
# actual private serviceClient over a socketpair fd, and asserts the second
# frame is never dispatched once the duplicate HELLO has kicked the client;
# the same frame alone from a client in good standing is. Same QUIC-OFF host build as hello_reentry_guard.
if wanted inbound_kick_gate && command -v cmake >/dev/null 2>&1; then
    echo "=== inbound_kick_gate ==="
    RAN=$((RAN + 1))
    HOST_BUILD="$UBQ_BUILD_ROOT/host-noquic"
    HOST_OBJDIR="$HOST_BUILD/CMakeFiles/ubiquitty-host.dir"
    if cmake -B "$HOST_BUILD" -S "$ROOT" -G "$UBQ_GENERATOR" -DUBIQUITTY_FRONTEND=Host -DCMAKE_BUILD_TYPE=Debug \
                \
                >"$OUT/inboundkick-host-build.log" 2>&1 \
            && cmake --build "$HOST_BUILD" -j"$BUILD_JOBS" >>"$OUT/inboundkick-host-build.log" 2>&1; then
        host_objects "$HOST_OBJDIR" "$OUT/inboundkick-objs.txt" \
            || build_broken "host object list is empty (the harness, not the test under it)"
        if ubq_compile_parallel "$OUT/inbound_kick_gate" "$OUT/objects/inbound_kick_gate" "$OUT/inboundkick-build.log" \
                -std=c++23 -DUBIQUITTY_QUIC=1 -I"$QUIC_PREFIX/include" -I"$ROOT/Tests" -I"$ROOT/Source" -- \
                "$ROOT/Tests/Host/inbound_kick_gate.cpp" \
                -- "@$OUT/inboundkick-objs.txt" "${HOST_LINK_ARCHIVES[@]}"; then
            "$OUT/inbound_kick_gate" || exit 1
        else
            build_broken "inbound_kick_gate link failed (see $OUT/inboundkick-build.log)"
        fi
    else
        build_broken "inbound_kick_gate host build failed (see $OUT/inboundkick-host-build.log)"
    fi
fi

# rowdelta_baseline_corruption — the baseline the host records for a client is
# what the client holds: across plain rows, a row under an OSC 8 link and rows
# left unchanged, every frame is an update onto that baseline and the client
# holds the host's screen after each, with no resync. Same QUIC-OFF host build
# as origin_staleness/hello_reentry_guard.
if wanted rowdelta_baseline_corruption && command -v cmake >/dev/null 2>&1; then
    echo "=== rowdelta_baseline_corruption ==="
    RAN=$((RAN + 1))
    HOST_BUILD="$UBQ_BUILD_ROOT/host-noquic"
    HOST_OBJDIR="$HOST_BUILD/CMakeFiles/ubiquitty-host.dir"
    if cmake -B "$HOST_BUILD" -S "$ROOT" -G "$UBQ_GENERATOR" -DUBIQUITTY_FRONTEND=Host -DCMAKE_BUILD_TYPE=Debug \
                \
                >"$OUT/rowdeltabaseline-host-build.log" 2>&1 \
            && cmake --build "$HOST_BUILD" -j"$BUILD_JOBS" >>"$OUT/rowdeltabaseline-host-build.log" 2>&1; then
        host_objects "$HOST_OBJDIR" "$OUT/rowdeltabaseline-objs.txt" \
            || build_broken "host object list is empty (the harness, not the test under it)"
        if ubq_compile_parallel "$OUT/rowdelta_baseline_corruption" "$OUT/objects/rowdelta_baseline_corruption" "$OUT/rowdeltabaseline-build.log" \
                -std=c++23 -DUBIQUITTY_QUIC=1 -I"$QUIC_PREFIX/include" -I"$ROOT/Tests" -I"$ROOT/Source" -- \
                "$ROOT/Tests/Host/rowdelta_baseline_corruption.cpp" \
                -- "@$OUT/rowdeltabaseline-objs.txt" "${HOST_LINK_ARCHIVES[@]}"; then
            "$OUT/rowdelta_baseline_corruption" || exit 1
        else
            build_broken "rowdelta_baseline_corruption link failed (see $OUT/rowdeltabaseline-build.log)"
        fi
    else
        build_broken "rowdelta_baseline_corruption host build failed (see $OUT/rowdeltabaseline-host-build.log)"
    fi
fi

# overflowed_client_stays_unsynced — a client that never got a frame is never
# treated as holding it: one flagged on the high-water mark is queued nothing
# more, and one whose frame was lost refuses the next update, asks for a
# checkpoint and holds the host's screen again, while the client beside them
# never falls out of step. Friend seam, same QUIC-OFF host build as
# rowdelta_baseline_corruption.
if wanted overflowed_client_stays_unsynced && command -v cmake >/dev/null 2>&1; then
    echo "=== overflowed_client_stays_unsynced ==="
    RAN=$((RAN + 1))
    HOST_BUILD="$UBQ_BUILD_ROOT/host-noquic"
    HOST_OBJDIR="$HOST_BUILD/CMakeFiles/ubiquitty-host.dir"
    if cmake -B "$HOST_BUILD" -S "$ROOT" -G "$UBQ_GENERATOR" -DUBIQUITTY_FRONTEND=Host -DCMAKE_BUILD_TYPE=Debug \
                \
                >"$OUT/overflowedclient-host-build.log" 2>&1 \
            && cmake --build "$HOST_BUILD" -j"$BUILD_JOBS" >>"$OUT/overflowedclient-host-build.log" 2>&1; then
        host_objects "$HOST_OBJDIR" "$OUT/overflowedclient-objs.txt" \
            || build_broken "host object list is empty (the harness, not the test under it)"
        if ubq_compile_parallel "$OUT/overflowed_client_stays_unsynced" "$OUT/objects/overflowed_client_stays_unsynced" "$OUT/overflowedclient-build.log" \
                -std=c++23 -DUBIQUITTY_QUIC=1 -I"$QUIC_PREFIX/include" -I"$ROOT/Tests" -I"$ROOT/Source" -- \
                "$ROOT/Tests/Host/overflowed_client_stays_unsynced.cpp" \
                -- "@$OUT/overflowedclient-objs.txt" "${HOST_LINK_ARCHIVES[@]}"; then
            ubq_run_bounded 300 "$OUT/overflowed_client_stays_unsynced" || exit 1
        else
            build_broken "overflowed_client_stays_unsynced link failed (see $OUT/overflowedclient-build.log)"
        fi
    else
        build_broken "overflowed_client_stays_unsynced host build failed (see $OUT/overflowedclient-host-build.log)"
    fi
fi

# backlog_drop_says_why — a client pushed past its outbound high-water mark is
# sent Bye{OutputBacklog} after the rest of the frame in flight, written past
# the mark, and the reap waits a bounded time for it to leave. Friend seam,
# same QUIC-OFF host build as overflowed_client_stays_unsynced.
if wanted backlog_drop_says_why && command -v cmake >/dev/null 2>&1; then
    echo "=== backlog_drop_says_why ==="
    RAN=$((RAN + 1))
    HOST_BUILD="$UBQ_BUILD_ROOT/host-noquic"
    HOST_OBJDIR="$HOST_BUILD/CMakeFiles/ubiquitty-host.dir"
    if cmake -B "$HOST_BUILD" -S "$ROOT" -G "$UBQ_GENERATOR" -DUBIQUITTY_FRONTEND=Host -DCMAKE_BUILD_TYPE=Debug \
                \
                >"$OUT/backlogdrop-host-build.log" 2>&1 \
            && cmake --build "$HOST_BUILD" -j"$BUILD_JOBS" >>"$OUT/backlogdrop-host-build.log" 2>&1; then
        host_objects "$HOST_OBJDIR" "$OUT/backlogdrop-objs.txt" \
            || build_broken "host object list is empty (the harness, not the test under it)"
        if ubq_compile_parallel "$OUT/backlog_drop_says_why" "$OUT/objects/backlog_drop_says_why" "$OUT/backlogdrop-build.log" \
                -std=c++23 -DUBIQUITTY_QUIC=1 -I"$QUIC_PREFIX/include" -I"$ROOT/Tests" -I"$ROOT/Source" -- \
                "$ROOT/Tests/Host/backlog_drop_says_why.cpp" \
                -- "@$OUT/backlogdrop-objs.txt" "${HOST_LINK_ARCHIVES[@]}"; then
            ubq_run_bounded 300 "$OUT/backlog_drop_says_why" || exit 1
        else
            build_broken "backlog_drop_says_why link failed (see $OUT/backlogdrop-build.log)"
        fi
    else
        build_broken "backlog_drop_says_why host build failed (see $OUT/backlogdrop-host-build.log)"
    fi
fi

# lost_drop_says_why — a connection whose bytes stay unacknowledged past the
# silence bound is Lost, and the host drops it with a Bye{Silent} that a
# client still reading receives. Friend seam, same QUIC-OFF host build as
# backlog_drop_says_why.
if wanted lost_drop_says_why && command -v cmake >/dev/null 2>&1; then
    echo "=== lost_drop_says_why ==="
    RAN=$((RAN + 1))
    HOST_BUILD="$UBQ_BUILD_ROOT/host-noquic"
    HOST_OBJDIR="$HOST_BUILD/CMakeFiles/ubiquitty-host.dir"
    if cmake -B "$HOST_BUILD" -S "$ROOT" -G "$UBQ_GENERATOR" -DUBIQUITTY_FRONTEND=Host -DCMAKE_BUILD_TYPE=Debug \
                \
                >"$OUT/lostdrop-host-build.log" 2>&1 \
            && cmake --build "$HOST_BUILD" -j"$BUILD_JOBS" >>"$OUT/lostdrop-host-build.log" 2>&1; then
        host_objects "$HOST_OBJDIR" "$OUT/lostdrop-objs.txt" \
            || build_broken "host object list is empty (the harness, not the test under it)"
        if ubq_compile_parallel "$OUT/lost_drop_says_why" "$OUT/objects/lost_drop_says_why" "$OUT/lostdrop-build.log" \
                -std=c++23 -DUBIQUITTY_QUIC=1 -I"$QUIC_PREFIX/include" -I"$ROOT/Tests" -I"$ROOT/Source" -- \
                "$ROOT/Tests/Host/lost_drop_says_why.cpp" \
                -- "@$OUT/lostdrop-objs.txt" "${HOST_LINK_ARCHIVES[@]}"; then
            ubq_run_bounded 300 "$OUT/lost_drop_says_why" || exit 1
        else
            build_broken "lost_drop_says_why link failed (see $OUT/lostdrop-build.log)"
        fi
    else
        build_broken "lost_drop_says_why host build failed (see $OUT/lostdrop-host-build.log)"
    fi
fi

# foreground_probe_budget — probeForegroundIfDue's foregroundHolder probes
# (tty ioctls + process sysctls + string allocations) run
# on a ~50 ms tick budget per session, never per broadcast frame. A
# foreground-group change (one cheap ioctl, the change-detector) forces an
# immediate re-probe whose Status frame reaches CommandEvents clients; an
# unsettled pre-exec probe (group 0 / empty name) never latches the budget.
# Drives the real publishFrame with a counting ProcessFacts stub through
# a friend seam. Same QUIC-OFF host build as rowdelta_baseline_corruption.
if wanted foreground_probe_budget && command -v cmake >/dev/null 2>&1; then
    echo "=== foreground_probe_budget ==="
    RAN=$((RAN + 1))
    HOST_BUILD="$UBQ_BUILD_ROOT/host-noquic"
    HOST_OBJDIR="$HOST_BUILD/CMakeFiles/ubiquitty-host.dir"
    if cmake -B "$HOST_BUILD" -S "$ROOT" -G "$UBQ_GENERATOR" -DUBIQUITTY_FRONTEND=Host -DCMAKE_BUILD_TYPE=Debug \
                \
                >"$OUT/foregroundprobe-host-build.log" 2>&1 \
            && cmake --build "$HOST_BUILD" -j"$BUILD_JOBS" >>"$OUT/foregroundprobe-host-build.log" 2>&1; then
        host_objects "$HOST_OBJDIR" "$OUT/foregroundprobe-objs.txt" \
            || build_broken "host object list is empty (the harness, not the test under it)"
        if $CXX_TEST -std=c++23 -DUBIQUITTY_QUIC=1 -I"$QUIC_PREFIX/include" -I"$ROOT/Tests" -I"$ROOT/Source" \
                "$ROOT/Tests/Host/foreground_probe_budget.cpp" "@$OUT/foregroundprobe-objs.txt" \
                "${HOST_LINK_ARCHIVES[@]}" \
                -o "$OUT/foreground_probe_budget" 2>"$OUT/foregroundprobe-build.log"; then
            "$OUT/foreground_probe_budget" || exit 1
        else
            build_broken "foreground_probe_budget link failed (see $OUT/foregroundprobe-build.log)"
        fi
    else
        build_broken "foreground_probe_budget host build failed (see $OUT/foregroundprobe-host-build.log)"
    fi
fi

# sync_output_e2e — the same property against a REAL daemon driving a REAL
# shell. sync_output_withhold pins mayPublishFrame's decision and can see
# nothing between the PTY and it: whether the escape reaches the session's
# Terminal, whether the mode survives the parse, whether the poll loop reaches
# emission while the block is open. Two arms — the same sliced workload with and
# without the block — because a gate that does nothing gives two equal counts.
if wanted sync_output_e2e && command -v cmake >/dev/null 2>&1; then
    echo "=== sync_output_e2e ==="
    RAN=$((RAN + 1))
    HOST_BUILD="$UBQ_BUILD_ROOT/host-noquic"
    HOST_OBJDIR="$HOST_BUILD/CMakeFiles/ubiquitty-host.dir"
    if cmake -B "$HOST_BUILD" -S "$ROOT" -G "$UBQ_GENERATOR" -DUBIQUITTY_FRONTEND=Host -DCMAKE_BUILD_TYPE=Debug \
                \
                >"$OUT/sync-output-host-build.log" 2>&1 \
            && cmake --build "$HOST_BUILD" -j"$BUILD_JOBS" >>"$OUT/sync-output-host-build.log" 2>&1; then
        host_objects "$HOST_OBJDIR" "$OUT/sync-output-objs.txt" \
            || build_broken "host object list is empty (the harness, not the test under it)"
        if ubq_compile_parallel "$OUT/sync_output_e2e" "$OUT/objects/sync_output_e2e" "$OUT/sync-output-build.log" \
                -std=c++23 -DUBIQUITTY_QUIC=1 -I"$QUIC_PREFIX/include" -I"$ROOT/Tests" -I"$ROOT/Tests/Host" -I"$ROOT/Source" -- \
                "$ROOT/Tests/Host/sync_output_e2e.cpp" \
                -- "@$OUT/sync-output-objs.txt" "${HOST_LINK_ARCHIVES[@]}"; then
            ubq_run_private "$OUT/sync_output_e2e" "$HOST_BUILD/ubiquitty-host" || exit 1
        else
            build_broken "sync_output_e2e link failed (see $OUT/sync-output-build.log)"
        fi
    else
        build_broken "sync_output_e2e host build failed (see $OUT/sync-output-host-build.log)"
    fi
fi

# sync_prompt_mark_e2e — the OSC 133 bracket reaches a client on a Sync update
# rather than on the next checkpoint, in a REAL daemon driving a REAL shell. No
# unit test sees what lies between the shell and the row record: whether the
# escape survives the PTY and the parse, whether the mark reaches the session's
# Terminal before the publish, whether the client is sent an update at all. The
# client is the client end of a Sync connection fed the frames in arrival
# order; the assertion is the transition an update makes, with no checkpoint in
# between.
if wanted sync_prompt_mark_e2e && command -v cmake >/dev/null 2>&1; then
    echo "=== sync_prompt_mark_e2e ==="
    RAN=$((RAN + 1))
    HOST_BUILD="$UBQ_BUILD_ROOT/host-noquic"
    HOST_OBJDIR="$HOST_BUILD/CMakeFiles/ubiquitty-host.dir"
    if cmake -B "$HOST_BUILD" -S "$ROOT" -G "$UBQ_GENERATOR" -DUBIQUITTY_FRONTEND=Host -DCMAKE_BUILD_TYPE=Debug \
                \
                >"$OUT/promptmark-host-build.log" 2>&1 \
            && cmake --build "$HOST_BUILD" -j"$BUILD_JOBS" >>"$OUT/promptmark-host-build.log" 2>&1; then
        host_objects "$HOST_OBJDIR" "$OUT/promptmark-objs.txt" \
            || build_broken "host object list is empty (the harness, not the test under it)"
        if ubq_compile_parallel "$OUT/sync_prompt_mark_e2e" "$OUT/objects/sync_prompt_mark_e2e" "$OUT/promptmark-build.log" \
                -std=c++23 -DUBIQUITTY_QUIC=1 -I"$QUIC_PREFIX/include" -I"$ROOT/Tests" -I"$ROOT/Tests/Host" -I"$ROOT/Source" -- \
                "$ROOT/Tests/Host/sync_prompt_mark_e2e.cpp" \
                -- "@$OUT/promptmark-objs.txt" "${HOST_LINK_ARCHIVES[@]}"; then
            ubq_run_private "$OUT/sync_prompt_mark_e2e" "$HOST_BUILD/ubiquitty-host" || exit 1
        else
            build_broken "sync_prompt_mark_e2e link failed (see $OUT/promptmark-build.log)"
        fi
    else
        build_broken "sync_prompt_mark_e2e host build failed (see $OUT/promptmark-host-build.log)"
    fi
fi

# sync_image_e2e — an inline image a program prints on the host reaches a thin
# client decoded and placed, in a REAL daemon driving a REAL shell, for both the
# OSC 1337 and the Kitty graphics forms. The Sync tests pin the IMAGE codec in
# isolation; this pins the PTY, the host's decode into the session's store, and
# the daemon's delivery to a client that claimed Images in its HELLO. The shell
# is /bin/sh under a scratch HOME (ubq_with_hermetic_shell), so no hook of the
# person running the suite starts in it.
if wanted sync_image_e2e && command -v cmake >/dev/null 2>&1; then
    echo "=== sync_image_e2e ==="
    RAN=$((RAN + 1))
    HOST_BUILD="$UBQ_BUILD_ROOT/host-noquic"
    HOST_OBJDIR="$HOST_BUILD/CMakeFiles/ubiquitty-host.dir"
    if cmake -B "$HOST_BUILD" -S "$ROOT" -G "$UBQ_GENERATOR" -DUBIQUITTY_FRONTEND=Host -DCMAKE_BUILD_TYPE=Debug \
                >"$OUT/syncimage-host-build.log" 2>&1 \
            && cmake --build "$HOST_BUILD" -j"$BUILD_JOBS" >>"$OUT/syncimage-host-build.log" 2>&1; then
        host_objects "$HOST_OBJDIR" "$OUT/syncimage-objs.txt" \
            || build_broken "host object list is empty (the harness, not the test under it)"
        if ubq_compile_parallel "$OUT/sync_image_e2e" "$OUT/objects/sync_image_e2e" "$OUT/syncimage-build.log" \
                -std=c++23 -DUBIQUITTY_QUIC=1 -I"$QUIC_PREFIX/include" -I"$ROOT/Tests" -I"$ROOT/Tests/Host" -I"$ROOT/Source" -- \
                "$ROOT/Tests/Host/sync_image_e2e.cpp" \
                -- "@$OUT/syncimage-objs.txt" "${HOST_LINK_ARCHIVES[@]}"; then
            ubq_with_hermetic_shell "$OUT/sync_image_e2e" "$HOST_BUILD/ubiquitty-host" || exit 1
        else
            build_broken "sync_image_e2e link failed (see $OUT/syncimage-build.log)"
        fi
    else
        build_broken "sync_image_e2e host build failed (see $OUT/syncimage-host-build.log)"
    fi
fi

# sync_output_withhold — the daemon publishes by the coherence rule
# (Runtime::FrameCoherence): DEC private mode 2026, a cursor-hidden redraw and
# its deadline. The escapes reach the daemon's terminal and are never forwarded,
# and a frame carries no flag for an open block or bracket, so a client cannot
# make this call for itself: publishing mid-composition tears on every attached
# client at once. Pins that a block or a bracket withholds, that the withheld
# frame is deferred rather than dropped, that a block outliving maxHoldTicks
# expires on its own, and that a bracket outliving the deadline is forced. Same
# QUIC-OFF host build as foreground_probe_budget.
if wanted sync_output_withhold && command -v cmake >/dev/null 2>&1; then
    echo "=== sync_output_withhold ==="
    RAN=$((RAN + 1))
    HOST_BUILD="$UBQ_BUILD_ROOT/host-noquic"
    HOST_OBJDIR="$HOST_BUILD/CMakeFiles/ubiquitty-host.dir"
    if cmake -B "$HOST_BUILD" -S "$ROOT" -G "$UBQ_GENERATOR" -DUBIQUITTY_FRONTEND=Host -DCMAKE_BUILD_TYPE=Debug \
                \
                >"$OUT/syncwithhold-host-build.log" 2>&1 \
            && cmake --build "$HOST_BUILD" -j"$BUILD_JOBS" >>"$OUT/syncwithhold-host-build.log" 2>&1; then
        host_objects "$HOST_OBJDIR" "$OUT/syncwithhold-objs.txt" \
            || build_broken "host object list is empty (the harness, not the test under it)"
        if ubq_compile_parallel "$OUT/sync_output_withhold" "$OUT/objects/sync_output_withhold" "$OUT/syncwithhold-build.log" \
                -std=c++23 -DUBIQUITTY_QUIC=1 -I"$QUIC_PREFIX/include" -I"$ROOT/Tests" -I"$ROOT/Source" -- \
                "$ROOT/Tests/Host/sync_output_withhold.cpp" \
                -- "@$OUT/syncwithhold-objs.txt" "${HOST_LINK_ARCHIVES[@]}"; then
            "$OUT/sync_output_withhold" || exit 1
        else
            build_broken "sync_output_withhold link failed (see $OUT/syncwithhold-build.log)"
        fi
    else
        build_broken "sync_output_withhold host build failed (see $OUT/syncwithhold-host-build.log)"
    fi
fi

# consent_owner_gate — the attach + terminate consent gates hold in every
# no-live-human window (the daemon keeps a session alive across client detach
# so it can be reattached). A gate predicated on `isAgent &&
# sessionHasHumanClient` would skip there and let an ungranted agent attach to
# or kill a detached-but-alive session with ZERO consent, so the no-human
# branch consults the standing-grant machinery (session ownership is
# implicitly the Local human who granted it), with a just-created exemption
# (attach) and an already-attached exemption (terminate) so terminal_new /
# self-cleanup keep working. Drives the REAL
# handleFrame: a never-attached ungranted third party is denied both ATTACH
# and TERMINATE of a real detached session; a standing-grant agent, an
# already-attached agent, and a session's own creator are all unaffected.
# Same QUIC-OFF host build as hello_reentry_guard/origin_staleness.
if wanted consent_owner_gate && command -v cmake >/dev/null 2>&1; then
    echo "=== consent_owner_gate ==="
    RAN=$((RAN + 1))
    HOST_BUILD="$UBQ_BUILD_ROOT/host-noquic"
    HOST_OBJDIR="$HOST_BUILD/CMakeFiles/ubiquitty-host.dir"
    if cmake -B "$HOST_BUILD" -S "$ROOT" -G "$UBQ_GENERATOR" -DUBIQUITTY_FRONTEND=Host -DCMAKE_BUILD_TYPE=Debug \
                \
                >"$OUT/consentowner-host-build.log" 2>&1 \
            && cmake --build "$HOST_BUILD" -j"$BUILD_JOBS" >>"$OUT/consentowner-host-build.log" 2>&1; then
        host_objects "$HOST_OBJDIR" "$OUT/consentowner-objs.txt" \
            || build_broken "host object list is empty (the harness, not the test under it)"
        if ubq_compile_parallel "$OUT/consent_owner_gate" "$OUT/objects/consent_owner_gate" "$OUT/consentowner-build.log" \
                -std=c++23 -DUBIQUITTY_QUIC=1 -I"$QUIC_PREFIX/include" -I"$ROOT/Tests" -I"$ROOT/Source" -- \
                "$ROOT/Tests/Host/consent_owner_gate.cpp" \
                -- "@$OUT/consentowner-objs.txt" "${HOST_LINK_ARCHIVES[@]}"; then
            "$OUT/consent_owner_gate" || exit 1
        else
            build_broken "consent_owner_gate link failed (see $OUT/consentowner-build.log)"
        fi
    else
        build_broken "consent_owner_gate host build failed (see $OUT/consentowner-host-build.log)"
    fi
fi

# capability_prompt_cap — the capability-grant consent path (not the attach
# gate). Its outstanding-prompt table is removed from only by a human ruling, a
# session ending, or a revoke, so an agent reconnecting under a fresh identity
# each time leaves records nothing prunes. The cap drops the oldest and resolves
# its triple in the consent queue together; dropping without resolving would leave the
# capability suppressed as a repeat with no prompt behind it, which is the
# permanent-Prompt shape the queue's dedupe is otherwise there to avoid.
if wanted capability_prompt_cap && command -v cmake >/dev/null 2>&1; then
    echo "=== capability_prompt_cap ==="
    RAN=$((RAN + 1))
    HOST_BUILD="$UBQ_BUILD_ROOT/host-noquic"
    HOST_OBJDIR="$HOST_BUILD/CMakeFiles/ubiquitty-host.dir"
    if cmake -B "$HOST_BUILD" -S "$ROOT" -G "$UBQ_GENERATOR" -DUBIQUITTY_FRONTEND=Host -DCMAKE_BUILD_TYPE=Debug \
                >"$OUT/promptcap-host-build.log" 2>&1 \
            && cmake --build "$HOST_BUILD" -j"$BUILD_JOBS" >>"$OUT/promptcap-host-build.log" 2>&1; then
        host_objects "$HOST_OBJDIR" "$OUT/promptcap-objs.txt" \
            || build_broken "host object list is empty (the harness, not the test under it)"
        if ubq_compile_parallel "$OUT/capability_prompt_cap" "$OUT/objects/capability_prompt_cap" "$OUT/promptcap-build.log" \
                -std=c++23 -DUBIQUITTY_QUIC=1 -I"$QUIC_PREFIX/include" -I"$ROOT/Tests" -I"$ROOT/Source" -- \
                "$ROOT/Tests/Host/capability_prompt_cap.cpp" \
                -- "@$OUT/promptcap-objs.txt" "${HOST_LINK_ARCHIVES[@]}"; then
            "$OUT/capability_prompt_cap" || exit 1
        else
            build_broken "capability_prompt_cap link failed (see $OUT/promptcap-build.log)"
        fi
    else
        build_broken "capability_prompt_cap host build failed (see $OUT/promptcap-host-build.log)"
    fi
fi

# ubq_sync_stream_fixture LOG — prints the path of the stream fixture the
# editor clients' decoder stanzas read, built in the Core test tree: the one
# ctest's build fixture already compiled, with the Core it links, or this run's
# own, rather than a Core of each stanza's own, which on a cold cache is the
# longest wait in the lane. A tree with no cache yet is configured as
# the ctest handoff configures it; one that has one keeps its settings.
ubq_sync_stream_fixture() {
    local log="$1"
    local tree="${UBQ_HOST_TEST_TREE:-$CTEST_BUILD_DIR}"
    if [ "${UBQ_TEST_TARGETS_PREBUILT:-0}" != 1 ]; then
        ubq_build_test_targets "$log" ubq-sync-stream-fixture || return 1
    fi
    if [ ! -x "$tree/Tests/ubq-sync-stream-fixture" ]; then
        printf 'Sync stream fixture is missing: %s\n' "$tree/Tests/ubq-sync-stream-fixture" >>"$log"
        return 1
    fi
    printf '%s\n' "$tree/Tests/ubq-sync-stream-fixture"
}

# daemon_start_arbitration — two starters cannot both decide they own the
# socket. bindSocket's stale-socket probe cannot tell a dead daemon's leftover
# socket from a live one between bind and listen, so the arbitration is what
# stops the loser unlinking the winner's socket. Drives the real Daemon with
# the lock held by the test, which makes the race deterministic.
if wanted daemon_start_arbitration && command -v cmake >/dev/null 2>&1; then
    echo "=== daemon_start_arbitration ==="
    RAN=$((RAN + 1))
    HOST_BUILD="$UBQ_BUILD_ROOT/host-noquic"
    HOST_OBJDIR="$HOST_BUILD/CMakeFiles/ubiquitty-host.dir"
    if cmake -B "$HOST_BUILD" -S "$ROOT" -G "$UBQ_GENERATOR" -DUBIQUITTY_FRONTEND=Host -DCMAKE_BUILD_TYPE=Debug \
                >"$OUT/daemonarb-host-build.log" 2>&1 \
            && cmake --build "$HOST_BUILD" -j"$BUILD_JOBS" >>"$OUT/daemonarb-host-build.log" 2>&1; then
        host_objects "$HOST_OBJDIR" "$OUT/daemonarb-objs.txt" \
            || build_broken "host object list is empty (the harness, not the test under it)"
        if ubq_compile_parallel "$OUT/daemon_start_arbitration" "$OUT/objects/daemon_start_arbitration" "$OUT/daemonarb-build.log" \
                -std=c++23 -DUBIQUITTY_QUIC=1 -I"$QUIC_PREFIX/include" -I"$ROOT/Tests" -I"$ROOT/Source" -- \
                "$ROOT/Tests/Host/daemon_start_arbitration.cpp" \
                -- "@$OUT/daemonarb-objs.txt" "${HOST_LINK_ARCHIVES[@]}"; then
            "$OUT/daemon_start_arbitration" || exit 1
        else
            build_broken "daemon_start_arbitration link failed (see $OUT/daemonarb-build.log)"
        fi
    else
        build_broken "daemon_start_arbitration host build failed (see $OUT/daemonarb-host-build.log)"
    fi
fi

# daemon_socket_path_limit — a socket path past sockaddr_un::sun_path is refused
# by name and leaves no lock file behind. The limit differs per target (Darwin
# 104, Linux 108), so the boundary is read from sizeof(sun_path): a literal
# would assert nothing on the platform it was not written on. Reuses the same
# host-noquic build the arbitration test above configures.
if wanted daemon_socket_path_limit && command -v cmake >/dev/null 2>&1; then
    echo "=== daemon_socket_path_limit ==="
    RAN=$((RAN + 1))
    HOST_BUILD="$UBQ_BUILD_ROOT/host-noquic"
    HOST_OBJDIR="$HOST_BUILD/CMakeFiles/ubiquitty-host.dir"
    if cmake -B "$HOST_BUILD" -S "$ROOT" -G "$UBQ_GENERATOR" -DUBIQUITTY_FRONTEND=Host -DCMAKE_BUILD_TYPE=Debug \
                >"$OUT/sockpathlimit-host-build.log" 2>&1 \
            && cmake --build "$HOST_BUILD" -j"$BUILD_JOBS" >>"$OUT/sockpathlimit-host-build.log" 2>&1; then
        host_objects "$HOST_OBJDIR" "$OUT/sockpathlimit-objs.txt" \
            || build_broken "host object list is empty (the harness, not the test under it)"
        if ubq_compile_parallel "$OUT/daemon_socket_path_limit" "$OUT/objects/daemon_socket_path_limit" "$OUT/sockpathlimit-build.log" \
                -std=c++23 -DUBIQUITTY_QUIC=1 -I"$QUIC_PREFIX/include" -I"$ROOT/Tests" -I"$ROOT/Source" -- \
                "$ROOT/Tests/Host/daemon_socket_path_limit.cpp" \
                -- "@$OUT/sockpathlimit-objs.txt" "${HOST_LINK_ARCHIVES[@]}"; then
            "$OUT/daemon_socket_path_limit" || exit 1
        else
            build_broken "daemon_socket_path_limit link failed (see $OUT/sockpathlimit-build.log)"
        fi
    else
        build_broken "daemon_socket_path_limit host build failed (see $OUT/sockpathlimit-host-build.log)"
    fi
fi

# capability_consent — the daemon side of the consent-PROMPT flow: a
# consent.respond records a capability grant only from the daemon's own pending
# record and only when the responder is a human on that session (agent /
# wrong-session / deny refused). Same QUIC-OFF host build as consent_owner_gate.
if wanted capability_consent && command -v cmake >/dev/null 2>&1; then
    echo "=== capability_consent ==="
    RAN=$((RAN + 1))
    HOST_BUILD="$UBQ_BUILD_ROOT/host-noquic"
    HOST_OBJDIR="$HOST_BUILD/CMakeFiles/ubiquitty-host.dir"
    if cmake -B "$HOST_BUILD" -S "$ROOT" -G "$UBQ_GENERATOR" -DUBIQUITTY_FRONTEND=Host -DCMAKE_BUILD_TYPE=Debug \
                \
                >"$OUT/capconsent-host-build.log" 2>&1 \
            && cmake --build "$HOST_BUILD" -j"$BUILD_JOBS" >>"$OUT/capconsent-host-build.log" 2>&1; then
        host_objects "$HOST_OBJDIR" "$OUT/capconsent-objs.txt" \
            || build_broken "host object list is empty (the harness, not the test under it)"
        if ubq_compile_parallel "$OUT/capability_consent" "$OUT/objects/capability_consent" "$OUT/capconsent-build.log" \
                -std=c++23 -DUBIQUITTY_QUIC=1 -I"$QUIC_PREFIX/include" -I"$ROOT/Tests" -I"$ROOT/Source" -- \
                "$ROOT/Tests/Host/capability_consent.cpp" \
                -- "@$OUT/capconsent-objs.txt" "${HOST_LINK_ARCHIVES[@]}"; then
            "$OUT/capability_consent" || exit 1
        else
            build_broken "capability_consent link failed (see $OUT/capconsent-build.log)"
        fi
    else
        build_broken "capability_consent host build failed (see $OUT/capconsent-host-build.log)"
    fi
fi

# capability_grant_scope — capability-grant lifetime model: OneShot is
# consumed on first Allow-returning check; HostEscaping/ReadExfil are forced to
# OneShot regardless of the human's AllowAlways; LowRiskInfo + AllowAlways with
# a token identity persists to disk and survives reload; a non-token identity
# degrades to Session without persistence; dropSession clears Session grants.
# Same QUIC-OFF host build as consent_owner_gate.
if wanted capability_grant_scope && command -v cmake >/dev/null 2>&1; then
    echo "=== capability_grant_scope ==="
    RAN=$((RAN + 1))
    HOST_BUILD="$UBQ_BUILD_ROOT/host-noquic"
    HOST_OBJDIR="$HOST_BUILD/CMakeFiles/ubiquitty-host.dir"
    if cmake -B "$HOST_BUILD" -S "$ROOT" -G "$UBQ_GENERATOR" -DUBIQUITTY_FRONTEND=Host -DCMAKE_BUILD_TYPE=Debug \
                \
                >"$OUT/capgrantscope-host-build.log" 2>&1 \
            && cmake --build "$HOST_BUILD" -j"$BUILD_JOBS" >>"$OUT/capgrantscope-host-build.log" 2>&1; then
        host_objects "$HOST_OBJDIR" "$OUT/capgrantscope-objs.txt" \
            || build_broken "host object list is empty (the harness, not the test under it)"
        if ubq_compile_parallel "$OUT/capability_grant_scope" "$OUT/objects/capability_grant_scope" "$OUT/capgrantscope-build.log" \
                -std=c++23 -DUBIQUITTY_QUIC=1 -I"$QUIC_PREFIX/include" -I"$ROOT/Tests" -I"$ROOT/Source" -- \
                "$ROOT/Tests/Host/capability_grant_scope.cpp" \
                -- "@$OUT/capgrantscope-objs.txt" "${HOST_LINK_ARCHIVES[@]}"; then
            "$OUT/capability_grant_scope" || exit 1
        else
            build_broken "capability_grant_scope link failed (see $OUT/capgrantscope-build.log)"
        fi
    else
        build_broken "capability_grant_scope host build failed (see $OUT/capgrantscope-host-build.log)"
    fi
fi

# consent_pending_toctou — regression lock for the consent-pending-identity
# TOCTOU ("a held agent can mutate its bound identity via a second consent
# request"). Two guards close it: the reject-2nd-HELLO path fires before the
# attach-hold's write site, and TerminateSession's own
# `if (consentPending) break` guards its write site. This proves a held
# agent's second HELLO/Terminate can't retarget the pending consentSessionID/
# consentRequestID, and the human's eventual decision still resolves against
# the ORIGINAL, unmutated session. Same QUIC-OFF host build as consent_owner_gate.
if wanted consent_pending_toctou && command -v cmake >/dev/null 2>&1; then
    echo "=== consent_pending_toctou ==="
    RAN=$((RAN + 1))
    HOST_BUILD="$UBQ_BUILD_ROOT/host-noquic"
    HOST_OBJDIR="$HOST_BUILD/CMakeFiles/ubiquitty-host.dir"
    if cmake -B "$HOST_BUILD" -S "$ROOT" -G "$UBQ_GENERATOR" -DUBIQUITTY_FRONTEND=Host -DCMAKE_BUILD_TYPE=Debug \
                \
                >"$OUT/consentpending-host-build.log" 2>&1 \
            && cmake --build "$HOST_BUILD" -j"$BUILD_JOBS" >>"$OUT/consentpending-host-build.log" 2>&1; then
        host_objects "$HOST_OBJDIR" "$OUT/consentpending-objs.txt" \
            || build_broken "host object list is empty (the harness, not the test under it)"
        if ubq_compile_parallel "$OUT/consent_pending_toctou" "$OUT/objects/consent_pending_toctou" "$OUT/consentpending-build.log" \
                -std=c++23 -DUBIQUITTY_QUIC=1 -I"$QUIC_PREFIX/include" -I"$ROOT/Tests" -I"$ROOT/Source" -- \
                "$ROOT/Tests/Host/consent_pending_toctou.cpp" \
                -- "@$OUT/consentpending-objs.txt" "${HOST_LINK_ARCHIVES[@]}"; then
            "$OUT/consent_pending_toctou" || exit 1
        else
            build_broken "consent_pending_toctou link failed (see $OUT/consentpending-build.log)"
        fi
    else
        build_broken "consent_pending_toctou host build failed (see $OUT/consentpending-host-build.log)"
    fi
fi

# create_ownership_negative — security negative for the create-ownership gate:
# ubq reconnects on every MCP tool call (fresh HELLO per verb), so
# the creating agent arrives on a NEW connection where
# didCreateThisSession=false, which the ownership gate denies unless a standing
# grant exists. Create writes a
# durable ownership grant onto the new session, so a same-identity reconnect
# passes agentHasStandingGrant without a live human. Four assertions:
# (1) same identity reconnects to its created session; (2) different identity
# denied; (3) same identity, foreign session denied; (4) empty-key (tokenless)
# agent denied on reattach, no empty-string bypass. Same QUIC-OFF host build as
# consent_owner_gate/consent_pending_toctou.
if wanted create_ownership_negative && command -v cmake >/dev/null 2>&1; then
    echo "=== create_ownership_negative ==="
    RAN=$((RAN + 1))
    HOST_BUILD="$UBQ_BUILD_ROOT/host-noquic"
    HOST_OBJDIR="$HOST_BUILD/CMakeFiles/ubiquitty-host.dir"
    if cmake -B "$HOST_BUILD" -S "$ROOT" -G "$UBQ_GENERATOR" -DUBIQUITTY_FRONTEND=Host -DCMAKE_BUILD_TYPE=Debug \
                \
                >"$OUT/createowner-host-build.log" 2>&1 \
            && cmake --build "$HOST_BUILD" -j"$BUILD_JOBS" >>"$OUT/createowner-host-build.log" 2>&1; then
        host_objects "$HOST_OBJDIR" "$OUT/createowner-objs.txt" \
            || build_broken "host object list is empty (the harness, not the test under it)"
        if ubq_compile_parallel "$OUT/create_ownership_negative" "$OUT/objects/create_ownership_negative" "$OUT/createowner-build.log" \
                -std=c++23 -DUBIQUITTY_QUIC=1 -I"$QUIC_PREFIX/include" -I"$ROOT/Tests" -I"$ROOT/Source" -- \
                "$ROOT/Tests/Host/create_ownership_negative.cpp" \
                -- "@$OUT/createowner-objs.txt" "${HOST_LINK_ARCHIVES[@]}"; then
            "$OUT/create_ownership_negative" || exit 1
        else
            build_broken "create_ownership_negative link failed (see $OUT/createowner-build.log)"
        fi
    else
        build_broken "create_ownership_negative host build failed (see $OUT/createowner-host-build.log)"
    fi
fi

# ssh_relay_origin — a client the bridge relays from an SSH channel keeps the
# owner's authority but drives its sessions at the Remote origin, so a file read
# Local allows is refused; only the daemon's own bridge may mark. Production
# ingress throughout: the mark's bytes and a real Greet through feedClientFrames,
# real typing through a ScreenClient. Same QUIC-OFF host build as
# create_ownership_negative.
if wanted ssh_relay_origin && command -v cmake >/dev/null 2>&1; then
    echo "=== ssh_relay_origin ==="
    RAN=$((RAN + 1))
    HOST_BUILD="$UBQ_BUILD_ROOT/host-noquic"
    HOST_OBJDIR="$HOST_BUILD/CMakeFiles/ubiquitty-host.dir"
    if cmake -B "$HOST_BUILD" -S "$ROOT" -G "$UBQ_GENERATOR" -DUBIQUITTY_FRONTEND=Host -DCMAKE_BUILD_TYPE=Debug \
                >"$OUT/sshrelay-host-build.log" 2>&1 \
            && cmake --build "$HOST_BUILD" -j"$BUILD_JOBS" >>"$OUT/sshrelay-host-build.log" 2>&1; then
        host_objects "$HOST_OBJDIR" "$OUT/sshrelay-objs.txt" \
            || build_broken "host object list is empty (the harness, not the test under it)"
        if ubq_compile_parallel "$OUT/ssh_relay_origin" "$OUT/objects/ssh_relay_origin" "$OUT/sshrelay-build.log" \
                -std=c++23 -DUBIQUITTY_QUIC=1 -I"$QUIC_PREFIX/include" -I"$ROOT/Tests" -I"$ROOT/Source" -- \
                "$ROOT/Tests/Host/ssh_relay_origin.cpp" \
                -- "@$OUT/sshrelay-objs.txt" "${HOST_LINK_ARCHIVES[@]}"; then
            "$OUT/ssh_relay_origin" || exit 1
        else
            build_broken "ssh_relay_origin link failed (see $OUT/sshrelay-build.log)"
        fi
    else
        build_broken "ssh_relay_origin host build failed (see $OUT/sshrelay-host-build.log)"
    fi
fi

# aggregate_outbound_capacity — the per-client output high-water (16 MiB) alone admits
# N x 16 MiB across N clients (a many-client memory exhaustion below the per-client cap).
# enforceOutboundBudget sums every client's queued output and drops NEWEST-first until
# the total is back under the daemon-wide aggregate cap (4x the per-client high-water =
# 64 MiB). Staging N clients each parked just under the per-client cap can't be driven
# black-box without N live shells flooding output, so this links the daemon objects and
# unit-tests the aggregate enforcement via a friend seam. Same QUIC-OFF host build as
# drop_defer (the cap is QUIC-agnostic; HostQUIC.cpp is an empty TU at QUIC-OFF).
if wanted aggregate_outbound_capacity && command -v cmake >/dev/null 2>&1; then
    echo "=== aggregate_outbound_capacity ==="
    RAN=$((RAN + 1))
    HOST_BUILD="$UBQ_BUILD_ROOT/host-noquic"
    HOST_OBJDIR="$HOST_BUILD/CMakeFiles/ubiquitty-host.dir"
    if cmake -B "$HOST_BUILD" -S "$ROOT" -G "$UBQ_GENERATOR" -DUBIQUITTY_FRONTEND=Host -DCMAKE_BUILD_TYPE=Debug \
                \
                >"$OUT/aggcap-host-build.log" 2>&1 \
            && cmake --build "$HOST_BUILD" -j"$BUILD_JOBS" >>"$OUT/aggcap-host-build.log" 2>&1; then
        host_objects "$HOST_OBJDIR" "$OUT/aggcap-objs.txt" \
            || build_broken "host object list is empty (the harness, not the test under it)"
        if ubq_compile_parallel "$OUT/aggregate_outbound_capacity" "$OUT/objects/aggregate_outbound_capacity" "$OUT/aggcap-build.log" \
                -std=c++23 -DUBIQUITTY_QUIC=1 -I"$QUIC_PREFIX/include" -I"$ROOT/Tests" -I"$ROOT/Source" -- \
                "$ROOT/Tests/Host/aggregate_outbound_capacity.cpp" \
                -- "@$OUT/aggcap-objs.txt" "${HOST_LINK_ARCHIVES[@]}"; then
            "$OUT/aggregate_outbound_capacity" || exit 1
        else
            build_broken "aggregate_outbound_capacity link failed (see $OUT/aggcap-build.log)"
        fi
    else
        build_broken "aggregate_outbound_capacity host build failed (see $OUT/aggcap-host-build.log)"
    fi
fi

# flush_client_cursor — flushClient drains a backlogged client's outputBuffer via a
# consumed-bytes cursor (write from data()+consumed, compact only on full drain or a
# thresholded prefix) instead of erase-from-front per partial write, which is O(n^2)
# memmove across a multi-MiB drain at ~8 KB per POLLOUT wake. Drives the
# private queueToClient/flushClient over a real small-SO_SNDBUF socketpair with a slow
# reader and checks byte-exact ordered delivery, the bounded-dead-prefix invariant,
# cursor reset on full drain, and that the 16 MiB high-water keys on PENDING bytes
# (size - consumed), not the raw buffer size. Same QUIC-OFF host build as drop_defer
# (the drain path is QUIC-agnostic; HostQUIC.cpp is an empty TU at QUIC-OFF).
if wanted flush_client_cursor && command -v cmake >/dev/null 2>&1; then
    echo "=== flush_client_cursor ==="
    RAN=$((RAN + 1))
    HOST_BUILD="$UBQ_BUILD_ROOT/host-noquic"
    HOST_OBJDIR="$HOST_BUILD/CMakeFiles/ubiquitty-host.dir"
    if cmake -B "$HOST_BUILD" -S "$ROOT" -G "$UBQ_GENERATOR" -DUBIQUITTY_FRONTEND=Host -DCMAKE_BUILD_TYPE=Debug \
                \
                >"$OUT/flushcursor-host-build.log" 2>&1 \
            && cmake --build "$HOST_BUILD" -j"$BUILD_JOBS" >>"$OUT/flushcursor-host-build.log" 2>&1; then
        host_objects "$HOST_OBJDIR" "$OUT/flushcursor-objs.txt" \
            || build_broken "host object list is empty (the harness, not the test under it)"
        if ubq_compile_parallel "$OUT/flush_client_cursor" "$OUT/objects/flush_client_cursor" "$OUT/flushcursor-build.log" \
                -std=c++23 -DUBIQUITTY_QUIC=1 -I"$QUIC_PREFIX/include" -I"$ROOT/Tests" -I"$ROOT/Source" -- \
                "$ROOT/Tests/Host/flush_client_cursor.cpp" \
                -- "@$OUT/flushcursor-objs.txt" "${HOST_LINK_ARCHIVES[@]}"; then
            "$OUT/flush_client_cursor" || exit 1
        else
            build_broken "flush_client_cursor link failed (see $OUT/flushcursor-build.log)"
        fi
    else
        build_broken "flush_client_cursor host build failed (see $OUT/flushcursor-host-build.log)"
    fi
fi

# origin_unlabeled_agent — security negative for the origin classifier: a verified-local client
# with an EMPTY agentLabel must be classified as Agent (the gated tier), not Local
# (which bypasses agent-tier gating and allows HostEscaping capabilities without a
# human prompt). On current main the test FAILS — it is the proof of the escalation.
# Same QUIC-OFF host build as drop_defer / origin_staleness.
if wanted origin_unlabeled_agent && command -v cmake >/dev/null 2>&1; then
    echo "=== origin_unlabeled_agent ==="
    RAN=$((RAN + 1))
    HOST_BUILD="$UBQ_BUILD_ROOT/host-noquic"
    HOST_OBJDIR="$HOST_BUILD/CMakeFiles/ubiquitty-host.dir"
    if cmake -B "$HOST_BUILD" -S "$ROOT" -G "$UBQ_GENERATOR" -DUBIQUITTY_FRONTEND=Host -DCMAKE_BUILD_TYPE=Debug \
                \
                >"$OUT/unlabeled-agent-host-build.log" 2>&1 \
            && cmake --build "$HOST_BUILD" -j"$BUILD_JOBS" >>"$OUT/unlabeled-agent-host-build.log" 2>&1; then
        host_objects "$HOST_OBJDIR" "$OUT/unlabeled-agent-objs.txt" \
            || build_broken "host object list is empty (the harness, not the test under it)"
        if ubq_compile_parallel "$OUT/origin_unlabeled_agent" "$OUT/objects/origin_unlabeled_agent" "$OUT/unlabeled-agent-build.log" \
                -std=c++23 -DUBIQUITTY_QUIC=1 -I"$QUIC_PREFIX/include" -I"$ROOT/Tests" -I"$ROOT/Source" -- \
                "$ROOT/Tests/Host/origin_unlabeled_agent.cpp" \
                -- "@$OUT/unlabeled-agent-objs.txt" "${HOST_LINK_ARCHIVES[@]}"; then
            "$OUT/origin_unlabeled_agent" || exit 1
        else
            build_broken "origin_unlabeled_agent link failed (see $OUT/unlabeled-agent-build.log)"
        fi
    else
        build_broken "origin_unlabeled_agent host build failed (see $OUT/unlabeled-agent-host-build.log)"
    fi
fi

# agent_classifier_origin_unify — security negative for the origin-unify gate: a tokenless,
# capability-clean REMOTE (QUIC-shaped) peer must be isAgentClient (not human),
# denied every tokenScopeAllows scope, and rejected under require_token — all
# derived from originOf(), never the self-asserted HELLO caps a remote peer can
# forge. On current main this FAILS on all three — the escalation proof. Same
# QUIC-OFF host build as origin_unlabeled_agent (originOf's Remote guard doesn't
# need a real QUICConnection: empty peerProcess alone is Remote).
if wanted agent_classifier_origin_unify && command -v cmake >/dev/null 2>&1; then
    echo "=== agent_classifier_origin_unify ==="
    RAN=$((RAN + 1))
    HOST_BUILD="$UBQ_BUILD_ROOT/host-noquic"
    HOST_OBJDIR="$HOST_BUILD/CMakeFiles/ubiquitty-host.dir"
    if cmake -B "$HOST_BUILD" -S "$ROOT" -G "$UBQ_GENERATOR" -DUBIQUITTY_FRONTEND=Host -DCMAKE_BUILD_TYPE=Debug \
                \
                >"$OUT/agent-classifier-host-build.log" 2>&1 \
            && cmake --build "$HOST_BUILD" -j"$BUILD_JOBS" >>"$OUT/agent-classifier-host-build.log" 2>&1; then
        host_objects "$HOST_OBJDIR" "$OUT/agent-classifier-objs.txt" \
            || build_broken "host object list is empty (the harness, not the test under it)"
        if ubq_compile_parallel "$OUT/agent_classifier_origin_unify" "$OUT/objects/agent_classifier_origin_unify" "$OUT/agent-classifier-build.log" \
                -std=c++23 -DUBIQUITTY_QUIC=1 -I"$QUIC_PREFIX/include" -I"$ROOT/Tests" -I"$ROOT/Source" -- \
                "$ROOT/Tests/Host/agent_classifier_origin_unify.cpp" \
                -- "@$OUT/agent-classifier-objs.txt" "${HOST_LINK_ARCHIVES[@]}"; then
            "$OUT/agent_classifier_origin_unify" || exit 1
        else
            build_broken "agent_classifier_origin_unify link failed (see $OUT/agent-classifier-build.log)"
        fi
    else
        build_broken "agent_classifier_origin_unify host build failed (see $OUT/agent-classifier-host-build.log)"
    fi
fi

# remote_session_smoke — the daemon's remote-session path end to end, headless:
# the REAL portable RemoteSessionCore over a REAL SubprocessChannel bridged to a
# REAL ubiquitty-host daemon by `ubiquitty-host --attach`. The Linux CI container
# also requires the isolated OpenSSH bootstrap/deployment fixture.
# Validates what the fake-channel unit test can't: attach → checkpoint → typed
# marker → clean-detach reattach persistence over the actual wire.
if wanted remote_session_smoke && command -v cmake >/dev/null 2>&1; then
    echo "=== remote_session_smoke ==="
    RAN=$((RAN + 1))
    if HOST_BINARY="$(ubq_host_binary "$OUT/rss-host-build.log")" \
            && ubq_build_test ubq-shell-remote_session_smoke "$OUT/remote_session_smoke" "$OUT/rss-build.log"; then
        ubq_run_private "$OUT/remote_session_smoke" "$HOST_BINARY" || exit 1
        # The same bootstrap, install and update path with a shell standing in
        # for ssh, in a private HOME: every machine runs the install's digest
        # check and its refusal to end a live session, not only the CI
        # container that can stand up sshd. The daemons it leaves holding the
        # detached sessions are stopped here.
        RSS_HOME="$(mktemp -d /tmp/ubq-rss-XXXXXX)"
        RSS_STATUS=0
        ubq_run_private "$OUT/remote_session_smoke" --ssh "env HOME=$RSS_HOME UBIQUITTY_HOME= /bin/sh -c" "$HOST_BINARY" || RSS_STATUS=1
        for RSS_PID_FILE in "$RSS_HOME"/.ubiquitty/host-*.pid; do
            [ -f "$RSS_PID_FILE" ] || continue
            RSS_PID="$(cat "$RSS_PID_FILE")"
            pkill -P "$RSS_PID" 2>/dev/null || true
            kill "$RSS_PID" 2>/dev/null || true
        done
        # A shell the stopped daemon ran is still ending, and bash writes its
        # history into this HOME as it goes: a file that appears while the
        # directory is being emptied fails the removal with "Directory not
        # empty", and the stanza with it. So the removal is repeated until the
        # directory is gone, for five seconds at most.
        RSS_TRIES=0
        until rm -rf "$RSS_HOME" 2>/dev/null || [ "$RSS_TRIES" -ge 50 ]; do
            RSS_TRIES=$((RSS_TRIES + 1))
            sleep 0.1
        done
        if [ -e "$RSS_HOME" ]; then
            echo "remote_session_smoke: $RSS_HOME is still written to five seconds after its daemons were stopped" >&2
            exit 1
        fi
        [ "$RSS_STATUS" = 0 ] || exit 1
        if [ "${UBQ_SSH_SMOKE_REQUIRED:-0}" = 1 ]; then
            ubq_run_private python3 "$ROOT/Tests/Host/ssh_bootstrap.py" "$OUT/remote_session_smoke" "$HOST_BINARY" || exit 1
        fi
    else
        build_broken "remote_session_smoke build failed (see $OUT/rss-build.log + $OUT/rss-host-build.log)"
    fi
fi

# remote_workspace_smoke — a workspace over Sync with the real client stack
# against a real forked daemon: the projection on attach, a verb minting a
# backed pane, a second client attached to that pane told the same tree, a
# verb from either reaching both, the local model agreeing with both mirrors,
# and a close ending the sessions behind it.
if wanted remote_workspace_smoke && command -v cmake >/dev/null 2>&1; then
    echo "=== remote_workspace_smoke ==="
    RAN=$((RAN + 1))
    if HOST_BINARY="$(ubq_host_binary "$OUT/rws-host-build.log")" \
            && ubq_build_test ubq-shell-remote_workspace_smoke "$OUT/remote_workspace_smoke" "$OUT/rws-build.log"; then
        ubq_run_private "$OUT/remote_workspace_smoke" "$HOST_BINARY" || exit 1
    else
        build_broken "remote_workspace_smoke build failed (see $OUT/rws-build.log + $OUT/rws-host-build.log)"
    fi
fi

# ack_rides_input — an Ack the client owes leaves in the same carrier write as
# the input typed after it: the real client stack over `ubiquitty-host --attach`
# to a real forked daemon, the client's clock stopped and its timers held from
# before the output it owes the Ack for arrives until the keystroke.
if wanted ack_rides_input && command -v cmake >/dev/null 2>&1; then
    echo "=== ack_rides_input ==="
    RAN=$((RAN + 1))
    if HOST_BINARY="$(ubq_host_binary "$OUT/ari-host-build.log")" \
            && ubq_build_test ubq-shell-ack_rides_input "$OUT/ack_rides_input" "$OUT/ari-build.log"; then
        ubq_run_bounded 300 "$OUT/ack_rides_input" "$HOST_BINARY" || exit 1
    else
        build_broken "ack_rides_input build failed (see $OUT/ari-build.log + $OUT/ari-host-build.log)"
    fi
fi

# remote_pane_link_e2e — the remote pane link keeps the PaneLink contract
# against a real daemon: the real client stack over `ubiquitty-host --attach` to
# a real forked daemon, driven only through its PaneLink face, runs the checks
# test_pane_link_conformance runs on the in-process and loopback links, sizes
# the host's pane to the viewport it reports, and reads as Ended once stopped.
if wanted remote_pane_link_e2e && command -v cmake >/dev/null 2>&1; then
    echo "=== remote_pane_link_e2e ==="
    RAN=$((RAN + 1))
    if HOST_BINARY="$(ubq_host_binary "$OUT/rple-host-build.log")" \
            && ubq_build_test ubq-shell-remote_pane_link_e2e "$OUT/remote_pane_link_e2e" "$OUT/rple-build.log"; then
        ubq_run_bounded 300 "$OUT/remote_pane_link_e2e" "$HOST_BINARY" || exit 1
    else
        build_broken "remote_pane_link_e2e build failed (see $OUT/rple-build.log + $OUT/rple-host-build.log)"
    fi
fi

# named_layout_restore — a named workspace survives the daemon that held it:
# real daemons, killed outright between phases, and the real client stack. The
# next daemon lists the workspace as restorable, and attaching by name hands
# back its windows, tabs, split and tab title with a fresh shell in each pane's
# recorded directory at its recorded grid. A named session that ends on its own
# leaves no record, terminating a restorable entry forgets it, and a record file
# that does not decode is logged and read as none.
if wanted named_layout_restore && command -v cmake >/dev/null 2>&1; then
    echo "=== named_layout_restore ==="
    RAN=$((RAN + 1))
    if HOST_BINARY="$(ubq_host_binary "$OUT/nlr-host-build.log")" \
            && ubq_build_test ubq-shell-named_layout_restore "$OUT/named_layout_restore" "$OUT/nlr-build.log"; then
        ubq_run_private "$OUT/named_layout_restore" "$HOST_BINARY" || exit 1
    else
        build_broken "named_layout_restore build failed (see $OUT/nlr-build.log + $OUT/nlr-host-build.log)"
    fi
fi

# remote_clear_e2e — Cmd-K in a host session with the client stack the app
# attaches through, against a real forked daemon: the clearing window's screen
# and history go, a second window on the session loses the same history, and a
# window without the Drive scope cannot clear.
if wanted remote_clear_e2e && command -v cmake >/dev/null 2>&1; then
    echo "=== remote_clear_e2e ==="
    RAN=$((RAN + 1))
    if HOST_BINARY="$(ubq_host_binary "$OUT/rce-host-build.log")" \
            && ubq_build_test ubq-shell-remote_clear_e2e "$OUT/remote_clear_e2e" "$OUT/rce-build.log"; then
        ubq_run_private "$OUT/remote_clear_e2e" "$HOST_BINARY" || exit 1
    else
        build_broken "remote_clear_e2e build failed (see $OUT/rce-build.log + $OUT/rce-host-build.log)"
    fi
fi

if wanted adopt_session && command -v cmake >/dev/null 2>&1; then
    echo "=== adopt_session ==="
    RAN=$((RAN + 1))
    ADOPT_SESSION_QUIC_FLAGS=()
    ADOPT_SESSION_LINK_FLAGS=()
    [ "$(uname)" = "Darwin" ] && ADOPT_SESSION_LINK_FLAGS=(-framework CoreFoundation -framework Security)
    if HOST_BINARY="$(ubq_host_binary "$OUT/adopt-host-build.log")" \
            && ADOPT_SESSION_QUIC="$(sed -n 's/^UBIQUITTY_QUIC:BOOL=//p' "$HOST_TEST_BUILD/CMakeCache.txt")" \
            && {
                case "$ADOPT_SESSION_QUIC" in
                    ON|1|YES|TRUE|Y|on|yes|true|y)
                        ADOPT_SESSION_QUIC_FLAGS=(-DUBIQUITTY_QUIC=1 -I"$QUIC_PREFIX/include")
                        ADOPT_SESSION_LINK_FLAGS=("${HOST_LINK_ARCHIVES[@]}") ;;
                    OFF|0|NO|FALSE|N|off|no|false|n) true ;;
                    *) echo "adopt_session: missing or unrecognized UBIQUITTY_QUIC cache value" >&2; false ;;
                esac
            } \
            && host_objects "$HOST_TEST_BUILD/CMakeFiles/ubiquitty-host.dir" "$OUT/adopt-objs.txt" \
            && ubq_compile_parallel "$OUT/adopt_session" "$OUT/objects/adopt_session" "$OUT/adopt-build.log" \
                    -std=c++23 ${ADOPT_SESSION_QUIC_FLAGS[@]+"${ADOPT_SESSION_QUIC_FLAGS[@]}"} -I"$ROOT/Tests" -I"$ROOT/Source" -- \
                    "$ROOT/Tests/Host/adopt_session.cpp" -- "@$OUT/adopt-objs.txt" ${ADOPT_SESSION_LINK_FLAGS[@]+"${ADOPT_SESSION_LINK_FLAGS[@]}"}; then
        ubq_run_bounded 300 "$OUT/adopt_session" || exit 1
    else
        build_broken "adopt_session build failed (see $OUT/adopt-build.log + $OUT/adopt-host-build.log)"
    fi
fi

# adopt_handoff_time — windows of about 1, 8 and 35 MiB of history are handed
# to a real daemon over its local socket by the sender an application uses
# (reHomeViaAdopt, with a fixture-self daemon and retained in-process owners),
# and each is adopted: the handshake's deadline bounds silence, not the whole,
# so a session is not lost to a timer while its bytes move or are applied.
# The time each handoff took is printed.
if wanted adopt_handoff_time && command -v cmake >/dev/null 2>&1; then
    echo "=== adopt_handoff_time ==="
    RAN=$((RAN + 1))
    ADOPT_HANDOFF_QUIC_FLAGS=()
    ADOPT_HANDOFF_LINK_FLAGS=()
    [ "$(uname)" = "Darwin" ] && ADOPT_HANDOFF_LINK_FLAGS=(-framework CoreFoundation -framework Security)
    if HOST_BINARY="$(ubq_host_binary "$OUT/adopthandoff-host-build.log")" \
            && ADOPT_HANDOFF_QUIC="$(sed -n 's/^UBIQUITTY_QUIC:BOOL=//p' "$HOST_TEST_BUILD/CMakeCache.txt")" \
            && {
                case "$ADOPT_HANDOFF_QUIC" in
                    ON|1|YES|TRUE|Y|on|yes|true|y)
                        ADOPT_HANDOFF_QUIC_FLAGS=(-DUBIQUITTY_QUIC=1 -I"$QUIC_PREFIX/include")
                        ADOPT_HANDOFF_LINK_FLAGS=("${HOST_LINK_ARCHIVES[@]}") ;;
                    OFF|0|NO|FALSE|N|off|no|false|n) true ;;
                    *) echo "adopt_handoff_time: missing or unrecognized UBIQUITTY_QUIC cache value" >&2; false ;;
                esac
            } \
            && host_objects "$HOST_TEST_BUILD/CMakeFiles/ubiquitty-host.dir" "$OUT/adopthandoff-objs.txt" \
            && ubq_compile_parallel "$OUT/adopt_handoff_time" "$OUT/objects/adopt_handoff_time" "$OUT/adopthandoff-build.log" \
                    -std=c++23 ${ADOPT_HANDOFF_QUIC_FLAGS[@]+"${ADOPT_HANDOFF_QUIC_FLAGS[@]}"} -I"$ROOT/Tests" -I"$ROOT/Source" -- \
                    "$ROOT/Tests/Host/adopt_handoff_time.cpp" -- "@$OUT/adopthandoff-objs.txt" ${ADOPT_HANDOFF_LINK_FLAGS[@]+"${ADOPT_HANDOFF_LINK_FLAGS[@]}"}; then
        ubq_run_bounded 300 "$OUT/adopt_handoff_time" || exit 1
    else
        build_broken "adopt_handoff_time build failed (see $OUT/adopthandoff-build.log + $OUT/adopthandoff-host-build.log)"
    fi
fi

# pane_facts_attachments — a shared login shell, spawned as the app spawns it
# and re-homed by the real in-process ownership sender, is titled from facts that
# name it "-zsh" as a local pane does; an agent attaching beside the human
# receives the human's record byte for byte, and its attachment leaves the
# human's whole across the next change.
if wanted pane_facts_attachments && command -v cmake >/dev/null 2>&1; then
    echo "=== pane_facts_attachments ==="
    RAN=$((RAN + 1))
    PANE_FACTS_QUIC_FLAGS=()
    PANE_FACTS_LINK_FLAGS=()
    [ "$(uname)" = "Darwin" ] && PANE_FACTS_LINK_FLAGS=(-framework CoreFoundation -framework Security)
    if HOST_BINARY="$(ubq_host_binary "$OUT/panefacts-host-build.log")" \
            && PANE_FACTS_QUIC="$(sed -n 's/^UBIQUITTY_QUIC:BOOL=//p' "$HOST_TEST_BUILD/CMakeCache.txt")" \
            && {
                case "$PANE_FACTS_QUIC" in
                    ON|1|YES|TRUE|Y|on|yes|true|y)
                        PANE_FACTS_QUIC_FLAGS=(-DUBIQUITTY_QUIC=1 -I"$QUIC_PREFIX/include")
                        PANE_FACTS_LINK_FLAGS=("${HOST_LINK_ARCHIVES[@]}") ;;
                    OFF|0|NO|FALSE|N|off|no|false|n) true ;;
                    *) echo "pane_facts_attachments: missing or unrecognized UBIQUITTY_QUIC cache value" >&2; false ;;
                esac
            } \
            && host_objects "$HOST_TEST_BUILD/CMakeFiles/ubiquitty-host.dir" "$OUT/panefacts-objs.txt" \
            && ubq_compile_parallel "$OUT/pane_facts_attachments" "$OUT/objects/pane_facts_attachments" "$OUT/panefacts-build.log" \
                    -std=c++23 ${PANE_FACTS_QUIC_FLAGS[@]+"${PANE_FACTS_QUIC_FLAGS[@]}"} -I"$ROOT/Tests" -I"$ROOT/Source" -- \
                    "$ROOT/Tests/Host/pane_facts_attachments.cpp" -- "@$OUT/panefacts-objs.txt" ${PANE_FACTS_LINK_FLAGS[@]+"${PANE_FACTS_LINK_FLAGS[@]}"}; then
        ubq_run_private "$OUT/pane_facts_attachments" || exit 1
    else
        build_broken "pane_facts_attachments build failed (see $OUT/panefacts-build.log + $OUT/panefacts-host-build.log)"
    fi
fi

# adopt_fd_cli — retained in-process owners use the real adoption sender and
# kernel ownership gate. The parent/ancestor commits; an unrelated sibling gets
# exact NotOwner evidence; a session leader prepares then exactly aborts.
if wanted adopt_fd_cli && command -v cmake >/dev/null 2>&1; then
    echo "=== adopt_fd_cli ==="
    RAN=$((RAN + 1))
    ADOPT_FD_QUIC_FLAGS=()
    ADOPT_FD_LINK_FLAGS=()
    [ "$(uname)" = "Darwin" ] && ADOPT_FD_LINK_FLAGS=(-framework CoreFoundation -framework Security)
    if HOST_BINARY="$(ubq_host_binary "$OUT/adoptfd-host-build.log")" \
            && ADOPT_FD_QUIC="$(sed -n 's/^UBIQUITTY_QUIC:BOOL=//p' "$HOST_TEST_BUILD/CMakeCache.txt")" \
            && {
                case "$ADOPT_FD_QUIC" in
                    ON|1|YES|TRUE|Y|on|yes|true|y)
                        ADOPT_FD_QUIC_FLAGS=(-DUBIQUITTY_QUIC=1 -I"$QUIC_PREFIX/include")
                        ADOPT_FD_LINK_FLAGS=("${HOST_LINK_ARCHIVES[@]}") ;;
                    OFF|0|NO|FALSE|N|off|no|false|n) true ;;
                    *) echo "adopt_fd_cli: missing or unrecognized UBIQUITTY_QUIC cache value" >&2; false ;;
                esac
            } \
            && host_objects "$HOST_TEST_BUILD/CMakeFiles/ubiquitty-host.dir" "$OUT/adoptfd-objs.txt" \
            && ubq_compile_parallel "$OUT/adopt_fd_cli" "$OUT/objects/adopt_fd_cli" "$OUT/adoptfd-build.log" \
                    -std=c++23 ${ADOPT_FD_QUIC_FLAGS[@]+"${ADOPT_FD_QUIC_FLAGS[@]}"} -I"$ROOT/Tests" -I"$ROOT/Source" -- \
                    "$ROOT/Tests/Host/adopt_fd_cli.cpp" -- "@$OUT/adoptfd-objs.txt" ${ADOPT_FD_LINK_FLAGS[@]+"${ADOPT_FD_LINK_FLAGS[@]}"}; then
        ubq_run_bounded 300 "$OUT/adopt_fd_cli" || exit 1
    else
        build_broken "adopt_fd_cli build failed (see $OUT/adoptfd-build.log + $OUT/adoptfd-host-build.log)"
    fi
fi

# gate_report — the verdict format lives twice, in Scripts/gate-report-lib.sh and
# in Scripts/gate_report.py, and this stanza runs the test holding them to the
# same bytes. A format spelled twice and checked nowhere is one edit
# from being two formats, which is the whole failure the format exists to stop.
if wanted gate_report; then
    echo "=== gate_report ==="
    RAN=$((RAN + 1))
    "$ROOT/Tests/test-gate-report.sh" || exit 1
fi

# snapshot_clean — the source-export script must reject a missing manifest path
# and a dry-run/failed run without ever touching an existing destination, and
# must refuse a dead link in a shipped document or a missing legal file.
if wanted snapshot_clean; then
    echo "=== snapshot_clean ==="
    RAN=$((RAN + 1))
    "$ROOT/Tests/test-snapshot-clean.sh" || exit 1
fi

# local_hooks — run-local-hooks.sh runs nothing and prints nothing in a clone
# with no hooks.d, and otherwise runs each executable check in name order from
# the top of the tree, with the hook's arguments and input, stopping at the
# first that fails; a linked worktree runs its clone's checks.
if wanted local_hooks; then
    echo "=== local_hooks ==="
    RAN=$((RAN + 1))
    python3 "$ROOT/Scripts/test-run-local-hooks.py" || exit 1
fi

# pre_push_cache — a retry reuses only successful gates from the exact source,
# command, and toolchain identity that produced their verdict.
if wanted pre_push_cache; then
    echo "=== pre_push_cache ==="
    RAN=$((RAN + 1))
    "$ROOT/Tests/test-pre-push-cache.sh" || exit 1
fi

if wanted pre_push_hook; then
    echo "=== pre_push_hook ==="
    RAN=$((RAN + 1))
    bash "$ROOT/Tests/test-pre-push-hook.sh" || exit 1
fi

# pre_commit_hook — CONTRIBUTING tells every contributor to install the hooks,
# so a plain clone or linked worktree commits; only a checkout named in
# UBIQUITTY_SHARED_CHECKOUT is pull-only. A gate whose tool the machine lacks
# lets the commit through and is named.
if wanted pre_commit_hook; then
    echo "=== pre_commit_hook ==="
    RAN=$((RAN + 1))
    bash "$ROOT/Tests/test-pre-commit-hook.sh" || exit 1
fi

# toolchain_floor — a compiler below the floor is refused at configure time and
# by Base/Types.h, each naming the floor, and the PGO profile goes only to a
# compiler that reads it.
if wanted toolchain_floor; then
    echo "=== toolchain_floor ==="
    RAN=$((RAN + 1))
    "$ROOT/Tests/test-toolchain-floor.sh" || exit 1
fi

# build_scripts_pgo — build-agent.sh, build-host.sh and the Linux app's build
# hand the caller's UBIQUITTY_PGO to each optimised configure, ON when it is
# unset, and hand none to a Debug one. A stand-in cmake records the arguments.
if wanted build_scripts_pgo; then
    echo "=== build_scripts_pgo ==="
    RAN=$((RAN + 1))
    "$ROOT/Tests/test-build-scripts-pgo.sh" || exit 1
fi

# The driver compiles three different ways and a change to how it reaches a
# compiler can break one while the others stay green. Cheap, and it runs before
# anything that would be misread as a Core failure.
if wanted driver_shapes; then
    echo "=== driver_shapes ==="
    RAN=$((RAN + 1))
    "$ROOT/Tests/test-driver-shapes.sh" || exit 1
fi

# Every gate that carries a --self-test runs it. A self-test nothing executes is
# unwatched, and an unwatched control cannot be told from a rotten one until
# someone runs it.
#
# Discovered rather than listed, because a per-gate stanza lets unrun self-tests
# accumulate: adding a gate gives nobody a reason to edit a list of gates. The
# stanzas below keep their own self-test lines so that naming one still runs it
# alone; this covers the rest, and covers a new gate by its existing.
if wanted gate_self_tests; then
    echo "=== gate_self_tests ==="
    RAN=$((RAN + 1))
    python3 "$ROOT/Scripts/run-gate-self-tests.py" --self-test || exit 1
    python3 "$ROOT/Scripts/run-gate-self-tests.py" || exit 1
fi

# ubiquitty-kit and the app carry this repository's Scripts/ as a library and run
# their own hooks, section tables and push gates from it. A consumer is built
# from the library as it stands, with a submodule and a table of its own, and
# each of those is run from there: the roots, check-style.py, the hooks git
# dispatches and a push list sourcing the library's runner.
if wanted library_roots; then
    echo "=== library_roots ==="
    RAN=$((RAN + 1))
    python3 "$ROOT/Scripts/test-library-roots.py" || exit 1
fi

# check-empty-array-expansion reports nothing on a tree that has no instances,
# which is the same output it would produce if its rule matched nothing at all.
# Its self-test runs the rule against a construct it must catch and three it
# must not, and executes the bad one under /bin/bash so the premise — that this
# aborts at all — is measured rather than asserted.
if wanted empty_array_expansion; then
    echo "=== empty_array_expansion ==="
    RAN=$((RAN + 1))
    python3 "$ROOT/Scripts/check-empty-array-expansion.py" --self-test || exit 1
    python3 "$ROOT/Scripts/check-empty-array-expansion.py" || exit 1
fi

# check-unwired-flags reads one `git grep` pass and compares a SET, so a tree
# where every flag has a caller and a tree where the reader pattern stopped
# matching both produce no findings. Its self-test settles which: it proves the
# token boundary holds (a short flag is not reported as wired by a longer
# sibling that contains it), and classifies a wired flag, an unwired one and a
# name that occurs nowhere.
# check-pgo-commit-shape reports nothing on a commit that touches no profile,
# which is the same output it would give if its path predicate stopped matching.
# The self-test runs it against the two real violations in this history and two
# commits that must stay clean, so the rule is proven against the tree rather
# than asserted.
if wanted pgo_commit_shape; then
    echo "=== pgo_commit_shape ==="
    RAN=$((RAN + 1))
    python3 "$ROOT/Scripts/check-pgo-commit-shape.py" --self-test || exit 1
fi

# check-pgo-applies' verdict is the compiler's, and a self-test cannot fixture a
# compiler; what it holds is the arithmetic around that verdict. The allowance
# tracks the pre-push gates' per-push bound, the budget sidecar
# round-trips, a stale or unreadable sidecar is refused rather than read as a
# zero budget, the warning and the refusal turn where the percentages say, and
# a diagnostic in a format the parser does not know counts as unparsed rather
# than as no discard.
if wanted pgo_applies; then
    echo "=== pgo_applies ==="
    RAN=$((RAN + 1))
    python3 "$ROOT/Scripts/check-pgo-applies.py" --self-test || exit 1
fi

# check-core-subset-symbols reads the host build's object files rather than the
# sources, because the question is which definitions a link needs and only the
# symbol tables answer it. It REFUSES when a canonical source has no object, so a
# tree that was never built cannot report an intact cut.
if wanted core_subset_symbols; then
    echo "=== core_subset_symbols ==="
    RAN=$((RAN + 1))
    # Named, because this run built it: a named tree missing a canonical
    # source is a build that dropped one and refuses, where an unnamed tree
    # missing one is read as another run's leftovers and stands down.
    # The caller's tree wins. Under ctest a stanza is handed its own scratch
    # directory, and the build directory derived from that one holds no object
    # files at all — a tree the gate can only refuse. The ctest entry
    # (Scripts/cmake/UbiquittyTests.cmake) passes the tree it is actually about.
    subsetTree="${UBQ_HOST_TEST_TREE:-$CTEST_BUILD_DIR}"
    UBQ_HOST_TEST_TREE="$subsetTree" python3 "$ROOT/Scripts/check-core-subset-symbols.py" --self-test || exit 1
    UBQ_HOST_TEST_TREE="$subsetTree" python3 "$ROOT/Scripts/check-core-subset-symbols.py" || exit 1
fi

# License scope is a Python filesystem and source-export contract, with no Core binary dependency.
if wanted license_scope; then
    echo "=== license_scope ==="
    RAN=$((RAN + 1))
    python3 "$ROOT/Scripts/check-licenses.py" --self-test || exit 1
fi

if wanted sync_struct_defaults; then
    echo "=== sync_struct_defaults ==="
    RAN=$((RAN + 1))
    python3 "$ROOT/Scripts/check-sync-struct-defaults.py" --self-test || exit 1
    python3 "$ROOT/Scripts/check-sync-struct-defaults.py" || exit 1
fi

# Three tests of the build system itself. A test nobody runs reports on the
# tree it was written against, so the machinery they cover — hook installation,
# the provenance stamp, and the bench staleness rule — could regress with every
# suite still green.
#
# All three work in their own mktemp directory and leave this tree untouched,
# and together they are cheap, which is why they are stanzas rather than a
# tier someone has to remember.
if wanted install_hooks; then
    echo "=== install_hooks ==="
    RAN=$((RAN + 1))
    "$ROOT/Scripts/test-install-hooks.sh" || exit 1
fi

if wanted build_provenance; then
    echo "=== build_provenance ==="
    RAN=$((RAN + 1))
    "$ROOT/Scripts/test-build-provenance.sh" || exit 1
fi

if wanted build_version; then
    echo "=== build_version ==="
    RAN=$((RAN + 1))
    "$ROOT/Scripts/test-build-version.sh" || exit 1
fi

# Build-wrapper tests use stand-in tools, without compilers or installed SDKs.
if wanted build_wrappers; then
    echo "=== build_wrappers ==="
    RAN=$((RAN + 1))
    python3 "$ROOT/Scripts/test-build-wrappers.py" || exit 1
fi

# Build lanes against a throwaway repository: lock contention, a worktree's
# uncommitted changes, a dirty lane reset, release on failure and on a signal,
# and clean-build-roots keeping lane roots. Nothing is compiled.
if wanted build_lane; then
    echo "=== build_lane ==="
    RAN=$((RAN + 1))
    python3 "$ROOT/Scripts/build-lane.py" --self-test || exit 1
fi

if wanted compare_bench_staleness; then
    echo "=== compare_bench_staleness ==="
    RAN=$((RAN + 1))
    python3 "$ROOT/Scripts/test-compare-bench-staleness.py" || exit 1
    python3 "$ROOT/Scripts/test-compare-bench-direction.py" || exit 1
fi

# check-mutants.sh picks the tests that can notice a mutant instead of running
# all of them, and picking too few looks exactly like picking correctly — the
# verdicts print the same, from less evidence. This stubs ninja and ctest and
# checks what the gate ASKS for, so it needs no compiler and adds no load.
if wanted covering_selection; then
    echo "=== covering_selection ==="
    RAN=$((RAN + 1))
    "$ROOT/Tests/test-covering-selection.sh" || exit 1
fi

# An A/B builds each arm with the profile checked in at its own commit, so two
# arms of unequal profile freshness measure placement alongside code. Runs
# against a synthetic repository, so it needs neither this history nor a build.
if wanted bench_baseline; then
    echo "=== bench_baseline ==="
    RAN=$((RAN + 1))
    "$ROOT/Tests/test-bench-baseline.sh" || exit 1
fi

# The mutation gate maps changed lines back to the file they came from. Getting
# that wrong applies mutants to another file's line numbers and still reports
# killed and survived, so the mapping needs an assertion of its own.
if wanted changed_line_mapping; then
    echo "=== changed_line_mapping ==="
    RAN=$((RAN + 1))
    "$ROOT/Tests/test-changed-line-mapping.sh" || exit 1
fi

# snapshot_prerequisites — the snapshot check refuses a build root without
# its two binaries and names the script that builds each.
if wanted snapshot_prerequisites; then
    echo "=== snapshot_prerequisites ==="
    RAN=$((RAN + 1))
    "$ROOT/Tests/test-snapshot-prerequisites.sh" || exit 1
fi

if wanted shell_lane_census; then
    echo "=== shell_lane_census ==="
    RAN=$((RAN + 1))
    "$ROOT/Tests/test-shell-lane-census.sh" || exit 1
fi

# The stanzas of the other repositories this checkout holds. Each is a run of
# `if wanted <name>; then` blocks written against this driver's helpers, and
# names its stanzas literally for Scripts/stanza-declarations.awk to read.
for ubq_stanza_file in ${UBQ_STANZA_FILES+"${UBQ_STANZA_FILES[@]}"}; do
    [ "$ubq_stanza_file" = "$0" ] && continue
    # shellcheck source=/dev/null
    . "$ubq_stanza_file"
done

# Filter sanity LAST, after every section (incl. the QUIC ones above): a filter
# that named only tests which never ran is a typo worth flagging.
#
if [ "$SKIPPED" -gt 0 ]; then
    echo "run-tests.sh: $SKIPPED stanza(s) declined for want of a dependency" >&2
fi

# RAN counts stanzas that ANSWERED, not stanzas that exercised anything: a
# stanza skipping for want of a sanitizer runtime, a fuzzer-capable compiler or
# the QUIC archives still increments it. So a run on a machine missing every
# optional toolchain reports the same RAN as a full one and exits 0.
#
# Each skip says so on its own line, which is the only place that distinction
# currently lives. Auditing it means reading them; there is no count to compare.
# A stanza that declined ANSWERED the filter, so it counts as matched: without
# this, filtering to a stanza whose dependency is absent reports "no tests
# matched" and exits 1, which names the filter as the problem rather than the
# missing dependency the SKIP line just described.
# Per filter, not over the set. Judging the whole set lets one good name mask
# any number of misses: `run-tests.sh test_vt100 test_display` ran test_display,
# reported PASSED, and said nothing about test_vt100 — which is not a stanza.
# A caller who named two suites and got one has a green run that skipped what
# they asked for, and nothing to read that says so.
ubq_unmatched=()
for ubq_filter in ${FILTERS+"${FILTERS[@]}"}; do
    case "$FILTERS_MATCHED" in *" $ubq_filter "*) ;; *) ubq_unmatched+=("$ubq_filter") ;; esac
done
if [ "${#ubq_unmatched[@]}" -gt 0 ]; then
    ubq_report_unknown_filters "${ubq_unmatched[@]}"
    exit 1
fi

if [ ${#FILTERS[@]} -gt 0 ] && [ "$((RAN + SKIPPED))" -eq 0 ]; then
    echo "run-tests.sh: no tests matched filter: ${FILTERS[*]}" >&2
    exit 1
fi

# The handoff's verdict is part of this script's exit status. Stanzas that all
# passed say nothing about a ctest run that failed above, and reporting them as
# the whole answer would turn that failure into a pass.
if [ "$CTEST_FAILED" != 0 ]; then
    echo "run-tests.sh: ctest failed; its failing entries are listed in the ctest section above" >&2
    exit 1
fi

# The last line of a completed run says so, and every failure path above exits
# before reaching it. That makes its ABSENCE the signal: a reader who sees the
# final stanza's description and no verdict after it is looking at a run that
# stopped, not at a run that passed.
#
# Worth stating because the exit code does not survive the way this output is
# usually read. `run-tests.sh … | tail` reports tail's status, so a driver that
# exited 1 is indistinguishable from one that exited 0 unless the verdict is in
# the text itself.
if [ "$SKIP_CTEST_COVERED" = "1" ] && [ "$CTEST_COVERED_CONSULTED" = "0" ]; then
    echo "run-tests.sh: SKIP_CTEST_COVERED=1 decided nothing — every stanza came" \
         "from an explicit filter, which answers first"
fi
# After the ctest handoff the counts are this loop's alone: ctest ran the C++
# tests and the shell stanzas it schedules, and reported them above.
if [ "$USE_CTEST" = "1" ]; then
    echo "run-tests.sh: PASSED — $RAN ran, $SKIPPED skipped after ctest, whose result is above"
else
    echo "run-tests.sh: PASSED — $RAN ran, $SKIPPED skipped"
fi

# The declines, named. A reader who scrolls to the verdict has passed every SKIP
# line on the way, and "20 skipped" reads the same whether what went missing was
# one sanitizer runtime or every lane that touches the network.
#
# Deduplicated because one absent runtime declines several stanzas with the same
# sentence, and a list that repeats it four times buries the reason that appears
# once.
if [ "${#SKIPPED_REASONS[@]}" -gt 0 ]; then
    printf '%s\n' ${SKIPPED_REASONS[@]+"${SKIPPED_REASONS[@]}"} \
        | sort | uniq -c \
        | sed -E 's/^ *([0-9]+) /  did not run (\1 stanza(s)): /'
fi
# The list is built where a decline prints its reason, and a few sites count one
# without printing one. Saying so beats a list that looks complete: the whole
# point of naming them is that the reader can tell what this run does not speak
# for, and a short list answers that question wrongly rather than partially.
if [ "${#SKIPPED_REASONS[@]}" -ne "$SKIPPED" ]; then
    echo "  ($((SKIPPED - ${#SKIPPED_REASONS[@]})) of the $SKIPPED declines named no reason and are not listed above)"
fi
