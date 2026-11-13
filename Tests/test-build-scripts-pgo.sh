#!/usr/bin/env bash
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0
#
# test-build-scripts-pgo.sh — the bash builders hand UBIQUITTY_PGO to each
# optimised configure and to no Debug one.
#
# Scripts/build-agent.sh, build-host.sh and build-linux-gtk.sh run cmake
# themselves, so a UBIQUITTY_PGO in the caller's environment reaches the build
# only through the argument each one passes. Each runs here against a stand-in
# `cmake` first on PATH that records its arguments and builds nothing:
#
#   UBIQUITTY_PGO=OFF   the configure is handed -DUBIQUITTY_PGO=OFF
#   unset               it is handed -DUBIQUITTY_PGO=ON, which replaces an OFF
#                       an earlier run left in that build directory's cache
#   a Debug configure   it is handed no UBIQUITTY_PGO, which Debug never reads
#
# build-linux-gtk.sh first runs its prerequisite check, which asks pkg-config
# and the compiler about GTK and Vulkan and looks for glslc, so all three have
# stand-ins that say yes.
#
# It also captures each step's output when quiet, so a failed configure or
# build must still print what the step said: the compiler-floor refusal is a
# configure failure, and its remedy is only in that output.
#
#   Tests/test-build-scripts-pgo.sh
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# shellcheck source=Tests/shell-assertions.sh
. "$ROOT/Tests/shell-assertions.sh"

FIXTURE="$(mktemp -d "${TMPDIR:-/tmp}/ubiquitty-build-scripts-pgo.XXXXXX")"
FIXTURE="$(cd "$FIXTURE" && pwd)"
trap 'rm -rf "$FIXTURE"' EXIT

STUBS="$FIXTURE/bin"
CMAKE_LOG="$FIXTURE/cmake.log"
mkdir -p "$STUBS"
cat >"$STUBS/cmake" <<'CMAKE'
#!/bin/sh
printf '%s\n' "$*" >>"$UBQ_TEST_CMAKE_LOG"
case "${UBQ_TEST_CMAKE_FAIL:-}:$1" in
    configure:-B) echo "Ubiquitty needs GCC 14 or newer; this is GCC 13.3.0."; exit 1 ;;
    build:--build) echo "stand-in.cpp:1: error: the build step failed"; exit 1 ;;
    warn:-B) printf '%s\n' "-- Configuring done" "CMake Warning at CMakeLists.txt:1 (message):" \
        "  a backend was left out" "" "-- Generating done" ;;
esac
exit 0
CMAKE
cat >"$STUBS/pkg-config" <<'PKGCONFIG'
#!/bin/sh
case "$1" in --exists | --atleast-version=*) exit 0 ;; esac
exit 1
PKGCONFIG
printf '#!/bin/sh\nexit 0\n' >"$STUBS/stand-in-cxx"
printf '#!/bin/sh\nexit 0\n' >"$STUBS/glslc"
chmod +x "$STUBS/cmake" "$STUBS/pkg-config" "$STUBS/stand-in-cxx" "$STUBS/glslc"

configureOf() { # <UBIQUITTY_PGO value, or - for unset> <script> [arguments...]
    local pgo="$1" script="$2" status
    shift 2
    : >"$CMAKE_LOG"
    local environment=(
        PATH="$STUBS:$PATH"
        UBQ_TEST_CMAKE_LOG="$CMAKE_LOG"
        UBQ_BUILD_ROOT="$FIXTURE/build"
        UBQ_BUILD_JOBS=1
        CXX="$STUBS/stand-in-cxx"
        VERBOSE=0
    )
    if [ "$pgo" = "-" ]; then
        env -u UBIQUITTY_PGO "${environment[@]}" bash "$ROOT/Scripts/$script" "$@" >"$FIXTURE/script.log" 2>&1
    else
        env "${environment[@]}" UBIQUITTY_PGO="$pgo" bash "$ROOT/Scripts/$script" "$@" >"$FIXTURE/script.log" 2>&1
    fi
    status=$?
    if [ "$status" -ne 0 ]; then
        printf 'exit %s: %s\n' "$status" "$(tail -n 5 "$FIXTURE/script.log")"
        return
    fi
    grep -e '-DUBIQUITTY_FRONTEND=' "$CMAKE_LOG"
}

