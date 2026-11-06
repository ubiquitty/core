# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""Soft fonts: DECDLD downloadable character sets.

DECDLD loads glyph bitmaps under a Dscs name (optional intermediate, then a
final), and an SCS designator naming the same bytes puts the set into a G-set.
Every entry puts ASCII back into G0 to G3 and invokes G0 before it returns.
"""

from .. import tui as T
from ..tui import CSI, DCS, ST, ESC, RESET, BOLD, move, fg_truecolor
from . import Entry

TITLE = "Soft fonts"
ORDER = 250


# DECDLD bitmap geometry. A VT320 soft cell is up to 15 wide x 12 high; here a
# compact 8x10 cell keeps the sixel payload small and legible. Sixels stack 6
# pixels vertically per character (LSB = top), so a 10-pixel-tall cell needs two
# sixel rows separated by "/": the first carries pixel rows 0..5, the second
# rows 6..9 (only the low bits used).
_CELL_W = 8
_CELL_H = 10


def _sixel_rows(columns):
    """Encode one DRCS glyph from per-column bit patterns into sixel groups.

    `columns` is a list of `_CELL_W` integers, each a bitmask of set pixels in
    that column (bit 0 = top row). Per DECDLD, a glyph is a sequence of sixel
    bytes (value + 0x3F) read left-to-right; "/" advances to the next band of
    six rows. Returns the byte string for one glyph (no trailing separator).
    """
    bands = (_CELL_H + 5) // 6  # number of 6-row bands
    out = []
    for band in range(bands):
        for col in range(_CELL_W):
            six = (columns[col] >> (band * 6)) & 0x3F
            out.append(chr(0x3F + six))
        if band != bands - 1:
            out.append("/")
    return "".join(out)


def _glyph_arrow():
    """A right-pointing arrow drawn column by column (bit 0 = top pixel)."""
    cols = [0] * _CELL_W
    # Horizontal shaft across the vertical middle (rows 4 and 5).
    for c in range(_CELL_W):
        cols[c] |= (1 << 4) | (1 << 5)
    # Arrow head: a wedge near the right edge.
    cols[5] |= (1 << 2) | (1 << 3) | (1 << 6) | (1 << 7)
    cols[6] |= (1 << 3) | (1 << 6)
    return cols


def _glyph_block():
    """A hollow square outline filling most of the cell."""
    cols = [0] * _CELL_W
    top = 1
    bot = 8
    for c in range(_CELL_W):
        cols[c] |= (1 << top) | (1 << bot)  # top and bottom edges
    for r in range(top, bot + 1):
        cols[0] |= (1 << r)   # left edge
        cols[7] |= (1 << r)   # right edge
    return cols


def _build_decdld():
    """Assemble a complete DECDLD blob defining two DRCS glyphs.

    Header parameters (VT320):
      Pfn=1   font number; Ubiquitty keeps one soft set, so it is read and
              ignored.
      Pcn=1   start cell: the first glyph lands on '!' (0x21), the byte after
              SPACE. DEC reads Pcn 0 the same way for a 94-cell set, and 1
              says so; only a 96-cell set starts at SPACE.
      Pe=2    erase control: 0 and 2 both wipe the set before loading; only
              1 keeps the cells this payload does not name.
      Pcmw=8  matrix width in pixels; 0 would take the 8-pixel default.
      Pw=0    font width attribute (device default screen cells).
      Pt=0    text/full-cell usage (0 = text).
      Pcmh=10 matrix height in pixels; 0 would take the 12-pixel default.
      Pcss=0  character set size: 0 => 94-cell set (1 => 96-cell set).
    Dscs (after '{') names the set: SPACE then 'A'. An SCS designator
    invokes it only when it repeats both bytes, so ESC ) SP A matches and
    ESC ) A (the UK set) does not.
    """
    header = "1;1;2;8;0;0;10;0"
    dscs = " A"           # DRCS intermediate + final naming this soft set
    g_arrow = _sixel_rows(_glyph_arrow())
    g_block = _sixel_rows(_glyph_block())
    # Glyphs within the payload are separated by ';'; each maps to the next
    # character cell starting at Pcn.
    body = g_arrow + ";" + g_block
    return DCS + header + "{" + dscs + body + ST


# The full DECDLD blob this section uploads, then designates and prints from.
DECDLD_BLOB = _build_decdld()

SCS_DRCS = {slot: ESC + "()*+"[slot] + " A" for slot in range(4)}
SI = "\x0f"   # LS0: G0 into GL
SO = "\x0e"   # LS1: G1 into GL
SS2 = ESC + "N"
SS3 = ESC + "O"
DECSTR = CSI + "!p"
# ASCII back into every G-set, G0 invoked.
RESTORE = SI + ESC + "(B" + ESC + ")B" + ESC + "*B" + ESC + "+B"


def _soft(tui, row, col, text):
    """Write `text` in the accent colour at row, col, then return to the page pen."""
    tui.write(move(row, col), fg_truecolor(*tui.theme.accent), BOLD, text, RESET)


def _demo_decdld(tui):
    row = tui.page(TITLE, "With the set in G1, '!' and '\"' draw the uploaded arrow and box.")
    tui.write(DECDLD_BLOB, SCS_DRCS[1])
    tui.label(row, 4, "ASCII")
    _soft(tui, row, 30, '!"')
    tui.label(row + 1, 4, "ESC ) SP A, SO")
    _soft(tui, row + 1, 30, SO + '!"' + SI)
    tui.write(RESTORE)
    return tui.footer()


def _demo_g2_g3(tui):
    row = tui.page(TITLE, "A single shift draws one soft glyph from G2 or G3; the next byte is ASCII.")
    tui.write(DECDLD_BLOB, SCS_DRCS[2], SCS_DRCS[3])
    tui.label(row, 4, "ESC * SP A, ESC N ! !")
    _soft(tui, row, 30, SS2 + "!!")
    tui.label(row + 1, 4, 'ESC + SP A, ESC O " "')
    _soft(tui, row + 1, 30, SS3 + '""')
    tui.write(RESTORE)
    return tui.footer()


def _demo_persistence(tui):
    row = tui.page(TITLE, "DECSTR resets G1 to ASCII; a new ESC ) SP A brings the glyphs back.")
    tui.write(DECDLD_BLOB, SCS_DRCS[1])
    tui.label(row, 4, "loaded, ESC ) SP A, SO")
    _soft(tui, row, 30, SO + '!"' + SI)
    tui.write(DECSTR, T.HIDE_CURSOR)
    tui.label(row + 1, 4, "after CSI ! p, SO")
    _soft(tui, row + 1, 30, SO + '!"' + SI)
    tui.write(SCS_DRCS[1])
    tui.label(row + 2, 4, "ESC ) SP A again, SO")
    _soft(tui, row + 2, 30, SO + '!"' + SI)
    tui.write(RESTORE)
    return tui.footer()


_PAIR = SO + '!"' + SI

ENTRIES = [
    Entry(
        title="Soft character set (DECDLD)",
        explanation=(
            "DECDLD loads an arrow into '!' and a hollow box into '\"' of a "
            "94-cell set named SPACE A. ESC ) SP A designates it into G1, so "
            "the two bytes print as the glyphs under SO and as ASCII under SI."),
        run=_demo_decdld,
        sequence=(DECDLD_BLOB + SCS_DRCS[1] + 'ASCII !" soft ' + _PAIR
                  + ESC + ")B"),
        golden="soft_fonts_decdld",
    ),
    Entry(
        title="Soft set in G2 and G3 (SS2, SS3)",
        explanation=(
            "The same set designated into G2 and G3 is reached with a single "
            "shift: ESC N or ESC O draws one byte from that set and the byte "
            "after it prints as ASCII."),
        run=_demo_g2_g3,
        sequence=(DECDLD_BLOB + SCS_DRCS[2] + SCS_DRCS[3] + SS2 + "!!" + SS3 + '""'
                  + ESC + "*B" + ESC + "+B"),
    ),
    Entry(
        title="Soft set across soft reset (DECSTR)",
        explanation=(
            "DECSTR returns G0 to G3 to ASCII, so the bytes print plain, but "
            "it keeps the loaded glyphs: designating the set again brings the "
            "arrow and box back without a new DECDLD."),
        run=_demo_persistence,
        sequence=(DECDLD_BLOB + SCS_DRCS[1] + _PAIR + " " + DECSTR + _PAIR + " "
                  + SCS_DRCS[1] + _PAIR + ESC + ")B"),
    ),
]
