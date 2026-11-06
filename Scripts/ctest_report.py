#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""Runs CTest with quiet liveness, JUnit results, and durable diagnostics."""

import argparse
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import time
import xml.etree.ElementTree as ElementTree

sys.path.insert(0, str(Path(__file__).resolve().parent))
import gate_report
from gate_framework import Reporter


class CTestActivity:
    """Tracks CTest's native start/completion lines for bounded heartbeats."""

    start_pattern = re.compile(r"^\s*Start\s+[0-9]+:\s+(.+?)\s*$")
    finish_pattern = re.compile(
        r"^\s*[0-9]+/[0-9]+\s+Test\s+#[0-9]+:\s+(\S+).*?"
        r"(?:Passed|\*\*\*.+?)\s+[0-9.]+\s+sec\s*$")

    def __init__(self, total):
        self.total = total
        self.completed = 0
        self.active = []

    def __call__(self, line):
        started = self.start_pattern.match(line)
        if started:
            name = started.group(1)
            if name not in self.active:
                self.active.append(name)
            return self.finish()
        finished = self.finish_pattern.match(line)
        if finished:
            name = finished.group(1)
            if name in self.active:
                self.active.remove(name)
            self.completed += 1
            return self.finish()
        return ""

    def finish(self):
        active = ", ".join(self.active[:3])
        if len(self.active) > 3:
            active += f" (+{len(self.active) - 3} more)"
        suffix = f"; active {active}" if active else "; waiting for CTest"
        return f"{self.completed}/{self.total} completed{suffix}"


def selection_arguments(arguments):
    """Returns only arguments that affect CTest discovery."""
    with_value = {
        "-R", "--tests-regex", "-E", "--exclude-regex", "-L", "--label-regex",
        "-LE", "--label-exclude", "-I", "--tests-information",
        "-FA", "--fixture-exclude-any", "-FS", "--fixture-exclude-setup",
        "-FC", "--fixture-exclude-cleanup", "--tests-from-file",
        "--exclude-from-file",
    }
    without_value = {"--rerun-failed", "-U", "--union"}
    selected = []
    index = 0
    while index < len(arguments):
        argument = arguments[index]
        if argument in without_value:
            selected.append(argument)
            index += 1
            continue
        if argument in with_value and index + 1 < len(arguments):
            selected.extend((argument, arguments[index + 1]))
            index += 2
            continue
        if any(argument.startswith(name + "=") for name in with_value if name.startswith("--")):
            selected.append(argument)
        elif any(argument.startswith(prefix) for prefix in ("-R", "-E", "-L", "-LE", "-I")):
            selected.append(argument)
        index += 1
    return selected


def discover(build, arguments, environment):
    command = ["ctest", "--test-dir", str(build), "--show-only=json-v1",
               *selection_arguments(arguments)]
    finished = subprocess.run(command, capture_output=True, text=True,
                              check=False, env=environment, cwd=build)
    if finished.returncode != 0:
        raise RuntimeError(f"CTest discovery failed: {finished.stderr.strip() or finished.stdout.strip()}")
    # The first JSON document on stdout, not the whole of stdout. Anything the
    # toolchain prints after it — a CMake deprecation notice, a linker warning
    # that arrives late — is trailing text, and reading the stream as one
    # document turns that into "invalid JSON" and stops the run with a message
    # about the parser rather than about what printed.
    #
    # The trailing text is reported rather than dropped: it is the evidence for
    # whatever wrote it, and a discovery that silently tolerated anything would
    # be the next thing nobody can diagnose.
    try:
        payload, consumed = json.JSONDecoder().raw_decode(finished.stdout.lstrip())
    except json.JSONDecodeError as error:
        raise RuntimeError(f"CTest discovery returned invalid JSON: {error}") from error
    trailing = finished.stdout.lstrip()[consumed:].strip()
    if trailing:
        print(f"ctest: discovery read its JSON and ignored {len(trailing)} trailing byte(s) on stdout: "
              f"{trailing[:200]!r}", file=sys.stderr)
    tests = payload.get("tests", [])
    if not tests:
        raise RuntimeError("CTest selected no tests; an empty suite is not a pass")
    return [entry["name"] for entry in tests]


def supports_junit(environment):
    finished = subprocess.run(["ctest", "--help"], capture_output=True, text=True,
                              check=False, env=environment)
    return finished.returncode == 0 and "--output-junit" in finished.stdout


def failure_location(text, reporter=None):
    for candidate in text.splitlines():
        matched = re.match(
            r"^\s*([^:\n]+):([0-9]+)(?::[0-9]+)?: "
            r"test-harness: assertion failed \u2014 (.+)$", candidate)
        if matched:
            source = matched.group(1)
            if reporter:
                source = reporter.portable_source_path(source)
            return source, int(matched.group(2)), matched.group(3)[:500]
        finding = reporter.finding_from_line(candidate) if reporter else None
        if finding:
            return finding.get("file", ""), finding.get("line"), finding["reason"][:500]
    return "", None, text.strip().split("\n", 1)[0][:500]


