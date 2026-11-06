# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""Rich text blocks (OSC 7404, Docs/rich-text.md).

A block is a rectangle of cells a program fills with styled, wrapped text in
one escape: the body is a subset of the Rich Text Format, and the escape's
keys carry the rectangle, the overflow policy and the alignment. Before it
places anything a program can ask what fits, the way a layout system asks a
view for the size it wants.

Each entry's snapshot is its exact sequence. The cards are drawn with the
grid's own box drawing around a block, so the frame shows where the block's
rectangle is whether or not the terminal draws the block itself.
"""

import re

from .. import tui as T
from . import Entry

TITLE = "Rich text blocks"
ORDER = 60


def _osc(keys, body=""):
    return "\x1b]7404;" + keys + (";" + body if body else "") + "\x1b\\"


def _frame(row, col, height, width, colour="38;2;150;150;160"):
    """A rounded frame whose interior is `height` rows of `width` cells."""
    lines = [T.move(row, col) + "\x1b[" + colour + "m╭" + "─" * width + "╮"]
    for offset in range(1, height + 1):
        lines.append(T.move(row + offset, col) + "│" + T.move(row + offset, col + width + 1) + "│")
    lines.append(T.move(row + height + 1, col) + "╰" + "─" * width + "╯\x1b[0m")
    return "".join(lines)


_CLEAR = "\x1b[0m\x1b[?25l\x1b[2J\x1b[H"

# Each card is sized the way a program sizes one: it asks for the rows its
# body and padding need at the card's width, then places the block and frames
# it at that answer, centred in the rows with y=c so what the whole rows leave
# over is shared above and below. A recorded sequence cannot ask, so its
# numbers are the larger of the replies the Apple and Linux arms give in the
# configured faces: the Linux arm's line box is taller, and y=c centres the
# Apple arms' shorter content in the rows that leaves over. The interactive
# entries ask the terminal they run in.

_RELEASE_NOTES_BODY = (
    r"{\rtf1{\fonttbl{\f0\fswiss;}{\f1\fmodern;}}"
    r"{\colortbl;\red0\green102\blue204;\red110\green110\blue120;\red46\green140\blue67;}"
    r"\f0\fs40\b\cf1 Release notes\b0\fs24\cf2   this week\par"
    r"\pard\sb140\fs26\b\cf3 New\b0\cf0\par"
    r"\pard\li360\fi-360\fs24\bullet\tab Rich text blocks: headings, paragraphs, colour and wrapping in"
    r" one escape, laid out by the terminal in the faces it draws its text in.\par"
    r"\bullet\tab Ask what fits with \f1 a=q\f0  before placing a block.\par"
    r"\pard\sb140\fs26\b\cf3 Fixed\b0\cf0\par"
    r"\pard\li360\fi-360\fs24\bullet\tab A selection inside a block copies per character, not per cell.}"
)
# Twelve points at the sides, less above the heading than below the last
# line, whose descenders need the room.
_RELEASE_NOTES_PADDING = "p=6,12,8,12"
_RELEASE_NOTES_SIZE = (70, 11)

_RELEASE_NOTES_SEQ = (
    _CLEAR
    + _osc("a=q:i=1:w=%d:h=0:%s" % (_RELEASE_NOTES_SIZE[0], _RELEASE_NOTES_PADDING), _RELEASE_NOTES_BODY)
    + _frame(2, 3, _RELEASE_NOTES_SIZE[1], _RELEASE_NOTES_SIZE[0])
    + T.move(3, 4)
    + _osc("w=%d:h=%d:%s:y=c:C=1" % (_RELEASE_NOTES_SIZE + (_RELEASE_NOTES_PADDING,)), _RELEASE_NOTES_BODY)
    + T.move(_RELEASE_NOTES_SIZE[1] + 4, 4)
    + "\x1b[38;2;110;110;120mOne OSC 7404 placement; the frame is ordinary box drawing.\x1b[0m"
)

# The rich-text profile carries status marks directly as UTF-8.
_CHECK = "✓"
_CROSS = "✗"
_DOT = "●"

_AGENT_STATUS_BODY = (
    r"{\rtf1{\fonttbl{\f0\fswiss;}{\f1\fmodern;}}"
    r"{\colortbl;\red46\green160\blue67;\red207\green34\blue46;\red191\green135\blue0;\red110\green110\blue120;}"
    r"\f0\fs32\b Agent status\b0\par"
    r"\fs22\cf4 Three tasks, one still running\cf0\par"
    r"\pard\sb80\f1\fs24\cf1" + _CHECK + r"\cf0  build    \cf1 passed in 2m 14s\cf0\line"
    r"\cf2" + _CROSS + r"\cf0  tests    \cf2 2 failing: selection, reflow\cf0\line"
    r"\cf3" + _DOT + r"\cf0  deploy   \cf3 waiting for review\cf0}"
)
_AGENT_STATUS_PADDING = "p=4,10,4,10"
_AGENT_STATUS_SIZE = (51, 7)

_AGENT_STATUS_SEQ = (
    _CLEAR
    + _osc("a=q:i=3:w=%d:h=0:%s" % (_AGENT_STATUS_SIZE[0], _AGENT_STATUS_PADDING), _AGENT_STATUS_BODY)
    + _frame(2, 3, _AGENT_STATUS_SIZE[1], _AGENT_STATUS_SIZE[0])
    + T.move(3, 4)
    + _osc("i=3:w=%d:h=%d:%s:y=c:C=1" % (_AGENT_STATUS_SIZE + (_AGENT_STATUS_PADDING,)), _AGENT_STATUS_BODY)
    + T.move(_AGENT_STATUS_SIZE[1] + 4, 4)
    + "\x1b[38;2;110;110;120mPlacing i=3 again updates the panel in place.\x1b[0m"
)

# The body the fit-query entry sizes, with its padding. Its numbers below are
# the larger of the Apple and Linux arms' replies in the configured faces: the
# ideal size, and the rows the body needs wrapped at 24 cells, padding included
# in both.
_FIT_BODY = (
    r"{\rtf1{\colortbl;\red0\green102\blue204;}"
    r"\fs28\b\cf1 Sized by asking\b0\cf0\par"
    r"\fs22 The frame hugs the block because the program asked for the ideal size first.}"
)
_FIT_PADDING = "p=4,8,6,8"
_FIT_IDEAL = (70, 4)
_FIT_NARROW = (24, 7)

_FIT_QUERY_SEQ = (
    _CLEAR
    + _osc("a=q:i=7:w=0:h=0:" + _FIT_PADDING, _FIT_BODY)
    + _osc("a=q:i=8:w=%d:h=0:%s" % (_FIT_NARROW[0], _FIT_PADDING), _FIT_BODY)
    + _frame(2, 3, _FIT_IDEAL[1], _FIT_IDEAL[0])
    + T.move(3, 4)
    + _osc("w=%d:h=%d:%s:y=c:C=1" % (_FIT_IDEAL + (_FIT_PADDING,)), _FIT_BODY)
    + _frame(_FIT_IDEAL[1] + 4, 3, _FIT_NARROW[1], _FIT_NARROW[0])
    + T.move(_FIT_IDEAL[1] + 5, 4)
    + _osc("w=%d:h=%d:%s:y=c:C=1" % (_FIT_NARROW + (_FIT_PADDING,)), _FIT_BODY)
)

_REPLY = re.compile(rb"\x1b\]7404;a=r:i=(\d+):f=(\d):w=(\d+):h=(\d+):n=(\d+):c=(\d+):l=(\d+):m=(\d+):x=(\d)")


def _ask(tui, keys, body=_FIT_BODY):
    """The reply to a query, as (width, height, minimum), or None."""
    match = _REPLY.search(tui.query(_osc(keys, body), timeout=0.5))
    if not match:
        return None
    return int(match.group(3)), int(match.group(4)), int(match.group(8))


def _show_card(tui, caption, body, width, padding, keys, fallback_height):
    """Asks for the rows a card's body and padding need at its width, then
    frames and places it at that answer."""
    row = tui.page(TITLE, caption)
    answer = _ask(tui, "a=q:w=%d:h=0:%s" % (width, padding), body)
    height = answer[1] if answer else fallback_height
    tui.write(_frame(row, 3, height, width) + T.move(row + 1, 4)
              + _osc("%sw=%d:h=%d:%s:y=c:C=1" % (keys, width, height, padding), body))
    return tui.footer()


def _demo_release_notes(tui):
    return _show_card(tui, "A heading, two sections and wrapped bullets, placed by one escape.",
                      _RELEASE_NOTES_BODY, _RELEASE_NOTES_SIZE[0], _RELEASE_NOTES_PADDING, "",
                      _RELEASE_NOTES_SIZE[1])


def _demo_agent_status(tui):
    return _show_card(tui, "Three colored status lines in the monospace face, names aligned.",
                      _AGENT_STATUS_BODY, _AGENT_STATUS_SIZE[0], _AGENT_STATUS_PADDING, "i=3:",
                      _AGENT_STATUS_SIZE[1])


def _demo_fit_query(tui):
    """Asks for the ideal size and a height at a width, then places both."""
    row = tui.page(TITLE, "Each frame hugs its block because the terminal was asked what fits first.")
    ideal = _ask(tui, "a=q:i=7:w=0:h=0:" + _FIT_PADDING)
    narrow = _ask(tui, "a=q:i=8:w=%d:h=0:%s" % (_FIT_NARROW[0], _FIT_PADDING))
    if not ideal or not narrow:
        tui.unavailable(row, "No reply to OSC 7404 a=q, so there is no size to place by.")
        return tui.footer()
    width, height, minimum = ideal
    narrow_width = max(_FIT_NARROW[0], minimum)
    tui.write(_frame(row, 3, height, width) + T.move(row + 1, 4)
              + _osc("w=%d:h=%d:%s:y=c:C=1" % (width, height, _FIT_PADDING), _FIT_BODY))
    top = row + height + 2
    tui.write(_frame(top, 3, narrow[1], narrow_width) + T.move(top + 1, 4)
              + _osc("w=%d:h=%d:%s:y=c:C=1" % (narrow_width, narrow[1], _FIT_PADDING), _FIT_BODY))
    tui.label(top, narrow_width + 8, "a=q:w=0:h=0")
    tui.result(top + 1, narrow_width + 8, "ideal %d x %d, minimum width %d" % (width, height, minimum))
    tui.label(top + 3, narrow_width + 8, "a=q:w=%d:h=0" % narrow_width)
    tui.result(top + 4, narrow_width + 8, "%d rows" % narrow[1])
    return tui.footer()


ENTRIES = [
    Entry(
        title="Release notes card",
        explanation=(
            "Inside the frame, a blue bold 'Release notes' with a grey 'this "
            "week' on its baseline, then green section heads over bulleted "
            "lines that wrap at the frame's inner edge; 'a=q' is set in the "
            "monospace face."),
        run=_demo_release_notes,
        sequence=_RELEASE_NOTES_SEQ,
        golden="rich_text_release_notes",
    ),
    Entry(
        title="Agent status panel",
        explanation=(
            "A bold heading, a grey summary, and three monospace status lines "
            "whose names line up: a green check, a red cross and an amber dot. "
            "Placing the block again with i=3 replaces it."),
        run=_demo_agent_status,
        sequence=_AGENT_STATUS_SEQ,
        golden="rich_text_agent_status",
    ),
    Entry(
        title="Fit query (OSC 7404 a=q)",
        explanation=(
            "Two a=q queries answer with a=r replies: the ideal size, and the "
            "rows the body needs at 24 cells. The body is placed at both sizes "
            "inside frames that fit it exactly, with nothing cut."),
        run=_demo_fit_query,
        sequence=_FIT_QUERY_SEQ,
        expect=r"\x1b\]7404;a=r:i=7:f=1:w=\d+:h=\d+:n=\d+:c=\d+:l=\d+:m=\d+:x=[01][^\x1b]*\x1b\\",
        golden="rich_text_fit_query",
    ),
]
