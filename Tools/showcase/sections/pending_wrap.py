# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""Pending wrap and the right-margin edge cases.

Entries that change autowrap (?7) or reverse-wrap (?45) put both back to their
defaults, with DECSCA and the scroll region, before they return.
"""

from ..tui import CSI, RESET, fg_truecolor, move
from . import Entry

TITLE = "Pending wrap"
ORDER = 190

# Mode set/reset helpers (DEC private modes use the '?' intermediate).
AUTOWRAP_ON = CSI + "?7h"  # DECAWM on: wrap to next line at right margin.
AUTOWRAP_OFF = CSI + "?7l"  # DECAWM off: overprint the last column.
REVWRAP_ON = CSI + "?45h"  # reverse-wrap on (xterm ?45).
REVWRAP_OFF = CSI + "?45l"  # reverse-wrap off (the default).
DECSCA_OFF = CSI + '0"q'  # DECSCA: clear protection (the default).
DECSTBM_FULL = CSI + "r"  # DECSTBM with no parameters: full-screen region.


def _restore_defaults(tui):
    """Return autowrap, reverse-wrap, DECSCA, and the scroll region to their
    power-on defaults so the menu renders in a known mode."""
    tui.write(AUTOWRAP_ON, REVWRAP_OFF, DECSCA_OFF, DECSTBM_FULL)


# ---------------------------------------------------------------------------
# Pending wrap (the deferred-wrap / "last column" state)
# ---------------------------------------------------------------------------

# A self-contained snapshot fixture for the deferred-wrap behavior on an 80-col
# grid: park at the last column (CSI 13;80H), print 'A' to arm pending wrap,
# print 'B' (which performs the deferred wrap to row 14 col 1); then arm again
# with 'C' at row 20 col 80, move left (CUB) to CANCEL the pending wrap, and
# print 'D' on the same row. Matches what _demo_pending_wrap emits.
_PENDING_WRAP_SEQ = "\x1b[13;80HAB\x1b[20;80HC\x1b[1DD"


# ---------------------------------------------------------------------------
# Self-contained snapshot fixtures for the mode-set demos
#
# The golden harness (Tests/Snapshots/run.py) feeds only
# Entry.sequence into a fresh 80x24 grid — never the _demo_*() body. A bare
# mode-set escape would render a blank screen, so each fixture below prints
# reference content then issues the escape so the snapshot shows the EFFECT,
# modeled on _PENDING_WRAP_SEQ above.
# ---------------------------------------------------------------------------

# Reverse-wrap (?45): fill row 12 to its last column, a '.' marker, and let one
# more character wrap onto row 13, so row 13 is a soft-wrapped continuation of
# row 12. With autowrap and reverse-wrap on, two BS from column 2 of row 13
# step back onto row 12's last column, where '<' overprints the marker —
# proving the cursor crossed the row boundary. Mode 45 goes back only onto a
# row that wrapped into the current one (xterm, patch 383 and later), so a row
# 13 reached by CUP would keep the cursor at column 1; the wrap is what makes
# the step back possible.
_REVERSE_WRAP_ROW = "row 12 " + "." * 73  # 80 columns; the last is the marker
_REVERSE_WRAP_SEQ = (
    "\x1b[11;1H" + "row 12 wraps onto 13; under CSI ?45 h two BS step back, and < replaces the dot"
    "\x1b[?7h\x1b[?45h"  # autowrap on + reverse-wrap on
    "\x1b[12;1H" + _REVERSE_WRAP_ROW  # 80 characters, the wrap armed
    + "x"  # the 81st wraps onto row 13, column 1: row 13 continues row 12
    + "\b\b"  # to column 1 of row 13, then back onto row 12's last column
    + "<"  # overprints the marker
    + "\x1b[?45l"  # restore default (reverse-wrap off)
)

# Autowrap off (?7l): disable autowrap, park 4 cells short of the right margin,
# then write more characters than fit. With wrap off the surplus glyphs all
# overprint the final column, so the visible last cell shows only the last glyph
# ('G') and the row does not wrap to the next line.
_AUTOWRAP_OFF_SEQ = (
    "\x1b[11;1Hautowrap off (?7l): cols 77-79 hold 123, col 80 only G, the row never wraps"
    "\x1b[?7l"  # DECAWM off
    "\x1b[12;77H1234567890ABCDEFG"  # cols 77..80 then overprint col 80
    "\x1b[?7h"  # restore autowrap on
)


def _demo_pending_wrap(tui):
    tui.page(TITLE, "B wraps to row 14 after A; CSI D cancels C's wrap, so D stays on row 20.")
    t = tui.theme
    last = tui.cols
    tui.label(11, 4, "CSI 13 ; 80 H  A  B")
    tui.write(move(13, last), fg_truecolor(*t.accent2), "A", "B", RESET)
    tui.label(18, 4, "CSI 20 ; 80 H  C  CSI 1 D  D")
    tui.write(move(20, last), fg_truecolor(*t.accent2), "C", CSI + "1D",
              fg_truecolor(*t.title), "D", RESET)
    key = tui.footer()
    _restore_defaults(tui)
    return key


def _demo_reverse_wrap(tui):
    row = tui.page(TITLE, "Row 11 wraps onto row 12; two BS step back onto row 11, where < lands.")
    t = tui.theme
    last = tui.cols
    tui.label(row, 4, "CSI ?7 h  CSI ?45 h   autowrap and reverse-wrap on")
    tui.label(row + 1, 4, f"CSI 11 ; 1 H  row 11 \u00b7\u00b7\u00b7 ({last} columns)  x  BS BS  <")
    # Row 11 is filled to its last column, so the x wraps onto row 12 and row 12
    # continues row 11; the two BS then step back across the wrap.
    tui.write(AUTOWRAP_ON, REVWRAP_ON, move(11, 1), tui.fg_muted(), "row 11 ", "\u00b7" * (last - 7),
              fg_truecolor(*t.accent2), "x", "\b\b", fg_truecolor(*t.title), "<", RESET)
    key = tui.footer()
    _restore_defaults(tui)
    return key


def _demo_autowrap_off(tui):
    row = tui.page(TITLE, "1234 fill four cells before the edge; the edge cell shows only G.")
    t = tui.theme
    last = tui.cols
    tui.label(row, 4, f"CSI ?7 l  CSI 11 ; {last - 4} H  1234567890ABCDEFG  CSI ?7 h")
    # Wrap stays off for the demo string alone: anything written after it
    # with DECAWM reset would collapse onto the last column too.
    tui.write(AUTOWRAP_OFF, move(11, last - 4), fg_truecolor(*t.accent2),
              "1234567890ABCDEFG", RESET, AUTOWRAP_ON)
    key = tui.footer()
    _restore_defaults(tui)
    return key


ENTRIES = [
    Entry(
        title="Pending wrap (last column)",
        explanation=(
            "A glyph printed in the last column leaves the cursor there with the "
            "wrap armed; the next glyph wraps to column 1 of the next row. A "
            "cursor move clears the armed wrap, so after CSI D the next glyph "
            "stays on the same row."
        ),
        run=_demo_pending_wrap,
        sequence=_PENDING_WRAP_SEQ,
        golden="pending_wrap_deferred",
    ),
    Entry(
        title="Reverse wrap (?45)",
        explanation=(
            "With CSI ?7 h and CSI ?45 h set, a BS at column 1 of a row that "
            "wrapped from the row above moves the cursor to that row's last "
            "column, as xterm has it since patch 383; a row reached by a cursor "
            "move keeps the cursor at column 1 (CSI ?1045 h wraps without that "
            "condition). Both modes return to their defaults, ?7 set and ?45 "
            "reset, on exit."
        ),
        run=_demo_reverse_wrap,
        sequence=_REVERSE_WRAP_SEQ,
        golden="pending_wrap_reverse_wrap",
    ),
    Entry(
        title="Autowrap off (?7l)",
        explanation=(
            "With CSI ?7 l the cursor holds at the last column and each further "
            "glyph overwrites that cell, so a long string stays on one row and "
            "the edge cell shows the glyph written last."
        ),
        run=_demo_autowrap_off,
        sequence=_AUTOWRAP_OFF_SEQ,
        golden="pending_wrap_autowrap_off",
    ),
]
