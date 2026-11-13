#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""Run a macOS presentation test outside an inherited service QoS clamp."""

import json
import os
from pathlib import Path
import platform
import plistlib
import select
import signal
import subprocess
import sys
import tempfile
import time
import uuid


def launchctl(arguments):
    return subprocess.run(["launchctl", *arguments], capture_output=True, text=True)


def worker(result, command):
    result = Path(result)
    result.with_suffix(".started").write_text(str(os.getpid()))
    child = None
    try:
        child = subprocess.Popen(command, stdin=subprocess.DEVNULL)
        while child.poll() is None:
            readable, _, _ = select.select([sys.stdin], [], [], .05)
            if readable and not os.read(sys.stdin.fileno(), 1):
                print("presentation-test: caller exited before test completion", file=sys.stderr)
                return 125
        status = child.returncode
        status = status if status >= 0 else 128 - status
    except OSError as error:
        print(f"presentation-test: {error}", file=sys.stderr)
        status = 127
    finally:
        if child is not None and child.poll() is None:
            child.terminate()
            try:
                child.wait(timeout=2)
            except subprocess.TimeoutExpired:
                child.kill()
                child.wait()
    temporary = result.with_suffix(".tmp")
    temporary.write_text(json.dumps({"status": status}))
    temporary.replace(result)
    return status


def run_interactive(command):
    label = "org.ubiquitty.presentation-test." + uuid.uuid4().hex
    domain = "gui/" + str(os.getuid())
    # The launched test needs its caller's paths and instrumentation, never the
    # runner's credentials. Its working directory and exit status stay CTest's.
    names = {"HOME", "PATH", "TMPDIR", "DEVELOPER_DIR", "LLVM_PROFILE_FILE",
             "ASAN_OPTIONS", "UBSAN_OPTIONS", "TSAN_OPTIONS", "LSAN_OPTIONS",
             "DYLD_LIBRARY_PATH", "DYLD_FRAMEWORK_PATH", "LANG", "LC_ALL"}
    environment = {key: value for key, value in os.environ.items() if key in names}
    handlers = {}

    def interrupted(number, frame):
        raise SystemExit(128 + number)

    with tempfile.TemporaryDirectory(prefix="ubq-presentation-") as directory:
        root = Path(directory)
        result = root / "result.json"
        output = root / "output.log"
        errors = root / "errors.log"
        job = root / "job.plist"
        pipe = root / "lifeline"
        os.mkfifo(pipe, 0o600)
        lifeline = os.open(pipe, os.O_RDWR | os.O_NONBLOCK)
        job.write_bytes(plistlib.dumps({
            "Label": label,
            "ProgramArguments": [sys.executable, str(Path(__file__).resolve()), "--worker", str(result), *command],
            "WorkingDirectory": str(Path.cwd()),
            "EnvironmentVariables": environment,
            "ProcessType": "Interactive",
            "RunAtLoad": True,
            "StandardInPath": str(pipe),
            "StandardOutPath": str(output),
            "StandardErrorPath": str(errors),
        }))
        try:
            for number in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
                handlers[number] = signal.signal(number, interrupted)
            started = launchctl(["bootstrap", domain, str(job)])
            if started.returncode:
                raise RuntimeError("could not start the interactive test: " + started.stderr.strip())
            deadline = time.monotonic() + 30
            while not result.exists():
                if not result.with_suffix(".started").exists() and time.monotonic() >= deadline:
                    raise RuntimeError("the interactive test worker did not start within 30 seconds")
                time.sleep(.05)
            status = json.loads(result.read_text())["status"]
            if not isinstance(status, int) or not 0 <= status <= 255:
                raise RuntimeError("invalid interactive test exit status")
            return status
        finally:
            # A unique owned label, including on interruption and startup error.
            # CTest retains its existing timeout; removing the job also ends its
            # process group, so no detached test survives a timeout.
            launchctl(["bootout", domain + "/" + label])
            os.close(lifeline)
            for path, stream in ((output, sys.stdout), (errors, sys.stderr)):
                if path.exists():
                    stream.write(path.read_text(errors="replace"))
                    stream.flush()
            for number, handler in handlers.items():
                signal.signal(number, handler)


def main(arguments):
    if arguments and arguments[0] == "--worker":
        return worker(arguments[1], arguments[2:])
    if not arguments:
        raise RuntimeError("usage: run-presentation-test.py <test> [arguments...]")
    if platform.system() != "Darwin":
        return subprocess.run(arguments).returncode
    return run_interactive(arguments)


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv[1:]))
    except (OSError, ValueError, KeyError, RuntimeError) as error:
        print(f"presentation-test: {error}", file=sys.stderr)
        sys.exit(1)
