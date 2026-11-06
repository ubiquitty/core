#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""run-tests.sh's CORE_SRCS stays reconciled with the portable-Core SSOT.

CORE_SRCS is the third hand-maintained list of Core sources, after the SSOT
(Scripts/portable-core-sources.txt, which CMake and the wasm build read) and the
per-target ubiquitty_core_subset() trims in CMakeLists.txt. It is the subset the
unit tests link to instantiate and drive a headless Ubiquitty::Terminal, and it
derives from nothing: a bash array a person edits by hand.

The other two lists self-check. CMake FATAL_ERRORs on a subset entry that names
a file the SSOT does not hold, and check-core-subset-trims.py checks the trim
paths exist. CORE_SRCS had neither, so it drifted in silence: on 2026-08-13
Source/Core/OSC/TextSizing.cpp reached the SSOT and registered an OSC 66 handler
that Dispatch.cpp (already in CORE_SRCS) calls, but the source itself never
reached CORE_SRCS. Every stanza that links CORE_SRCS then failed at link with
handleOSC66 undefined. A checkout that does not run the whole suite never sees it, so the break
sat six hours until another checkout's instrumented build hit it.

The fix that catches the class rather than the instance: partition the SSOT.
Every portable-Core source belongs to exactly one of CORE_SRCS (the live
Terminal needs it) or Scripts/core-srcs-excluded.txt (it does not). A source
added to the SSOT and classified into neither fails here, at the commit that
adds it, rather than at a link nobody runs. The exclusion file is explicit per
path rather than a directory glob on purpose: a glob over an excluded subsystem
would auto-swallow a future file in it that the live Terminal does need, which
is the same silent miss this gate exists to close.

This is a static reconciliation, not a link. It cannot see a symbol that moved
between two files both already in CORE_SRCS; the link stanzas remain the oracle
for that. What it does see, at check-style time, is the addition or rename that
leaves the hand list behind the SSOT.
"""

import pathlib
import re
import sys

sys.path.insert(0, str(pathlib.Path(__file__).parent))
import gate_report

GATE = "check-core-srcs-complete"

SUFFIXES = (".cpp", ".c", ".mm")


def core_srcs(root):
    """The Source/ paths inside run-tests.sh's CORE_SRCS=( ... ) array.

    Read from the array bounds rather than a tree-wide grep: run-tests.sh names
    other Source/ files outside the array (the extra sources image and fuzz
    stanzas add to a link), and those are not CORE_SRCS.
    """
    lines = (root / "Scripts" / "run-tests.sh").read_text().splitlines()
    starts = [i for i, line in enumerate(lines) if line.startswith("CORE_SRCS=(")]
    if not starts:
        return None
    start = starts[0]
    end = next((i for i in range(start + 1, len(lines)) if lines[i].strip() == ")"), None)
    if end is None:
        return None
    found = set()
    for line in lines[start + 1:end]:
        match = re.search(r"Source/[A-Za-z0-9/._-]+\.(?:cpp|c|mm)", line)
        if match:
            found.add(match.group(0))
    return found


def path_set(path):
    """Non-comment, non-blank source paths in a newline-delimited list file."""
    found = set()
    for line in path.read_text().splitlines():
        stripped = line.split("#", 1)[0].strip()
        if stripped.endswith(SUFFIXES):
            found.add(stripped)
    return found


def main(argv):
    # Anchored to the script's location when no root is given: check-style runs
    # gates from the repo root, but the ctest shell wrapper runs from its build
    # tree, and a cwd default made a sibling gate's verdict depend on the caller.
    root = pathlib.Path(argv[1]) if len(argv) > 1 else pathlib.Path(__file__).resolve().parent.parent

    ssot_file = root / "Scripts" / "portable-core-sources.txt"
    excluded_file = root / "Scripts" / "core-srcs-excluded.txt"

    core = core_srcs(root)
    if core is None:
        return gate_report.failed(
            GATE, "run-tests.sh has no readable CORE_SRCS=( ... ) array; the "
            "pattern stopped matching rather than the array stopping existing",
            ["CORE_SRCS bounds not found"])
    if not core:
        return gate_report.failed(
            GATE, "the CORE_SRCS array parsed to zero source paths; the entry "
            "spelling drifted and this gate would pass having read nothing",
            ["0 CORE_SRCS entries"])

    ssot = path_set(ssot_file)
    if not ssot:
        return gate_report.failed(
            GATE, "portable-core-sources.txt parsed to zero paths; the SSOT is "
            "the denominator here and an empty one certifies nothing",
            ["0 SSOT entries"])

    excluded = path_set(excluded_file)
    if not excluded:
        return gate_report.failed(
            GATE, "core-srcs-excluded.txt parsed to zero paths; the partition "
            "needs its excluded half to be legible",
            ["0 excluded entries"])

    # A CORE_SRCS entry naming a file that does not exist is the rename half of
    # drift: the array points at a path a move left behind.
    gone = sorted(f for f in core if not (root / f).exists())
    if gone:
        for f in gone:
            gate_report.finding(f, GATE, "named by CORE_SRCS but absent on disk")
        return gate_report.failed(
            GATE, "CORE_SRCS entry(s) naming a file that does not exist", gone)

    # An exclusion that is not in the SSOT is stale: the file it excused was
    # renamed or dropped, and the entry now excuses nothing.
    stale = sorted(f for f in excluded if f not in ssot)
    if stale:
        for f in stale:
            gate_report.finding(f, GATE, "excluded but not in the SSOT; the entry is stale")
        return gate_report.failed(
            GATE, "core-srcs-excluded.txt entry(s) naming a path the SSOT does "
            "not hold", stale)

    # A file classified both ways is a contradiction the partition cannot hold.
    both = sorted(core & excluded)
    if both:
        for f in both:
            gate_report.finding(f, GATE, "in CORE_SRCS and in core-srcs-excluded.txt at once")
        return gate_report.failed(
            GATE, "source(s) both linked into and excluded from the live-Terminal "
            "set", both)

    # The partition: every SSOT source is classified. One that is in neither list
    # is the class this gate exists for — a Core file the SSOT gained that nobody
    # placed into or out of the live-Terminal link.
    unclassified = sorted(ssot - core - excluded)
    if unclassified:
        for f in unclassified:
            gate_report.finding(
                f, GATE, "in the SSOT but neither in CORE_SRCS nor core-srcs-excluded.txt")
        print("  Classify each: add it to CORE_SRCS in Scripts/run-tests.sh if a "
              "headless Terminal test needs its symbols, or to "
              "Scripts/core-srcs-excluded.txt if it does not.", file=sys.stderr)
        return gate_report.failed(
            GATE, "portable-core source(s) classified into neither CORE_SRCS nor "
            "the exclusion list", unclassified)

    return gate_report.ok(
        GATE, "%d portable-core sources partitioned: %d in CORE_SRCS, %d excluded"
        % (len(ssot), len(ssot & core), len(excluded)))


if __name__ == "__main__":
    sys.exit(main(sys.argv))
