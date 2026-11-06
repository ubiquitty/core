#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""Tests check-stale-landing.py against a synthetic repository.

A branch that takes a recently landed line back to its old text fails until its
message names the commit it undoes, in both the range check the pre-push gate
runs and the staged check the commit-msg hook runs.
"""

import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
GATE = ROOT / "Scripts/check-stale-landing.py"
IDENTITY = {"GIT_AUTHOR_NAME": "t", "GIT_AUTHOR_EMAIL": "t@t",
            "GIT_COMMITTER_NAME": "t", "GIT_COMMITTER_EMAIL": "t@t"}


class StaleLandingTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="ubq-stale-landing-test-")
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.environment = dict(os.environ, **IDENTITY)
        self.git("init", "-q", "-b", "main")
        self.write("a.txt", "alpha\nbeta\n")
        self.commit("Base")
        self.write("a.txt", "alpha\nbeta two\ngamma\n")
        self.landed = self.commit("Rename beta and add gamma")
        self.git("branch", "upstream")

    def git(self, *arguments):
        return subprocess.run(
            ["git", "-c", "commit.gpgsign=false", "-c", "core.hooksPath=/dev/null"]
            + list(arguments), cwd=self.root, env=self.environment,
            capture_output=True, text=True, check=True).stdout

    def write(self, name, text):
        (self.root / name).write_text(text, encoding="utf-8")

    def commit(self, message):
        self.git("add", "-A")
        self.git("commit", "-q", "-m", message)
        return self.git("rev-parse", "HEAD").strip()

    def gate(self, *arguments):
        return subprocess.run([sys.executable, str(GATE)] + list(arguments),
                              cwd=self.root, env=self.environment,
                              capture_output=True, text=True, check=False)

    def takeOldFile(self):
        self.write("a.txt", "alpha\nbeta\n")
        self.write("b.txt", "the change the branch is about\n")

    def testUnnamedRevertFailsTheRange(self):
        self.takeOldFile()
        self.commit("Add b")
        finished = self.gate("--base", "upstream")
        self.assertEqual(finished.returncode, 1, finished.stdout)
        self.assertIn(self.landed[:9], finished.stdout)
        self.assertIn("Rename beta and add gamma", finished.stdout)

    def testNamedRevertPasses(self):
        self.takeOldFile()
        self.commit(f"Add b\n\nUndoes {self.landed[:9]}: beta keeps its name.")
        finished = self.gate("--base", "upstream")
        self.assertEqual(finished.returncode, 0, finished.stdout)

    def testSingleCommitForm(self):
        self.takeOldFile()
        commit = self.commit("Add b")
        self.assertEqual(self.gate(commit, "--base", "upstream").returncode, 1)

    def testRewriteWithNewTextPasses(self):
        self.write("a.txt", "alpha\nbeta three\ngamma\n")
        self.commit("Rename beta again")
        self.assertEqual(self.gate("--base", "upstream").returncode, 0)

    def testBranchMayUndoItsOwnCommit(self):
        self.write("a.txt", "alpha\nbeta two\ngamma\ndelta\n")
        self.commit("Add delta")
        self.write("a.txt", "alpha\nbeta two\ngamma\n")
        self.commit("Drop delta")
        self.assertEqual(self.gate("--base", "upstream").returncode, 0)

    def testStagedCheckReadsTheMessage(self):
        self.takeOldFile()
        self.git("add", "-A")
        message = self.root / "MESSAGE"
        message.write_text("Add b\n", encoding="utf-8")
        refused = self.gate("--staged", "--message-file", str(message), "--base", "upstream")
        self.assertEqual(refused.returncode, 1, refused.stderr)
        message.write_text(f"Add b\n\nUndoes {self.landed[:9]}.\n", encoding="utf-8")
        accepted = self.gate("--staged", "--message-file", str(message), "--base", "upstream")
        self.assertEqual(accepted.returncode, 0, accepted.stderr)

    def testOldWorkIsOutsideTheWindow(self):
        self.git("reset", "-q", "--hard", "HEAD~1")
        self.write("a.txt", "alpha\nbeta two\ngamma\n")
        self.environment["GIT_COMMITTER_DATE"] = "2000-01-01T00:00:00Z"
        self.commit("Rename beta and add gamma, long ago")
        del self.environment["GIT_COMMITTER_DATE"]
        self.git("branch", "-f", "upstream")
        self.takeOldFile()
        self.commit("Add b")
        self.assertEqual(self.gate("--base", "upstream").returncode, 0)

    def testBuiltInSelfTestPasses(self):
        self.assertEqual(self.gate("--self-test").returncode, 0)


if __name__ == "__main__":
    unittest.main()