expectPGO() { # <label> <expected -DUBIQUITTY_PGO argument> <configure line>
    contains "$1 configures" "-DUBIQUITTY_FRONTEND=" "$3"
    contains "$1 passes $2" "$2" "$3"
}

expectNoPGO() { # <label> <configure line>
    contains "$1 configures" "-DUBIQUITTY_FRONTEND=" "$2"
    absent "$1 passes no UBIQUITTY_PGO" "UBIQUITTY_PGO" "$2"
}

echo "build-agent.sh"
expectPGO "OFF, Release" "-DUBIQUITTY_PGO=OFF" "$(configureOf OFF build-agent.sh)"
expectPGO "unset, Release" "-DUBIQUITTY_PGO=ON" "$(configureOf - build-agent.sh)"
expectNoPGO "OFF, --debug" "$(configureOf OFF build-agent.sh --debug)"

echo
echo "build-host.sh"
expectPGO "OFF, Release" "-DUBIQUITTY_PGO=OFF" "$(configureOf OFF build-host.sh)"
expectPGO "unset, --release" "-DUBIQUITTY_PGO=ON" "$(configureOf - build-host.sh --release)"
expectNoPGO "OFF, --debug" "$(configureOf OFF build-host.sh --debug)"

# The GTK build script ships with the repository that holds the GTK frontend.
if [ -f "$ROOT/Scripts/build-linux-gtk.sh" ]; then
echo
echo "build-linux-gtk.sh"
expectPGO "OFF, Release" "-DUBIQUITTY_PGO=OFF" "$(configureOf OFF build-linux-gtk.sh Release)"
expectPGO "unset, RelWithDebInfo" "-DUBIQUITTY_PGO=ON" "$(configureOf - build-linux-gtk.sh RelWithDebInfo)"
expectPGO "OFF, MinSizeRel" "-DUBIQUITTY_PGO=OFF" "$(configureOf OFF build-linux-gtk.sh MinSizeRel)"
expectNoPGO "OFF, Debug" "$(configureOf OFF build-linux-gtk.sh)"

failureOf() { # <configure|build|warn>: build-linux-gtk.sh's quiet output when that step fails, or warns
    local status
    env PATH="$STUBS:$PATH" UBQ_TEST_CMAKE_LOG="$FIXTURE/failure.log" UBQ_TEST_CMAKE_FAIL="$1" \
        UBQ_BUILD_ROOT="$FIXTURE/build" UBQ_BUILD_JOBS=1 CXX="$STUBS/stand-in-cxx" VERBOSE=0 \
        bash "$ROOT/Scripts/build-linux-gtk.sh" >"$FIXTURE/failure.out" 2>&1
    status=$?
    printf 'exit %s\n' "$status"
    cat "$FIXTURE/failure.out"
}

echo
echo "build-linux-gtk.sh, quiet, when a step fails"
output="$(failureOf configure)"
contains "a failed configure exits 1" "exit 1" "$output"
contains "a failed configure prints what cmake said" "needs GCC 14 or newer" "$output"
output="$(failureOf build)"
contains "a failed build exits 1" "exit 1" "$output"
contains "a failed build prints the compiler's error" "error: the build step failed" "$output"

# The build step writes over the configure's log, so a warning the configure
# raised is printed before it, heading and body, or nobody reads it.
echo
echo "build-linux-gtk.sh, quiet, when the configure warns"
output="$(failureOf warn)"
contains "a warning configure still builds" "exit 0" "$output"
contains "the warning's heading is printed" "CMake Warning at CMakeLists.txt:1" "$output"
contains "the warning's body is printed" "a backend was left out" "$output"
absent "the configure's other lines are not" "Generating done" "$output"