def read_junit(path):
    root = ElementTree.parse(path).getroot()
    return root.findall(".//testcase") if root.tag != "testcase" else [root]


def portable_build_path(build, environment):
    root = environment.get("UBQ_BUILD_ROOT")
    if root:
        try:
            relative = build.resolve().relative_to(Path(root).resolve())
            return f'"$(python3 Scripts/build_root.py .)/{relative}"'
        except ValueError:
            pass
    workspace = environment.get("GITHUB_WORKSPACE")
    if workspace:
        try:
            return shlex.quote(str(build.resolve().relative_to(Path(workspace).resolve())))
        except ValueError:
            pass
    return shlex.quote(str(build))


def reproduce_command(build, name, environment):
    if name.startswith("shell_"):
        return f"./Scripts/run-tests.sh {shlex.quote(name.removeprefix('shell_'))}"
    return (f"ctest --test-dir {portable_build_path(build, environment)} -R "
            f"{shlex.quote('^' + re.escape(name) + '$')} --output-on-failure")


def record_results(reporter, build, junit_path, discovered, status, log_path,
                   environment):
    selected = set(discovered)
    executed = set()
    failures = []
    for case in read_junit(junit_path):
        name = case.get("name", "unnamed")
        if name not in selected:
            raise ValueError(f"CTest JUnit contains unselected test: {name}")
        executed.add(name)
        failure = case.find("failure")
        error = case.find("error")
        skipped = case.find("skipped")
        problem = failure if failure is not None else error
        if problem is not None:
            state = "failed"
            body = "\n".join(part for part in (
                problem.text, problem.get("message"), case.findtext("system-out"),
                case.findtext("system-err")) if part).strip()
            if not body:
                body = "CTest reported a failure"
            failures.append((name, body))
        elif skipped is not None or case.get("status") in ("disabled", "notrun", "skipped"):
            state = "skipped"
            body = skipped.get("message") if skipped is not None else "CTest skipped the test"
        else:
            state = "passed"
            body = ""
        reproduce = reproduce_command(build, name, environment)
        source, line, reason = failure_location(body, reporter) if body else ("", None, "")
        failed_cases = re.findall(r"^test-case: FAILED \u2014 (.+?) \(", body, re.M)
        fatal_cases = re.findall(r"^test-harness: FATAL \u2014 signal in case (.+)$", body, re.M)
        case_names = list(dict.fromkeys(failed_cases + fatal_cases))
        exit_status = 0 if state == "passed" else None
        reporter.record("test", label=name, state=state,
                        elapsed_seconds=float(case.get("time", "0") or 0),
                        test=name, file=source, line=line, reason=reason,
                        command=reproduce, exitStatus=exit_status,
                        suiteExitStatus=status,
                        testCases=case_names,
                        shellStanza=name.removeprefix("shell_") if name.startswith("shell_") else "",
                        logPath=str(log_path),
                        logArtifactPath=reporter.artifact_path(log_path),
                        junitPath=str(junit_path),
                        junitArtifactPath=reporter.artifact_path(junit_path))

    not_run = [name for name in discovered if name not in executed]
    for name in not_run:
        reporter.record("test", label=name, state="unexecuted", test=name,
                        reason="CTest stopped before this test started",
                        command=reproduce_command(build, name, environment),
                        suiteExitStatus=status, logPath=str(log_path),
                        logArtifactPath=reporter.artifact_path(log_path),
                        junitPath=str(junit_path),
                        junitArtifactPath=reporter.artifact_path(junit_path))
    return failures, not_run


def result_paths(build, environment):
    configured = environment.get("UBQ_RESULT_DIR")
    root = Path(configured) if configured else build / "Testing" / "ubiquitty-results"
    root.mkdir(parents=True, exist_ok=True)
    safe_name = re.sub(r"[^A-Za-z0-9_.-]+", "-", "-".join(build.parts[-2:]))
    return root / f"ctest-{safe_name}.log", root / f"ctest-{safe_name}.xml"


