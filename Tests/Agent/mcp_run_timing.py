#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""Runs commands in a private /bin/sh with no prompt integration.

Usage: mcp_run_timing.py /path/to/ubq /path/to/ubiquitty-host
The command's output, foreground lifetime and deadline are independent checks.
"""

import json
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import tempfile
import time


def call(agent, environment, tool, arguments):
    """One MCP tool answer and its elapsed wall time, including attach."""
    request = {"jsonrpc": "2.0", "id": 1, "method": "tools/call",
               "params": {"name": tool, "arguments": arguments}}
    started = time.monotonic()
    process = subprocess.run([agent, "mcp"], input=json.dumps(request) + "\n",
                             text=True, capture_output=True, env=environment,
                             timeout=2 * arguments.get("timeout_ms", 10000) / 1000 + 5)
    elapsed = time.monotonic() - started
    assert process.returncode == 0, process.stderr
    answers = [json.loads(line) for line in process.stdout.splitlines() if line.strip()]
    answer = next(answer for answer in answers if answer.get("id") == 1)
    assert "result" in answer, answer
    return answer["result"], elapsed


def text(result):
    """The tool's text blocks, retaining line boundaries."""
    return "\n".join(block["text"] for block in result.get("content", [])
                     if block.get("type") == "text")


def cli_run(agent, environment, session, command, timeout):
    """The CLI's documented result, process status and wall time."""
    started = time.monotonic()
    process = subprocess.run([agent, "--json", "run", "-s", f"session:{session}",
                              "--timeout", str(timeout / 1000), "--", command],
                             text=True, capture_output=True, env=environment,
                             timeout=2 * timeout / 1000 + 5)
    return json.loads(process.stdout), process.returncode, time.monotonic() - started


def check_runs(agent, environment, directory):
    """Each case gets a fresh shell, so a timed-out child cannot affect another."""
    delayed = directory / "delayed-output"
    delayed.write_text("#!/bin/sh\nsleep 1\nprintf 'EXTERNAL_%s\\n' finished\n")
    delayed.chmod(0o700)
    marked = directory / "marked-output"
    marked.write_text("#!/bin/sh\nprintf '\\033]133;C\\007MARKED_%s\\n\\033]133;D;7\\007' finished\n")
    marked.chmod(0o700)
    unfinished = directory / "prompt-without-end"
    unfinished.write_text("#!/bin/sh\nprintf '\\033]133;A\\007marked prompt\\n'\n")
    unfinished.chmod(0o700)
    cases = (
        ("builtin", "printf 'BUILTIN_%s\\n' finished", 10000, 0.45, "BUILTIN_finished"),
        ("external", str(delayed), 10000, 1.0, "EXTERNAL_finished"),
        ("deadline", "/bin/sleep 5", 1200, 1.0, None),
        ("marked", str(marked), 10000, 0.0, "MARKED_finished"),
        ("prompt-without-end", str(unfinished), 1200, 1.0, None),
    )
    failures = []
    for name, command, timeout, minimum, marker in cases:
        maximum = timeout / (4000 if name == "builtin" else 2000) if marker else timeout / 1000 + 1
        created, _ = call(agent, environment, "terminal_new", {})
        assert not created.get("isError"), created
        match = re.search(r"session (\d+)", text(created))
        assert match, created
        result, elapsed = call(agent, environment, "terminal_run",
                               {"session": int(match[1]), "command": command, "timeout_ms": timeout})
        if not minimum <= elapsed < maximum:
            failures.append(f"{name}: {elapsed:.3f}s outside [{minimum}, {maximum})")
        if marker is not None:
            if result.get("isError") or marker not in text(result).splitlines():
                failures.append(f"{name}: returned before its output: {result}")
            if name == "marked" and "[exit=7]" not in text(result):
                failures.append(f"{name}: lost the command's explicit completion: {result}")
            elif name != "marked" and "[exit=" in text(result):
                failures.append(f"{name}: invented an exit status without prompt integration")
        else:
            if not result.get("isError"):
                failures.append(f"{name}: reported a running command as complete: {result}")
            expected = f"the command did not complete within {timeout} ms; terminal_screen shows its screen"
            if text(result) != expected:
                failures.append(f"{name}: did not explain its deadline: {result}")
        print(f"mcp-run-timing: {name}: {elapsed:.3f}s", flush=True)

    correlated = directory / "correlated-output"
    correlated.write_text("#!/bin/sh\nprintf '\\033]133;A\\007\\033]133;D;3\\007'\n"
                          "sleep 1\nprintf '\\033]133;C\\007CORRELATED_%s\\n\\033]133;D;7\\007' finished\n")
    correlated.chmod(0o700)
    for name, command, timeout, minimum, marker in (*cases, ("correlated", str(correlated), 10000, 1.0, "CORRELATED_finished")):
        maximum = timeout / (4000 if name == "builtin" else 2000) if marker else timeout / 1000 + 1
        created, _ = call(agent, environment, "terminal_new", {})
        assert not created.get("isError"), created
        match = re.search(r"session (\d+)", text(created))
        assert match, created
        result, status, elapsed = cli_run(agent, environment, int(match[1]), command, timeout)
        if not minimum <= elapsed < maximum:
            failures.append(f"cli {name}: {elapsed:.3f}s outside [{minimum}, {maximum})")
        if marker is None:
            if status != 9 or result.get("error", {}).get("data", {}).get("kind") != "timeout":
                failures.append(f"cli {name}: did not report timeout exit 9: {status}, {result}")
            if "may still be running; ubq read shows its screen" not in result.get("error", {}).get("message", ""):
                failures.append(f"cli {name}: did not explain the running command: {result}")
        else:
            if marker not in result.get("output", "").splitlines():
                failures.append(f"cli {name}: returned before its output: {result}")
            if name in ("marked", "correlated"):
                if status != 7 or result.get("exitStatus") != 7 or result.get("outcome") == "settled":
                    failures.append(f"cli {name}: lost correlated command status: {status}, {result}")
            elif status != 11 or result.get("outcome") != "settled" or "exitStatus" in result:
                failures.append(f"cli {name}: invented command success instead of settled: {status}, {result}")
            if not isinstance(result.get("cursor"), int):
                failures.append(f"cli {name}: omitted its event cursor: {result}")
            validated = subprocess.run([agent, "api", "validate", "run", json.dumps(result)],
                                       text=True, capture_output=True, env=environment, timeout=5)
            if validated.returncode:
                failures.append(f"cli {name}: result violates its published schema: {validated.stdout}")
        print(f"cli-run-timing: {name}: {elapsed:.3f}s, exit {status}", flush=True)
    assert not failures, "\n".join(failures)


