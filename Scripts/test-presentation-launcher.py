#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""Presentation launcher exit, environment, and owned-job cleanup contracts."""

import importlib.util
import io
import os
from pathlib import Path
import plistlib
import signal
import subprocess
import sys
import tempfile
import time
import unittest
from contextlib import redirect_stdout, redirect_stderr
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("presentation_launcher", Path(__file__).with_name("run-presentation-test.py"))
launcher = importlib.util.module_from_spec(spec)
spec.loader.exec_module(launcher)


class PresentationLauncherTests(unittest.TestCase):
    def setUp(self):
        self.child = None
        self.calls = []
        self.job = None

    def launchctl(self, arguments):
        self.calls.append(arguments)
        if arguments[0] == "bootstrap":
            self.job = plistlib.loads(Path(arguments[2]).read_bytes())
            self.output = open(self.job["StandardOutPath"], "w")
            self.errors = open(self.job["StandardErrorPath"], "w")
            self.input = open(self.job["StandardInPath"], "rb")
            self.child = subprocess.Popen(self.job["ProgramArguments"], cwd=self.job["WorkingDirectory"],
                                          env=self.job["EnvironmentVariables"], stdout=self.output,
                                          stderr=self.errors, stdin=self.input, start_new_session=True)
        else:
            if self.child is not None:
                if self.child.poll() is None:
                    os.killpg(self.child.pid, signal.SIGTERM)
                self.child.wait(timeout=5)
                self.output.close()
                self.errors.close()
                self.input.close()
        return subprocess.CompletedProcess(arguments, 0, "", "")

    def run_command(self, code, *arguments):
        with patch.object(launcher, "launchctl", side_effect=self.launchctl), \
                redirect_stdout(io.StringIO()) as output, redirect_stderr(io.StringIO()) as errors:
            status = launcher.run_interactive([sys.executable, "-c", code, *arguments])
        self.assertEqual(self.calls[-1], ["bootout", "gui/" + str(os.getuid()) + "/" + self.job["Label"]])
        self.assertIsNotNone(self.child.poll())
        self.assertFalse(Path(self.job["StandardOutPath"]).parent.exists())
        return status, output.getvalue(), errors.getvalue()

    def test_status_output_and_spaced_arguments_survive_the_worker(self):
        status, output, errors = self.run_command("import sys; print(sys.argv[1]); print('diagnostic',file=sys.stderr); sys.exit(23)", "path with spaces")
        self.assertEqual(status, 23)
        self.assertIn("path with spaces", output)
        self.assertIn("diagnostic", errors)
        self.assertEqual(self.job["ProcessType"], "Interactive")

    def test_skip_status_is_not_changed_to_success(self):
        status, output, errors = self.run_command("import sys; sys.exit(77)")
        self.assertEqual(status, 77)

    def test_instrumentation_survives_but_credentials_are_not_copied(self):
        with patch.dict(os.environ, {"LLVM_PROFILE_FILE": "fixture-%p.profraw", "GITHUB_TOKEN": "fixture-secret"}):
            status, output, errors = self.run_command("import os; print(os.environ['LLVM_PROFILE_FILE']); assert 'GITHUB_TOKEN' not in os.environ")
        self.assertEqual(status, 0)
        self.assertIn("fixture-%p.profraw", output)
        self.assertNotIn("GITHUB_TOKEN", self.job["EnvironmentVariables"])

    def test_bootstrap_failure_still_removes_only_its_owned_job(self):
        calls = []
        def failed(arguments):
            calls.append(arguments)
            return subprocess.CompletedProcess(arguments, 5, "", "bootstrap refused")
        with patch.object(launcher, "launchctl", side_effect=failed):
            with self.assertRaisesRegex(RuntimeError, "bootstrap refused"):
                launcher.run_interactive(["missing-test"])
        job = Path(calls[0][2])
        self.assertFalse(job.exists())
        self.assertEqual(calls[-1][0], "bootout")
        self.assertIn("org.ubiquitty.presentation-test.", calls[-1][1])

    def test_interruption_ends_the_owned_worker_and_removes_its_files(self):
        with patch.object(launcher, "launchctl", side_effect=self.launchctl), \
                patch.object(launcher.time, "sleep", side_effect=SystemExit(143)), \
                redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()):
            with self.assertRaises(SystemExit) as stopped:
                launcher.run_interactive([sys.executable, "-c", "import time; time.sleep(60)"])
        self.assertEqual(stopped.exception.code, 143)
        self.assertIsNotNone(self.child.poll())
        self.assertEqual(self.calls[-1][0], "bootout")
        self.assertFalse(Path(self.job["StandardOutPath"]).parent.exists())

    def test_worker_ends_its_test_when_the_caller_lifeline_closes(self):
        with tempfile.TemporaryDirectory() as directory:
            result = Path(directory) / "result.json"
            process = subprocess.Popen([sys.executable, str(Path(launcher.__file__)), "--worker", str(result),
                                        sys.executable, "-c", "import time; time.sleep(60)"],
                                       stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            try:
                for attempt in range(100):
                    if result.with_suffix(".started").exists():
                        break
                    time.sleep(.02)
                self.assertTrue(result.with_suffix(".started").exists())
                process.stdin.close()
                process.wait(timeout=5)
                self.assertEqual(process.returncode, 125)
                self.assertIn(b"caller exited", process.stderr.read())
            finally:
                if process.poll() is None:
                    process.kill()
                    process.wait()
                process.stdout.close()
                process.stderr.close()


if __name__ == "__main__":
    unittest.main()
