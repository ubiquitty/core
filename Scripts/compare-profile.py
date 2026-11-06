#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

# compare-profile.py — diff two profile-results.json files from the latency
# profiler and report how each operation's distribution moved. Latency, so lower is
# BETTER: a regression is current > baseline by more than --threshold percent.
#
#   compare-profile.py BASELINE.json CURRENT.json [--metric p95] [--threshold PCT] [--gate]
#
# Shows p50/p95/p99 for every op (baseline → current, % delta on the chosen
# --metric). With --gate, exits non-zero if any op regressed past the threshold on
# that metric. Ops present on only one side are listed (new/removed) but never gate.
# The tail (p95/p99) is what's felt as a stall, so the gate keys on p95 by default.

import argparse
import json
import sys

METRICS = ("p50", "p90", "p95", "p99", "max", "mean", "stddev")


def load(path):
    with open(path) as f:
        data = json.load(f)
    by_name = {p["name"]: p for p in data.get("profiles", [])}
    return data, by_name


def main():
    parser = argparse.ArgumentParser(description="Diff two profile-results.json files (latency, lower=better).")
    parser.add_argument("baseline")
    parser.add_argument("current")
    parser.add_argument("--metric", default="p95", choices=METRICS,
                        help="distribution metric the gate keys on (default p95)")
    parser.add_argument("--threshold", type=float, default=12.0,
                        help="regression threshold in percent (default 12)")
    parser.add_argument("--gate", action="store_true",
                        help="exit non-zero if any op regressed past the threshold on --metric")
    args = parser.parse_args()

    base_meta, base = load(args.baseline)
    cur_meta, cur = load(args.current)
    metric = args.metric

    names = sorted(set(base) | set(cur))
    rows = []
    regressed = []
    for name in names:
        b = base.get(name)
        c = cur.get(name)
        if b is None:
            rows.append((name, "—", "—", "—", "new", ""))
            continue
        if c is None:
            rows.append((name, f'{b["p50"]:.2f}', f'{b["p95"]:.2f}', f'{b["p99"]:.2f}', "removed", ""))
            continue
        bm, cm = b[metric], c[metric]
        delta = (cm - bm) / bm * 100.0 if bm else 0.0
        flag = ""
        if delta > args.threshold:          # higher latency = worse
            flag = "🔻 slower"
            regressed.append((name, delta))
        elif delta < -args.threshold:
            flag = "🔺 faster"
        rows.append((name, f'{c["p50"]:.2f}', f'{c["p95"]:.2f}', f'{c["p99"]:.2f}', f"{delta:+.1f}%", flag))

    plat = cur_meta.get("platform", "?")
    unit = cur_meta.get("unit", "us")
    print(f"### Profiles ({plat}) — {unit}/op, lower is better")
    print(f"baseline `{base_meta.get('commit','?')}` → current `{cur_meta.get('commit','?')}` "
          f"· gate on {metric}, threshold {args.threshold:g}%\n")
    print(f"| op | p50 | p95 | p99 | Δ{metric} | |")
    print("|---|---:|---:|---:|---:|---|")
    for name, p50, p95, p99, d, flag in rows:
        print(f"| {name} | {p50} | {p95} | {p99} | {d} | {flag} |")

    if regressed:
        print(f"\n**{len(regressed)} regression(s) on {metric}:** " +
              ", ".join(f"`{n}` ({d:+.1f}%)" for n, d in regressed))

    if args.gate and regressed:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
