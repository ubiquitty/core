#!/usr/bin/env bash
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0
#
# test-toolchain-floor.sh — a compiler below the floor is refused by name, and
# the training profile goes only to a compiler that reads it.
#
#   configure  Scripts/cmake/ToolchainFloor.cmake refuses GCC 13 and Clang 18
#              with the remedy and admits GCC 14, Clang 19 and AppleClang. The
#              compiler identity is passed as data through `cmake -P`, so no old
#              compiler has to be installed for the refusal to be exercised.
#              Then the tree itself is configured with a compiler stated to be
#              GCC 13.3 and must stop at the floor, which is what fails if
#              Scripts/cmake/UbiquittyToolchain.cmake stops calling the
#              function.
#   header     Base/Types.h carries Base/ToolchainFloor.h: a C++20 compile of it
#              fails naming C++23, and a standard library whose <version> does
#              not announce std::expected fails naming std::expected. That second
#              case is the Clang 18 over libstdc++ shape, where <expected> exists
#              and is empty, so header presence would have admitted it.
#   profile    ubiquitty_compiler_reads_profile admits a profile the compiler's
#              own llvm-profdata wrote and refuses one it cannot read. Then a
#              Release configure of the tree, with a clang the floor admits,
#              hands no translation unit a profile that clang cannot read and
#              hands every one a profile it can. It needs a clang and that
#              clang's llvm-profdata, and the tree configure needs that clang to
#              clear the floor. Without them the arm says SKIPPED by name, and
#              the other two still decide the verdict.
#
#   Tests/test-toolchain-floor.sh
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# shellcheck source=Tests/shell-assertions.sh
. "$ROOT/Tests/shell-assertions.sh"

FIXTURE="$(mktemp -d "${TMPDIR:-/tmp}/ubiquitty-toolchain-floor.XXXXXX")" || exit 1
FIXTURE="$(cd "$FIXTURE" && pwd -P)" || exit 1
trap 'rm -rf "$FIXTURE"' EXIT

if ! command -v cmake >/dev/null 2>&1; then
    echo "test-toolchain-floor: FAILED — no cmake on PATH, and every arm here drives it" >&2
    exit 1
fi
CXX_UNDER_TEST="${CXX:-c++}"

echo "configure: the floor refuses by compiler identity, with the remedy"
cat >"$FIXTURE/floor.cmake" <<FLOOR
include("$ROOT/Scripts/cmake/ToolchainFloor.cmake")
ubiquitty_check_toolchain_floor("\${COMPILER_ID}" "\${COMPILER_VERSION}")
FLOOR

floorVerdict() { # <compiler-id> <compiler-version>
    local output status
    output="$(cmake -DCOMPILER_ID="$1" -DCOMPILER_VERSION="$2" -P "$FIXTURE/floor.cmake" 2>&1)"
    status=$?
    printf '%s\n%s' "$status" "$output"
}

verdict="$(floorVerdict GNU 13.3.0)"
check "GCC 13.3 is refused" "1" "${verdict%%$'\n'*}"
contains "the refusal names the GCC floor" "needs GCC 14 or newer" "$verdict"
contains "the refusal names the Ubuntu package" "apt install g++-14" "$verdict"
contains "the refusal names the compilers to configure with" "CC=gcc-14 CXX=g++-14" "$verdict"

verdict="$(floorVerdict Clang 18.1.3)"
check "Clang 18.1 is refused" "1" "${verdict%%$'\n'*}"
contains "the refusal names the Clang floor" "needs Clang 19 or newer" "$verdict"
contains "the refusal names the Ubuntu package" "apt install clang-19" "$verdict"

for admitted in "GNU 14.2.0" "GNU 15.2.0" "Clang 19.1.1" "Clang 22.0.0" "AppleClang 17.0.0"; do
    # shellcheck disable=SC2086
    verdict="$(floorVerdict $admitted)"
    check "$admitted is admitted" "0" "${verdict%%$'\n'*}"
