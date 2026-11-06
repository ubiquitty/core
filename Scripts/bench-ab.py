#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0
#
# bench-ab.py — an A/B between two bench binaries, normalised against lanes the
# change cannot reach.
#
# An interleaved A/B is not enough on a loaded machine: lanes with no path to
# the change move between runs, and the same subject comparison can read
# differently minutes apart.
#
# So a raw paired sign test lies here. It assumes independent pairs; a machine
# drifting through a run (thermal, a concurrent build, DVFS) biases every pair
# the same way, and a consistent direction appears with a small p-value attached
# to it.
#
# The fix is a within-run control. Each binary prints every lane in one JSON
# run, so the controls are free: divide the subject lane's ratio by the
# geometric mean of the control lanes' ratios, per pair, and the drift common to
# both cancels.
#
#     normalised = (subject_arm / subject_base) / geomean(control_arm / control_base)
#
# Usage:
#   Scripts/bench-ab.py BASE_BINARY ARM_BINARY --lane render_build_full
#   Scripts/bench-ab.py A B --lane render_build_full --pairs 21 \
#       --controls parse_plain,update_apply,checkpoint_apply_50x200,parse_sgr
#
# Both binaries must be built the same way. For a change to buildCellInstances,
# build both with UBIQUITTY_PGO=OFF or train a profile per arm — see
# Scripts/build-perf-bench.sh's header, which explains why a single profile
# measures profile staleness and reports it as the change's cost.
#
# It is the wrong instrument for a PROFILE change, and refuses one it can
# detect. The normalisation assumes the change cannot reach the control lanes.
# That holds for a code change and fails for a retrain: re-measuring the
# counters moves every lane, so dividing the subject by the controls cancels
# exactly the effect the comparison exists to isolate. Evaluate a profile
# against an independent workload instead — vtebench unicode — since
# pgo-train.sh trains on bench_stack, and a profile scored there is scored
# against its own training set.
#
# Prerequisites are refused, not skipped: a missing binary, a missing lane, an
# absent control, or two binaries with the same hash all stop the run. A control
# that is not in the output cannot cancel anything, and reporting a number
# anyway is the failure this whole tool exists to prevent.

import argparse
import json
import math
import statistics
import subprocess
import sys
from pathlib import Path

# Lanes that no renderer or parser change reaches at once — they exercise
# different subsystems, so drift they share is the machine's, not the change's.
DEFAULT_CONTROLS = ["parse_plain", "update_apply", "checkpoint_apply_50x200", "parse_sgr"]


