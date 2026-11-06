#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""Scripts/check-docs.py — documentation hygiene lint for Markdown + TOML.

The comment lint (check-comments.sh) scans C/ObjC source; docs slip through it.
This covers the same rules in `.md` / `.toml`, where a public tree must not ship
scaffolding:

BLOCKS on:
  - `TODO` / `FIXME` / `XXX` markers — a public doc states what is, not what's pending.
  - internal issue/PR ticket refs (#NNN) — temporal, and dangling for a reader
    without the tracker; state the invariant, not the tracker id.

ADVISORY (printed, never blocks): temporal phrasing (`used to` / `former` /
  `no longer` / `will be` / `lands in` …) to restate present-tense.

EXEMPT (not scanned): vendored/upstream license texts (Docs/third-party-licenses/,
  *LICENSE*, *OFL*, UnifontLICENSE), Source/ThirdParty/, fuzz corpora, and this
  script. External-standard citations (UAX #29, RFC 1951, …) are not flagged.

Usage:
  Scripts/check-docs.py            # all tracked .md / .toml
  Scripts/check-docs.py --staged   # staged files only (pre-commit)
  Scripts/check-docs.py file ...   # specific files

Exit 0 = clean, 1 = marker/ticket violations, 2 = usage error.
Wired into Scripts/hooks/pre-commit (BLOCKS) and Scripts/check-style.py.

Both rules walk the corpus once: a process per file per rule would pay a few
hundred interpreter launches to read a few hundred small files.
"""

import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import gate_corpus
import gate_report

GATE = "check-docs"

EXCLUDE = (r"(^|/)(Source/ThirdParty/|Docs/third-party-licenses/"
           r"|Tests/Core/fuzz-corpus/)"
           r"|(LICENSE|OFL|UnifontLICENSE)"
           r"|(^|/)check-docs\.(sh|py)$")

KINDS = (".md", ".toml")

MARKER = re.compile(r"\b(TODO|FIXME|XXX)\b")

# A quoted hex id is a commit, not a tracker reference, and is removed before the
# ticket rule reads the line.
QUOTED_HEX = re.compile(r"""(["'])\#(?:[0-9a-fA-F]{8}|[0-9a-fA-F]{6}"""
                        r"""|[0-9a-fA-F]{4}|[0-9a-fA-F]{3})\1""")
TICKET = re.compile(r"\#[0-9]{2,}\b")
# An external standard cites a number the same way, and a citation line carries
# the standard's name, a letter before the hash, a unit after it, or a URL.
CITATION = re.compile(r"\b(?:UAX|UTS|RFC|ISO|ECMA|Annex|Unicode|TR)\b")
IDENTIFIER_HASH = re.compile(r"[A-Za-z]\#[0-9]")
PIXELS = re.compile(r"\#[0-9]+\s*px")
URL = re.compile(r"https?://")

TEMPORAL = re.compile(
    r"\b(?:used to|no longer|previously|formerly|will be|for now"
    r"|to be (?:added|done)|going forward|lands in"
    r"|once (?:it|we) (?:ship|land))\b", re.IGNORECASE)


def blocking(line):
    """Which blocking rule a line trips, or None."""
    if MARKER.search(line):
        return "marker"
    ticketLine = QUOTED_HEX.sub("", line)
    if (TICKET.search(ticketLine)
            and not CITATION.search(line)
            and not IDENTIFIER_HASH.search(line)
            and not PIXELS.search(line)
            and not URL.search(line)):
        return "ticket-ref"
    return None


def main(argv):
    if argv and argv[0] in ("--help", "-h"):
        print(__doc__.strip())
        return 0
    os.chdir(gate_corpus.root())

    if argv and argv[0] == "--staged":
        # The commit path, and the one that guards. It carries added files, so
        # a doc being introduced is scanned here.
        paths = [path for path in gate_corpus.staged()
                 if path.endswith(KINDS) and not re.search(EXCLUDE, path)]
    elif argv:
        # Named files are scanned as named, exemptions and suffixes included:
        # someone who types a path is asking about that file.
        paths = list(argv)
    else:
        # Tracked only, as the usage above documents. The
        # full-tree pass is a survey; the --staged pass above is what stands
        # between a doc and the tree, and it sees files being added.
        paths = [path for path in gate_corpus.tracked(["*.md", "*.toml"])
                 if not re.search(EXCLUDE, path)]
    if not paths:
        print(f"{GATE}: no docs to scan")
        return 0

    blocked = []
    advisory = []
    # One walk for both rules. The shell form opened each file twice, once per
    # perl.
    for path, number, line in gate_corpus.lines(paths):
        hit = blocking(line)
        if hit:
            blocked.append(f"  {path}:{number}: [{hit}] {line.strip()}")
        if TEMPORAL.search(line):
            advisory.append(f"  {path}:{number}: warning: {GATE}: {line.strip()}")

    if blocked:
        print(f"{GATE}: TODO markers / internal ticket refs in docs — state the "
              f"invariant, not scaffolding:")
        for finding in blocked:
            print(finding)

    if advisory:
        print()
        print(f"{GATE}: ADVISORY — temporal phrasing (restate present-tense; "
              f"does NOT block):")
        for finding in advisory:
            print(finding)

    print()
    if blocked:
        sys.stdout.flush()
        # The count is the number of findings. The shell form passed a flag it
        # set to 1 on the first hit, so a doc with fifty markers reported a
        # count of one above a list of fifty — and the count is the half of
        # gate_failed_count that is supposed to make the line actionable.
        return gate_report.failed_count(
            GATE, len(blocked),
            "TODO marker(s) / internal ticket ref(s) in docs, listed above")

    print(f"{GATE}: ok ({len(paths)} doc(s) scanned)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
