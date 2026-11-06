#!/bin/bash
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

# profile-gate.sh — collect the portable latency profiles and compare them against
# an external per-platform baseline, gating on a TAIL regression. The profiling
# counterpart of bench-gate.sh: run-profile.sh collects, this stores/compares,
# compare-profile.py scores (latency, lower=better). One baseline per platform.
#
#   ./Scripts/profile-gate.sh            run + compare against the baseline, GATE on regress
#   ./Scripts/profile-gate.sh --report   run + compare, print the table but never fail
#   ./Scripts/profile-gate.sh --update   run + overwrite the baseline with this run
#
# Hand-run by design, not wired into any gate tier: a latency baseline is
# per-machine — a shared box measures whatever else is running — so the
# baseline lives outside the tree, generated locally at retrain time, and
# this script is the instrument a PGO retrain session runs on a quiet
# machine rather than a push gate.
#
# Tunables (env):
#   UBIQUITTY_PERF_BASELINE_DIR  external directory holding this machine's records
#   PROFILE_METRIC      distribution metric the gate keys on (default p95 — the tail
#                       is what's felt; p99 swings more on a dev box)
#   PROFILE_THRESHOLD   regression threshold in percent (default 15 — latency tails
#                       on a dev box swing more than throughput medians)
#
# Exit 0 = no gated regression (or --report/--update); 1 = a profile regressed;
# 2 = usage / missing baseline.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
MODE="gate"
case "${1:-}" in
    --report) MODE="report" ;;
    --update) MODE="update" ;;
    --help|-h)
        sed -n '/^#!/d; /^[^#]/q; s/^# \{0,1\}//; p' "$0"; exit 0 ;;
    "") ;;
    *) echo "profile-gate.sh: unknown argument: $1" >&2; exit 2 ;;
esac

PLATFORM="$(uname -s)-$(uname -m)"
. "$ROOT/Scripts/perf-baseline-path.sh"
BASELINE_DIR="$(ubiquitty_perf_baseline_directory)"
BASELINE="$BASELINE_DIR/profile-$PLATFORM.json"
METRIC="${PROFILE_METRIC:-p95}"
THRESHOLD="${PROFILE_THRESHOLD:-15}"

if [ "$MODE" != "update" ] && [ ! -f "$BASELINE" ]; then
    echo "profile-gate: no baseline for $PLATFORM at $BASELINE" >&2
    echo "  capture one first: ./Scripts/profile-gate.sh --update" >&2
    exit 2
fi

CURRENT="$(mktemp -t profile-current.XXXXXX.json)"
trap 'rm -f "$CURRENT"' EXIT

# Collect this machine's current distributions (table on stderr, JSON to the file).
"$ROOT/Scripts/run-profile.sh" "$CURRENT" >/dev/null

if [ "$MODE" = "update" ]; then
    mkdir -p "$BASELINE_DIR"
    cp "$CURRENT" "$BASELINE"
    echo "profile-gate: baseline updated → $BASELINE"
    echo "  (retain it outside the product checkout; it is this machine's reference)"
    exit 0
fi

if [ ! -f "$BASELINE" ]; then
    echo "profile-gate: no baseline for $PLATFORM at $BASELINE" >&2
    echo "  capture one first:  ./Scripts/profile-gate.sh --update" >&2
    exit 2
fi

GATE_FLAG=()
[ "$MODE" = "gate" ] && GATE_FLAG=(--gate)

# macOS ships bash 3.2, where expanding an EMPTY array under `set -u` is an
# unbound-variable error. GATE_FLAG is empty in --report mode, so a plain
# "${GATE_FLAG[@]}" kills exactly the mode that must never fail.
"$ROOT/Scripts/compare-profile.py" "$BASELINE" "$CURRENT" \
    --metric "$METRIC" --threshold "$THRESHOLD" ${GATE_FLAG[@]+"${GATE_FLAG[@]}"}
