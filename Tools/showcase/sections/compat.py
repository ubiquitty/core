# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""Compatibility: the Tektronix 4014 grammar beside VT/ANSI.

The Tek entry enters Tek mode (CSI ? 38 h), draws with GS vector chains, and
always sends ESC ETX on the way out so the menu is back on VT/ANSI.
"""

from ..tui import ESC, CSI
from . import Entry

TITLE = "Compatibility"
ORDER = 260


# ---------------------------------------------------------------------------
# Tektronix 4014 mode entry / exit
#
# Ubiquitty enters Tek mode on DECSET 38 (CSI ? 38 h) and routes every following
# byte into its Tek storage-tube state machine. There is no DECRST-38 exit; the
# defined way back to VT is an in-stream ESC ETX (or ESC can), handled inside
# the Tek engine. Both sequences are emitted to be safe.
# ---------------------------------------------------------------------------

TEK_ENTER = CSI + "?38h"          # DECSET 38 — switch the host stream into Tek
TEK_EXIT = ESC + "\x03"           # ESC ETX — return to VT (ESC can is equivalent)

GS = "\x1d"                       # GS — Tek graph (vector) mode; next addr is a move


def _tek_addr(x, y):
    """Encode a Tek 4014 graphic address (x, y) as 4 coordinate bytes.

    The 4014 sends Hi-Y, Lo-Y, Hi-X, Lo-X. Each byte carries a 2-bit tag in
    the top bits plus 5 data bits: Hi bytes hold the upper coordinate bits and
    Lo bytes the lower ones. Tags are 0x20 for Hi-Y/Hi-X, 0x60 for Lo-Y, 0x40
    for Lo-X — which is how the decoder classifies each byte by its tag. The
    low two coordinate bits (the optional EGM "extra" byte) are omitted, so
    addresses land on a 4-unit grid; that is plenty for a simple shape.
    """
    hi_y = 0x20 | ((y >> 7) & 0x1F)
    lo_y = 0x60 | ((y >> 2) & 0x1F)
    hi_x = 0x20 | ((x >> 7) & 0x1F)
    lo_x = 0x40 | ((x >> 2) & 0x1F)
    return "".join(chr(b) for b in (hi_y, lo_y, hi_x, lo_x))


def _tek_vectors(points):
    """Build a GS-introduced vector chain through `points` [(x, y), ...].

    GS enters vector mode; the first address after GS is a dark move (pen up),
    each subsequent address draws a line from the previous point. Returns the
    full byte string (GS + coordinate groups).
    """
    out = [GS]
    for x, y in points:
        out.append(_tek_addr(x, y))
    return "".join(out)


# A small closed shape (a house outline) in the 4014's ~1024x780 coordinate
# space; the first point is the pen-up move, the rest trace the outline.
# Coordinates span the 4014's 4096x3120 addressable space so the house fills
# most of the screen (centered) rather than a small corner.
_TEK_SHAPE = [
    (1000, 600), (3000, 600), (3000, 2000), (1000, 2000), (1000, 600),  # box
    (1000, 2000), (2000, 2700), (3000, 2000),                           # roof
]

# The probe enters Tek and draws, so the snapshot holds the picture; a live
# run always follows it with TEK_EXIT.
_TEK_SEQUENCE = TEK_ENTER + _tek_vectors(_TEK_SHAPE)


def _tek_page(tui):
    row = tui.page(TITLE, "Space switches to Tek 4014 and draws a house; any key comes back here.")
    tui.label(row, 4, "CSI ? 38 h      enter Tek 4014")
    tui.label(row + 1, 4, "GS <addresses>  draw the outline")
    tui.label(row + 2, 4, "ESC ETX         return to VT")
    return row


def _demo_tek(tui):
    _tek_page(tui)
    while True:
        tui.keys(("Space", "draw"))
        tui.flush()
        key = tui.read_key()
        if key != " ":
            return key
        try:
            tui.write(TEK_ENTER, _tek_vectors(_TEK_SHAPE))
            tui.flush()
            tui.read_key()
        finally:
            tui.write(TEK_EXIT)
            tui.flush()
        row = _tek_page(tui)
        tui.result(row + 4, 4, "Back on VT/ANSI after ESC ETX.")


ENTRIES = [
    Entry(
        title="Tektronix 4014 (CSI ? 38 h)",
        explanation=(
            "CSI ? 38 h switches the stream to Tek 4014; a GS chain of graphic "
            "addresses traces a box under a roof, which stays on the tube "
            "until the mode ends. ESC ETX returns to VT and the ANSI page comes "
            "back unchanged."),
        run=_demo_tek,
        sequence=_TEK_SEQUENCE,
        golden="compat_tek4014",
    ),
]