def main():
    assert len(sys.argv) == 3, __doc__
    agent, host = (str(Path(argument).resolve()) for argument in sys.argv[1:])
    with tempfile.TemporaryDirectory(prefix="ubq-run-", dir="/tmp") as temporary:
        directory = Path(temporary)
        control = directory / "control"
        control.mkdir(mode=0o700)
        policy = directory / "policy"
        policy.write_text("default allow\n")
        environment = os.environ.copy()
        for name in ("UBIQUITTY_SOCKET", "UBQ_REMOTE_TOKEN", "UBIQUITTY_AGENT_TOKEN",
                     "UBIQUITTY_SESSION", "UBIQUITTY_PANE", "ZDOTDIR", "BASH_ENV"):
            environment.pop(name, None)
        environment.update(UBIQUITTY_HOME=str(control), HOME=str(directory), SHELL="/bin/sh",
                           ENV="/dev/null", PS1="plain> ", UBIQUITTY_HOST_BIN=host,
                           UBIQUITTY_AGENT_POLICY=str(policy))
        try:
            subprocess.run([host, "--ensure-daemon", "--columns", "80", "--rows", "24"],
                           env=environment, check=True, capture_output=True, timeout=15)
            issued = subprocess.run([host, "token", "issue", "--scope", "all", "--label", "run-timing"],
                                    env=environment, check=True, text=True, capture_output=True, timeout=5)
            environment["UBIQUITTY_AGENT_TOKEN"] = issued.stdout.strip().splitlines()[-1]
            check_runs(agent, environment, directory)
        finally:
            for pid_file in control.glob("host*.pid"):
                try:
                    process_id = int(pid_file.read_text().strip())
                    if process_id > 1:
                        os.kill(process_id, signal.SIGTERM)
                        deadline = time.monotonic() + 5
                        while time.monotonic() < deadline:
                            os.kill(process_id, 0)
                            time.sleep(0.05)
                        os.kill(process_id, signal.SIGKILL)
                except (FileNotFoundError, ProcessLookupError):
                    pass


if __name__ == "__main__":
    main()