done

# The call in Scripts/cmake/UbiquittyToolchain.cmake, not only the function: the
# tree is configured with a compiler stated to be GCC 13.3. With
# CMAKE_CXX_COMPILER_ID_RUN set, CMake takes the stated identity instead of
# identifying the compiler, so whichever compiler is installed stands in for one
# that is not. The two computed defaults are what identification would have
# supplied, and CMake stops without them. run-tests.sh supports a machine with
# Ninja and no make, so both tree configures take Ninja when it is there rather
# than CMake's default generator.
generator=()
command -v ninja >/dev/null 2>&1 && generator=(-G Ninja)
output="$(cmake -S "$ROOT" -B "$FIXTURE/tree-floor" ${generator[@]+"${generator[@]}"} -DCMAKE_CXX_COMPILER="$CXX_UNDER_TEST" \
    -DCMAKE_CXX_COMPILER_ID_RUN=1 -DCMAKE_CXX_COMPILER_ID=GNU -DCMAKE_CXX_COMPILER_VERSION=13.3.0 \
    -DCMAKE_CXX_STANDARD_COMPUTED_DEFAULT=17 -DCMAKE_CXX_EXTENSIONS_COMPUTED_DEFAULT=ON \
    -DUBIQUITTY_FRONTEND=Core -DUBIQUITTY_QUIC=OFF 2>&1)"
check "the tree refuses a compiler stated as GCC 13.3" "1" "$?"
contains "the tree's refusal names the GCC floor" "needs GCC 14 or newer" "$output"
contains "the tree's refusal names the compilers to use" "CC=gcc-14 CXX=g++-14" "$output"

echo
echo "header: Base/Types.h refuses a compiler below the floor by name"
mkdir -p "$FIXTURE/header"
printf '#include "Base/Types.h"\n' >"$FIXTURE/header/types.cpp"
printf '#include "Base/ToolchainFloor.h"\n' >"$FIXTURE/header/floor.cpp"

if output="$("$CXX_UNDER_TEST" -std=c++23 -fsyntax-only -I "$ROOT/Source" "$FIXTURE/header/types.cpp" 2>&1)"; then
    pass "Types.h compiles as C++23 on $CXX_UNDER_TEST"
else
    fail "Types.h compiles as C++23 on $CXX_UNDER_TEST" \
        "the compiler running this suite is itself below the floor, or the floor is wrong:" \
        "$(printf '%s' "$output" | head -n 5)"
fi

if output="$("$CXX_UNDER_TEST" -std=c++20 -fsyntax-only -I "$ROOT/Source" "$FIXTURE/header/types.cpp" 2>&1)"; then
    fail "a C++20 compile of Types.h is refused" "it compiled, so the floor is not reached from Types.h"
else
    contains "a C++20 compile of Types.h is refused naming C++23" "requires C++23" "$output"
fi

# A <version> that announces nothing stands in for a standard library that
# ships <expected> without the template. Found ahead of the real one because
# directories named with -I are searched before the system's.
mkdir -p "$FIXTURE/header/library"
printf '#pragma once\n' >"$FIXTURE/header/library/version"
if output="$("$CXX_UNDER_TEST" -std=c++23 -fsyntax-only -I "$FIXTURE/header/library" -I "$ROOT/Source" \
        "$FIXTURE/header/floor.cpp" 2>&1)"; then
    fail "a library without std::expected is refused" "it compiled, so the floor asks about the header, not the template"
else
    contains "a library without std::expected is refused by name" "requires std::expected" "$output"
fi

echo
echo "profile: the training profile goes only to a compiler that reads it"
clangFloor="$(sed -n 's/^set(UBIQUITTY_CLANG_FLOOR \([0-9][0-9]*\))$/\1/p' "$ROOT/Scripts/cmake/ToolchainFloor.cmake")"
if [ -z "$clangFloor" ]; then
    fail "the Clang floor reads from ToolchainFloor.cmake" "no 'set(UBIQUITTY_CLANG_FLOOR <n>)' line was found"
