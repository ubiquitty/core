#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""Check that the ECMA-48 conformance table agrees with the CSI dispatch table.

Docs/ecma48_conformance.md scores this terminal control by control, and its own
legend says what a mark means: ✅ is "dispatch table entry exists, handler is
functional". 185 rows make that claim and nothing reads them, so the document
asserts and the code answers and the two are related by whoever last edited
both.

The half that is mechanically checkable is the CSI table, because a row names
the final byte and `csiHandlers[]` is keyed on exactly that. Both directions
fail, and they catch different mistakes:

    a row marked ✅ or 🟡 with no handler   the doc claims a control this
                                            terminal does not dispatch
    a row marked ❌ that has a handler      the doc denies something shipped,
                                            which is how a control gets
                                            implemented twice

The second is the one worth having. A missing handler shows up in the end as a
sequence that does nothing; a stale ❌ shows up as nothing at all, and the next
person to read the table plans work that is already done.

What this does not check, stated rather than implied. That a handler is
*correct* is esctest2's job and the unit suite's — this compares presence
against presence. The DEC-private and mode sections are not compared: their rows are keyed on a
mode number rather than a final byte, so matching them against a dispatch table
means a different parse, and one that guesses is worse than one that declines.

Stdlib only.

    python3 Scripts/check-conformance-claims.py