echo
echo "build-linux-gtk.sh, the GPU shaders"
contains "unset, the shaders are built" "-DUBIQUITTY_LINUX_GPU_SHADERS=ON" "$(configureOf - build-linux-gtk.sh)"
contains "OFF reaches the configure" "-DUBIQUITTY_LINUX_GPU_SHADERS=OFF" \
    "$(UBIQUITTY_LINUX_GPU_SHADERS=OFF configureOf - build-linux-gtk.sh)"
fi

# Exercise the training driver without compiling: the fake corpus emits the
# required lane names and a raw shard, while the real resolver chooses a tool.
# A Linux fixture makes xcrun unavailable; the Apple fixture resolves through it.
TRAIN_TREE="$FIXTURE/trainer"
TRAIN_LOG="$FIXTURE/trainer.log"
mkdir -p "$TRAIN_TREE/Scripts" "$TRAIN_TREE/Support/pgo"
cp "$ROOT/Scripts/pgo-train.sh" "$ROOT/Scripts/pgo-flags.sh" \
    "$ROOT/Scripts/pgo_localize.py" "$ROOT/Scripts/pgo_toolchain.py" "$TRAIN_TREE/Scripts/"
printf 'shipping profile\n' >"$TRAIN_TREE/Support/pgo/portable-core.profdata"
cat >"$TRAIN_TREE/Scripts/portable-core-lib.sh" <<'CORE'
ubiquitty_lto_flag() { printf '%s' '-flto=thin'; }
build_portable_core_archive() {
    printf 'archive %s\n' "$2" >>"$UBQ_TEST_TRAIN_LOG"
    PORTABLE_CORE_ARCHIVE="$1/core.a"
    touch "$PORTABLE_CORE_ARCHIVE"
}
ubiquitty_core_pal_stub_source() { touch "$1"; }
CORE
printf 'print("fixture profile applies")\n' >"$TRAIN_TREE/Scripts/check-pgo-applies.py"
cat >"$STUBS/training-cxx" <<'COMPILER'
#!/bin/sh
case " $* " in
    *" --version "*) printf 'fixture clang 22\n'; exit 0 ;;
    *" -dumpmachine "*) printf 'fixture-linux-gnu\n'; exit 0 ;;
    *" -dM "*)
        if [ "${UBQ_TEST_TRAIN_COMPILER:-clang}" = clang ]; then
            echo '#define __clang__ 1'
        else
            echo '#define __GNUC__ 15'
        fi
        case "${UBQ_TEST_TRAIN_TARGET:-${UBQ_TEST_TRAIN_PLATFORM:-apple}}" in
            linux|linux-x86_64) printf '#define __linux__ 1\n#define __x86_64__ 1\n' ;;
            linux-arm64) printf '#define __linux__ 1\n#define __aarch64__ 1\n' ;;
            apple-x86_64) printf '#define __APPLE__ 1\n#define __x86_64__ 1\n' ;;
            apple) printf '#define __APPLE__ 1\n#define __aarch64__ 1\n' ;;
        esac
        exit 0 ;;
esac
printf 'compile %s\n' "$*" >>"$UBQ_TEST_TRAIN_LOG"
while [ "$#" -gt 0 ]; do
    if [ "$1" = -o ]; then
        output="$2"
        break
    fi
    shift
done
cat >"$output" <<'CORPUS'
#!/bin/sh
printf 'corpus %s stream=%s\n' "${0##*/}" "${UBQ_BENCH_STREAM-unset}" >>"$UBQ_TEST_TRAIN_LOG"
[ "${UBQ_TEST_TRAIN_MODE:-fixed}" = fixed ] && printf '%s\n' '"training_mode": "fixed-input-counts"'
printf '%s\n' '"render_build" "render_build_subpixel" "render_build_full" "render_build_underline" "parse_plain" "parse_plain_syncfeed" "parse_sgr_syncfeed" "parse_wide_syncfeed" "parse_emoji_syncfeed" "parse_charset_syncfeed" "sync_burst_k8" "key_encode" "parse_charset" "ascii" "latin1_multibyte" "cjk_wide" "combining" "sgr_truecolor" "legacy_graphics"'
touch "$UBQ_TEST_TRAIN_RAW"
CORPUS
chmod +x "$output"
COMPILER
cat >"$STUBS/llvm-profdata" <<'PROFDATA'
#!/bin/sh
printf '%s %s\n' "${0##*/}" "$*" >>"$UBQ_TEST_TRAIN_LOG"
[ "$1" = merge ] && [ "$2" = -output ] || exit 1
printf 'private profile\n' >"$3"
PROFDATA
cp "$STUBS/llvm-profdata" "$STUBS/apple-profdata"
cat >"$STUBS/xcrun" <<'XCRUN'
#!/bin/sh
if [ "${UBQ_TEST_TRAIN_PLATFORM:-linux}" = apple ] && [ "$1" = --find ] && [ "$2" = llvm-profdata ]; then
    printf '%s\n' "$UBQ_TEST_APPLE_PROFDATA"
    exit 0