fi

# The llvm-profdata of the compiler's own LLVM. Asked of the directory the
# compiler resolves to first: -print-prog-name searches the directory it was
# invoked from, and on Debian /usr/bin/llvm-profdata is an alternatives link that
# can name a newer LLVM than /usr/bin/clang++, whose profile that clang refuses
# for a reason unrelated to this tree.
profileToolFor() { # <clang> -> that clang's llvm-profdata, or nothing
    local resolved tool
    resolved="$(command -v "$1")"
    resolved="$(readlink -f "$resolved" 2>/dev/null || printf '%s' "$resolved")"
    if [ -x "${resolved%/*}/llvm-profdata" ]; then
        printf '%s' "${resolved%/*}/llvm-profdata"
        return
    fi
    tool="$("$1" -print-prog-name=llvm-profdata 2>/dev/null)"
    case "$tool" in
        /*) [ -x "$tool" ] && printf '%s' "$tool" ;;
    esac
}

# The newest clang first, so the tree configure runs wherever a clang that
# clears the floor is installed beside an older default one.
clang=""
profdata=""
for candidate in ${UBQ_TEST_CLANG:-} $(compgen -c clang++- 2>/dev/null | grep -E '^clang\+\+-[0-9]+$' | sort -t- -k2 -rn | uniq) clang++; do
    command -v "$candidate" >/dev/null 2>&1 || continue
    "$candidate" -dM -E -x c++ /dev/null 2>/dev/null | grep -q '__clang__' || continue
    tool="$(profileToolFor "$candidate")"
    [ -n "$tool" ] || continue
    clang="$candidate"
    profdata="$tool"
    break
done

if [ -z "$clang" ]; then
    echo "  SKIPPED: no clang with its own llvm-profdata on PATH; the profile arm did not run"
else
    echo "  using $clang with $profdata"
    printf '# IR level Instrumentation Flag\n:ir\n' >"$FIXTURE/empty.proftext"
    if ! "$profdata" merge -o "$FIXTURE/readable.profdata" "$FIXTURE/empty.proftext" >"$FIXTURE/merge.log" 2>&1; then
        fail "$profdata writes a profile to probe with" "$(head -n 3 "$FIXTURE/merge.log")"
    else
        printf 'not a profile\n' >"$FIXTURE/unreadable.profdata"
        mkdir -p "$FIXTURE/project"
        cat >"$FIXTURE/project/CMakeLists.txt" <<'PROJECT'
cmake_minimum_required(VERSION 3.13)
project(ProfileProbe LANGUAGES CXX)
include("${UBQ_ROOT}/Scripts/cmake/ToolchainFloor.cmake")
ubiquitty_compiler_reads_profile(readable refusal "${READABLE_PROFILE}" readable)
message(STATUS "readable-profile=${readable}")
ubiquitty_compiler_reads_profile(readable refusal "${UNREADABLE_PROFILE}" unreadable)
message(STATUS "unreadable-profile=${readable} refusal=${refusal}")
PROJECT
        output="$(cmake -S "$FIXTURE/project" -B "$FIXTURE/project-build" ${generator[@]+"${generator[@]}"} \
            -DCMAKE_CXX_COMPILER="$clang" -DUBQ_ROOT="$ROOT" \
            -DREADABLE_PROFILE="$FIXTURE/readable.profdata" \
            -DUNREADABLE_PROFILE="$FIXTURE/unreadable.profdata" 2>&1)"
        contains "a profile the compiler's own tool wrote is read" "readable-profile=TRUE" "$output"
        contains "a profile the compiler cannot read is refused" "unreadable-profile=FALSE" "$output"
        contains "the refusal carries the compiler's reason" "reading profile" "$output"

        # The probe as Scripts/cmake/UbiquittyOptimize.cmake calls it, through a
        # Release configure of the tree, which needs a clang the floor admits.
        # AppleClang is numbered apart from LLVM and admitted by identity, which
        # __apple_build_version__ announces.
        macros="$("$clang" -dM -E -x c++ /dev/null 2>/dev/null)"
        major="$(printf '%s\n' "$macros" | awk '$2 == "__clang_major__" { print $3 }')"
        if ! printf '%s\n' "$macros" | grep -q '__apple_build_version__' \
                && [ "${major:-0}" -lt "${clangFloor:-0}" ]; then
            echo "  SKIPPED: $clang is Clang $major, below the floor of $clangFloor; the tree configure did not run"
        else
            # The matching C compiler, so the tree's C sources are handed the
            # profile by the same LLVM that was asked about it.
            cCompiler=()
            case "$clang" in
                *clang++*) command -v "${clang/clang++/clang}" >/dev/null 2>&1 \
                    && cCompiler=(-DCMAKE_C_COMPILER="${clang/clang++/clang}") ;;
            esac
            tree="$FIXTURE/tree-profile"
            output="$(cmake -S "$ROOT" -B "$tree" ${generator[@]+"${generator[@]}"} \
                -DCMAKE_CXX_COMPILER="$clang" ${cCompiler[@]+"${cCompiler[@]}"} \
                -DCMAKE_BUILD_TYPE=Release -DUBIQUITTY_FRONTEND=Core -DUBIQUITTY_QUIC=OFF \
                -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
                -DUBIQUITTY_PGO_PROFILE="$FIXTURE/unreadable.profdata" 2>&1)"
            check "the tree configures past an unreadable profile" "0" "$?"
            contains "the tree says the profile was not applied" "PGO: NOT APPLIED" "$output"
            commands="$(cat "$tree/compile_commands.json" 2>/dev/null)"
            contains "the tree's compile database lists Core sources" "/Source/Core/" "$commands"
            absent "no translation unit is handed the unreadable profile" "-fprofile-use" "$commands"

            output="$(cmake -S "$ROOT" -B "$tree" -DUBIQUITTY_PGO_PROFILE="$FIXTURE/readable.profdata" 2>&1)"
            check "the tree reconfigures with a readable profile" "0" "$?"
            commands="$(cat "$tree/compile_commands.json" 2>/dev/null)"
            if printf '%s\n' "$macros" | grep -q '#define __linux__ '; then
                # A Linux profile applies only with the release image and
                # toolchain it records (Scripts/pgo_toolchain.py). This fixture
                # profile records neither, so even a readable one is refused.
                contains "a Linux tree refuses a profile without its trained toolchain" \
                    "PGO: NOT APPLIED" "$output"
                if [ -z "${UBQ_CI_LINUX_IMAGE_ID:-}" ]; then
                    contains "outside the release image the refusal names it" \
                        "Linux PGO requires Scripts/ci-linux-image.sh" "$output"
                fi
                absent "no Linux translation unit is handed the profile" "-fprofile-use" "$commands"
            else
                contains "the tree says the readable profile is applied" \
                    "PGO: using $FIXTURE/readable.profdata" "$output"
                localizedProfile="$(printf '%s\n' "$output" | sed -n 's/^-- PGO: using .* as //p')"
                if [ -z "$localizedProfile" ] || [ ! -f "$localizedProfile" ]; then
                    fail "the tree identifies its localized profile" "no existing profile was reported"
                else
                    "$profdata" show "$localizedProfile" >"$FIXTURE/localized-profile.log" 2>&1
                    check "the localized profile is readable" "0" "$?"
                    contains "the compile commands carry the readable profile" \
                        "-fprofile-use=$localizedProfile" "$commands"
                fi
            fi
        fi
    fi
fi

report "test-toolchain-floor" "the floor refuses GCC 13 and Clang 18 by name, the tree calls it, Types.h carries it, and the tree asks before it hands over the profile"
