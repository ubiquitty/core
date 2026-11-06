#!/bin/bash
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

# build-perf-bench.sh — build the PTY-capable perf tools (bench_drain,
# bench_core) with SHIPPING flags against the portable Core (macOS).
#
#   ./Scripts/build-perf-bench.sh [tool ...]      # default: bench_drain bench_core
#
# Same recipe as run-bench.sh (portable core archive, -O3 + ThinLTO + PGO;
# UBIQUITTY_PGO=OFF disables both sides) PLUS the real PTY transport
# (PTYStream, OutputQueue, macOS PTYFacts) the no-op PAL stub does not carry —
# bench_drain forkpty's a live flood child, so it cannot run against the stub
# alone. Binaries land in BuildBench/.
#
# UBQ_PGO_PROFILE picks the profile, as it already does for run-bench.sh and
# check-bench-baseline.sh. Without it an A/B of two source arms has to swap the
# tracked artifact in and out of Support/pgo/ between builds, and a branch switch
# mid-swap carries the wrong copy across.
#
# Measuring a change to `buildCellInstances` NEEDS A PROFILE PER ARM. That
# function is profile-sensitive enough that a small structural edit orphans its
# own counters and reads slower until retrained. A single-profile A/B of a
# builder change therefore measures profile staleness and reports it as the
# change's cost, at a magnitude that swamps anything these lanes can resolve.
# Train once per arm, point this at each, and leave Support/pgo/ alone:
#
#     ./Scripts/pgo-train.sh                 # on arm A's source
#     cp Support/pgo/portable-core.profdata /tmp/A.profdata && git checkout -- Support/pgo
#     ... apply arm B ...
#     ./Scripts/pgo-train.sh
#     cp Support/pgo/portable-core.profdata /tmp/B.profdata && git checkout -- Support/pgo
#     UBQ_PGO_PROFILE=/tmp/A.profdata ./Scripts/build-perf-bench.sh bench_stack
#
# An arm whose absolute lands on the known-stale value for a lane is telling you
# the profile is the variable, not the source. An implausible magnitude is the
# cheapest diagnosis available.
#
# Once both arms are built: Scripts/profile-lane.sh says where the time goes and
# Scripts/bench-ab.py compares them against control lanes, because a raw
# interleaved A/B on this host reports drift as a result. The whole procedure is
# Docs/measuring-a-core-change.md.
set -euo pipefail
export UBIQUITTY_PGO="${UBIQUITTY_PGO:-REQUIRE}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# Build output lives outside the checkout; see Scripts/build-root.sh.
# shellcheck source=Scripts/build-root.sh
. "$ROOT/Scripts/build-root.sh"
CXX="${CXX:-c++}"
# Launcher plus compiler, unquoted at the call so an absent launcher adds no
# argument. A bare "$CXX" would compile fine and skip ccache without saying so.
CXX_LAUNCHED="${UBQ_COMPILER_LAUNCHER:+$UBQ_COMPILER_LAUNCHER }$CXX"
BUILD="$UBQ_BUILD_ROOT/BuildBench"
mkdir -p "$BUILD"
# shellcheck source=Scripts/portable-core-lib.sh
source "$ROOT/Scripts/portable-core-lib.sh"

BENCH_INCLUDES=()
while IFS= read -r bench_include_flag; do
    BENCH_INCLUDES+=("$bench_include_flag")
done < <(ubiquitty_bench_include_flags "$ROOT")

# ThinLTO is clang's spelling; g++ rejects it. The probe lives with the
# archive builder so every driver asks the same question of the same compiler.
UBQ_LTO_FLAG="$(ubiquitty_lto_flag)"
# UBQ_BENCH_DEBUG_INFO=1 adds -g and keeps the LTO backend objects, so a trace
# of this binary attributes to file:line instead of to a symbol name. Off by
# default: the objects are large and nothing but a profiler reads them.
SHIPPING_FLAGS="-O3 $UBQ_LTO_FLAG -DNDEBUG"
if [ "${UBQ_BENCH_DEBUG_INFO:-}" = "1" ]; then
    mkdir -p "$BUILD/lto-objects"
    SHIPPING_FLAGS="$SHIPPING_FLAGS -g"
    LDFLAGS_EXTRA="-Wl,-object_path_lto,$BUILD/lto-objects"
fi
# Who may be handed the profile is one decision, and it lives beside the
# profile rather than in each caller: the container's g++ compiles clean with a
# .profdata path it cannot read, so a caller that checks only whether the file
# exists builds a plain -O3 binary and reports PGO.
# shellcheck source=Scripts/pgo-flags.sh
source "$ROOT/Scripts/pgo-flags.sh"
PGO_FLAGS="$(ubiquitty_pgo_flags "$ROOT" "$CXX")"
if [ -n "$PGO_FLAGS" ]; then
    SHIPPING_FLAGS="$SHIPPING_FLAGS $PGO_FLAGS"
fi

build_portable_core_archive "$BUILD" "$SHIPPING_FLAGS"
# The real PTY transport is linked below, so the stub must not answer for it.
ubiquitty_core_pal_stub_source "$BUILD/bench_pal_stub.cpp" --with-pty-stream

TOOLS=("${@:-}")
if [ ${#TOOLS[@]} -eq 0 ] || [ -z "${TOOLS[0]}" ]; then
    TOOLS=(bench_drain bench_core bench_unicode_parse)
fi
for tool in "${TOOLS[@]}"; do
    # shellcheck disable=SC2086
    $CXX_LAUNCHED -std=c++23 $SHIPPING_FLAGS -fno-exceptions -fno-rtti -pthread \
        ${BENCH_INCLUDES[@]+"${BENCH_INCLUDES[@]}"} \
        "$ROOT/Tests/Performance/$tool.cpp" "$BUILD/bench_pal_stub.cpp" \
        "$ROOT/Source/Platform/POSIX/PTYStream.cpp" \
        "$ROOT/Source/PAL/OutputQueue.cpp" \
        "$ROOT/Source/Platform/POSIX/PTYFacts.cpp" \
        ${LDFLAGS_EXTRA:-} "$PORTABLE_CORE_ARCHIVE" -o "$BUILD/$tool"
    echo "built $BUILD/$tool"
done