fi
exit 127
XCRUN
chmod +x "$STUBS/training-cxx" "$STUBS/llvm-profdata" "$STUBS/apple-profdata" "$STUBS/xcrun"
ln -s training-cxx "$STUBS/clang++"

trainOf() { # <linux|apple> <clang|gcc> [private output, or default]
    local status
    local destination=("UBQ_PGO_TRAIN_OUT=${3:-$FIXTURE/private/$1.profdata}")
    if [ "${3:-}" = default ]; then
        destination=(-u UBQ_PGO_TRAIN_OUT)
    fi
    : >"$TRAIN_LOG"
    env "${destination[@]}" PATH="$STUBS:$PATH" CXX="$STUBS/training-cxx" \
        UBQ_BUILD_ROOT="$TRAIN_TREE" \
        UBQ_PGO_LLVM="$STUBS" \
        UBQ_CI_LINUX_IMAGE_ID="sha256:$(printf '%064d' 1)" UBQ_CI_LINUX_RECIPE=fixture \
        UBQ_TEST_TRAIN_PLATFORM="$1" UBQ_TEST_TRAIN_COMPILER="$2" \
        UBQ_TEST_APPLE_PROFDATA="$STUBS/apple-profdata" \
        UBQ_TEST_TRAIN_LOG="$TRAIN_LOG" \
        UBQ_TEST_TRAIN_RAW="$TRAIN_TREE/BuildPGOTrain/profraw/training.profraw" \
        UBQ_BENCH_STREAM="$FIXTURE/unrequested-stream" \
        bash "$TRAIN_TREE/Scripts/pgo-train.sh" >"$FIXTURE/train.out" 2>&1
    status=$?
    printf 'exit %s\n' "$status"
    cat "$FIXTURE/train.out" "$TRAIN_LOG"
}

echo
echo "pgo-train.sh, portable tool lookup"
output="$(trainOf linux clang)"
contains "Linux training succeeds without xcrun" "exit 0" "$output"
contains "Linux training merges through PATH" "llvm-profdata merge -output $FIXTURE/private/linux.profdata" "$output"
check "Linux training writes its private profile" "private profile" "$(cat "$FIXTURE/private/linux.profdata")"
contains "stack driver uses fixed training calls" "-DUBIQUITTY_PGO_TRAINING=1" "$(grep 'compile .*bench_stack.cpp' "$TRAIN_LOG")"
absent "Core keeps shipping compile flags" "UBIQUITTY_PGO_TRAINING" "$(grep '^archive ' "$TRAIN_LOG")"
absent "fixed-count corpora needs no harness override" "UBIQUITTY_PGO_TRAINING" "$(grep 'compile .*bench_ingest_corpora.cpp' "$TRAIN_LOG")"
contains "stack training ignores an inherited corpus" "corpus bench_stack_instrumented stream=unset" "$output"
contains "corpora training ignores an inherited corpus" "corpus bench_corpora_instrumented stream=unset" "$output"
output="$(UBQ_TEST_TRAIN_MODE=adaptive trainOf linux clang)"
contains "training refuses an adaptive stack driver" "exit 1" "$output"
contains "training refusal names the fixed-count contract" "did not run with fixed training input counts" "$output"
absent "adaptive driver counters are not merged" "merge -output" "$(cat "$TRAIN_LOG")"
output="$(trainOf apple clang)"
contains "Apple training succeeds" "exit 0" "$output"
contains "Apple training keeps the xcrun-selected tool" "apple-profdata merge -output $FIXTURE/private/apple.profdata" "$output"
check "private training leaves the shipping profile intact" "shipping profile" "$(cat "$TRAIN_TREE/Support/pgo/portable-core.profdata")"
output="$(trainOf linux gcc)"
contains "non-LLVM training is refused" "exit 1" "$output"
contains "compiler refusal names the remedy" "set CXX=clang++" "$output"
absent "compiler refusal happens before the archive build" "archive" "$(cat "$TRAIN_LOG")"

