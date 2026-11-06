#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""check-verdict-lines — a line that says FAILED says what failed.

The failure this exists for. A gate that prints

    check-x: FAILED —

and lists the offenders underneath is unreadable to anything that searches for
the word: the line it finds names no offender, no count, and nothing to act on.
Every reader of this repo's output — a person scrolling, a CI annotator, a model
reading a transcript — searches for that word first. Nine failing tests were read
here as nine defects when they were one link error, because the summary carried
the word and the cause did not.

So a line carrying FAILED must carry the verdict with it: a count, or an
offender, or both. `Scripts/gate-report-lib.sh` and `Scripts/gate_report.py`
produce that shape; this gate is what stops a new one from arriving without it.

The second bound is one polarity further out: a file that can PRINT the word and
cannot RETURN it. A verdict nothing downstream can act on is the same defect as a
verdict that names nothing — a runner reading exit codes sees a pass, and the
line saying otherwise scrolls by. Twenty-two tests were in that state before they
were routed through the harness; the bound holds the class at zero rather than
waiting for the twenty-third.

Two traps, both from ubiquitty-layers' attempt at a stricter version of this.
A shell script can exit non-zero from inside an embedded heredoc, so the whole
file text is searched rather than its shell statements. And an exit status can be
computed — `exit "$MISSING"` — so any non-literal-zero exit counts. Requiring a
literal `exit 1` accused four gates that were all correct.

What this cannot decide is a PATH: a file with failure modes elsewhere whose
measurement branch prints and returns zero. Separating those needs to know which
branch a line sits on, and a pattern-matcher over shell that guesses will fire on
correct code far more often than not.

What is not a violation. A per-item status marker printed on a line that already
names its item — `printf '  %-34s ' "$name"` followed by `echo FAILED` — is
already self-contained, and the two halves are separate statements. Judging the
source line alone cannot see that, so a marker whose emission is the bare word
is accepted; the rule is about SUMMARIES, which are the lines that end in a
separator and defer.
"""

import pathlib
import re
import subprocess
import sys

sys.path.insert(0, str(pathlib.Path(__file__).parent))
import gate_report

GATE = "check-verdict-lines"

# An emission whose FAILED text runs to the end of the line with nothing after
# the separator: the summary defers to lines this one does not carry.
DEFERS = re.compile(r"""FAILED[^"']*?[:—-]\s*(?:\\n)?\s*["')]""")

# The second shape of the same defect, and the one the first bound lets through.
# A line reading `FAILED — see stderr` is not bare, so the pattern above passes
# it, and it is exactly as useless: it names no offender and sends the reader to
# a stream they may not have kept. A run that puts the identifier on stderr and
# the verdict on stdout hands a reader filtering for the verdict a failure with
# nothing to act on, and the suite has to run again to learn what.
#
# A pointer is not a verdict. Name the offender or the count on the line.
POINTS_ELSEWHERE = re.compile(
    r"""FAILED[^"']*?\b(?:see|check|consult|refer\s+to)\b[^"']*?"""
    r"""\b(?:stderr|stdout|above|below|log|logs|output|the\s+list)\b""",
    re.IGNORECASE)

# The word alone, as the whole emitted string. Accepted: see the note above.
BARE_MARKER = re.compile(r"""^\s*(?:echo|printf)\s+["']?(?:%s\\n)?FAILED["']?\s*$""")

EMISSION = re.compile(r"\b(?:echo|printf|print)\b")


def offenders():
    files = subprocess.run(
        ["git", "ls-files", "Scripts", "Tests"],
        capture_output=True, text=True, check=True).stdout.split()
    found = []
    for name in files:
        if not name.endswith((".sh", ".py")) and "hooks/" not in name:
            continue
        path = pathlib.Path(name)
        if not path.is_file():
            continue
        try:
            lines = path.read_text(errors="replace").split("\n")
        except OSError:
            continue
        for number, line in enumerate(lines, 1):
            if "FAILED" not in line or not EMISSION.search(line):
                continue
            if name in ("Scripts/check-verdict-lines.py", "Scripts/gate-report-lib.sh",
                        "Scripts/gate_report.py"):
                continue  # the library and this gate quote the shape they define
            if BARE_MARKER.match(line):
                continue
            if DEFERS.search(line) or POINTS_ELSEWHERE.search(line):
                found.append((f"{name}:{number}", line.strip()))
    return found


# Any expression that can leave a non-zero status, read generously: a computed
# exit, a `sys.exit` inside a heredoc, a non-zero return from a function.
NONZERO_EXIT = re.compile(
    r"""exit\s+(?!0\b)[^\s;)]|sys\.exit\(|\breturn\s+(?!0\b)[^\s;)]|"""
    r"""\bfail\b|\bFAILED=1\b""")

FAIL_GRAMMAR = re.compile(r"\b(?:FAIL|FAILED)\b")


def unactionable():
    """[(file, count)] for files that print a failure and cannot return one."""
    files = subprocess.run(
        ["git", "ls-files", "Scripts", "Tests"],
        capture_output=True, text=True, check=True).stdout.split()
    found = []
    for name in files:
        if not name.endswith((".sh", ".py")) and "hooks/" not in name:
            continue
        if name == "Scripts/check-verdict-lines.py":
            continue  # this file quotes the shapes it defines
        path = pathlib.Path(name)
        if not path.is_file():
            continue
        try:
            text = path.read_text(errors="replace")
        except OSError:
            continue
        if not FAIL_GRAMMAR.search(text):
            continue
        if NONZERO_EXIT.search(text):
            continue
        found.append(name)
    return found


def main():
    silent = unactionable()
    for name in silent:
        gate_report.finding(name, GATE,
                            "prints a failure and has no path that can leave a "
                            "non-zero status, so a runner reading the exit code "
                            "sees a pass")
    if silent:
        print("  Route it through the harness, or give it an exit path its "
              "verdict can reach.", file=sys.stderr)
        return gate_report.failed(GATE, "file(s) whose failure cannot be returned",
                                  silent)

    deferring = offenders()
    for location, text in deferring:
        gate_report.finding(location, GATE,
                            f"a FAILED line that defers to the lines below it: {text}")
    if deferring:
        print("  Use gate_failed / gate_failed_count from Scripts/gate-report-lib.sh, or",
              file=sys.stderr)
        print("  gate_report.failed from Scripts/gate_report.py, so the count and the first",
              file=sys.stderr)
        print("  offenders ride on the line that carries the word.", file=sys.stderr)
        return gate_report.failed(GATE, "verdict line(s) that name nothing",
                                  [location for location, _ in deferring])
    return gate_report.ok(GATE, "every FAILED line carries its own count or "
                                "offender, and every file that prints one can "
                                "return one")


if __name__ == "__main__":
    sys.exit(main())
