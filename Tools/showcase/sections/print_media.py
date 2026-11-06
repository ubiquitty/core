# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""Print: the printer controller and the print-control modes.

Prints go to a print sink file the application installs, never to the screen,
so the entries show what a person can see: the printer-controller payload
landing on the page, and DECRQM following DECPFF and DECPEX as they flip.
Every entry stops the controller and turns DECPFF off before it returns.
"""

from .. import tui as T
from ..tui import CSI, RESET, move, fg_truecolor
from . import Entry

TITLE = "Print"
ORDER = 270

PRINTER_CTRL_START = CSI + "5i"
PRINTER_CTRL_STOP = CSI + "4i"
_PAYLOAD = "printer-controller payload"


def _disarm_all(tui):
    """Stop the controller first, so the rest of these bytes reach the terminal."""
    tui.write(PRINTER_CTRL_STOP, CSI + "?4i", CSI + "?18l", CSI + "?19h")
    tui.flush()


def _demo_printer_controller(tui):
    row = tui.page(TITLE, "The payload between CSI 5 i and CSI 4 i goes to the printer: the page reads [].")
    tui.label(row, 4, f"[  CSI 5 i  {_PAYLOAD}  CSI 4 i  ]")
    tui.write(move(row + 2, 4), fg_truecolor(*tui.theme.accent2),
              "[", PRINTER_CTRL_START, _PAYLOAD, PRINTER_CTRL_STOP, "]", RESET)
    key = tui.footer()
    _disarm_all(tui)
    return key


def _demo_pff_pex(tui):
    row = tui.page(TITLE, "DECRQM follows DECPFF and DECPEX as each one is set and reset.")
    steps = [
        ("", 18), (CSI + "?18h", 18), (CSI + "?18l", 18),
        ("", 19), (CSI + "?19l", 19), (CSI + "?19h", 19),
    ]
    for offset, (change, mode) in enumerate(steps):
        line = row + offset + (1 if mode == 19 else 0)
        tui.write(change)
        tui.label(line, 4, f"CSI ? {mode} {change[-1]}" if change else "initial")
        tui.label(line, 20, f"CSI ? {mode} $ p")
        tui.result(line, 36, T.decode_reply(tui.query(CSI + f"?{mode}$p")))
    key = tui.footer()
    _disarm_all(tui)
    return key


ENTRIES = [
    Entry(
        title="Printer controller (CSI 5 i, CSI 4 i)",
        explanation=(
            "CSI 5 i starts the printer controller and CSI 4 i stops it. "
            "The bytes between the two go to the printer sink and never reach "
            "the screen, as the VT510 defines MC, so the brackets written "
            "around them close on nothing: the page reads []."),
        run=_demo_printer_controller,
        sequence="[\x1b[5i" + _PAYLOAD + "\x1b[4i]",
        golden="print_media_controller",
    ),
    Entry(
        title="Print form feed and extent (DECPFF, DECPEX)",
        explanation=(
            "DECPFF (?18) adds a form feed after each print and DECPEX (?19) "
            "picks the whole screen (set) or the scroll region (reset) for "
            "print screen. DECRQM answers 1 while each is set and 2 while "
            "reset."),
        run=_demo_pff_pex,
        sequence="\x1b[?18h\x1b[?18l\x1b[?19h\x1b[?18$p\x1b[?19$p",
        expect=r"\x1b\[\?18;2\$y\x1b\[\?19;1\$y",
        snapshot=False,
        golden="print_media_pff_pex",
    ),
]