# Select using target macros even when this fixture runs on a different host.
# shellcheck source=Scripts/pgo-flags.sh
source "$ROOT/Scripts/pgo-flags.sh"
echo
echo "target profile selection"
cat >"$STUBS/streaming-cxx" <<'COMPILER'
#!/usr/bin/env python3
import sys
sys.stdout.write("#define __clang__ 1\n" + "#define OTHER_MACRO 1\n" * 16384)
COMPILER
chmod +x "$STUBS/streaming-cxx"
ubiquitty_compiler_reads_llvm_profile "$STUBS/streaming-cxx"
check "Clang detection consumes a full macro stream under pipefail" "0" "$?"
for target in apple apple-x86_64 unknown; do
    selected="$(UBQ_TEST_TRAIN_TARGET="$target" ubiquitty_default_pgo_profile "$TRAIN_TREE" "$STUBS/training-cxx")"
    check "$target keeps the portable profile" "$TRAIN_TREE/Support/pgo/portable-core.profdata" "$selected"
done
selected="$(UBQ_TEST_TRAIN_TARGET=linux-arm64 ubiquitty_default_pgo_profile "$TRAIN_TREE" "$STUBS/training-cxx")"
check "Linux ARM selects its release profile" "$TRAIN_TREE/Support/pgo/linux-aarch64-core.profdata" "$selected"
selected="$(UBQ_TEST_TRAIN_TARGET=linux-x86_64 ubiquitty_default_pgo_profile "$TRAIN_TREE" "$STUBS/training-cxx")"
check "Linux x86 selects its profile" "$TRAIN_TREE/Support/pgo/linux-x86_64-core.profdata" "$selected"
selected="$(UBQ_PGO_PROFILE="$FIXTURE/private/explicit.profdata" UBQ_TEST_TRAIN_TARGET=linux-x86_64 \
    ubiquitty_pgo_profile "$TRAIN_TREE" "$STUBS/training-cxx")"
check "a private consumer profile overrides the target default" "$FIXTURE/private/explicit.profdata" "$selected"
selected="$(UBIQUITTY_PGO=OFF UBQ_PGO_PROFILE="$FIXTURE/private/explicit.profdata" \
    ubiquitty_pgo_flags "$TRAIN_TREE" "$STUBS/stand-in-cxx" 2>/dev/null)"
check "OFF adds no profile flags even with an override" "" "$selected"

# Run the actual CMake default block, from the module every build of Core
# includes, without configuring or compiling a project. This proves the same
# target selection, read from Core's checkout rather than the including
# project's, and that a cached override is retained.
python3 - "$ROOT/Scripts/cmake/UbiquittyOptimize.cmake" "$FIXTURE/profile.cmake" <<'PY'
import sys
from pathlib import Path
text = Path(sys.argv[1]).read_text()
start = text.index("    if(NOT DEFINED UBIQUITTY_PGO_PROFILE)")
# The block ends at its own endif; what follows it is configuration-specific.
closing = "\n    endif()\n"
end = text.index(closing, start) + len(closing)
Path(sys.argv[2]).write_text('set(CMAKE_SOURCE_DIR "${UBQ_TEST_ROOT}/parent")\n'
                           + 'set(UBIQUITTY_CORE_SOURCE_DIR "${UBQ_TEST_ROOT}")\n' + text[start:end]
                           + '\nmessage("selected=${UBIQUITTY_PGO_PROFILE}")\n')
