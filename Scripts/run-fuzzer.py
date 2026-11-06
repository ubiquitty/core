#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""Runs one libFuzzer invocation with quiet progress and lossless diagnostic logs.

LLVM's verbosity flag suppresses progress, not its recommended dictionary.
Only its exact dictionary grammar and final counters are console-silent;
unrecognized output, including diagnostics inside a dictionary, stays visible.
Dictionary suggestions are evidence for review, never automatically seed input.
"""

import argparse
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import tempfile
import time


DICTIONARY_BEGIN = b"###### Recommended dictionary. ######\n"
DICTIONARY_END = b"###### End of recommended dictionary. ######\n"
# LLVM dictionary entries contain printable ASCII or escaped octal bytes.
DICTIONARY_ENTRY = re.compile(
    rb'"(?:[\x20-\x21\x23-\x5b\x5d-\x7e]|\\(?:["\\]|[0-3][0-7]{2}))*" # Uses: [0-9]+\n')
FINAL_STAT = re.compile(
    rb"stat::(number_of_executed_units|average_exec_per_sec|new_units_added|"
    rb"slowest_unit_time_sec|peak_rss_mb): +([0-9]+)\n")
QUIET_FLAGS = ("-verbosity=0", "-print_funcs=0", "-print_pcs=0", "-print_final_stats=1")


class InterruptedRun(Exception):
    def __init__(self, signal_number):
        super().__init__(f"interrupted by signal {signal_number}")
        self.signal_number = signal_number


def interrupt_run(signal_number, _frame):
    raise InterruptedRun(signal_number)


def stop_process(process):
    # The session belongs to this invocation, including any target descendants.
    try:
        os.killpg(process.pid, signal.SIGKILL)
    except ProcessLookupError:
        pass
    process.wait(timeout=5)


def route_output(stream, output, dictionary):
    inside_dictionary = False
    partial_line = False
    statistics = {}
    while data := stream.readline(65536):
        output.write(data)
        complete_line = not partial_line and data.endswith(b"\n")
        partial_line = not data.endswith(b"\n")
        silent = False
        if complete_line and data == DICTIONARY_BEGIN:
            inside_dictionary = True
            silent = True
        if inside_dictionary:
            dictionary.write(data)
            if complete_line and data == DICTIONARY_END:
                inside_dictionary = False
                silent = True
            elif complete_line and DICTIONARY_ENTRY.fullmatch(data):
                silent = True
        if complete_line and not inside_dictionary and (match := FINAL_STAT.fullmatch(data)):
            statistics[match[1].decode("ascii")] = match[2].decode("ascii")
            silent = True
        if not silent:
            sys.stderr.buffer.write(data)
            sys.stderr.buffer.flush()
    return statistics


def run(command, log_root):
    log_root.mkdir(parents=True, exist_ok=True)
    log_directory = Path(tempfile.mkdtemp(prefix=Path(command[0]).name + "-", dir=log_root))
    output_path = log_directory / "output.log"
    dictionary_path = log_directory / "recommended-dictionary.txt"
    started = time.monotonic()
    statistics = {}
    status = 2
    process = None
    previous_handlers = {}
    try:
        with output_path.open("wb") as output, dictionary_path.open("wb") as dictionary:
            for signal_number in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
                previous_handlers[signal_number] = signal.signal(signal_number, interrupt_run)
            # Appending logging flags prevents callers from accidentally restoring progress.
            process = subprocess.Popen([*command, *QUIET_FLAGS], stdout=subprocess.PIPE,
                                       stderr=subprocess.STDOUT, start_new_session=True)
            try:
                statistics = route_output(process.stdout, output, dictionary)
                returncode = process.wait()
                status = returncode if returncode >= 0 else 128 - returncode
            finally:
                if process.poll() is None:
                    stop_process(process)
                process.stdout.close()
    except InterruptedRun as error:
        status = 128 + error.signal_number
        print(f"run-fuzzer: {error}", file=sys.stderr)
    except FileNotFoundError as error:
        status = 127
        print(f"run-fuzzer: {error}", file=sys.stderr)
    except OSError as error:
        status = 2
        print(f"run-fuzzer: {error}", file=sys.stderr)
    finally:
        for signal_number, handler in previous_handlers.items():
            signal.signal(signal_number, handler)
    summary = [f"{Path(command[0]).name}: exit={status}", f"{time.monotonic() - started:.1f}s"]
    for key, label, suffix in (("number_of_executed_units", "units", ""),
                               ("new_units_added", "added", ""), ("peak_rss_mb", "rss", "MB")):
        if key in statistics:
            summary.append(f"{label}={statistics[key]}{suffix}")
    summary.extend((f"log={output_path}", f"dictionary={dictionary_path}"))
    print("; ".join(summary), flush=True)
    return status


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--log-dir", required=True, type=Path)
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    command = args.command[1:] if args.command[:1] == ["--"] else args.command
    if not command:
        parser.error("a fuzzer binary and its arguments are required after --")
    try:
        return run(command, args.log_dir)
    except OSError as error:
        print(f"run-fuzzer: cannot reserve logs: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
