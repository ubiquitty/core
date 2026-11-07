#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""Tests check-stale-landing.py against a synthetic repository.

A branch that takes a recently landed line back to its old text fails until its
message names the commit it undoes, in both the range check the pre-push gate
runs and the staged check the commit-msg hook runs.
"""

import os
import json
from pathlib import Path
import shutil
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


class MergePRTargetTests(unittest.TestCase):
    """The shared merger uses the caller's refs and never contacts GitHub here."""

    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="ubq-merge-pr-test-")
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.log = self.root / "gh.jsonl"
        self.bin = self.root / "bin"
        self.bin.mkdir()
        self.environment = dict(os.environ, **IDENTITY)
        for name in ("GIT_DIR", "GIT_WORK_TREE", "GIT_INDEX_FILE", "GIT_PREFIX",
                     "GIT_COMMON_DIR", "GIT_OBJECT_DIRECTORY",
                     "GIT_ALTERNATE_OBJECT_DIRECTORIES", "GIT_QUARANTINE_PATH"):
            self.environment.pop(name, None)
        self.environment.update(PATH=str(self.bin) + os.pathsep + os.environ["PATH"],
                                UBQ_TEST_GH_LOG=str(self.log))
        gh = self.bin / "gh"
        gh.write_text("#!" + sys.executable + "\n" + '''
import json
import os
from pathlib import Path
import sys

arguments = sys.argv[1:]
with open(os.environ["UBQ_TEST_GH_LOG"], "a") as output:
    output.write(json.dumps({"cwd": os.getcwd(), "args": arguments}) + "\\n")
if arguments[:2] == ["repo", "view"]:
    print("fixture/" + Path.cwd().name)
elif arguments[:2] == ["pr", "view"]:
    print(os.environ["UBQ_TEST_GH_HEAD"] + " topic")
elif arguments[:1] == ["api"] or arguments[:2] == ["pr", "merge"]:
    pass
else:
    sys.exit("unexpected fake gh operation: " + repr(arguments))
''', encoding="utf-8")
        gh.chmod(0o755)
        self.core, self.coreHead = self.repository("core")
        scripts = self.core / "Scripts"
        scripts.mkdir()
        for name in ("merge-pr.sh", "check-stale-landing.py"):
            shutil.copyfile(ROOT / "Scripts" / name, scripts / name)
        self.helper = scripts / "merge-pr.sh"

    def git(self, root, *arguments):
        return subprocess.run(
            ["git", "-c", "commit.gpgsign=false", "-c", "core.hooksPath=/dev/null",
             *arguments], cwd=root, env=self.environment, capture_output=True,
            text=True, check=True).stdout.strip()

    def repository(self, role):
        root = self.root / role
        root.mkdir()
        remote = self.root / (role + ".git")
        self.git(self.root, "init", "--bare", "-q", str(remote))
        self.git(root, "init", "-q", "-b", "main")
        self.git(root, "remote", "add", "origin", str(remote))
        (root / "state.txt").write_text("alpha\nbeta\n", encoding="utf-8")
        self.git(root, "add", "state.txt")
        self.git(root, "commit", "-q", "-m", "Base")
        (root / "state.txt").write_text("alpha\nbeta two\ngamma\n", encoding="utf-8")
        self.git(root, "commit", "-qam", "Expand state")
        self.git(root, "push", "-q", "origin", "main")
        self.git(root, "checkout", "-qb", "topic")
        (root / "addition.txt").write_text("the proposed addition\n", encoding="utf-8")
        self.git(root, "add", "addition.txt")
        self.git(root, "commit", "-q", "-m", "Add independent content")
        head = self.git(root, "rev-parse", "HEAD")
        self.git(root, "push", "-q", "origin", "HEAD:refs/pull/7/head")
        return root, head

    def merge(self, caller, head, number="7"):
        environment = dict(self.environment, UBQ_TEST_GH_HEAD=head)
        result = subprocess.run(["bash", str(self.helper), number], cwd=caller,
                                env=environment, capture_output=True, text=True,
                                check=False)
        calls = [json.loads(line) for line in self.log.read_text().splitlines()] if self.log.exists() else []
        return result, calls

    def assertMerged(self, caller, head, root):
        result, calls = self.merge(caller, head)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertTrue(all(call["cwd"] == str(root) for call in calls), calls)
        status = next(call["args"] for call in calls if call["args"][0] == "api")
        self.assertIn("repos/fixture/" + root.name + "/statuses/" + head, status)
        self.assertIn("state=success", status)
        self.assertIn("context=stale-landing", status)
        merge = next(call["args"] for call in calls if call["args"][:2] == ["pr", "merge"])
        self.assertEqual(merge, ["pr", "merge", "7", "--rebase", "--match-head-commit", head])

    def testAppCallerUsesAppRefsAndStatus(self):
        app, head = self.repository("app")
        source = app / "Source"
        source.mkdir()
        self.assertMerged(source, head, app)

    def testKitCallerUsesKitRefsAndStatus(self):
        kit, head = self.repository("kit")
        self.assertMerged(kit, head, kit)

    def testForeignGitVariablesCannotRedirectAppMergeToCore(self):
        app, head = self.repository("app")
        git = self.core / ".git"
        self.environment.update(GIT_DIR=str(git), GIT_WORK_TREE=str(self.core),
                                GIT_INDEX_FILE=str(git / "index"), GIT_PREFIX="Source/",
                                GIT_COMMON_DIR=str(git), GIT_OBJECT_DIRECTORY=str(git / "objects"),
                                GIT_ALTERNATE_OBJECT_DIRECTORIES=str(git / "objects"),
                                GIT_QUARANTINE_PATH=str(git / "objects"))
        self.assertMerged(app, head, app)

    def testStandaloneCoreUsesCoreRefsAndStatus(self):
        self.assertMerged(self.core, self.coreHead, self.core)

    def testOutsideRepositoryUsesStandaloneCore(self):
        self.assertMerged(self.root, self.coreHead, self.core)

    def testFetchedHeadMismatchNeverPostsSuccessOrMerges(self):
        app, _ = self.repository("app")
        result, calls = self.merge(app, "f" * 40)
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertIn("but GitHub reports head", result.stderr)
        self.assertFalse(any(call["args"][0] == "api" or call["args"][:2] == ["pr", "merge"]
                             for call in calls), calls)

    def testOutdatedBaseNeverPostsSuccessOrMerges(self):
        app, head = self.repository("app")
        self.git(app, "checkout", "-q", "main")
        (app / "later.txt").write_text("new main content\n", encoding="utf-8")
        self.git(app, "add", "later.txt")
        self.git(app, "commit", "-q", "-m", "Advance main")
        self.git(app, "push", "-q", "origin", "main")
        self.git(app, "checkout", "-q", "topic")
        result, calls = self.merge(app, head)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("not on the current main", result.stderr)
        self.assertFalse(any(call["args"][0] == "api" or call["args"][:2] == ["pr", "merge"]
                             for call in calls), calls)

    def testStaleLandingPostsFailureAndNeverMerges(self):
        app, _ = self.repository("app")
        (app / "state.txt").write_text("alpha\nbeta\n", encoding="utf-8")
        self.git(app, "commit", "-qam", "Change proposal")
        head = self.git(app, "rev-parse", "HEAD")
        self.git(app, "push", "-q", "origin", "HEAD:refs/pull/7/head")
        result, calls = self.merge(app, head)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        status = next(call["args"] for call in calls if call["args"][0] == "api")
        self.assertIn("repos/fixture/app/statuses/" + head, status)
        self.assertIn("state=failure", status)
        self.assertFalse(any(call["args"][:2] == ["pr", "merge"] for call in calls), calls)


if __name__ == "__main__":
    unittest.main()
