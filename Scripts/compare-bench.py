#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

# compare-bench.py — diff two bench-results.json files from the automated
# perf harness and report per-benchmark deltas, flagging regressions.
#
#   compare-bench.py BASELINE.json CURRENT.json [--threshold PCT] [--gate]
#
# Emits a Markdown table (suitable for a PR comment) of every benchmark's baseline
# vs current value with a % delta. Metric direction is per-benchmark: higher-is-
# better for rates (MiB/s, Mcell/s), lower-is-better for sizes and latencies (unit
# `bytes` or `ms`, or an explicit `"direction": "lower"` field). A regression is a
# move in the worse direction by more than --threshold percent (default 8). With
# --gate, exits non-zero if any benchmark regressed (otherwise always exits 0).
#
# A benchmark present on one side only (added / removed) is listed and never gates.

import argparse
import json
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent


def load(path):
    with open(path) as f:
        data = json.load(f)
    by_name = {b["name"]: b for b in data.get("benchmarks", [])}
    return data, by_name


# A RATE is higher-is-better; everything else -- a duration, a size, a cost per
# unit of work -- is lower-is-better. A bench may override with an explicit
# "direction" field ("higher"/"lower").
#
# Stated as a rule about rates rather than a list of lower-is-better units,
# because a list goes stale: a unit absent from it, such as `ms/step` or
# `ns/row`, reads backwards, so a lane that slows is reported as improved while
# the gate exits clean, and a real improvement is called a regression.
#
# The default also sits on the safe side. An unrecognised unit is treated
# as lower-is-better, so a new lane whose direction nobody declared fails loudly
# on a win rather than passing silently through a loss.
#
# Suffix-matched, not substring-matched: `"/s" in "ms/step"` is true, which is
# the same mistake one layer down.
def isRateUnit(unit):
    return unit.endswith("/s") or unit == "fps"


def is_lower_better(bench):
    direction = bench.get("direction")
    if direction in ("higher", "lower"):
        return direction == "lower"
    return not isRateUnit(bench.get("unit", ""))


def _git(*args):
    return subprocess.check_output(["git", *args], cwd=REPO_ROOT, stderr=subprocess.DEVNULL, text=True).strip()


# Staleness detection — a hard-fail baseline comparison is only as trustworthy
# as the baseline itself; a committed baseline captured from an architecturally
# different tree reports false regressions. These checks are advisory: they
# never gate (a stale baseline is a human decision — regenerate it — not a build
# failure), they only make the staleness visible instead of an unenforced
# README caveat.
def staleness_warnings(base_meta, base_names, cur_names, age_threshold, drift_threshold):
    warnings = []

    base_commit = base_meta.get("commit")
    if base_commit:
        try:
            age = int(_git("rev-list", "--count", f"{base_commit}..HEAD"))
            if age > age_threshold:
                warnings.append(
                    f"baseline is {age} commits behind HEAD (threshold {age_threshold}) — "
                    f"comparisons against it may be architecturally meaningless, not just noisy"
                )
        except (subprocess.CalledProcessError, ValueError, FileNotFoundError):
            warnings.append(f"could not resolve baseline commit `{base_commit}` age vs HEAD (shallow clone? rewritten history?)")
    else:
        warnings.append("baseline has no recorded commit — cannot check age")

    union = base_names | cur_names
    if union:
        churn = len(base_names ^ cur_names) / len(union)
        if churn > drift_threshold:
            added = sorted(cur_names - base_names)
            removed = sorted(base_names - cur_names)
            warnings.append(
                f"benchmark set churned {churn:.0%} vs baseline (threshold {drift_threshold:.0%}) — "
                f"surviving names may no longer measure comparable operations"
                + (f"; new: {', '.join(added)}" if added else "")
                + (f"; gone: {', '.join(removed)}" if removed else "")
            )

    if base_commit:
        try:
            base_sources = _git("show", f"{base_commit}:Scripts/portable-core-sources.txt").splitlines()
            with open(REPO_ROOT / "Scripts/portable-core-sources.txt") as f:
                cur_sources = f.read().splitlines()
            base_n, cur_n = len(base_sources), len(cur_sources)
            if base_n and abs(cur_n - base_n) / base_n > drift_threshold:
                warnings.append(
                    f"portable-core-sources.txt file count drifted {base_n} → {cur_n} "
                    f"(>{drift_threshold:.0%}) — the binary being measured now links "
                    f"meaningfully different code than the baseline did"
                )
        except (subprocess.CalledProcessError, FileNotFoundError):
            pass  # source-list didn't exist at the baseline commit or locally — not this check's problem

    return warnings