def runtime_arguments(arguments):
    """Removes caller-owned paths so discovery and execution share one tree."""
    result = []
    index = 0
    while index < len(arguments):
        argument = arguments[index]
        if argument in ("--output-junit", "--test-dir"):
            index += 2
            continue
        if argument.startswith("--output-junit=") or argument.startswith("--test-dir="):
            index += 1
            continue
        result.append(argument)
        index += 1
    return result


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", required=True, type=Path)
    parser.add_argument("arguments", nargs=argparse.REMAINDER)
    options = parser.parse_args(argv)
    options.build = options.build.resolve()
    arguments = options.arguments[1:] if options.arguments[:1] == ["--"] else options.arguments
    environment = dict(os.environ)
    reporter = Reporter("ctest", environment)
    log_path, junit_path = result_paths(options.build, environment)
    suite_began = time.monotonic()

    try:
        discovered = discover(options.build, arguments, environment)
    except (OSError, RuntimeError) as error:
        gate_report.failed("ctest", str(error))
        reporter.record(
            "suite", label=str(options.build), state="failed", reason=str(error),
            elapsed_seconds=time.monotonic() - suite_began, exitStatus=2,
            command=f"ctest --test-dir {portable_build_path(options.build, environment)} "
                    "--show-only=json-v1")
        return 2
    if not supports_junit(environment):
        reason = "CTest 3.21 or newer with --output-junit is required for structured results"
        gate_report.failed("ctest", reason)
        reporter.record(
            "suite", label=str(options.build), state="failed", reason=reason,
            elapsed_seconds=time.monotonic() - suite_began, exitStatus=2,
            command="ctest --help")
        return 2

    run_arguments = runtime_arguments(arguments)
    command = ["ctest", "--test-dir", str(options.build), *run_arguments]
    if "--no-tests=error" not in run_arguments:
        command.append("--no-tests=error")
    if "--output-on-failure" not in run_arguments:
        command.append("--output-on-failure")
    command.extend(("--output-junit", str(junit_path)))
    try:
        junit_path.unlink(missing_ok=True)
    except OSError as error:
        reason = f"could not remove stale CTest JUnit result at {junit_path}: {error}"
        gate_report.failed("ctest", reason)
        reporter.record("suite", label=str(options.build), state="failed", reason=reason,
                        elapsed_seconds=time.monotonic() - suite_began,
                        command=" ".join(shlex.quote(str(argument)) for argument in command),
                        exitStatus=73, logPath=str(log_path), junitPath=str(junit_path))
        return 73
    activity = CTestActivity(len(discovered))
    result = reporter.run_process(
        command, label=f"{len(discovered)} tests in {options.build.name}",
        cwd=options.build, environment=environment, log_path=str(log_path),
        junit_path=str(junit_path), activity=activity)

    if not junit_path.is_file():
        reason = f"CTest produced no JUnit result at {junit_path}"
        gate_report.failed("ctest", reason)
        reporter.record("suite", label=str(options.build), state="failed", reason=reason,
                        elapsed_seconds=result.elapsed_seconds, exitStatus=result.status,
                        command=" ".join(shlex.quote(str(argument)) for argument in command),
                        logPath=str(log_path), junitPath=str(junit_path))
        return result.status or 2

    try:
        failures, not_run = record_results(
            reporter, options.build, junit_path, discovered, result.status, log_path,
            environment)
    except (ElementTree.ParseError, OSError, ValueError) as error:
        reason = f"could not read CTest JUnit result: {error}"
        gate_report.failed("ctest", reason)
        reporter.record("suite", label=str(options.build), state="failed", reason=reason,
                        elapsed_seconds=result.elapsed_seconds, exitStatus=result.status,
                        command=" ".join(shlex.quote(str(argument)) for argument in command),
                        logPath=str(log_path), junitPath=str(junit_path))
        return result.status or 2

    if result.status == 0 and (failures or not_run):
        reason = ("CTest returned success with failed or unexecuted tests: "
                  + ", ".join(name for name, _ in failures) + (
                      (", " if failures and not_run else "") + ", ".join(not_run)))
        gate_report.failed("ctest", reason)
        reporter.record(
            "suite", label=str(options.build), state="failed", reason=reason,
            elapsed_seconds=result.elapsed_seconds, exitStatus=2,
            command=" ".join(shlex.quote(str(argument)) for argument in command),
            logPath=str(log_path), logArtifactPath=reporter.artifact_path(log_path),
            junitPath=str(junit_path),
            junitArtifactPath=reporter.artifact_path(junit_path))
        return 2

    if failures:
        names = [name for name, _ in failures]
        gate_report.failed("ctest", "tests failed", names)
        for name, body in failures:
            source, line, reason = failure_location(body, reporter)
            location = f"{source}:{line}" if source and line else name
            reproduce = reproduce_command(options.build, name, environment)
            gate_report.finding(location, "ctest", f"{reason}; reproduce: {reproduce}")
        print(f"ctest: rerun — ctest --test-dir {portable_build_path(options.build, environment)} "
              "--rerun-failed --output-on-failure")
    if not_run and result.status != 0:
        print(f"ctest: unexecuted — {len(not_run)} test(s) did not start after fail-fast")
    reporter.record(
        "suite", label=str(options.build),
        state="passed" if result.status == 0 else "failed",
        elapsed_seconds=result.elapsed_seconds, exitStatus=result.status,
        tests=len(discovered), failures=len(failures), unexecuted=len(not_run),
        reason=(failure_location(failures[0][1], reporter)[2] if failures
                else reporter.failure_reason(result.tail) if result.status else ""),
        command=" ".join(shlex.quote(str(argument)) for argument in command),
        logPath=str(log_path), logArtifactPath=reporter.artifact_path(log_path),
        junitPath=str(junit_path),
        junitArtifactPath=reporter.artifact_path(junit_path))
    if result.status == 0 and environment.get("GITHUB_ACTIONS") \
            and environment.get("UBQ_RETAIN_PASSED_LOGS") != "1":
        try:
            log_path.unlink()
        except OSError:
            pass
    return result.status


if __name__ == "__main__":
    sys.exit(main())
