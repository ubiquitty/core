#!/bin/bash
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

# run-tests-lib.sh — what a stanza calls on its driver.
#
# Sourced by Scripts/run-tests.sh of every repository that has a driver. A stanza
# is written against these helpers (`wanted`, `ubq_skip`, `ubq_skip_unless_required`,
# `ubq_build_test`) and the driver's state: FILTERS, FILTERS_MATCHED, FUZZ_ONLY,
# SKIP_CTEST_COVERED, UBQ_OWN_STANZAS (optional), ROOT and CORE_ROOT (with it), CTEST_COVERED, CTEST_COVERED_CONSULTED, CTEST_BUILD_DIR,
# BUILD_JOBS, SKIPPED, SKIPPED_REASONS, and ubq_configure_ctest_tree, which the
# driver defines for the tree it configures.
#
# shellcheck disable=SC2034  # state the sourcing driver reads

# wanted <name> — true when no filter was given or the name matches.
# A name CTest already ran is skipped here. An explicit filter always wins, so
# `run-tests.sh test_host_tools` runs that shell recipe outside CMake.
wanted() {
    # A stanza of ubiquitty-core is not the checkout under test's to run when the
    # driver serves another repository (the driver sets UBQ_OWN_STANZAS there).
    if [ -n "${UBQ_OWN_STANZAS:-}" ] && [ "${ROOT:-}" != "${CORE_ROOT:-}" ]; then
        case $'\n'"$UBQ_OWN_STANZAS"$'\n' in *$'\n'"$1"$'\n'*) ;; *) return 1 ;; esac
    fi
    # --fuzz-only declines every stanza. The fuzz section reads WITH_FUZZ rather
    # than this, so it still runs; what it stops is the suite running a second
    # time inside the job whose name says it fuzzes. Whether a fuzzer actually
    # ran is FUZZ_REQUIRED's question, which checks for each produced binary --
    # pair the two wherever a green line has to mean fuzzing happened.
    if [ "$FUZZ_ONLY" = "1" ]; then
        return 1
    fi
    if [ ${#FILTERS[@]} -eq 0 ]; then
        if [ "$SKIP_CTEST_COVERED" = "1" ]; then
            # Recorded because the verdict cannot otherwise say whether the
            # covered list decided anything: a run where it skipped forty
            # stanzas and a run where it was never opened print the same line.
            CTEST_COVERED_CONSULTED=1
            if [ -f "$CTEST_COVERED" ] && grep -qx -- "$1" "$CTEST_COVERED"; then
                return 1
            fi
        fi
        return 0
    fi
    local n
    for n in "${FILTERS[@]}"; do
        if [ "$n" = "$1" ]; then
            # Record the ANSWER, not the run. A stanza that declines for a
            # missing dependency answered its filter, and the SKIP line it
            # printed is the diagnosis — calling that an unmatched filter names
            # the wrong thing.
            case "$FILTERS_MATCHED" in *" $n "*) ;; *) FILTERS_MATCHED="$FILTERS_MATCHED$n " ;; esac
            return 0
        fi
    done
    return 1
}

ubq_build_test_targets() { # <build log> <target...>
    local log="$1"
    shift
    local tree="${UBQ_HOST_TEST_TREE:-$CTEST_BUILD_DIR}"
    if [ ! -f "$tree/CMakeCache.txt" ]; then
        ubq_configure_ctest_tree "$tree" >"$log" 2>&1 || return 1
    fi
    cmake --build "$tree" --target "$@" -j"$BUILD_JOBS" >>"$log" 2>&1
}

# Link the stanza's output name to its executable in the shared test tree.
# CTest's build fixtures supply the prebuilt promise; direct runs build on demand.
ubq_build_test() { # <target> <binary> <build log>
    local target="$1" binary="$2" log="$3"
    local tree="${UBQ_HOST_TEST_TREE:-$CTEST_BUILD_DIR}"
    if [ "${UBQ_TEST_TARGETS_PREBUILT:-0}" != 1 ]; then
        ubq_build_test_targets "$log" "$target" || return 1
    fi
    if [ ! -x "$tree/Tests/$target" ]; then
        printf 'shared test target is missing: %s\n' "$tree/Tests/$target" >>"$log"
        return 1
    fi
    ln -sf "$tree/Tests/$target" "$binary"
}

# Records a decline for the verdict and prints it as a `SKIP:` line in the log.
#
# - Parameter 1: Why this stanza declined, in the words the reader needs.
ubq_skip() {
    echo "SKIP: $1"
    SKIPPED_REASONS+=("$1")
}

# Whether a tool this stanza needs is one the run is entitled to expect.
#
# A missing tool on a developer's machine is a decline; the same tool missing
# where the environment was built to carry it is a hole in the run, and a hole
# that answers with SKIP and exit 0 reads as coverage. UBQ_REQUIRED_TOOLS names
# the ones an environment promises, so a promise broken fails rather than
# declines. Empty everywhere the promise was never made.
#
# - Parameter 1: The tool's command name.
# - Returns: 0 when its absence must fail the run.
ubq_tool_is_required() {
    case " ${UBQ_REQUIRED_TOOLS:-} " in
        *" $1 "*) return 0 ;;
        *) return 1 ;;
    esac
}

# Declines for a missing tool, or fails when the environment promised it.
#
# - Parameter 1: The tool's command name.
# - Parameter 2: Why this stanza wanted it.
ubq_skip_unless_required() {
    if ubq_tool_is_required "$1"; then
        echo "MISSING-TOOL: $1 — $2"
        echo "  The stanza asserted nothing, and UBQ_REQUIRED_TOOLS names $1, so this"
        echo "  environment is one where that absence is a fault in the environment"
        echo "  rather than the shape of a machine. A decline here would exit 0 and"
        echo "  read as coverage."
        exit 2
    fi
    ubq_skip "$2"
}
