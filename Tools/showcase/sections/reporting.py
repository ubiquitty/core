# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""Reporting: queries the terminal answers on stdin.

Each entry sends its queries and prints the replies beside them. `sequence`
holds one query's exact bytes and `expect` the reply's shape, so the live page
and the regression harness share one source.
"""

from .. import tui as T
from ..tui import CSI, DCS, ST
from . import Entry

TITLE = "Reporting"
ORDER = 210


def _reply(tui, query):
    return T.decode_reply(tui.query(query))


def _table(tui, row, lines):
    """Rows of (sequence label, meaning, query); the reply goes in column 46."""
    for label, meaning, query in lines:
        tui.label(row, 4, label)
        tui.text(row, 26, meaning, tui.fg_default())
        tui.result(row, 46, T.truncate(_reply(tui, query), tui.cols - 47))
        row += 1
    return row


_DA1 = CSI + "c"
_DA2 = CSI + ">c"
_DA3 = CSI + "=c"


def _demo_da(tui):
    row = tui.page(TITLE, "The terminal names its class, model and unit id.")
    _table(tui, row, [
        ("CSI c", "primary (DA1)", _DA1),
        ("CSI > c", "secondary (DA2)", _DA2),
        ("CSI = c", "tertiary (DA3)", _DA3),
    ])
    return tui.footer()


_DSR_CPR = CSI + "6n"
_DSR_STATUS = CSI + "5n"


def _demo_dsr(tui):
    row = tui.page(TITLE, "The cursor position and the operating status.")
    _table(tui, row, [
        ("CSI 6 n", "cursor position", _DSR_CPR),
        ("CSI 5 n", "status", _DSR_STATUS),
    ])
    return tui.footer()


_RQSS_SGR = DCS + "$qm" + ST
_RQSS_STBM = DCS + "$qr" + ST


def _demo_decrqss(tui):
    row = tui.page(TITLE, "Each setting comes back spelled as the bytes that would set it.")
    _table(tui, row, [
        ("DCS $ q m ST", "SGR", _RQSS_SGR),
        ("DCS $ q r ST", "scroll region", _RQSS_STBM),
    ])
    return tui.footer()


_RQM = CSI + "?25$p"


def _demo_decrqm(tui):
    row = tui.page(TITLE, "DECTCEM reads 2 while the cursor is hidden and 1 once CSI ? 25 h shows it.")
    _table(tui, row, [("CSI ? 25 $ p", "cursor hidden", _RQM)])
    tui.write(CSI + "?25h")
    _table(tui, row + 1, [("CSI ? 25 $ p", "after ? 25 h", _RQM)])
    tui.write(CSI + "?25l")
    return tui.footer()


# Checksum of the 1x1 home rectangle on page 1, request id 1.
_RQCRA = CSI + "1;1;1;1;1;1*y"


def _demo_decrqcra(tui):
    row = tui.page(TITLE, "The checksum of the home cell, tagged with request id 1.")
    _table(tui, row, [("CSI 1;1;1;1;1;1 * y", "home cell", _RQCRA)])
    return tui.footer()


def _tcap(name):
    return DCS + "+q" + name.encode("ascii").hex() + ST


_XTGETTCAP = _tcap("Co")


def _demo_xtgettcap(tui):
    row = tui.page(TITLE, "Each capability name and value comes back hex-encoded.")
    _table(tui, row, [
        ("DCS + q 436f ST", "Co (colors)", _XTGETTCAP),
        ("DCS + q 544e ST", "TN (name)", _tcap("TN")),
    ])
    return tui.footer()


_TEXT_CELLS = CSI + "18t"


def _demo_csi_t(tui):
    row = tui.page(TITLE, "The text area in rows and columns.")
    _table(tui, row, [("CSI 18 t", "text area", _TEXT_CELLS)])
    return tui.footer()


ENTRIES = [
    Entry(
        title="Device attributes (DA1, DA2, DA3)",
        explanation=(
            "CSI c answers CSI ? 65 ; <features> c, VT500 class first. "
            "CSI > c answers CSI > 64 ; <version> ; 0 c. CSI = c answers "
            "DCS ! | 4D414352 ST, the DECRPTUI unit id."),
        run=_demo_da,
        sequence=_DA1,
        expect=r"\x1b\[\?[0-9;]+c",
        snapshot=False,
        golden="reporting_device_attributes_da1_da2_da3",
    ),
    Entry(
        title="Device status report (DSR 5, DSR 6)",
        explanation=(
            "CSI 6 n answers CSI <row> ; <column> R for the cursor's cell, "
            "counted from 1. CSI 5 n answers CSI 0 n when the terminal is "
            "ready."),
        run=_demo_dsr,
        sequence=_DSR_CPR,
        expect=r"\x1b\[[0-9]+;[0-9]+R",
        snapshot=False,
    ),
    Entry(
        title="Current settings (DECRQSS)",
        explanation=(
            "DCS $ q <setting> ST answers DCS 1 $ r <value> ST with the value "
            "spelled as it would be set: 0 m for a default pen, "
            "<top> ; <bottom> r for the margins. An unknown setting answers "
            "DCS 0 $ r ST."),
        run=_demo_decrqss,
        sequence=_RQSS_SGR,
        expect=r"\x1bP1\$r[0-9;:]*m\x1b\\",
        snapshot=False,
    ),
    Entry(
        title="Mode state (DECRQM)",
        explanation=(
            "CSI ? 25 $ p answers CSI ? 25 ; <state> $ y: 1 set, 2 reset, 0 "
            "unknown, 3 and 4 permanently set and reset. The state follows "
            "CSI ? 25 h and l as they arrive."),
        run=_demo_decrqm,
        sequence=_RQM,
        expect=r"\x1b\[\?25;[0-4]\$y",
        snapshot=False,
    ),
    Entry(
        title="Rectangle checksum (DECRQCRA)",
        explanation=(
            "CSI <id> ; <page> ; <top> ; <left> ; <bottom> ; <right> * y "
            "answers DCS <id> ! ~ <4 hex digits> ST: the negated 16-bit sum "
            "of the cells' characters, so one blank cell answers FFE0."),
        run=_demo_decrqcra,
        sequence=_RQCRA,
        expect=r"\x1bP[0-9]*!~[0-9A-Fa-f]+\x1b\\",
        snapshot=False,
    ),
    Entry(
        title="Terminfo capabilities (XTGETTCAP)",
        explanation=(
            "DCS + q <hex name> ST answers DCS 1 + r <hex name> = <hex value> "
            "ST for a known capability, so Co answers 436f=323536 (256), and "
            "DCS 0 + r ST for an unknown one."),
        run=_demo_xtgettcap,
        sequence=_XTGETTCAP,
        expect=r"\x1bP1\+r[0-9A-Fa-f]+=[0-9A-Fa-f]+\x1b\\",
        snapshot=False,
    ),
    Entry(
        title="Text area in cells (CSI 18 t)",
        explanation=(
            "CSI 18 t answers CSI 8 ; <rows> ; <columns> t, the grid a host "
            "program writes into and the size TIOCGWINSZ reports."),
        run=_demo_csi_t,
        sequence=_TEXT_CELLS,
        expect=r"\x1b\[8;[0-9]+;[0-9]+t",
        snapshot=False,
    ),
]
