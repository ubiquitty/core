# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""Keyboard: the bytes a key sends, and the modes that change them.

The inspectors read the tty directly instead of through `tui.read_key()`,
which folds bytes into key names, and show each press caret-escaped. They must
see Esc as a key like any other, so leaving one takes Esc twice in a row.
Every entry that sets an input mode or programs a key resets it on exit.
"""

import os
import re
import select

from .. import tui as T
from ..tui import BOLD, CSI, DCS, ESC, ST, fg_truecolor, move
from . import Entry

TITLE = "Keyboard"
ORDER = 150

_EXIT = ("Esc Esc", "back")

# Under the Kitty protocol Esc arrives as CSI 27 u, with optional modifier and
# event fields.
_KITTY_ESC = re.compile(rb"^\x1b\[27(?:[;:][\d;:]*)?u$")


def _read_raw_keypress(tui, gap=0.04):
    """One keypress as raw bytes, gathering a whole escape burst."""
    first = os.read(tui._fd, 1)
    if not first:
        return b""
    chunks = [first]
    while True:
        ready, _, _ = select.select([tui._fd], [], [], gap)
        if not ready:
            break
        more = os.read(tui._fd, 4096)
        if not more:
            break
        chunks.append(more)
    return b"".join(chunks)


def _is_esc(data):
    return data == b"\x1b" or bool(_KITTY_ESC.match(data))


def _inspect(tui, top, actions=(), on_key=None):
    """Show each keypress's bytes from row `top` down until Esc comes twice.

    `actions` are extra key-bar entries; `on_key(data)` returns True when it
    consumed a press as one of them.
    """
    depth = tui.rows - 1 - top
    history = []
    previous = None
    while True:
        tui.status_bar(list(actions) + [_EXIT])
        tui.flush()
        data = _read_raw_keypress(tui)
        if not data:
            return "esc"
        if _is_esc(data) and previous is not None and _is_esc(previous):
            return "esc"
        previous = data
        if on_key and on_key(data):
            continue
        history = (history + [data])[-depth:]
        for i in range(depth):
            tui.write(move(top + i, 1), T.CLEAR_LINE)
        for i, raw in enumerate(history):
            tui.label(top + i, 4, f"{len(raw):>2} B")
            tui.result(top + i, 10, T.truncate(T.decode_reply(raw), tui.cols - 12))


def _demo_inspector(tui):
    row = tui.page(TITLE, "Each key you press appears below as the bytes it sends.")
    tui.label(row, 4, "Up  ESC [ A    F1  ESC O P    Ctrl-A  ^A    Alt-x  ESC x")
    return _inspect(tui, row + 2)


def _demo_modes(tui):
    row = tui.page(TITLE, "Arrows send ESC [ A, or ESC O A after c; k switches the keypad to SS3.")
    t = tui.theme
    state = {"cursor": False, "keypad": False}

    def draw():
        tui.write(f"{CSI}?1h" if state["cursor"] else f"{CSI}?1l",
                  f"{ESC}=" if state["keypad"] else f"{ESC}>")
        cursor = "CSI ?1 h   application, SS3" if state["cursor"] else "CSI ?1 l   normal, CSI       "
        keypad = "ESC =      application, SS3" if state["keypad"] else "ESC >      numeric, digits   "
        tui.text(row, 4, "cursor keys", tui.fg_default())
        tui.text(row, 18, cursor, fg_truecolor(*t.accent), BOLD)
        tui.text(row + 1, 4, "keypad", tui.fg_default())
        tui.text(row + 1, 18, keypad, fg_truecolor(*t.accent), BOLD)

    def on_key(data):
        if data in (b"c", b"k"):
            state["cursor" if data == b"c" else "keypad"] ^= True
            draw()
            return True
        return False

    draw()
    try:
        return _inspect(tui, row + 3, (("c", "toggle DECCKM"), ("k", "toggle keypad")), on_key)
    finally:
        tui.write(f"{CSI}?1l", f"{ESC}>")
        tui.flush()


def _demo_modify_other(tui):
    row = tui.page(TITLE, "Ctrl-I and Shift-Enter arrive as CSI 27 ; mod ; code ~, not Tab and Enter.")
    tui.label(row, 4, "CSI > 4 ; 2 m   modifyOtherKeys level 2, until CSI > 4 ; 0 m on exit")
    tui.write(f"{CSI}>4;2m")
    try:
        return _inspect(tui, row + 2)
    finally:
        tui.write(f"{CSI}>4;0m")
        tui.flush()


def _demo_kitty(tui):
    row = tui.page(TITLE, "Every key, its release, and its modifiers arrive as CSI code ; mods u.")
    tui.label(row, 4, "CSI > 31 u   push all five flags, until CSI < u pops them on exit")
    tui.write(f"{CSI}>31u")
    try:
        return _inspect(tui, row + 2)
    finally:
        tui.write(f"{CSI}<u")
        tui.flush()


# F1 is DEC function-key code 11. Pc 1 leaves other definitions alone and Pl 1
# leaves the keys unlocked, so the empty definition on exit can clear it.
_UDK_TEXT = "F1 was programmed"
_UDK_DEFINE = f"{DCS}1;1|11/{_UDK_TEXT.encode().hex().upper()}{ST}"
_UDK_CLEAR = f"{DCS}1;1|11/{ST}"


def _demo_udk(tui):
    row = tui.page(TITLE, f"F1 sends \"{_UDK_TEXT}\" instead of ESC O P until you leave.")
    tui.label(row, 4, f"DCS 1 ; 1 | 11 / {_UDK_TEXT.encode().hex().upper()} ST   define F1")
    tui.label(row + 1, 4, "DCS 1 ; 1 | 11 / ST   clear it, on exit")
    tui.write(_UDK_DEFINE)
    try:
        return _inspect(tui, row + 3)
    finally:
        tui.write(_UDK_CLEAR)
        tui.flush()


_KEYBOARD_MODES = [
    (1034, "eight-bit input"),
    (1036, "meta sends escape"),
    (1039, "alt sends escape"),
    (1037, "delete sends DEL"),
    (1050, "termcap function keys"),
    (1051, "Sun function keys"),
    (1052, "HP function keys"),
    (1053, "SCO function keys"),
    (1060, "legacy keyboard"),
    (1061, "VT220 keyboard"),
]

_DECRPM_STATE = {"0": "not recognized", "1": "set", "2": "reset",
                 "3": "permanently set", "4": "permanently reset"}


def _demo_keyboard_modes(tui):
    row = tui.page(TITLE, "Each keyboard mode's DECRQM reply, with the state it names.")
    replies = [(ps, name, tui.query(f"{CSI}?{ps}$p")) for ps, name in _KEYBOARD_MODES]
    if not any(reply for _, _, reply in replies):
        tui.unavailable(row, "No reply to DECRQM, so there is no state to show.")
        return tui.footer()
    for ps, name, reply in replies:
        match = re.search(rb"\x1b\[\?\d+;(\d)\$y", reply)
        state = _DECRPM_STATE.get(match.group(1).decode(), "") if match else ""
        tui.label(row, 4, f"CSI ?{ps} $ p")
        tui.text(row, 20, name, tui.fg_default())
        tui.result(row, 44, T.truncate(f"{T.decode_reply(reply)}  {state}", tui.cols - 45))
        row += 1
    return tui.footer()


ENTRIES = [
    Entry(
        title="Key inspector (raw bytes)",
        explanation=(
            "Each keypress shows the bytes sent to the host: arrows ESC [ A-D, "
            "F1-F4 ESC O P-S, F5 and up CSI n ~, Ctrl-letter one control byte, "
            "and an Alt-held key ESC then the key while ?1036 is set."),
        run=_demo_inspector,
    ),
    Entry(
        title="Cursor and keypad modes (DECCKM, DECKPAM)",
        explanation=(
            "CSI ?1 h makes the arrows send ESC O A-D instead of ESC [ A-D; ESC = "
            "makes the numeric keypad send SS3 sequences instead of digits. Both "
            "return to normal and numeric on exit."),
        run=_demo_modes,
        sequence="\x1b[?1h\x1b=\x1b[?1$p\x1b[?66$p",
        expect=r"\x1b\[\?1;1\$y\x1b\[\?66;1\$y",
        snapshot=False,
    ),
    Entry(
        title="modifyOtherKeys (CSI > 4 ; 2 m)",
        explanation=(
            "At level 2 a modified key that would collapse onto a control byte "
            "reports CSI 27 ; <modifiers> ; <code> ~, so Ctrl-I differs from Tab "
            "and Shift-Enter from Enter. CSI > 4 ; 0 m turns it off on exit."),
        run=_demo_modify_other,
        sequence="\x1b[>4;2m\x1b[?4m",
        expect=r"\x1b\[>4;2m",
        snapshot=False,
    ),
    Entry(
        title="Kitty keyboard protocol (CSI > flags u)",
        explanation=(
            "CSI > 31 u pushes all five enhancement flags; keys then arrive as "
            "CSI <code> ; <modifiers> u with press, repeat, and release events. "
            "CSI < u pops the flags on exit."),
        run=_demo_kitty,
        sequence="\x1b[>31u\x1b[?u",
        expect=r"\x1b\[\?31u",
        snapshot=False,
    ),
    Entry(
        title="User-defined keys (DECUDK)",
        explanation=(
            "DCS 1 ; 1 | 11 / <hex> ST programs F1 to send the decoded string in "
            "place of ESC O P; an empty definition on exit restores the default."),
        run=_demo_udk,
    ),
    Entry(
        title="Keyboard modes (DECRQM)",
        explanation=(
            "CSI ? Ps $ p answers CSI ? Ps ; Pm $ y for each keyboard mode: 1034, "
            "1036, 1039 and 1037 report set or reset from live state, with 1036 "
            "set at power-on; the function-key tables 1050-1061 report 4, "
            "permanently reset."),
        run=_demo_keyboard_modes,
        sequence="\x1b[?1034$p",
        expect=r"\x1b\[\?1034;[0-4]\$y",
        snapshot=False,
    ),
]
