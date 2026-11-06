#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0
"""check-blink-hint-chokepoint.py — the blink hint and its stamp move together.

`CellGrid` carries a conservative DECBLINK hint: `hasBlinkHint()` true MAY mean
a Blink cell exists, false means none does. A frontend's animation clock rests
on it — the Metal frame clock guards on `frameClockHasAnimatingContent`, so a
hint that is true forever is a terminal that redraws forever, and one that is
false while a Blink cell is on screen is a cursor that stops blinking.

Because sticky-true is safe and stale-false is not, a viewer that scanned the
whole grid and saw no Blink cell may retract the hint for itself — but only if
its scan is not older than the most recent blink-attributed write. That is what
`m_blinkHintGeneration` records, and the retraction is only sound while the stamp
cannot be older than the flag it describes.

`noteBlinkWrite` is what makes that true. It sets both, and the eight arming
sites call it: the per-cell, known-diff, row-span, ASCII-run, deferred-blank,
fill and wide-pair write paths. Before it existed, eight sites assigned the flag
directly and a stamp added to three of them would have left the other five
publishing a stamp older than their own blink.

This gate holds the choke point rather than the count. The count is not the
invariant — a ninth write path is ordinary and welcome, and it is safe the moment
it calls `noteBlinkWrite`. What is not safe is a site that assigns the flag
itself, which is invisible in review precisely because it looks like the eight
lines it replaces, and which fails nothing at runtime: the flag is still armed,
so the terminal still blinks, and the only symptom is a viewer retracting against
a stamp that predates the write.

So: the flag is armed in one place, and every assignment to either name is paired
with the other.

Three shapes are allowed, and each pairs:

    noteBlinkWrite        flag = true,  stamp = m_damageGeneration
    clearBlinkHintIfClean flag = false, stamp = 0
    the constructor       both in the initialiser list

A fourth is what this rejects.

Stdlib only.

    python3 Scripts/check-blink-hint-chokepoint.py
    python3 Scripts/check-blink-hint-chokepoint.py --self-test
"""

import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gate_report  # noqa: E402 — the path above is what makes it importable

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, ".."))

SOURCES = (
    os.path.join("Source", "Core", "CellGrid.h"),
    os.path.join("Source", "Core", "CellGrid.cpp"),
)

FLAG = "m_blinkHint"
STAMP = "m_blinkHintGeneration"

# The arming site. Named rather than counted: which functions call it is free to
# change, and does, every time a write path is added.
CHOKE = "noteBlinkWrite"

# The stamp is pinned harder than the flag, and the reason is the mirror of the
# flag's. A site that arms without stamping publishes a stamp OLDER than its
# write, and a viewer retracts against a write it never saw. A site that moves
# the stamp without arming publishes one NEWER than the blink it describes, and a
# viewer retracts against a window that never existed. Pairing alone does not
# catch the second: `m_blinkHintGeneration = x; m_blinkHint = false;` is a pair,
# and it is not either of the two writes this class performs.
STAMP_MEMBERS = (CHOKE, "clearBlinkHintIfClean")

# `m_blinkHint = true;` and the initialiser-list form `m_blinkHint(false)`. The
# word boundary is what keeps FLAG from matching STAMP, which starts with it, and
# the captured operator is what separates the two forms: a constructor sets both
# members before any of this class's writes can run, so it is a legitimate site
# for the stamp while an assignment elsewhere is not.
def assignment(name):
    return re.compile(r"(?<![A-Za-z0-9_])" + re.escape(name) + r"(?![A-Za-z0-9_])\s*([=(])")


# How far apart the two may sit and still count as one write. They are adjacent
# lines everywhere today; the slack is for a comment between them, not for a
# different function.
PAIR_WINDOW = 3


def read(path):
    try:
        with open(os.path.join(REPO, path), encoding="utf-8", errors="replace") as handle:
            return handle.read()
    except OSError:
        return None


# The last identifier before an unterminated `(` — the member's own name, whether
# it is written inline as `void noteBlinkWrite()` or out of line as
# `void CellGrid::noteBlinkWrite()`. Taking the LAST identifier is what makes the
# qualified form work: anchoring on the return type reads
# `CellGrid::noteBlinkWrite` as unnamed, so moving the member from the header to
# the .cpp — a refactor that changes nothing this gate is about — would fail it
# and blame a function called "unknown".
#
# `[^;=]` excludes call sites and assignments: `noteBlinkWrite();` ends in a
# semicolon and `m_blinkHint = true;` carries an `=`, so neither is read as a
# definition.
SIGNATURE = re.compile(r"^[^;=]*?([A-Za-z_]\w*)\s*\([^;]*$")

# Words that take a parenthesis and are not definitions.
KEYWORDS = ("if", "for", "while", "switch", "return", "catch", "sizeof")


