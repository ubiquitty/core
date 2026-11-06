#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

# Copyright (C) 2026 Stanislav Yaglo

"""Check that a stanza comment in run-tests.sh has a stanza under it.

Every test in `Scripts/run-tests.sh` is a comment saying what the test pins,
followed by the `build_and_run` that compiles and runs it. The comment is the
only place the reason lives — the stanza itself is a compiler invocation and a
file list.

Deleting a test deletes the stanza. The comment above it has no syntax, so
nothing notices when it stays, and what is left reads exactly like coverage that
runs: a paragraph pinning an encoder whose test is gone, or describing a sweep
that never runs.

A reader planning coverage reads such a paragraph and crosses the thing off.
That is worse than silence: it is the harness describing a scope it does not
have, in the file whose subject is what the suite covers.

The rule is structural, so it needs no list of what is currently true. A comment
block inside the stanza region is followed by a stanza or by nothing, and a
block followed only by another comment block is describing something that is not
there.

Blocks that document a shell function or close the file are not stanza comments;
they sit outside the region, which runs from the first stanza to the last.

A stanza is written two ways — `build_and_run`, and `if … wanted <name>; then`
for the ones needing a platform guard or their own compiler flags. Reading only
the first form ends the region early and judges a fraction of the stanzas,
which is a walk over the population the check can see rather than the one it is
about.

Stdlib only.

    python3 Scripts/check-stanza-comments.py
"""

import os
import re
import sys

import glob

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ubq_roots  # noqa: E402
REPO = ubq_roots.subject_of(__file__)
SCRIPT = os.path.join(REPO, "Scripts", "run-tests.sh")
# The stanzas of the other repositories a checkout holds, which the driver sources.
STANZA_FILES = sorted(glob.glob(os.path.join(REPO, "Scripts", "run-tests-stanzas-*.sh")))


# A stanza is written two ways. Most compile through `build_and_run`; the ones
# needing a platform guard or their own compiler flags open with
# `if … wanted <name>; then` and drive the compiler themselves.
#
# Keying the region on `build_and_run` alone ends it early, and a comment block
# anywhere past that end is outside the walk; the check then passes for the
# population it has rather than the one it is about.
# The two stanza spellings, kept apart so each can be checked for presence.
# One regex with both alternations bounds the walked region by whichever
# survives, so a rename of one spelling shrinks the region in silence — the
# defect the header already records from the `build_and_run` side, and which is
# reachable from the `wanted` side too.
BUILD_AND_RUN = re.compile(r'^build_and_run\b')
WANTED = re.compile(r'^if .*\bwanted\b')
STANZA = re.compile(BUILD_AND_RUN.pattern + '|' + WANTED.pattern)

# A family that drops below its floor is drift, not a clean tree. The floor is
# set well under the live count (dozens of wanted) and above zero, so a spelling
# rename trips the refusal rather than collapsing the region. A genuine migration
# that empties a family is meant to trip it too: the fix is then to update this
# gate, not to let it certify a region it no longer walks. The driver defines
# `build_and_run`, which is the one line of the family it still holds: the
# stanzas that call it are in the stanza files, walked on their own.
BUILD_AND_RUN_FLOOR = 1
WANTED_FLOOR = 10


def family_drift(lines):
    """A message naming a stanza spelling that has thinned past its floor, or None.

    - Parameter lines: the file's lines.
    - Returns: a refusal string when one family collapsed, else None.
    """
    build = sum(1 for line in lines if BUILD_AND_RUN.match(line))
    wanted = sum(1 for line in lines if WANTED.match(line))
    if build < BUILD_AND_RUN_FLOOR:
        return ("only %d line(s) match the build_and_run spelling (floor %d) — "
                "the walked region now spans only the `wanted` stanzas, and an "
                "orphan among the build_and_run ones would read as ok"
                % (build, BUILD_AND_RUN_FLOOR))
    if wanted < WANTED_FLOOR:
        return ("only %d line(s) match the `if ... wanted` spelling (floor %d) — "
                "the walked region now spans only the build_and_run stanzas, and "
                "an orphan among the guarded ones would read as ok"
                % (wanted, WANTED_FLOOR))
    return None


def orphans(lines):
    """[(line number, first line)] for every comment block with no stanza."""
    starts = [index for index, line in enumerate(lines) if STANZA.match(line)]
    if not starts:
        return None
    first, last = starts[0], starts[-1]

    found = []
    index = first
    while index < last:
        if not lines[index].startswith("#"):
            index += 1
            continue
        start = index
        while index < len(lines) and lines[index].startswith("#"):
            index += 1
        # The lookahead runs to the end of the file, not to the last stanza. A
        # block sitting immediately above that stanza is the one case where the
        # two differ, and bounding the scan at `last` reports it as an orphan.
        after = index
        while after < len(lines) and not lines[after].strip():
            after += 1
        if after >= len(lines) or lines[after].startswith("#"):
            found.append((start + 1, lines[start]))
    return found


