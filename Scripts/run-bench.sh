#!/bin/bash
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

# run-bench.sh — build + run the portable performance benchmarks and emit one
# JSON results object for the automated cross-platform perf harness.
#
#   ./Scripts/run-bench.sh [out.json]
#
# Compiles Tests/Performance/bench_stack.cpp against the portable Core (release, -O2)
# and runs it, stamping the platform + commit. Each CI runner (Linux, macOS, …)
# produces the same JSON shape, so runs key by commit for the trend and the
# regression comparison. Portable: no GUI, no platform frontend.
#
# READING A DELTA. Lanes here do not share a noise floor, and judging one
# against another's is how a good run gets discarded and a bad one believed.
# parse_plain is the noisier: it is the throughput lane, and it never
# accumulates a parameter, which makes it the control for anything touching
# parameter parsing rather than a second signal. parse_sgr drives the CSI
# parameter accumulate.
#
# The spread is a property of the method, not the lane. Interleaving the arms
# and taking best-of-2 suppresses drift that sequential rounds absorb into one
# arm. So quote a spread with the method that produced it; a threshold carried
# across methods rejects real results.
#
# A control that moves while the target does not is load landing unevenly across
# the run rather than an effect. Discard it and re-run on a quiet box; do not
# reason about the size of the move.
set -euo pipefail
export UBIQUITTY_PGO="${UBIQUITTY_PGO:-REQUIRE}"

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# Build output lives outside the checkout; see Scripts/build-root.sh.
# shellcheck source=Scripts/build-root.sh
. "$ROOT/Scripts/build-root.sh"
# `--rounds N` measures N times and keeps each lane's best, which is what a
# contended box needs: see the block above the measured pass.
ROUNDS=1
POSITIONAL=()
while [ "$#" -gt 0 ]; do
    case "$1" in
        --rounds) ROUNDS="${2:?--rounds needs a count}"; shift 2 ;;
        --rounds=*) ROUNDS="${1#--rounds=}"; shift ;;
        *) POSITIONAL+=("$1"); shift ;;
    esac
done
case "$ROUNDS" in
    ''|*[!0-9]*|0) echo "run-bench.sh: --rounds takes a positive integer, got '$ROUNDS'" >&2; exit 2 ;;
esac
OUT="$ROOT/bench-results.json"
[ "${#POSITIONAL[@]}" -gt 0 ] && OUT="${POSITIONAL[0]}"
CXX="${CXX:-c++}"
# Launcher plus compiler, unquoted at the call so an absent launcher adds no
# argument. A bare "$CXX" would compile fine and skip ccache without saying so.
CXX_LAUNCHED="${UBQ_COMPILER_LAUNCHER:+$UBQ_COMPILER_LAUNCHER }$CXX"
BUILD="$UBQ_BUILD_ROOT/BuildBench"
mkdir -p "$BUILD"

PLATFORM="$(uname -s)-$(uname -m)"
COMMIT="$(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo unknown)"

# The portable Terminal + parse + Sync snapshot path. The link set is DERIVED from
# the one canonical libUbiquittyCore source list (Scripts/portable-core-sources.txt)
# — built into a static archive so only the TUs bench_stack references are pulled.
# This is the single source CMake's `ubiquitty-core` target also reads, so the
# bench link set can never drift behind a file split again.
# shellcheck source=Scripts/portable-core-lib.sh
source "$ROOT/Scripts/portable-core-lib.sh"

# Flags must match the shipping app (-O3 + ThinLTO, as the app's build and the
# CMake IPO block set them): a bench built at a different inline threshold ranks
# call-overhead changes differently than production runs them. That parity
# includes PGO on Core: the measured archive takes the same profile, or the
# bench would rank inline/layout decisions against codegen production does not
# ship. The non-shipping workload controller stays unprofiled in both arms.
# UBIQUITTY_PGO=OFF disables both sides identically (the A/B escape hatch).
SHIPPING_FLAGS="-O3 $(ubiquitty_lto_flag) -DNDEBUG"
# The measured Core uses shipping PGO; the workload controller must have the
# same compile inputs regardless of which Core profile is being measured.
DRIVER_FLAGS="$SHIPPING_FLAGS"
# UBQ_PGO_PROFILE points the build at a profile somewhere else, which is what an
# A/B of two profiles needs. Without it the only way to compare them is to copy
# each arm over the checked-in file between rounds — and that file is tracked
# and shared, so for the length of the run every other checkout's build, and any
# gate reading it, sees whichever arm happens to be in place. A measurement
# harness that mutates a tracked artefact is a race against the rest of the
# team, and nothing announces it.
# shellcheck source=Scripts/pgo-flags.sh
source "$ROOT/Scripts/pgo-flags.sh"
PGO_PROFILE="$(ubiquitty_pgo_profile "$ROOT" "$CXX")"
PGO_FLAGS="$(ubiquitty_pgo_flags "$ROOT" "$CXX")"
if [ -n "$PGO_FLAGS" ]; then
    SHIPPING_FLAGS="$SHIPPING_FLAGS $PGO_FLAGS"
    # shellcheck source=Scripts/pgo-staleness.sh
    source "$ROOT/Scripts/pgo-staleness.sh"
    pgo_staleness_warning "$ROOT" "${PGO_PROFILE#"$ROOT"/}"
