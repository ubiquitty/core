#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0
"""check-graphemebreak-generated.py — Extended_Pictographic is generated from
emoji-data.txt, not carried by hand beside the width union that already is.

isExtendedPictographic reads a property the generator emits from emoji-data.txt
for the width table, and that GraphemeBreak.cpp also carries as a hand range
array. Two sources for one property drift, silently and in range: the
Emoji_Presentation half of the same file is the precedent, and generating it
removed the second source rather than refreshing it. This asserts the same for
Extended_Pictographic — GraphemeBreak.cpp carries no range-literal table of its
own, because the generated .inc it already includes is the one source.

Keyed on the definition, not the name. The generated array keeps the name
extPict, so a grep for the word discriminates nothing; a range literal
{ 0x.., 0x.. } is what moves into GraphemeBreak.tables.inc, and its absence from
the .cpp is the property this checks. GraphemeBreak.cpp's only range literals
today are the extPict array; Width.cpp's ambiguousRanges is a curated policy,
not a UCD snapshot, so this is scoped to the one file the emit changes.
"""
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
TARGET = REPO / "Source/Core/Unicode/GraphemeBreak.cpp"
RANGE_LITERAL = re.compile(r"\{\s*0x[0-9A-Fa-f]+\s*,\s*0x[0-9A-Fa-f]+\s*\}")


def hitsIn(text):
    """Lines carrying a range literal, as (lineNumber, line)."""
    return [(number, line)
            for number, line in enumerate(text.split("\n"), 1)
            if RANGE_LITERAL.search(line)]


def selfTest():
    ok = True

    # What a hand table looks like if one comes back — the generated array's own
    # shape, which is the thing this reports as absent.
    carried = "static constexpr Range extPict[] = {\n    { 0x1F300, 0x1F5FF },\n};\n"
    if len(hitsIn(carried)) != 1:
        print("self-test: a range literal was not counted", file=sys.stderr)
        ok = False

    # What the file holds today: the generated table included, a lookup through
    # it, and hex that is not a range pair. None may count, or the gate fires on
    # a file that is already correct.
    converted = ('#include "GraphemeBreak.tables.inc"\n'
                 "bool isExtendedPictographic(char32_t c) { return inRanges(c, extPict); }\n"
                 "constexpr char32_t zwj = 0x200D;\n"
                 "if (c == 0x1F3FB || c == 0x1F3FF) return true;\n")
    if hitsIn(converted):
        print("self-test: a converted file counted as a hand table: %s"
              % hitsIn(converted), file=sys.stderr)
        ok = False

    print("check-graphemebreak-generated: self-test %s" % ("ok" if ok else "FAILED"))
    return 0 if ok else 1


def main():
    if "--self-test" in sys.argv:
        return selfTest()

    # A renamed or moved target has to say so. read_text() raises instead, and a
    # traceback from a gate reads as the gate being broken rather than as the
    # thing it watches having moved — and the absence it reports would not have
    # been established either way.
    if not TARGET.is_file():
        print("check-graphemebreak-generated: FAILED — %s is missing, so no line was "
              "scanned and the absence of a hand table was not established."
              % TARGET.name, file=sys.stderr)
        return 1

    hits = hitsIn(TARGET.read_text())
    if not hits:
        print("check-graphemebreak-generated: ok (no hand range table in GraphemeBreak.cpp)")
        return 0
    print("GraphemeBreak.cpp carries %d line(s) of range literals by hand:" % len(hits),
          file=sys.stderr)
    for number, line in hits[:5]:
        print("  %s:%d  %s" % (TARGET.name, number, line.strip()[:88]), file=sys.stderr)
    if len(hits) > 5:
        print("  ... and %d more" % (len(hits) - 5), file=sys.stderr)
    print("\n  A Unicode property carried both by hand here and by the generator drifts\n"
          "  silently. Emit Extended_Pictographic into GraphemeBreak.tables.inc from the\n"
          "  emoji-data.txt parse that already builds the width union, and delete the\n"
          "  hand array, so one source cannot fall behind the other.", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
