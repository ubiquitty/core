# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""Tab & window indicators: OSC 9;4 task progress and OSC 21337 tab status.

Both draw in the window chrome rather than the grid. OSC 9 ; 4 ; state ; value
is the ConEmu and Windows Terminal progress report: 0 clear, 1 determinate
(0-100), 2 error, 3 indeterminate, 4 paused. Ubiquitty draws a thin bar above
the content, a bar in the active tab and a ring on the others. OSC 21337 ;
indicator=#RRGGBB puts a colored dot on a background tab; an empty value
clears it. Every entry clears what it set before returning.
"""

import os
import select
import time

from . import Entry

TITLE = "Tab and window indicators"
ORDER = 100


def _osc94(state, value):
    return f"\x1b]9;4;{state};{value}\x07"


_CLEAR = _osc94(0, 0)


def _sleep_or_key(tui, secs):
    """Sleep up to `secs`; return True early if a key arrives (and drain it)."""
    end = time.monotonic() + secs
    while True:
        remaining = end - time.monotonic()
        if remaining <= 0:
            return False
        r, _, _ = select.select([tui._fd], [], [], remaining)
        if r:
            try:
                os.read(tui._fd, 64)
            except OSError:
                pass
            return True


def _demo_download(tui):
    row = tui.page(TITLE, "The bar spins, then fills from 0 to 100 with a pause at 63.")
    tui.label(row, 4, "OSC 9 ; 4 ; 3 ; 0   then OSC 9 ; 4 ; 1 ; 0 ... 100, OSC 9 ; 4 ; 4 at 63")
    status_row = row + 2
    tui.keys()
    tui.flush()

    def show(text):
        tui.text(status_row, 4, f"{text:<40}")
        tui.result(status_row, 4, text)
        tui.flush()

    try:
        tui.write(_osc94(3, 0))
        show("state 3, no value")
        if _sleep_or_key(tui, 1.2):
            return None
        for percent in range(0, 101):
            tui.write(_osc94(1, percent))
            show(f"state 1, value {percent}")
            if percent == 63:
                tui.write(_osc94(4, percent))
                show("state 4, paused at 63")
                if _sleep_or_key(tui, 0.8):
                    return None
                tui.write(_osc94(1, percent))
                tui.flush()
            if _sleep_or_key(tui, 0.045):
                return None
        show("state 1, value 100")
        return tui.wait_key()
    finally:
        tui.write(_CLEAR)
        tui.flush()


def _hold(caption, label, sequence, clear):
    """A run() that sends one sequence, holds it until a key, then clears it."""
    def run(tui):
        row = tui.page(TITLE, caption)
        tui.label(row, 4, label)
        tui.write(sequence)
        try:
            return tui.footer()
        finally:
            tui.write(clear)
            tui.flush()
    return run


def _progress(caption, state, value):
    return _hold(caption, f"OSC 9 ; 4 ; {state} ; {value} BEL", _osc94(state, value), _CLEAR)


def _osc21337(indicator, status, status_color):
    return f"\x1b]21337;indicator={indicator};status={status};status-color={status_color}\x07"


_TABSTATUS_CLEAR = _osc21337("", "", "")


def _tab_dot(caption, indicator, status, status_color):
    return _hold(
        caption,
        f"OSC 21337 ; indicator={indicator} ; status={status} ; status-color={status_color} BEL",
        _osc21337(indicator, status, status_color), _TABSTATUS_CLEAR)


_BACKGROUND = "; select another tab to see it."

ENTRIES = [
    Entry(
        title="Download progress (OSC 9;4)",
        explanation=(
            "State 3 for about a second, then state 1 stepping 0 to 100 with a "
            "state 4 stall at 63, then state 0 on return. The fill grows from the "
            "left edge and holds at 63 during the stall."),
        run=_demo_download,
        golden="progress_download",
    ),
    Entry(
        title="Determinate at 40% (OSC 9;4;1)",
        explanation=(
            "The bar fills 40% of its width and holds there. It takes the app "
            "accent while the window is active and its pane focused, and a "
            "dimmed gray otherwise."),
        run=_progress("The bar above the content fills 40% of its width.", 1, 40),
        sequence="\x1b]9;4;1;40\x07",
        golden="progress_determinate_40",
    ),
    Entry(
        title="Indeterminate (OSC 9;4;3)",
        explanation=(
            "A comet sweeps the width and mirrors at each end, its solid cap "
            "leading on both legs. No percentage is shown."),
        run=_progress("A comet sweeps across the bar and back.", 3, 0),
        sequence="\x1b]9;4;3;0\x07",
        golden="progress_indeterminate",
    ),
    Entry(
        title="Error (OSC 9;4;2)",
        explanation=(
            "The bar fills its whole width in red, whatever the value. Red is "
            "never dimmed, so it stays red while the window is in the background."),
        run=_progress("The whole bar turns red.", 2, 0),
        sequence="\x1b]9;4;2;0\x07",
        golden="progress_error",
    ),
    Entry(
        title="Paused at 60% (OSC 9;4;4)",
        explanation=(
            "The fill stands at 60% of the width and nothing animates: the "
            "geometry of state 1 at 60, held."),
        run=_progress("The bar holds at 60% and does not move.", 4, 60),
        sequence="\x1b]9;4;4;60\x07",
        golden="progress_paused_60",
    ),
    Entry(
        title="Progress clear (OSC 9;4;0)",
        explanation=(
            "State 0 hides the indicator: the bar above the content and any ring "
            "on a background tab go at once."),
        run=_progress("No progress bar is shown.", 0, 0),
        sequence="\x1b]9;4;0;0\x07",
    ),
    Entry(
        title="Waiting tab dot (OSC 21337)",
        explanation=(
            "indicator=#5f87ff puts one blue dot on this tab while another tab "
            "is selected, and none while it is selected. The status text and "
            "status-color are parsed and not drawn."),
        run=_tab_dot("This tab carries a blue dot" + _BACKGROUND,
                     "#5f87ff", "Waiting", "#5f87ff"),
        sequence="\x1b]21337;indicator=#5f87ff;status=Waiting;status-color=#5f87ff\x07",
        golden="tab_status_waiting",
    ),
    Entry(
        title="Busy tab dot (OSC 21337)",
        explanation=(
            "indicator=#ff9500 puts one orange dot on this tab while another tab "
            "is selected."),
        run=_tab_dot("This tab carries an orange dot" + _BACKGROUND,
                     "#ff9500", "Working…", "#ff9500"),
        sequence="\x1b]21337;indicator=#ff9500;status=Working…;status-color=#ff9500\x07",
        golden="tab_status_busy",
    ),
    Entry(
        title="Idle tab dot (OSC 21337)",
        explanation=(
            "indicator=#00d75f puts one green dot on this tab. status-color="
            "#888888 differs and changes nothing: the dot takes its color from "
            "indicator alone."),
        run=_tab_dot("This tab carries a green dot" + _BACKGROUND,
                     "#00d75f", "Idle", "#888888"),
        sequence="\x1b]21337;indicator=#00d75f;status=Idle;status-color=#888888\x07",
        golden="tab_status_idle",
    ),
    Entry(
        title="Tab dot clear (OSC 21337)",
        explanation=(
            "An empty indicator= drops the dot from the tab at once; a bare key "
            "with no '=' clears it the same way."),
        run=_tab_dot("This tab carries no dot.", "", "", ""),
        sequence="\x1b]21337;indicator=;status=;status-color=\x07",
    ),
]
