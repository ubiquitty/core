#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""Negative controls for shared quiet, verbose, and structured reporting."""

from contextlib import redirect_stderr, redirect_stdout
import io
import math
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import patch

from gate_framework import EVENT_SCHEMA, Reporter


def _is_gone(pid):
    """Whether `pid` names no running process, counting a zombie as gone.

    A zombie still answers `os.kill(pid, 0)`: its entry stays until its parent
    reaps it. A descendant orphaned inside a container started without a
    reaping init, such as a plain `docker run`, is reparented to a PID 1 that
    never reaps, and stays a zombie however dead it is. These cases assert that
    a process was ended, so a zombie is gone.
    """
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return True
    except PermissionError:
        return False
    try:
        with open(f"/proc/{pid}/stat", encoding="ascii", errors="replace") as handle:
            # The state follows the parenthesised command name, which may
            # itself hold spaces and parentheses.
            state = handle.read().rpartition(")")[2].split()[0]
    except (OSError, IndexError):
        listed = subprocess.run(["ps", "-o", "stat=", "-p", str(pid)],
                                capture_output=True, text=True, check=False)
        state = listed.stdout.strip()
        if not state:
            return True
    return state.startswith("Z")


class ReporterTests(unittest.TestCase):
    def events(self, path):
        return [json.loads(line) for line in path.read_text().splitlines()]

    def test_quiet_run_keeps_full_log_and_emits_bounded_liveness(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            event_path = root / "events.jsonl"
            log_path = root / "stage.log"
            output = io.StringIO()
            reporter = Reporter(
                "battery", {"UBQ_REPORT_MODE": "quiet"},
                event_path=event_path, heartbeat_seconds=0.05)
            with redirect_stdout(output):
                result = reporter.run_process(
                    ["bash", "-c", "printf 'compiling target\\n'; sleep 0.15; printf 'done\\n'"],
                    label="host tests", log_path=str(log_path))
            self.assertEqual(result.status, 0)
            self.assertEqual(log_path.read_text(), "compiling target\ndone\n")
            self.assertNotIn("\ncompiling target\n", output.getvalue())
            self.assertIn("battery: alive — host tests", output.getvalue())
            # Bounded by what the run took, not by a guess about the runner: the
            # heartbeat is a timer that fires only while the child is alive, so
            # a child that ran for `elapsed` seconds allows about
            # elapsed / heartbeat_seconds beats. A fixed number is the nominal
            # count on an idle machine, and a loaded runner exceeds it by one.
            # This still faults an emitter that spins: the loop waits at least
            # 10ms between turns, so a heartbeat that never re-arms fires
            # every 10ms — about ten beats in this child's run against an
            # allowance of five. The interval is 0.05s and not smaller so that
            # ratio stays wide; at 0.02s a spin lands within one beat of the
            # allowance and the bound catches it only sometimes.
            allowed = math.ceil(result.elapsed_seconds / reporter.heartbeat_seconds) + 1
            self.assertLessEqual(output.getvalue().count("battery: alive"), allowed)
            events = self.events(event_path)
            self.assertTrue(all(event["schema"] == EVENT_SCHEMA for event in events))
            self.assertEqual(events[0]["state"], "running")
            self.assertEqual(events[-1]["state"], "passed")

    def test_verbose_run_streams_child_output(self):
        output = io.StringIO()
        reporter = Reporter("battery", {"UBQ_REPORT_MODE": "verbose"}, heartbeat_seconds=0)
        with redirect_stdout(output):
            result = reporter.run_process(["printf", "visible detail\n"], label="style")
        self.assertEqual(result.status, 0)
        self.assertIn("visible detail", output.getvalue())

    def test_quiet_ci_preserves_child_annotations_without_streaming_prose(self):
        output = io.StringIO()
        errors = io.StringIO()
        reporter = Reporter(
            "battery", {"GITHUB_ACTIONS": "true", "UBQ_REPORT_MODE": "quiet"},
            heartbeat_seconds=0)
        with redirect_stdout(output), redirect_stderr(errors):
            result = reporter.run_process(
                ["bash", "-c", "printf 'ordinary detail\\n::error file=Source/Core/Foo.cpp,line=9::broken\\n'; exit 7"],
                label="style")
        self.assertEqual(result.status, 7)
        self.assertNotIn("ordinary detail", output.getvalue())
        self.assertEqual(
            errors.getvalue().count("::error file=Source/Core/Foo.cpp,line=9::broken"), 1)

    def test_failure_preserves_tail_and_structured_source_finding(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            event_path = root / "events.jsonl"
            log_path = root / "stage.log"
            output = io.StringIO()
            errors = io.StringIO()
            reporter = Reporter(
                "linux-battery", {"UBQ_REPORT_MODE": "quiet"},
                event_path=event_path, heartbeat_seconds=0)
            with redirect_stdout(output), redirect_stderr(errors):
                result = reporter.run_process(
                    ["bash", "-c", "printf 'Source/Core/Foo.cpp:42: error: missing symbol\\n'; exit 7"],
                    label="host tests", log_path=str(log_path), command="./Scripts/run-tests.sh")
            self.assertEqual(result.status, 7)
            self.assertIn("Source/Core/Foo.cpp:42", errors.getvalue())
            findings = [event for event in self.events(event_path) if event["kind"] == "finding"]
            self.assertEqual(len(findings), 1)
            self.assertEqual(findings[0]["file"], "Source/Core/Foo.cpp")
            self.assertEqual(findings[0]["line"], 42)
            self.assertEqual(findings[0]["reason"], "missing symbol")
            self.assertEqual(findings[0]["command"], "./Scripts/run-tests.sh")
            self.assertEqual(findings[0]["exitStatus"], 7)
            self.assertEqual(findings[0]["logPath"], str(log_path))
            self.assertIn("elapsedSeconds", findings[0])
            stages = [event for event in self.events(event_path) if event["kind"] == "stage"]
            self.assertEqual(stages[-1]["reason"], "missing symbol")

    def test_success_does_not_route_expected_failure_shaped_output(self):
        with tempfile.TemporaryDirectory() as directory:
            event_path = Path(directory) / "events.jsonl"
            reporter = Reporter(
                "linux-battery", {"UBQ_REPORT_MODE": "quiet"},
                event_path=event_path, heartbeat_seconds=0)
            with redirect_stdout(io.StringIO()):
                result = reporter.run_process(
                    ["printf", "Source/Core/Foo.cpp:42: error: expected negative control\n"],
                    label="host tests")
            self.assertEqual(result.status, 0)
            self.assertEqual(result.findings, ())
            self.assertFalse(any(
                event["kind"] == "finding" for event in self.events(event_path)))

    def test_indented_linker_findings_are_checkout_portable(self):
        reporter = Reporter("linux-battery", {
            "GITHUB_WORKSPACE": "/data/github-runners/ubiquitty/_work/ubiquitty/ubiquitty",
        })
        finding = reporter.finding_from_line(
            "  /usr/bin/ld.bfd: /data/github-runners/ubiquitty/_work/ubiquitty/ubiquitty/"
            "Source/Core/Rendering/OverlaySpanGather.cpp:19:(.text.symbol): "
            "undefined reference to `Ubiquitty::Screen::ScrollSearch::activeMatch()'")
        self.assertEqual(finding["file"], "Source/Core/Rendering/OverlaySpanGather.cpp")
        self.assertEqual(finding["line"], 19)
        self.assertEqual(finding["severity"], "error")
        self.assertIn("undefined reference", finding["reason"])
        self.assertTrue(finding["rawFile"].startswith("/data/github-runners/"))
        container = reporter.finding_from_line(
            " /src/Source/Core/Foo.cpp:42:7: error: missing symbol")
        self.assertEqual(container["file"], "Source/Core/Foo.cpp")

    def test_file_only_gate_finding_keeps_a_routable_location(self):
        with tempfile.TemporaryDirectory() as directory:
            event_path = Path(directory) / "events.jsonl"
            reporter = Reporter("preflight", {}, event_path=event_path)
            finding = reporter.finding_from_line(
                "Source/Core/Foo.cpp: check-style: generated file is stale")
            self.assertEqual(finding["file"], "Source/Core/Foo.cpp")
            self.assertEqual(finding["location"], "Source/Core/Foo.cpp")
            self.assertNotIn("line", finding)
            reporter.record_output_findings("style", (
                "Source/Core/Foo.cpp: check-style: generated file is stale\n"))
            event = self.events(event_path)[0]
            self.assertEqual(event["location"], "Source/Core/Foo.cpp")
            self.assertEqual(event["gate"], "check-style")
            self.assertNotIn("line", event)

    def test_root_markdown_finding_routes_to_its_file(self):
        reporter = Reporter("preflight", {})
        for document in ("README.md", "third-party-notices.md"):
            finding = reporter.finding_from_line(
                f"{document}: check-folder-map: App/ is drawn as Source/App")
            self.assertEqual(finding["file"], document)
        prose = reporter.finding_from_line(
            "notes md: check-folder-map: App/ is drawn as Source/App")
        self.assertNotIn("file", prose)

    def test_documentation_advisory_does_not_become_a_failed_ci_finding(self):
        with tempfile.TemporaryDirectory() as directory:
            document = Path(directory) / "advisory.md"
            document.write_text("host: an instance that no longer exists\n", encoding="utf-8")
            gate = Path(__file__).resolve().parent / "check-docs.py"
            checked = subprocess.run([sys.executable, str(gate), str(document)],
                                     capture_output=True, text=True)
            self.assertEqual(checked.returncode, 0, checked.stdout + checked.stderr)
            reporter = Reporter("preflight", {})
            findings = reporter.record_output_findings("docs", checked.stdout)
            self.assertEqual(len(findings), 1, checked.stdout)
            self.assertEqual(findings[0]["severity"], "warning")
            document.write_text("TODO: replace this section\n", encoding="utf-8")
            blocked = subprocess.run([sys.executable, str(gate), str(document)],
                                     capture_output=True, text=True)
            self.assertEqual(blocked.returncode, 1, blocked.stdout + blocked.stderr)

    def test_warning_scenario_keeps_location_without_becoming_a_file_or_error(self):
        reporter = Reporter("preflight", {})
        finding = reporter.finding_from_line(
            "known-flaky-scenario: check-ui-scenarios: UNGATED — passed this run")
        self.assertEqual(finding["location"], "known-flaky-scenario")
        self.assertEqual(finding["gate"], "check-ui-scenarios")
        self.assertEqual(finding["severity"], "warning")
        self.assertNotIn("file", finding)

    def test_events_carry_the_exact_github_run_identity(self):
        with tempfile.TemporaryDirectory() as directory:
            event_path = Path(directory) / "events.jsonl"
            reporter = Reporter("battery", {
                "GITHUB_REPOSITORY": "owner/repository",
                "GITHUB_WORKFLOW": "CI",
                "GITHUB_JOB": "linux",
                "GITHUB_RUN_ID": "123",
                "GITHUB_RUN_ATTEMPT": "2",
                "GITHUB_SHA": "deadbeef",
                "GITHUB_REF": "refs/pull/9/merge",
            }, event_path=event_path)
            reporter.record("stage", label="tests", state="failed")
            self.assertEqual(self.events(event_path)[0]["github"], {
                "repository": "owner/repository",
                "workflow": "CI",
                "job": "linux",
                "runId": "123",
                "runAttempt": "2",
                "sha": "deadbeef",
                "ref": "refs/pull/9/merge",
            })

    def test_environment_selects_quiet_ci_and_verbose_local(self):
        self.assertEqual(Reporter("gate", {"GITHUB_ACTIONS": "true"}).mode, "quiet")
        self.assertEqual(Reporter("gate", {}).mode, "verbose")
        self.assertEqual(Reporter("gate", {
            "GITHUB_ACTIONS": "true", "UBQ_REPORT_MODE": "verbose"}).mode, "verbose")

    def test_required_log_failure_never_starts_or_passes_the_child(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            marker = root / "started"
            reporter = Reporter("battery", {"UBQ_REPORT_MODE": "quiet"})
            with redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()):
                result = reporter.run_process(
                    ["touch", str(marker)], label="stage", log_path=str(root))
            self.assertEqual(result.status, 73)
            self.assertFalse(marker.exists())

    def test_mid_run_log_failure_stops_the_child_and_returns_a_verdict(self):
        class BrokenLog:
            def write(self, _chunk):
                raise OSError("disk full")

            def flush(self):
                pass

            def close(self):
                pass

        with tempfile.TemporaryDirectory() as directory:
            log_path = Path(directory) / "stage.log"
            real_open = Path.open

            def open_path(path, *args, **kwargs):
                if path == log_path:
                    return BrokenLog()
                return real_open(path, *args, **kwargs)

            reporter = Reporter("battery", {"UBQ_REPORT_MODE": "quiet"}, heartbeat_seconds=0)
            with patch.object(Path, "open", open_path), redirect_stdout(io.StringIO()), \
                    redirect_stderr(io.StringIO()):
                result = reporter.run_process(
                    ["bash", "-c", "printf output; sleep 10"],
                    label="stage", log_path=str(log_path))
            self.assertEqual(result.status, 74)
            self.assertIn("could not preserve required log", result.tail[-1])

    def test_output_read_failure_stops_the_child_and_fails_the_log_contract(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            ready = root / "ready"
            pid_path = root / "pid"
            child_code = (
                "import os,pathlib,signal,time\n"
                "signal.signal(signal.SIGTERM, signal.SIG_IGN)\n"
                f"pathlib.Path({str(pid_path)!r}).write_text(str(os.getpid()))\n"
                f"pathlib.Path({str(ready)!r}).write_text('ready')\n"
                "while True: time.sleep(1)\n")

            def read_failure(*_arguments):
                deadline = time.monotonic() + 5
                while not ready.exists() and time.monotonic() < deadline:
                    time.sleep(0.01)
                raise OSError("read failed")

            reporter = Reporter("battery", {"UBQ_REPORT_MODE": "quiet"}, heartbeat_seconds=0)
            with patch("gate_framework._readProcessOutput", side_effect=read_failure), \
                    redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()):
                result = reporter.run_process(
                    [sys.executable, "-c", child_code], label="stage",
                    log_path=str(root / "stage.log"))
            self.assertEqual(result.status, 74)
            self.assertIn("could not preserve required log", result.tail[-1])
            self.assertTrue(_is_gone(int(pid_path.read_text())))

    def test_descendant_cannot_hold_the_output_pipe_open_after_its_parent_exits(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            pid_path = root / "descendant-pid"
            descendant_code = (
                "import signal,time\n"
                "signal.signal(signal.SIGTERM, signal.SIG_IGN)\n"
                "time.sleep(60)\n")
            parent_code = (
                "import pathlib,subprocess,sys\n"
                f"child=subprocess.Popen([sys.executable, '-c', {descendant_code!r}])\n"
                f"pathlib.Path({str(pid_path)!r}).write_text(str(child.pid))\n")
            reporter = Reporter("battery", {
                "UBQ_REPORT_MODE": "quiet",
                "UBQ_REPORT_DRAIN_SECONDS": "0.1",
                "UBQ_REPORT_DEPTH": "20",
            }, heartbeat_seconds=0)
            began = time.monotonic()
            with redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()):
                result = reporter.run_process(
                    [sys.executable, "-c", parent_code], label="stage",
                    log_path=str(root / "stage.log"))
            self.assertEqual(result.status, 74)
            self.assertLess(time.monotonic() - began, 2)
            self.assertIn("descendant kept the output pipe open", result.tail[-1])
            self.assertTrue(_is_gone(int(pid_path.read_text())))

    def test_signal_cancels_nested_reporter_and_its_grandchild(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            ready = root / "grandchild-ready"
            child_pid = root / "grandchild-pid"
            child_code = (
                "import os,pathlib,signal,time\n"
                f"ready=pathlib.Path({str(ready)!r})\n"
                f"child_pid=pathlib.Path({str(child_pid)!r})\n"
                "signal.signal(signal.SIGTERM, signal.SIG_IGN)\n"
                "child_pid.write_text(str(os.getpid()))\n"
                "ready.write_text('ready')\n"
                "while True: time.sleep(1)\n")
            nested_code = (
                "import os,sys\n"
                "from gate_framework import Reporter\n"
                f"Reporter('nested', os.environ, heartbeat_seconds=0).run_process("
                f"[sys.executable, '-c', {child_code!r}], label='child', "
                f"log_path={str(root / 'nested.log')!r})\n")
            driver_code = (
                "import os,sys\n"
                "from gate_framework import Reporter\n"
                f"Reporter('outer', os.environ, heartbeat_seconds=0).run_process("
                f"[sys.executable, '-c', {nested_code!r}], label='nested', "
                f"log_path={str(root / 'outer.log')!r})\n")
            parent_events = root / "parent-events.jsonl"
            inherited_environment = dict(
                os.environ,
                UBQ_BATTERY="preflight",
                UBQ_EVENT_LOG=str(parent_events),
                UBQ_RESULT_DIR=str(root),
                UBQ_STAGE_KEY="orchestration-tests",
                UBQ_STAGE_LABEL="CI orchestration tests",
                UBQ_STAGE_LOG_PATH=str(root / "parent-stage.log"),
            )
            environment = {
                key: value for key, value in inherited_environment.items()
                if key not in {
                    "UBQ_BATTERY", "UBQ_EVENT_LOG", "UBQ_RESULT_DIR",
                    "UBQ_STAGE_KEY", "UBQ_STAGE_LABEL", "UBQ_STAGE_LOG_PATH",
                    "UBQ_STAGE_JUNIT_PATH",
                }
            }
            environment["UBQ_REPORT_MODE"] = "quiet"
            process = subprocess.Popen(
                [sys.executable, "-c", driver_code], cwd=Path(__file__).resolve().parent,
                env=environment, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            try:
                deadline = time.monotonic() + 5
                while not ready.exists() and time.monotonic() < deadline:
                    time.sleep(0.02)
                self.assertTrue(ready.exists(), "grandchild never became ready")
                process.send_signal(signal.SIGTERM)
                process.wait(timeout=8)
                pid = int(child_pid.read_text())
                deadline = time.monotonic() + 2
                while time.monotonic() < deadline and not _is_gone(pid):
                    time.sleep(0.02)
                self.assertEqual(process.returncode, 128 + signal.SIGTERM)
                self.assertFalse(parent_events.exists())
                self.assertTrue(_is_gone(pid))
            finally:
                if process.poll() is None:
                    process.kill()
                    process.wait()

    def test_signal_during_process_spawn_cannot_orphan_the_child(self):
        real_popen = subprocess.Popen
        spawned = []
        installed_handlers = {}

        def install_handler(signum, handler):
            previous = installed_handlers.get(signum, signal.getsignal(signum))
            installed_handlers[signum] = handler
            return previous

        def spawn_then_cancel(*arguments, **keywords):
            process = real_popen(*arguments, **keywords)
            spawned.append(process.pid)
            installed_handlers[signal.SIGTERM](signal.SIGTERM, None)
            return process

        child_code = (
            "import signal,time\n"
            "signal.signal(signal.SIGTERM, signal.SIG_IGN)\n"
            "while True: time.sleep(1)\n")
        reporter = Reporter("battery", {"UBQ_REPORT_MODE": "quiet"}, heartbeat_seconds=0)
        with patch("gate_framework.signal.signal", side_effect=install_handler), \
                patch("gate_framework.subprocess.Popen", side_effect=spawn_then_cancel), \
                redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()), \
                self.assertRaises(SystemExit) as stopped:
            reporter.run_process(
                [sys.executable, "-c", child_code], label="stage")
        self.assertEqual(stopped.exception.code, 128 + signal.SIGTERM)
        self.assertEqual(len(spawned), 1)
        self.assertTrue(_is_gone(spawned[0]))


if __name__ == "__main__":
    unittest.main()
