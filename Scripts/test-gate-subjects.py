#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0
"""Exercises probe ownership, process isolation, discovery, and refusal controls."""

from contextlib import redirect_stderr, redirect_stdout
import importlib.util
import io
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import patch

import gate_subjects


FAKE_GATE = r'''
import json
import os
from pathlib import Path
import signal
import sys
import time
import gate_subjects

root = Path.cwd()
row = next(row for row in gate_subjects.rows() if row.name == sys.argv[1])
paths = sorted(path for directory in (root / "Scripts", root / "Tests/Core")
               for path in directory.glob("*zz-gate-subject-*")
               if "caller-" not in path.name)
if row.name == "format-code.sh" and paths and paths[0].suffix == ".swift":
    row = gate_subjects.rows()[-1]
mode = os.environ.get("PROBE_TEST_MODE", "good")
with (root / ".git/events").open("a") as stream:
    stream.write(json.dumps({"row": row.name, "paths": [str(path) for path in paths],
                            "parent": os.getppid()}) + "\n")
if mode == "wait" and paths:
    ready = root / ".git/ready"
    temporary = ready.with_suffix(".tmp")
    temporary.write_text(str(os.getpid()))
    temporary.replace(ready)
    time.sleep(30)
time.sleep(float(os.environ.get("PROBE_TEST_DELAY", "0")))
if mode == "crash":
    os.kill(os.getpid(), signal.SIGTERM)
if mode == "broken":
    print("tool execution failed")
    sys.exit(127)
if mode == "constant":
    print("zz_gate_subject_probe.cpp ZZGateSubjectProbe.swift zz-gate-subject-probe.sh")
    sys.exit(1)
if mode == "empty":
    sys.exit(0)
failed = False
for path in paths:
    if mode == "tracked-only":
        continue
    violated = path.read_text() != row.clean
    if row.registered:
        violated = not list((root / "Scripts").glob("zz-gate-subject-caller-*"))
    if violated or mode in ("all-files", "tool-error-as-drift"):
        if mode == "long-before":
            print("x" * 1000000)
        if mode != "status-only":
            print(path.name)
        if mode == "long-after":
            print("x" * 1000000)
        failed = True
if os.environ.get("PROBE_TEST_BASELINE"):
    print("Tests/Core/existing.cpp: unrelated baseline finding")
    failed = True
sys.exit(0 if mode == "wrong-status" else int(failed))
'''


class Fixture:
    def __init__(self, base):
        self.root = Path(base).resolve()
        (self.root / "Scripts").mkdir()
        (self.root / "Tests/Core").mkdir(parents=True)
        self.environment = {key: value for key, value in os.environ.items()
                            if not key.startswith("GIT_") and not key.startswith("UBIQUITTY_CHECK_GATE_SUBJECTS")}
        for name in ("check-gate-subjects.sh", "gate_subjects.py", "gate_report.py"):
            shutil.copy2(Path(__file__).parent / name, self.root / "Scripts" / name)
        (self.root / "Scripts/fake_subject_gate.py").write_text(FAKE_GATE)
        for row in gate_subjects.rows():
            if row.swift:
                continue
            path = self.root / row.selector
            if path.suffix == ".sh":
                path.write_text(f'#!/usr/bin/env bash\nexec "{sys.executable}" Scripts/fake_subject_gate.py "{row.name}"\n')
            else:
                path.write_text("import runpy, sys\nsys.argv = ['fake_subject_gate.py', " + repr(row.name)
                                + "]\nrunpy.run_path('Scripts/fake_subject_gate.py', run_name='__main__')\n")
            path.chmod(0o755)
        self.git("init", "-q")
        self.git("add", ".")
        tree = self.git("write-tree").strip()
        commit = self.git("-c", "user.name=probe", "-c", "user.email=probe@example.invalid",
                          "commit-tree", tree, "-m", "fixture").strip()
        self.git("update-ref", "HEAD", commit)
        self.git("update-ref", "refs/remotes/origin/main", commit)
        tools = self.root / ".git/tools"
        tools.mkdir()
        for name in ("xcrun", "swift-format"):
            path = tools / name
            path.write_text("#!/bin/sh\n" + (f"printf '%s\\n' '{tools / 'swift-format'}'\n" if name == "xcrun" else "exit 0\n"))
            path.chmod(0o755)
        self.environment["PATH"] = str(tools) + os.pathsep + self.environment["PATH"]

    def git(self, *arguments):
        return subprocess.run(["git", *arguments], cwd=self.root, env=self.environment,
                              capture_output=True, text=True, check=True).stdout

    def process(self, **environment):
        return subprocess.Popen(["bash", "Scripts/check-gate-subjects.sh"], cwd=self.root,
                                env={**self.environment, **environment}, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, text=True)

    def run(self, **environment):
        process = self.process(**environment)
        output, _ = process.communicate(timeout=30)
        return process.returncode, output

    def events(self):
        path = self.root / ".git/events"
        return [json.loads(line) for line in path.read_text().splitlines()] if path.exists() else []

    def change(self, name):
        path = self.root / "Scripts" / name
        with path.open("a") as stream:
            stream.write("\n# selector changed\n")

    def assert_clean(self, test):
        for directory in ("Scripts", "Tests/Core"):
            test.assertEqual(list((self.root / directory).glob("*zz-gate-subject-*")), [])


class GateSubjectTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="gate-subject-controls-")
        self.addCleanup(self.temporary.cleanup)
        self.fixture = Fixture(self.temporary.name)

    def assert_run(self, success, **environment):
        status, output = self.fixture.run(**environment)
        self.assertEqual(status == 0, success, output)
        self.fixture.assert_clean(self)
        return output

    def test_full_seven_rows_have_three_measurements(self):
        self.assertIn("7 selector(s) proved", self.assert_run(True, UBIQUITTY_CHECK_GATE_SUBJECTS_ALL="1"))
        self.assertEqual(len(self.fixture.events()), 21)

    def test_unchanged_tree_does_not_run_selectors(self):
        self.assertIn("0 selector(s) proved, 7 explicitly skipped", self.assert_run(True))
        self.assertEqual(self.fixture.events(), [])

    def test_each_selector_activates_its_rows(self):
        for name, count in (("check-slop.py", 3), ("check-comments.sh", 3), ("format-code.sh", 6),
                            ("check-shell.sh", 3), ("check-gates-invoked.py", 3), ("check-personal-paths.py", 3)):
            with self.subTest(name=name), tempfile.TemporaryDirectory(prefix="gate-selector-") as temporary:
                fixture = Fixture(temporary)
                fixture.change(name)
                status, output = fixture.run()
                self.assertEqual(status, 0, output)
                self.assertEqual(len(fixture.events()), count)

    def test_shared_sources_activate_every_row(self):
        for name in ("check-gate-subjects.sh", "gate_subjects.py", "test-gate-subjects.py",
                     "gate_corpus.py", "gate-report-lib.sh", "gate_report.py", "check-style.py",
                     "ubq_roots.py", "style_support.py"):
            with self.subTest(name=name), tempfile.TemporaryDirectory(prefix="gate-shared-") as temporary:
                fixture = Fixture(temporary)
                fixture.change(name)
                status, output = fixture.run()
                self.assertEqual(status, 0, output)
                self.assertEqual(len(fixture.events()), 21)

    def test_missing_base_runs_every_row(self):
        self.fixture.git("update-ref", "-d", "refs/remotes/origin/main")
        output = self.assert_run(True)
        self.assertIn("scope unavailable; probing every selector", output)
        self.assertEqual(len(self.fixture.events()), 21)

    def test_inherited_hook_git_context_does_not_redirect_probes(self):
        self.assert_run(True, GIT_DIR="/not-a-git-directory", GIT_WORK_TREE="/not-a-worktree",
                        GIT_INDEX_FILE="/not-an-index", UBIQUITTY_CHECK_GATE_SUBJECTS_ALL="1")

    def test_all_rows_reject_constant_output(self):
        for row in gate_subjects.rows():
            with self.subTest(row=row.name):
                probes = gate_subjects.Probes(self.fixture.root, {**self.fixture.environment, "PROBE_TEST_MODE": "constant"})
                try:
                    with self.assertRaises(gate_subjects.ProbeError):
                        probes.probe(row)
                finally:
                    self.assertEqual(probes.cleanup(), [])

    def test_false_green_controls(self):
        self.fixture.change("check-slop.py")
        for mode in ("empty", "tracked-only", "all-files", "tool-error-as-drift", "status-only",
                     "wrong-status", "broken", "crash"):
            with self.subTest(mode=mode):
                self.assert_run(False, PROBE_TEST_MODE=mode)

    def test_full_output_is_drained_before_matching(self):
        self.fixture.change("check-slop.py")
        for mode in ("long-before", "long-after"):
            with self.subTest(mode=mode):
                self.assert_run(True, PROBE_TEST_MODE=mode)

    def test_unrelated_baseline_findings_are_allowed(self):
        self.assert_run(True, PROBE_TEST_BASELINE="1", UBIQUITTY_CHECK_GATE_SUBJECTS_ALL="1")

    def test_missing_gate_fails(self):
        (self.fixture.root / "Scripts/check-slop.py").unlink()
        self.assert_run(False)

    def test_missing_git_fails_before_planting(self):
        probes = gate_subjects.Probes(self.fixture.root, {"PATH": "/nonexistent"})
        with redirect_stderr(io.StringIO()), redirect_stdout(io.StringIO()):
            self.assertNotEqual(gate_subjects.execute(probes), 0)
        self.fixture.assert_clean(self)

    def test_scope_command_errors_cannot_skip_all_rows(self):
        probes = gate_subjects.Probes(self.fixture.root, {})
        for index in range(4):
            with self.subTest(index=index):
                responses = [(0, "")] * index + [(128, "broken repository")]
                with patch.object(probes, "command", side_effect=responses), redirect_stderr(io.StringIO()):
                    self.assertIsNone(probes.changed())

    def test_each_git_scope_source_is_included(self):
        probes = gate_subjects.Probes(self.fixture.root, {})
        for index in range(4):
            with self.subTest(index=index):
                responses = [(0, "")] * 4
                responses[index] = (0, "Scripts/check-shell.sh\0")
                with patch.object(probes, "command", side_effect=responses):
                    self.assertIn("Scripts/check-shell.sh", probes.changed())

    def test_legacy_probe_paths_are_preserved(self):
        paths = ("Tests/Core/zz_gate_subject_probe.cpp", "Tests/Core/ZZGateSubjectProbe.swift",
                 "Scripts/zz-gate-subject-probe.sh", "Scripts/" + "check-zzprobe.sh")
        for relative in paths:
            path = self.fixture.root / relative
            path.write_bytes(b"original\x00contents")
        probes = gate_subjects.Probes(self.fixture.root, self.fixture.environment)
        with patch.object(probes, "measure"):
            for row in gate_subjects.rows():
                probes.probe(row)
        self.assertEqual(probes.cleanup(), [])
        for relative in paths:
            self.assertEqual((self.fixture.root / relative).read_bytes(), b"original\x00contents")

    def test_exclusive_creation_preserves_existing_file_and_symlink(self):
        probes = gate_subjects.Probes(self.fixture.root)
        original = self.fixture.root / "Tests/Core/original.cpp"
        original.write_bytes(b"original")
        link = self.fixture.root / "Tests/Core/link.cpp"
        link.symlink_to(original)
        for path in (original, link):
            with self.subTest(path=path), self.assertRaises(FileExistsError):
                probes.create(path.relative_to(self.fixture.root), "replacement")
        self.assertEqual(probes.cleanup(), [])
        self.assertEqual(original.read_bytes(), b"original")
        self.assertTrue(link.is_symlink())

    def test_replaced_probe_is_not_overwritten_or_deleted(self):
        probes = gate_subjects.Probes(self.fixture.root)
        path = probes.create("Tests/Core/probe.cpp", "owned")
        path.unlink()
        original = self.fixture.root / "Tests/Core/original.cpp"
        original.write_text("original")
        path.symlink_to(original)
        with self.assertRaises(gate_subjects.ProbeError):
            probes.write(path, "violation")
        self.assertEqual(len(probes.cleanup()), 1)
        self.assertTrue(path.is_symlink())
        self.assertEqual(original.read_text(), "original")

    def test_symlinked_parent_and_lock_are_refused(self):
        probes = gate_subjects.Probes(self.fixture.root)
        parent = self.fixture.root / "Linked"
        parent.symlink_to(self.fixture.root / "Tests/Core", target_is_directory=True)
        with self.assertRaises(gate_subjects.ProbeError):
            probes.create("Linked/probe.cpp", "violation")
        original = self.fixture.root / ".git/original"
        original.write_text("original")
        (self.fixture.root / ".git/ubiquitty-gate-subjects.lock").symlink_to(original)
        with self.assertRaises(OSError):
            probes.lock()
        self.assertEqual(probes.cleanup(), [])
        self.assertEqual(original.read_text(), "original")

    def test_creation_write_failure_still_cleans_owned_path(self):
        probes = gate_subjects.Probes(self.fixture.root)
        with patch.object(probes, "write", side_effect=OSError("write failed")):
            with self.assertRaises(OSError):
                probes.create("Tests/Core/probe.cpp", "data")
        self.assertEqual(probes.cleanup(), [])
        self.assertFalse((self.fixture.root / "Tests/Core/probe.cpp").exists())

    def test_registration_creation_failure_cleans_the_probe(self):
        probes = gate_subjects.Probes(self.fixture.root)
        original_create = probes.create

        def create(relative, content):
            if "caller-" in relative:
                raise OSError("caller creation failed")
            return original_create(relative, content)

        with patch.object(probes, "create", side_effect=create), patch.object(probes, "measure"):
            with self.assertRaises(OSError):
                probes.probe(next(row for row in gate_subjects.rows() if row.registered))
        self.assertEqual(probes.cleanup(), [])
        self.fixture.assert_clean(self)

    def test_cleanup_failure_does_not_stop_other_owned_cleanup(self):
        probes = gate_subjects.Probes(self.fixture.root)
        first = probes.create("Tests/Core/first.cpp", "owned")
        second = probes.create("Tests/Core/second.cpp", "owned")
        original_remove = probes.remove

        def remove(path):
            if path == first:
                raise PermissionError("unlink refused")
            original_remove(path)

        with patch.object(probes, "remove", side_effect=remove):
            self.assertEqual(probes.cleanup(), ["unlink refused"])
        self.assertTrue(first.exists())
        self.assertFalse(second.exists())

    def test_cleanup_failure_changes_success_but_preserves_failure(self):
        for initial in (0, 1, 2, 143):
            with self.subTest(initial=initial):
                probes = gate_subjects.Probes(self.fixture.root)
                errors = io.StringIO()
                with patch.object(probes, "run", return_value=initial), \
                        patch.object(probes, "cleanup", return_value=["unlink refused"]), \
                        redirect_stderr(errors):
                    self.assertEqual(gate_subjects.execute(probes), initial or 2)
                self.assertIn("FAILED — 1 cleanup failure(s): unlink refused", errors.getvalue())

    def test_every_measurement_failure_cleans_every_row(self):
        for row in gate_subjects.rows():
            for phase in ("absent", "valid", "violating"):
                with self.subTest(row=row.name, phase=phase):
                    probes = gate_subjects.Probes(self.fixture.root)

                    def measure(_row, _path, actual, _expected):
                        if actual == phase:
                            raise gate_subjects.ProbeError("injected measurement failure")

                    with patch.object(probes, "measure", side_effect=measure):
                        with self.assertRaises(gate_subjects.ProbeError):
                            probes.probe(row)
                    self.assertEqual(probes.cleanup(), [])
                    self.fixture.assert_clean(self)

    def test_absent_arm_rejects_the_specific_probe_name(self):
        probes = gate_subjects.Probes(self.fixture.root)
        path = self.fixture.root / "Tests/Core/probe.cpp"
        with patch.object(probes, "command", return_value=(1, str(path))):
            with self.assertRaises(gate_subjects.ProbeError):
                probes.measure(gate_subjects.rows()[0], path, "absent", False)

    def test_missing_swift_is_explicit_and_broken_resolution_fails(self):
        probes = gate_subjects.Probes(self.fixture.root, {})
        with patch("gate_subjects.shutil.which", return_value=None):
            self.assertFalse(probes.swift_available())
        for status in (1, 72, 127):
            with self.subTest(status=status), patch("gate_subjects.shutil.which", return_value="/fake/xcrun"), \
                    patch.object(probes, "command", return_value=(status, "cannot execute")):
                with self.assertRaises(gate_subjects.ProbeError):
                    probes.swift_available()
        with patch("gate_subjects.shutil.which", side_effect=["/fake/xcrun", None]), \
                patch.object(probes, "command", return_value=(72, 'xcrun: error: unable to find utility "swift-format"')):
            self.assertFalse(probes.swift_available())
        with patch("gate_subjects.shutil.which", return_value="/fake/xcrun"), \
                patch.object(probes, "command", return_value=(0, "/nonexistent/swift-format")):
            with self.assertRaises(gate_subjects.ProbeError):
                probes.swift_available()
        with patch.object(probes, "lock"), patch.object(probes, "changed", return_value=None), \
                patch.object(probes, "probe"), patch.object(probes, "swift_available", return_value=False), \
                redirect_stdout(io.StringIO()) as output:
            self.assertEqual(probes.run(), 0)
        self.assertIn("SKIPPED", output.getvalue())
        self.assertIn("6 selector(s) proved, 1 explicitly skipped", output.getvalue())

    def test_concurrent_invocations_are_serialized_for_all_measurements(self):
        processes = [self.fixture.process(UBIQUITTY_CHECK_GATE_SUBJECTS_ALL="1", PROBE_TEST_DELAY="0.02")
                     for _ in range(3)]
        try:
            for process in processes:
                output, _ = process.communicate(timeout=30)
                self.assertEqual(process.returncode, 0, output)
        finally:
            for process in processes:
                if process.poll() is None:
                    process.terminate()
                    process.communicate(timeout=5)
        parents = [event["parent"] for event in self.fixture.events()]
        blocks = [parent for index, parent in enumerate(parents) if index == 0 or parent != parents[index - 1]]
        self.assertEqual(len(blocks), 3, parents)
        self.assertEqual(len(parents), 63)
        self.fixture.assert_clean(self)

    def test_signals_cleanup_and_release_the_lock(self):
        self.fixture.change("check-slop.py")
        for number in (signal.SIGHUP, signal.SIGINT, signal.SIGTERM):
            with self.subTest(number=number):
                ready = self.fixture.root / ".git/ready"
                ready.unlink(missing_ok=True)
                process = self.fixture.process(PROBE_TEST_MODE="wait")
                try:
                    deadline = time.monotonic() + 5
                    while not ready.exists() and time.monotonic() < deadline:
                        time.sleep(0.01)
                    self.assertTrue(ready.exists())
                    child = int(ready.read_text())
                    process.send_signal(number)
                    output, _ = process.communicate(timeout=5)
                    self.assertEqual(process.returncode, 128 + number, output)
                    with self.assertRaises(ProcessLookupError):
                        os.kill(child, 0)
                    self.fixture.assert_clean(self)
                    self.assert_run(True)
                finally:
                    if process.poll() is None:
                        process.terminate()
                        process.communicate(timeout=5)

    def test_timeout_stops_owned_process_and_preserves_nonzero(self):
        probes = gate_subjects.Probes(self.fixture.root)
        with self.assertRaises(subprocess.TimeoutExpired):
            probes.command((sys.executable, "-c", "import time; time.sleep(30)"), timeout=0.05)

    def test_abnormal_measurement_exits_cannot_be_findings(self):
        probes = gate_subjects.Probes(self.fixture.root)
        path = self.fixture.root / "Tests/Core/probe.cpp"
        for status in (-15, 2, 126, 127):
            with self.subTest(status=status), patch.object(probes, "command", return_value=(status, str(path))):
                with self.assertRaises(gate_subjects.ProbeError):
                    probes.measure(gate_subjects.rows()[0], path, "violating", True)

    def test_self_test_discovery_and_ratchet(self):
        path = Path(__file__).parent / "run-gate-self-tests.py"
        specification = importlib.util.spec_from_file_location("gate_self_tests", path)
        module = importlib.util.module_from_spec(specification)
        specification.loader.exec_module(module)
        self.assertIn(("check-gate-subjects.sh", "--self-test"), module.discover())
        uncovered = len(module.gates()) - len(module.discover())
        self.assertEqual(uncovered, module.ubq_roots.ledger(module.REPO)[0]["gate-self-tests-uncovered"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