PY
for target in Linux:x86_64 Linux:aarch64 Darwin:arm64 Darwin:x86_64; do
    system="${target%:*}"
    processor="${target#*:}"
    expected="$(python3 "$ROOT/Scripts/pgo_localize.py" --default-profile "$TRAIN_TREE" "$system" "$processor")"
    selected="$(cmake -DUBQ_TEST_ROOT="$TRAIN_TREE" -DCMAKE_SYSTEM_NAME="$system" \
        -DCMAKE_SYSTEM_PROCESSOR="$processor" -P "$FIXTURE/profile.cmake" 2>&1)"
    contains "CMake $target selects the shared default" "selected=$expected" "$selected"
done
selected="$(cmake -DUBQ_TEST_ROOT="$TRAIN_TREE" -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=x86_64 \
    -DUBIQUITTY_PGO_PROFILE="$FIXTURE/private/explicit.profdata" -P "$FIXTURE/profile.cmake" 2>&1)"
contains "CMake preserves an explicit profile override" "selected=$FIXTURE/private/explicit.profdata" "$selected"

# A host with two LLVMs can name the older llvm-profdata unversioned, and an
# older one refuses a newer profile's format. Rooting the profile tries each
# llvm-profdata until one reads it, rather than giving up at the first.
output="$(python3 - "$ROOT/Scripts" "$FIXTURE" <<'PY' 2>&1
import os, subprocess, sys
from unittest import mock
sys.dont_write_bytecode = True
sys.path.insert(0, sys.argv[1])
import pgo_localize
tried = []
def derive(tool, profile, root, cache, target):
    tried.append(os.path.basename(tool[0]))
    if tool[0].endswith("/llvm-profdata"):
        raise subprocess.CalledProcessError(1, tool, stderr=b"unsupported instrumentation profile format version")
    return target
profile = os.path.join(sys.argv[2], "two-llvms.profdata")
open(profile, "wb").write(b"profile")
candidates = [["/older/llvm-profdata"], ["/newer/llvm-profdata-22"]]
with mock.patch.object(pgo_localize, "llvm_profdata_candidates", return_value=candidates), \
        mock.patch.object(pgo_localize, "derive", side_effect=derive):
    rooted = pgo_localize.localized_profile(profile, "/consumer", os.path.join(sys.argv[2], "rooted"))
print("tried=" + ",".join(tried), "rooted=" + ("copy" if rooted != profile else "committed"))
PY
)"
check "rooting falls through to an llvm-profdata that reads the profile" \
    "tried=llvm-profdata,llvm-profdata-22 rooted=copy" "$output"

# A configure that cannot apply the profile goes on under ON and stops under
# REQUIRE, for a compiler that does not read it and for an absent profile alike.
# The block runs as a script, so no compiler is probed and no project configured.
python3 - "$ROOT/Scripts/cmake/UbiquittyOptimize.cmake" "$FIXTURE/require.cmake" <<'PY'
import sys
from pathlib import Path
text = Path(sys.argv[1]).read_text()
start = text.index('    if(NOT UBIQUITTY_PGO STREQUAL "OFF" AND NOT _ubiquitty_pgo_caller_flags')
end = text.index("\n    endif()\nendif()", start) + len("\n    endif()")
Path(sys.argv[2]).write_text(text[start:end] + '\nmessage("configured")\n')
PY
: >"$FIXTURE/present.profdata"
for unapplied in "GNU:$FIXTURE/present.profdata" "Clang:$FIXTURE/absent.profdata"; do
    compiler="${unapplied%%:*}"
    profile="${unapplied#*:}"
    for pgo in ON REQUIRE; do
        output="$(cmake -DCMAKE_CXX_COMPILER_ID="$compiler" -DUBIQUITTY_PGO="$pgo" \
            -DUBIQUITTY_PGO_PROFILE="$profile" -P "$FIXTURE/require.cmake" 2>&1)"
        if [ "$pgo" = ON ]; then
            contains "ON configures without the profile ($compiler, $(basename "$profile"))" "configured" "$output"
        else
            contains "REQUIRE refuses a configure without the profile ($compiler, $(basename "$profile"))" \
                "UBIQUITTY_PGO=REQUIRE, and this build would have no profile" "$output"
            absent "REQUIRE stops before the configure finishes ($compiler)" "configured" "$output"
        fi
    done
