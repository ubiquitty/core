# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""Control strings: DCS, APC, PM, SOS and OSC framing.

A control string runs from its introducer to ST (OSC also closes on BEL). A
terminal that does not act on a payload still consumes the whole string and
prints nothing, so each entry writes its string between brackets: empty
brackets are the pass.
"""

from .. import tui as T
from ..tui import BEL, CSI, DCS, ESC, OSC, RESET, ST, fg_truecolor, move
from . import Entry

TITLE = "Control strings"
ORDER = 230

_EFFECT_COLUMN = 44


def _bracketed(tui, row, label, payload, before="[", after="]"):
    """One table row: the sequence label, then the payload written in brackets."""
    tui.label(row, 4, label)
    tui.write(move(row, _EFFECT_COLUMN), fg_truecolor(*tui.theme.accent2),
              before, payload, after, RESET)


# DECRQSS for the current SGR: DCS $ q m ST.
_DECRQSS_SGR = DCS + "$qm" + ST


def _demo_dcs(tui):
    row = tui.page(TITLE, "A DECRQSS request rides in a DCS string; the reply is another DCS.")
    tui.label(row, 4, "DCS $ q m ST")
    tui.result(row, _EFFECT_COLUMN, T.decode_reply(tui.query(_DECRQSS_SGR)))
    return tui.footer()


# An APC string whose payload starts with no recognized key.
_APC_HARMLESS = ESC + "_apc-payload-no-key" + ST


def _demo_apc(tui):
    row = tui.page(TITLE, "An APC string with an unknown key is consumed; the brackets stay empty.")
    _bracketed(tui, row, "ESC _ apc-payload-no-key ESC \\", _APC_HARMLESS)
    return tui.footer()


_PM_HARMLESS = ESC + "^pm-payload" + ST
_SOS_7BIT = ESC + "Xsos-7bit-payload" + ST
# The C1 introducers open a string only on a stream read as 8-bit; in UTF-8
# they decode as the characters U+0098 and U+009E and print.
_SOS_8BIT = "\x98" + "sos-c1-payload" + ST
_PM_8BIT = "\x9e" + "pm-c1-payload" + ST
_SOSPM_SEQ = _PM_HARMLESS + _SOS_7BIT + "VISIBLE" + _SOS_8BIT + _PM_8BIT


def _demo_pm_sos(tui):
    row = tui.page(TITLE, "PM and SOS payloads vanish; text after ST prints; C1 code points in UTF-8 are ignored.")
    _bracketed(tui, row, "ESC ^ pm-payload ESC \\", _PM_HARMLESS)
    _bracketed(tui, row + 1, "ESC X sos-7bit-payload ESC \\ VISIBLE", _SOS_7BIT + "VISIBLE")
    _bracketed(tui, row + 2, "U+0098 sos-c1-payload ESC \\ (UTF-8)", _SOS_8BIT)
    return tui.footer()


# OSC 0 closed by BEL and by ST, inside a title push and pop.
_TITLE_PUSH = CSI + "22;0t"
_TITLE_POP = CSI + "23;0t"
_OSC_BEL = OSC + "0;title-via-bel" + BEL
_OSC_ST = OSC + "0;title-via-st" + ST
_TERMINATOR_SEQ = _TITLE_PUSH + _OSC_BEL + _OSC_ST + _TITLE_POP


def _demo_terminator(tui):
    row = tui.page(TITLE, "OSC 0 closes on BEL and on ST alike; each sets the title it carries.")
    tui.write(_TITLE_PUSH)
    _bracketed(tui, row, "OSC 0 ; title-via-bel BEL", _OSC_BEL)
    tui.result(row + 1, _EFFECT_COLUMN,
               "CSI 21 t  " + (T.decode_reply(tui.query(CSI + "21t"))))
    _bracketed(tui, row + 3, "OSC 0 ; title-via-st ESC \\", _OSC_ST)
    tui.result(row + 4, _EFFECT_COLUMN,
               "CSI 21 t  " + (T.decode_reply(tui.query(CSI + "21t"))))
    tui.label(row + 6, 4, "CSI 22 ; 0 t before, CSI 23 ; 0 t on the way out")
    key = tui.footer()
    tui.write(_TITLE_POP)
    tui.flush()
    return key


# A 64-parameter CSI with the undefined final '{', then a private CSI with a
# 64-digit parameter and the undefined final '$z'.
_LONG_PARAMS = ";".join(str(n % 100) for n in range(64))
_UNKNOWN_CSI = CSI + _LONG_PARAMS + "{"
_OVERLONG_CSI = CSI + "?" + "9" * 64 + "$z"
_ROBUST_SEQ = _UNKNOWN_CSI + _OVERLONG_CSI


def _demo_robust(tui):
    row = tui.page(TITLE, "Both sequences are discarded, so the row reads exactly 'before[]after'.")
    tui.label(row, 4, "CSI 0;1;...;63 {   64 parameters")
    tui.label(row + 1, 4, "CSI ? 999...9 $ z  64-digit parameter")
    tui.write(move(row, _EFFECT_COLUMN), fg_truecolor(*tui.theme.accent2),
              "before[", _ROBUST_SEQ, "]after", RESET)
    return tui.footer()


# tmux passthrough: DCS tmux ; <body with every ESC doubled> ST. Outside tmux
# the whole thing is one DCS with an unknown prefix and is discarded.
_INNER = DCS + "demo-passthrough-payload" + ST
_PASSTHROUGH = DCS + "tmux;" + _INNER.replace(ESC, ESC + ESC) + ST


def _demo_passthrough(tui):
    row = tui.page(TITLE, "Outside tmux the passthrough is one unknown DCS; the brackets stay empty.")
    _bracketed(tui, row, "DCS tmux ; ESC ESC P ... ESC ESC \\ ST", _PASSTHROUGH)
    return tui.footer()


ENTRIES = [
    Entry(
        title="Device control string (DCS)",
        explanation=(
            "DCS (ESC P ... ST) carries sixel, DECDLD and DECRQSS. DCS $ q m ST "
            "asks for the current SGR without changing it and is answered "
            "DCS 1 $ r <parameters> m ST."),
        run=_demo_dcs,
        sequence=_DECRQSS_SGR,
        expect=r"\x1bP1\$r[0-9;:]*m\x1b\\",
        snapshot=False,
    ),
    Entry(
        title="Application program command (APC)",
        explanation=(
            "APC (ESC _ ... ST) carries Kitty graphics. A payload with no "
            "recognized key is consumed from introducer to ST and prints "
            "nothing, so the brackets around it stay empty."),
        run=_demo_apc,
        sequence=_APC_HARMLESS,
        expect="(consumed, nothing printed)",
    ),
    Entry(
        title="Privacy message and start of string (PM, SOS)",
        explanation=(
            "PM (ESC ^) and SOS (ESC X) are consumed through ST and print "
            "nothing; text after ST prints normally. The C1 introducers 0x98 "
            "and 0x9E open a string only on an 8-bit stream; in UTF-8 they "
            "are code points with no graphic and are ignored, so the text "
            "around them runs on."),
        run=_demo_pm_sos,
        sequence=_SOSPM_SEQ,
        expect="(consumed, nothing printed; text after ST renders)",
        golden="control_strings_sospm",
    ),
    Entry(
        title="OSC terminators (BEL, ST)",
        explanation=(
            "An OSC string closes on BEL or on ST and both parse the same: "
            "each OSC 0 sets the title and prints nothing. The title is pushed "
            "before and popped after, so it reads the same on return."),
        run=_demo_terminator,
        sequence=_TERMINATOR_SEQ,
        expect="(consumed, nothing printed)",
    ),
    Entry(
        title="Unknown and over-long sequences",
        explanation=(
            "A CSI with 64 parameters and an undefined final, and a private "
            "CSI with a 64-digit parameter, are both discarded without "
            "wedging the parser: the row reads 'before[]after' and the next "
            "key reads as a key."),
        run=_demo_robust,
        sequence=_ROBUST_SEQ,
        expect="(consumed, nothing printed)",
    ),
    Entry(
        title="tmux passthrough (DCS tmux)",
        explanation=(
            "tmux forwards DCS tmux ; <body> ST to the outer terminal after "
            "halving the doubled ESCs in the body. Outside tmux the string is "
            "one DCS with an unknown prefix, consumed whole, so the brackets "
            "stay empty."),
        run=_demo_passthrough,
        sequence=_PASSTHROUGH,
        expect="(consumed, nothing printed)",
        golden="multiplexing_tmux_dcs_passthrough",
    ),
]
