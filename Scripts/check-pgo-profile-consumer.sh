#!/usr/bin/env bash
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0
#
# check-pgo-profile-consumer.sh — the profile is only handed to a reader of it.
#
# Support/pgo carries LLVM's IR-instrumentation format, which only clang reads.
# GCC's -fprofile-use takes a DIRECTORY of .gcda files, so handed a .profdata
# path it resolves `<path>/<object>.gcda`, finds nothing, emits one
# -Wmissing-profile, and COMPILES ANYWAY. Exit 0. The binary is a plain -O3
# build, and every status line above it in the log says profile-guided.
#
# That is why this is a gate and not a comment. The failure is silent by
# construction: it produces a working binary, a green build, and a number that
# is quietly off on the lanes the profile was measured on. Nothing else
# in the tree would notice -- not the compiler, not the tests, not a reviewer
# reading a diff that adds a flag which is correct on the machine they run.
#
# Two things are checked, because the flag is added from two places:
#
#   the shell path   Scripts/pgo-flags.sh decides for the bash builders, and
#                    its own --self-test proves the decision.
#   the CMake path   the block in Scripts/cmake/UbiquittyOptimize.cmake, which
#                    every build of Core includes, must gate on the compiler's
#                    identity, and then ask that compiler whether it reads the
#                    profile (ubiquitty_compiler_reads_profile) before adding the
#                    flag. Without the first, every GCC Release build is a silent
#                    no-op. Without the second, a clang older than the
#                    llvm-profdata that wrote the profile fails every
#                    translation unit on "unsupported instrumentation profile
#                    format version", which is the stock Ubuntu 24.04 clang.
#
#   ./Scripts/check-pgo-profile-consumer.sh [--cmake <file>]
#   ./Scripts/check-pgo-profile-consumer.sh --self-test
#
# --cmake names the file holding the PGO block, Scripts/cmake/UbiquittyOptimize.cmake
# of this checkout when not named.
set -uo pipefail

cd "$(dirname "$0")/.." || exit 1

pgoBlock=Scripts/cmake/UbiquittyOptimize.cmake
if [ "${1:-}" = "--cmake" ]; then
    pgoBlock="${2:?--cmake needs a file}"
    shift 2
fi

# The CMake PGO block, and whether it establishes the compiler and asks it about
# the profile before adding the flag. Read as text rather than by configuring a
# project: the check is that the guard is written, and a configure run would
# only prove it for the one compiler this machine happens to have.
cmakeGuardsOnCompiler() {
    local file="$1"
    awk '
        /^[[:space:]]*#/ { next }
        # The profile itself or the copy rooted for this tree (pgo_localize.py).
        /-fprofile-use=\$\{(UBIQUITTY_PGO_PROFILE|_ubiquitty_pgo_local)\}/ { flagLine = NR }
        /CMAKE_CXX_COMPILER_ID MATCHES "Clang"/ { guardLine = NR }
        /ubiquitty_compiler_reads_profile\(/ { probeLine = NR }
        END {
            if (flagLine == 0) { print "no-flag"; exit }
            if (guardLine == 0 || guardLine > flagLine) { print "unguarded"; exit }
            if (probeLine == 0 || probeLine > flagLine) { print "unprobed"; exit }
            print "guarded"
        }
    ' "$file"
}

if [ "${1:-}" = "--self-test" ]; then
    selfTestStatus=0
    fixture=$(mktemp -d) || exit 1
    trap 'rm -rf "$fixture"' EXIT

    # Guarded: the compiler is established, and asked, above the flag.
    cat > "$fixture/guarded.cmake" <<'GUARDED'
        elseif(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
            message(STATUS "PGO: NOT APPLIED")
        else()
            ubiquitty_compiler_reads_profile(readable refusal "${UBIQUITTY_PGO_PROFILE}" stamp)
            if(NOT readable)
                message(STATUS "PGO: NOT APPLIED")
            else()
                add_compile_options(-fprofile-use=${UBIQUITTY_PGO_PROFILE})
            endif()
        endif()
GUARDED
    # Unprobed: clang is established and never asked, the shape that hands a
    # profile from a newer LLVM to every translation unit of an older clang.
    cat > "$fixture/unprobed.cmake" <<'UNPROBED'
        elseif(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
            message(STATUS "PGO: NOT APPLIED")
        else()
            add_compile_options(-fprofile-use=${UBIQUITTY_PGO_PROFILE})
        endif()
UNPROBED
    # Unguarded: the flag added on the strength of the file existing, which is
    # the shape that compiles clean under GCC and does nothing.
    cat > "$fixture/unguarded.cmake" <<'UNGUARDED'
        if(EXISTS "${UBIQUITTY_PGO_PROFILE}")
            add_compile_options(-fprofile-use=${UBIQUITTY_PGO_PROFILE})
        endif()
UNGUARDED

    verdict=$(cmakeGuardsOnCompiler "$fixture/guarded.cmake")
    [ "$verdict" = "guarded" ] || { echo "self-test: a guarded block read as '$verdict'" >&2; selfTestStatus=1; }
    verdict=$(cmakeGuardsOnCompiler "$fixture/unguarded.cmake")
    [ "$verdict" = "unguarded" ] || { echo "self-test: an unguarded block read as '$verdict'" >&2; selfTestStatus=1; }
    verdict=$(cmakeGuardsOnCompiler "$fixture/unprobed.cmake")
    [ "$verdict" = "unprobed" ] || { echo "self-test: an unprobed block read as '$verdict'" >&2; selfTestStatus=1; }

    if ! bash Scripts/pgo-flags.sh --self-test >/dev/null 2>&1; then
        echo "self-test: Scripts/pgo-flags.sh --self-test does not pass" >&2
        selfTestStatus=1
    fi

    [ "$selfTestStatus" = 0 ] && echo "check-pgo-profile-consumer: self-test ok (unguarded and unprobed CMake blocks are caught; a guarded one is not)"
    exit "$selfTestStatus"
fi

FAIL=0

verdict=$(cmakeGuardsOnCompiler "$pgoBlock")
case "$verdict" in
    guarded)
        echo "ok: the CMake PGO block establishes the compiler and asks it before adding -fprofile-use"
        ;;
    unprobed)
        echo "FAIL: $pgoBlock adds -fprofile-use to any clang without asking it whether it reads" >&2
        echo "      the profile (ubiquitty_compiler_reads_profile). A clang older than the LLVM that" >&2
        echo "      wrote Support/pgo fails every translation unit on the file instead of building" >&2
        echo "      without it." >&2
        FAIL=$((FAIL + 1))
        ;;
    unguarded)
        echo "FAIL: $pgoBlock adds -fprofile-use without establishing that the compiler reads" >&2
        echo "      LLVM's profile format. Under GCC that flag compiles clean and does nothing," >&2
        echo "      so the build ships unprofiled while reporting otherwise." >&2
        FAIL=$((FAIL + 1))
        ;;
    *)
        echo "FAIL: $pgoBlock no longer adds -fprofile-use at all, so no Release build is" >&2
        echo "      profiled. If that is intended, this gate is what needs removing, deliberately." >&2
        FAIL=$((FAIL + 1))
        ;;
esac

if bash Scripts/pgo-flags.sh --self-test; then
    :
else
    echo "FAIL: Scripts/pgo-flags.sh no longer decides correctly (its --self-test above)" >&2
    FAIL=$((FAIL + 1))
fi

if [ "$FAIL" -ne 0 ]; then
    echo "check-pgo-profile-consumer: FAILED — $FAIL check(s)" >&2
    exit 1
fi
echo "check-pgo-profile-consumer: PASSED"
