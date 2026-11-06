# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""Search: the find bar, driven by hand over rows whose hits can be counted.

No escape sequence opens the find bar or steps its results, so these entries
send none. Each prints a backdrop and its caption says what to press and what
the bar shows. The captions count: a query typed from a caption also matches
the caption itself, and the numbers they give include it. The snapshot pins
each backdrop; the interaction belongs to the app UI tests.
"""

from ..tui import RESET, move, fg_truecolor
from . import Entry

TITLE = "Search"
ORDER = 160


def _rows(tui, row, lines):
    for line in lines:
        tui.text(row, 6, line, tui.fg_default())
        row += 1
    return tui.footer()


# ---------------------------------------------------------------------------
# Snapshotable backdrops
#
# The find bar has no driving escape sequence, so the interaction is a person's
# (no `expect`). The content each entry prints is deterministic, though, so
# hoisting it to a `_SEQ` lets the golden render exactly the rows Find will then
# be driven against.
# ---------------------------------------------------------------------------

def _backdrop_seq(lines):
    """Join plain backdrop lines into a CR/LF byte string for snapshotting."""
    return "".join(line + "\r\n" for line in lines)


# WIDGET stands five times here and nowhere else in these rows, in one case only,
# so a count is the same whether or not case-sensitive search is on.
_SCROLLBACK_SEQ = _backdrop_seq([
    "make: entering directory '/src/press'",
    "install -m 644 build/WIDGET.o /opt/press/lib",
    "3 targets rebuilt, 0 cached, 12 already up to date",
    "inventory: 3 WIDGET, 12 gadget, 7 sprocket, 1 WIDGET (spare)",
    "checksum 4b0f2c1d  size 91232  mtime 2026-05-25 09:14",
    "ship the WIDGET to dock 4; the WIDGET manifest follows",
    "queue drained: 18 packed, 2 held, 0 failed",
])

_HISTORY_SEQ = _backdrop_seq([
    f"line {i}: this row contains {tok} as a marker"
    for i, tok in enumerate(["FIRSTTERM", "SECONDTERM", "THIRDTERM"], 1)
])

# One long logical line that SOFT-WRAPS at the 80-col right margin: 'error'
# lands on the first visual row, 'fatal' on the wrapped continuation. A
# soft-wrap joins with no separator in the search engine's joined buffer, so a
# pattern with '.*' spans the visual break (the engine's '.' does not cross a
# hard '\n', which a single-line Find field can't type anyway).
_MULTILINE_TEXT = (
    "status: connection lost while retrying twice, then it kept giving error "
    "and finally fatal: aborting after the retry budget is exhausted")
_MULTILINE_SEQ = _MULTILINE_TEXT + "\r\n"

_BY_SELECTION_SEQ = _backdrop_seq([
    "build start  CHECKPOINT-42  queued",
    "running tests for module core ...",
    "build done   CHECKPOINT-42  passed",
    "uploading artifacts to the cache ...",
    "deploy gate  CHECKPOINT-42  approved",
])

_NAVIGATION_SEQ = _backdrop_seq([
    "cluster map  ->  NODE #1 (leader)   region us-east",
    "          link  ---- 12ms ----",
    "cluster map  ->  NODE #2 (follower) region us-east",
    "          link  ---- 31ms ----",
    "cluster map  ->  NODE #3 (follower) region eu-west",
    "          link  ---- 88ms ----",
    "cluster map  ->  NODE #4 (follower) region ap-south",
])

_SAMPLE_BACKDROP_SEQ = _backdrop_seq([
    "repeats: TOKEN TOKEN once-only TOKEN spacer TOKEN end-TOKEN",
    "phrase: the queue drained at 09:14 and the cache stayed",
    "  ...: warm  (the phrase breaks at a hard row end)",
    "dates: 2026-05-25  2024-01-01  1999-12-31  not-a-date",
    "ipv4: 10.0.0.1  192.168.1.254  255.255.255.0  8.8.8.8",
    "hex: 0xDEADBEEF  0xCAFEBABE  0x0000  feedface",
    "email: ada@example.com  grace+ci@host.example  root@localhost",
    "case: Mixed CASE case CaSe cAsE for case-sensitivity tests",
    "words: cat category scatter cathedral concatenate bobcat",
    "digits: id-001 id-010 id-100 id-1000 ref-001 ref-010",
])


def _demo_scrollback(tui):
    row = tui.page(TITLE, "Press Cmd-F and type WIDGET: six hits, the focused one solid yellow.")
    return _rows(tui, row, [line for line in _SCROLLBACK_SEQ.split("\r\n") if line])


def _demo_history(tui):
    row = tui.page(TITLE, "Find each marker with Enter; the magnifier menu then lists all three.")
    return _rows(tui, row, [line for line in _HISTORY_SEQ.split("\r\n") if line])


def _demo_multiline_regex(tui):
    row = tui.page(TITLE, "With regex on, find error.*fatal: one hit spans both wrapped rows.")
    # Written from column 1 so it soft-wraps at the right margin; a soft wrap
    # joins with no separator in the search buffer, so '.*' crosses it.
    tui.write(move(row, 1), tui.fg_default(), _MULTILINE_TEXT, RESET)
    return tui.footer()


def _demo_by_selection(tui):
    row = tui.page(TITLE, "Select a CHECKPOINT-42, press Cmd-E then Cmd-G: the next one is focused.")
    return _rows(tui, row, [line for line in _BY_SELECTION_SEQ.split("\r\n") if line])


def _demo_navigation(tui):
    row = tui.page(TITLE, "Find NODE; Cmd-G and Shift-Cmd-G step the focus and wrap at either end.")
    return _rows(tui, row, [line for line in _NAVIGATION_SEQ.split("\r\n") if line])


def _demo_backdrop(tui):
    row = tui.page(TITLE, "With regex on, \\bcat\\b lights one word on the words row, not category.")
    t = tui.theme
    for line in (line for line in _SAMPLE_BACKDROP_SEQ.split("\r\n") if line):
        name, _, rest = line.partition(":")
        tui.text(row, 4, name + ":", fg_truecolor(*t.accent))
        tui.text(row, 14, rest.strip(), tui.fg_default())
        row += 1
    return tui.footer()


ENTRIES = [
    Entry(
        title="Find (Cmd-F)",
        explanation=(
            "One search covers the scrollback, oldest first, then the live "
            "screen. Each hit is a grey pill and the focused one a solid yellow "
            "cell; the counter gives the focused hit's place among all of them."),
        run=_demo_scrollback,
        sequence=_SCROLLBACK_SEQ,
        golden="search_scrollback_backdrop",
    ),
    Entry(
        title="Search history",
        explanation=(
            "A query committed with Enter joins the field's recents, newest "
            "first, deduplicated, twelve deep; choosing one runs it again. The "
            "list is saved with the field and survives a relaunch."),
        run=_demo_history,
        sequence=_HISTORY_SEQ,
        golden="search_history_backdrop",
    ),
    Entry(
        title="Regex across a soft wrap",
        explanation=(
            "A soft-wrapped line joins with no separator, so error.*fatal "
            "matches across the wrap, paints one run per row, and counts once. "
            "A hard line end joins with a newline that no match crosses."),
        run=_demo_multiline_regex,
        sequence=_MULTILINE_SEQ,
        golden="search_multiline_regex_backdrop",
    ),
    Entry(
        title="Search by selection (Cmd-E)",
        explanation=(
            "Cmd-E loads the selection as the query without raising the bar, "
            "so Cmd-G steps to the next CHECKPOINT-42 with nothing typed. The "
            "regex toggle decides whether it reads as a literal or a pattern."),
        run=_demo_by_selection,
        sequence=_BY_SELECTION_SEQ,
        golden="search_by_selection_backdrop",
    ),
    Entry(
        title="Next and previous (Cmd-G, Shift-Cmd-G)",
        explanation=(
            "Cmd-G steps hits in buffer order and Shift-Cmd-G in reverse; next "
            "from the last wraps to the first and previous from the first to "
            "the last, and the counter follows the focus."),
        run=_demo_navigation,
        sequence=_NAVIGATION_SEQ,
        golden="search_navigation_backdrop",
    ),
    Entry(
        title="Regex and case",
        explanation=(
            "Search folds case unless the case toggle is on. Regex is RE2 "
            "without back-references or lookaround: \\bcat\\b takes only the "
            "standalone word, and 0x[0-9A-Fa-f]+ takes the three 0x literals "
            "and not feedface."),
        run=_demo_backdrop,
        sequence=_SAMPLE_BACKDROP_SEQ,
        golden="search_sample_backdrop",
    ),
]