elif [ -f "$PGO_PROFILE" ] && [ "${UBIQUITTY_PGO:-ON}" != "OFF" ] \
        && ! ubiquitty_compiler_reads_llvm_profile "$CXX"; then
    # The bench links the same portable-Core archive the CMake build does, over
    # the same vendored wuffs; the IPO block in
    # Scripts/cmake/UbiquittyOptimize.cmake carries the reason GCC's -Wodr says
    # nothing actionable about it.
    SHIPPING_FLAGS="$SHIPPING_FLAGS -Wno-odr"
    DRIVER_FLAGS="$DRIVER_FLAGS -Wno-odr"
fi
echo "Building libUbiquittyCore archive + bench_stack ($CXX, $SHIPPING_FLAGS) ..." >&2
build_portable_core_archive "$BUILD" "$SHIPPING_FLAGS"
ubiquitty_core_pal_stub_source "$BUILD/bench_pal_stub.cpp"
BENCH_INCLUDES=()
while IFS= read -r bench_include_flag; do
    BENCH_INCLUDES+=("$bench_include_flag")
done < <(ubiquitty_bench_include_flags "$ROOT")

# shellcheck disable=SC2086
$CXX_LAUNCHED -std=c++23 $DRIVER_FLAGS -fno-exceptions -fno-rtti -pthread \
    ${BENCH_INCLUDES[@]+"${BENCH_INCLUDES[@]}"} \
    -c "$ROOT/Tests/Performance/bench_stack.cpp" -o "$BUILD/bench_stack-driver.o"
# shellcheck disable=SC2086
$CXX_LAUNCHED -std=c++23 $SHIPPING_FLAGS -fno-exceptions -fno-rtti -pthread \
    ${BENCH_INCLUDES[@]+"${BENCH_INCLUDES[@]}"} \
    "$BUILD/bench_stack-driver.o" "$BUILD/bench_pal_stub.cpp" \
    "$PORTABLE_CORE_ARCHIVE" -o "$BUILD/bench_stack"

# One discarded pass first, because the recorded one would otherwise be this
# binary's first execution after linking it. The lane measured first pays for
# paging the whole image in, and it pays enough to cross the gate's threshold
# on its own. A gate reporting a regression nobody introduced is one people
# learn to skip.
echo "Warming the binary (one discarded pass) ..." >&2
"$BUILD/bench_stack" --platform "$PLATFORM" --commit "$COMMIT" > /dev/null 2>&1 || true

# One round is one sample of a shared machine, and on a CI box that sample can
# move between runs of equivalent trees by more than any threshold survives, so
# a single round cannot gate — it either passes everything or fails on load.
#
# Contention only ever moves a lane the wrong way, so the best of several rounds
# is the estimate least contaminated by whatever else the box was doing.
# Scripts/merge-bench-rounds.py holds what "best" means, per lane, by the
# direction rule compare-bench.py owns — the same merge bench-gate.sh already
# uses to capture a baseline, so a baseline and a gated run are folded the same
# way.
if [ "$ROUNDS" -eq 1 ]; then
    echo "Running benchmarks (platform=$PLATFORM commit=$COMMIT) ..." >&2
    "$BUILD/bench_stack" --platform "$PLATFORM" --commit "$COMMIT" | tee "$OUT"
else
    ROUND_FILES=()
    for round in $(seq 1 "$ROUNDS"); do
        echo "Running benchmarks, round $round of $ROUNDS (platform=$PLATFORM commit=$COMMIT) ..." >&2
        "$BUILD/bench_stack" --platform "$PLATFORM" --commit "$COMMIT" > "$BUILD/bench-round-$round.json"
        ROUND_FILES+=("$BUILD/bench-round-$round.json")
    done
    python3 "$ROOT/Scripts/merge-bench-rounds.py" --out "$OUT" ${ROUND_FILES[@]+"${ROUND_FILES[@]}"}
    cat "$OUT"
fi
echo "wrote $OUT" >&2
