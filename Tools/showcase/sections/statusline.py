# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""Status line: the DEC status display (DECSSDT, DECSASD).

The status line is one row with its own cells and cursor, drawn over the
bottom row of the grid, which is where the key bar sits. So each entry shows
it on Space for a moment and then tears it down (DECSASD 0, DECSSDT 0) before
the key bar comes back.
"""

import time

from ..tui import CSI, RESET, BOLD, move, fg_truecolor, bg_truecolor
from . import Entry

TITLE = "Status line"
ORDER = 200

DECSSDT_NONE = CSI + "0$~"
DECSSDT_INDICATOR = CSI + "1$~"
DECSSDT_HOST = CSI + "2$~"
DECSASD_MAIN = CSI + "0$}"
DECSASD_STATUS = CSI + "1$}"

_HOLD = 2.5


def _reset_status(tui):
    tui.write(DECSASD_MAIN, DECSSDT_NONE)
    tui.flush()


def _show(tui, caption, labels, setup, body=None):
    """Draw the page; on Space send `setup`, hold, and tear the line down."""
    while True:
        row = tui.page(TITLE, caption)
        for i, text in enumerate(labels):
            tui.label(row + i, 4, text)
        if body:
            body(row + len(labels) + 1)
        tui.keys(("Space", "show it"))
        tui.flush()
        key = tui.read_key()
        if key != " ":
            _reset_status(tui)
            return key
        tui.write(setup)
        tui.flush()
        time.sleep(_HOLD)
        _reset_status(tui)


def _demo_decssdt(tui):
    return _show(
        tui, "Space puts the terminal's own indicator line on the bottom row.",
        ["CSI 1 $ ~   indicator status line, filled by the terminal",
         "CSI 0 $ ~   no status line, after 2.5 seconds"],
        DECSSDT_INDICATOR)


def _demo_decsasd(tui):
    t = tui.theme
    text = (bg_truecolor(*t.panel_alt) + fg_truecolor(*t.highlight_fg) + BOLD
            + " written by the host through DECSASD " + RESET)
    return _show(
        tui, "Space writes a highlighted line onto the status row, not into the page.",
        ["CSI 2 $ ~   host-writable status line",
         "CSI 1 $ }   aim writes at it, write, CSI 0 $ } aim back"],
        DECSSDT_HOST + DECSASD_STATUS + move(1, 1) + text + DECSASD_MAIN)


# Powerline separators sit in the private use area, so they come from the
# user's font rather than from Unicode. The string below joins its segments
# with the solid wedge alone.
_SEP_SOLID = ""   # U+E0B0 solid right-pointing wedge


def _segment(fg, bg, text):
    return bg_truecolor(*bg) + fg_truecolor(*fg) + BOLD + text + RESET


def _statusline_string():
    """Build a Powerline-ish status string with truecolor segment backgrounds.

    Each segment carries its own background; the wedge between two segments is
    painted in the left segment's fill over the right segment's, which is what
    makes the two read as one chained strip rather than two blocks.
    """
    seg_bg = [(40, 110, 60), (50, 90, 160), (90, 60, 130), (60, 64, 80)]
    seg_fg = (236, 242, 255)
    labels = [" MODE ", " ~/ubiquitty ", " main ", " 12:00 "]
    out = []
    for i, (bg, label) in enumerate(zip(seg_bg, labels)):
        out.append(_segment(seg_fg, bg, label))
        nxt = seg_bg[i + 1] if i + 1 < len(seg_bg) else None
        if nxt is not None:
            # Solid wedge: prev fill as fg, next fill as bg.
            out.append(bg_truecolor(*nxt) + fg_truecolor(*bg)
                       + _SEP_SOLID + RESET)
        else:
            out.append(fg_truecolor(*bg) + _SEP_SOLID + RESET)
    return "".join(out)


def _demo_full(tui):
    status = _statusline_string()

    def body(row):
        tui.label(row, 4, "The same string in the grid:")
        tui.write(move(row + 1, 4), status)

    return _show(
        tui, "Space puts four colored segments, joined by wedges, on the status row.",
        ["CSI 2 $ ~  CSI 1 $ }  <segments>  CSI 0 $ }"],
        DECSSDT_HOST + DECSASD_STATUS + move(1, 1) + status + DECSASD_MAIN, body)


ENTRIES = [
    Entry(
        title="Indicator status line (DECSSDT)",
        explanation=(
            "CSI 1 $ ~ gives the bottom row to the terminal, which shows the "
            "cursor's line and column there and follows it; CSI 2 $ ~ gives it "
            "to the host; CSI 0 $ ~ returns it to the grid."),
        run=_demo_decssdt,
        sequence="\x1b[2$~\x1bP$q$~\x1b\\",
        expect=r"\x1bP1\$r2\$~\x1b\\",
        snapshot=False,
        golden="statusline_decssdt_host",
    ),
    Entry(
        title="Host-written status line (DECSASD)",
        explanation=(
            "After CSI 2 $ ~, CSI 1 $ } sends writes to the status row from its "
            "first column and CSI 0 $ } sends them back to the grid; the text "
            "stays on the status row and none of it reaches the grid."),
        run=_demo_decsasd,
        sequence="\x1b[2$~\x1b[1$}\x1b[0$}\x1bP$q$}\x1b\\",
        expect=r"\x1bP1\$r0\$}\x1b\\",
        snapshot=False,
        golden="statusline_decsasd_switch",
    ),
    Entry(
        title="Powerline status line",
        explanation=(
            "Four truecolor segments joined by U+E0B0 wedges, written through "
            "DECSASD, stand on the status row with no seam between a segment and "
            "its wedge."),
        run=_demo_full,
        sequence=(
            "\x1b[2$~\x1b[1$}"
            "\x1b[48:2::40:110:60m MODE \x1b[0m"
            "\x1b[0$}"),
        golden="statusline_powerline",
    ),
]