done

# Missing fixture artifacts expose the chosen path before any profile reader
# runs, so this stays independent of the LLVM tools installed on the host.
SYMBOL_TREE="$FIXTURE/symbols"
mkdir -p "$SYMBOL_TREE/Scripts"
cp "$ROOT/Scripts/pgo-symbol-mass.sh" "$ROOT/Scripts/pgo-flags.sh" \
    "$ROOT/Scripts/pgo_localize.py" "$SYMBOL_TREE/Scripts/"
output="$(UBQ_TEST_TRAIN_TARGET=linux-x86_64 CXX="$STUBS/training-cxx" \
    bash "$SYMBOL_TREE/Scripts/pgo-symbol-mass.sh" CellGrid 2>&1)"
contains "symbol mass follows the Linux target profile" \
    "no profile at $SYMBOL_TREE/Support/pgo/linux-x86_64-core.profdata" "$output"
output="$(UBQ_PGO_PROFILE="$FIXTURE/shared-override.profdata" CXX="$STUBS/training-cxx" \
    bash "$SYMBOL_TREE/Scripts/pgo-symbol-mass.sh" CellGrid 2>&1)"
contains "symbol mass accepts the shared consumer override" "no profile at $FIXTURE/shared-override.profdata" "$output"
output="$(UBIQUITTY_PROFILE="$FIXTURE/symbol-override.profdata" UBQ_PGO_PROFILE="$FIXTURE/shared-override.profdata" \
    CXX="$STUBS/training-cxx" bash "$SYMBOL_TREE/Scripts/pgo-symbol-mass.sh" CellGrid 2>&1)"
contains "symbol mass preserves its explicit override precedence" "no profile at $FIXTURE/symbol-override.profdata" "$output"

output="$(UBQ_PGO_PROFILE="$FIXTURE/consumer-only.profdata" trainOf linux clang default)"
contains "native Linux training succeeds" "exit 0" "$output"
check "native Linux training writes its selected profile" "private profile" "$(cat "$TRAIN_TREE/Support/pgo/linux-x86_64-core.profdata")"
check "native Linux training preserves the Apple profile" "shipping profile" "$(cat "$TRAIN_TREE/Support/pgo/portable-core.profdata")"
check "a consumer override cannot redirect training output" "absent" "$(test -e "$FIXTURE/consumer-only.profdata" && echo present || echo absent)"
printf 'linux profile\n' >"$TRAIN_TREE/Support/pgo/linux-x86_64-core.profdata"
output="$(trainOf apple clang default)"
contains "native Apple training succeeds" "exit 0" "$output"
check "native Apple training writes the portable profile" "private profile" "$(cat "$TRAIN_TREE/Support/pgo/portable-core.profdata")"
check "native Apple training preserves the Linux profile" "linux profile" "$(cat "$TRAIN_TREE/Support/pgo/linux-x86_64-core.profdata")"

for benchmark in build-perf-bench.sh run-bench.sh; do
    output="$(env -u UBIQUITTY_PGO CXX="$STUBS/training-cxx" UBQ_TEST_TRAIN_COMPILER=gcc \
        UBQ_PGO_PROFILE="$FIXTURE/present.profdata" UBQ_BUILD_ROOT="$FIXTURE/perf" \
        bash "$ROOT/Scripts/$benchmark" 2>&1)"
    check "$benchmark refuses an implicitly unprofiled build" "1" "$?"
    contains "$benchmark names its required profile" "UBIQUITTY_PGO=REQUIRE" "$output"
done

python3 "$ROOT/Scripts/test-pgo-toolchain.py"
check "the Linux profile's toolchain provenance tests pass" "0" "$?"

python3 "$ROOT/Scripts/test-portable-core-build.py"
check "portable archive workers preserve failure and dependency checks" "0" "$?"

report "test-build-scripts-pgo" "builders forward PGO, select profiles by target, preserve explicit overrides and private training outputs, hold a Linux profile to its image, and report quiet failures"