def self_test():
    """Proves the orphan walk catches a planted block and the drift guard fires.

    The finding this closes was reproduced against the real file; the arms here
    are synthetic so the check carries its own proof rather than depending on
    run-tests.sh keeping a particular shape.

    - Returns: 0 when every arm behaves, 1 otherwise.
    """
    failures = []

    healthy = (
        ["build_and_run one", "build_and_run two"]
        + ["if x wanted a%d; then" % i for i in range(WANTED_FLOOR + 2)]
    )
    # An orphan is a comment block whose next non-blank line is another comment
    # or the end — a block with no stanza under it. Placed mid-region, between
    # the first and last stanza, with a second comment following so the first
    # cannot claim the stanza below as its own.
    withOrphan = (["build_and_run one", "", "# orphan describing nothing", "",
                   "# a second, separate block"]
                  + ["if x wanted a%d; then" % i for i in range(WANTED_FLOOR + 2)])
    if not orphans(withOrphan):
        failures.append("an orphan comment block mid-region was not caught")

    if family_drift(healthy) is not None:
        failures.append("a healthy two-family tree was flagged as drift")

    renamed_wanted = ["build_and_run one", "build_and_run two"] + \
                     ["if x want a%d; then" % i for i in range(WANTED_FLOOR + 2)]
    if family_drift(renamed_wanted) is None:
        failures.append("a renamed `wanted` spelling did not trip the drift guard")

    renamed_build = ["run_it one"] + ["if x wanted a%d; then" % i for i in range(WANTED_FLOOR + 2)]
    if family_drift(renamed_build) is None:
        failures.append("a renamed build_and_run spelling did not trip the drift guard")

    for failure in failures:
        print("check-stanza-comments --self-test: " + failure, file=sys.stderr)
    if failures:
        return 1
    print("check-stanza-comments --self-test: ok (a planted orphan is caught, and "
          "renaming either stanza spelling trips the drift refusal)")
    return 0


def main():
    if "--self-test" in sys.argv[1:]:
        return self_test()
    # The driver and its spelling floors are ubiquitty-core's. A repository that
    # carries Core as a library has a driver of its own that holds no stanza, and
    # its stanzas are in the files it keeps beside it.
    holdsDriver = "core" in ubq_roots.repositories(REPO)
    lines = []
    if holdsDriver:
        with open(SCRIPT, encoding="utf-8") as handle:
            lines = handle.read().split("\n")

    drift = family_drift(lines) if holdsDriver else None
    if drift is not None:
        print("check-stanza-comments: REFUSED — " + drift, file=sys.stderr)
        print("  A region bounded by one surviving spelling walks a subset of the "
              "stanzas and reports ok for the rest. Update STANZA (and the floors) "
              "if a spelling changed on purpose.", file=sys.stderr)
        return 2

    found = orphans(lines) if holdsDriver else []
    if found is None:
        print("check-stanza-comments: no build_and_run in %s — the file's shape "
              "changed and" % os.path.relpath(SCRIPT, REPO), file=sys.stderr)
        print("  nothing was compared.", file=sys.stderr)
        return 2

    # A stanza file is walked as the driver is, without the spelling floors: it
    # holds one repository's stanzas, not the driver's population.
    otherFound = []
    for path in STANZA_FILES:
        with open(path, encoding="utf-8") as handle:
            stanzaLines = handle.read().split("\n")
        walked = orphans(stanzaLines)
        if walked is None:
            print("check-stanza-comments: no stanza in %s — the file's shape changed and"
                  % os.path.relpath(path, REPO), file=sys.stderr)
            print("  nothing was compared.", file=sys.stderr)
            return 2
        otherFound += [(os.path.relpath(path, REPO), number, text) for number, text in walked]

    if found or otherFound:
        print("check-stanza-comments: a stanza comment with no stanza under it",
              file=sys.stderr)
        for number, text in found:
            print("  %s:%d  %s" % (os.path.relpath(SCRIPT, REPO), number,
                                   text[:72]), file=sys.stderr)
        for path, number, text in otherFound:
            print("  %s:%d  %s" % (path, number, text[:72]), file=sys.stderr)
        print("""
  The comment is the only record of why a test exists, so it outlives the test
  by default and then reads as coverage that runs. Delete it with the stanza, or
  put the stanza back.""", file=sys.stderr)
        return 1

    if not holdsDriver:
        print("check-stanza-comments: ok (%d stanza file(s), each comment with a stanza under it)"
              % len(STANZA_FILES))
        return 0

    starts = [index for index, line in enumerate(lines) if STANZA.match(line)]
    # The count is of blocks this actually judged, not of blocks in the file.
    # Prose above the first stanza and below the last describes the harness
    # rather than a test, and saying 113 where 20 were compared would report
    # coverage the walk does not have.
    judged = sum(1 for index in range(starts[0], starts[-1])
                 if lines[index].startswith("#")
                 and not lines[index - 1].startswith("#"))
    print("check-stanza-comments: ok (%d stanzas over lines %d-%d, %d comment "
          "blocks between the first and the last, each with a stanza under it)"
          % (len(starts), starts[0] + 1, starts[-1] + 1, judged))
    return 0


if __name__ == "__main__":
    sys.exit(main())
