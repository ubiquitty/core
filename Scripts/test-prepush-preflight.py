#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""Exercises the pre-push preflight: documentation-only diffs and missing build products."""

import io
import os
from pathlib import Path
import subprocess
import tempfile
import time
import unittest
import unittest.mock

import prepush_preflight as preflight

SCRIPTS = Path(__file__).resolve().parent


def context(root, darwin=True, tools=("xcrun", "emcc")):
    return preflight.Context(build_root=root / "build", core_root=root / "core", kit_root=root / "kit",
                             subject_root=root / "app", darwin=darwin, tools=frozenset(tools))


class ClassifyTests(unittest.TestCase):
    def testOnlyDocumentationIsDocumentation(self):
        for path in ("README.md", "Docs/architecture.md", "Docs/screens/a.png", "LICENSES/Apache-2.0.txt",
                     "Docs/third-party-licenses/OFL.txt", "NOTICE", "third-party-notices.md", "TRADEMARKS.md",
                     "Scripts/README.md", ".github/pull_request_template.md", "Source/Core/README.md"):
            self.assertTrue(preflight.is_documentation(path), path)

    def testCodeAndDataAreNeverDocumentation(self):
        for path in ("Scripts/check-docs.py", "CMakeLists.txt", "license-scope.json", "Docs/generate.py",
                     "Docs/build.sh", "Docs/other-repositories.txt", "Docs/allowlist", "README.sh", "LICENSE.py",
                     "Source/Core/Terminal.cpp",
                     ".github/workflows/ci.yml", "Scripts/hooks/pre-push", "../Docs/escape.md", "", "Docs"):
            self.assertFalse(preflight.is_documentation(path), path)

    def testADocumentationOnlyDiff(self):
        self.assertEqual(preflight.classify(["README.md", "Docs/a.md", "LICENSES/MIT.txt\n"]), "docs")

    def testAMixedDiffIsCode(self):
        self.assertEqual(preflight.classify(["README.md", "Scripts/pre-push-gates-lib.sh"]), "code")
        self.assertEqual(preflight.classify(["Docs/a.md", "CMakeLists.txt"]), "code")

    def testAMarkedNonRegularPathMakesTheDiffCode(self):
        self.assertEqual(preflight.classify(["README.md", "!Docs/link.md"]), "code")
        self.assertEqual(preflight.classify(["Docs/link.md"]), "docs")

    def testMarkdownUnderTestsIsNotDocumentation(self):
        self.assertFalse(preflight.is_documentation("Tests/Core/fixture.md"))
        self.assertTrue(preflight.is_documentation("Docs/fixture.md"))

    def testAnEmptyOrUnreadableDiffIsCode(self):
        self.assertEqual(preflight.classify([]), "code")
        self.assertEqual(preflight.classify(["", "  "]), "code")


class PlanTests(unittest.TestCase):
    def testAGatesScriptNamesItsProducts(self):
        wanted = preflight.read_plan([
            "GATE\tswift-renderer drivers (draw)\t/x/Scripts/check-swift-renderer.sh --draw\n",
            "GATE\tgate coverage\tpython3 ./Scripts/check-gate-coverage.py\n",
        ])
        self.assertEqual(sorted(wanted), ["core-snapshot-library", "kit-metallib"])
        self.assertEqual(wanted["kit-metallib"], ["swift-renderer drivers (draw)"])

    def testTheTypeCheckPassReadsNoProducts(self):
        wanted = preflight.read_plan([
            "GATE\tswift-renderer drivers (type-check)\t./Scripts/check-swift-renderer.sh --typecheck-only\n",
            "GATE\tswift-renderer drivers (draw)\t./Scripts/check-swift-renderer.sh\n",
        ])
        self.assertEqual(wanted["kit-metallib"], ["swift-renderer drivers (draw)"])
        self.assertEqual(wanted["core-snapshot-library"], ["swift-renderer drivers (draw)"])

    def testADeclaredNeedIsRecorded(self):
        wanted = preflight.read_plan(["NEED\tweb-dist\tsnapshot arm: web\n"])
        self.assertEqual(wanted, {"web-dist": ["snapshot arm: web"]})

    def testAGateWithNoProductsAsksForNothing(self):
        self.assertEqual(preflight.read_plan(["GATE\tbuild dirs\tpython3 ./Scripts/check-build-dirs.py\n"]), {})


class ResolveTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)

    def absent(self, wanted, **options):
        """The needs that apply here and are not present."""
        ctx = context(self.root, **options)
        return [need.identifier for need, _ in preflight.resolve(wanted, ctx) if not need.present(ctx)]

    def testEachMissingProductIsNamed(self):
        wanted = {"core-snapshot-library": ["g"], "kit-metallib": ["g"], "web-dist": ["g"], "host-test-objects": ["g"]}
        self.assertEqual(self.absent(wanted), ["core-snapshot-library", "host-test-objects", "kit-metallib", "web-dist"])

    def testAPresentProductIsNotNamed(self):
        build = self.root / "build"
        (build / "core").mkdir(parents=True)
        (build / "core/libubiquitty-core.a").write_text("")
        (build / "metallib/sub").mkdir(parents=True)
        (build / "metallib/sub/default.metallib").write_text("")
        (build / "web-dist").mkdir()
        (build / "web-dist/ubiquitty.wasm").write_text("")
        (build / "BuildHostTests/ctest").mkdir(parents=True)
        (build / "BuildHostTests/ctest/Terminal.cpp.o").write_text("")
        wanted = {"core-snapshot-library": ["g"], "kit-metallib": ["g"], "web-dist": ["g"], "host-test-objects": ["g"]}
        self.assertEqual(self.absent(wanted), [])

    def testAWebDirectoryWithoutTheWasmIsNotTheWebBuild(self):
        (self.root / "build/web-dist").mkdir(parents=True)
        self.assertEqual(self.absent({"web-dist": ["g"]}), ["web-dist"])

    def testTheAppBundlesMetallibCountsToo(self):
        path = self.root / "build/macos/Source/Platform/macOS"
        path.mkdir(parents=True)
        (path / "default.metallib").write_text("")
        self.assertEqual(self.absent({"kit-metallib": ["g"]}), [])

    def testAppleProductsAreNotAskedForWithoutTheAppleToolchain(self):
        wanted = {"core-snapshot-library": ["g"], "kit-metallib": ["g"]}
        self.assertEqual(self.absent(wanted, darwin=False), [])
        self.assertEqual(self.absent(wanted, tools=("emcc",)), [])

    def testTheWebBuildIsNotAskedForWithoutEmscripten(self):
        saved = os.environ.pop("EMSDK", None)
        self.addCleanup(lambda: os.environ.update({"EMSDK": saved}) if saved else None)
        self.assertEqual(self.absent({"web-dist": ["g"]}, tools=("xcrun",)), [])

    def testAProductNoRuleKnowsIsMissingNotWavedThrough(self):
        self.assertEqual(self.absent({"invented": ["g"]}), ["invented"])


class CheckTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.context = context(self.root)

    def library(self):
        library = self.root / "build/core/libubiquitty-core.a"
        library.parent.mkdir(parents=True, exist_ok=True)
        library.write_text("")

    def testNothingNeededPasses(self):
        out = io.StringIO()
        self.assertEqual(preflight.check({}, self.context, "build", out=out), 0)
        self.assertEqual(out.getvalue(), "")

    def testRefusingNamesTheExactCommandPerProduct(self):
        out = io.StringIO()
        status = preflight.check({"core-snapshot-library": ["swift-renderer drivers"], "kit-metallib": ["swift-renderer drivers"]},
                                 self.context, "refuse", out=out)
        text = out.getvalue()
        self.assertEqual(status, 1)
        self.assertIn("REFUSED before any gate ran", text)
        self.assertIn(f"build it:  {self.root}/core/Scripts/build-core-snapshot.sh", text)
        self.assertIn(f"build it:  {self.root}/kit/Scripts/build-metallib.sh", text)
        self.assertIn("needed by: swift-renderer drivers", text)

    def testRefuseModeBuildsNothing(self):
        built = []
        preflight.check({"core-snapshot-library": ["g"]}, self.context, "refuse", out=io.StringIO(),
                        run=lambda command: built.append(command) or 0)
        self.assertEqual(built, [])

    def testBuildModeBuildsAMissingProductAndPasses(self):
        built = []

        def build(command):
            built.append(command)
            self.library()
            return 0

        out = io.StringIO()
        status = preflight.check({"core-snapshot-library": ["g"]}, self.context, "build", out=out, run=build)
        self.assertEqual(status, 0)
        self.assertEqual(built, [f"{self.root}/core/Scripts/build-core-snapshot.sh"])
        self.assertIn("building core-snapshot-library first", out.getvalue())

    def testBuildModeRunsTheIncrementalBuildEvenWhenACopyStands(self):
        self.library()
        built = []
        status = preflight.check({"core-snapshot-library": ["g"]}, self.context, "build", out=io.StringIO(),
                                 run=lambda command: built.append(command) or 0)
        self.assertEqual(status, 0)
        self.assertEqual(len(built), 1)

    def testAFailedBuildRefusesEvenWithACopyStanding(self):
        self.library()
        out = io.StringIO()
        status = preflight.check({"core-snapshot-library": ["g"]}, self.context, "build", out=out, run=lambda command: 1)
        self.assertEqual(status, 1)
        self.assertIn("core-snapshot-library: FAILED", out.getvalue())
        self.assertIn("(its build failed)", out.getvalue())

    def testABuildThatLeavesTheProductMissingRefuses(self):
        self.assertEqual(preflight.check({"kit-metallib": ["g"]}, self.context, "build", out=io.StringIO(),
                                         run=lambda command: 0), 1)

    def testABuildScriptThatCannotStartIsAFailedBuildNotATraceback(self):
        out = io.StringIO()
        status = preflight.check({"kit-metallib": ["g"]}, self.context, "build", out=out)
        self.assertEqual(status, 1)
        self.assertIn("REFUSED", out.getvalue())

    def testAProductNotCheapToBuildIsNeverBuiltByTheHook(self):
        costly = preflight.Need("costly", "a long build", lambda c: True, lambda c: False, lambda c: "./build-costly.sh")
        built = []
        out = io.StringIO()
        with unittest.mock.patch.dict(preflight.NEEDS, {"costly": costly}):
            status = preflight.check({"costly": ["g"]}, self.context, "build", out=out,
                                     run=lambda command: built.append(command) or 0)
        self.assertEqual(status, 1)
        self.assertEqual(built, [])
        self.assertIn("build it:  ./build-costly.sh", out.getvalue())

    def testTheWebBuildIsAdvisoryNeverBuiltAndNeverRefused(self):
        built = []
        out = io.StringIO()
        status = preflight.check({"web-dist": ["snapshot arm: web"]}, self.context, "build", out=out,
                                 run=lambda command: built.append(command) or 1)
        self.assertEqual(status, 0)
        self.assertEqual(built, [])
        self.assertIn("NOTE", out.getvalue())
        self.assertIn("./Scripts/build-web.sh", out.getvalue())

    def testAnAdvisoryProductIsReportedAndDoesNotRefuse(self):
        out = io.StringIO()
        status = preflight.check({"host-test-objects": ["style (check-style.py)"]}, self.context, "refuse", out=out)
        self.assertEqual(status, 0)
        self.assertIn("NOTE", out.getvalue())
        self.assertIn("./Scripts/build-tests.sh", out.getvalue())
        self.assertNotIn("REFUSED", out.getvalue())


class PlanTimeoutTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)

    def testAPlanThatFinishesIsZero(self):
        self.assertEqual(preflight.plan(["true"], str(self.root / "plan"), "token", 10, out=io.StringIO()), 0)
        self.assertEqual((self.root / "plan.nonce").read_text(), "token")

    def testAFailingPlanIsReportedWithItsLastOutput(self):
        out = io.StringIO()
        status = preflight.plan(["bash", "-c", "echo boom >&2; exit 7"], str(self.root / "plan"), "token", 10, out=out)
        self.assertEqual(status, 3)
        self.assertIn("exited 7", out.getvalue())
        self.assertIn("boom", out.getvalue())

    def testATimedOutPlanKillsItsWholeProcessGroup(self):
        marker = self.root / "survivor"
        script = f"(sleep 3; touch {marker}) & wait"
        out = io.StringIO()
        status = preflight.plan(["bash", "-c", script], str(self.root / "plan"), "token", 1, out=out)
        self.assertEqual(status, 3)
        self.assertIn("timed out after 1s", out.getvalue())
        time.sleep(3.5)
        self.assertFalse(marker.exists())


RUNNER = """
set -u
. "{scripts}/pre-push-cache.sh"
. "{scripts}/pre-push-runner.sh"
ubq_gate_cache_key() {{ printf 'key'; }}
ubq_gate_cache_has() {{ [ -n "${{CACHED_GATE:-}}" ] && [ "$1" = "key" ]; }}
ubq_gate_cache_store() {{ :; }}
export UBQ_SUBJECT_ROOT="$PWD"
"""


