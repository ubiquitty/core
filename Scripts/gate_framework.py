#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""gate_framework — one shape for a gate: what it examines, what it found, how long it took.

`gate_report` fixes the wording of a verdict line. It does not fix who may print
one, when, or at what level, so every gate carries its own control flow and its
own idea of what a non-blocking remark looks like. This fixes the rest: a gate
subclasses `Gate`, writes `examine`, and returns a verdict. The start line, the
finish line, the elapsed time and the level come from here.

The timing does not ride on the verdict line. `Scripts/
gate_verdict.py` runs each gate with stderr merged into stdout and caches the
whole of it on success, replaying that text the next time the tree is unchanged.
A duration written into that stream would be cached with it, so a gate that did
not run would report the time of one that did — a measurement that is wrong
precisely when it is cheapest to believe. Durations go to the sink named by
UBQ_GATE_LOG instead, which is written only by a real run.

So the duration does not belong on the verdict line however much better it would
read there: a cached value that cannot say it is cached is worse than no value.
Anything else derived per run — a count of what was walked, a machine's name —
carries the same hazard and belongs in the sink for the same reason.

The console output is byte-identical to what `gate_report` already prints, so a
port changes what a gate is, not what a reader sees.
"""

from collections import deque
from dataclasses import dataclass
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import queue
import re
import shlex
import signal
import subprocess
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gate_report
import ubq_roots


EVENT_SCHEMA = "ubiquitty.ci-event.v1"


@dataclass(frozen=True)
class CommandResult:
    """The complete result of one reported child process."""

    status: int
    elapsed_seconds: float
    log_path: str
    tail: tuple
    findings: tuple


class Reporter:
    """Runs commands with one quiet/verbose console and JSON event contract.

    CI keeps the complete byte stream in `log_path`, while the console carries
    stage boundaries, bounded heartbeats, and actionable failure context. Local
    runs are verbose by default. `UBQ_REPORT_MODE=quiet|verbose` overrides that
    choice, and `UBQ_EVENT_LOG` names the append-only event stream automation
    consumes.
    """

    _compilerFinding = re.compile(
        r"^\s*(?P<file>[^:\n]+):(?P<line>[0-9]+)(?::[0-9]+)?: "
        r"(?P<severity>fatal error|error|warning): (?P<reason>.+)$")
    _gateFinding = re.compile(
        r"^\s*(?P<file>[^:\n]+):(?P<line>[0-9]+): "
        r"(?P<gate>[a-zA-Z0-9_.-]+): (?P<reason>.+)$")
    _gateLocationFinding = re.compile(
        r"^\s*(?P<location>[^:\n]+): "
        r"(?P<gate>[a-zA-Z0-9_.-]+): (?P<reason>.+)$")
    _linkerFinding = re.compile(
        r"^\s*(?:\S*ld(?:\.bfd)?:\s+)?(?P<file>[^:\n]+):(?P<line>[0-9]+):"
        r"(?:\(.+\):\s*)?(?P<reason>(?:undefined reference|multiple definition) .+)$")

    def __init__(self, name, environment=None, *, event_path=None, mode=None,
                 heartbeat_seconds=None):
        self.name = name
        self.environment = dict(os.environ if environment is None else environment)
        self.mode = mode or self.environment.get("UBQ_REPORT_MODE") or (
            "quiet" if self.environment.get("GITHUB_ACTIONS") else "verbose")
        if self.mode not in ("quiet", "verbose"):
            raise ValueError("UBQ_REPORT_MODE must be quiet or verbose")
        configured = self.environment.get("UBQ_REPORT_HEARTBEAT_SECONDS", "60")
        self.heartbeat_seconds = float(configured) if heartbeat_seconds is None else heartbeat_seconds
        self.output_drain_seconds = max(
            0.1, float(self.environment.get("UBQ_REPORT_DRAIN_SECONDS", "5")))
        try:
            self.process_depth = max(0, int(self.environment.get("UBQ_REPORT_DEPTH", "0")))
        except ValueError:
            self.process_depth = 0
        self.termination_grace_seconds = max(0.25, 3.0 / (self.process_depth + 1))
        self.event_path = str(event_path or self.environment.get("UBQ_EVENT_LOG", ""))
        self.github_context = _githubContext(self.environment)

    def record(self, kind, *, label, state=None, elapsed_seconds=None, **fields):
        """Appends one versioned event. Missing sinks never change a verdict."""
        if not self.event_path:
            return
        entry = {
            "schema": EVENT_SCHEMA,
            "at": datetime.now(timezone.utc).isoformat(timespec="milliseconds"),
            "source": self.name,
            "kind": kind,
            "label": label,
        }
        if state is not None:
            entry["state"] = state
        if elapsed_seconds is not None:
            entry["elapsedSeconds"] = round(elapsed_seconds, 4)
        if self.github_context:
            entry["github"] = self.github_context
        for key, variable in (("battery", "UBQ_BATTERY"),
                              ("stageKey", "UBQ_STAGE_KEY"),
                              ("stageLabel", "UBQ_STAGE_LABEL")):
            if self.environment.get(variable):
                entry[key] = self.environment[variable]
        inherited_log = self.environment.get("UBQ_STAGE_LOG_PATH", "")
        inherited_junit = self.environment.get("UBQ_STAGE_JUNIT_PATH", "")
        if inherited_log and not fields.get("logPath"):
            fields["logPath"] = inherited_log
            fields["logArtifactPath"] = self.artifact_path(inherited_log)
        if inherited_junit and not fields.get("junitPath"):
            fields["junitPath"] = inherited_junit
            fields["junitArtifactPath"] = self.artifact_path(inherited_junit)
        for path_field, artifact_field in (
                ("logPath", "logArtifactPath"),
                ("junitPath", "junitArtifactPath")):
            if fields.get(path_field) and not fields.get(artifact_field):
                fields[artifact_field] = self.artifact_path(fields[path_field])
        entry.update({key: value for key, value in fields.items() if value not in (None, "", [])})
        try:
            path = Path(self.event_path)
            path.parent.mkdir(parents=True, exist_ok=True)
            with path.open("a", encoding="utf-8") as handle:
                handle.write(json.dumps(entry, sort_keys=True) + "\n")
        except OSError:
            pass

    def start(self, label, *, command="", log_path="", junit_path=""):
        print(f"{self.name}: start — {label}", flush=True)
        self.record("stage", label=label, state="running", command=command,
                    logPath=log_path, logArtifactPath=self.artifact_path(log_path),
                    junitPath=junit_path, junitArtifactPath=self.artifact_path(junit_path))

    def alive(self, label, elapsed_seconds, activity=""):
        detail = f"; {activity}" if activity else ""
        print(f"{self.name}: alive — {label}; {elapsed_seconds:.1f}s{detail}", flush=True)
        self.record("heartbeat", label=label, state="running",
                    elapsed_seconds=elapsed_seconds, activity=activity)

    def finish(self, label, status, elapsed_seconds, *, command="", log_path="",
               junit_path="", findings=(), reason=""):
        state = "passed" if status == 0 else "failed"
        self.record("stage", label=label, state=state, elapsed_seconds=elapsed_seconds,
                    command=command, exitStatus=status, logPath=log_path,
                    logArtifactPath=self.artifact_path(log_path), junitPath=junit_path,
                    junitArtifactPath=self.artifact_path(junit_path), reason=reason,
                    findings=findings)
        for finding in findings:
            self.record("finding", label=label, state=finding["severity"],
                        elapsed_seconds=elapsed_seconds,
                        location=finding.get("location"),
                        file=finding.get("file"), line=finding.get("line"),
                        gate=finding.get("gate"),
                        rawFile=finding.get("rawFile"),
                        reason=finding["reason"], command=command,
                        exitStatus=status, logPath=log_path,
                        logArtifactPath=self.artifact_path(log_path),
                        junitPath=junit_path,
                        junitArtifactPath=self.artifact_path(junit_path))
        if status == 0:
            print(f"{self.name}: ok — {label} ({elapsed_seconds:.1f}s)", flush=True)
            return
        detail = f"{label}; exit {status}; {elapsed_seconds:.1f}s"
        if log_path:
            detail += f"; log {log_path}"
        gate_report.failed(self.name, detail)

    def finding_from_line(self, line):
        """Parses compiler or gate-style source findings into artifact fields."""
        matched = self._compilerFinding.match(line)
        if matched:
            finding = matched.groupdict()
        else:
            matched = self._gateFinding.match(line)
            if not matched:
                matched = self._linkerFinding.match(line)
                if not matched:
                    matched = self._gateLocationFinding.match(line)
                    if not matched:
                        return None
                    finding = matched.groupdict()
                    raw_location = finding["location"]
                    location = self.portable_source_path(raw_location)
                    finding["location"] = location
                    finding["severity"] = (
                        "warning" if finding["reason"].startswith("UNGATED —") else "error")
                    if _isSourceLocation(raw_location):
                        finding["file"] = location
                        if location != raw_location:
                            finding["rawFile"] = raw_location
                    return finding
            finding = matched.groupdict()
            finding["severity"] = "error"
        finding["line"] = int(finding["line"])
        raw_file = finding["file"]
        finding["file"] = self.portable_source_path(raw_file)
        finding["location"] = f'{finding["file"]}:{finding["line"]}'
        if finding["file"] != raw_file:
            finding["rawFile"] = raw_file
        return finding

    def portable_source_path(self, path):
        """Maps runner and container source paths back to the checkout."""
        source = Path(path)
        workspace = self.environment.get("GITHUB_WORKSPACE")
        if workspace and source.is_absolute():
            try:
                return str(source.resolve().relative_to(Path(workspace).resolve()))
            except ValueError:
                pass
        if path.startswith("/src/"):
            return path.removeprefix("/src/")
        return path

    def artifact_path(self, path):
        """Returns a path relative to the uploaded result root when possible."""
        root = self.environment.get("UBQ_RESULT_DIR")
        if not root or not path:
            return ""
        try:
            return str(Path(path).resolve().relative_to(Path(root).resolve()))
        except ValueError:
            return ""

    def record_output_findings(self, label, output, *, command="", status=1,
                               log_path="", junit_path="", elapsed_seconds=None):
        """Records every source finding already captured by another runner."""
        text = output.decode("utf-8", "replace") if isinstance(output, bytes) else output
        found = []
        for line in text.splitlines():
            finding = self.finding_from_line(line)
            if not finding:
                continue
            found.append(finding)
            self.record("finding", label=label, state=finding["severity"],
                        elapsed_seconds=elapsed_seconds,
                        location=finding.get("location"),
                        file=finding.get("file"), line=finding.get("line"),
                        gate=finding.get("gate"),
                        rawFile=finding.get("rawFile"),
                        reason=finding["reason"], command=command,
                        exitStatus=status, logPath=log_path,
                        logArtifactPath=self.artifact_path(log_path),
                        junitPath=junit_path,
                        junitArtifactPath=self.artifact_path(junit_path))
        return found

    def failure_reason(self, output):
        """Extracts one bounded reason from captured command output."""
        text = output.decode("utf-8", "replace") if isinstance(output, bytes) else output
        lines = deque((line for line in text.splitlines() if line), maxlen=80)
        findings = [finding for line in lines
                    if (finding := self.finding_from_line(line))]
        return _failureReason(lines, findings)

    def run_process(self, arguments, *, label, cwd=None, environment=None,
                    log_path="", command="", junit_path="", activity=None):
        """Runs and drains one child, preserving all bytes and bounded liveness.

        `activity` receives each decoded line and may return a concise current
        activity string for the next heartbeat. It may also be an object with a
        `finish()` method; CTest uses that to describe its active set.
        """
        child_environment = dict(self.environment if environment is None else environment)
        child_environment["UBQ_REPORT_DEPTH"] = str(self.process_depth + 1)
        if log_path:
            child_environment["UBQ_STAGE_LOG_PATH"] = str(log_path)
        if junit_path:
            child_environment["UBQ_STAGE_JUNIT_PATH"] = str(junit_path)
        rendered_command = command or _renderCommand(arguments)
        self.start(label, command=rendered_command, log_path=log_path,
                   junit_path=junit_path)
        began = time.monotonic()
        tail = deque(maxlen=80)
        findings = []
        latest_activity = ""
        line_buffer = ""
        path = Path(log_path) if log_path else None
        try:
            if path:
                path.parent.mkdir(parents=True, exist_ok=True)
            log_handle = path.open("wb") if path else None
        except OSError as error:
            elapsed = time.monotonic() - began
            line = f"could not open required log {path}: {error}"
            print(f"{self.name}: {line}", file=sys.stderr)
            self.finish(label, 73, elapsed, command=rendered_command,
                        log_path=log_path, junit_path=junit_path, reason=line)
            return CommandResult(73, elapsed, log_path, (line,), ())

        stream_error = None
        process = None
        interrupted_signal = None
        previous_handlers = {}

        def interrupt(signum, _frame):
            nonlocal interrupted_signal
            interrupted_signal = signum
            if process is not None:
                _stopProcessGroup(process, self.termination_grace_seconds)
                raise _ReporterSignal(signum)

        main_thread = threading.current_thread() is threading.main_thread()
        if main_thread:
            managed_signals = (signal.SIGINT, signal.SIGTERM)
            for signum in managed_signals:
                previous_handlers[signum] = signal.getsignal(signum)
                signal.signal(signum, interrupt)
        try:
            process = subprocess.Popen(
                arguments, cwd=cwd, env=child_environment, stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT, start_new_session=True)
        except OSError as error:
            for signum, previous in previous_handlers.items():
                signal.signal(signum, previous)
            if log_handle:
                try:
                    log_handle.close()
                except OSError:
                    pass
            elapsed = time.monotonic() - began
            line = f"could not start: {error}"
            print(f"{self.name}: {line}", file=sys.stderr)
            self.finish(label, 127, elapsed, command=rendered_command,
                        log_path=log_path, junit_path=junit_path, reason=line)
            return CommandResult(127, elapsed, log_path, (line,), ())
        except BaseException:
            for signum, previous in previous_handlers.items():
                signal.signal(signum, previous)
            if log_handle:
                try:
                    log_handle.close()
                except OSError:
                    pass
            raise

        chunks = queue.Queue()
        output_descriptor = process.stdout.fileno()

        def drain():
            try:
                while True:
                    chunk = _readProcessOutput(output_descriptor, 65536)
                    if not chunk:
                        break
                    chunks.put(chunk)
            except OSError as error:
                chunks.put(error)
            finally:
                chunks.put(None)

        reader = threading.Thread(target=drain, name=f"{self.name}-output")
        reader.start()
        next_heartbeat = began + self.heartbeat_seconds
        stream_done = False
        child_exit_observed = None
        no_chunk = object()
        def accept_line(line):
            nonlocal latest_activity
            stripped = line.rstrip("\r")
            if stripped:
                tail.append(stripped)
                if self.mode == "quiet" and self.environment.get("GITHUB_ACTIONS") \
                        and _isWorkflowAnnotation(stripped):
                    print(stripped, file=sys.stderr, flush=True)
                finding = self.finding_from_line(stripped)
                if finding:
                    findings.append(finding)
                if activity:
                    reported = activity(stripped)
                    if reported:
                        latest_activity = reported
                elif len(stripped) <= 240:
                    latest_activity = stripped

        try:
            if interrupted_signal is not None:
                interrupt(interrupted_signal, None)
            while not stream_done:
                now = time.monotonic()
                timeout = max(0.01, next_heartbeat - now) if self.heartbeat_seconds > 0 else 0.1
                if child_exit_observed is not None:
                    timeout = min(timeout, max(
                        0.01, child_exit_observed + self.output_drain_seconds - now))
                try:
                    chunk = chunks.get(timeout=timeout)
                except queue.Empty:
                    chunk = no_chunk
                if chunk is None:
                    stream_done = True
                elif isinstance(chunk, OSError):
                    raise chunk
                elif chunk is not no_chunk:
                    if log_handle:
                        log_handle.write(chunk)
                        log_handle.flush()
                    decoded = chunk.decode("utf-8", "replace")
                    if self.mode == "verbose":
                        sys.stdout.write(decoded)
                        sys.stdout.flush()
                    line_buffer += decoded
                    lines = line_buffer.split("\n")
                    line_buffer = lines.pop()
                    for line in lines:
                        accept_line(line)
                now = time.monotonic()
                direct_status = process.poll()
                if direct_status is not None and child_exit_observed is None:
                    child_exit_observed = now
                elif child_exit_observed is not None \
                        and now >= child_exit_observed + self.output_drain_seconds:
                    stream_error = OSError(
                        "child exited but a descendant kept the output pipe open")
                    _stopProcessGroup(process, self.termination_grace_seconds)
                if self.heartbeat_seconds > 0 and now >= next_heartbeat and direct_status is None:
                    current = activity.finish() if activity and hasattr(activity, "finish") else latest_activity
                    self.alive(label, now - began, current)
                    next_heartbeat = now + self.heartbeat_seconds
            if line_buffer:
                accept_line(line_buffer)
            status = process.wait()
            if stream_error is not None:
                status = 74
        except OSError as error:
            stream_error = error
            _stopProcessGroup(process, self.termination_grace_seconds)
            status = 74
        except _ReporterSignal as interruption:
            status = 128 + interruption.signum
            tail.append(f"interrupted by signal {interruption.signum}")
        except BaseException:
            _stopProcessGroup(process, self.termination_grace_seconds)
            raise
        finally:
            for signum, previous in previous_handlers.items():
                signal.signal(signum, previous)
            if log_handle:
                try:
                    log_handle.close()
                except OSError as error:
                    if stream_error is None:
                        stream_error = error
                        status = 74
            try:
                process.stdout.close()
            except (OSError, ValueError) as error:
                if stream_error is None:
                    stream_error = error
                    status = 74
            reader.join()

        elapsed = time.monotonic() - began
        if stream_error is not None:
            line = f"could not preserve required log {path}: {stream_error}"
            tail.append(line)
            print(f"{self.name}: {line}", file=sys.stderr)
        if status != 0 and self.mode == "quiet" and tail:
            print(f"{self.name}: failure tail — {label}", file=sys.stderr)
            for line in tail:
                if not (self.environment.get("GITHUB_ACTIONS")
                        and _isWorkflowAnnotation(line)):
                    print(f"  {line}", file=sys.stderr)
        if status == 0:
            # Successful commands often exercise negative controls whose
            # expected input contains compiler-shaped errors. The command's
            # verdict is authoritative; only warnings remain routable.
            findings = [finding for finding in findings
                        if finding["severity"] == "warning"]
        reason = _failureReason(tail, findings) if status else ""
        self.finish(label, status, elapsed, command=rendered_command,
                    log_path=log_path, junit_path=junit_path, findings=findings,
                    reason=reason)
        if interrupted_signal is not None:
            raise SystemExit(status)
        return CommandResult(status, elapsed, log_path, tuple(tail), tuple(findings))


def _renderCommand(arguments):
    return " ".join(shlex.quote(str(argument)) for argument in arguments)


def _readProcessOutput(descriptor, size):
    return os.read(descriptor, size)


def _isWorkflowAnnotation(line):
    """Keeps actionable child annotations visible through a quiet parent."""
    return line.startswith(("::error ", "::error::", "::warning ",
                            "::warning::", "::notice ", "::notice::"))


def _githubContext(environment):
    """Builds the self-contained workflow/PR identity used for routing."""
    context = {
        "repository": environment.get("GITHUB_REPOSITORY"),
        "workflow": environment.get("GITHUB_WORKFLOW"),
        "job": environment.get("GITHUB_JOB"),
        "eventName": environment.get("GITHUB_EVENT_NAME"),
        "runId": environment.get("GITHUB_RUN_ID"),
        "runAttempt": environment.get("GITHUB_RUN_ATTEMPT"),
        "sha": environment.get("GITHUB_SHA"),
        "ref": environment.get("GITHUB_REF"),
        "headRef": environment.get("GITHUB_HEAD_REF"),
    }
    event_path = environment.get("GITHUB_EVENT_PATH")
    if event_path:
        try:
            payload = json.loads(Path(event_path).read_text(encoding="utf-8"))
            pull_request = payload.get("pull_request") or {}
            head = pull_request.get("head") or {}
            context.update({
                "pullRequest": payload.get("number"),
                "headSha": head.get("sha"),
                "headRef": head.get("ref") or context["headRef"],
            })
        except (OSError, json.JSONDecodeError, AttributeError):
            pass
    return {key: value for key, value in context.items() if value not in (None, "")}


def _isSourceLocation(value):
    """Accepts paths a gate can route without treating prose labels as files."""
    if Path(value).is_absolute():
        return True
    if value == "CMakeLists.txt" or ("/" not in value and value.endswith(".md")):
        return True
    return value.startswith((
        ".github/", "Docs/", "Resources/", "Samples/", "Scripts/", "Source/",
        "Support/", "Tests/", "Tools/", "cmake/"))


def _failureReason(tail, findings):
    """Chooses a bounded human reason without making console prose an API."""
    for finding in findings:
        if finding["severity"] in ("fatal error", "error"):
            return finding["reason"][:500]
    diagnostic = re.compile(
        r"(?:FAILED|REFUSED|BUILD-BROKEN|ERROR|Error|failed)"
        r"(?:\s*[:\u2014-]\s*|:\s*)(.+)")
    for line in reversed(tail):
        matched = diagnostic.search(line)
        if matched:
            return matched.group(1).strip()[:500]
    return tail[-1][:500] if tail else "command exited without diagnostic output"


class _ReporterSignal(BaseException):
    """Carries an external signal through cleanup without losing its status."""

    def __init__(self, signum):
        self.signum = signum


def _stopProcessGroup(process, grace_seconds):
    """Stops the complete process group created for this child."""
    group = process.pid
    try:
        os.killpg(group, signal.SIGTERM)
    except OSError:
        return
    deadline = time.monotonic() + grace_seconds
    while time.monotonic() < deadline:
        if process.poll() is None:
            try:
                process.wait(timeout=0.05)
            except subprocess.TimeoutExpired:
                pass
        if not _processGroupExists(group):
            return
        time.sleep(0.05)
    try:
        os.killpg(group, signal.SIGKILL)
    except OSError:
        pass
    try:
        process.wait(timeout=3)
    except (OSError, subprocess.TimeoutExpired):
        pass


def _processGroupExists(group):
    try:
        os.killpg(group, 0)
        return True
    except ProcessLookupError:
        return False
    except PermissionError:
        return True


class Level:
    """The levels a gate may speak at. Fixed here so a gate cannot invent a fourth kind of remark.

    OK and FAILED are verdicts and end a run. SKIPPED is a verdict too: the gate
    could not answer, which is not the same as answering yes. ADVISORY carries a
    finding that does not block, which several gates already emit in prose and
    no reader can currently filter on. DEBUG and INFO are detail, off unless
    asked for.
    """

    DEBUG = "debug"
    INFO = "info"
    OK = "ok"
    SKIPPED = "skipped"
    ADVISORY = "advisory"
    FAILED = "failed"

    #: Order of increasing consequence, for threshold comparisons.
    ORDER = (DEBUG, INFO, OK, SKIPPED, ADVISORY, FAILED)

    @staticmethod
    def atLeast(level, threshold):
        """Whether `level` is as consequential as `threshold`."""
        return Level.ORDER.index(level) >= Level.ORDER.index(threshold)


class Verdict:
    """What a gate concluded: the level, the exit status, and the line already printed.

    Constructed by the reporting methods on `Gate` rather than directly, so the
    printed line and the level it was printed at cannot disagree.
    """

    def __init__(self, level, status, what):
        self.level = level
        self.status = status
        self.what = what


class Gate:
    """Base for one gate. A subclass sets `name` and writes `examine`.

    The method set is fixed: everything a gate needs to say has a method here,
    and a gate that prints its own verdict line is not using the framework. The
    reporting methods return a `Verdict` so `examine` ends with `return
    self.ok(...)` rather than with a status a reader has to interpret.
    """

    #: The gate's own name, as it appears in its verdict line. Required.
    name = ""

    #: What this gate examines, as a noun phrase reading after "file(s)". Used by `skipped`.
    subject = "this gate reads"

    def __init__(self, argv=None):
        self.argv = list(argv if argv is not None else sys.argv[1:])
        # The tree judged is the repository the gate runs in, whichever checkout
        # holds the framework.
        self.root = ubq_roots.subject_root()
        self._log = _Log(self.name)

    # ---- what a subclass writes -------------------------------------------

    def examine(self):
        """Runs the check and returns a `Verdict` from one of the methods below.

        The one method a gate must write. Raising from here is reported as a
        failure naming the exception, so a gate that breaks does not read as a
        gate that passed.
        """
        raise NotImplementedError(f"{type(self).__name__} must write examine()")

    def selfTest(self):
        """Proves this gate can still fail, and returns a `Verdict`.

        Reached by `--self-test`, which `Scripts/run-gate-self-tests.py`
        discovers. A gate with no self-test is one whose green cannot be told
        from a green it would print over a planted fault.
        """
        return self.skipped("this gate carries no self-test")

    # ---- what a subclass calls --------------------------------------------

    def ok(self, what):
        """The passing verdict, carrying what was examined rather than a bare word."""
        gate_report.ok(self.name, what)
        return Verdict(Level.OK, 0, what)

    def failed(self, what, offenders=()):
        """The failing verdict, naming the first few offenders and counting the rest."""
        gate_report.failed(self.name, what, offenders)
        return Verdict(Level.FAILED, 1, what)

    def failedCount(self, count, what):
        """The failing verdict where offenders were counted while walking, not collected."""
        gate_report.failed_count(self.name, count, what)
        return Verdict(Level.FAILED, 1, f"{count} {what}")

    def skipped(self, why, uncovered=()):
        """The verdict of a gate that could not answer, naming what the silence left uncovered.

        A bare skip reads the same whether the branch touched this gate's subject
        or not, which is the difference that matters to the reader deciding
        whether to build what the gate needs. The shape is fixed here and the
        nouns come from `subject`, so every gate's skip reads alike without every
        gate describing itself in its own words.

        - Parameters:
          - why: What was missing, phrased to read after an em dash.
          - uncovered: The paths this branch changed that the gate would have read.
        """
        print(f"{self.name}: SKIPPED — {why}", file=sys.stderr)
        items = [str(path) for path in uncovered]
        if items:
            print(f"  This branch changes {len(items)} file(s) {self.subject},"
                  " and none of them ran:", file=sys.stderr)
            for path in items:
                print(f"    {path}", file=sys.stderr)
        else:
            print(f"  This branch changes nothing {self.subject},", file=sys.stderr)
            print(f"  so {self.name} had nothing of its to say.", file=sys.stderr)
        return Verdict(Level.SKIPPED, 0, why)

    def advisory(self, what, offenders=()):
        """A finding that does not block, kept apart from the ones that do.

        Several gates carry a standing backlog they report without failing on.
        Printed at its own level so a reader — or a filter — can tell it from the
        verdict rather than inferring from the wording.
        """
        print(f"{self.name}: ADVISORY — {what}", file=sys.stderr)
        for offender in offenders:
            print(f"  {offender}", file=sys.stderr)
        self._log.record(Level.ADVISORY, what)
        return Verdict(Level.ADVISORY, 0, what)

    def finding(self, location, message):
        """One offender, in the shape every compiler emits: `path:line: gate: what`."""
        gate_report.finding(location, self.name, message)

    def detail(self, message, level=Level.INFO):
        """Something worth recording that is not a finding. Off the console unless asked for."""
        if Level.atLeast(level, _consoleThreshold()):
            print(f"{self.name}: {message}", file=sys.stderr)
        self._log.record(level, message)

    # ---- what the framework does ------------------------------------------

    @classmethod
    def main(cls, argv=None):
        """Runs one gate: times it, logs the start and the finish, returns its exit status.

        The whole of a gate's control flow, so no gate writes its own. A gate
        that raises is reported at FAILED naming the exception, because an
        unhandled error is the one outcome a gate must never report as silence.

        - Returns: 0 when the gate passed or stood down, 1 when it failed.
        """
        gate = cls(argv)
        if not gate.name:
            raise ValueError(f"{cls.__name__} must set a name")
        isSelfTest = "--self-test" in gate.argv
        gate._log.record(Level.INFO, "self-test started" if isSelfTest else "started")
        began = time.monotonic()
        try:
            verdict = gate.selfTest() if isSelfTest else gate.examine()
        except Exception as error:  # noqa: BLE001 — a gate that breaks must not read as one that passed
            elapsed = time.monotonic() - began
            gate._log.record(Level.FAILED, f"raised {type(error).__name__}: {error}", elapsed)
            gate_report.failed(gate.name, f"raised {type(error).__name__}: {error}")
            return 1
        elapsed = time.monotonic() - began
        if verdict is None:
            gate._log.record(Level.FAILED, "examine() returned no verdict", elapsed)
            gate_report.failed(gate.name, "examine() returned no verdict")
            return 1
        gate._log.record(verdict.level, verdict.what, elapsed)
        return verdict.status


def _consoleThreshold():
    """The lowest level `detail` prints to the console. INFO and below are silent by default."""
    return os.environ.get("UBQ_GATE_LOG_LEVEL", Level.OK)


class _Log:
    """The structured record of one gate's run, appended as JSON lines.

    Separate from the console so a duration is never cached with a verdict and
    replayed as though it had been measured again. Absent UBQ_GATE_LOG this
    writes nothing, which is what keeps a gate's observable output the same as
    before it was ported.
    """

    def __init__(self, gate):
        self.gate = gate
        self.path = os.environ.get("UBQ_GATE_LOG", "")

    def record(self, level, message, elapsedSeconds=None):
        """Appends one record. A sink that cannot be written is not a gate failure."""
        if not self.path:
            return
        entry = {"gate": self.gate, "level": level, "message": message}
        if elapsedSeconds is not None:
            entry["elapsedSeconds"] = round(elapsedSeconds, 4)
        try:
            with open(self.path, "a", encoding="utf-8") as handle:
                handle.write(json.dumps(entry, sort_keys=True) + "\n")
        except OSError:
            pass