def enclosingFunction(lines, index):
    """The nearest function-ish name at or above `index`, for naming a site.

    Not a parser. It reports where a reader should look, and every caller below
    treats a miss as "unknown" rather than as a verdict.
    """
    for i in range(index, max(-1, index - 60), -1):
        match = SIGNATURE.match(lines[i])
        if match and match.group(1) not in KEYWORDS:
            return match.group(1)
    return "unknown"


def sites(text):
    """Every write of the flag or the stamp, as (lineNumber, which, function, form).

    `form` is "=" for an assignment and "(" for an initialiser-list entry.
    """
    flag, stamp = assignment(FLAG), assignment(STAMP)
    lines = text.split("\n")
    found = []
    for i, line in enumerate(lines):
        # STAMP contains FLAG as a prefix, so ask for the stamp first and let a
        # line be one or the other, never both.
        match = stamp.search(line)
        which = STAMP
        if not match:
            match = flag.search(line)
            which = FLAG
        if match:
            found.append((i + 1, which, enclosingFunction(lines, i), match.group(1)))
    return found


def unpaired(found):
    """Writes with no partner within PAIR_WINDOW lines."""
    lone = []
    for number, which, function, _form in found:
        other = STAMP if which == FLAG else FLAG
        if not any(o == other and abs(n - number) <= PAIR_WINDOW for n, o, _f, _o in found):
            lone.append((number, which, function))
    return lone


def armingSites(text):
    """Lines setting the flag TRUE, which is the assignment the stamp exists for."""
    arm = re.compile(r"(?<![A-Za-z0-9_])" + re.escape(FLAG) + r"(?![A-Za-z0-9_])\s*=\s*true")
    lines = text.split("\n")
    return [(i + 1, enclosingFunction(lines, i))
            for i, line in enumerate(lines) if arm.search(line)]


def check(texts):
    """Returns a list of complaints; empty means the choke point holds."""
    offenders = []
    arming = []
    total = 0

    for path, text in texts:
        found = sites(text)
        total += len(found)
        for number, which, function in unpaired(found):
            offenders.append("%s:%d assigns %s with no %s beside it (in %s)"
                             % (path, number, which, STAMP if which == FLAG else FLAG, function))
        for number, function in armingSites(text):
            arming.append((path, number, function))
        # The stamp's own pin. An initialiser-list entry is exempt: a constructor
        # runs before any write of this class can, so it cannot publish a stamp
        # that disagrees with a blink.
        for number, which, function, form in found:
            if which == STAMP and form == "=" and function not in STAMP_MEMBERS:
                offenders.append("%s:%d writes %s in %s — only %s may move it"
                                 % (path, number, STAMP, function, " and ".join(STAMP_MEMBERS)))

    if not total:
        return None, 0, 0  # the names moved; the caller reports that, not a pass

    for path, number, function in arming:
        if function != CHOKE:
            offenders.append("%s:%d arms the hint outside %s (in %s) — call %s instead"
                             % (path, number, CHOKE, function, CHOKE))

    if not arming:
        offenders.append("nothing sets %s true anywhere — the hint can no longer arm" % FLAG)

    return offenders, total, len(arming)


