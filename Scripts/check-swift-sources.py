#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""check-swift-sources.py — fail when a .swift file under Source/ is in no CMake
source list, and so is never compiled.

  ./Scripts/check-swift-sources.py

A C++ translation unit missing from its target fails at LINK, because something
calls into it. Swift does not give that back. A .swift absent from
UBIQUITTY_SWIFT_SOURCES is not part of the module: nothing references it,
nothing misses it, and the file sits in the tree reading like shipping code.

Such a file rots against renames in the code it calls and can declare a class
colliding with a live one, and no build reports either, because no build reads
it. Grepping the tree against the CMake list is what finds it.

The check is textual on purpose. Asking CMake would mean configuring, and the
failure is that a file is absent from a list, which is a question about the list.
"""

import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import gate_corpus
import gate_report

GATE = "check-swift-sources"

# Every .swift named anywhere CMake reads, whatever variable holds it and
# whichever file it sits in: a second list added later must not need this
# script edited to be honoured. An included .cmake counts — a source list
# extracted into one is still the list that compiles the file, and a gate that
# reads only CMakeLists.txt calls every entry uncompiled the day it moves.
NAMED = re.compile(r"[A-Za-z0-9_/.+-]*\.swift")


def listed_names(paths):
    """Basenames of every .swift a CMake list or include mentions.

    Basenames, matching the shell form this replaces: it compared the two sides
    with `sed 's|.*/||'`, so a file is listed when its NAME appears in any list.
    """
    names = set()
    for _, text in gate_corpus.texts(paths):
        for match in NAMED.finditer(text):
            names.add(os.path.basename(match.group(0)))
    return names


def main(argv):
    if argv and argv[0] in ("--help", "-h"):
        print(__doc__.strip())
        return 0
    os.chdir(gate_corpus.root())

    # Both sides read tracked and untracked files. The shell form walked the
    # filesystem with find, so a Swift file being added was inside the question;
    # a tracked-only list reports it as listed because it never sees it, which
    # is the pass-on-the-one-file-nobody-read failure gate_corpus exists to
    # stop.
    everything = gate_corpus.tracked_and_untracked()
    cmake = [path for path in everything
             if (os.path.basename(path) == "CMakeLists.txt" or path.endswith(".cmake"))
             and not path.startswith("build")]
    listed = listed_names(cmake)

    found = {}
    for path in everything:
        if path.startswith("Source/") and path.endswith(".swift"):
            found.setdefault(os.path.basename(path), path)

    # The two sides fail differently, and only one fails loudly on its own. If
    # the CMake extraction stops matching, every Swift file reads as unlisted and
    # this shouts. If the walk over Source/ stops matching — Source/ moved, the
    # suffix changed — the comparison runs over nothing, finds nothing missing,
    # and reports that every Swift file is listed. Vacuously true, and the
    # reassuring direction to be wrong in.
    if not found:
        print(f"{GATE}: FAILED — no .swift under Source/, so nothing was compared",
              file=sys.stderr)
        print("  against the CMake lists. The tree carries eighty-odd; a walk "
              "returning none", file=sys.stderr)
        print("  has broken rather than found a tree without Swift in it.",
              file=sys.stderr)
        return 1

    missing = sorted(name for name in found if name not in listed)
    if missing:
        for name in missing:
            gate_report.finding(
                found[name], GATE,
                "in no CMake list, so it is never compiled and cannot fail to "
                "build however wrong it is")
        print("  Add each to UBIQUITTY_SWIFT_SOURCES, or delete it.",
              file=sys.stderr)
        return gate_report.failed(
            GATE, "Swift file(s) under Source/ in no CMake list", missing)

    print(f"{GATE}: ok ({len(found)} Swift files, every one in a CMake list)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