"""

import os
import re
import sys

# Every mark the legend defines. A row carrying one is a conformance claim,
# whether or not this check is able to compare it.
MARKS = ("✅", "🟡", "❌", "⏭️", "⏭")

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, ".."))
DOC = os.path.join(REPO, "Docs", "ecma48_conformance.md")
DISPATCH = os.path.join(REPO, "Source", "Core", "VT", "VT.cpp")
ESCAPE = os.path.join(REPO, "Source", "Core", "VT", "Escape.cpp")

# A CSI row: acronym, final byte, ECMA-48 clause. The clause is what tells a
# CSI row from the several other tables in the file — a shape that is only
# "pipe, word, pipe, single character" also matches the legend and the
# divergence table.
CSI_ROW = re.compile(r"^\| ([A-Z][A-Z0-9]*)\s*\| ([A-Za-z@`{|}~\\^_])\s+\| 8\.3\.")

# A few controls travel the other way: the terminal emits them and never parses
# one, so their final byte is absent from a dispatch table by design. CPR is the
# case here, and asking it to have a handler reports a defect at a row that is
# correct. The doc already carries the discriminator in its notes, so this reads
# the reason it states rather than keeping a second list beside it — a hardcoded
# exemption would still be excusing CPR after the row's own note changed.
#
# Exempt from the dispatch comparison and not from checking: a send-only control
# is emitted by whatever control requests it, and that requesting control has a
# row and a final byte of its own. CPR is reachable only through DSR, whose 'n'
# is compared like every other row.
SEND_ONLY = re.compile(r"send-only|not received", re.I)

# An ESC row names its final in column/row notation: `ESC 04/05` is column 4,
# row 5, which is 0x45 — `E`, NEL. That is the same key `escapeHandlers[]` is
# built on, so the section compares the same way the CSI one does.
ESCAPE_ROW = re.compile(r"^\| ([A-Z][A-Z0-9]*)\s*\| [0-9/]+\s*\| ESC (\d\d)/(\d\d)\s*\|")

# Seven finals open a string rather than running a handler: the parser changes
# state on them and reads the payload that follows. They are implemented in the
# state machine, not in escapeHandlers[], so their absence from that table says
# nothing about whether the control works — the same shape as CPR, which is
# emitted rather than parsed.
#
# Each is named rather than pattern-matched, because "it introduces a string" is
# not something the doc row says and guessing it from the acronym would make the
# exemption unauditable.
STRING_INTRODUCERS = {
    "P": "DCS — parser enters the DCS state and reads to ST",
    "X": "SOS — parser enters the string state",
    "[": "CSI — parser enters the CSI state, csiHandlers[] takes it from there",
    "\\": "ST — terminates a string the parser is already inside",
    "]": "OSC — parser enters the OSC state",
    "^": "PM — parser enters the string state",
    "_": "APC — parser enters the string state",
}

# Finals this terminal dispatches that ECMA-48 does not define, so their absence
# from an ECMA-48 table is correct rather than a gap. Listed by name because an
# unexplained exemption is one nobody can audit later.
NON_ECMA_FINALS = {
    "r": "DECSTBM — DEC private",
    "s": "SCOSC — SCO",
    "t": "XTWINOPS — xterm",
    "u": "SCORC — SCO",
    "x": "DECREQTPARM — DEC private",
    "]": "Linux console power management",
}


def claimsAMark(line):
    """True for a row scoring a control, false for the legend row defining a mark.

    The legend carries every mark by construction — it is what the marks mean —
    and counting those rows as claims inflates the population this check reports
    itself against.
    """
    if not line.startswith("|"):
        return False
    cells = [cell.strip() for cell in line.strip().strip("|").split("|")]
    return len(cells) >= 4 and any(m in cell for cell in cells[1:] for m in MARKS)


def dispatchedFinals():
    """The CSI final bytes `csiHandlers[]` maps to a handler."""
    text = open(DISPATCH, encoding="utf-8").read()
    start = text.find("static constexpr CSIEntry csiHandlers[] = {")
    if start < 0:
        raise SystemExit("check-conformance-claims: no csiHandlers[] in %s — "
                         "the table moved or was renamed" % DISPATCH)
    body = text[start:text.index("};", start)]
    finals = set(re.findall(r"\{\s*'(\\?.)'\s*,", body))
    if not finals:
        raise SystemExit("check-conformance-claims: csiHandlers[] parsed as empty")
    return finals


def escapeFinals():
    """The ESC final bytes `escapeHandlers[]` maps to a handler."""
    text = open(ESCAPE, encoding="utf-8").read()
    start = text.find("static constexpr EscapeEntry escapeHandlers[] = {")
    if start < 0:
        raise SystemExit("check-conformance-claims: no escapeHandlers[] in %s — "
                         "the table moved or was renamed" % ESCAPE)
    body = text[start:text.index("};", start)]
    finals = set(re.findall(r"\{\s*'(\\?.)'\s*,", body))
    if not finals:
        raise SystemExit("check-conformance-claims: escapeHandlers[] parsed as empty")
    return finals


def claimedEscapes():
    """{final: (acronym, mark)} for every ESC row in the conformance table."""
    claims = {}
    for line in open(DOC, encoding="utf-8"):
        match = ESCAPE_ROW.match(line)
        if not match:
            continue
        mark = next((m for m in ("✅", "🟡", "❌", "⏭️", "⏭") if m in line), "?")
        claims[chr(int(match.group(2)) * 16 + int(match.group(3)))] = (match.group(1), mark)
    if not claims:
        raise SystemExit("check-conformance-claims: no ESC rows parsed from %s — "
                         "the table's shape changed" % DOC)
    return claims


def claimedFinals():
    """{final: (acronym, mark, sendOnly)} for every CSI row in the table."""
    claims = {}
    for line in open(DOC, encoding="utf-8"):
        match = CSI_ROW.match(line)
        if not match:
            continue
        mark = next((m for m in ("✅", "🟡", "❌", "⏭️", "⏭") if m in line), "?")
        claims[match.group(2)] = (match.group(1), mark, bool(SEND_ONLY.search(line)))
    if not claims:
        raise SystemExit("check-conformance-claims: no CSI rows parsed from %s — "
                         "the table's shape changed" % DOC)
    return claims


def main():
    dispatched = dispatchedFinals()
    claims = claimedFinals()
    failures = []

    for final, (acronym, mark, sendOnly) in sorted(claims.items()):
        present = final in dispatched
        if sendOnly:
            if present:
                failures.append("  %-5s '%s' is noted send-only but csiHandlers[] dispatches it "
                                "— the note or the handler is wrong" % (acronym, final))
            continue
        if mark in ("✅", "🟡") and not present:
            failures.append("  %-5s '%s' is marked %s but csiHandlers[] has no entry — "
                            "the table claims a control this terminal does not dispatch"
                            % (acronym, final, mark))
        if mark == "❌" and present:
            failures.append("  %-5s '%s' is marked ❌ but csiHandlers[] dispatches it — "
                            "the table denies something that shipped" % (acronym, final))

    escapes = claimedEscapes()
    escapeDispatched = escapeFinals()
    for final, (acronym, mark) in sorted(escapes.items()):
        if final in STRING_INTRODUCERS:
            continue
        present = final in escapeDispatched
        if mark in ("✅", "🟡") and not present:
            failures.append("  %-5s ESC '%s' is marked %s but escapeHandlers[] has no entry"
                            % (acronym, final, mark))
        if mark == "❌" and present:
            failures.append("  %-5s ESC '%s' is marked ❌ but escapeHandlers[] dispatches it — "
                            "the table denies something that shipped" % (acronym, final))

    # An introducer that grew a handler is a change in how it is parsed, and the
    # exemption below would then be describing a mechanism the code left.
    for final, why in sorted(STRING_INTRODUCERS.items()):
        if final in escapeDispatched:
            failures.append("  ESC '%s' is exempted as \"%s\" but escapeHandlers[] "
                            "dispatches it now" % (final, why))

    for final in sorted(dispatched - set(claims)):
        if final not in NON_ECMA_FINALS:
            failures.append("  '%s' is dispatched but has no row and no exemption. If it is "
                            "an ECMA-48 control it wants a row; if it is a private "
                            "extension it wants naming in NON_ECMA_FINALS." % final)

    # An exemption for a final nobody dispatches is a note about code that is
    # gone, and the list it sits in is the only place anyone would look for it.
    for final, why in sorted(NON_ECMA_FINALS.items()):
        if final not in dispatched:
            failures.append("  '%s' is exempted as \"%s\" but nothing dispatches it — "
                            "delete the exemption with the handler" % (final, why))

    if failures:
        print("check-conformance-claims: the conformance table and the dispatch table disagree",
              file=sys.stderr)
        print("\n".join(failures), file=sys.stderr)
        print("\n  %s" % os.path.relpath(DOC, REPO), file=sys.stderr)
        print("  %s" % os.path.relpath(DISPATCH, REPO), file=sys.stderr)
        return 1

    marks = {}
    for _, mark, _ in claims.values():
        marks[mark] = marks.get(mark, 0) + 1
    sendOnly = sum(1 for _, _, s in claims.values() if s)
    # The rows this cannot reach are counted and said out loud. A verdict naming
    # only what it compared reads as a verdict on the document, and most of the
    # document is not compared: the mode sections key on a mode number, and the
    # C0/C1 and column-row CSI sections spell their code as `04/05` rather than
    # as the final byte `csiHandlers[]` is built on. Each is a different parse.
    claimRows = sum(1 for line in open(DOC, encoding="utf-8")
                    if claimsAMark(line))
    compared = len(claims) + len(escapes)

    # The two counts are read by different patterns over one document, and a
    # verdict saying "73 of 313" only means anything if the 73 are among the 313.
    # A row the dispatch comparison reaches but claimsAMark does not would make
    # the fraction a comparison of two populations wearing one name — the shape
    # ubiquitty-performance once found in a render gate: a numerator of 108
    # tested against a denominator of 99, which nothing could ever reach. The tell is arithmetic that cannot be true, and it is checkable without
    # understanding either population.
    outside = [line for line in open(DOC, encoding="utf-8")
               if (CSI_ROW.match(line) or ESCAPE_ROW.match(line))
               and not claimsAMark(line)]
    if outside:
        print("check-conformance-claims: %d row(s) are compared against the "
              "dispatch table but fall outside" % len(outside), file=sys.stderr)
        print("  the marked-row population the verdict counts them against, so "
              "the fraction it prints", file=sys.stderr)
        print("  relates two different sets. First: %s"
              % outside[0].strip()[:90], file=sys.stderr)
        return 1
    print("check-conformance-claims: ok (%d of %d marked rows compared — %d CSI: "
          "%s; %d ESC. %d marked rows are spelled in a notation no bound here "
          "parses. %d send-only, %d private extensions, %d string introducers)"
          % (compared, claimRows, len(claims),
             ", ".join("%s %d" % (m, n) for m, n in sorted(marks.items())),
             len(escapes), claimRows - compared,
             sendOnly, len(NON_ECMA_FINALS), len(STRING_INTRODUCERS)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
