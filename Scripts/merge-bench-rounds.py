#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

# merge-bench-rounds.py — fold several bench-results.json rounds into one by
# keeping each lane's best value.
#
# A baseline is an absolute reference, and this machine is a shared dev box: a
# loaded run makes a lane look slower, never faster. So across rounds, a lane's
# best is its least contaminated estimate, and the merge is a per-lane max or
# min according to which way the lane's unit points.
#
# Whole-file fields (platform, commit, build, hardware) come from the first
# round, which is the run all of them describe. p95 travels with the value it
# was measured beside rather than being merged on its own: a p95 from one round
# and a value from another describe no run that happened.
#
#   ./Scripts/merge-bench-rounds.py --out merged.json round1.json round2.json ...

import argparse
import importlib.util
import json
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent


def loadCompareBench():
    """The direction rule, from the one place that owns it.

    Imported rather than restated: a second copy of "which way does this unit
    point" is how ten lanes came to be gated backwards, and a merge that
    disagreed with the gate would pick the worst round for exactly those lanes.
    """
    spec = importlib.util.spec_from_file_location("compare_bench", REPO_ROOT / "Scripts" / "compare-bench.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main():
    parser = argparse.ArgumentParser(description="Fold bench rounds into one, keeping each lane's best.")
    parser.add_argument("--out", required=True, help="path to write the merged results to")
    parser.add_argument("rounds", nargs="+", help="bench-results.json files, one per round")
    args = parser.parse_args()

    compareBench = loadCompareBench()

    merged = None
    bestForLane = {}
    for path in args.rounds:
        with open(path, encoding="utf-8") as handle:
            data = json.load(handle)
        if merged is None:
            merged = data
        for bench in data.get("benchmarks", []):
            name = bench.get("name")
            if name is None:
                continue
            previous = bestForLane.get(name)
            if previous is None:
                bestForLane[name] = bench
                continue
            # A lane absent from a round, or holding a non-positive value, is
            # not evidence about the machine — keep what the other rounds said.
            value = bench.get("value", 0)
            if not isinstance(value, (int, float)) or value <= 0:
                continue
            previousValue = previous.get("value", 0)
            if not isinstance(previousValue, (int, float)) or previousValue <= 0:
                bestForLane[name] = bench
                continue
            isBetter = value < previousValue if compareBench.is_lower_better(bench) else value > previousValue
            if isBetter:
                bestForLane[name] = bench

    if merged is None:
        print("merge-bench-rounds: no rounds given", file=sys.stderr)
        return 2

    # The first round's order, so a merged file reads like a run rather than
    # like a dictionary.
    order = [b.get("name") for b in merged.get("benchmarks", [])]
    seen = set(order)
    order.extend(name for name in bestForLane if name not in seen)
    merged["benchmarks"] = [bestForLane[name] for name in order if name in bestForLane]

    with open(args.out, "w", encoding="utf-8") as handle:
        json.dump(merged, handle, indent=2)
        handle.write("\n")
    print(f"merge-bench-rounds: {len(args.rounds)} round(s), {len(merged['benchmarks'])} lane(s), each lane's best kept",
          file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
