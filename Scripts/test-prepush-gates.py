#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""Drives the real pre-push gate library and hook in a scratch repository.

The gates it runs are two scripts that append their names to a file, so what the
library chose to run, and in which tier, is read off that file. Each case is a
way a push could be narrowed, or a plan could stand in for a run, that must not
be possible.
"""

import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

SCRIPTS = Path(__file__).resolve().parent
HOOK = SCRIPTS / "hooks/pre-push"
UNSET = "0000000000000000000000000000000000000000"

GATES = f"""#!/usr/bin/env bash
source "{SCRIPTS}/pre-push-gates-lib.sh"
echo "tier: $TIER" >&2
if ubq_plan_active && [ -n "${{PLAN_SLEEP:-}}" ]; then
    sleep "$PLAN_SLEEP"
fi
[ -z "${{NEEDS_PRODUCT:-}}" ] || ubq_need "$NEEDS_PRODUCT" "a scratch gate"
case "$TIER" in
    lint) run "lint gate" ./Scripts/fake-lint.sh ;;
    *) run "lint gate" ./Scripts/fake-lint.sh; run "build gate" ./Scripts/fake-build.sh ;;
esac
ubq_pre_push_finish
ubq_plan_active && echo "ran-past-the-end-of-a-plan" >> "$MARKER"
exit 0
"""


def run(arguments, cwd, **options):
    return subprocess.run(arguments, cwd=cwd, capture_output=True, text=True, check=True, **options).stdout.strip()


class PushGateTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        self.repository = self.directory / "repo"
        (self.repository / "Scripts").mkdir(parents=True)
        (self.repository / "Docs").mkdir()
        (self.repository / "Tests").mkdir()
        self.marker = self.directory / "ran"
        for command in (["init", "-q", "-b", "main"], ["config", "user.email", "t@example.com"],
                        ["config", "user.name", "t"]):
            self.git(*command)
        (self.repository / "CMakeLists.txt").write_text("# scratch\n")
        (self.repository / "README.md").write_text("readme\n")
        (self.repository / "Docs/guide.md").write_text("guide\n")
        (self.repository / "Tests/test_x.cpp").write_text("// test\n")
        (self.repository / "Scripts/pre-push-gates.sh").write_text(GATES)
        for name in ("lint", "build"):
            script = self.repository / f"Scripts/fake-{name}.sh"
            script.write_text(f'#!/bin/sh\necho {name} >> "$MARKER"\n')
            script.chmod(0o755)
        (self.repository / "Scripts/pre-push-gates.sh").chmod(0o755)
        self.git("add", "-A")
        self.git("commit", "-q", "-m", "base")
        self.git("update-ref", "refs/remotes/origin/main", "HEAD")

    def git(self, *arguments):
        return run(["git", *arguments], self.repository)

    def commit(self, message="change"):
        self.git("add", "-A")
        self.git("commit", "-q", "-m", message)

    def environment(self, **extra):
        env = {key: value for key, value in os.environ.items()
               if not key.startswith(("UBQ_", "UBIQUITTY_", "GIT_", "PLAN_", "NEEDS_"))}
        env.update(MARKER=str(self.marker), UBQ_PREPUSH_PREREQUISITES="off", UBQ_PREPUSH_PTY_HEADROOM="0", UBQ_BUILD_ROOT=str(self.directory / "build"))
        env.update(extra)
        return env

    def push(self, tier=None, refs=None, hook=False, **extra):
        """Runs the gates, or the hook with `refs`; returns what ran, the status and the stderr."""
        self.marker.unlink(missing_ok=True)
        env = self.environment(**extra)
        if tier:
            env["UBIQUITTY_PREPUSH"] = tier
        if hook:
            arguments = ["bash", str(HOOK), "origin", "https://example.invalid/repo.git"]
            finished = subprocess.run(arguments, cwd=self.repository, env=env, input=refs or "",
                                      capture_output=True, text=True)
        else:
            finished = subprocess.run(["bash", "Scripts/pre-push-gates.sh"], cwd=self.repository, env=env,
                                      capture_output=True, text=True)
        ran = self.marker.read_text().split() if self.marker.exists() else []
        return ran, finished.returncode, finished.stderr

    def head(self):
        return self.git("rev-parse", "HEAD")

    DOCUMENTATION_ONLY = ["lint"]
    EVERYTHING = ["lint", "build"]

    # -- the narrowing itself -------------------------------------------------

    def testADocumentationOnlyDiffRunsTheLintTierAndSaysSo(self):
        (self.repository / "README.md").write_text("changed\n")
        self.commit()
        ran, status, error = self.push()
        self.assertEqual((ran, status), (self.DOCUMENTATION_ONLY, 0), error)
        self.assertIn("tier: lint", error)
        self.assertIn("documentation-only diff", error)

    def testACodeDiffRunsEverything(self):
        (self.repository / "Tests/test_x.cpp").write_text("// changed\n")
        self.commit()
        self.assertEqual(self.push()[:2], (self.EVERYTHING, 0))

    def testAMixedDiffRunsEverything(self):
        (self.repository / "README.md").write_text("changed\n")
        (self.repository / "Tests/test_x.cpp").write_text("// changed\n")
        self.commit()
        self.assertEqual(self.push()[:2], (self.EVERYTHING, 0))

    def testAnEmptyDiffRunsEverything(self):
        self.assertEqual(self.push()[:2], (self.EVERYTHING, 0))

    def testExplicitTiersAreNeverNarrowed(self):
        (self.repository / "README.md").write_text("changed\n")
        self.commit()
        for tier in ("ctest", "linux", "full"):
            with self.subTest(tier=tier):
                self.assertEqual(self.push(tier=tier)[:2], (self.EVERYTHING, 0))

    def testTheDefaultTiersAreTheOnesNarrowed(self):
        (self.repository / "README.md").write_text("changed\n")
        self.commit()
        for tier in ("quick", "default", "lint"):
            with self.subTest(tier=tier):
                self.assertEqual(self.push(tier=tier)[:2], (self.DOCUMENTATION_ONLY, 0))

    def testTheNarrowingCanBeTurnedOff(self):
        (self.repository / "README.md").write_text("changed\n")
        self.commit()
        self.assertEqual(self.push(UBQ_PREPUSH_DOCS_ONLY="off")[:2], (self.EVERYTHING, 0))

    def testALeftOverInternalFlagNarrowsNothing(self):
        (self.repository / "Tests/test_x.cpp").write_text("// changed\n")
        self.commit()
        self.assertEqual(self.push(tier="full", UBQ_DOCS_ONLY="1")[:2], (self.EVERYTHING, 0))

    # -- a code change dressed as documentation ------------------------------

    def testARenameIntoDocumentationIsACodeChange(self):
        self.git("mv", "Tests/test_x.cpp", "Docs/test_x.md")
        self.commit()
        self.assertEqual(self.push()[:2], (self.EVERYTHING, 0))

    def testADeletionOfCodeIsACodeChange(self):
        self.git("rm", "-q", "Tests/test_x.cpp")
        (self.repository / "README.md").write_text("changed\n")
        self.commit()
        self.assertEqual(self.push()[:2], (self.EVERYTHING, 0))

    def testASymlinkAtADocumentationPathIsACodeChange(self):
        os.symlink("../Tests/test_x.cpp", self.repository / "Docs/link.md")
        self.commit()
        self.assertEqual(self.push()[:2], (self.EVERYTHING, 0))

    def testAnExecutableBitOnADocumentationPathIsACodeChange(self):
        (self.repository / "README.md").write_text("changed\n")
        (self.repository / "README.md").chmod(0o755)
        self.git("add", "README.md")
        self.git("update-index", "--chmod=+x", "README.md")
        self.commit()
        self.assertEqual(self.push()[:2], (self.EVERYTHING, 0))

    def testMarkdownUnderTestsIsAFixtureAndACodeChange(self):
        (self.repository / "Tests/fixture.md").write_text("input a test reads\n")
        self.commit()
        self.assertEqual(self.push()[:2], (self.EVERYTHING, 0))

    def pin_bump(self):
        """README plus a library pin that moves, with the mount left unexpandable."""
        for number, sha in enumerate(("1" * 40, "2" * 40)):
            self.git("update-index", "--add", "--cacheinfo", f"160000,{sha},Libraries/ubiquitty-kit")
            if number == 0:
                self.git("commit", "-q", "-m", "mount")
                self.git("update-ref", "refs/remotes/origin/main", "HEAD")
        (self.repository / "README.md").write_text("changed\n")
        self.commit("pin")

    def testALibraryPinBumpIsACodeChange(self):
        self.pin_bump()
        self.assertEqual(self.push()[:2], (self.EVERYTHING, 0))

    def testAContributorsIgnoreSubmodulesConfigCannotHideAPinBump(self):
        self.pin_bump()
        self.git("config", "diff.ignoreSubmodules", "all")
        self.git("config", "submodule.Libraries/ubiquitty-kit.ignore", "all")
        self.assertEqual(self.push()[:2], (self.EVERYTHING, 0))

    def testAClassifierThatFailsIsCode(self):
        (self.repository / "README.md").write_text("changed\n")
        self.commit()
        shims = self.directory / "shims"
        shims.mkdir()
        shim = shims / "python3"
        shim.write_text(f'#!/bin/sh\ncase "$*" in *" classify"*) exit 1;; esac\nexec "{sys.executable}" "$@"\n')
        shim.chmod(0o755)
        ran, status, _ = self.push(PATH=str(shims) + os.pathsep + os.environ["PATH"])
        self.assertEqual((ran, status), (self.EVERYTHING, 0))

    # -- the pushed refs, not HEAD --------------------------------------------

    def testAPushOfAnotherBranchIsNotJudgedOnADocumentationOnlyHead(self):
        code = self.head()
        (self.repository / "README.md").write_text("changed\n")
        self.commit()
        refs = f"refs/heads/other {code} refs/heads/other {UNSET}\n"
        self.assertEqual(self.push(refs=refs, hook=True)[:2], (self.EVERYTHING, 0))

    def testAPushOfHeadItselfIsNarrowed(self):
        (self.repository / "README.md").write_text("changed\n")
        self.commit()
        refs = f"refs/heads/main {self.head()} refs/heads/main {UNSET}\n(delete) {UNSET} refs/heads/gone {'a' * 40}\n"
        self.assertEqual(self.push(refs=refs, hook=True)[:2], (self.DOCUMENTATION_ONLY, 0))

    def testARefsVariableFromTheEnvironmentIsNotTrustedByTheHook(self):
        (self.repository / "README.md").write_text("changed\n")
        self.commit()
        refs = f"refs/heads/main {self.head()} refs/heads/main {UNSET}\n"
        lie = f"refs/heads/main {'b' * 40} refs/heads/main {UNSET}\n"
        self.assertEqual(self.push(refs=refs, hook=True, UBQ_PREPUSH_REFS=lie)[:2], (self.DOCUMENTATION_ONLY, 0))

    # -- plan variables ---------------------------------------------------------

    def testAPlanVariableFromTheEnvironmentCannotTurnARunIntoAPass(self):
        (self.repository / "Tests/test_x.cpp").write_text("// changed\n")
        self.commit()
        ran, status, error = self.push(UBQ_PREPUSH_PLAN="1", UBQ_PREPUSH_PLAN_FILE="/dev/null")
        self.assertEqual((ran, status), (self.EVERYTHING, 0), error)
        self.assertIn("ignoring UBQ_PREPUSH_PLAN", error)

    def testTheHookClearsPlanVariablesBeforeTheGatesSeeThem(self):
        (self.repository / "Tests/test_x.cpp").write_text("// changed\n")
        self.commit()
        refs = f"refs/heads/main {self.head()} refs/heads/main {UNSET}\n"
        ran, status, error = self.push(refs=refs, hook=True, UBQ_PREPUSH_PLAN="1", UBQ_PREPUSH_PLAN_FILE="/dev/null")
        self.assertEqual((ran, status), (self.EVERYTHING, 0), error)
        self.assertNotIn("ignoring UBQ_PREPUSH_PLAN", error)

    # -- the plan ---------------------------------------------------------------

    def testThePlanRunsNoGateAndTheRealRunRunsEachOnce(self):
        (self.repository / "Tests/test_x.cpp").write_text("// changed\n")
        self.commit()
        ran, status, error = self.push(UBQ_PREPUSH_PREREQUISITES="build")
        self.assertEqual((ran, status), (self.EVERYTHING, 0), error)

    def testAMachineOutOfPtysRefusesACodePushBeforeAnyGateRuns(self):
        (self.repository / "Tests/test_x.cpp").write_text("// changed\n")
        self.commit()
        ran, status, error = self.push(UBQ_PREPUSH_PTY_HEADROOM="1000000")
        self.assertEqual((ran, status), ([], 1))
        self.assertIn("pseudo-terminals can be opened", error)

    def testADocumentationOnlyPushNeedsNoPtys(self):
        (self.repository / "README.md").write_text("changed\n")
        self.commit()
        ran, status, error = self.push(UBQ_PREPUSH_PTY_HEADROOM="1000000")
        self.assertEqual((ran, status), (self.DOCUMENTATION_ONLY, 0), error)

    def testAMissingProductRefusesBeforeAnyGateRuns(self):
        (self.repository / "Tests/test_x.cpp").write_text("// changed\n")
        self.commit()
        ran, status, error = self.push(UBQ_PREPUSH_PREREQUISITES="build", NEEDS_PRODUCT="invented-product")
        self.assertEqual((ran, status), ([], 1))
        self.assertIn("REFUSED before any gate ran", error)
        self.assertIn("invented-product", error)

    def testAPlanThatCannotBeMadeIsReportedAndKilledAndTheGatesStillRun(self):
        (self.repository / "Tests/test_x.cpp").write_text("// changed\n")
        self.commit()
        ran, status, error = self.push(UBQ_PREPUSH_PREREQUISITES="build", PLAN_SLEEP="5", UBQ_PREPUSH_PLAN_TIMEOUT="1")
        self.assertEqual((ran, status), (self.EVERYTHING, 0), error)
        self.assertIn("could not be made (timed out after 1s)", error)
        self.assertIn("WITHOUT the build-product check", error)

    def testNoPlanIsMadeForADocumentationOnlyPush(self):
        (self.repository / "README.md").write_text("changed\n")
        self.commit()
        ran, status, error = self.push(UBQ_PREPUSH_PREREQUISITES="build", NEEDS_PRODUCT="invented-product")
        self.assertEqual((ran, status), (self.DOCUMENTATION_ONLY, 0), error)


RUNNER_GATES = f"""#!/usr/bin/env bash
source "{SCRIPTS}/pre-push-gates-lib.sh"
fake_gate_lint() {{ run "lint gate" ./Scripts/fake-lint.sh; }}
fake_gate_build() {{ run "build gate" ./Scripts/fake-build.sh; }}
fake_gate_selected() {{ if ubq_select_all; then run "selected gate" ./Scripts/fake-selected.sh; fi; }}
fake_gate_stands_down() {{ run "standing-down gate" ./Scripts/fake-skip.sh; }}
case "$TIER" in
    families) ubq_run_families ;;
    full) fake_gate_lint; fake_gate_build; fake_gate_selected; fake_gate_stands_down ;;
    *) fake_gate_lint; fake_gate_selected; ubq_in_ci "the build gate" ;;
