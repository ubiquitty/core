# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""Text attributes: SGR toggles and their resets, underline styles and color,
line and frame attributes, super- and subscript, and DECCARA.

The underline entries run over text with descenders, the only text where the
skip-ink cut around a glyph tail is visible.
"""

from ..tui import BOLD, CSI, ITALIC, RESET, fg_truecolor, move
from . import Entry

TITLE = "Text attributes"
ORDER = 20

# A phrase packed with descenders (g/y/p/j/q) plus a slash, which crosses the
# underline diagonally — so any underline rule has to cut its skip-ink mask
# around the glyph tails and the stroke.
DESCENDERS = "g p / jogging pygmy gq/y"

# Mixed sample: a clean, descender-free run first (so the rule reads as an
# uninterrupted line / even dots / uniform dashes) and then a descender cluster
# with a slash (so the skip-ink mask is shown carving around the tails).
MIXED = "Sail warm river  jogging pygmy gq/y"


def _demo_core(tui):
    row = tui.page(TITLE, "Each attribute styles its sample and 'on'; its reset leaves 'off' plain.")

    def line(set_seq, set_lbl, reset_seq, reset_lbl, sample):
        nonlocal row
        tui.label(row, 4, f"CSI {set_lbl} m / CSI {reset_lbl} m")
        tui.write(move(row, 28), tui.fg_default(), set_seq, sample, RESET)
        tui.write(move(row, 48), tui.fg_default(), set_seq, "on", reset_seq, " off", RESET)
        row += 1

    line(BOLD, "1", f"{CSI}22m", "22", "bold")
    line(f"{CSI}2m", "2", f"{CSI}22m", "22", "dim")
    line(ITALIC, "3", f"{CSI}23m", "23", "italic")
    line(f"{CSI}7m", "7", f"{CSI}27m", "27", "inverse")
    line(f"{CSI}8m", "8", f"{CSI}28m", "28", "concealed")
    line(f"{CSI}5m", "5", f"{CSI}25m", "25", "blinking")
    return tui.footer()


def _demo_underline_styles(tui):
    row = tui.page(TITLE, "Five underline shapes; each breaks around the descenders g, y, p, j and q.")
    for code, name in (("4:1", "single"), ("4:2", "double"), ("4:3", "curly"),
                       ("4:4", "dotted"), ("4:5", "dashed")):
        tui.label(row, 4, f"CSI {code} m")
        tui.text(row, 16, name, tui.fg_default())
        tui.write(move(row, 26), tui.fg_default(), f"{CSI}{code}m", MIXED, RESET)
        row += 1
    return tui.footer()


def _demo_underline_color(tui):
    row = tui.page(TITLE, "The underline takes its own color while the letters keep the text color.")
    tui.label(row, 4, "CSI 4:3 m  CSI 58:2::255:80:80 m")
    tui.write(move(row, 40), tui.fg_default(), f"{CSI}4:3m", f"{CSI}58:2::255:80:80m",
              DESCENDERS, RESET)
    tui.label(row + 2, 4, "CSI 4:1 m  CSI 58:5:46 m")
    tui.write(move(row + 2, 40), tui.fg_default(), f"{CSI}4:1m", f"{CSI}58:5:46m",
              DESCENDERS, RESET)
    tui.label(row + 4, 4, "CSI 58:5:46 m ... CSI 59 m")
    tui.write(move(row + 4, 40), tui.fg_default(), f"{CSI}4:1m", f"{CSI}58:5:46m",
              "green", f"{CSI}59m", " then text color", RESET)
    return tui.footer()


def _demo_lines_boxes(tui):
    row = tui.page(TITLE, "A strike, an overline, a frame and a circle, each ended by its reset.")
    for code, set_seq, reset_seq, sample in (
            ("CSI 9 m / CSI 29 m", f"{CSI}9m", f"{CSI}29m", "struck out"),
            ("CSI 53 m / CSI 55 m", f"{CSI}53m", f"{CSI}55m", "over the top"),
            ("CSI 51 m / CSI 54 m", f"{CSI}51m", f"{CSI}54m", "framed"),
            ("CSI 52 m / CSI 54 m", f"{CSI}52m", f"{CSI}54m", "encircled")):
        tui.label(row, 4, code)
        tui.write(move(row, 28), tui.fg_default(), set_seq, sample, reset_seq, " plain", RESET)
        row += 2
    return tui.footer()


# SGR 73 raises a superscript, 74 drops a subscript, and 75 clears both back to
# baseline. Samples that read wrong if the shift or shrink is missing.
_SUPERSUB_SEQ = (
    "E = mc" + f"{CSI}73m" + "2" + f"{CSI}75m" + "\r\n"
    "H" + f"{CSI}74m" + "2" + f"{CSI}75m" + "O\r\n"
    "x" + f"{CSI}73m" + "2" + f"{CSI}75m" + " + y" + f"{CSI}73m" + "2" + f"{CSI}75m"
    + " = r" + f"{CSI}73m" + "2" + f"{CSI}75m" + "\r\n"
    "a" + f"{CSI}74m" + "i" + f"{CSI}75m" + " + b" + f"{CSI}74m" + "j" + f"{CSI}75m"
    + "\r\n"
)


def _demo_supersub(tui):
    row = tui.page(TITLE, "The raised and lowered digits are smaller and still one cell each.")
    sup, sub, clr = f"{CSI}73m", f"{CSI}74m", f"{CSI}75m"
    for code, runs in (
            ("CSI 73 m ... CSI 75 m", ("E = mc", sup, "2", clr)),
            ("CSI 74 m ... CSI 75 m", ("H", sub, "2", clr, "O")),
            ("CSI 73 m, repeated", ("x", sup, "2", clr, " + y", sup, "2", clr,
                                    " = r", sup, "2", clr)),
            ("CSI 74 m, repeated", ("a", sub, "i", clr, " + b", sub, "j", clr))):
        tui.label(row, 4, code)
        tui.write(move(row, 30), tui.fg_default(), *runs, RESET)
        row += 2
    return tui.footer()


def _demo_stack(tui):
    row = tui.page(TITLE, "One run is bold, italic, amber and curly-underlined in blue at once.")
    stack = (BOLD + ITALIC + f"{CSI}4:3m" + f"{CSI}58:2::120:200:255m"
             + fg_truecolor(255, 200, 80))
    tui.label(row, 4, "CSI 1 m  CSI 3 m  CSI 4:3 m")
    tui.label(row + 1, 4, "CSI 58:2::120:200:255 m  CSI 38:2::255:200:80 m")
    tui.write(move(row + 3, 4), stack, DESCENDERS + " five codes on one run", RESET)
    return tui.footer()


# A filled block of identical text: DECCARA changes attributes only, so a
# uniform fill makes any character rewrite obvious.
_DECCARA_FILL = "DECCARA rectangle change-attributes "


def _deccara(top, left, bottom, right, *ps):
    """DECCARA: CSI Pt;Pl;Pb;Pr ; Ps... $ r over a 1-based inclusive area."""
    coords = ";".join(str(n) for n in (top, left, bottom, right, *ps))
    return f"{CSI}{coords}$r"


def _decsace(extent):
    """DECSACE: CSI Ps * x. 1 = stream (the default), 2 = rectangle."""
    return f"{CSI}{extent}*x"


def _demo_deccara(tui):
    row = tui.page(TITLE, "One DECCARA bolds and underlines rows 2-4: as a stream, then as a block.")
    block_w = 44
    fill = (_DECCARA_FILL * 2)[:block_w]
    left, right = 12, 30
    stream_row = row + 1
    rect_row = row + 9
    for base in (stream_row, rect_row):
        for i in range(5):
            tui.write(move(base + i, 6), tui.fg_default(), fill, RESET)
    tui.label(stream_row - 1, 6, f"CSI 1 * x  CSI {stream_row + 1};{left};{stream_row + 3};{right};1;4 $ r")
    tui.write(_decsace(1))
    tui.write(_deccara(stream_row + 1, left, stream_row + 3, right, 1, 4))
    tui.label(rect_row - 1, 6, f"CSI 2 * x  CSI {rect_row + 1};{left};{rect_row + 3};{right};1;4 $ r")
    tui.write(_decsace(2))
    tui.write(_deccara(rect_row + 1, left, rect_row + 3, right, 1, 4))
    tui.write(_decsace(1))
    return tui.footer()


ENTRIES = [
    Entry(
        title="Basic attributes (SGR 1-8, 22-28)",
        explanation=(
            "SGR 1 draws a heavier stroke and 2 a lighter one, both cleared by "
            "22; 3 slants and 23 rights; 7 swaps foreground and background until "
            "27; 8 hides the run until 28; 5 blinks it until 25."
        ),
        run=_demo_core,
        sequence="\x1b[1mheavier stroke\x1b[22m \x1b[2mlighter stroke\x1b[22m "
        "\x1b[3mslanted glyphs\x1b[23m \x1b[7mfg and bg swap\x1b[27m "
        "\x1b[8mnot drawn\x1b[28m \x1b[5mfades\x1b[25m",
        golden="sgr_core_toggles",
    ),
    Entry(
        title="Underline styles (SGR 4:1-4:5)",
        explanation=(
            "4:1 draws one rule, 4:2 two, 4:3 a wave, 4:4 dots and 4:5 dashes. "
            "Each rule stops at a descender of g, y, p, j or q and picks up "
            "after it rather than crossing the tail."
        ),
        run=_demo_underline_styles,
        sequence="\x1b[4:1m" + DESCENDERS + "\x1b[0m\n"
        "\x1b[4:2m" + DESCENDERS + "\x1b[0m\n"
        "\x1b[4:3m" + DESCENDERS + "\x1b[0m\n"
        "\x1b[4:4m" + DESCENDERS + "\x1b[0m\n"
        "\x1b[4:5m" + DESCENDERS + "\x1b[0m",
        golden="sgr_underline_styles",
    ),
    Entry(
        title="Underline color (SGR 58, 59)",
        explanation=(
            "58:2::r:g:b paints the rule a 24-bit color and 58:5:n a palette "
            "color without touching the glyph color; 59 returns the rule to the "
            "text color. The colored rule still stops at descenders."
        ),
        run=_demo_underline_color,
        sequence="\x1b[4:3m\x1b[58:2::255:80:80m" + DESCENDERS + "\x1b[0m\n"
        "\x1b[4:1m\x1b[58:5:46m" + DESCENDERS + "\x1b[59m\x1b[0m",
        golden="sgr_underline_color",
    ),
    Entry(
        title="Lines and frames (SGR 9, 51-55)",
        explanation=(
            "SGR 9 strikes through at mid height until 29; 53 draws a rule along "
            "the top of the cell until 55; 51 frames every cell of the run and "
            "52 draws that same rectangle, both cleared by 54."
        ),
        run=_demo_lines_boxes,
        sequence="\x1b[9mstruck out\x1b[29m \x1b[53mover the top\x1b[55m "
        "\x1b[51mone box per cell\x1b[54m \x1b[52mnot an ellipse\x1b[54m",
        golden="sgr_lines_boxes",
    ),
    Entry(
        title="Superscript and subscript (SGR 73-75)",
        explanation=(
            "SGR 73 draws the following run at about seven tenths size above "
            "the centre line, 74 at that size below it, and 75 returns to the "
            "baseline. Each glyph still fills one cell, so 'H2O' reads as H two O."
        ),
        run=_demo_supersub,
        sequence=_SUPERSUB_SEQ,
        golden="sgr_super_subscript",
    ),
    Entry(
        title="Stacked attributes (SGR 1, 3, 4:3, 38, 58)",
        explanation=(
            "One run is at once bold (1), italic (3), amber (38:2) and carries a "
            "blue (58:2) curly (4:3) rule. Each code adds exactly its own trait, "
            "and the rule still stops at descenders."
        ),
        run=_demo_stack,
        sequence="\x1b[1m\x1b[3m\x1b[4:3m\x1b[58:2::120:200:255m"
        "\x1b[38:2::255:200:80m" + DESCENDERS + "\x1b[0m",
        golden="sgr_stack",
    ),
    Entry(
        title="Change attributes in an area (DECCARA)",
        explanation=(
            "DECCARA sets bold and underline over four corners. Under DECSACE 1 "
            "(stream, the default) it runs in reading order from the first "
            "corner to the last; under DECSACE 2 it keeps to the rectangle. The "
            "characters themselves never change."
        ),
        run=_demo_deccara,
        sequence="\x1b[8;6H" + _DECCARA_FILL[:32] + "\n"
        "\x1b[9;6H" + _DECCARA_FILL[:32] + "\n"
        "\x1b[10;6H" + _DECCARA_FILL[:32] + "\n"
        "\x1b[2*x\x1b[8;12;10;30;1;4$r\x1b[1*x",
        golden="sgr_deccara",
    ),
]