# Half the suite reports a rate and half a duration, and dividing one arm by the
# other reports a regression as an improvement for the second half. The unit is
# in the JSON, so the direction is read rather than assumed — and it is read
# through compare-bench.py's rule rather than a list kept here, because a list
# kept here is one more copy to rot, and a rotted copy reads lanes backwards.
# One owner, everyone imports.
def loadCompareBench():
    import importlib.util
    path = Path(__file__).resolve().parent / "compare-bench.py"
    spec = importlib.util.spec_from_file_location("compare_bench", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


COMPARE_BENCH = loadCompareBench()


def isLowerBetter(unit: str | None) -> bool:
    return COMPARE_BENCH.is_lower_better({"unit": unit or ""})


def read_lanes(binary: Path, onlyTags: str | None = None) -> tuple[dict[str, float], dict[str, str]]:
    """One run of a bench binary, as {lane: value} and {lane: unit}."""
    argv = [str(binary)] + (["--only", onlyTags] if onlyTags else [])
    out = subprocess.run(argv, capture_output=True, text=True)
    if out.returncode != 0:
        sys.exit(f"bench-ab: {binary} exited {out.returncode}\n{out.stderr[-2000:]}")
    try:
        doc = json.loads(out.stdout)
    except json.JSONDecodeError as exc:
        sys.exit(f"bench-ab: {binary} did not print JSON ({exc}). Never redirect a bench's stdout.")
    return ({b["name"]: float(b["value"]) for b in doc["benchmarks"]},
            {b["name"]: str(b["unit"]) for b in doc["benchmarks"]})


def sign_test(wins: int, n: int) -> float:
    """Two-sided sign test. Reported beside the normalised median, never alone."""
    tail = min(wins, n - wins)
    return min(1.0, sum(math.comb(n, k) for k in range(tail + 1)) / 2 ** n * 2)


def main() -> int:
    ap = argparse.ArgumentParser(description="Control-normalised A/B between two bench binaries.")
    ap.add_argument("base")
    ap.add_argument("arm")
    ap.add_argument("--lane", required=True, help="the lane under test")
    ap.add_argument("--controls", default=",".join(DEFAULT_CONTROLS),
                    help="comma-separated lanes the change cannot reach")
    ap.add_argument("--pairs", type=int, default=15, help="interleaved pairs (default 15)")
    ap.add_argument("--only-tags", default=None,
                    help="comma-separated lane-group tags passed to the binaries as --only, so a pair "
                         "runs the groups it compares instead of the whole battery. Shorter pairs "
                         "cancel drift at finer timescales and buy more pairs per minute; the run "
                         "refuses if the scoping excludes the subject or a control, because a scan "
                         "whose scope excludes the answer reports whatever survived it. The null "
                         "floor is a property of the CONFIGURATION — re-measure it with --null-floor "
                         "before trusting verdicts near it")
    ap.add_argument("--null-floor", action="store_true",
                    help="run the same binary as both arms to measure the floor. This is the one use "
                         "where byte-identical arms are the point, so the identity refusal is waived "
                         "and the verdict line reports the floor instead of a direction")
    ap.add_argument("--profile-change", action="store_true",
                    help="declare the arms differ only by PGO profile; this tool then refuses")
    args = ap.parse_args()

    base, arm = Path(args.base), Path(args.arm)
    for p in (base, arm):
        if not p.is_file():
            sys.exit(f"bench-ab: {p} is not a file")

    # Two binaries with the same bytes measure the noise floor, not a change,
    # so the identity refusal stands unless the floor is what was asked for.
    # (The check hashes content, so a copy under another name never got past
    # it — the flag is the escape, not renaming.)
    digest = subprocess.run(["shasum", "-a", "1", str(base), str(arm)], capture_output=True, text=True).stdout.split()
    identical = bool(digest) and digest[0] == digest[2]
    if identical and not args.null_floor:
        sys.exit("bench-ab: the two binaries are byte-identical — this measures the floor, not a change.\n"
                 "         Build the arms separately, or pass --null-floor to measure the floor deliberately.")
    if args.null_floor and not identical:
        sys.exit("bench-ab: --null-floor with two DIFFERENT binaries folds the change into the floor.\n"
                 "          Pass the same binary twice.")

    # A retrain leaves the sources identical and changes only the profile the
    # two arms were built with. That is the case the controls cannot survive,
    # so it is refused rather than normalised.
    if args.profile_change:
        sys.exit("bench-ab: this compares a profile change, and control normalisation cannot.\n"
                 "          A retrain re-measures every counter, so it moves the control lanes with\n"
                 "          the subject and the division cancels the effect. Score a profile against an\n"
                 "          independent workload — vtebench unicode — not against bench_stack, which is\n"
                 "          what pgo-train.sh trains on.")

    controls = [c for c in args.controls.split(",") if c]
    probe, units = read_lanes(base, args.only_tags)
    missing = [name for name in [args.lane, *controls] if name not in probe]
    if missing:
        scopeNote = (f"          The run was scoped to --only-tags {args.only_tags}; a scope that excludes\n"
                     f"          the answer reports whatever survived it, so widen the tags to cover every\n"
                     f"          lane named here.\n") if args.only_tags else ""
        sys.exit(f"bench-ab: {base} emits no lane named {', '.join(missing)}.\n"
                 + scopeNote
                 + f"          A control that is absent cannot cancel drift, so this refuses rather than\n"
                 f"          reporting a raw ratio. Lanes present: {', '.join(sorted(probe))}")
    if args.lane in controls:
        sys.exit(f"bench-ab: {args.lane} is both the subject and a control — it would cancel itself.")

    ratios, drifts = [], []
    baseBest, armBest = [], []
    import time
    pairsStarted = time.monotonic()
    for i in range(args.pairs):
        (b, _), (a, _) = read_lanes(base, args.only_tags), read_lanes(arm, args.only_tags)   # interleaved, base first
        try:
            # Controls are inverted the same way, so a mixed set still cancels.
            def controlRatio(name: str) -> float:
                r = a[name] / b[name]
                return 1.0 / r if isLowerBetter(units.get(name)) else r

            drift = math.exp(sum(math.log(controlRatio(c)) for c in controls) / len(controls))
        except (ZeroDivisionError, ValueError):
            sys.exit("bench-ab: a control lane read zero or negative — cannot normalise.")
        subject = a[args.lane] / b[args.lane]
        if isLowerBetter(units.get(args.lane)):
            subject = 1.0 / subject          # a duration falling is the arm winning
        ratios.append(subject / drift)
        baseBest.append(b[args.lane])
        armBest.append(a[args.lane])
        drifts.append(drift)
        print(f"  pair {i + 1:>3}/{args.pairs}  subject {a[args.lane] / b[args.lane]:.4f}"
              f"   control drift {drift:.4f}   normalised {ratios[-1]:.4f}", file=sys.stderr)

    wins = sum(1 for r in ratios if r > 1.0)
    median = statistics.median(ratios)
    p = sign_test(wins, len(ratios))

    # Best-of-n, because the contamination here is one-sided: a stall can only
    # slow a run, never speed one. The best sample is the closest thing to the
    # lane's real speed and a median mixes it with whatever else the machine
    # was doing. Reported beside
    # the paired median rather than instead of it — they answer different
    # questions, and a change is only convincing when they agree.
    lower = isLowerBetter(units.get(args.lane))
    pick = min if lower else max
    bestRatio = (pick(baseBest) / pick(armBest)) if lower else (pick(armBest) / pick(baseBest))

    pairSeconds = (time.monotonic() - pairsStarted) / max(1, len(ratios))

    print(f"\nlane        {args.lane}")
    print(f"controls    {', '.join(controls)}")
    print(f"pairs       {len(ratios)} interleaved, base first, {pairSeconds:.1f}s per pair"
          + (f"  (scoped to --only {args.only_tags})" if args.only_tags else "  (full battery)"))
    print(f"drift       control geomean ranged {min(drifts):.4f} .. {max(drifts):.4f}"
          f"  ({(max(drifts) / min(drifts) - 1) * 100:.1f}% spread the change did not cause)")
    print(f"normalised  median {median:.4f}  ->  {(median - 1) * 100:+.2f}%"
          f"   range {min(ratios):.4f} .. {max(ratios):.4f}")
    print(f"direction   arm faster in {wins}/{len(ratios)} pairs   sign test p={p:.3f}")
    print(f"best-of-n   {(bestRatio - 1) * 100:+.2f}%   (each arm's cleanest run; noise here only subtracts)")

    if args.null_floor:
        # The floor a verdict is judged against must be the same statistic the
        # verdict uses, and the verdict uses the MEDIAN of the normalised
        # ratios. The widest single pair is reported beside it as what one
        # pair can lie by — it is why medians gate and pairs do not.
        floor = abs(median - 1) * 100
        widest = max(abs(r - 1) for r in ratios) * 100
        print(f"\nverdict     NULL FLOOR {floor:.2f}% (median, the statistic verdicts use)."
              f"\n            Widest single pair {widest:.2f}% — what one pair can lie by."
              f"\n            A real A/B in this configuration cannot claim a direction inside the floor.")
        return 0

    # The floor is a property of the host and of the configuration, measured with
    # --null-floor on two identical arms. A median inside it says the instrument
    # could not separate the arms, whatever p says.
    if abs(median - 1) * 100 < 1.2:
        print("\nverdict     UNRESOLVED — inside the null floor (1.2%). Do not claim a direction.")
    elif p > 0.05:
        print("\nverdict     UNRESOLVED — the direction is not consistent across pairs.")
    else:
        agree = (bestRatio - 1) * (median - 1) > 0
        print(f"\nverdict     {'ARM FASTER' if median > 1 else 'ARM SLOWER'} by {abs(median - 1) * 100:.2f}%,"
              f" consistent across pairs (p={p:.3f})."
              + ("" if agree else "\n            NOTE: best-of-n disagrees in sign with the paired median."
                                  " Treat as unresolved until they agree."))
    return 0


if __name__ == "__main__":
    sys.exit(main())