def main():
    parser = argparse.ArgumentParser(description="Compare two bench-results.json files.")
    parser.add_argument("baseline")
    parser.add_argument("current")
    parser.add_argument("--threshold", type=float, default=12.0,
                        help="regression threshold in percent (default 12, matching "
                             "bench-gate.sh's dev-box default — a pinned CI runner "
                             "can pass a tighter value explicitly)")
    parser.add_argument("--gate", action="store_true",
                        help="exit non-zero if any benchmark regressed past the threshold")
    parser.add_argument("--exclude", default="",
                        help="comma-separated benchmark names excluded from the RELATIVE "
                             "gate (still shown in the table, flagged 'noisy'). Use for the "
                             "I/O-bound benches whose run-to-run swing exceeds the threshold. "
                             "A --ceiling still applies — an excluded bench is off the "
                             "relative gate, not the absolute one.")
    parser.add_argument("--ceiling", default="",
                        help="comma-separated name:milliseconds absolute budgets (e.g. "
                             "flood_frame_tail_max_ms:33.3). A metric whose CURRENT value "
                             "exceeds its ceiling gates regardless of the baseline delta — "
                             "the locked latency/frame-tail budgets that must hold in "
                             "absolute terms, not just relative to a drifting baseline. "
                             "Independent of --threshold and --exclude.")
    parser.add_argument("--staleness-age-threshold", type=int, default=200,
                        help="warn (never gate) when the baseline commit is more than this "
                             "many commits behind HEAD (default 200 — roughly a week at this "
                             "project's merge velocity; tune once the per-commit result store "
                             "shows real drift rates)")
    parser.add_argument("--staleness-drift-threshold", type=float, default=0.20,
                        help="warn (never gate) when the benchmark-name set or the "
                             "portable-core-sources.txt file count churns by more than this "
                             "fraction vs the baseline (default 0.20)")
    parser.add_argument("--no-staleness-check", action="store_true",
                        help="skip the staleness warnings entirely (e.g. shallow/detached "
                             "checkouts without baseline-commit history available)")
    args = parser.parse_args()
    excluded = {name.strip() for name in args.exclude.split(",") if name.strip()}
    ceilings = {}
    for item in args.ceiling.split(","):
        item = item.strip()
        if not item:
            continue
        name, _, value = item.partition(":")
        ceilings[name.strip()] = float(value)

    base_meta, base = load(args.baseline)
    cur_meta, cur = load(args.current)

    if not args.no_staleness_check:
        warnings = staleness_warnings(base_meta, set(base), set(cur),
                                       args.staleness_age_threshold, args.staleness_drift_threshold)
        if warnings:
            print("### ⚠️  Baseline staleness warning (non-gating)")
            for w in warnings:
                print(f"- {w}")
            print()

    names = sorted(set(base) | set(cur))
    rows = []
    regressed = []
    for name in names:
        b = base.get(name)
        c = cur.get(name)
        if c is None:
            rows.append((name, f'{b["value"]:.1f}', "—", "removed", ""))
            continue
        cv = c["value"]
        ceiling = ceilings.get(name)
        over_ceiling = ceiling is not None and cv > ceiling
        if b is None:
            # New metric: no baseline delta to score, but an absolute ceiling
            # still gates it.
            flag = ""
            if over_ceiling:
                flag = f"🔻 over ceiling ({cv:.3g} > {ceiling:g})"
                regressed.append((name, f"{cv:.3g} > {ceiling:g} ceiling"))
            rows.append((name, "—", f'{cv:.1f}', "new", flag))
            continue
        bv = b["value"]
        delta = (cv - bv) / bv * 100.0 if bv else 0.0
        # Normalize to "goodness": positive `effective` is a move in the better
        # direction, whichever way that is for this metric.
        effective = -delta if is_lower_better(c) else delta
        flag = ""
        if over_ceiling:
            # An absolute ceiling beats the relative gate and --exclude: a locked
            # budget must hold regardless of baseline drift or per-run noise.
            flag = f"🔻 over ceiling ({cv:.3g} > {ceiling:g})"
            regressed.append((name, f"{cv:.3g} > {ceiling:g} ceiling"))
        elif name in excluded:
            flag = "noisy (not gated)"
        elif effective < -args.threshold:
            flag = "🔻 regressed"
            regressed.append((name, f"{delta:+.1f}%"))
        elif effective > args.threshold:
            flag = "🔺 improved"
        rows.append((name, f"{bv:.1f}", f"{cv:.1f}", f"{delta:+.1f}%", flag))

    plat = cur_meta.get("platform", "?")
    print(f"### Benchmarks ({plat})")
    print(f"baseline `{base_meta.get('commit','?')}` → current `{cur_meta.get('commit','?')}` "
          f"· regression threshold {args.threshold:g}%"
          + (f" · ceilings {', '.join(f'{n}≤{v:g}' for n, v in ceilings.items())}" if ceilings else "")
          + "\n")
    print("| benchmark | baseline | current | Δ | |")
    print("|---|---:|---:|---:|---|")
    for name, bv, cv, d, flag in rows:
        print(f"| {name} | {bv} | {cv} | {d} | {flag} |")

    if regressed:
        print(f"\n**{len(regressed)} regression(s):** " +
              ", ".join(f"`{n}` ({note})" for n, note in regressed))

    # A lane the baseline has never seen prints as "new" in the table above and
    # then contributes nothing to the verdict. Silence there reads as coverage:
    # the gate says PASS while a lane it cannot compare sits beside it. Name the
    # count and the lanes, so an ungated lane is a stated gap rather than a row
    # someone has to notice.
    ungated = sorted(set(cur) - set(base))
    if ungated:
        print(f"\n**{len(ungated)} lane(s) not in the baseline, so not gated:** " +
              ", ".join(f"`{n}`" for n in ungated) +
              "\n\nRefresh with `./Scripts/bench-gate.sh --update` on an idle machine —"
              " a baseline captured under load becomes a floor nobody can hold.")

    if args.gate and regressed:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
