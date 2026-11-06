#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""Exercise malformed forms of every verb the current ubq advertises.

The private home has no daemon or host binary: parse refusals must happen
before a connection, and verbs that accept a bare separator cannot reach a
real daemon. The runner also bounds this whole process.
"""

import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile


INVOCATION_SECONDS = 5
HUGE_NUMBER = b"99999999999999999999999999999999"
VERB_LINE = re.compile(r"^  ([a-z][a-z0-9-]*(?: [a-z][a-z0-9-]*)*)\s{2,}\S")
VALUE_OPTION = re.compile(r"(--[a-z][a-z0-9-]*)\s+(\S+)$")


def run(binary, arguments, environment):
    try:
        completed = subprocess.run(
            [binary, *arguments],
            env=environment,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=INVOCATION_SECONDS,
            check=False,
        )
    except subprocess.TimeoutExpired as error:
        raise AssertionError(f"{arguments!r} ran past {INVOCATION_SECONDS} s") from error
    if completed.returncode < 0:
        raise AssertionError(f"{arguments!r} died from signal {-completed.returncode}")
    return completed


def advertised_verbs(binary, environment):
    result = run(binary, [b"--help"], environment)
    if result.returncode:
        raise AssertionError(f"ubq --help exited {result.returncode}")
    lines = result.stdout.decode("utf-8", "replace").splitlines()
    try:
        first = lines.index("verbs:") + 1
    except ValueError as error:
        raise AssertionError("ubq --help has no verbs list") from error
    verbs = []
    for line in lines[first:]:
        if not line.strip():
            break
        match = VERB_LINE.match(line)
        if not match:
            raise AssertionError(f"unrecognized ubq --help verb line: {line!r}")
        verbs.append(match.group(1))
    if not verbs or len(verbs) != len(set(verbs)):
        raise AssertionError(f"empty or duplicate ubq --help verbs: {verbs!r}")
    return verbs


def value_options(binary, verb, environment):
    words = [os.fsencode(word) for word in verb.split()]
    result = run(binary, [*words, b"--help"], environment)
    if result.returncode:
        raise AssertionError(f"ubq {verb} --help exited {result.returncode}")
    options = []
    in_options = False
    for line in result.stdout.decode("utf-8", "replace").splitlines():
        if line == "options:":
            in_options = True
            continue
        if in_options and not line.strip():
            break
        if not in_options:
            continue
        column = re.split(r"\s{2,}", line.strip(), maxsplit=1)[0]
        match = VALUE_OPTION.search(column)
        if match:
            options.append((match.group(1).encode(), match.group(2)))
    return options


def check_case(binary, verb, case, arguments, environment, must_refuse):
    result = run(binary, arguments, environment)
    output = (result.stderr + result.stdout).decode("utf-8", "backslashreplace")
    if verb == "search" and case == "non-UTF-8 word":
        if result.returncode != 2 or "pattern" not in output or "UTF-8" not in output:
            raise AssertionError(f"search {case}: expected usage refusal naming the pattern and UTF-8, got exit {result.returncode}: {output[:400]}")
    if must_refuse and result.returncode == 0:
        raise AssertionError(f"{verb} {case}: accepted malformed {arguments!r}")
    if result.returncode == 0 and b"ubq:" in result.stderr:
        raise AssertionError(f"{verb} {case}: refusal exited 0: {output[:400]}")
    if result.returncode != 0 and not re.search(rf"(?<![a-z0-9_-]){re.escape(verb)}(?![a-z0-9_-])", output):
        raise AssertionError(f"{verb} {case}: refusal omitted its verb: {output[:400]}")


def main():
    binary = os.environ.get("UBQ_BIN")
    if not binary or not Path(binary).is_file():
        raise AssertionError("UBQ_BIN must name the built ubq")
    with tempfile.TemporaryDirectory(prefix="ubq-malformed-", dir="/tmp") as private_home:
        environment = os.environ.copy()
        for name in (
            "UBIQUITTY_SOCKET", "UBIQUITTY_HOST_BIN", "UBIQUITTY_AGENT_TOKEN",
            "UBQ_REMOTE_TOKEN", "UBIQUITTY_SESSION", "UBIQUITTY_PANE",
            "UBIQUITTY_WINDOW", "UBIQUITTY_TAB", "UBIQUITTY_AGENT_LABEL",
            "UBIQUITTY_AGENT_AUDIT",
        ):
            environment.pop(name, None)
        environment["HOME"] = private_home
        environment["UBIQUITTY_HOME"] = str(Path(private_home) / "control")
        environment["UBIQUITTY_AGENT_POLICY"] = str(Path(private_home) / "policy.conf")
        Path(environment["UBIQUITTY_HOME"]).mkdir(mode=0o700)
        Path(environment["UBIQUITTY_AGENT_POLICY"]).write_text("default confirm\n")
        verbs = advertised_verbs(binary, environment)
        # -H is global and can fail before any verb has been parsed.
        missing_host = run(binary, [b"-H"], environment)
        if missing_host.returncode == 0 or b"-H needs a host" not in missing_host.stderr:
            raise AssertionError("global -H without a host was not refused")
        count = 1
        for verb in verbs:
            prefix = [os.fsencode(word) for word in verb.split()]
            options = value_options(binary, verb, environment)
            cases = [
                ("unknown short flag", [*prefix, b"-x"], True),
                ("unknown long flag", [*prefix, b"--nonsense-option"], True),
                ("non-UTF-8 word", [*prefix, b"--", b"\xff"], True),
                ("bare separator", [*prefix, b"--"], False),
            ]
            for flag, value in options:
                cases.append((f"missing {flag.decode()} value", [*prefix, flag], True))
                if value == "N":
                    cases.append((f"huge {flag.decode()} value", [*prefix, flag, HUGE_NUMBER], True))
            for case, arguments, must_refuse in cases:
                check_case(binary, verb, case, arguments, environment, must_refuse)
                count += 1
    print(f"ubq-malformed-sweep: PASS {count} malformed calls across {len(verbs)} advertised verbs")


if __name__ == "__main__":
    try:
        main()
    except AssertionError as error:
        print(f"ubq-malformed-sweep: FAIL: {error}", file=sys.stderr)
        sys.exit(1)
