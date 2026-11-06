#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""Exercises run-local-hooks.sh against a scratch clone and a linked worktree.

A clone with no hooks.d directory runs nothing and prints nothing. A placed
check runs from the top of the working tree with the hook's arguments and its
input, in name order; a file that is not executable is passed over; the first
check that fails stops the run with its status. A linked worktree runs the
checks its clone holds.
"""

import os
from pathlib import Path
import stat
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
RUNNER = ROOT / "Scripts" / "run-local-hooks.sh"


class LocalHooksTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="local-hooks-")
        self.addCleanup(self.temporary.cleanup)
        self.base = Path(self.temporary.name).resolve()
        self.clone = self.base / "clone"
        self.environment = {key: value for key, value in os.environ.items()
                            if not key.startswith("GIT_")}
        self.environment.update(GIT_CONFIG_GLOBAL=os.devnull, GIT_CONFIG_NOSYSTEM="1")
        self.git("init", "-q", "-b", "main", str(self.clone), directory=self.base)
        (self.clone / "Docs").mkdir()
        (self.clone / "Docs" / "notes.md").write_text("Notes.\n")
        self.git("add", "-A")
        self.git("-c", "user.name=Fixture", "-c", "user.email=fixture@example.com",
                 "commit", "-q", "-m", "fixture")
        self.checks = self.clone / ".git" / "hooks.d" / "pre-push"

    def git(self, *arguments, directory=None):
        return subprocess.run(["git", "-C", str(directory or self.clone), *arguments],
                              env=self.environment, capture_output=True, text=True, check=True)

    def run_hook(self, *arguments, data="", directory=None):
        return subprocess.run(["bash", str(RUNNER), "pre-push", *arguments],
                              cwd=directory or self.clone / "Docs", input=data,
                              env=self.environment, capture_output=True, text=True)

    def place(self, name, body, executable=True):
        self.checks.mkdir(parents=True, exist_ok=True)
        path = self.checks / name
        path.write_text("#!/bin/sh\n" + body)
        if executable:
            path.chmod(path.stat().st_mode | stat.S_IXUSR)
        return path

    def test_a_clone_with_nothing_placed_runs_nothing_and_says_nothing(self):
        finished = self.run_hook("origin", "https://example.com/r.git", data="refs\n")
        self.assertEqual((finished.returncode, finished.stdout, finished.stderr), (0, "", ""))

    def test_a_check_gets_the_top_the_arguments_and_the_input_in_name_order(self):
        record = self.base / "record"
        self.place("20-second", f'echo "second $(pwd) $*" >> "{record}"; cat >> "{record}"\n')
        self.place("10-first", f'echo "first $*" >> "{record}"\n')
        self.place("15-not-executable", f'echo "never" >> "{record}"\n', executable=False)
        finished = self.run_hook("origin", "url", data="refs/heads/main abc refs/heads/main def\n")
        self.assertEqual(finished.returncode, 0, finished.stderr)
        self.assertEqual(record.read_text().splitlines(), [
            "first origin url", f"second {self.clone} origin url",
            "refs/heads/main abc refs/heads/main def"])

    def test_the_first_failing_check_stops_the_run_with_its_status(self):
        record = self.base / "record"
        self.place("10-fails", "echo refused >&2; exit 3\n")
        self.place("20-after", f'echo ran >> "{record}"\n')
        finished = self.run_hook()
        self.assertEqual(finished.returncode, 3)
        self.assertIn("refused", finished.stderr)
        self.assertIn("10-fails exited 3", finished.stderr)
        self.assertFalse(record.exists())

    def test_a_linked_worktree_runs_the_checks_of_its_clone(self):
        record = self.base / "record"
        self.place("10-check", f'pwd >> "{record}"\n')
        worktree = self.base / "worktree"
        self.git("worktree", "add", "-q", "-b", "topic", str(worktree))
        finished = self.run_hook(directory=worktree)
        self.assertEqual(finished.returncode, 0, finished.stderr)
        self.assertEqual(record.read_text().strip(), str(worktree))


if __name__ == "__main__":
    unittest.main()
