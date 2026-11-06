# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""DEC private modes: DECSET, DECRST and the DECRQM report.

Each page reads its modes with DECRQM (CSI ? Ps $ p, answered
CSI ? Ps ; Pm $ y with Pm 1 set, 2 reset, 0 unknown, 3 and 4 permanent) and
shows the mode acting where it has something to draw. Every mode a page
changes is put back to the menu's default before it returns.
"""

from .. import tui as T
from ..tui import CSI, RESET, move, fg_truecolor
from . import Entry

TITLE = "DEC private modes"
ORDER = 220


def _set(ps):
    return f"{CSI}?{ps}h"


def _reset(ps):
    return f"{CSI}?{ps}l"


def _rqm(ps):
    return f"{CSI}?{ps}$p"


def _report(tui, row, ps, name, change=""):
    """Send `change`, then one table row: the query, what it asks, the reply."""
    tui.write(change)
    if change:
        name += ", after CSI ? " + change[3:-1] + " " + change[-1]
    tui.label(row, 4, f"CSI ? {ps} $ p")
    tui.text(row, 20, name, tui.fg_default())
    tui.result(row, 58, T.truncate(T.decode_reply(tui.query(_rqm(ps))), tui.cols - 59))
    return row + 1


def _toggle_page(tui, draw, verb, act, undo):
    """Draw a page; Space runs `act` until the next key, then `undo`."""
    row = draw()
    while True:
        tui.keys(("Space", verb))
        tui.flush()
        key = tui.read_key()
        if key != " ":
            return key, row
        act(row)
        tui.flush()
        tui.read_key()
        undo()
        row = draw()


def _demo_input_modes(tui):
    row = tui.page(TITLE, "With ?7 reset the run stops in the last column; with ?7 set it wraps.")
    edge = max(10, tui.cols - 5)
    tui.label(row, 4, "CSI ? 7 l")
    tui.write(_reset(7), move(row, edge), fg_truecolor(*tui.theme.accent), "ABCDEFGHIJ", RESET)
    tui.label(row + 2, 4, "CSI ? 7 h")
    tui.write(_set(7), move(row + 2, edge), fg_truecolor(*tui.theme.accent2), "ABCDEFGHIJ", RESET)
    line = row + 5
    line = _report(tui, line, 7, "autowrap", _reset(7))
    line = _report(tui, line, 7, "autowrap", _set(7))
    line = _report(tui, line, 1, "cursor keys (SS3)")
    _report(tui, line, 8, "autorepeat")
    key = tui.footer()
    tui.write(_set(7), _reset(1))
    return key


def _demo_column_mode(tui):
    def draw():
        tui.refresh_size()
        row = tui.page(TITLE, f"Space asks for 132 columns; the window is {tui.cols} wide now.")
        line = _report(tui, row, 3, "132 columns")
        _report(tui, line, 40, "allow 80/132")
        return row

    def act(row):
        tui.write(_set(40), _set(3))
        tui.flush()
        tui.refresh_size()
        tui.result(1, 2, f"CSI ? 40 h  CSI ? 3 h  now {tui.cols} columns; any key restores")

    def undo():
        tui.write(_reset(3), _reset(40))
        tui.flush()

    key, _ = _toggle_page(tui, draw, "132 columns", act, undo)
    tui.refresh_size()
    return key


def _demo_screen_origin(tui):
    def draw():
        row = tui.page(TITLE, "Space reverses the whole screen; CUP 1;1 under DECOM lands on the margin.")
        # DECOM: margins on the two rows below the table, origin on, home.
        top = row + 4
        tui.label(top - 1, 4, f"CSI {top} ; {top + 1} r   CSI ? 6 h   CSI 1 ; 5 H")
        tui.write(f"{CSI}{top};{top + 1}r", _set(6), f"{CSI}1;5H",
                  fg_truecolor(*tui.theme.accent2), "row 1 is the top margin", RESET,
                  _reset(6), f"{CSI}r")
        line = _report(tui, row, 5, "reverse video")
        _report(tui, line, 6, "origin")
        return row

    def act(row):
        tui.write(_set(5))
        tui.result(row + 7, 4, "CSI ? 5 h  any key sends CSI ? 5 l")

    # draw() already left DECOM reset; resetting it again would home the cursor.
    key, _ = _toggle_page(tui, draw, "reverse video", act, lambda: tui.write(_reset(5)))
    tui.write(_reset(5))
    return key


def _demo_alt_variants(tui):
    row = tui.page(TITLE, "The alternate-buffer modes as DECRQM reports them on this page.")
    for ps, name in ((47, "alternate buffer"), (1047, "alternate, cleared"),
                     (1048, "saved cursor"), (1049, "saved cursor + alt")):
        row = _report(tui, row, ps, name)
    return tui.footer()


def _demo_io_modes(tui):
    row = tui.page(TITLE, "Bracketed paste, focus reports and synchronized output, each off.")
    for ps, name in ((2004, "bracketed paste"), (1004, "focus reports"),
                     (2026, "synchronized output")):
        row = _report(tui, row, ps, name)
    key = tui.footer()
    tui.write(_reset(2004), _reset(1004), _reset(2026))
    return key


def _demo_cursor_modes(tui):
    def draw():
        row = tui.page(TITLE, "Space shows a blinking cursor at the arrow; any key hides it.")
        line = _report(tui, row, 12, "blink")
        _report(tui, line, 25, "visible")
        tui.label(row + 3, 4, "CSI ? 25 h  CSI ? 12 h  ->")
        return row

    def act(row):
        tui.write(_set(25), _set(12), move(row + 3, 32))

    def undo():
        tui.write(_reset(12), T.HIDE_CURSOR)

    key, _ = _toggle_page(tui, draw, "show cursor", act, undo)
    tui.write(_reset(12), T.HIDE_CURSOR)
    return key


def _demo_mouse_overview(tui):
    row = tui.page(TITLE, "The mouse modes as DECRQM reports them; the Mouse section drives them.")
    for ps, name in ((1000, "press and release"), (1002, "drag"),
                     (1003, "any motion"), (1006, "SGR encoding")):
        row = _report(tui, row, ps, name)
    key = tui.footer()
    tui.write(_reset(1000), _reset(1002), _reset(1003), _reset(1006))
    return key


def _demo_alt_gate(tui):
    row = tui.page(TITLE, "The alternate-screen gate reads set; resetting it would drop this menu.")
    _report(tui, row, 1046, "alternate screen allowed")
    tui.write(_set(1046))
    return tui.footer()


def _demo_color_scheme(tui):
    row = tui.page(TITLE, "DECRQM reads 1 after CSI ? 2031 h and 2 after CSI ? 2031 l.")
    line = _report(tui, row, 2031, "scheme reports", _set(2031))
    _report(tui, line, 2031, "scheme reports", _reset(2031))
    key = tui.footer()
    tui.write(_reset(2031))
    return key


ENTRIES = [
    Entry(
        title="Autowrap and key modes (DECAWM, DECCKM, DECARM)",
        explanation=(
            "With ?7 reset a run printed past the right margin stops in the "
            "last column; with ?7 set it continues on the next row. DECCKM ?1 "
            "picks SS3 arrow keys and DECARM ?8 autorepeat; DECRQM reports all "
            "three."),
        run=_demo_input_modes,
        sequence="\x1b[?7l\x1b[?7$p",
        expect=r"\x1b\[\?7;[12]\$y",
        snapshot=False,
    ),
    Entry(
        title="Column mode (DECCOLM)",
        explanation=(
            "With ?40 set, CSI ? 3 h asks for 132 columns: the window widens, "
            "the screen clears and the cursor homes. With ?40 reset ?3 does "
            "nothing. ?3 and ?40 are reset on return."),
        run=_demo_column_mode,
        sequence="\x1b[?40h\x1b[?3h\x1b[?3$p",
        expect=r"\x1b\[\?3;[12]\$y",
    ),
    Entry(
        title="Reverse video and origin (DECSCNM, DECOM)",
        explanation=(
            "CSI ? 5 h draws the whole screen with foreground and background "
            "swapped until CSI ? 5 l. Under DECOM ?6, row 1 of a CUP is the top "
            "margin. Both are reset on return."),
        run=_demo_screen_origin,
        sequence="\x1b[?5h\x1b[?5$p",
        expect=r"\x1b\[\?5;[12]\$y",
    ),
    Entry(
        title="Alternate screen modes (?47, ?1047, ?1048, ?1049)",
        explanation=(
            "DECRQM reports each alternate-buffer mode as 1 set or 2 reset. The "
            "page only reads them; the Screens section enters and leaves the "
            "buffer."),
        run=_demo_alt_variants,
        sequence="\x1b[?1049$p",
        expect=r"\x1b\[\?1049;[1234]\$y",
        snapshot=False,
    ),
    Entry(
        title="Paste, focus and sync modes (?2004, ?1004, ?2026)",
        explanation=(
            "DECRQM reports bracketed paste, focus reporting and synchronized "
            "output as 1 set or 2 reset. All three are reset on return."),
        run=_demo_io_modes,
        sequence="\x1b[?2004$p",
        expect=r"\x1b\[\?2004;[1234]\$y",
        snapshot=False,
    ),
    Entry(
        title="Cursor blink and visibility (?12, DECTCEM)",
        explanation=(
            "CSI ? 25 h shows the text cursor and CSI ? 12 h makes it blink; "
            "their l forms hide it and hold it steady. The cursor is hidden and "
            "steady on return."),
        run=_demo_cursor_modes,
        sequence="\x1b[?25h\x1b[?12h\x1b[?25$p",
        expect=r"\x1b\[\?25;[12]\$y",
        snapshot=False,
    ),
    Entry(
        title="Mouse modes (?1000, ?1002, ?1003, ?1006)",
        explanation=(
            "DECRQM reports press and release, drag, any-motion tracking and "
            "SGR encoding as 1 set or 2 reset. All four are reset on return."),
        run=_demo_mouse_overview,
        sequence="\x1b[?1000$p",
        expect=r"\x1b\[\?1000;[1234]\$y",
        snapshot=False,
    ),
    Entry(
        title="Alternate screen gate (?1046)",
        explanation=(
            "While ?1046 is set, the default, ?47, ?1047 and ?1049 switch "
            "buffers; reset, they do nothing and the primary screen returns. "
            "DECRQM reports 1, and the page leaves it set."),
        run=_demo_alt_gate,
        sequence="\x1b[?1046$p",
        expect=r"\x1b\[\?1046;[1234]\$y",
        snapshot=False,
    ),
    Entry(
        title="Color scheme notification (?2031)",
        explanation=(
            "While ?2031 is set the terminal sends CSI ? 997 ; 1 n when its "
            "background turns dark and CSI ? 997 ; 2 n when it turns light. "
            "DECRQM follows h and l; it is reset on return."),
        run=_demo_color_scheme,
        sequence="\x1b[?2031h\x1b[?2031$p",
        expect=r"\x1b\[\?2031;[12]\$y",
        snapshot=False,
    ),
]
