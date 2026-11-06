#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""gate_report — one shape for every gate's verdict line.

The failure this exists for. A gate that prints "check-x: FAILED —" and then
lists the offenders on the following lines is unreadable to anything that looks
for the word FAILED: the line it finds ends in a colon and names no offender, no
count, and nothing to act on. That is how a reader — human or tool — concludes a
build is broken without learning what broke, and it is how nine failing tests get
read as nine defects when they are one link error.

So the rule is that the line CARRYING the word is the line carrying the verdict:
which gate, what kind of thing, how many, and the first few by name. Detail may
still follow. It may not be the only copy.

`Scripts/gate-report-lib.sh` is the same format for shell gates, and
`Tests/test-gate-report.sh` asserts the two agree byte for byte — a format
spelled twice drifts the first time one side is edited alone.
"""

import os
import sys

# How many offenders the verdict line names before it summarises the rest. Three
# fits a terminal width and is enough to recognise the pattern; the count is what
# makes the remainder actionable.
NAMED_LIMIT = 3


def failed(gate, what, offenders=()):
    """Writes the failing verdict to stderr and returns 1, for `return failed(…)`.

    - Parameters:
      - gate: The gate's own name, as it appears in its passing line.
      - what: The kind of thing that failed, phrased to read after a count.
      - offenders: The failing items, most useful first.
    - Returns: 1, so a caller can return it directly as an exit status.
    """
    line = _line(gate, what, offenders)
    print(line, file=sys.stderr)
    annotate("error", "", gate, line.split(" — ", 1)[-1])
    summary_row(gate, line.split(" — ", 1)[-1], os.environ.get("UBQ_GATE_REPRODUCE", gate))
    return 1


def failed_count(gate, count, what):
    """The verdict where offenders were counted but not collected.

    A gate that tallies while it walks rather than accumulating a list still has
    to put the number on the line, which is the part that makes it actionable.
    """
    print(count_line(gate, count, what), file=sys.stderr)
    annotate("error", "", gate, f"{count} {what}")
    summary_row(gate, f"{count} {what}", os.environ.get("UBQ_GATE_REPRODUCE", gate))
    return 1


def refused(gate, why, *, location=""):
    """Writes a non-verdict diagnostic when the gate could not measure its subject.

    - Parameters:
      - gate: The gate's own name.
      - why: The missing prerequisite or unreadable input.
      - location: The relevant path when one is known.
    - Returns: 2, so callers cannot confuse a refusal with a tested failure.
    """
    line = f"{gate}: REFUSED — {why}"
    print(line, file=sys.stderr)
    annotate("error", location, gate, why)
    summary_row(gate, why, os.environ.get("UBQ_GATE_REPRODUCE", gate))
    return 2


def count_line(gate, count, what):
    """The counted verdict text, so a test can compare the two libraries."""
    return f"{gate}: FAILED — {count} {what}"


def finding(location, gate, message, *, level="error"):
    """Writes one offender to stderr, in the shape every compiler emits.

        Source/Core/Foo.cpp:42: check-style: a boolean parameter takes an enum

    That shape is what every editor, CI annotator and language model already
    parses without being told. The gate's name rides on every line rather than
    only on the header, so a finding stays attributable when it is read on its
    own — which is how findings are read once the output is more than a screen.

    - Parameters:
      - location: `path:line`, or `path` where no line is known.
      - gate: The gate's own name.
      - message: What is wrong at that location, in one line.
      - level: The annotation severity; non-gating findings use warning.
    """
    print(finding_line(location, gate, message), file=sys.stderr)
    annotate(level, location, gate, message)


def finding_line(location, gate, message):
    """The finding text, so a test can compare it against the shell library."""
    return f"{location}: {gate}: {message}"


def ok(gate, what):
    """Writes the passing verdict to stdout and returns 0.

    Carries what was examined rather than a bare "ok": a gate that says only
    "ok" cannot be told apart from one that examined nothing, which is the other
    half of the same problem.
    """
    print(f"{gate}: ok ({what})")
    return 0


# How many annotations one process emits before it stops counting them out.
# GitHub renders ten per level per step and discards the rest, so a gate with
# four hundred findings would spend the whole budget before the gate after it
# emits one. The remainder stays in the log.
ANNOTATION_BUDGET = 10
_annotationsEmitted = 0
_annotationsSuppressed = 0


def _annotating():
    """Whether this run is the verdict a CI job reports.

    Read from UBQ_CI_ANNOTATE, set once at the workflow level, rather than from
    GITHUB_ACTIONS: gates also run inside jobs measuring something else, and a
    run that is not a job's verdict should spend neither its annotation budget
    nor its summary.
    """
    return bool(os.environ.get("UBQ_CI_ANNOTATE"))


def _escape(text):
    """A workflow command is one line; an unescaped newline truncates it there."""
    return text.replace("%", "%25").replace("\r", "%0D").replace("\n", "%0A")


def annotate(level, location, gate, message):
    """Writes one annotation, so a failing gate reaches the run summary and the
    annotations API without the reader fetching the log at all.

    The location rides in the message as well as in the file and line
    properties: the runner renders the command into the log as the message with
    its properties stripped, so a message without them is unreadable in exactly
    the log a reader greps.

    - Parameters:
      - level: `error`, `warning` or `notice`.
      - location: `path:line`, `path`, or empty for a verdict naming no file.
      - gate: The gate's own name, which becomes the annotation's title.
      - message: What is wrong, in one line.
    """
    global _annotationsEmitted, _annotationsSuppressed
    if not _annotating():
        return
    if _annotationsEmitted >= ANNOTATION_BUDGET:
        _annotationsSuppressed += 1
        return
    path, _, line = (location or "").partition(":")
    if not line.isdigit():
        line = ""
    if path and not os.path.exists(path):
        path = ""
    body = _escape(f"{location}: {gate}: {message}" if location else f"{gate}: {message}")
    properties = []
    if path:
        properties.append(f"file={path}")
        if line:
            properties.append(f"line={line}")
    properties.append(f"title={gate}")
    print(f"::{level} {','.join(properties)}::{body}", file=sys.stderr)
    _annotationsEmitted += 1


def summary_row(gate, what, reproduce):
    """Writes one row into the job summary GitHub renders above the log.

    The summary is the only surface a reader sees before deciding whether to
    fetch anything, so it carries the three facts that decide it: which gate,
    what it found, and the command that runs that finding alone.

    - Parameters:
      - gate: The gate's own name.
      - what: The finding, as the verdict line phrases it.
      - reproduce: The command that runs this gate alone.
    """
    if not _annotating():
        return
    path = os.environ.get("GITHUB_STEP_SUMMARY")
    if not path:
        return
    header = not os.path.exists(path) or os.path.getsize(path) == 0
    with open(path, "a", encoding="utf-8") as handle:
        if header:
            handle.write("### Failing gates\n\n| gate | finding | reproduce |\n|---|---|---|\n")
        handle.write(f"| `{gate}` | {what} | `{reproduce}` |\n")


def _line(gate, what, offenders=()):
    """The verdict text, so a test can compare it against the shell library."""
    items = [str(offender) for offender in offenders]
    if not items:
        return f"{gate}: FAILED — {what}"
    named = ", ".join(items[:NAMED_LIMIT])
    remaining = len(items) - min(len(items), NAMED_LIMIT)
    if remaining > 0:
        return f"{gate}: FAILED — {len(items)} {what}: {named} (+{remaining} more)"
    return f"{gate}: FAILED — {len(items)} {what}: {named}"


if __name__ == "__main__":
    # Invoked by Tests/test-gate-report.sh to compare against the shell library.
    if len(sys.argv) >= 3 and sys.argv[1] == "--count":
        print(count_line(sys.argv[2], sys.argv[3], sys.argv[4]))
    elif len(sys.argv) >= 3 and sys.argv[1] == "--finding":
        print(finding_line(sys.argv[2], sys.argv[3], sys.argv[4]))
    elif len(sys.argv) >= 3:
        print(_line(sys.argv[1], sys.argv[2], sys.argv[3:]))
    else:
        print("usage: gate_report.py <gate> <what> [offender ...]", file=sys.stderr)
        print("       gate_report.py --finding <location> <gate> <message>", file=sys.stderr)
        raise SystemExit(2)
