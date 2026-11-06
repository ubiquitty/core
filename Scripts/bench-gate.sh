#!/bin/bash
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

# bench-gate.sh — collect the portable benchmarks and compare them against the
# external per-platform baseline, gating on regressions. The whole perf loop on
# one machine (no CI): run-bench.sh collects, this stores/compares, compare-bench.py
# scores. Run it on each platform you care about; each keeps its own baseline file
# (numbers are machine-specific, so a Linux gate compares against the Linux baseline,
# a macOS gate against the macOS one).
#
#   ./Scripts/bench-gate.sh            run + compare against the baseline, GATE on regress
#   ./Scripts/bench-gate.sh --report  run + compare, print the table but never fail
#   ./Scripts/bench-gate.sh --update  run + overwrite the baseline with this run
#   ./Scripts/bench-gate.sh --update-best-of N   run N times, keep each lane's
#     best. Load only ever makes a lane look slower, so the best of several
#     rounds is the least contaminated estimate this machine can give -- which
#     is what a baseline wants, since a slow capture writes the load into the
#     gate and silently loosens every lane it touched.
#
# Tunables (env):
#   UBIQUITTY_PERF_BASELINE_DIR  external directory holding this machine's records
#   BENCH_THRESHOLD   regression threshold in percent (default 12 — a dev box is
#                     noisier than a pinned CI runner; widen if your machine swings)
#   BENCH_EXCLUDE     comma-separated benches excluded from the RELATIVE gate
#                     (default channel_loopback — I/O-bound, swings past any sane
#                     threshold; flood_frame_tail_max_milliseconds — the lone worst
#                     frame is too high-variance to gate relatively, held to the
#                     absolute ceiling below instead; lock_wait_under_flood_p99/max
#                     — only a handful of samples fit in the flood window, so p99
#                     and max both land on essentially the same one or two draws;
#                     T7's motivating problem, not yet a ceiling — a real fix
#                     changes the ARCHITECTURE, not something to gate pre-redesign)
#   BENCH_CEILING     comma-separated name:milliseconds absolute budgets (the Fast-
#                     pillar locked budgets: our-slice latency p50 <= half a 60Hz
#                     refresh, p99 <= one refresh; frame-tail worst <= two refreshes).
#                     Gates independent of the baseline, and applies even to
#                     BENCH_EXCLUDE'd benches.
#
# Every gate/report run also prints a non-gating staleness warning (baseline
# age vs HEAD, benchmark-set churn, portable-core-sources.txt drift) when
# the committed baseline looks too old
# or too structurally divergent to trust — regenerate with --update once the
# new numbers are themselves confirmed on a quiet machine, don't silently
# keep gating against it.
#
# Exit 0 = no gated regression (or --report/--update); 1 = a gated benchmark
# regressed; 2 = usage / missing baseline.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
MODE="gate"
# One round unless asked for more. A baseline is an absolute reference, and a
# machine under load produces a slow run, never a fast one — so across rounds
# each lane's best is the one least contaminated by whatever else was running.
ROUNDS=1
case "${1:-}" in
    --report) MODE="report" ;;
    --update) MODE="update" ;;
    --update-best-of) MODE="update"; ROUNDS="${2:-}" ; shift ;;
    --help|-h)
        sed -n '/^#!/d; /^[^#]/q; s/^# \{0,1\}//; p' "$0"; exit 0 ;;
    "") ;;
    *) echo "bench-gate.sh: unknown argument: $1" >&2; exit 2 ;;
esac

PLATFORM="$(uname -s)-$(uname -m)"
. "$ROOT/Scripts/perf-baseline-path.sh"
BASELINE_DIR="$(ubiquitty_perf_baseline_directory)"
BASELINE="$BASELINE_DIR/bench-$PLATFORM.json"
THRESHOLD="${BENCH_THRESHOLD:-12}"
# flood_frame_tail_p99 joins the max variant in the exclude: on a quiet dev
# box the value is so small that the relative gate flips on pure run-to-run
# jitter. Both tail lanes are held to absolute ceilings instead.
EXCLUDE="${BENCH_EXCLUDE:-channel_loopback,flood_frame_tail_p99_milliseconds,flood_frame_tail_max_milliseconds,lock_wait_under_flood_p99_milliseconds,lock_wait_under_flood_max_milliseconds}"
# Locked Fast-pillar budgets (60Hz refresh = 16.667ms): our-slice p50 <= 8.3,
# p99 <= 16.7; frame-tail p99 <= one refresh, worst <= 33.3. Held in absolute
# terms, not vs baseline.
CEILING="${BENCH_CEILING:-line_latency_p50_milliseconds:8.3,line_latency_p99_milliseconds:16.7,flood_frame_tail_p99_milliseconds:16.7,flood_frame_tail_max_milliseconds:33.3}"

