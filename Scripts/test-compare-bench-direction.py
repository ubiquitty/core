#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

# test-compare-bench-direction.py — which way each benchmark unit points, and
# that the merge of several rounds agrees with the gate about it.
#
# Verify-by-revert coverage for a defect the gate cannot see in itself: a
# lower-is-better set listing `bytes` and `milliseconds` and nothing else reads
# lanes in `ms`, `ms/step` and `ns/row` as rates, so a lane at three times its
# ns/row is reported as improved and the gate exits clean.
#
#   ./Scripts/test-compare-bench-direction.py

import importlib.util
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent


def loadModule(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


compare_bench = loadModule("compare_bench", REPO_ROOT / "Scripts" / "compare-bench.py")
merge_rounds = loadModule("merge_bench_rounds", REPO_ROOT / "Scripts" / "merge-bench-rounds.py")


class DirectionTests(unittest.TestCase):
    def test_rates_are_higher_better(self):
        for unit in ("MiB/s", "Mcell/s", "Mencode/s", "Mstep/s", "fps"):
            self.assertFalse(compare_bench.is_lower_better({"unit": unit}), unit)

    def test_everything_else_is_lower_better(self):
        # ms/step and ns/row are the two the substring test `"/s" in unit` gets
        # wrong, and ms/milliseconds/bytes are the plain cases.
        for unit in ("ms", "ms/step", "ns/row", "milliseconds", "bytes"):
            self.assertTrue(compare_bench.is_lower_better({"unit": unit}), unit)

    def test_unknown_unit_defaults_to_lower_better(self):
        # The safe side: a new lane nobody declared a direction for fails loudly
        # on an improvement rather than passing silently through a regression.
        self.assertTrue(compare_bench.is_lower_better({"unit": "widgets"}))
        self.assertTrue(compare_bench.is_lower_better({}))

    def test_explicit_direction_wins(self):
        self.assertTrue(compare_bench.is_lower_better({"unit": "MiB/s", "direction": "lower"}))
        self.assertFalse(compare_bench.is_lower_better({"unit": "ns/row", "direction": "higher"}))

class GateDirectionTests(unittest.TestCase):
    """The rule as the gate applies it, end to end through compare-bench.py."""

    def runGate(self, scale_by_name):
        baseline = {"platform": "fixture", "benchmarks": [
            {"name": "shaped_row_cached", "unit": "ns/row", "value": 100.0, "p95": 110.0},
            {"name": "rowdelta_encode", "unit": "Mencode/s", "value": 10.0, "p95": 9.0}]}
        current = json.loads(json.dumps(baseline))
        for bench in current["benchmarks"]:
            factor = scale_by_name.get(bench["name"])
            if factor is not None:
                bench["value"] *= factor
                bench["p95"] = bench["value"]
        with tempfile.TemporaryDirectory() as directory:
            reference = Path(directory) / "baseline.json"
            candidate = Path(directory) / "current.json"
            reference.write_text(json.dumps(baseline))
            candidate.write_text(json.dumps(current))
            result = subprocess.run(
                [sys.executable, str(REPO_ROOT / "Scripts" / "compare-bench.py"), str(reference), str(candidate),
                 "--gate", "--threshold", "12", "--no-staleness-check"],
                capture_output=True, text=True, check=False)
        return result.returncode, result.stdout + result.stderr

    def test_a_slower_cost_per_row_lane_gates(self):
        code, output = self.runGate({"shaped_row_cached": 3.0})
        self.assertEqual(code, 1, "a lane at 3x its ns/row must gate")
        self.assertIn("regressed", output)

    def test_a_faster_cost_per_row_lane_passes(self):
        code, _ = self.runGate({"shaped_row_cached": 0.5})
        self.assertEqual(code, 0, "a lane at half its ns/row is an improvement, not a regression")

    def test_a_slower_rate_lane_still_gates(self):
        # The control: a rate lane reads as a rate, so reading cost units
        # correctly has not traded one direction for the other.
        code, _ = self.runGate({"rowdelta_encode": 1.0 / 3.0})
        self.assertEqual(code, 1, "a rate lane at a third of its throughput must gate")


class MergeRoundsTests(unittest.TestCase):
    """A merged capture keeps each lane's best, by the same rule the gate uses."""

    def merge(self, rounds):
        paths = []
        for data in rounds:
            with tempfile.NamedTemporaryFile("w", suffix=".json", delete=False) as handle:
                json.dump(data, handle)
                paths.append(handle.name)
        with tempfile.NamedTemporaryFile("w", suffix=".json", delete=False) as handle:
            out = handle.name
        subprocess.run([sys.executable, str(REPO_ROOT / "Scripts" / "merge-bench-rounds.py"), "--out", out, *paths],
                       capture_output=True, text=True, check=True)
        with open(out, encoding="utf-8") as handle:
            merged = json.load(handle)
        for path in [*paths, out]:
            Path(path).unlink()
        return {b["name"]: b for b in merged["benchmarks"]}

    def test_best_is_max_for_a_rate_and_min_for_a_cost(self):
        roundOne = {"platform": "x", "benchmarks": [
            {"name": "rate", "unit": "MiB/s", "value": 100.0, "p95": 90.0},
            {"name": "cost", "unit": "ns/row", "value": 50.0, "p95": 55.0}]}
        roundTwo = {"platform": "x", "benchmarks": [
            {"name": "rate", "unit": "MiB/s", "value": 140.0, "p95": 130.0},
            {"name": "cost", "unit": "ns/row", "value": 40.0, "p95": 44.0}]}
        merged = self.merge([roundOne, roundTwo])
        self.assertEqual(merged["rate"]["value"], 140.0)
        self.assertEqual(merged["cost"]["value"], 40.0)
        # p95 travels with the value it was measured beside.
        self.assertEqual(merged["rate"]["p95"], 130.0)
        self.assertEqual(merged["cost"]["p95"], 44.0)

    def test_a_lane_missing_from_a_round_survives(self):
        roundOne = {"platform": "x", "benchmarks": [{"name": "only", "unit": "MiB/s", "value": 10.0}]}
        roundTwo = {"platform": "x", "benchmarks": []}
        merged = self.merge([roundOne, roundTwo])
        self.assertEqual(merged["only"]["value"], 10.0)

    def test_a_non_positive_value_is_not_evidence(self):
        # A lane that failed to measure reads as zero, which on a cost lane is
        # the best possible number and would win every merge.
        roundOne = {"platform": "x", "benchmarks": [{"name": "cost", "unit": "ns/row", "value": 50.0}]}
        roundTwo = {"platform": "x", "benchmarks": [{"name": "cost", "unit": "ns/row", "value": 0.0}]}
        merged = self.merge([roundOne, roundTwo])
        self.assertEqual(merged["cost"]["value"], 50.0)


if __name__ == "__main__":
    sys.exit(unittest.main())