def selfTest():
    ok = True

    good = ("void noteBlinkWrite()\n"
            "{\n"
            "    m_blinkHint = true;\n"
            "    m_blinkHintGeneration = m_damageGeneration;\n"
            "}\n")
    offenders, total, arming = check([("good.h", good)])
    if offenders or total != 2 or arming != 1:
        print("self-test: the paired choke point was rejected: %s" % offenders, file=sys.stderr)
        ok = False

    # The defect: a new write path arms the flag itself. The flag is still set,
    # so nothing blinks wrong — only the stamp is stale.
    rogue = good + ("void writeSomethingNew()\n"
                    "{\n"
                    "    m_blinkHint = true;\n"
                    "}\n")
    offenders, _, _ = check([("rogue.h", rogue)])
    if not any("outside noteBlinkWrite" in o for o in offenders):
        print("self-test: a site arming outside the choke point passed: %s" % offenders,
              file=sys.stderr)
        ok = False
    if not any("with no m_blinkHintGeneration beside it" in o for o in offenders):
        print("self-test: an unpaired flag assignment passed: %s" % offenders, file=sys.stderr)
        ok = False

    # The other direction: a stamp moved without the flag.
    lone = good + ("void refreshStamp()\n"
                   "{\n"
                   "    m_blinkHintGeneration = m_damageGeneration;\n"
                   "}\n")
    offenders, _, _ = check([("lone.h", lone)])
    if not any("with no m_blinkHint beside it" in o for o in offenders):
        print("self-test: an unpaired stamp assignment passed: %s" % offenders, file=sys.stderr)
        ok = False

    # A renamed member must read as "nothing compared", never as a pass.
    offenders, total, _ = check([("gone.h", "int unrelated = 0;\n")])
    if offenders is not None or total:
        print("self-test: absent members did not report as nothing-compared", file=sys.stderr)
        ok = False

    # The clear pairs too, and must not be mistaken for an arming site.
    clear = good + ("void clearBlinkHintIfClean()\n"
                    "{\n"
                    "    m_blinkHint = false;\n"
                    "    m_blinkHintGeneration = 0;\n"
                    "}\n")
    offenders, _, arming = check([("clear.h", clear)])
    if offenders or arming != 1:
        print("self-test: the paired clear was rejected: %s" % offenders, file=sys.stderr)
        ok = False

    # The stamp's own defect, and the one a flag-only rule passes: a member that
    # moves the stamp while leaving the flag alone. It pairs, and it is not an
    # arming site, so both earlier rules are satisfied and it is still a write
    # this class does not perform. It publishes a stamp newer than the blink it
    # describes.
    drifting = clear + ("void refreshOnScroll()\n"
                        "{\n"
                        "    m_blinkHintGeneration = m_damageGeneration;\n"
                        "    m_blinkHint = false;\n"
                        "}\n")
    offenders, _, _ = check([("drifting.h", drifting)])
    if not any("only noteBlinkWrite and clearBlinkHintIfClean may move it" in o
               for o in offenders):
        print("self-test: a paired stamp write outside the two members passed: %s"
              % offenders, file=sys.stderr)
        ok = False

    # And the shape that makes this a member rule rather than a count rule: the
    # two legitimate members hold four assignments between them, so a gate keyed
    # on "how many" reads the file above as its own violation.
    offenders, total, _ = check([("both.h", clear)])
    if offenders or total != 4:
        print("self-test: the two legitimate members were miscounted or rejected: %s"
              % offenders, file=sys.stderr)
        ok = False

    # The same two members, defined out of line. Nothing this gate is about
    # changes when a member moves from the header to the .cpp, so nothing here
    # may fail; a gate that does blocks a refactor it has no opinion on.
    outOfLine = ("void CellGrid::noteBlinkWrite()\n"
                 "{\n"
                 "    m_blinkHint = true;\n"
                 "    m_blinkHintGeneration = m_damageGeneration;\n"
                 "}\n"
                 "void CellGrid::clearBlinkHintIfClean()\n"
                 "{\n"
                 "    m_blinkHint = false;\n"
                 "    m_blinkHintGeneration = 0;\n"
                 "}\n")
    offenders, _, arming = check([("outofline.cpp", outOfLine)])
    if offenders or arming != 1:
        print("self-test: out-of-line definitions were rejected: %s" % offenders,
              file=sys.stderr)
        ok = False

    # And the call site must not read as a definition, or every write path that
    # calls the choke point would look like a member arming the flag itself.
    calling = good + ("void writeWideCell()\n"
                      "{\n"
                      "    noteBlinkWrite();\n"
                      "}\n")
    offenders, _, _ = check([("calling.cpp", calling)])
    if offenders:
        print("self-test: a call to the choke point was read as a violation: %s"
              % offenders, file=sys.stderr)
        ok = False

    # A constructor initialiser list writes both and is not either member.
    built = good + ("CellGrid::CellGrid(int rows)\n"
                    "    : m_rows(rows)\n"
                    "    , m_blinkHint(false)\n"
                    "    , m_blinkHintGeneration(0)\n"
                    "{\n"
                    "}\n")
    offenders, _, _ = check([("built.cpp", built)])
    if offenders:
        print("self-test: a constructor initialiser list was rejected: %s" % offenders,
              file=sys.stderr)
        ok = False

    print("check-blink-hint-chokepoint: self-test %s" % ("ok" if ok else "FAILED"))
    return 0 if ok else 1


def main():
    if "--self-test" in sys.argv:
        return selfTest()

    texts = []
    for path in SOURCES:
        text = read(path)
        if text is None:
            print("check-blink-hint-chokepoint: FAILED — could not read %s, so nothing "
                  "was compared" % path, file=sys.stderr)
            return 1
        texts.append((path, text))

    offenders, total, arming = check(texts)

    if offenders is None:
        print("check-blink-hint-chokepoint: FAILED — no assignment to %s or %s in %s. The "
              "members were renamed or moved, so this compared nothing."
              % (FLAG, STAMP, " or ".join(SOURCES)), file=sys.stderr)
        return 1

    if offenders:
        status = gate_report.failed("check-blink-hint-chokepoint",
                                    "site(s) where the hint and its stamp can disagree",
                                    offenders)
        print("\n  A viewer retracts the hint only when its own scan is not older than the\n"
              "  stamp, so the two ways to break it are opposite. Arming without stamping\n"
              "  leaves a stamp OLDER than the write, and a viewer retracts against a\n"
              "  write it never saw. Moving the stamp without arming leaves one NEWER\n"
              "  than the blink it describes, and a viewer retracts against a window that\n"
              "  never existed. Neither fails at runtime — the flag still says blink, so\n"
              "  the terminal still blinks and only the clock stands down.", file=sys.stderr)
        return status

    print("check-blink-hint-chokepoint: ok (%d assignment(s) to %s/%s, every one paired; "
          "armed in %s alone)" % (total, FLAG, STAMP, CHOKE))
    return 0


if __name__ == "__main__":
    sys.exit(main())
