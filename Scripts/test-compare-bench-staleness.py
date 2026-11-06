#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

# test-compare-bench-staleness.py — regression test for compare-bench.py's
# non-gating baseline-staleness warnings. Verify-by-revert coverage for the
# case the warnings exist to catch: a stale/architecturally-divergent
# baseline must warn loudly, never gate, and never crash on an unresolvable
# commit.
#
#   ./Scripts/test-compare-bench-staleness.py

import importlib.util
import sys
import unittest
from pathlib import Path
from subprocess import CalledProcessError
from unittest import mock

SCRIPT = Path(__file__).resolve().parent / "compare-bench.py"
spec = importlib.util.spec_from_file_location("compare_bench", SCRIPT)
compare_bench = importlib.util.module_from_spec(spec)
spec.loader.exec_module(compare_bench)


class StalenessWarnings(unittest.TestCase):
    def _git_side_effect(self, age=None, sources_error=True):
        # The real _git is called for both "rev-list --count" (age) and "show
        # <commit>:Scripts/portable-core-sources.txt" (toolchain drift) — a single
        # shared return_value collides the two. Route by subcommand instead.
        def fake_git(*args):
            if args[0] == "rev-list":
                return str(age)
            if args[0] == "show":
                if sources_error:
                    raise CalledProcessError(1, "git")  # baseline predates the source list
                return "\n".join(f"file{i}.cpp" for i in range(200))  # same-size list, no drift
            raise AssertionError(f"unexpected _git call: {args}")
        return fake_git

    def test_age_over_threshold_warns(self):
        with mock.patch.object(compare_bench, "_git", side_effect=self._git_side_effect(age=250)):
            warnings = compare_bench.staleness_warnings({"commit": "abc123"}, {"a"}, {"a"}, 200, 0.20)
        self.assertEqual(len(warnings), 1)
        self.assertIn("250 commits behind", warnings[0])

    def test_age_under_threshold_is_silent(self):
        with mock.patch.object(compare_bench, "_git", side_effect=self._git_side_effect(age=10)):
            warnings = compare_bench.staleness_warnings({"commit": "abc123"}, {"a"}, {"a"}, 200, 0.20)
        self.assertEqual(warnings, [])

    def test_missing_commit_warns_without_crashing(self):
        warnings = compare_bench.staleness_warnings({}, {"a"}, {"a"}, 200, 0.20)
        self.assertEqual(len(warnings), 1)
        self.assertIn("no recorded commit", warnings[0])

    def test_unresolvable_commit_degrades_gracefully(self):
        with mock.patch.object(compare_bench, "_git", side_effect=CalledProcessError(1, "git")):
            warnings = compare_bench.staleness_warnings({"commit": "deadbeef"}, {"a"}, {"a"}, 200, 0.20)
        self.assertEqual(len(warnings), 1)
        self.assertIn("could not resolve", warnings[0])

    def test_benchmark_set_churn_over_threshold_warns(self):
        base_names = {"parse_plain", "parse_sgr", "parse_scroll"}
        cur_names = {"parse_plain", "render_build", "shaped_row_cached"}  # 5/6 churned
        with mock.patch.object(compare_bench, "_git", side_effect=self._git_side_effect(age=5)):
            warnings = compare_bench.staleness_warnings({"commit": "abc123"}, base_names, cur_names, 200, 0.20)
        self.assertTrue(any("churned" in w for w in warnings))

    def test_benchmark_set_stable_is_silent(self):
        names = {"parse_plain", "parse_sgr", "parse_scroll"}
        with mock.patch.object(compare_bench, "_git", side_effect=self._git_side_effect(age=5)):
            warnings = compare_bench.staleness_warnings({"commit": "abc123"}, names, names, 200, 0.20)
        self.assertEqual(warnings, [])

    def test_toolchain_source_count_drift_warns(self):
        with mock.patch.object(compare_bench, "_git", side_effect=self._git_side_effect(age=5, sources_error=False)), \
             mock.patch("builtins.open", mock.mock_open(read_data="only_one_file.cpp\n")):
            warnings = compare_bench.staleness_warnings({"commit": "abc123"}, {"a"}, {"a"}, 200, 0.20)
        # baseline's fake source list has 200 entries, current (mocked open) has 1 — drifted.
        self.assertTrue(any("portable-core-sources.txt" in w for w in warnings))

    def test_warnings_never_gate(self):
        # The regression exit code is driven solely by `regressed`, which staleness_warnings
        # never touches — assert its return type stays a plain list of strings, not something
        # a caller could accidentally fold into the gate decision.
        with mock.patch.object(compare_bench, "_git", side_effect=self._git_side_effect(age=9999)):
            warnings = compare_bench.staleness_warnings({"commit": "abc123"}, {"a"}, {"a"}, 200, 0.20)
        self.assertIsInstance(warnings, list)
        self.assertTrue(all(isinstance(w, str) for w in warnings))


if __name__ == "__main__":
    sys.exit(unittest.main())
