#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0
#
# ablate-sweep.py — rank the builder's phases by what removing one costs.
#
# `buildCellInstances` reads UBIQUITTY_BUILDER_ABLATE and skips a phase per bit
# (the flags are `AblateBit` in Source/Core/Rendering/CellInstanceBuilder.cpp).
# Output is wrong under any non-zero mask, so this answers "what does this phase
# cost" and never "is the frame right". The bits RANK phases; they do not
# decompose a total, because skipping a fetch also skips the geometry and the
# instance write behind it.
#
# It exists because a short hand-rolled sweep reads the machine as a result.
# Five pairs cannot separate a phase from the machine, and the sign test's
# denominator is what hides that — 5 of 5 has p=0.06 before any effect exists.
#
# So the defaults are fifteen pairs, control lanes divided out per pair, and
# no direction claimed inside the host's floor. Same instrument as
# Scripts/bench-ab.py, for the same reason — see Docs/measuring-a-core-change.md.
#
# Usage:
#   Scripts/ablate-sweep.py                       # every bit, 15 pairs
#   Scripts/ablate-sweep.py --pairs 25 --lane render_build_50x200
#   Scripts/ablate-sweep.py --bit AblateResolve   # one phase, when following a lead
#
# Most bits refuse on `render_build_full`: the lane asserts each stream it
# covers is non-empty, so ablating a fetch empties one and the bench stops.
# That refusal is reported, never silently skipped — a phase that could not be
# measured must not read like a phase that measured zero.

import argparse
import json
import math
import os
import statistics
import subprocess
import sys
from pathlib import Path

BITS = [
    ("AblateResolve", 1 << 0),
    ("AblateGlyphFetch", 1 << 2),
    ("AblateCellWrite", 1 << 3),
    ("AblateDecorations", 1 << 4),
    ("AblateClusterRead", 1 << 5),
    ("AblateMonoFetch", 1 << 6),
    ("AblateWideFetch", 1 << 7),
    ("AblateColorFetch", 1 << 8),
    ("AblateBoxFetch", 1 << 9),
]
DEFAULT_CONTROLS = ["parse_plain", "update_apply", "checkpoint_apply_50x200", "parse_sgr"]
FLOOR = 1.2  # percent; the null floor of two byte-different but identical binaries


def read_lanes(binary: Path, mask: int) -> dict[str, float] | None:
    env = dict(os.environ)
    if mask:
        env["UBIQUITTY_BUILDER_ABLATE"] = str(mask)
    else:
        env.pop("UBIQUITTY_BUILDER_ABLATE", None)
    out = subprocess.run([str(binary)], capture_output=True, text=True, env=env)
    try:
        return {b["name"]: float(b["value"]) for b in json.loads(out.stdout)["benchmarks"]}
    except Exception:
        return None


def sign_test(wins: int, n: int) -> float:
    tail = min(wins, n - wins)
    return min(1.0, sum(math.comb(n, k) for k in range(tail + 1)) / 2 ** n * 2)


def main() -> int:
    ap = argparse.ArgumentParser(description="Rank the builder's phases by ablation.")
    ap.add_argument("--bench", default="BuildBench/bench_stack")
    ap.add_argument("--lane", default="render_build_full")
    ap.add_argument("--controls", default=",".join(DEFAULT_CONTROLS))
    ap.add_argument("--pairs", type=int, default=15)
    ap.add_argument("--bit", help="measure one named phase instead of all")
    args = ap.parse_args()

    binary = Path(args.bench)
    if not binary.is_file():
        sys.exit(f"ablate-sweep: {binary} is missing.\n"
                 f"              UBIQUITTY_PGO=OFF Scripts/build-perf-bench.sh bench_stack")
    if args.pairs < 9:
        sys.exit(f"ablate-sweep: {args.pairs} pairs cannot separate a phase from this machine.\n"
                 f"              Five pairs once read +11.6% '5 of 5' for a phase that measures\n"
                 f"              -0.3%, 7 of 15. The minimum here is 9.")

    controls = [c for c in args.controls.split(",") if c]
    probe = read_lanes(binary, 0)
    if probe is None:
        sys.exit(f"ablate-sweep: {binary} printed no JSON with no mask set.")
    missing = [n for n in [args.lane, *controls] if n not in probe]
    if missing:
        sys.exit(f"ablate-sweep: {binary} emits no lane named {', '.join(missing)}.")

    bits = [(n, b) for n, b in BITS if not args.bit or n == args.bit]
    if args.bit and not bits:
        sys.exit(f"ablate-sweep: no phase named {args.bit}. Known: {', '.join(n for n, _ in BITS)}")

    print(f"lane      {args.lane}")
    print(f"controls  {', '.join(controls)}")
    print(f"pairs     {args.pairs} interleaved per phase\n")
    print(f"{'phase removed':22} {'gain':>8} {'pairs':>8}  verdict")
    print(f"{'-' * 22} {'-' * 8} {'-' * 8}  {'-' * 34}")

    for name, bit in bits:
        ratios, refused = [], False
        for _ in range(args.pairs):
            base, arm = read_lanes(binary, 0), read_lanes(binary, bit)
            if base is None or arm is None or args.lane not in arm:
                refused = True
                break
            drift = math.exp(sum(math.log(arm[c] / base[c]) for c in controls) / len(controls))
            ratios.append((arm[args.lane] / base[args.lane]) / drift)
        if refused:
            print(f"{name:22} {'—':>8} {'—':>8}  refused: the lane asserts this stream is non-empty")
            continue
        median = statistics.median(ratios)
        wins = sum(1 for r in ratios if r > 1.0)
        p = sign_test(wins, len(ratios))
        gain = (median - 1) * 100
        if abs(gain) < FLOOR:
            verdict = f"inside the ~{FLOOR}% floor"
        elif p > 0.05:
            verdict = f"not consistent (p={p:.3f})"
        else:
            verdict = f"costs {abs(gain):.1f}% (p={p:.3f})" if gain > 0 else f"removing it is SLOWER (p={p:.3f})"
        print(f"{name:22} {gain:+7.1f}% {wins:>4}/{len(ratios)}  {verdict}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
