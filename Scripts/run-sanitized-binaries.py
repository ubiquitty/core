#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""Run the selected whole binaries with their existing bounds and CTest locks."""

import argparse
from dataclasses import dataclass
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import time

from ctest_report import discovery_payload
import gate_report


@dataclass
class Test:
    name: str
    binary: Path
    serial: bool
    resources: frozenset
    processors: int


def selected_tests(build, names, jobs):
    pattern = "^(" + "|".join(re.escape(name) for name in names) + ")([.][0-9]+)?$"
    listing = subprocess.run(
        ["ctest", "--test-dir", str(build), "--show-only=json-v1", "-R", pattern],
        capture_output=True, text=True, check=True)
    entries = discovery_payload(listing.stdout).get("tests", [])
    tests = []
    for name in names:
        binary = build / "Tests" / name
        if not binary.is_file() or not os.access(binary, os.X_OK):
            raise RuntimeError(f"selected binary is missing or not executable: {binary}")
        matches = [entry for entry in entries
                   if re.fullmatch(re.escape(name) + r"([.][0-9]+)?", entry["name"])]
        serial = not matches
        resources = set()
        processors = 1
        for entry in matches:
            properties = {row["name"]: row["value"] for row in entry.get("properties", [])}
            serial |= str(properties.get("RUN_SERIAL", False)).lower() in ("true", "1", "on")
            locks = properties.get("RESOURCE_LOCK", [])
            resources.update(locks.split(";") if isinstance(locks, str) else locks)
            processors = max(processors, int(properties.get("PROCESSORS", 1)))
        tests.append(Test(name, binary, serial, frozenset(resources), min(jobs, processors)))
    return tests


def run_tests(tests, logs, jobs):
    """Keep each binary whole and bounded at 300 seconds, including its HOME."""
    pending = list(tests)
    active = []
    failed = []
    completed = 0
    interrupted = 0

    def stop(number, _frame):
        nonlocal interrupted
        interrupted = number

    for number in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
        signal.signal(number, stop)
    bounded = Path(__file__).with_name("run-bounded.sh")
    heartbeat = time.monotonic() + 30
    try:
        while (pending or active) and not interrupted:
            for item in list(active):
                test, child, stream, started = item
                status = child.poll()
                if status is None:
                    continue
                active.remove(item)
                stream.close()
                completed += 1
                print(f"sanitized_sync: {completed}/{len(tests)} {test.name}: "
                      f"exit {status}; {time.monotonic() - started:.1f}s", flush=True)
                if status:
                    failed.append(test.name)
                    print((logs / f"sanitized-{test.name}.log").read_text(errors="replace"),
                          file=sys.stderr, flush=True)
            index = 0
            while index < len(pending) and len(active) < jobs and not interrupted:
                test = pending[index]
                if any(item[0].serial for item in active) or (test.serial and active):
                    break
                held = set().union(*(item[0].resources for item in active))
                used = sum(item[0].processors for item in active)
                if test.resources & held or used + test.processors > jobs:
                    index += 1
                    continue
                stream = (logs / f"sanitized-{test.name}.log").open("w")
                try:
                    child = subprocess.Popen(
                        ["bash", "-c", 'source "$1"; ubq_run_bounded 300 "$2"',
                         "sanitized-case", str(bounded), str(test.binary)],
                        stdout=stream, stderr=subprocess.STDOUT, start_new_session=True)
                except BaseException:
                    stream.close()
                    raise
                active.append((test, child, stream, time.monotonic()))
                pending.pop(index)
                print(f"sanitized_sync: start {test.name}", flush=True)
                if test.serial:
                    break
            if time.monotonic() >= heartbeat:
                print(f"sanitized_sync: alive; {completed}/{len(tests)} completed; "
                      f"active {', '.join(item[0].name for item in active)}", flush=True)
                heartbeat = time.monotonic() + 30
            if active:
                time.sleep(0.05)
    finally:
        # This thread alone reaps these leaders. A live, unreaped child pins its
        # process-group ID while the existing bounded runner stops its children.
        for _test, child, _stream, _started in active:
            if child.poll() is None:
                try:
                    os.killpg(child.pid, signal.SIGTERM)
                except ProcessLookupError:
                    pass
        grace = float(os.environ.get("UBQ_STANZA_GRACE_SECONDS", "20")) + 2
        for _test, child, stream, _started in active:
            try:
                child.wait(timeout=grace)
            except subprocess.TimeoutExpired:
                os.killpg(child.pid, signal.SIGKILL)
                child.wait()
            stream.close()
    if interrupted:
        return 128 + interrupted
    if failed:
        return gate_report.failed("sanitized_sync", "sanitized binary failure(s)", failed)
    print(f"sanitized_sync: {completed} binaries ran clean under both sanitizers", flush=True)
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--logs", type=Path, required=True)
    parser.add_argument("--jobs", type=int, required=True)
    parser.add_argument("names", nargs="+")
    arguments = parser.parse_args()
    if arguments.jobs < 1 or len(arguments.names) != len(set(arguments.names)):
        parser.error("jobs must be positive and selected binary names must be unique")
    arguments.logs.mkdir(parents=True, exist_ok=True)
    try:
        tests = selected_tests(arguments.build.resolve(), arguments.names, arguments.jobs)
        return run_tests(tests, arguments.logs.resolve(), arguments.jobs)
    except (OSError, RuntimeError, ValueError, subprocess.CalledProcessError) as error:
        print(f"sanitized_sync: FAILED — {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
