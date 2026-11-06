#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""Tests the Sync gate's command boundary without compiling or running binaries."""

import importlib.util
import io
from pathlib import Path
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "Scripts"))
SPEC = importlib.util.spec_from_file_location("sync_memory_gate", ROOT / "Scripts/check-sync-memory.py")
gate = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(gate)


class SyncMemoryGateTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="ubq-sync-memory-gate-test-")
        self.addCleanup(self.directory.cleanup)
        self.build = Path(self.directory.name).resolve()
        (self.build / "Tests").mkdir()
        for source in gate.TEST_SOURCES:
            binary = self.build / "Tests" / Path(source).stem
            binary.touch()
            binary.chmod(0o700)
        self.commands = []
        self.output = io.StringIO()
        self.addCleanup(mock.patch.stopall)
        mock.patch("sys.stdout", self.output).start()
        mock.patch("sys.stderr", self.output).start()

    def command(self, arguments, **keywords):
        self.commands.append((arguments, keywords))
        return SimpleNamespace(returncode=0, stdout="test-harness: ok (2 cases; 0.0001s)\n", stderr="")

    def runGate(self, handler=None):
        with mock.patch.object(gate.subprocess, "run", side_effect=handler or self.command):
            return gate.run_gate(self.build, 2)

    def testOnlyExplicitTargetsAreBuiltAndRunDirectly(self):
        self.assertEqual(self.runGate(), 0)
        targets = [Path(source).stem for source in gate.TEST_SOURCES]
        configure = self.commands[0][0]
        self.assertIn("-DUBIQUITTY_FRONTEND=Core", configure)
        self.assertIn("-DUBIQUITTY_QUIC=OFF", configure)
        self.assertIn("-DUBIQUITTY_CTEST_SHELL_REMAINDER=OFF", configure)
        self.assertEqual(self.commands[1][0], ["cmake", "--build", str(self.build), "--parallel", "2", "--target", *targets])
        self.assertEqual([command for command, _ in self.commands[2:]], [[str(self.build / "Tests" / target)] for target in targets])
        for _, keywords in self.commands[2:]:
            self.assertEqual(keywords["timeout"], gate.TEST_TIMEOUT_SECONDS)
            self.assertNotIn("shell", keywords)
            configuration = Path(keywords["env"]["XDG_CONFIG_HOME"])
            self.assertTrue(configuration.is_absolute())
            self.assertTrue(configuration.name.startswith("ubq-sync-memory-config-"))
            self.assertFalse(configuration.exists())
        self.assertIn(f"{len(targets)} in-memory binaries; {2 * len(targets)} cases", self.output.getvalue())

    def testReviewedSetCannotSilentlyLoseACategory(self):
        self.assertEqual(gate.TEST_SOURCES, (
            "Core/Protocol/Sync/test_sync_admission.cpp",
            "Core/Protocol/Sync/test_sync_envelope.cpp",
            "Core/Protocol/Sync/test_sync_resume.cpp",
            "Core/Protocol/Sync/test_sync_checkpoint_storage_admission.cpp",
            "Core/Protocol/Sync/test_sync_rich_text.cpp",
            "Core/Protocol/Sync/test_sync_wire_fingerprint.cpp",
            "Core/Session/test_adopt_transaction.cpp",
            "Core/Session/test_reclaim_transaction.cpp",
            "Platform/test_owned_child.cpp",
            "Core/Protocol/Sync/test_sync_checkpoint_budget.cpp",
            "Core/Protocol/Sync/test_sync_checkpoint_restore_budget.cpp",
            "Base/test_fallible_storage.cpp",
            "Core/Protocol/Sync/test_sync_checkpoint_full_budget.cpp",
            "Core/Protocol/Sync/test_sync_checkpoint_admission.cpp",
            "Core/Protocol/Sync/test_sync_handover_fallible_owners.cpp",
            "Core/Session/test_remotesessioncore.cpp",
            "Core/Session/test_background_refresh_state.cpp",
            "Core/Protocol/Sync/test_sync_images.cpp",
            "Core/Protocol/Sync/test_sync_lanes.cpp",
            "Core/Protocol/Sync/test_sync_input.cpp",
            "Core/Protocol/Sync/test_sync_history_cache.cpp",
            "Core/Protocol/Sync/test_sync_memory_owner.cpp",
            "Core/Protocol/Sync/test_sync_checkpoint_ceiling.cpp",
        ))

    def testEmptyOrDuplicateSetIsRefusedBeforeBuild(self):
        for sources in ((), (gate.TEST_SOURCES[0], gate.TEST_SOURCES[0])):
            with self.subTest(sources=sources), mock.patch.object(gate, "TEST_SOURCES", sources):
                self.assertEqual(self.runGate(), 2)
                self.assertEqual(self.commands, [])

    def testConfigureFailureRunsNeitherBuildNorTests(self):
        with mock.patch.object(gate.subprocess, "run", return_value=SimpleNamespace(returncode=1)) as run:
            self.assertEqual(gate.run_gate(self.build, 2), 1)
        self.assertEqual(run.call_count, 1)

    def testFailedBuildNeverRunsStaleBinaries(self):
        with mock.patch.object(gate.subprocess, "run", side_effect=[SimpleNamespace(returncode=0), SimpleNamespace(returncode=1)]) as run:
            self.assertEqual(gate.run_gate(self.build, 2), 1)
        self.assertEqual(run.call_count, 2)

    def testMissingBinaryFailsBeforeAnyBinaryRuns(self):
        (self.build / "Tests" / Path(gate.TEST_SOURCES[-1]).stem).unlink()
        self.assertEqual(self.runGate(), 1)
        self.assertEqual(len(self.commands), 2)

    def testRedirectedBinaryIsNotExecuted(self):
        binary = self.build / "Tests" / Path(gate.TEST_SOURCES[0]).stem
        binary.unlink()
        binary.symlink_to(self.build / "Tests" / Path(gate.TEST_SOURCES[-1]).stem)
        self.assertEqual(self.runGate(), 1)
        self.assertEqual(len(self.commands), 2)

    def testAnotherCheckoutsCacheIsRefusedAndPreserved(self):
        cache = self.build / "CMakeCache.txt"
        content = f"CMAKE_HOME_DIRECTORY:INTERNAL={self.build}\n"
        cache.write_text(content)
        self.assertEqual(self.runGate(), 2)
        self.assertEqual(self.commands, [])
        self.assertEqual(cache.read_text(), content)

    def testRedirectedParentDirectoryIsNotExecuted(self):
        binaries = self.build / "Tests"
        relocated = self.build / "redirected-tests"
        binaries.rename(relocated)
        binaries.symlink_to(relocated, target_is_directory=True)
        self.assertEqual(self.runGate(), 1)
        self.assertEqual(len(self.commands), 2)

    def testUnreadableOwnerIsNotTreatedAsAnUnusedBuild(self):
        (self.build / "CMakeCache.txt").write_text("CMAKE_BUILD_TYPE:STRING=Debug\n")
        self.assertEqual(self.runGate(), 2)
        self.assertEqual(self.commands, [])

    def testBuildInsideTheCheckoutIsRefusedWithoutCommands(self):
        with mock.patch.object(gate.subprocess, "run") as run:
            self.assertEqual(gate.run_gate(ROOT / "build" / "sync-memory", 2), 2)
        run.assert_not_called()

    def testTestFailurePropagatesAndStopsTheRun(self):
        def fail(arguments, **keywords):
            result = self.command(arguments, **keywords)
            if len(self.commands) == 3:
                result.returncode = 1
            return result
        self.assertEqual(self.runGate(fail), 1)
        self.assertEqual(len(self.commands), 3)

    def testZeroMissingOrDuplicateCaseSummariesCannotPass(self):
        for output in ("", "test-harness: ok (0 cases; 0.0s)\n", "SKIPPED\n", "test-harness: ok (1 case; 0.0s)\n" * 2):
            with self.subTest(output=output):
                self.commands.clear()
                def noCases(arguments, **keywords):
                    result = self.command(arguments, **keywords)
                    result.stdout = output
                    return result
                self.assertEqual(self.runGate(noCases), 1)
                self.assertEqual(len(self.commands), 3)

    def testTimeoutFailsWithoutRunningFurtherBinaries(self):
        def timeout(arguments, **keywords):
            result = self.command(arguments, **keywords)
            if len(self.commands) == 3:
                raise subprocess.TimeoutExpired(arguments, gate.TEST_TIMEOUT_SECONDS)
            return result
        self.assertEqual(self.runGate(timeout), 1)
        self.assertEqual(len(self.commands), 3)

    def testMissingBuildToolIsAnExplicitRefusal(self):
        with mock.patch.object(gate.subprocess, "run", side_effect=FileNotFoundError("cmake")):
            self.assertEqual(gate.run_gate(self.build, 2), 2)

    def testListLaunchesNothing(self):
        with mock.patch.object(gate.subprocess, "run") as run:
            self.assertEqual(gate.main(["--list"]), 0)
        run.assert_not_called()
        self.assertEqual(self.output.getvalue().splitlines(), ["Tests/" + source for source in gate.TEST_SOURCES])

    def testZeroJobsAreRejectedBeforeAnyCommands(self):
        with mock.patch.object(gate.subprocess, "run") as run:
            with self.assertRaises(SystemExit) as exit:
                gate.main(["--jobs", "0"])
        self.assertEqual(exit.exception.code, 2)
        run.assert_not_called()


if __name__ == "__main__":
    unittest.main()
