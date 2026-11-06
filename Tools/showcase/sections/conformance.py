# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""Conformance levels: DECSCL, the level report, and C1 transmission.

DECSCL (CSI Pl ; Pc " p) selects VT100 (61) through VT500 (65) and performs a
soft reset; Pc 1 selects 7-bit C1 replies, 0 or 2 8-bit. Each entry returns
the terminal to VT500 with 7-bit replies, the startup state, and re-hides the
cursor the soft reset showed.
"""

from .. import tui as T
from ..tui import CSI, DCS, ESC, ST
from . import Entry

TITLE = "Conformance levels"
ORDER = 240

_DECSCL_VT100 = CSI + '61"p'
_DECRQSS_DECSCL = DCS + '$q"p' + ST
_DA1 = CSI + "c"
_DA2 = CSI + ">c"
_S7C1T = ESC + " F"
_S8C1T = ESC + " G"


def _restore(tui):
    tui.write(CSI + '65;1"p', _S7C1T, T.HIDE_CURSOR)


def _decfra(row, column):
    """DECFRA: fill one row, eight columns wide, with '#'."""
    return f"{CSI}35;{row};{column};{row};{column + 7}$x"


def _demo_set_level(tui):
    row = tui.page(TITLE, "Each level from VT200 reads back through DECRQSS; DECFRA fills only from VT400.")
    tui.label(row, 4, "DECSCL")
    tui.label(row, 22, "DECRQSS reply")
    tui.label(row, 52, "DECFRA")
    for offset, level in enumerate(range(61, 66)):
        line = row + 1 + offset
        tui.label(line, 4, f'CSI {level} ; 1 " p')
        tui.write(CSI + f'{level};1"p')
        reply = T.decode_reply(tui.query(_DECRQSS_DECSCL))
        tui.write(_decfra(line, 52))
        tui.result(line, 22, T.truncate(reply, 28))
    _restore(tui)
    return tui.footer()


def _demo_query_level(tui):
    row = tui.page(TITLE, "The level as DECRQSS, DA1 and DA2 report it.")
    for offset, (label, query) in enumerate([
            ('DCS $ q " p ST', _DECRQSS_DECSCL),
            ("CSI c", _DA1),
            ("CSI > c", _DA2)]):
        tui.label(row + offset, 4, label)
        tui.result(row + offset, 22, T.truncate(T.decode_reply(tui.query(query)), tui.cols - 23))
    _restore(tui)
    return tui.footer()


def _demo_c1(tui):
    row = tui.page(TITLE, "The same DA1 reply opens with 0x9B under S8C1T and ESC [ under S7C1T.")
    tui.write(_S8C1T)
    eight = T.decode_reply(tui.query(_DA1))
    tui.write(_S7C1T)
    seven = T.decode_reply(tui.query(_DA1))
    tui.label(row, 4, "ESC SP G   CSI c")
    tui.result(row, 22, T.truncate(eight, tui.cols - 23))
    tui.label(row + 1, 4, "ESC SP F   CSI c")
    tui.result(row + 1, 22, T.truncate(seven, tui.cols - 23))
    return tui.footer()


ENTRIES = [
    Entry(
        title="Conformance level (DECSCL)",
        explanation=(
            'CSI 61 " p through CSI 65 " p select VT100 to VT500, and DECRQSS '
            "reads back every level but VT100: DECRQSS is a Level 2 function, "
            "and VT100 mode answers no Level 2 query. The rectangular-area "
            "operations are inert below VT400, so DECFRA fills only the VT400 "
            "and VT500 rows."),
        run=_demo_set_level,
        # At VT100 level the terminal answers no DECRQSS (DEC STD 070: a Level 2
        # function works only in Level 2 operation), so the probe expects silence.
        sequence=_DECSCL_VT100 + _DECRQSS_DECSCL,
        expect="(consumed, nothing printed: VT100 mode answers no DECRQSS)",
    ),
    Entry(
        title="Level report (DECRQSS, DA1, DA2)",
        explanation=(
            'DCS $ q " p ST answers DCS 1 $ r <level> ; <Pc> " p ST. DA1 leads '
            "with the level as its device class (65 for VT500) and DA2 names "
            "the model; neither carries Pc."),
        run=_demo_query_level,
        sequence=_DA1,
        expect=r"\x1b\[\?[0-9;]+c",
        snapshot=False,
    ),
    Entry(
        title="C1 transmission (S7C1T, S8C1T)",
        explanation=(
            "ESC SP G makes the terminal send C1 controls as single 8-bit "
            "bytes, so a reply opens with 0x9B; ESC SP F returns to the 7-bit "
            "ESC [ form."),
        run=_demo_c1,
        sequence=_S7C1T + CSI + "c",
        expect=r"\x1b\[\?[0-9;]+c",
        snapshot=False,
    ),
]
