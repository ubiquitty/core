# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""Color: the indexed palette, truecolor, and the OSC color controls.

Every entry that changes a palette slot or a dynamic color sends the matching
reset (OSC 104, 110-119) on every path out, so the menu never inherits it. Each
entry's `sequence` is a self-contained round trip the snapshot renders.
"""

from .. import tui as T
from ..tui import CSI, OSC, ST, RESET, move, bg_truecolor, bg_256
from . import Entry

TITLE = "Color"
ORDER = 10


def _ramps_seq():
    out = ["SGR 40-47 and 100-107:\r\n"]
    for code in list(range(40, 48)) + list(range(100, 108)):
        out.append(f"{CSI}{code}m  {RESET}")
    out.append("\r\nCSI 48;5;N, indices 16-231:\r\n")
    for block in range(6):
        for i in range(36):
            out.append(bg_256(16 + block * 36 + i) + " " + RESET)
        out.append("\r\n")
    out.append("indices 232-255, the gray ramp:\r\n")
    for idx in range(232, 256):
        out.append(bg_256(idx) + "  " + RESET)
    out.append("\r\n")
    return "".join(out)


def _gradient_color(x, y, width, height):
    fx = x / max(1, width - 1)
    fy = y / max(1, height - 1)
    return (int(255 * (1 - fx)), int(255 * fx * (1 - fy * 0.5)),
            int(255 * (0.3 + 0.7 * fy)))


def _gradient_seq():
    out = []
    width, height = 60, 8
    for y in range(height):
        for x in range(width):
            out.append(bg_truecolor(*_gradient_color(x, y, width, height)) + " ")
        out.append(RESET + "\r\n")
    return "".join(out)


_RAMPS_SEQ = _ramps_seq()
_GRADIENT_SEQ = _gradient_seq()
# OSC 4: redefine palette index 1 to orange, then paint it; reset on exit.
_OSC4_SEQ = (f"{OSC}4;1;rgb:ff/7a/18{ST}{CSI}48;5;1m      {RESET}\r\n"
             f"{OSC}104;1{ST}")
# Override indices 1 and 2, paint both, then reset index 1 (OSC 104;1) and the
# rest (a bare OSC 104); the trailing repaints prove the reset took.
_OSC104_SEQ = (f"{OSC}4;1;rgb:00/ff/88{ST}{OSC}4;2;rgb:ff/00/aa{ST}"
               f"{CSI}48;5;1m  {RESET}{CSI}48;5;2m  {RESET}"
               " OSC 4 overrides indices 1 and 2\r\n"
               f"{OSC}104;1{ST}{OSC}104{ST}"
               f"{CSI}48;5;1m  {RESET}{CSI}48;5;2m  {RESET}"
               " OSC 104 restores the startup palette\r\n")
# OSC 10/11/12 set default fg/bg/cursor, then reset via OSC 110/111/112.
_DYNAMIC_SEQ = (f"{OSC}10;#d0d6e4{ST}{OSC}11;#10121a{ST}{OSC}12;#7aa2f7{ST}"
                "OSC 10, 11 and 12 set the default fg, bg and cursor\r\n"
                f"{OSC}110{ST}{OSC}111{ST}{OSC}112{ST}")
_SGR_FORMS_SEQ = (
    f"{CSI}38:2::255:128:0mcolon truecolor (38:2::r:g:b){RESET}\r\n"
    f"{CSI}38;2;0;200;255msemicolon truecolor (38;2;r;g;b){RESET}\r\n"
    f"{CSI}48:2::40:60:120mcolon bg (48:2::r:g:b){RESET} "
    f"{CSI}48;2;40;60;120msemicolon bg (48;2;r;g;b){RESET}\r\n")


def _reply(data):
    return T.decode_reply(data)


def _demo_ramps(tui):
    row = tui.page(TITLE, "The 16 palette colors, the 6x6x6 cube and the gray ramp, as swatches.")
    tui.label(row, 4, "SGR 40-47, 100-107")
    tui.write(move(row + 1, 4))
    for code in list(range(40, 48)) + list(range(100, 108)):
        tui.write(f"{CSI}{code}m  {RESET}")
    row += 3
    tui.label(row, 4, "CSI 48;5;N m   N = 16-231")
    for block in range(6):
        tui.write(move(row + 1 + block, 4))
        for i in range(36):
            tui.write(bg_256(16 + block * 36 + i), " ", RESET)
    row += 8
    tui.label(row, 4, "CSI 48;5;N m   N = 232-255")
    tui.write(move(row + 1, 4))
    for idx in range(232, 256):
        tui.write(bg_256(idx), "  ", RESET)
    return tui.footer()


def _demo_gradient(tui):
    row = tui.page(TITLE, "Every cell has its own 24-bit background; the block reads as one blend.")
    tui.label(row, 4, "CSI 48:2::r:g:b m   per cell")
    top = row + 2
    height = min(12, tui.rows - top - 2)
    width = min(tui.cols - 8, 120)
    for y in range(height):
        tui.write(move(top + y, 4))
        for x in range(width):
            tui.write(bg_truecolor(*_gradient_color(x, y, width, height)), " ")
        tui.write(RESET)
    return tui.footer()


def _demo_osc4(tui):
    tui.write(f"{OSC}4;1;rgb:ff/7a/18{ST}")
    row = tui.page(TITLE, "Palette index 1 is orange instead of red while this page is open.")
    tui.write(move(row, 4), f"{CSI}48;5;1m      {RESET}")
    tui.label(row, 12, "CSI 48;5;1 m")
    tui.label(row + 2, 4, "OSC 4 ; 1 ; rgb:ff/7a/18 ST   set index 1")
    tui.label(row + 3, 4, "OSC 4 ; 1 ; ? ST")
    tui.result(row + 3, 22, _reply(tui.query(f"{OSC}4;1;?{ST}")))
    tui.label(row + 4, 4, "OSC 104 ; 1 ST                on the way out")
    key = tui.footer()
    tui.write(f"{OSC}104;1{ST}")
    tui.flush()
    return key


def _demo_osc104(tui):
    tui.write(f"{OSC}4;1;rgb:00/ff/88{ST}{OSC}4;2;rgb:ff/00/aa{ST}")
    row = tui.page(TITLE, "Indices 1 and 2 are teal and magenta; Space resets them to red and green.")

    def swatches(at):
        tui.write(move(at, 4), f"{CSI}48;5;1m    {RESET} {CSI}48;5;2m    {RESET}")

    swatches(row)
    tui.label(row, 15, "OSC 4 ; 1 ; rgb:00/ff/88 ST   OSC 4 ; 2 ; rgb:ff/00/aa ST")
    tui.keys(("Space", "reset"))
    tui.flush()
    key = tui.read_key()
    if key == " ":
        tui.write(f"{OSC}104;1{ST}{OSC}104{ST}")
        tui.label(row + 2, 15, "OSC 104 ; 1 ST   then a bare OSC 104 ST")
        swatches(row + 2)
        key = tui.footer()
    tui.write(f"{OSC}104{ST}")
    tui.flush()
    return key


_DEFAULTS_SET = f"{OSC}10;#ffd000{ST}{OSC}11;#202840{ST}{OSC}12;#f7768e{ST}"
_DEFAULTS_RESET = f"{OSC}110{ST}{OSC}111{ST}{OSC}112{ST}"


def _demo_defaults(tui):
    caption = "Space turns default text yellow, the background navy, the cursor pink."
    applied = False
    try:
        while True:
            row = tui.page(TITLE, caption)
            tui.label(row, 4, "OSC 10 ; #ffd000 ST   OSC 11 ; #202840 ST   OSC 12 ; #f7768e ST")
            tui.label(row + 1, 4, "OSC 110 ST            OSC 111 ST            OSC 112 ST   restore")
            for i, code in enumerate((10, 11, 12)):
                tui.label(row + 3 + i, 4, f"OSC {code} ; ? ST")
                tui.result(row + 3 + i, 20, _reply(tui.query(f"{OSC}{code};?{ST}")))
            tui.write(T.SHOW_CURSOR, move(row + 7, 4))
            tui.keys(("Space", "restore" if applied else "apply"))
            tui.write(move(row + 7, 4))
            tui.flush()
            key = tui.read_key()
            if key != " ":
                return key
            tui.write(_DEFAULTS_RESET if applied else _DEFAULTS_SET)
            applied = not applied
    finally:
        tui.write(_DEFAULTS_RESET, T.HIDE_CURSOR)
        tui.flush()


_OSC17_SET = f"{OSC}17;rgb:30/50/c0{ST}"
_OSC19_SET = f"{OSC}19;rgb:ff/ff/00{ST}"
_OSC117_RESET = f"{OSC}117{ST}"
_OSC119_RESET = f"{OSC}119{ST}"
_SELECTION_SEQ = (_OSC17_SET + _OSC19_SET
                  + "OSC 17 and 19 set the selection bg and fg\r\n"
                  + _OSC117_RESET + _OSC119_RESET
                  + "OSC 117 and 119 clear both\r\n")


def _demo_selection_color(tui):
    tui.write(_OSC17_SET, _OSC19_SET)
    row = tui.page(TITLE, "Selected text is yellow on blue here; drag over the line below.")
    tui.text(row, 4, "Select these words to see the selection colors.", tui.fg_default())
    tui.label(row + 2, 4, "OSC 17 ; rgb:30/50/c0 ST   selection background")
    tui.label(row + 3, 4, "OSC 19 ; rgb:ff/ff/00 ST   selection foreground")
    tui.label(row + 5, 4, "OSC 17 ; ? ST")
    tui.result(row + 5, 20, _reply(tui.query(f"{OSC}17;?{ST}")))
    tui.label(row + 6, 4, "OSC 19 ; ? ST")
    tui.result(row + 6, 20, _reply(tui.query(f"{OSC}19;?{ST}")))
    key = tui.footer()
    tui.write(_OSC117_RESET, _OSC119_RESET)
    tui.flush()
    return key


_SPECIAL_COLORS = [
    (13, 113, "pointer foreground", "rgb:ff/40/40"),
    (14, 114, "pointer background", "rgb:20/20/20"),
    (15, 115, "Tektronix foreground", "rgb:40/ff/40"),
    (16, 116, "Tektronix background", "rgb:00/10/20"),
    (18, 118, "Tektronix cursor", "rgb:40/a0/ff"),
]
_SPECIAL_SEQ = "".join(
    f"{OSC}{setn};{spec}{ST}{OSC}{resetn}{ST}"
    for setn, resetn, _label, spec in _SPECIAL_COLORS)


def _demo_special_colors(tui):
    row = tui.page(TITLE, "Each color is set, queried back and cleared; the replies show the value.")
    replies = []
    for setn, resetn, label, spec in _SPECIAL_COLORS:
        tui.write(f"{OSC}{setn};{spec}{ST}")
        replies.append(tui.query(f"{OSC}{setn};?{ST}"))
        tui.write(f"{OSC}{resetn}{ST}")
    if not any(replies):
        tui.unavailable(row, "No reply to OSC 13-18 ; ?, so there is nothing to show.")
        return tui.footer()
    for (setn, resetn, label, spec), reply in zip(_SPECIAL_COLORS, replies):
        tui.label(row, 4, f"OSC {setn} ; {spec}")
        tui.text(row, 26, label, tui.fg_default())
        tui.result(row, 48, T.truncate(_reply(reply), tui.cols - 49))
        row += 1
    tui.label(row + 1, 4, "OSC 113-118 ST clear each one after its query.")
    return tui.footer()


def _demo_sgr_forms(tui):
    row = tui.page(TITLE, "Each pair names one color twice, so both rows of a pair match.")

    def line(seq_text, sample_seq, sample):
        nonlocal row
        tui.label(row, 4, seq_text)
        tui.write(move(row, 30), sample_seq, sample, RESET)
        row += 1

    line("CSI 38:2::255:128:0 m", f"{CSI}38:2::255:128:0m", "colon foreground")
    line("CSI 38;2;255;128;0 m", f"{CSI}38;2;255;128;0m", "semicolon foreground")
    row += 1
    line("CSI 48:2::40:60:120 m", f"{CSI}48:2::40:60:120m", "  colon background  ")
    line("CSI 48;2;40;60;120 m", f"{CSI}48;2;40;60;120m", "  semicolon background  ")
    return tui.footer()


_MINI_THEME = {
    "palette": {1: "rgb:f7/76/8e", 2: "rgb:9e/ce/6a", 4: "rgb:7a/a2/f7"},
    "fg": "#e0def4",
    "bg": "#191724",
    "cursor": "#f6c177",
}
_THEME_SEQUENCE = (
    f"{OSC}4;1;{_MINI_THEME['palette'][1]}{ST}"
    f"{OSC}4;2;{_MINI_THEME['palette'][2]}{ST}"
    f"{OSC}4;4;{_MINI_THEME['palette'][4]}{ST}"
    f"{OSC}10;{_MINI_THEME['fg']}{ST}"
    f"{OSC}11;{_MINI_THEME['bg']}{ST}"
    f"{OSC}12;{_MINI_THEME['cursor']}{ST}"
)
_THEME_RESET = (f"{OSC}104;1{ST}{OSC}104;2{ST}{OSC}104;4{ST}"
                f"{OSC}110{ST}{OSC}111{ST}{OSC}112{ST}")


def _demo_theme_swap(tui):
    caption = "Space re-themes the page: palette, text, background and cursor."
    applied = False
    try:
        while True:
            row = tui.page(TITLE, caption)
            tui.write(move(row, 4))
            for idx in (1, 2, 4):
                tui.write(bg_256(idx), "    ", RESET, " ")
            tui.write(move(row, 21), tui.fg_default(), "default text", RESET)
            tui.label(row + 2, 4, "OSC 4 ; 1, 2, 4 ; <color> ST   OSC 10, 11, 12 ; <color> ST")
            tui.label(row + 3, 4, "OSC 104 ; 1, 2, 4 ST           OSC 110, 111, 112 ST   restore")
            tui.keys(("Space", "restore" if applied else "apply"))
            tui.flush()
            key = tui.read_key()
            if key != " ":
                return key
            tui.write(_THEME_RESET if applied else _THEME_SEQUENCE)
            applied = not applied
    finally:
        tui.write(_THEME_RESET)
        tui.flush()


ENTRIES = [
    Entry(
        title="Palette ramps (SGR 40-47, 48;5)",
        explanation=(
            "The 16 palette backgrounds, the 6x6x6 cube of indices 16-231 in six "
            "rows of 36, and the 232-255 gray ramp from near-black to near-white. "
            "Every swatch fills whole cells."),
        run=_demo_ramps,
        sequence=_RAMPS_SEQ,
        golden="color_ramps",
    ),
    Entry(
        title="Truecolor gradient (SGR 48:2)",
        explanation=(
            "Each cell carries its own 24-bit background: red falls left to right "
            "while blue rises top to bottom. Neighbours differ by a level or two, "
            "so the block reads as one blend with no stair-steps."),
        run=_demo_gradient,
        sequence=_GRADIENT_SEQ,
        golden="color_gradient",
    ),
    Entry(
        title="Palette override (OSC 4)",
        explanation=(
            "OSC 4 ; 1 ; rgb:ff/7a/18 ST turns every cell painted with index 1 "
            "orange, and OSC 4 ; 1 ; ? ST answers with the new value. Cells hold "
            "the index, so OSC 104 ; 1 turns them red again."),
        run=_demo_osc4,
        sequence=_OSC4_SEQ,
        golden="color_osc4_palette",
    ),
    Entry(
        title="Palette reset (OSC 104)",
        explanation=(
            "OSC 104 ; 1 restores index 1 to the startup palette and a bare "
            "OSC 104 restores all 256, so swatches overridden to teal and magenta "
            "return to red and green."),
        run=_demo_osc104,
        sequence=_OSC104_SEQ,
        golden="color_osc104_reset",
    ),
    Entry(
        title="Default colors (OSC 10, 11, 12)",
        explanation=(
            "OSC 10, 11 and 12 set the default foreground, background and cursor "
            "color under every cell drawn in them, and answer a ? query with the "
            "current value. OSC 110, 111 and 112 restore the configured colors."),
        run=_demo_defaults,
        sequence=_DYNAMIC_SEQ,
        golden="color_dynamic_osc",
    ),
    Entry(
        title="Selection colors (OSC 17, 19)",
        explanation=(
            "OSC 17 sets the selection background and OSC 19 its text color, "
            "leaving the palette alone; a ? query answers once the color is set. "
            "OSC 117 and 119 clear both."),
        run=_demo_selection_color,
        sequence=_SELECTION_SEQ,
        golden="color_selection_osc17_19",
    ),
    Entry(
        title="Pointer and Tektronix colors (OSC 13-18)",
        explanation=(
            "OSC 13 and 14 set the pointer colors and 15, 16 and 18 the Tektronix "
            "colors; each answers a following ? with OSC <n> ; rgb:RRRR/GGGG/BBBB "
            "ST. OSC 113-118 clear them. No grid cell shows these colors."),
        run=_demo_special_colors,
        sequence=_SPECIAL_SEQ,
        golden="color_special_osc113_118",
    ),
    Entry(
        title="Truecolor encodings (SGR 38:2, 38;2)",
        explanation=(
            "The ISO 8613-6 colon form (38:2::r:g:b) and the semicolon form "
            "(38;2;r;g;b) name the same color, so both foreground rows are one "
            "orange and both background rows one blue."),
        run=_demo_sgr_forms,
        sequence=_SGR_FORMS_SEQ,
        golden="color_sgr_forms",
    ),
    Entry(
        title="Theme swap (OSC 4, 10, 11, 12)",
        explanation=(
            "OSC 4 for palette indices 1, 2 and 4 and OSC 10, 11 and 12 for the "
            "defaults re-theme the running session with no restart. OSC 104, 110, "
            "111 and 112 hand back the terminal's own colors."),
        run=_demo_theme_swap,
        sequence=_THEME_SEQUENCE,
        golden="color_theme_swap",
    ),
]