class PtyHeadroomTests(unittest.TestCase):
    @staticmethod
    def opener(limit, opened):
        def open_pair():
            if len(opened) >= limit:
                raise OSError(23, "out of pty devices")
            pair = os.pipe()
            opened.append(pair)
            return pair
        return open_pair

    def testEnoughPtysPassAndAreReleased(self):
        opened, out = [], io.StringIO()
        self.assertTrue(preflight.pty_headroom(4, out, self.opener(99, opened)))
        self.assertEqual(len(opened), 4)
        for pair in opened:
            for descriptor in pair:
                with self.assertRaises(OSError):
                    os.fstat(descriptor)
        self.assertEqual(out.getvalue(), "")

    def testTooFewPtysRefuseWithTheCountAndTheCause(self):
        opened, out = [], io.StringIO()
        self.assertFalse(preflight.pty_headroom(8, out, self.opener(3, opened)))
        self.assertIn("only 3 of 8 pseudo-terminals", out.getvalue())
        self.assertIn("login", out.getvalue())
        for pair in opened:
            for descriptor in pair:
                with self.assertRaises(OSError):
                    os.fstat(descriptor)

    def testZeroTurnsTheCheckOff(self):
        self.assertTrue(preflight.pty_headroom(0, io.StringIO(), self.opener(0, [])))


class RunnerTests(unittest.TestCase):
    """The shell side: a plan records and runs nothing, and only the preflight can issue one."""

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        (self.directory / "Scripts").mkdir()
        for name in ("check-swift-renderer.sh", "check-docs.py", "check-build-dirs.py"):
            script = self.directory / "Scripts" / name
            script.write_text('#!/bin/sh\necho "$0" >> "$MARKER"\n')
            script.chmod(0o755)
        self.marker = self.directory / "ran"
        self.plan = self.directory / "plan"

    def shell(self, body, plan=None, **environment):
        """Runs `body` after the runner; `plan` is the (token, nonce on disk) pair a preflight would issue."""
        program = RUNNER.format(scripts=SCRIPTS) + body
        env = {key: value for key, value in os.environ.items() if not key.startswith("UBQ_PREPUSH")}
        env.update(MARKER=str(self.marker), UBQ_PREPUSH_PLAN_FILE=str(self.plan))
        if plan:
            token, nonce = plan
            env["UBQ_PREPUSH_PLAN"] = token
            if nonce is not None:
                Path(str(self.plan) + ".nonce").write_text(nonce)
        env.update(environment)
        return subprocess.run(["bash", "-c", program], cwd=self.directory, env=env, capture_output=True, text=True)

    def ran(self):
        return self.marker.read_text().split() if self.marker.exists() else []

    def testAPlanRecordsGatesAndNeedsAndRunsNothing(self):
        finished = self.shell('run "swift-renderer drivers" ./Scripts/check-swift-renderer.sh\n'
                              'ubq_need web-dist "snapshot arm: web"\n', plan=("t0ken", "t0ken"))
        self.assertEqual(finished.returncode, 0, finished.stderr)
        self.assertEqual(self.ran(), [])
        lines = self.plan.read_text().splitlines()
        self.assertEqual(lines[0].split("\t")[:2], ["GATE", "swift-renderer drivers"])
        self.assertIn("check-swift-renderer.sh", lines[0])
        self.assertEqual(lines[1], "NEED\tweb-dist\tsnapshot arm: web")

    def testAGateWhosePassIsCachedNeedsNothingBuilt(self):
        self.shell('run "swift-renderer drivers" ./Scripts/check-swift-renderer.sh\n',
                   plan=("t0ken", "t0ken"), CACHED_GATE="1")
        self.assertFalse(self.plan.exists() and self.plan.read_text())

    def testAPlanVariableNoPreflightIssuedLeavesTheRunReal(self):
        for plan in (("1", None), ("1", "another-token"), ("t0ken", "t0ken-but-no-plan-file")):
            with self.subTest(plan=plan):
                self.marker.unlink(missing_ok=True)
                finished = self.shell('run "renderer" ./Scripts/check-swift-renderer.sh\n', plan=plan)
                self.assertEqual(finished.returncode, 0, finished.stderr)
                self.assertEqual([Path(path).name for path in self.ran()], ["check-swift-renderer.sh"])
                self.assertFalse(self.plan.exists() and self.plan.read_text())

    def testANeedIsIgnoredOutsideAPlan(self):
        finished = self.shell('ubq_need web-dist "x"\n')
        self.assertEqual(finished.returncode, 0, finished.stderr)
        self.assertFalse(self.plan.exists())

    def testEveryGateRunsWithoutAPlan(self):
        self.shell('run "docs" ./Scripts/check-docs.py\n'
                   'run "renderer" ./Scripts/check-swift-renderer.sh\n')
        self.assertEqual(sorted(Path(path).name for path in self.ran()), ["check-docs.py", "check-swift-renderer.sh"])


if __name__ == "__main__":
    unittest.main()
