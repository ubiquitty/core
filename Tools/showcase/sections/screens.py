# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""Screens & scrollback: scroll regions, insert and delete, erase, tab stops,
rectangles, margins, selective erase, and the alternate screen.

Entries that change terminal-wide state (scroll region, margins, tab stops,
insert mode, protection) put it back before they return. The menu owns the
alternate screen, so the entry that leaves it enters it again and redraws.
"""

import time

from .. import tui as T
from ..tui import BOLD, CSI, ESC, RESET, fg_truecolor, move
from . import Entry

TITLE = "Screens and scrollback"
ORDER = 180

# Reset the scroll region to the full screen: DECSTBM with no parameters (CSI r).
DECSTBM_FULL = CSI + "r"
# Reset insert/replace mode to replace (the default): IRM off, CSI 4 l.
IRM_OFF = CSI + "4l"


def _demo_scroll_region(tui):
    row = tui.page(TITLE, "Five new lines scroll rows 8-14 only; the green rows outside stay put.")
    t = tui.theme
    top, bottom = 8, 14
    tui.label(row, 4, f"CSI {top} ; {bottom} r")
    tui.text(row, 40, "row 6 never moves", fg_truecolor(*t.accent2))
    tui.text(16, 40, "row 16 never moves", fg_truecolor(*t.accent2))
    tui.write(CSI + f"{top};{bottom}r")
    for r in range(top, bottom + 1):
        tui.text(r, 6, f"region line {r - top + 1}", tui.fg_default())
    tui.write(move(bottom, 6))
    for n in range(1, 6):
        tui.write("\r\n", CSI + "6G", fg_truecolor(*t.accent), f"scrolled-in line {n}", RESET)
    tui.write(DECSTBM_FULL)
    return tui.footer()


# A self-contained snapshot fixture exercising the insert/delete family from a
# clean home position: lay six rows, insert a blank line (IL), delete a whole
# line (DL), delete chars from the first row (DCH), open a gap mid-row (ICH),
# and insert under IRM.
_INSERT_DELETE_SEQ = (
    "\x1b[H"  # home
    "ALPHA   row A\r\nBRAVO   row B\r\nCHARLIE row C\r\n"
    "DELTA   row D\r\nECHO    row E\r\nFOXTROT row F\r\n"
    "\x1b[2;1H\x1b[L"  # IL: blank line at row 2 (pushes rows down)
    "\x1b[5;1H\x1b[M"  # DL: delete row 5 (pulls rows below up)
    "\x1b[1;1H\x1b[4P"  # DCH: delete 4 chars on row 1
    "\x1b[4;8H\x1b[4@"  # ICH: open a 4-cell gap
    "\x1b[9;1HIRM: 0123456789\x1b[9;6H\x1b[4hXXXX\x1b[4l"  # IRM insert
)


def _demo_insert_delete(tui):
    row = tui.page(TITLE, "ALPHA loses 4 letters, CHARLIE gets a gap, ECHO goes, XXXX pushes digits.")
    t = tui.theme
    base = row + 1
    for i, text in enumerate(["ALPHA   row A", "BRAVO   row B", "CHARLIE row C",
                              "DELTA   row D", "ECHO    row E", "FOXTROT row F"]):
        tui.text(base + i, 6, text, tui.fg_default())
    tui.write(move(base + 1, 6), CSI + "L")
    tui.label(base + 1, 40, "CSI L     IL, a blank row")
    tui.write(move(base + 5, 1), CSI + "M")
    tui.label(base + 5, 40, "CSI M     DL, ECHO removed")
    tui.write(move(base, 6), CSI + "4P")
    tui.label(base, 40, "CSI 4 P   DCH, four letters out")
    # IL pushed CHARLIE down a row, so the gap goes in at base + 3.
    tui.write(move(base + 3, 13), CSI + "4@")
    tui.label(base + 3, 40, "CSI 4 @   ICH, a four-cell gap")
    irm_row = base + 8
    tui.text(irm_row, 6, "0123456789", tui.fg_default())
    tui.write(move(irm_row, 6), CSI + "4h", fg_truecolor(*t.accent2), "XXXX", RESET, IRM_OFF)
    tui.label(irm_row, 40, "CSI 4 h   IRM, XXXX inserted")
    key = tui.footer()
    tui.write(IRM_OFF)
    return key


# ---------------------------------------------------------------------------
# ED / EL — erase in display & line, incl. xterm ED 3 (clear scrollback)
# ---------------------------------------------------------------------------


def _fill(tui, top, bottom, t):
    for r in range(top, bottom + 1):
        tui.write(move(r, 1), fg_truecolor(*t.accent2), "#" * tui.cols, RESET)


def _demo_erase(tui):
    caption = "Each bar keeps what EL spared; Space fills the screen and runs ED 0, 1, 2."
    while True:
        row = tui.page(TITLE, caption)
        t = tui.theme
        for i, (parameter, span) in enumerate([("0", "cursor to the end"),
                                               ("1", "start to cursor"),
                                               ("2", "the whole row")]):
            r = row + i
            tui.write(move(r, 6), fg_truecolor(*t.accent2), "#" * 40, RESET)
            tui.write(move(r, 26), CSI + parameter + "K")
            tui.label(r, 50, f"CSI {parameter} K  {span}")
        tui.label(row + 4, 4, "CSI 0 J  cursor to the end    CSI 1 J  start to cursor    CSI 2 J  all")
        tui.keys(("Space", "run ED"))
        tui.flush()
        key = tui.read_key()
        if key != " ":
            return key
        # Each ED runs from the middle of a full screen and holds long enough
        # to see which half it took.
        for parameter in ("0", "1", "2"):
            _fill(tui, 1, tui.rows, t)
            tui.write(move(tui.rows // 2, tui.cols // 2), CSI + parameter + "J")
            tui.flush()
            time.sleep(1.0)


# ---------------------------------------------------------------------------
# Tab stops — HTS / TBC / DECST8C / CHT / CBT
# ---------------------------------------------------------------------------


def _demo_tabs(tui):
    row = tui.page(TITLE, "A-D land under the marks; CHT and CBT count stops; CSI ? 5 W resets them.")
    t = tui.theme
    custom = [10, 24, 40, 58]
    tui.write(CSI + "3g")
    for column in custom:
        tui.write(move(row + 1, column), ESC + "H")
    tui.label(row, 4, "CSI 3 g, then ESC H at 10, 24, 40, 58")
    for column in custom:
        tui.text(row + 1, column, "v", fg_truecolor(*t.accent2))
    tui.write(move(row + 2, 1), tui.fg_default(), "\tA\tB\tC\tD", RESET)
    tui.write(move(row + 4, 1), CSI + "3I", fg_truecolor(*t.accent), "CSI 3 I from column 1", RESET)
    tui.write(move(row + 5, 58), CSI + "2Z", fg_truecolor(*t.title), "CSI 2 Z from column 58", RESET)
    tui.write(CSI + "?5W")
    tui.label(row + 7, 4, "CSI ? 5 W   stops every eighth column again")
    tui.write(move(row + 8, 1), tui.fg_default(), "1\t9\t17\t25\t33\t41\t49\t57\t65\t73", RESET)
    key = tui.footer()
    tui.write(CSI + "?5W")
    return key


# ---------------------------------------------------------------------------
# VT400 rectangular area ops — DECFRA / DECERA / DECCRA
# ---------------------------------------------------------------------------


def _demo_rect_ops(tui):
    row = tui.page(TITLE, "An X block, its copy 22 columns right, and a blank window in the digits.")
    t = tui.theme
    for r in range(9, 17):
        tui.write(move(r, 6), tui.fg_muted(), "".join(str(c % 10) for c in range(1, 41)), RESET)
    tui.write(fg_truecolor(*t.accent), CSI + "88;9;8;11;15$x", RESET)
    tui.label(row, 4, "CSI 88 ; 9 ; 8 ; 11 ; 15 $ x   DECFRA, fill with X")
    tui.write(fg_truecolor(*t.accent2), CSI + "9;8;11;15;1;13;30;1$v", RESET)
    tui.label(row + 1, 4, "CSI 9 ; 8 ; 11 ; 15 ; 1 ; 13 ; 30 ; 1 $ v   DECCRA, copy")
    tui.write(CSI + "14;18;16;26$z")
    tui.label(18, 4, "CSI 14 ; 18 ; 16 ; 26 $ z   DECERA, erase")
    return tui.footer()


# ---------------------------------------------------------------------------
# Left/right margins — DECLRMM (?69) enable + DECSLRM (CSI Pl;Pr s)
# ---------------------------------------------------------------------------

# DECLRMM (DECSET ?69) enables the left/right margin mode; with it on, DECSLRM
# (CSI Pl ; Pr s) sets a vertical margin band so text wraps and scrolls within
# columns Pl..Pr only. Reset: DECSLRM with no parameters, then DECLRMM off (?69l).
LRMM_ON = CSI + "?69h"
LRMM_OFF = CSI + "?69l"
DECSLRM_FULL = CSI + "s"  # DECSLRM with no parameters: full-width margins.

# Self-contained snapshot fixture: enable left/right margins at cols 20..50 and
# a top/bottom band at rows 10..16, then fill the band so the confinement is
# visible. Park at the bottom margin and emit overlong lines: with margins on,
# they wrap at column 50 and scroll only within the rectangle. Reset on the way
# out so the fixture leaves no margin / scroll-region state set.
_LR_MARGINS_SEQ = (
    LRMM_ON
    + CSI
    + "20;50s"
    + CSI
    + "10;16r"
    + CSI
    + "16;20H"
    + "".join(
        "this line runs past column 50 and wraps back to column 20\r\n"
        for _ in range(6)
    )
    + DECSLRM_FULL
    + LRMM_OFF
    + DECSTBM_FULL
)


def _demo_lr_margins(tui):
    row = tui.page(TITLE, "Each line breaks at column 50 and resumes at 20, inside the green bars.")
    t = tui.theme
    left, right = 20, 50
    top, bottom = 10, 16
    tui.label(row, 4, f"CSI ? 69 h  CSI {left} ; {right} s  CSI {top} ; {bottom} r")
    tui.label(row + 1, 4, "CSI s  CSI ? 69 l  CSI r   on the way out")
    tui.text(9, left - 1, "\u258e", fg_truecolor(*t.accent2))
    tui.text(9, right + 1, "\u258e", fg_truecolor(*t.accent2))
    tui.write(LRMM_ON, CSI + f"{left};{right}s", CSI + f"{top};{bottom}r", move(bottom, left))
    for n in range(1, 6):
        tui.write(fg_truecolor(*t.accent), f"line {n} runs past column {right} and wraps back to {left}",
                  RESET, "\r\n")
    tui.write(DECSLRM_FULL, LRMM_OFF, DECSTBM_FULL)
    return tui.footer()


# ---------------------------------------------------------------------------
# Selective erase in the display: DECSCA + DECSED (CSI ? J)
# ---------------------------------------------------------------------------

# DECSCA (CSI Ps " q) sets a per-cell 'protected' attribute: Ps=1 protects the
# following cells, Ps=0 (or 2) clears protection. DECSED (CSI ? Ps J) is the
# selective counterpart of ED — it erases UNPROTECTED cells across the display
# while sparing DECSCA-protected ones, whereas plain ED (CSI Ps J) ignores
# protection and clears everything. DECSERA (CSI Pt;Pl;Pb;Pr $ {) is the
# rectangular counterpart and is supported too, gated at VT400 like the other
# rectangle operations; this fixture uses the display-wide DECSED because a
# whole-screen erase makes the surviving island unmistakable.
DECSCA_PROT = CSI + '1"q'
DECSCA_OFF = CSI + '0"q'

# Self-contained snapshot fixture: print several rows with a DECSCA-protected
# [PROTECTED] island on the middle row, then DECSED 2 (CSI ? 2 J) — the whole
# display is requested erased but the protected island survives, proving
# selective erase honors DECSCA. DECSCA is reset on the way out.
_SELECTIVE_ED_SEQ = (
    "\x1b[2J\x1b[H"
    + "\x1b[10;6Hthis row carries no mark, so it goes -----------------"
    + "\x1b[11;6Hbefore [ "
    + DECSCA_PROT + "[PROTECTED]" + DECSCA_OFF
    + " ] after  (CSI ? 2 J takes every unmarked cell here)"
    + "\x1b[12;6Hthis row carries no mark either ----------------------"
    + "\x1b[?2J"  # DECSED 2: selective-erase display, sparing protected cells
    + DECSCA_OFF
)


def _demo_selective_ed(tui):
    caption = "Space erases the whole display except the protected [PROTECTED] cells."
    row = tui.page(TITLE, caption)
    t = tui.theme
    tui.label(row, 4, 'CSI 1 " q  [PROTECTED]  CSI 0 " q   then CSI ? 2 J')
    tui.write(move(row + 2, 6), tui.fg_default(), "this row carries no mark", RESET)
    tui.write(move(row + 3, 6), tui.fg_default(), "before ", RESET,
              DECSCA_PROT, fg_truecolor(*t.accent2), BOLD, "[PROTECTED]", RESET, DECSCA_OFF,
              tui.fg_default(), " after", RESET)
    tui.write(move(row + 4, 6), tui.fg_default(), "this row carries no mark either", RESET)
    tui.keys(("Space", "run it"))
    tui.flush()
    key = tui.read_key()
    if key != " ":
        tui.write(DECSCA_OFF)
        return key
    tui.write(CSI + "?2J")
    tui.keys()
    key = tui.wait_key()
    tui.write(DECSCA_OFF)
    return key


# ---------------------------------------------------------------------------
# Alternate screen buffer (?1049)
# ---------------------------------------------------------------------------


def _demo_alt_screen(tui):
    while True:
        row = tui.page(TITLE, "Space shows the primary screen behind the showcase for a second.")
        tui.label(row, 4, "CSI ? 1049 l   back to the primary screen and its cursor")
        tui.label(row + 1, 4, "CSI ? 1049 h   the alternate screen again, cleared")
        tui.keys(("Space", "run it"))
        tui.flush()
        key = tui.read_key()
        if key != " ":
            return key
        tui.write(CSI + "?1049l")
        tui.flush()
        time.sleep(1.2)
        tui.write(CSI + "?1049h")
        tui.flush()


ENTRIES = [
    Entry(
        title="Scroll region (DECSTBM)",
        explanation=(
            "CSI 8 ; 14 r holds scrolling inside rows 8-14: a newline at row 14 "
            "lifts only that band, and rows 6 and 16 keep their text. CSI r "
            "restores the full screen."
        ),
        run=_demo_scroll_region,
        sequence="\x1b[8;14r\x1bP$qr\x1b\\",
        expect=r"\x1bP1\$r8;14r\x1b\\",
        snapshot=False,
        golden="screens_decstbm_region",
    ),
    Entry(
        title="Insert and delete (IL, DL, ICH, DCH, IRM)",
        explanation=(
            "CSI L opens a blank row and CSI M removes one, moving the rows "
            "below; CSI 4 P deletes four characters and CSI 4 @ opens four "
            "blanks, moving the tail; under CSI 4 h typed text pushes the row "
            "right. CSI 4 l restores replace mode."
        ),
        run=_demo_insert_delete,
        sequence=_INSERT_DELETE_SEQ,
        golden="screens_insert_delete",
    ),
    Entry(
        title="Erase in line and display (EL, ED)",
        explanation=(
            "CSI 0 K erases from the cursor to the end of the row, CSI 1 K from "
            "the start to the cursor, CSI 2 K the whole row; CSI 0, 1, 2 J take "
            "the same spans over the display. CSI 3 J drops the scrollback and "
            "leaves the visible grid alone."
        ),
        run=_demo_erase,
        sequence="\x1b[3J",
        golden="screens_erase",
    ),
    Entry(
        title="Tab stops (HTS, TBC, CHT, CBT, DECST8C)",
        explanation=(
            "CSI 3 g clears every stop and ESC H sets one at the cursor column; "
            "CSI n I and CSI n Z move n stops forward and back; CSI ? 5 W puts a "
            "stop on every eighth column, and those stay set on return."
        ),
        run=_demo_tabs,
        sequence="\x1b[?5W\x1b[2$w",
        expect=r"\x1bP2\$u9/17/25/33/41/49/57/65/73\x1b\\",
        snapshot=False,
        golden="screens_tabs",
    ),
    Entry(
        title="Rectangular areas (DECFRA, DECCRA, DECERA)",
        explanation=(
            "DECFRA fills rows 9-11, columns 8-15 with X; DECCRA copies that "
            "block to row 13, column 30; DECERA blanks rows 14-16, columns "
            "18-26. Every edge is square against the digit grid."
        ),
        run=_demo_rect_ops,
        sequence="\x1b[88;9;8;11;15$x",
        golden="screens_rect_ops",
    ),
    Entry(
        title="Left and right margins (DECLRMM, DECSLRM)",
        explanation=(
            "With CSI ? 69 h, CSI 20 ; 50 s sets margins at columns 20 and 50: "
            "text wraps at 50 back to 20 and newlines scroll only the "
            "rectangle. The margins, the mode, and the scroll region are reset "
            "on exit."
        ),
        run=_demo_lr_margins,
        sequence=_LR_MARGINS_SEQ,
        golden="screens_lr_margins",
    ),
    Entry(
        title="Selective erase (DECSCA, DECSED)",
        explanation=(
            'CSI 1 " q protects the cells written after it; CSI ? 2 J erases the '
            "whole display except those cells, so [PROTECTED] stands alone. "
            'CSI 0 " q clears the protection on exit.'
        ),
        run=_demo_selective_ed,
        sequence=_SELECTIVE_ED_SEQ,
        golden="screens_selective_ed",
    ),
    Entry(
        title="Alternate screen (?1049)",
        explanation=(
            "CSI ? 1049 l returns to the primary screen with its content and "
            "cursor as they were; CSI ? 1049 h saves the cursor and switches to "
            "a cleared alternate screen, where the showcase redraws."
        ),
        run=_demo_alt_screen,
    ),
]
