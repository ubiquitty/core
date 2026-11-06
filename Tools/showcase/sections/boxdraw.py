# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""Box drawing and blocks: U+2500 line drawing, U+2580 block elements, U+2591
shades and U+1FB00 sextants.

These glyphs are drawn from the cell metrics rather than taken from a font, so
lines join across cell boundaries and filled areas tile with no seam at any
cell size.
"""

from ..tui import BOLD, fg_truecolor, ROUNDED, DOUBLE, LIGHT
from . import Entry

TITLE = "Box drawing and blocks"
ORDER = 50


# Heavy-weight box set (U+250x..) — the bold counterpart to tui.LIGHT.
HEAVY = {
    "tl": "┏", "tr": "┓", "bl": "┗", "br": "┛",
    "h": "━", "v": "┃",
}


def _draw_box(tui, top, left, height, width, style, *styles):
    """Draw a hollow box from an explicit corner/edge style dict.

    Unlike `tui.panel` this paints only the border glyphs (no interior fill),
    so adjacent boxes and rules can share edges and the join behaviour is what
    is under test.
    """
    b = style
    tui.text(top, left, b["tl"] + b["h"] * (width - 2) + b["tr"], *styles)
    for i in range(1, height - 1):
        tui.text(top + i, left, b["v"], *styles)
        tui.text(top + i, left + width - 1, b["v"], *styles)
    tui.text(top + height - 1, left,
             b["bl"] + b["h"] * (width - 2) + b["br"], *styles)


# ---------------------------------------------------------------------------
# Box-drawing lines: frames, corners, junctions, crosses, dashes
# ---------------------------------------------------------------------------

# A grid assembled from light box-drawing glyphs. Every interior junction is a
# real cross/T glyph, so the only way it looks right is if each glyph's stub
# lines up pixel-exact with its neighbours'.
_GRID = [
    "┌─┬─┬─┐",
    "│ │ │ │",
    "├─┼─┼─┤",
    "│ │ │ │",
    "├─┼─┼─┤",
    "│ │ │ │",
    "└─┴─┴─┘",
]

# One deterministic line exercising every junction and a dashed run, useful as
# a single-string snapshot fixture.
_LINES_SEQ = (
    "┌─┬─┐ ╔═╦═╗ ╭─┬─╮ ┏━┳━┓\r\n"
    "├─┼─┤ ╠═╬═╣ ├─┼─┤ ┣━╋━┫\r\n"
    "└─┴─┘ ╚═╩═╝ ╰─┴─╯ ┗━┻━┛\r\n"
    "┄┄┄ ┅┅┅ ┈┈┈ ┉┉┉ ╌╌╌ ╍╍╍\r\n"
)


def _demo_lines(tui):
    row = tui.page(TITLE, "Every corner closes flush and every tee and cross joins with no break.")
    t = tui.theme
    for left, style, name, color in ((4, LIGHT, "light", t.accent), (22, HEAVY, "heavy", t.accent),
                                     (40, DOUBLE, "double", t.accent2),
                                     (58, ROUNDED, "rounded", t.accent2)):
        _draw_box(tui, row, left, 5, 14, style, fg_truecolor(*color))
        tui.label(row + 2, left + 3, name)
    top = row + 6
    tui.label(top, 4, "junctions")
    for i, ln in enumerate(_GRID):
        tui.text(top + 1 + i, 6, ln, fg_truecolor(*t.accent))
    tui.label(top, 22, "dashes")
    for i, (glyph, name) in enumerate((("┄", "light triple"), ("┅", "heavy triple"),
                                       ("┈", "light quadruple"), ("┉", "heavy quadruple"),
                                       ("╌", "light double"), ("╍", "heavy double"))):
        tui.text(top + 1 + i, 24, glyph * 12, fg_truecolor(*t.accent2))
        tui.label(top + 1 + i, 38, f"U+{ord(glyph):04X} {name}")
    return tui.footer()


# ---------------------------------------------------------------------------
# Block elements: full blocks, halves, eighths, quadrants
# ---------------------------------------------------------------------------

# Eighth blocks: a vertical ramp U+2581..U+2588 (lower) and the horizontal
# ramp U+2589..U+258F + U+2588 (left). Concatenated they form a smooth column.
_LOWER_EIGHTHS = "".join(chr(c) for c in range(0x2581, 0x2589))  # ▁..█
_LEFT_EIGHTHS = "█" + "".join(chr(c) for c in range(0x2589, 0x2590))  # █▉..▏

_QUADRANTS = "".join(chr(c) for c in range(0x2596, 0x25a0))  # ▖▗▘▙▚▛▜▝ + ▞▟

# A deterministic single-string snapshot fixture exercising the block family:
# a solid full-block field, the four halves, the two eighth ramps, and the
# quadrant glyphs. Newline-separated so it lays out as distinct rows.
_BLOCKS_SEQ = (
    "█" * 24 + "\r\n"
    "▀▀▀▀ ▄▄▄▄ ▌▌▌▌ ▐▐▐▐\r\n"
    + _LOWER_EIGHTHS + " " + _LEFT_EIGHTHS + "\r\n"
    + _QUADRANTS + "\r\n"
)


def _demo_blocks(tui):
    row = tui.page(TITLE, "Full blocks form one solid field, eighths step evenly, quadrants tile.")
    t = tui.theme
    bcol = fg_truecolor(*t.accent)
    tui.label(row, 4, "U+2588 full")
    for r in range(3):
        tui.text(row + 1 + r, 6, "█" * 24, bcol)
    tui.label(row, 36, "U+2580 U+2584 U+258C U+2590 halves")
    tui.text(row + 1, 38, "▀▀▀▀  ▄▄▄▄  ▌▌▌▌  ▐▐▐▐", fg_truecolor(*t.accent2))
    tui.label(row + 5, 4, "U+2581-U+2588 lower eighths, U+2589-U+258F left eighths")
    tui.text(row + 6, 6, _LOWER_EIGHTHS, bcol)
    tui.text(row + 6, 20, _LEFT_EIGHTHS, fg_truecolor(*t.accent2))
    tui.label(row + 8, 4, "U+2596-U+259F quadrants")
    for r in range(3):
        tui.text(row + 9 + r, 6, ("▚▞" * 12) if r % 2 == 0 else ("▞▚" * 12), bcol)
    tui.text(row + 9, 36, _QUADRANTS, fg_truecolor(*t.accent2))
    return tui.footer()


# ---------------------------------------------------------------------------
# Shades: light / medium / dark gradient
# ---------------------------------------------------------------------------

_SHADES_SEQ = ("░" * 12) + ("▒" * 12) + ("▓" * 12) + ("█" * 12) + "\r\n"


def _demo_shades(tui):
    row = tui.page(TITLE, "The shades step evenly from blank to full, each band one flat density.")
    t = tui.theme
    for r, (lab, fill) in enumerate((("blank", " " * 12), ("U+2591 light", "░" * 12),
                                     ("U+2592 medium", "▒" * 12), ("U+2593 dark", "▓" * 12),
                                     ("U+2588 full", "█" * 12))):
        tui.text(row + r, 6, fill, fg_truecolor(*t.accent))
        tui.label(row + r, 22, lab)
    tui.text(row + 7, 6, ("░" * 12) + ("▒" * 12) + ("▓" * 12) + ("█" * 12),
             fg_truecolor(*t.accent2))
    return tui.footer()


# ---------------------------------------------------------------------------
# Legacy-computing sextants: a 2x3 sub-cell bitmap mosaic
# ---------------------------------------------------------------------------

# Symbols for Legacy Computing carries sextants at U+1FB00..U+1FB3B: a cell
# split into a 2-wide x 3-tall grid of sub-pixels. Named positions 1..6 map to
# sub-cell bits as: 1=top-left 2=top-right 3=mid-left 4=mid-right 5=bottom-left
# 6=bottom-right, i.e. bit (pos-1). The block encodes 60 of the 62 partial
# masks in ascending numeric order, OMITTING two whole-column masks that
# already have block glyphs: left column (positions 1,3,5 -> U+258C left half)
# and right column (positions 2,4,6 -> U+2590 right half). The empty and full
# masks map to space and U+2588. `_SEXTANT_GLYPH` resolves a mask to its glyph.

_LEFT_COL_MASK = 0b010101   # positions 1,3,5 -> U+258C
_RIGHT_COL_MASK = 0b101010  # positions 2,4,6 -> U+2590


def _build_sextant_table():
    """Map each 6-bit sextant mask (0..63) to its rendering glyph."""
    table = {0: " ", 63: "█", _LEFT_COL_MASK: "▌", _RIGHT_COL_MASK: "▐"}
    cp = 0x1FB00
    for mask in range(1, 63):
        if mask in table:
            continue
        table[mask] = chr(cp)
        cp += 1
    return table


_SEXTANT_GLYPH = _build_sextant_table()


def _sextant(mask):
    """Return the glyph for a 6-bit sextant `mask` (bit layout above)."""
    return _SEXTANT_GLYPH[mask]


def _sextant_bitmap(rows):
    """Render a list of bit-strings into sextant glyph lines.

    `rows` is a list of strings of '0'/'1' (or ' '/'#'); width must be even and
    the row count a multiple of 3. Returns a list of glyph strings, one per
    cell-row (every 3 input rows collapse into 1 output row).
    """
    def on(r, c):
        if r >= len(rows) or c >= len(rows[r]):
            return 0
        return 1 if rows[r][c] in ("1", "#") else 0

    width = max(len(r) for r in rows)
    out = []
    for ry in range(0, len(rows), 3):
        line = []
        for cx in range(0, width, 2):
            mask = (on(ry, cx) << 0 | on(ry, cx + 1) << 1
                    | on(ry + 1, cx) << 2 | on(ry + 1, cx + 1) << 3
                    | on(ry + 2, cx) << 4 | on(ry + 2, cx + 1) << 5)
            line.append(_sextant(mask))
        out.append("".join(line))
    return out


# A small heart bitmap (12 sub-pixels wide x 9 tall -> 6 cells x 3 cell-rows).
_HEART_BITS = [
    "011110011110",
    "111111111111",
    "111111111111",
    "011111111110",
    "011111111110",
    "001111111100",
    "000111111000",
    "000011110000",
    "000001100000",
]

# A deterministic single-string snapshot fixture: the heart bitmap rendered as
# sextant glyphs, then the full U+1FB00..U+1FB3B range, each on its own row.
_SEXTANTS_SEQ = (
    "\r\n".join(_sextant_bitmap(_HEART_BITS)) + "\r\n"
    + "".join(chr(c) for c in range(0x1FB00, 0x1FB20)) + "\r\n"
    + "".join(chr(c) for c in range(0x1FB20, 0x1FB3C)) + "\r\n"
)


def _demo_sextants(tui):
    row = tui.page(TITLE, "The heart is drawn from 2x3 sub-cells that meet across cell borders.")
    t = tui.theme
    for i, ln in enumerate(_sextant_bitmap(_HEART_BITS)):
        tui.text(row + i, 8, ln, fg_truecolor(*t.title), BOLD)
    tui.label(row + 4, 4, "U+1FB00-U+1FB1F")
    tui.text(row + 4, 22, "".join(chr(c) for c in range(0x1FB00, 0x1FB20)), fg_truecolor(*t.accent))
    tui.label(row + 6, 4, "U+1FB20-U+1FB3B")
    tui.text(row + 6, 22, "".join(chr(c) for c in range(0x1FB20, 0x1FB3C)), fg_truecolor(*t.accent2))
    return tui.footer()


# ---------------------------------------------------------------------------
# Composed dashboard: a framed panel + bar graph from eighth blocks
# ---------------------------------------------------------------------------

def _bar(value, width):
    """An eighth-block horizontal bar of fractional `value` in [0,1] over
    `width` cells: full blocks plus one fractional left-eighth tail."""
    cells = value * width
    full = int(cells)
    frac = cells - full
    # U+2588 is full; U+2589..U+258F are 7/8..1/8 left blocks.
    eighth = ""
    step = int(round(frac * 8))
    if step:
        eighth = chr(0x2590 - step)  # 0x258F=1/8 .. 0x2589=7/8, 0x2588=full
    return "█" * full + eighth


def _build_dashboard_seq():
    """A deterministic single-string snapshot of the composed dashboard: a
    double-line frame, an inner T-junction rule, eighth-block bar rows, and a
    sparkline. Built without cursor addressing so it round-trips as one fixture.
    """
    w = 56
    metrics = [("CPU", 0.72), ("Memory", 0.45), ("Disk", 0.88),
               ("Network", 0.31), ("Swap", 0.12)]
    bar_w = w - 18
    lines = ["╔" + "═" * (w - 2) + "╗"]
    lines.append("║ System Monitor" + " " * (w - 17) + "║")
    lines.append("╟" + "─" * (w - 2) + "╢")
    for name, val in metrics:
        bar = _bar(val, bar_w)
        body = f" {name:<8} {bar}"
        lines.append("║" + body.ljust(w - 2)[:w - 2] + "║")
    spark = "".join(chr(0x2581 + (i * 7 // 11)) for i in range(12))
    lines.append("║ Load  " + (spark).ljust(w - 8)[:w - 8] + "║")
    lines.append("╚" + "═" * (w - 2) + "╝")
    return "\r\n".join(lines) + "\r\n"


_DASHBOARD_SEQ = _build_dashboard_seq()


def _demo_dashboard(tui):
    row = tui.page(TITLE, "A panel drawn only from box and block glyphs; each bar ends on an eighth.")
    t = tui.theme
    top, left, h, w = row, 4, 13, 56
    _draw_box(tui, top, left, h, w, DOUBLE, fg_truecolor(*t.accent))
    tui.text(top, left + 2, " System Monitor ", fg_truecolor(*t.title), BOLD)
    tui.text(top + 2, left, "╟" + "─" * (w - 2) + "╢", fg_truecolor(*t.accent))
    metrics = [("CPU", 0.72), ("Memory", 0.45), ("Disk", 0.88),
               ("Network", 0.31), ("Swap", 0.12)]
    bar_w = w - 18
    for i, (name, val) in enumerate(metrics):
        r = top + 3 + i
        tui.text(r, left + 2, f"{name:<8}", tui.fg_default())
        tui.text(r, left + 11, _bar(val, bar_w), fg_truecolor(*t.accent2))
        tui.label(r, left + 11 + bar_w + 1, f"{int(val * 100):>3}%")
    spark = "".join(chr(0x2581 + (i * 7 // 11)) for i in range(12))
    tui.text(top + h - 2, left + 2, "Load  " + spark, fg_truecolor(*t.title))
    return tui.footer()


# ---------------------------------------------------------------------------
# All box-drawing combinations: the full U+2500..257F block, exercised so every
# junction/weight combination renders in context with gapless neighbour joins.
# This is the acceptance screen for the 9-slice edge-stretch box renderer.
# ---------------------------------------------------------------------------

# Junction families, keyed by weight. Each is {left-tee, right-tee, down-tee,
# up-tee, cross} — the five interior nodes a grid needs.
_TEES_LIGHT = {"vr": "├", "vl": "┤", "hd": "┬", "hu": "┴", "x": "┼"}
_TEES_HEAVY = {"vr": "┣", "vl": "┫", "hd": "┳", "hu": "┻", "x": "╋"}
_TEES_DOUBLE = {"vr": "╠", "vl": "╣", "hd": "╦", "hu": "╩", "x": "╬"}


def _junction_grid(box, tees):
    """A 3x3 cell grid (so every interior node is a real tee or cross) built
    from a corner/edge `box` style and its matching `tees` family. Returns 5
    strings: each interior junction is surrounded by connecting lines."""
    h, v = box["h"], box["v"]
    seg = h * 3                       # 3-cell-wide horizontal edge
    top = box["tl"] + seg + tees["hd"] + seg + tees["hd"] + seg + box["tr"]
    mid = tees["vr"] + seg + tees["x"] + seg + tees["x"] + seg + tees["vl"]
    bot = box["bl"] + seg + tees["hu"] + seg + tees["hu"] + seg + box["br"]
    fill = v + "   " + v + "   " + v + "   " + v
    return [top, fill, mid, fill, mid, fill, bot]


# Mixed-weight junction glyphs (U+251C..254B): light↔heavy tees/crosses where a
# light stroke meets a heavy one. Placed so the transition is visible in-line.
_LH_TEES = "┝┞┟┠┡┢┥┦┧┨┩┪┭┮┯┰┱┲┵┶┷┸┹┺┽┾┿╀╁╂╃╄╅╆╇╈╉╊"
# Light↔double junction glyphs (U+2552..256C region): the ╞╟╪╫ family.
_LD_TEES = "╞╟╡╢╤╥╧╨╪╫"

# Dashed line glyphs, horizontal and vertical, light & heavy.
_DASH_H = ["┄  light triple-dash", "┅  heavy triple-dash",
           "┈  light quadruple-dash", "┉  heavy quadruple-dash",
           "╌  light double-dash", "╍  heavy double-dash"]
_DASH_V = ["┆", "┇", "┊", "┋", "╎", "╏"]


def _demo_combinations(tui):
    row = tui.page(TITLE, "All 128 glyphs of U+2500-U+257F, then boxes and junctions built from them.")
    t = tui.theme
    for r in range(8):
        base = 0x2500 + r * 16
        tui.label(row + r, 4, f"{base:04X}")
        tui.text(row + r, 10, " ".join(chr(base + c) for c in range(16)), fg_truecolor(*t.accent))
    _draw_box(tui, row, 50, 3, 8, LIGHT, fg_truecolor(*t.accent))
    _draw_box(tui, row, 60, 4, 10, LIGHT, fg_truecolor(*t.accent))
    _draw_box(tui, row + 4, 50, 3, 8, HEAVY, fg_truecolor(*t.accent2))
    _draw_box(tui, row + 4, 60, 4, 10, HEAVY, fg_truecolor(*t.accent2))
    _draw_box(tui, row, 72, 3, 8, DOUBLE, fg_truecolor(*t.title))
    _draw_box(tui, row + 4, 72, 4, 8, ROUNDED, fg_truecolor(*t.title))
    grids = [(_junction_grid(LIGHT, _TEES_LIGHT), t.accent, 4),
             (_junction_grid(HEAVY, _TEES_HEAVY), t.accent2, 24),
             (_junction_grid(DOUBLE, _TEES_DOUBLE), t.title, 44)]
    for lines, col, x in grids:
        for i, ln in enumerate(lines):
            tui.text(row + 9 + i, x, ln, fg_truecolor(*col))
    diag = ["╱   ╲   ╳", " ╲ ╱   ╳ ", "  ╳   ╱ ╲", " ╱ ╲   ╳ "]
    for i, ln in enumerate(diag):
        tui.text(row + 10 + i, 66, ln, fg_truecolor(*t.accent2))
    return tui.footer(("Space", "next page"))


def _demo_combinations2(tui):
    """Second page: mixed-weight transitions, dashed runs and arcs."""
    row = tui.page(TITLE, "Mixed weights meet at one stroke centre; dashes keep period; arcs close.")
    t = tui.theme
    for i, g in enumerate(_LH_TEES):
        rr, cc = row + i % 10, 4 + (i // 10) * 14
        tui.text(rr, cc, "──", fg_truecolor(*t.accent))
        tui.text(rr, cc + 2, g, fg_truecolor(*t.title), BOLD)
        tui.text(rr, cc + 3, "━━", fg_truecolor(*t.accent2))
        tui.label(rr, cc + 6, f"{ord(g):04X}")
    for i, g in enumerate(_LD_TEES):
        rr = row + 11 + i % 5
        cc = 4 + (i // 5) * 14
        tui.text(rr, cc, "──", fg_truecolor(*t.accent))
        tui.text(rr, cc + 2, g, fg_truecolor(*t.title), BOLD)
        tui.text(rr, cc + 3, "══", fg_truecolor(*t.accent2))
        tui.label(rr, cc + 6, f"{ord(g):04X}")
    for i, d in enumerate(_DASH_H):
        tui.text(row + i, 62, d[0] * 12, fg_truecolor(*t.accent2))
    for i, ch in enumerate(_DASH_V):
        for k in range(5):
            tui.text(row + 7 + k, 62 + i * 2, ch, fg_truecolor(*t.accent))
    _draw_box(tui, row + 11, 36, 3, 8, ROUNDED, fg_truecolor(*t.accent))
    _draw_box(tui, row + 11, 46, 4, 6, ROUNDED, fg_truecolor(*t.accent2))
    tui.text(row + 15, 36, "╭╮  ╭╮  ╭╮  ╭╮  ╭╮", fg_truecolor(*t.title))
    tui.text(row + 16, 36, "╯╰──╯╰──╯╰──╯╰──╯╰", fg_truecolor(*t.title))
    return tui.footer()


# A deterministic single-string snapshot fixture for the combinations screen:
# the full U+2500..257F block one row of 16 per line, then a light/heavy/double
# junction grid, then the mixed-weight transition rows, dashes and arcs.
def _build_combinations_seq():
    lines = []
    for r in range(8):
        base = 0x2500 + r * 16
        lines.append("".join(chr(base + c) for c in range(16)))
    lines.append("")
    lg = _junction_grid(LIGHT, _TEES_LIGHT)
    hg = _junction_grid(HEAVY, _TEES_HEAVY)
    dg = _junction_grid(DOUBLE, _TEES_DOUBLE)
    for a, b, c in zip(lg, hg, dg):
        lines.append(a + "  " + b + "  " + c)
    lines.append("")
    lines.append("──" + _LH_TEES + "━━")
    lines.append("──" + _LD_TEES + "══")
    lines.append("┄┄┄ ┅┅┅ ┈┈┈ ┉┉┉ ╌╌╌ ╍╍╍")
    lines.append("╭──╮ ╭╮  ╱╲ ╳")
    lines.append("╰──╯ ╰╯  ╲╱ ╳")
    return "\r\n".join(lines) + "\r\n"


_COMBINATIONS_SEQ = _build_combinations_seq()


def _demo_all_combinations(tui):
    """Drive both pages of the box-drawing combinations acceptance set: the
    full glyph table + constructed boxes + junction matrices first, then the
    mixed-weight transitions + dashes + arcs."""
    key = _demo_combinations(tui)
    if key != " ":
        return key
    return _demo_combinations2(tui)


ENTRIES = [
    Entry(
        title="Box-drawing lines (U+2500-U+257F)",
        explanation=(
            "Light, heavy, double and rounded frames close flush at every "
            "corner; tees and crosses join as one stroke across the cell "
            "boundary; dashed runs hold an even period."),
        run=_demo_lines,
        sequence=_LINES_SEQ,
        golden="boxdraw_lines",
    ),
    Entry(
        title="Block elements (U+2580-U+259F)",
        explanation=(
            "A field of full blocks reads as one solid rectangle; halves cut "
            "the cell exactly in two; eighth blocks change by one eighth per "
            "step; quadrants tile into a checkerboard with unbroken diagonals."),
        run=_demo_blocks,
        sequence=_BLOCKS_SEQ,
        golden="boxdraw_blocks",
    ),
    Entry(
        title="Shades (U+2591-U+2593)",
        explanation=(
            "Light, medium and dark shades between blank and full make an even "
            "five-step ramp, and like-shaded cells abut with no seam."),
        run=_demo_shades,
        sequence=_SHADES_SEQ,
        golden="boxdraw_shades",
    ),
    Entry(
        title="Sextants (U+1FB00-U+1FB3B)",
        explanation=(
            "Each sextant fills its own subset of a 2x3 sub-cell grid, so a "
            "bitmap drawn from them reads as a heart with sub-cells meeting "
            "across cell borders; no glyph falls back to a box."),
        run=_demo_sextants,
        sequence=_SEXTANTS_SEQ,
        golden="boxdraw_sextants",
    ),
    Entry(
        title="Box-drawing combinations",
        explanation=(
            "Over two pages, all 128 box-drawing glyphs render; boxes, junction "
            "grids and diagonals in every weight join with no seam; "
            "mixed-weight junctions meet at one stroke centre; dashes keep "
            "their period and arcs close."),
        run=_demo_all_combinations,
        sequence=_COMBINATIONS_SEQ,
        golden="boxdraw_combinations",
    ),
    Entry(
        title="Dashboard",
        explanation=(
            "A double frame meets its inner rule at the U+255F and U+2562 tees; "
            "each bar ends on one left-eighth block, so its length reads to an "
            "eighth of a cell; a sparkline of lower eighths rises along the "
            "bottom."),
        run=_demo_dashboard,
        sequence=_DASHBOARD_SEQ,
        golden="boxdraw_dashboard",
    ),
]
