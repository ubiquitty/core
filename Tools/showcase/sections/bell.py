# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""Bell & notifications: BEL, desktop notifications (OSC 9, OSC 777, OSC 99),
DECPS tones, the margin bell (mode 44) and the bell-attention modes 1042/1043.

None of these write a cell, so each entry acts on Space and prints what it
sent. Entries that set a mode clear it before returning.
"""

import time

from ..tui import BEL, CSI, OSC, ST, move
from . import Entry

TITLE = "Bell and notifications"
ORDER = 110

# How long the attention entry waits before ringing, so the window can be
# put in the background first.
_ATTENTION_DELAY = 3


def _act_page(tui, caption, labels, verb, act):
    """Draw labels; each Space runs `act`, which returns a result line."""
    row = tui.page(TITLE, caption)
    for offset, line in enumerate(labels):
        tui.label(row + offset, 4, line)
    result_row = row + len(labels) + 1
    while True:
        tui.keys(("Space", verb))
        tui.flush()
        key = tui.read_key()
        if key != " ":
            return key
        tui.result(result_row, 4, act())


def _demo_bel(tui):
    def act():
        tui.write(BEL)
        tui.flush()
        return "Sent BEL."
    return _act_page(tui, "Space rings the bell: a beep or one flash, and no cell changes.",
                     ["BEL   0x07, written ^G"], "send BEL", act)


_NOTIFY_SEQ = (
    f"{OSC}9;OSC 9 carries a body and no title{ST}"
    f"{OSC}777;notify;OSC 777 notify;this body follows the title{ST}"
    f"{OSC}99;;OSC 99 carries a body and empty metadata{ST}")


def _demo_notify(tui):
    def act():
        tui.write(_NOTIFY_SEQ)
        tui.flush()
        return "Sent three notifications."
    return _act_page(
        tui, "Space posts three desktop notifications; the grid does not change.",
        ["OSC 9 ; <body> ST",
         "OSC 777 ; notify ; <title> ; <body> ST",
         "OSC 99 ; ; <body> ST"],
        "notify", act)


def _demo_decps(tui):
    def act():
        tui.write(f"{CSI}5;8;1,~", f"{CSI}5;16;1;5;8,~")
        tui.flush()
        return "Sent one note, then three."
    return _act_page(
        tui, "Space plays one short tone, then three tones one after another.",
        ["CSI 5 ; 8 ; 1 , ~          volume 5, 8/32 s, note C5",
         "CSI 5 ; 16 ; 1 ; 5 ; 8 , ~  volume 5, 16/32 s, three notes in sequence"],
        "play", act)


def _demo_margin_bell(tui):
    row = tui.page(TITLE, "Typed keys echo toward the right edge; the bell rings at the mark.")
    # Core rings when a printed character moves the cursor onto column
    # columns - 10 (zero-based), so the character typed into the column
    # before it is the one that rings.
    ring_column = tui.cols - 10
    start_column = ring_column - 14
    line_row = row + 3
    tui.label(row, 4, "CSI ? 44 h   ring once per line as text reaches the margin")
    tui.label(line_row + 1, ring_column, "^ bell")
    tui.keys(("Space", "start typing"))
    tui.flush()
    key = tui.read_key()
    if key != " ":
        return key
    tui.label(row + 1, 4, "Type; Enter starts the line again.")
    tui.keys(("Enter", "new line"))
    # Set only once nothing else will print near the margin: the key bar
    # itself spans the full width and would ring.
    tui.write(CSI + "?44h", move(line_row, start_column), CSI + "?25h")
    tui.flush()
    column = start_column
    try:
        while True:
            key = tui.read_key()
            if key == "enter":
                tui.write(move(line_row, start_column), CSI + "K")
                column = start_column
            elif isinstance(key, str) and len(key) == 1 and key.isprintable():
                if column <= tui.cols:
                    tui.write(key)
                    column += 1
            elif key != "resize":
                return key
            tui.flush()
    finally:
        tui.write(CSI + "?44l", CSI + "?25l")
        tui.flush()


def _demo_attention(tui):
    def act():
        tui.write(CSI + "?1042h", CSI + "?1043h")
        tui.flush()
        time.sleep(_ATTENTION_DELAY)
        tui.write(BEL, CSI + "?1042l", CSI + "?1043l")
        tui.flush()
        return "Rang BEL with both modes set, then cleared them."
    return _act_page(
        tui, f"Space rings in {_ATTENTION_DELAY} seconds; switch away and the window asks for attention.",
        ["CSI ? 1042 h   a BEL flags the Dock or taskbar entry",
         "CSI ? 1043 h   a BEL brings the window to the front",
         "BEL, then CSI ? 1042 l and CSI ? 1043 l"],
        "ring later", act)


ENTRIES = [
    Entry(
        title="Bell (BEL)",
        explanation=(
            "BEL (0x07) rings the bell: the system beep or the terminal's own "
            "sound, or one flash when the bell is visual. No cell is written."),
        run=_demo_bel,
        sequence="\x07",
        golden="bell_bel",
        descriptive=True,
    ),
    Entry(
        title="Desktop notification (OSC 9, 777, 99)",
        explanation=(
            "OSC 9 ; <body>, OSC 777 ; notify ; <title> ; <body> and OSC 99 ; "
            "<metadata> ; <body> each post a desktop notification with that "
            "title and body. The grid is unchanged."),
        run=_demo_notify,
        sequence=_NOTIFY_SEQ,
        golden="bell_notify",
        descriptive=True,
    ),
    Entry(
        title="Play sound (DECPS)",
        explanation=(
            "CSI Pvol ; Pdur ; Pnote ... , ~ plays its notes in sequence at "
            "volume 0-7, for Pdur 32nds of a second, notes 1 (C5) to 25 (C7). "
            "One tone sounds, then three. No cell is written."),
        run=_demo_decps,
        sequence="\x1b[5;8;1,~\x1b[5;16;1;5;8,~",
        golden="bell_decps",
        descriptive=True,
    ),
    Entry(
        title="Margin bell (?44)",
        explanation=(
            "With CSI ? 44 h the bell rings once per line when printed text "
            "reaches ten columns from the right edge; a carriage return re-arms "
            "it. CSI ? 44 l clears the mode on return."),
        run=_demo_margin_bell,
        sequence="\x1b[?44h\x1b[?44$p",
        expect=r"\x1b\[\?44;1\$y",
        snapshot=False,
        golden="bell_margin",
    ),
    Entry(
        title="Bell attention (?1042, ?1043)",
        explanation=(
            "With CSI ? 1042 h a BEL at a background window flags its Dock or "
            "taskbar entry; with CSI ? 1043 h it brings the window to the front. "
            "Both are cleared after the ring."),
        run=_demo_attention,
        sequence="\x1b[?1042h\x1b[?1043h\x07\x1b[?1042$p\x1b[?1043$p",
        expect=r"\x1b\[\?1042;1\$y\x1b\[\?1043;1\$y",
        snapshot=False,
        golden="bell_attention",
    ),
]
