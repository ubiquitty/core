# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""Cursor: shape, visibility, color, save and restore, and movement.

Entries that change the cursor's shape, color, or visibility put the default
back (CSI 0 SP q, OSC 112, CSI ?25l, the menu's hidden cursor) before they
return.
"""

import time

from .. import tui as T
from ..tui import CSI, OSC, ST, BEL, RESET, BOLD, move, fg_truecolor
from . import Entry

TITLE = "Cursor"
ORDER = 170


def _decscusr(ps):
    return f"{CSI}{ps} q"


def _restore(tui):
    tui.write(_decscusr(0), T.HIDE_CURSOR)
    tui.flush()


_SHAPES = [
    (0, "the configured shape"),
    (1, "blinking block"),
    (2, "steady block"),
    (3, "blinking underline"),
    (4, "steady underline"),
    (5, "blinking bar"),
    (6, "steady bar"),
]


def _demo_shapes(tui):
    row = tui.page(TITLE, "The cursor after the arrow takes each shape as Up and Down step the list.")
    t = tui.theme
    index = 0
    while True:
        for i, (ps, name) in enumerate(_SHAPES):
            line = f"CSI {ps} SP q   {name}"
            if i == index:
                tui.text(row + 2 + i, 4, "> " + line, fg_truecolor(*t.accent), BOLD)
            else:
                tui.text(row + 2 + i, 4, "  " + line, tui.fg_muted())
        tui.text(row, 4, "cursor ->", tui.fg_default())
        tui.keys(("Up/Down", "change shape"))
        tui.write(_decscusr(_SHAPES[index][0]), T.SHOW_CURSOR, move(row, 15))
        tui.flush()
        key = tui.read_key()
        if key in ("up", "k"):
            index = (index - 1) % len(_SHAPES)
        elif key in ("down", "j"):
            index = (index + 1) % len(_SHAPES)
        else:
            break
    _restore(tui)
    return key


def _demo_visibility(tui):
    row = tui.page(TITLE, "The block after the arrow appears and disappears; nothing else moves.")
    visible = True
    while True:
        tui.text(row, 4, "cursor ->", tui.fg_default())
        tui.label(row + 2, 4, "CSI ?25 h   shown " if visible else "CSI ?25 l   hidden")
        tui.keys(("Space", "toggle"))
        tui.write(_decscusr(2), T.SHOW_CURSOR if visible else T.HIDE_CURSOR, move(row, 15))
        tui.flush()
        key = tui.read_key()
        if key != " ":
            break
        visible = not visible
    _restore(tui)
    return key


_CURSOR_HUES = ["#f7768e", "#e0af68", "#9ece6a", "#7aa2f7", "#bb9af7"]


def _demo_color(tui):
    row = tui.page(TITLE, "The block after the arrow turns red, amber, green, blue, then purple.")
    tui.text(row, 4, "cursor ->", tui.fg_default())
    tui.label(row + 2, 4, "OSC 12 ; <color> BEL   five colors, 150 ms apart")
    tui.label(row + 3, 4, "OSC 112 ST             the default color, on the way out")
    tui.keys(("Space", "run it"))
    tui.write(_decscusr(2), T.SHOW_CURSOR, move(row, 15))
    tui.flush()
    while True:
        key = tui.read_key()
        if key != " ":
            break
        for color in _CURSOR_HUES:
            tui.write(f"{OSC}12;{color}{BEL}", move(row, 15))
            tui.flush()
            time.sleep(0.15)
    tui.write(f"{OSC}112{ST}")
    _restore(tui)
    return key


def _demo_save_restore(tui):
    row = tui.page(TITLE, "Each star lands right after its label, on the cell saved before the move.")
    t = tui.theme
    tui.write(_decscusr(2), T.SHOW_CURSOR)

    # The anchor is the cell right after the label, so a correct restore puts
    # the star beside the label and a wrong one overwrites a letter or lands
    # on the far line.
    passes = [
        ("ESC 7 ... ESC 8", T.SAVE_CURSOR, T.RESTORE_CURSOR, t.accent2),
        ("CSI s ... CSI u", f"{CSI}s", f"{CSI}u", t.accent),
    ]
    for i, (name, save, restore, color) in enumerate(passes):
        top = row + i * 5
        tui.text(top, 4, name + ":", BOLD, tui.fg_default())
        anchor = 4 + len(name) + 1
        tui.write(move(top, anchor), save)
        tui.label(top + 2, anchor, "written between the save and the restore")
        tui.write(restore, fg_truecolor(*color), BOLD, "*", RESET)
    key = tui.footer()
    _restore(tui)
    return key


def _demo_positioning(tui):
    row = tui.page(TITLE, "Cursor moves alone draw the plus, then put @ and * on its center row.")
    t = tui.theme
    cr, cc = row + 3, 10
    dot = fg_truecolor(*t.accent) + BOLD + "+" + RESET
    tui.write(move(cr, cc), dot)
    for step in ("1A", "1B", "1C", "1D"):
        tui.write(move(cr, cc), f"{CSI}{step}", dot)
    tui.write(f"{CSI}{cc + 6}G", f"{CSI}{cr}d", fg_truecolor(*t.accent2), BOLD, "@", RESET)
    tui.write(f"{CSI}{cc + 12}`", fg_truecolor(*t.title), BOLD, "*", RESET)

    legend = [
        "CUP  CSI r ; c H   center of the plus",
        "CUU  CSI A         arm above",
        "CUD  CSI B         arm below",
        "CUF  CSI C         arm right",
        "CUB  CSI D         arm left",
        "CHA  CSI G         column of @",
        "VPA  CSI d         row of @",
        "HPA  CSI `         column of *",
    ]
    for i, line in enumerate(legend):
        tui.label(row + 1 + i, 32, line)
    return tui.footer()


ENTRIES = [
    Entry(
        title="Cursor shape (DECSCUSR)",
        explanation=(
            "CSI Ps SP q sets the cursor shape: 0 the configured shape, 1 and 2 "
            "block, 3 and 4 underline, 5 and 6 bar; odd values blink, even values "
            "are steady. CSI 0 SP q restores the configured shape on return."),
        run=_demo_shapes,
        sequence="\x1b[2 q\x1bP$q q\x1b\\",
        expect=r"\x1bP1\$r2 q\x1b\\",
        snapshot=False,
        golden="cursor_decscusr_steady_block",
    ),
    Entry(
        title="Cursor visibility (DECTCEM)",
        explanation=(
            "CSI ?25 l hides the cursor and CSI ?25 h shows it again in the same "
            "cell; no other cell changes."),
        run=_demo_visibility,
        sequence="\x1b[?25l",
        golden="cursor_dectcem_hidden",
    ),
    Entry(
        title="Cursor color (OSC 12, OSC 112)",
        explanation=(
            "OSC 12 ; <color> sets the cursor color and OSC 112 restores the "
            "terminal's own, which goes out before the entry returns."),
        run=_demo_color,
        sequence="\x1b]12;#f7768e\x07",
        golden="cursor_osc12_color",
    ),
    Entry(
        title="Save and restore (DECSC, DECRC, CSI s, CSI u)",
        explanation=(
            "ESC 7 and CSI s save the cursor position; ESC 8 and CSI u return to "
            "it. After writing elsewhere, the restore puts the star on the saved "
            "cell right after its label."),
        run=_demo_save_restore,
        sequence="\x1b[3;5H\x1b7\x1b[10;20H\x1b8\x1b[6n",
        expect=r"\x1b\[3;5R",
        snapshot=False,
        golden="cursor_save_restore",
    ),
    Entry(
        title="Cursor movement (CUP, CUU-CUB, CHA, VPA, HPA)",
        explanation=(
            "CUP places the center of a plus and CUU, CUD, CUF and CUB its four "
            "arms; CHA and VPA put @ on the center row by absolute column and "
            "row, and HPA puts * further right. The plus is symmetric."),
        run=_demo_positioning,
        sequence="\x1b[13;24H",
        golden="cursor_positioning_sampler",
    ),
]