esac
ubq_pre_push_finish
"""


class GateRunnerTests(unittest.TestCase):
    """The runner's account of a run: what it ran, how long each gate took, what
    it left to CI, and which of the tiers CI drives selects what."""

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        self.repository = self.directory / "repo"
        (self.repository / "Scripts").mkdir(parents=True)
        self.marker = self.directory / "ran"
        for command in (["init", "-q", "-b", "main"], ["config", "user.email", "t@example.com"],
                        ["config", "user.name", "t"]):
            run(["git", *command], self.repository)
        (self.repository / "CMakeLists.txt").write_text("# scratch\n")
        (self.repository / "Scripts/pre-push-gates.sh").write_text(RUNNER_GATES)
        (self.repository / "Scripts/pre-push-gates.sh").chmod(0o755)
        for name in ("lint", "build", "selected"):
            script = self.repository / f"Scripts/fake-{name}.sh"
            script.write_text(f'#!/bin/sh\necho {name} >> "$MARKER"\n')
            script.chmod(0o755)
        skip = self.repository / "Scripts/fake-skip.sh"
        skip.write_text('#!/bin/sh\necho skip >> "$MARKER"\necho "fake-skip: SKIPPED — no such tool here"\n')
        skip.chmod(0o755)
        run(["git", "add", "-A"], self.repository)
        run(["git", "commit", "-q", "-m", "base"], self.repository)
        run(["git", "update-ref", "refs/remotes/origin/main", "HEAD"], self.repository)

    def gates(self, tier, **extra):
        """Runs the gates in `tier`; returns what ran, the status, stdout and stderr."""
        self.marker.unlink(missing_ok=True)
        env = {key: value for key, value in os.environ.items()
               if not key.startswith(("UBQ_", "UBIQUITTY_", "GIT_"))}
        env.update(MARKER=str(self.marker), UBQ_PREPUSH_PREREQUISITES="off", UBQ_PREPUSH_PTY_HEADROOM="0",
                   UBQ_BUILD_ROOT=str(self.directory / "build"), UBIQUITTY_PREPUSH=tier,
                   UBIQUITTY_PREPUSH_CACHE="off")
        env.update(extra)
        finished = subprocess.run(["bash", "Scripts/pre-push-gates.sh"], cwd=self.repository, env=env,
                                  capture_output=True, text=True)
        ran = self.marker.read_text().split() if self.marker.exists() else []
        return ran, finished.returncode, finished.stdout, finished.stderr

    def testEveryGateLineCarriesItsTimeAndTheRunItsTotal(self):
        ran, status, output, error = self.gates("full")
        self.assertEqual((ran, status), (["lint", "build", "selected", "skip"], 0), error)
        for gate in ("lint gate", "build gate", "selected gate"):
            self.assertRegex(output, rf"(?m)^  {gate} +ok  [0-9]+\.[0-9]s$")
        self.assertRegex(output, r"(?m)^  standing-down gate +SKIPPED \(no such tool here\)  [0-9]+\.[0-9]s$")
        self.assertRegex(error, r"pre-push gates: wall time [0-9]+(\.[0-9]s|m[0-9]{2}s) \(tier: full\)")
        self.assertIn("slowest gates:", error)

    def testTheQuickTierNamesWhatItLeavesToCiAndSelectsByTheDiff(self):
        ran, status, _, error = self.gates("quick")
        self.assertEqual((ran, status), (["lint"], 0), error)
        self.assertIn("1 gate(s) run in CI, not in this tier", error)
        self.assertIn("  - the build gate", error)

    def testTheFullTierSelectsWhatTheDiffWouldNot(self):
        self.assertIn("selected", self.gates("full")[0])
        self.assertIn("selected", self.gates("quick", UBQ_PREPUSH_SELECT="all")[0])

    def testTheFamiliesTierRunsTheNamedFamiliesInTheirOrder(self):
        ran, status, _, error = self.gates("families", UBQ_PREPUSH_FAMILIES="fake_gate_build fake_gate_lint")
        self.assertEqual((ran, status), (["build", "lint"], 0), error)

    def testAFamilyNoScriptDefinesRefusesBeforeAnyGateRuns(self):
        for families in ("fake_gate_lint fake_gate_typo", "fake_gate_lint ubq_select_all", "fake_gate_lint run",
                         "fake_gate_lint fake_gate_lint;true"):
            with self.subTest(families=families):
                ran, status, _, error = self.gates("families", UBQ_PREPUSH_FAMILIES=families)
                self.assertEqual((ran, status), ([], 2), error)
                self.assertIn("is not a gate family this checkout defines", error)
        ran, status, _, error = self.gates("families")
        self.assertEqual((ran, status), ([], 2), error)
        self.assertIn("names none", error)

    def testAStrictRunFailsOnAGateThatStoodDown(self):
        families = "fake_gate_lint fake_gate_stands_down"
        ran, status, _, error = self.gates("families", UBQ_PREPUSH_FAMILIES=families)
        self.assertEqual((ran, status), (["lint", "skip"], 0), error)
        ran, status, _, error = self.gates("families", UBQ_PREPUSH_FAMILIES=families, UBQ_PREPUSH_STRICT="1")
        self.assertEqual((ran, status), (["lint", "skip"], 1), error)
        self.assertIn("strict run: 1 gate(s) stood down", error)


# Statements a gate function may contain: the runner's own entry points, shell
# control flow and bookkeeping, case labels, and calls to other gate functions
# (which are scanned too). Anything else, a tool called directly, would run for
# real in a plan, where only `run` is meant to be a dry run.
PERMITTED_WORDS = frozenset({
    "run", "run_for", "run_scoped", "run_whole", "ubq_need", "ubq_in_ci", "local", "return", "exit", "printf", "echo",
    "case", "esac", "if", "then", "else", "elif", "fi", "for", "do", "done", "while", "[", "[[", "{", "}", ";;",
})
CASE_LABEL = re.compile(r"^[A-Za-z0-9_*|\s\"$]+\)")


def permitted(statement):
    first = re.match(r"[^\s(;]+", statement)
    word = first.group(0) if first else statement
    return (word in PERMITTED_WORDS or "gate" in word or word.startswith(("SKIPPED+=", "FAILED+=", "ELSEWHERE+="))
            or bool(CASE_LABEL.match(statement)))


def unscanned_statements(path):
    """The statements in a gate function's body that a plan would run for real."""
    found = []
    inside = False
    continued = False
    for number, line in enumerate(Path(path).read_text().splitlines(), 1):
        if re.match(r"^[a-z_]+\(\) \{", line):
            inside = True
            continue
        if line == "}":
            inside = False
            continue
        text = line.strip()
        if not inside or not text or text.startswith("#"):
            continue
        if continued:
            continued = text.endswith("\\")
            continue
        continued = text.endswith("\\")
        if not permitted(text):
            found.append((number, text[:80]))
    return found


class GateFileTests(unittest.TestCase):
    def testNoStatementInCoresGateFilesRunsForRealUnderAPlan(self):
        self.assertEqual(unscanned_statements(SCRIPTS / "pre-push-gates-core.sh"), [])

    def testTheScanSeesATool(self):
        scratch = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, scratch)
        file = scratch / "gates.sh"
        file.write_text('core_gate_x() {\n    run "a" ./Scripts/a.sh\n    ssh hub true\n    local x=1\n}\n')
        self.assertEqual(unscanned_statements(file), [(3, "ssh hub true")])


if __name__ == "__main__":
    unittest.main()
