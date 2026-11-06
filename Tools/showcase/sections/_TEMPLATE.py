# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""Copyable skeleton for a showcase section.

Copy this file to `Tools/showcase/sections/<name>.py` (no leading underscore),
set TITLE and ORDER, and fill ENTRIES. Discovery is automatic. Then run
`python3 Tools/showcase/showcase.py --self-test` and mint the new baselines by
name with `python3 Tests/Snapshots/run.py --backend portable --update-missing
<identifier>`.

Every entry draws the same page through the Tui helpers: `tui.page(TITLE,
caption)` paints the banner and one caption line saying what to look at and
returns the first body row; the body makes the terminal do the thing, with
`tui.label` for sequence names and `tui.result` for what came back;
`tui.footer(("Space", "verb"))` draws the key bar and waits. An entry sends
every sequence it names and restores what it changed before it returns. An
entry that cannot demonstrate on this terminal prints one `tui.unavailable`
line instead of describing what it would have done.

`run(tui)` never enters or leaves the alternate screen or raw mode; the menu
owns both.
"""

from .. import tui as T
from ..tui import CSI
from . import Entry

TITLE = "Template"
ORDER = 9999


def _demo_underline(tui):
    row = tui.page(TITLE, "The word below is underlined; the rest of the row is not.")
    tui.write(T.move(row, 4), CSI + "4m", "underlined", T.RESET, " plain")
    tui.label(row + 2, 4, "CSI 4 m   underline on, CSI 0 m off")
    return tui.footer()


def _demo_query(tui):
    row = tui.page(TITLE, "The terminal's Primary Device Attributes reply.")
    tui.label(row, 4, "CSI c")
    tui.result(row, 14, T.decode_reply(tui.query(CSI + "c")))
    return tui.footer()


ENTRIES = [
    Entry(
        title="Underline (SGR 4)",
        explanation="SGR 4 underlines the run that follows and SGR 0 ends it.",
        run=_demo_underline,
        golden="template_underline",
    ),
    Entry(
        title="Device attributes (DA1)",
        explanation="CSI c answers CSI ? <class> ; <features> c.",
        run=_demo_query,
        # A reply check needs the exact bytes the snapshot feeds.
        sequence="\x1b[c",
        expect=r"\x1b\[\?[0-9;]*c",
    ),
]
