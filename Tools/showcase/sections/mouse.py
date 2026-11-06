# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""Mouse & selection: the xterm tracking modes and encodings, focus reports,
alternate scroll, pointer shapes, and the DEC locator.

Each live page starts by resetting every mode this section can set, enables
only its own, and resets them all again on exit; the menu turns its own mouse
reporting back on afterwards. The tracking pages read reports through
`tui.read_key()`, which decodes the SGR form; the encoding, focus, and
alternate-scroll pages read the tty directly to show the bytes as sent.
"""

import os
import select

from .. import tui as T
from ..tui import CSI, OSC, ST, move
from . import Entry

TITLE = "Mouse and selection"
ORDER = 140

_X10 = 9
_NORMAL = 1000
_HILITE = 1001
_BUTTON_EVENT = 1002
_ANY_EVENT = 1003
_FOCUS = 1004
_SGR = 1006
_ALTERNATE_SCROLL = 1007
_URXVT = 1015

_ALL_MOUSE_MODES = (_X10, _NORMAL, _HILITE, _BUTTON_EVENT, _ANY_EVENT, _FOCUS,
                    _SGR, _ALTERNATE_SCROLL, _URXVT)


def _dec_set(ps):
    return f"{CSI}?{ps}h"


def _dec_reset(ps):
    return f"{CSI}?{ps}l"


def _enable_only(tui, modes):
    for ps in _ALL_MOUSE_MODES:
        tui.write(_dec_reset(ps))
    for ps in modes:
        tui.write(_dec_set(ps))
    tui.flush()


def _disable_all(tui):
    _enable_only(tui, ())
    tui.set_mouse(False)


def _button_name(b):
    """Name an xterm button code: low bits pick the button, 32 marks motion,
    64 the wheel, and 4, 8, 16 Shift, Meta, Ctrl."""
    modifiers = [name for bit, name in ((4, "Shift"), (8, "Meta"), (16, "Ctrl")) if b & bit]
    if b & 64:
        base = "wheel up" if (b & 3) == 0 else "wheel down"
    elif b & 32:
        base = {0: "left drag", 1: "middle drag", 2: "right drag", 3: "move"}[b & 3]
    else:
        base = {0: "left", 1: "middle", 2: "right", 3: "release"}[b & 3]
    return base + ("+" + "+".join(modifiers) if modifiers else "")


class _Log:
    """A newest-first list of report lines from row `top` to above the key bar."""

    def __init__(self, tui, top):
        self.tui = tui
        self.top = top
        self.height = max(2, tui.rows - 1 - top)
        self.lines = []

    def add(self, line):
        self.lines = ([line] + self.lines)[:self.height]
        for i in range(self.height):
            self.tui.write(move(self.top + i, 1), T.CLEAR_LINE)
        for i, text in enumerate(self.lines):
            self.tui.result(self.top + i, 4, T.truncate(text, self.tui.cols - 6))
        self.tui.flush()


def _tracking(tui, caption, labels, modes):
    row = tui.page(TITLE, caption)
    for i, text in enumerate(labels):
        tui.label(row + i, 4, text)
    log = _Log(tui, row + len(labels) + 1)
    _enable_only(tui, modes)
    tui.keys()
    tui.flush()
    while True:
        key = tui.read_key()
        if not (isinstance(key, tuple) and key[0] == "mouse"):
            break
        _, b, column, line, pressed = key
        log.add(f"CSI < {b} ; {column} ; {line} {'M' if pressed else 'm'}"
                f"   {_button_name(b)} {'press' if pressed else 'release'} at {column}, {line}")
    _disable_all(tui)
    return key


def _read_raw(tui, gap=0.05):
    """One input burst as raw bytes, gathered across a short gap."""
    first = os.read(tui._fd, 1)
    if not first:
        return b""
    data = bytearray(first)
    while first == b"\x1b":
        ready, _, _ = select.select([tui._fd], [], [], gap)
        if not ready:
            break
        more = os.read(tui._fd, 64)
        if not more:
            break
        data += more
    return bytes(data)


def _demo_x10(tui):
    return _tracking(tui, "Each click logs one press and nothing for the release.",
                     ["CSI ? 9 h  CSI ? 1006 h   X10 tracking, SGR framing"],
                     (_X10, _SGR))


def _demo_normal(tui):
    return _tracking(tui, "Each click logs a press and then a release at the same cell.",
                     ["CSI ? 1000 h  CSI ? 1006 h   normal tracking, SGR framing"],
                     (_NORMAL, _SGR))


def _demo_button_event(tui):
    return _tracking(tui, "A drag logs a run of drag reports; moving with no button logs nothing.",
                     ["CSI ? 1002 h  CSI ? 1006 h   button-event tracking, SGR framing"],
                     (_BUTTON_EVENT, _SGR))


def _demo_any_event(tui):
    return _tracking(tui, "Moving the pointer logs a move report, code 35, for every cell it crosses.",
                     ["CSI ? 1003 h  CSI ? 1006 h   any-event tracking, SGR framing"],
                     (_ANY_EVENT, _SGR))


def _demo_hilite(tui):
    return _tracking(tui, "Each click logs its press under hilite tracking.",
                     ["CSI ? 1001 h  CSI ? 1006 h   hilite tracking, SGR framing"],
                     (_HILITE, _SGR))


_ENCODINGS = [("default", None), ("SGR", _SGR), ("urxvt", _URXVT)]


def _demo_encodings(tui):
    row = tui.page(TITLE, "One click, framed three ways: Tab switches the encoding.")
    names = {None: "CSI M Cb Cx Cy, each byte value + 32",
             _SGR: "CSI < b ; x ; y M, m on release",
             _URXVT: "CSI b+32 ; x ; y M, release too"}
    index = 0
    log = _Log(tui, row + 6)
    try:
        while True:
            name, ps = _ENCODINGS[index]
            _enable_only(tui, (_NORMAL,) + ((ps,) if ps else ()))
            for i, (other, other_ps) in enumerate(_ENCODINGS):
                sequence = f"CSI ? {other_ps} h" if other_ps else "neither"
                text = f"{'>' if i == index else ' '} {other:<8} {sequence:<14} {names[other_ps]}"
                if i == index:
                    tui.result(row + i, 4, text)
                else:
                    tui.label(row + i, 4, text)
            tui.label(row + 4, 4, "CSI ? 1000 h   normal tracking carries the clicks")
            tui.keys(("Tab", "next encoding"))
            tui.flush()
            data = _read_raw(tui)
            if data == b"\t":
                index = (index + 1) % len(_ENCODINGS)
                continue
            if data.startswith(b"\x1b[M") or (data.startswith(b"\x1b[") and data[-1:] in (b"M", b"m")):
                log.add(f"{name:<8} {T.decode_reply(data)}")
                continue
            return "esc"
    finally:
        _disable_all(tui)


def _demo_focus(tui):
    row = tui.page(TITLE, "Switching to another window and back logs CSI O, then CSI I.")
    tui.label(row, 4, "CSI ? 1004 h   focus reporting")
    log = _Log(tui, row + 2)
    _enable_only(tui, (_FOCUS,))
    tui.keys()
    tui.flush()
    try:
        while True:
            data = _read_raw(tui)
            if data == b"\x1b[I":
                log.add("CSI I   focus in")
            elif data == b"\x1b[O":
                log.add("CSI O   focus out")
            else:
                return "esc"
    finally:
        _disable_all(tui)


def _demo_alternate_scroll(tui):
    row = tui.page(TITLE, "Each wheel notch logs an arrow key instead of scrolling the page.")
    tui.label(row, 4, "CSI ? 1007 h   alternate scroll, no tracking mode")
    log = _Log(tui, row + 2)
    _enable_only(tui, (_ALTERNATE_SCROLL,))
    tui.keys()
    tui.flush()
    arrows = {b"\x1b[A": "up", b"\x1b[B": "down", b"\x1bOA": "up", b"\x1bOB": "down"}
    try:
        while True:
            data = _read_raw(tui)
            if data not in arrows:
                return "esc"
            log.add(f"{T.decode_reply(data):<10} {arrows[data]}")
    finally:
        _disable_all(tui)


_SHAPES = ["pointer", "text", "crosshair", "grab", "wait", "not-allowed"]


def _demo_pointer_shape(tui):
    row = tui.page(TITLE, "Press 1-6 and the pointer over the window takes that shape; 0 resets it.")
    for i, name in enumerate(_SHAPES, 1):
        tui.label(row + i - 1, 4, f"{i}   OSC 22 ; {name} ST")
    tui.label(row + len(_SHAPES), 4, "0   OSC 22 ; ST")
    reply = tui.query(f"{OSC}22;?__current__{ST}")
    if reply:
        tui.label(row + len(_SHAPES) + 2, 4, "OSC 22 ; ? __current__ ST")
        tui.result(row + len(_SHAPES) + 2, 32, T.decode_reply(reply))
    tui.keys(("1-6", "set shape"), ("0", "reset"))
    tui.flush()
    try:
        while True:
            key = tui.read_key()
            if key == "0":
                tui.write(f"{OSC}22;{ST}")
            elif isinstance(key, str) and key in "123456" and len(key) == 1:
                tui.write(f"{OSC}22;{_SHAPES[int(key) - 1]}{ST}")
            else:
                return key
            tui.flush()
    finally:
        tui.write(f"{OSC}22;{ST}")
        tui.flush()


def _demo_dec_locator(tui):
    row = tui.page(TITLE, "Move the pointer over the window, then Space: the reply carries its cell.")
    tui.label(row, 4, "CSI 1 ; 2 ' z   arm the locator in cells")
    tui.label(row + 1, 4, "CSI 1 ' |       poll it; CSI 0 ; 0 ' z disarms on exit")
    tui.set_mouse(True)
    tui.write(f"{CSI}1;2'z")
    try:
        polls = 0
        while True:
            reply = tui.query(f"{CSI}1'|")
            if not reply:
                tui.unavailable(row + 3, "No reply to DECRQLP, so there is no locator to poll.")
                return tui.footer()
            polls += 1
            tui.write(move(row + 3, 1), T.CLEAR_LINE)
            tui.result(row + 3, 4, f"poll {polls}   {T.decode_reply(reply)}")
            tui.keys(("Space", "poll again"))
            tui.flush()
            key = tui.wait_key()
            if key != " ":
                return key
    finally:
        tui.write(f"{CSI}0;0'z")
        tui.set_mouse(False)


ENTRIES = [
    Entry(
        title="X10 tracking (?9)",
        explanation=(
            "CSI ? 9 h reports button presses only: a click yields one press "
            "report, with no release and no modifier bits."),
        run=_demo_x10,
        sequence="\x1b[?9h\x1b[?9$p",
        expect=r"\x1b\[\?9;1\$y",
        snapshot=False,
    ),
    Entry(
        title="Normal tracking (?1000)",
        explanation=(
            "CSI ? 1000 h reports the press and the release, with the Shift, "
            "Meta, and Ctrl bits, and no motion."),
        run=_demo_normal,
        sequence="\x1b[?1000h\x1b[?1000$p",
        expect=r"\x1b\[\?1000;1\$y",
        snapshot=False,
    ),
    Entry(
        title="Drag tracking (?1002)",
        explanation=(
            "CSI ? 1002 h adds motion reports while a button is held, so a drag "
            "yields drag reports between the press and the release; motion with "
            "no button held reports nothing."),
        run=_demo_button_event,
        sequence="\x1b[?1002h\x1b[?1002$p",
        expect=r"\x1b\[\?1002;1\$y",
        snapshot=False,
    ),
    Entry(
        title="Motion tracking (?1003)",
        explanation=(
            "CSI ? 1003 h reports every pointer motion, button held or not; a "
            "move with no button carries code 35, no-button 3 plus motion 32."),
        run=_demo_any_event,
        sequence="\x1b[?1003h\x1b[?1003$p",
        expect=r"\x1b\[\?1003;1\$y",
        snapshot=False,
    ),
    Entry(
        title="Report encodings (?1006, ?1015)",
        explanation=(
            "The default report is CSI M and three bytes of value + 32, which "
            "fail past cell 223. CSI ? 1006 h sends CSI < b ; x ; y with M for a "
            "press and m for a release; CSI ? 1015 h sends CSI b+32 ; x ; y M "
            "for both."),
        run=_demo_encodings,
        sequence="\x1b[?1006h\x1b[?1006$p",
        expect=r"\x1b\[\?1006;1\$y",
        snapshot=False,
    ),
    Entry(
        title="Focus reporting (?1004)",
        explanation=(
            "With CSI ? 1004 h the terminal sends CSI I when its window gains "
            "focus and CSI O when it loses it."),
        run=_demo_focus,
        sequence="\x1b[?1004h\x1b[?1004$p",
        expect=r"\x1b\[\?1004;1\$y",
        snapshot=False,
    ),
    Entry(
        title="Alternate scroll (?1007)",
        explanation=(
            "With CSI ? 1007 h and no tracking mode, the wheel on the alternate "
            "screen sends up and down arrow keys, in the form DECCKM selects, "
            "instead of scrolling. A tracking mode takes precedence."),
        run=_demo_alternate_scroll,
        sequence="\x1b[?1007h\x1b[?1007$p",
        expect=r"\x1b\[\?1007;1\$y",
        snapshot=False,
    ),
    Entry(
        title="Hilite tracking (?1001)",
        explanation=(
            "CSI ? 1001 h reports presses like normal tracking and then waits "
            "about 400 ms for the host's XTHIMOUSE reply naming a region to "
            "highlight; without a reply the press stands as a plain report."),
        run=_demo_hilite,
        sequence="\x1b[?1001h\x1b[?1001$p",
        expect=r"\x1b\[\?1001;1\$y",
        snapshot=False,
    ),
    Entry(
        title="Pointer shape (OSC 22)",
        explanation=(
            "OSC 22 ; <name> ST sets the pointer over the window to a CSS cursor "
            "name and an empty name restores the default. OSC 22 ; ? <names> ST "
            "answers 1 or 0 per name, and the shape in force for __current__."),
        run=_demo_pointer_shape,
        sequence=f"{OSC}22;pointer{ST}{OSC}22;?pointer{ST}",
        expect=r"\x1b\]22;[a-z0-9,_-]*(\x1b\\|\x07)",
        snapshot=False,
    ),
    Entry(
        title="DEC locator (DECELR, DECRQLP)",
        explanation=(
            "CSI 1 ; 2 ' z arms the locator in cells and CSI 1 ' | polls it. The "
            "reply is CSI Pe ; Pb ; Pr ; Pc ; Pp & w with the pointer's row and "
            "column, or CSI 0 & w before the pointer has been seen."),
        run=_demo_dec_locator,
        sequence="\x1b[1;2'z\x1b['|",
        expect=r"\x1b\[[0-9;]*&w",
        snapshot=False,
    ),
]