if [ "$MODE" = "update" ] && [ -z "${UBQ_BASELINE_REASON:-}" ]; then
    echo "bench-gate: --update wants UBQ_BASELINE_REASON=\"why these numbers moved\"" >&2
    echo "  This baseline is absolute and machine-specific. Without the cause a later" >&2
    echo "  reader cannot tell a real regression from a refresh that buried one, which" >&2
    echo "  is why every refresh must identify its cause." >&2
    echo "  Asked before the battery runs, so a refusal costs a second rather than a minute." >&2
    exit 2
fi

if [ "$MODE" != "update" ] && [ ! -f "$BASELINE" ]; then
    echo "bench-gate: no baseline for $PLATFORM at $BASELINE" >&2
    echo "  capture one first: UBQ_BASELINE_REASON=... ./Scripts/bench-gate.sh --update" >&2
    exit 2
fi

CURRENT="$(mktemp -t bench-current.XXXXXX.json)"
trap 'rm -f "$CURRENT"' EXIT

# Collect this machine's current numbers.
if [ "$ROUNDS" -gt 1 ]; then
    ROUND_FILES=""
    for round in $(seq 1 "$ROUNDS"); do
        ROUND_FILE="$(mktemp -t bench-round.XXXXXX.json)"
        echo "bench-gate: round $round of $ROUNDS" >&2
        "$ROOT/Scripts/run-bench.sh" "$ROUND_FILE" >/dev/null
        ROUND_FILES="$ROUND_FILES $ROUND_FILE"
    done
    # shellcheck disable=SC2086
    python3 "$ROOT/Scripts/merge-bench-rounds.py" --out "$CURRENT" $ROUND_FILES
    # shellcheck disable=SC2086
    rm -f $ROUND_FILES
else
    "$ROOT/Scripts/run-bench.sh" "$CURRENT" >/dev/null
fi

if [ "$MODE" = "update" ]; then
    mkdir -p "$BASELINE_DIR"
    # A copy keeps only what run-bench.sh emits, and the two blocks it does not
    # emit are the ones that make an absolute baseline readable later: which
    # machine produced it, and why the numbers moved. The numbers survive a
    # refresh and the reason does not, which is how a file comes to record a
    # large gap with nothing saying whether that was a win or a broken capture.
    #
    # Carrying the previous provenance forward is worse than dropping it — it
    # would date this capture to the last one — so hardware carries forward,
    # provenance is written from what this run can observe, and the cause is the
    # caller's to name because nothing here can infer it.
    python3 "$ROOT/Scripts/stamp-bench-baseline.py" \
        --current "$CURRENT" --baseline "$BASELINE" --root "$ROOT" --reason "$UBQ_BASELINE_REASON"
    echo "bench-gate: baseline updated → $BASELINE"
    echo "  (retain it outside the product checkout; it is this machine's reference)"
    exit 0
fi

if [ ! -f "$BASELINE" ]; then
    echo "bench-gate: no baseline for $PLATFORM at $BASELINE" >&2
    echo "  capture one first:  ./Scripts/bench-gate.sh --update" >&2
    exit 2
fi

GATE_FLAG=()
[ "$MODE" = "gate" ] && GATE_FLAG=(--gate)

# macOS ships bash 3.2, where expanding an EMPTY array under `set -u` is an
# unbound-variable error. GATE_FLAG is empty in --report mode, so the plain
# "${GATE_FLAG[@]}" killed exactly the mode that must never fail.
"$ROOT/Scripts/compare-bench.py" "$BASELINE" "$CURRENT" \
    --threshold "$THRESHOLD" --exclude "$EXCLUDE" --ceiling "$CEILING" \
    ${GATE_FLAG[@]+"${GATE_FLAG[@]}"}
