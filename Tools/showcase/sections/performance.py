# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""Performance: timed write, scroll and synchronized-output runs.

The two timed runs are bounded to well under a second and measure the whole
stdout path with time.monotonic, not the renderer alone. The synchronized
output run clears ?2026 on every path out.
"""

import time

from ..tui import CSI, RESET, move, fg_truecolor, bg_truecolor, bg_256
from . import Entry

TITLE = "Performance"
ORDER = 280

# Synchronized output (DEC private mode 2026): h begins a buffered frame the
# terminal holds back, l commits it in one atomic update so a half-drawn frame
# never reaches the display.
SYNC_BEGIN = CSI + "?2026h"
SYNC_END = CSI + "?2026l"


def _rate(count, seconds):
    """Format `count / seconds` as a human-readable per-second rate.

    Guards against a zero interval (a clock with coarse resolution can report
    elapsed 0.0 for a tiny burst); reports as K/M units for readability. Call
    sites put the unit first, so the guard's phrase reads in the same slot.
    """
    if seconds <= 0:
        return "faster than this clock reads"
    rate = count / seconds
    if rate >= 1e6:
        return f"{rate / 1e6:.2f} M/s"
    if rate >= 1e3:
        return f"{rate / 1e3:.1f} K/s"
    return f"{rate:.0f}/s"


def _run_key(tui, verb):
    tui.keys(("Space", verb))
    tui.flush()
    return tui.read_key()


def _demo_throughput(tui):
    """Repaint a 256-color field 60 times, a new diagonal phase each frame."""
    caption = "Diagonal bands step one cell per frame at an even pace across 60 repaints."
    row = tui.page(TITLE, caption)
    top, left = row + 2, 4
    height = max(4, min(12, tui.rows - top - 4))
    width = max(20, min(tui.cols - left - 2, 100))
    frames = 60
    cells_per_frame = height * width
    tui.label(row, 4, f"SGR 48;5;n per cell, {width} x {height} cells, {frames} frames")
    verb = "run it"
    while True:
        key = _run_key(tui, verb)
        if key != " ":
            return key
        start = time.monotonic()
        for f in range(frames):
            for y in range(height):
                tui.write(move(top + y, left))
                for x in range(width):
                    tui.write(bg_256(16 + ((x + y + f) % 216)), " ")
                tui.write(RESET)
            tui.flush()
        elapsed = time.monotonic() - start
        total = frames * cells_per_frame
        tui.result(top + height + 1, 4,
                   f"{total} cells in {elapsed * 1000:.1f} ms: cells {_rate(total, elapsed)}, "
                   f"frames {_rate(frames, elapsed)}")
        verb = "run again"


def _demo_scroll(tui):
    """Write 400 lines at the last row, each CR/LF scrolling the screen."""
    caption = "Space scrolls 400 numbered lines past the bottom row at one even pace."
    row = tui.page(TITLE, caption)
    tui.label(row, 4, "400 x (SGR 48;5;n, text, CR LF) at the last row")
    lines = 400
    while True:
        key = _run_key(tui, "run it")
        if key != " ":
            return key
        tui.write(move(tui.rows - 1, 1))
        start = time.monotonic()
        for n in range(lines):
            tui.write(bg_256(16 + (n % 216)), fg_truecolor(*tui.theme.highlight_fg),
                      f" scroll line {n:4d} {'#' * (n % 40)}", RESET, "\r\n")
            if n % 16 == 0:
                tui.flush()
        tui.flush()
        elapsed = time.monotonic() - start
        row = tui.page(TITLE, caption)
        tui.label(row, 4, "400 x (SGR 48;5;n, text, CR LF) at the last row")
        tui.result(row + 2, 4,
                   f"{lines} lines in {elapsed * 1000:.1f} ms: lines {_rate(lines, elapsed)}")


# ---------------------------------------------------------------------------
# Synchronized output (DEC ?2026) — atomic frame vs. tearing
# ---------------------------------------------------------------------------

def _paint_complex_frame(tui, top, left, height, width, phase):
    """Paint one busy multi-cell frame used by the sync A/B comparison.

    Fills the region row-by-row with a shifting truecolor gradient; `phase`
    rotates the colors so successive frames differ. Writes many small cells
    rather than one bulk update, so an un-synchronized commit has a wide
    window to tear in.
    """
    for y in range(height):
        tui.write(move(top + y, left))
        for x in range(width):
            r = (x * 4 + phase * 7) & 0xff
            g = (y * 9 + phase * 3) & 0xff
            b = (x * 2 + y * 2 + phase * 5) & 0xff
            tui.write(bg_truecolor(r, g, b), " ")
        tui.write(RESET)


def _demo_sync(tui):
    """Animate one gradient twice: unbracketed, then each frame inside ?2026."""
    row = tui.page(TITLE, "The second pass shows only whole frames; the first can show a torn one.")
    top, left = row + 3, 4
    height = max(4, min(10, tui.rows - top - 4))
    width = max(20, min(tui.cols - left - 2, 100))
    frames = 30
    tui.label(row, 4, "pass 1   30 frames, no hold")
    tui.label(row + 1, 4, "pass 2   30 frames, each inside CSI ? 2026 h ... CSI ? 2026 l")
    try:
        while True:
            key = _run_key(tui, "run it")
            if key != " ":
                return key
            for synchronized in (False, True):
                tui.result(row + int(synchronized), 2, ">")
                tui.flush()
                for f in range(frames):
                    if synchronized:
                        tui.write(SYNC_BEGIN)
                    _paint_complex_frame(tui, top, left, height, width, f)
                    if synchronized:
                        tui.write(SYNC_END)
                    tui.flush()
                    time.sleep(0.01)
                tui.text(row + int(synchronized), 2, " ", tui.fg_default())
    finally:
        tui.write(SYNC_END)
        tui.flush()


ENTRIES = [
    Entry(
        title="Repaint throughput",
        explanation=(
            "Space repaints a field of 256-color cells 60 times, the diagonal "
            "stepping one cell per frame, and reports cells and frames per "
            "second over the whole stdout path. The bands move at one pace."),
        run=_demo_throughput,
        descriptive=True,
    ),
    Entry(
        title="Scroll throughput",
        explanation=(
            "Space writes 400 lines at the last row, so each CR LF scrolls "
            "the screen by one, and reports lines per second. The scroll runs "
            "evenly, without chunks or a long pause."),
        run=_demo_scroll,
        descriptive=True,
    ),
    Entry(
        title="Synchronized output (?2026)",
        explanation=(
            "The same gradient animates twice. Unbracketed frames can reach "
            "the display half-written; frames inside CSI ? 2026 h ... l are "
            "presented whole. A hold left open expires after a second."),
        run=_demo_sync,
        sequence="\x1b[?2026h\x1b[?2026l\x1b[?2026$p",
        expect=r"\x1b\[\?2026;2\$y",
        snapshot=False,
    ),
]
