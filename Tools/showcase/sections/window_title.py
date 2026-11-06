# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""Window & title: XTWINOPS (CSI Ps t) and the OSC 0/1/2 titles.

Every mutator entry sends its op on Space and then the op that undoes it, so
the window ends where it started: the position and pixel size are read back
first (CSI 13 t, CSI 14 t, CSI 18 t) and written back after. A terminal that
answers none of those has no window this section can move, and the entry says
so in one line. Title entries push the live title (CSI 22 t) and pop it
(CSI 23 t) on the way out.
"""

import re
import time

from .. import tui as T
from ..tui import CSI, OSC, ST
from . import Entry

TITLE = "Window and title"
ORDER = 120

# How long a change stays on screen before the op that undoes it. Full screen
# waits longer because macOS animates into and out of its own Space.
_HOLD = 1.0
_FULL_SCREEN_HOLD = 2.5


def _reply_pair(tui, query, code):
    """Send an XTWINOPS report and return its two numbers, or None."""
    match = re.search(rb"\x1b\[" + code.encode() + rb";(\d+);(\d+)t",
                      tui.query(CSI + query))
    return (int(match.group(1)), int(match.group(2))) if match else None


def _without_window(tui, verb):
    """The skip line when no window answers CSI 11 t, else None."""
    if re.search(rb"\x1b\[[12]t", tui.query(CSI + "11t")):
        return None
    return f"No window answered CSI 11 t, so there is nothing to {verb}."


def _round_trip(tui, caption, action, describe):
    """Draw one mutator page; on Space run `action`, which returns a result line.

    `describe` returns the lines naming what Space sends, or a reason string
    when this terminal cannot demonstrate it.
    """
    while True:
        row = tui.page(TITLE, caption)
        plan = describe()
        if isinstance(plan, str):
            tui.unavailable(row, plan)
            return tui.footer()
        for line in plan:
            tui.label(row, 4, line)
            row += 1
        tui.keys(("Space", "run it"))
        tui.flush()
        key = tui.read_key()
        if key != " ":
            return key
        outcome = action()
        tui.result(row + 1, 4, outcome)
        tui.keys(("Space", "run again"))
        tui.flush()
        key = tui.read_key()
        if key != " ":
            return key


def _demo_stack(tui):
    def describe():
        missing = _without_window(tui, "restack")
        if missing:
            return missing
        return ["CSI 6 t   send the window behind every other window",
                "CSI 5 t   bring it back to the front"]

    def action():
        tui.write(CSI + "6t")
        tui.flush()
        time.sleep(_HOLD)
        tui.write(CSI + "5t")
        tui.flush()
        return "Sent CSI 6 t, then CSI 5 t."
    return _round_trip(tui, "The window drops behind the others, then comes back to the front.",
                       action, describe)


def _demo_move(tui):
    origin = []

    def describe():
        position = _reply_pair(tui, "13t", "3")
        if position is None:
            return "No window answered CSI 13 t, so there is nothing to move."
        origin[:] = position
        x, y = position
        return [f"CSI 13 t  the window is at {x}, {y}",
                f"CSI 3 ; {x + 80} ; {y + 60} t   move it 80 pixels right, 60 down",
                f"CSI 3 ; {x} ; {y} t   put it back"]

    def action():
        x, y = origin
        tui.write(CSI + f"3;{x + 80};{y + 60}t")
        tui.flush()
        time.sleep(_HOLD)
        tui.write(CSI + f"3;{x};{y}t")
        tui.flush()
        return f"Moved to {x + 80}, {y + 60} and back to {x}, {y}."
    return _round_trip(tui, "The window steps down and to the right, then returns to where it was.",
                       action, describe)


def _demo_resize_pixels(tui):
    size = []

    def describe():
        area = _reply_pair(tui, "14t", "4")
        if area is None:
            return "No window answered CSI 14 t, so there is nothing to resize."
        size[:] = area
        height, width = area
        return [f"CSI 14 t  the text area is {width} x {height} pixels",
                f"CSI 4 ; {max(1, height - 120)} ; {max(1, width - 200)} t   shrink it",
                f"CSI 4 ; {height} ; {width} t   restore it"]

    def action():
        height, width = size
        tui.write(CSI + f"4;{max(1, height - 120)};{max(1, width - 200)}t")
        tui.flush()
        time.sleep(_HOLD)
        tui.write(CSI + f"4;{height};{width}t")
        tui.flush()
        return f"Shrank by 200 x 120 pixels and restored {width} x {height}."
    return _round_trip(tui, "The window shrinks by a few hundred pixels, then grows back.",
                       action, describe)


def _demo_resize_cells(tui):
    size = []

    def describe():
        grid = _reply_pair(tui, "18t", "8")
        if grid is None:
            return "No window answered CSI 18 t, so there is nothing to resize."
        size[:] = grid
        rows, columns = grid
        return [f"CSI 18 t  the grid is {columns} x {rows} cells",
                f"CSI 8 ; {max(1, rows - 4)} ; {max(1, columns - 10)} t   four rows and ten columns fewer",
                f"CSI 8 ; {rows} ; {columns} t   restore it"]

    def action():
        rows, columns = size
        tui.write(CSI + f"8;{max(1, rows - 4)};{max(1, columns - 10)}t")
        tui.flush()
        time.sleep(_HOLD)
        tui.write(CSI + f"8;{rows};{columns}t")
        tui.flush()
        time.sleep(0.2)
        tui.refresh_size()
        return f"Shrank to {max(1, columns - 10)} x {max(1, rows - 4)} cells and restored {columns} x {rows}."
    return _round_trip(tui, "The window loses four rows and ten columns, then gets them back.",
                       action, describe)


def _demo_maximize(tui):
    def describe():
        missing = _without_window(tui, "maximize")
        if missing:
            return missing
        return ["CSI 9 ; 1 t   fill the screen",
                "CSI 9 ; 0 t   restore the previous frame"]

    def action():
        tui.write(CSI + "9;1t")
        tui.flush()
        time.sleep(_HOLD)
        tui.write(CSI + "9;0t")
        tui.flush()
        return "Sent CSI 9 ; 1 t, then CSI 9 ; 0 t."
    return _round_trip(tui, "The window zooms to fill the screen, then returns to its frame.",
                       action, describe)


def _demo_full_screen(tui):
    def describe():
        missing = _without_window(tui, "take full screen")
        if missing:
            return missing
        return ["CSI 10 ; 2 t   toggle full screen",
                "CSI 10 ; 2 t   toggle it back"]

    def action():
        tui.write(CSI + "10;2t")
        tui.flush()
        time.sleep(_FULL_SCREEN_HOLD)
        tui.write(CSI + "10;2t")
        tui.flush()
        return "Toggled full screen twice."
    return _round_trip(tui, "The window enters full screen, then leaves it.",
                       action, describe)


def _demo_iconify(tui):
    def describe():
        missing = _without_window(tui, "minimize")
        if missing:
            return missing
        return ["CSI 2 t   minimize the window, with its tabs",
                "CSI 11 t  report the state: CSI 2 t while it is down",
                "CSI 1 t   restore it"]

    def action():
        tui.write(CSI + "2t")
        tui.flush()
        time.sleep(_HOLD)
        state = T.decode_reply(tui.query(CSI + "11t"))
        tui.write(CSI + "1t")
        tui.flush()
        return f"Minimized and restored; CSI 11 t answered {state} while it was down."
    return _round_trip(tui, "The window minimizes to the Dock, then comes back.",
                       action, describe)


def _demo_refresh(tui):
    def describe():
        missing = _without_window(tui, "repaint")
        if missing:
            return missing
        return ["CSI 7 t   redraw the window from the grid"]

    def action():
        tui.write(CSI + "7t")
        tui.flush()
        return "Sent CSI 7 t; the page redrew with every cell as it was."
    return _round_trip(tui, "The window redraws from the grid; no cell changes.",
                       action, describe)


def _demo_reports(tui):
    row = tui.page(TITLE, "Each report the window answers, beside the op that asked for it.")
    queries = [
        ("CSI 11 t", "window state", CSI + "11t"),
        ("CSI 13 t", "window position", CSI + "13t"),
        ("CSI 14 t", "text area in pixels", CSI + "14t"),
        ("CSI 16 t", "one cell in pixels", CSI + "16t"),
        ("CSI 18 t", "text area in cells", CSI + "18t"),
        ("CSI 19 t", "display in cells", CSI + "19t"),
        ("CSI 20 t", "icon label", CSI + "20t"),
        ("CSI 21 t", "window title", CSI + "21t"),
    ]
    for op, meaning, query in queries:
        tui.label(row, 4, op)
        tui.text(row, 14, meaning, tui.fg_default())
        tui.result(row, 36, T.truncate(T.decode_reply(tui.query(query)), tui.cols - 37))
        row += 1
    return tui.footer()


def _demo_title_stack(tui):
    tui.write(CSI + "22;0t")
    tui.write(f"{OSC}2;Pushed and replaced{ST}")
    row = tui.page(TITLE, "The title reads 'Pushed and replaced' until Space pops the saved one back.")
    tui.label(row, 4, "CSI 22 ; 0 t   push the window and icon titles")
    tui.label(row + 1, 4, "OSC 2 ; Pushed and replaced ST")
    tui.keys(("Space", "pop"))
    tui.flush()
    key = tui.read_key()
    if key == " ":
        tui.write(CSI + "23;0t")
        tui.flush()
        tui.label(row + 2, 4, "CSI 23 ; 0 t   pop")
        tui.result(row + 4, 4, "CSI 21 t now reports " + T.decode_reply(tui.query(CSI + "21t")))
        return tui.footer()
    tui.write(CSI + "23;0t")
    tui.flush()
    return key


def _demo_osc_title(tui):
    tui.write(CSI + "22;0t")
    tui.write(f"{OSC}2;Set by OSC 2{ST}")
    tui.write(f"{OSC}1;Set by OSC 1{ST}")
    row = tui.page(TITLE, "The window title reads 'Set by OSC 2'; the icon label changes on its own.")
    tui.label(row, 4, "OSC 2 ; Set by OSC 2 ST   window title")
    tui.label(row + 1, 4, "OSC 1 ; Set by OSC 1 ST   icon label")
    tui.result(row + 3, 4, "CSI 21 t reports " + T.decode_reply(tui.query(CSI + "21t")))
    tui.result(row + 4, 4, "CSI 20 t reports " + T.decode_reply(tui.query(CSI + "20t")))
    key = tui.footer()
    tui.write(CSI + "23;0t")
    tui.flush()
    return key


ENTRIES = [
    Entry(
        title="Window title (OSC 0, 1, 2)",
        explanation=(
            "OSC 2 sets the window title and OSC 1 the icon label, each on its "
            "own; OSC 0 sets both. CSI 21 t reports the title as OSC l <title> ST "
            "and CSI 20 t the label as OSC L <label> ST. The title live on entry "
            "is pushed and popped, so it reads the same on return."),
        run=_demo_osc_title,
        sequence="\x1b]2;Ubiquitty — OSC 2 title\x1b\\\x1b[21t",
        expect=r"\x1b\]l[^\x07\x1b]*\x1b\\",
        snapshot=False,
        golden="window_title_osc_0_1_2_set_window_icon_title",
    ),
    Entry(
        title="Title stack (CSI 22 t, CSI 23 t)",
        explanation=(
            "CSI 22 ; 0 t pushes the window and icon titles; OSC 2 replaces the "
            "title; CSI 23 ; 0 t pops the saved pair back, and CSI 21 t then "
            "reports the title from before the push."),
        run=_demo_title_stack,
        sequence=("\x1b]2;title pushed by CSI 22 t\x1b\\\x1b[22;0t"
                  "\x1b]2;title the pop discards\x1b\\\x1b[23;0t\x1b[21t"),
        expect=r"\x1b\]l[^\x07\x1b]*\x1b\\",
        snapshot=False,
        golden="window_title_title_stack_push_22_pop_23",
    ),
    Entry(
        title="Window reports (CSI 11-21 t)",
        explanation=(
            "CSI 11 t answers CSI 1 t (shown) or CSI 2 t (minimized); 13 t the "
            "position as CSI 3 ; x ; y t; 14 t the text area as CSI 4 ; H ; W t "
            "pixels; 16 t one cell as CSI 6 ; H ; W t; 18 t the grid as "
            "CSI 8 ; rows ; columns t; 19 t the display in cells as CSI 9 ; rows ; "
            "columns t; 20 t and 21 t the label and title as OSC L and OSC l."),
        run=_demo_reports,
        sequence="\x1b[11t\x1b[13t\x1b[14t\x1b[16t\x1b[19t\x1b[20t\x1b[21t",
        expect=r"\x1b\[(1|2|3;[0-9;]+|4;[0-9;]+|6;[0-9;]+|9;[0-9;]+)t"
               r"|\x1b\][lL][^\x07\x1b]*",
        snapshot=False,
        golden="window_title_xtwinops_reporters_csi_11_13_14_16_19_20_21_t",
    ),
    Entry(
        title="Move (CSI 3 t)",
        explanation=(
            "CSI 13 t reads the window position; CSI 3 ; x ; y t moves the window's "
            "top-left corner to that pixel position, and a second CSI 3 t puts it "
            "back where CSI 13 t found it."),
        run=_demo_move,
        sequence="\x1b[3;80;60t\x1b[3;0;0t",
    ),
    Entry(
        title="Resize in pixels (CSI 4 t)",
        explanation=(
            "CSI 14 t reads the text area in pixels; CSI 4 ; H ; W t resizes the "
            "window so its text area is H x W pixels, and a second CSI 4 t "
            "restores the size CSI 14 t reported."),
        run=_demo_resize_pixels,
        sequence="\x1b[4;400;600t\x1b[4;0;0t",
    ),
    Entry(
        title="Resize in cells (CSI 8 t)",
        explanation=(
            "CSI 18 t reads the grid in cells; CSI 8 ; rows ; columns t resizes "
            "the window to that grid, and a second CSI 8 t restores the rows and "
            "columns CSI 18 t reported."),
        run=_demo_resize_cells,
        sequence="\x1b[8;20;70t\x1b[8;24;80t",
    ),
    Entry(
        title="Maximize (CSI 9 t)",
        explanation=(
            "CSI 9 ; 1 t zooms the window to fill the screen and CSI 9 ; 0 t "
            "restores the frame it had before."),
        run=_demo_maximize,
        sequence="\x1b[9;1t\x1b[9;0t",
    ),
    Entry(
        title="Full screen (CSI 10 t)",
        explanation=(
            "CSI 10 ; 2 t toggles full screen, so sending it twice enters and "
            "leaves it from either starting state. CSI 10 ; 1 t and 10 ; 0 t set "
            "it on and off outright."),
        run=_demo_full_screen,
        sequence="\x1b[10;2t\x1b[10;2t",
    ),
    Entry(
        title="Minimize (CSI 2 t, CSI 1 t)",
        explanation=(
            "CSI 2 t minimizes the window with all its tabs and CSI 1 t restores "
            "it; while it is down CSI 11 t answers CSI 2 t."),
        run=_demo_iconify,
        sequence="\x1b[2t\x1b[1t",
        golden="window_title_csi_2_t_1_t_iconify_deiconify",
    ),
    Entry(
        title="Stacking order (CSI 6 t, CSI 5 t)",
        explanation=(
            "CSI 6 t lowers the window behind every other window and CSI 5 t "
            "raises it to the front; neither moves or resizes it."),
        run=_demo_stack,
        sequence="\x1b[6t\x1b[5t",
    ),
    Entry(
        title="Repaint (CSI 7 t)",
        explanation=(
            "CSI 7 t redraws the window from the grid. The grid is the only "
            "source of what a cell shows, so the redraw changes nothing."),
        run=_demo_refresh,
        sequence="\x1b[7t",
    ),
]
