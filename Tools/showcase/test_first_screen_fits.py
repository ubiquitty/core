#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""The showcase's menu screen fits the window it is drawn in.

The banner is a DEC double-height pair on the top two rows. A character the
menu writes past the bottom row's last column wraps onto a new line, the
screen scrolls up one row, and the banner's top half leaves the screen: the
title shows as its lower half, cut flat at the window's top edge. The key bar
did that at 64 columns, where its last pair began in the last column.

Each screen is replayed through a small model of the terminal's cursor:
absolute positioning, erases, DEC line attributes, which halve a row's
columns, and autowrap with the pending-wrap state. It answers whether the
screen scrolled and what each row holds; colours are not modelled.
"""

import os
import re
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from showcase import showcase as S  # noqa: E402
from showcase import tui as T  # noqa: E402

CONTROL_STRING = re.compile(r"\x1b[\]P_^X].*?(?:\x07|\x1b\\)", re.S)
CSI_SEQUENCE = re.compile(r"\x1b\[([?>=<]?)([0-9;:]*)([ -/]*)([@-~])")


class ScreenModel:
    """Where a stream leaves the cursor and each row's text, and whether it scrolled."""

    def __init__(self, rows, cols):
        self.rows, self.cols = rows, cols
        self.cells = [[" "] * cols for _ in range(rows)]
        self.attributes = [0] * rows
        self.row = self.col = 0
        self.is_wrap_pending = False
        self.does_autowrap = True
        self.scrolls = 0

    def text(self, row):
        width = self.cols // 2 if self.attributes[row] else self.cols
        return "".join(self.cells[row][:width]).rstrip()

    def feed(self, stream):
        index = 0
        while index < len(stream):
            character = stream[index]
            if character == "\x1b":
                index = self._escape(stream, index)
                continue
            index += 1
            if character == "\r":
                self.col, self.is_wrap_pending = 0, False
            elif character in "\n\x0b\x0c":
                self._line_feed()
            elif character == "\b":
                self.col, self.is_wrap_pending = max(0, self.col - 1), False
            elif character >= " ":
                self._print(character)

    def _escape(self, stream, index):
        control = CONTROL_STRING.match(stream, index)
        if control:
            return control.end()
        sequence = CSI_SEQUENCE.match(stream, index)
        if sequence:
            self._csi(*sequence.groups())
            return sequence.end()
        final = stream[index + 1:index + 2]
        if final == "#":
            selector = stream[index + 2:index + 3]
            self.attributes[self.row] = {"3": 3, "4": 4, "5": 0, "6": 6}.get(selector, self.attributes[self.row])
            self.col = min(self.col, self._width() - 1)
            return index + 3
        if final in ("D", "E"):
            self._line_feed()
            if final == "E":
                self.col = 0
        return index + 2

    def _csi(self, prefix, parameters, intermediates, final):
        values = [int(p.split(":")[0] or 0) for p in parameters.split(";")] if parameters else []

        def value(position, default):
            return values[position] if len(values) > position and values[position] else default

        if prefix == "?" and final in "hl":
            if 7 in values:
                self.does_autowrap = final == "h"
            return
        if prefix or intermediates:
            return
        if final in "Hf":
            self.row = min(value(0, 1), self.rows) - 1
            self.col = min(value(1, 1), self._width()) - 1
            self.is_wrap_pending = False
        elif final == "J" and value(0, 0) == 2:
            self.cells = [[" "] * self.cols for _ in range(self.rows)]
            self.attributes = [0] * self.rows
        elif final == "K" and value(0, 0) == 2:
            self.cells[self.row] = [" "] * self.cols
        elif final == "r":
            raise AssertionError("the menu sets no scrolling region; the model assumes the whole screen")

    def _width(self):
        return self.cols // 2 if self.attributes[self.row] else self.cols

    def _line_feed(self):
        self.is_wrap_pending = False
        if self.row < self.rows - 1:
            self.row += 1
            return
        self.scrolls += 1
        del self.cells[0], self.attributes[0]
        self.cells.append([" "] * self.cols)
        self.attributes.append(0)

    def _print(self, character):
        width = T.char_width(character)
        if width == 0:
            return
        if self.is_wrap_pending and self.does_autowrap:
            self.col = 0
            self._line_feed()
        self.cells[self.row][self.col] = character
        if self.col + width >= self._width():
            self.is_wrap_pending = True
        else:
            self.col += width


class FirstScreen(unittest.TestCase):
    SIZES = [(rows, cols) for rows in (12, 24, 38, 50) for cols in range(40, 201)]

    @classmethod
    def setUpClass(cls):
        cls.sections = S.discover()

    def render(self, rows, cols, in_section):
        tui = S._CaptureTui()
        try:
            tui.rows, tui.cols = rows, cols
            menu = S.Menu(tui, self.sections)
            if in_section:
                menu.in_section = self.sections[0]
            menu.render()
            screen = ScreenModel(rows, cols)
            screen.feed(tui.captured())
            return screen
        finally:
            tui.close()

    def check(self, in_section, leave):
        for rows, cols in self.SIZES:
            with self.subTest(rows=rows, cols=cols):
                screen = self.render(rows, cols, in_section)
                self.assertEqual(screen.scrolls, 0, "the screen scrolled, so the banner's top half left it")
                self.assertEqual(screen.attributes[:2], [3, 4], "the banner is not a DECDHL pair on the top two rows")
                title = screen.text(0)
                self.assertTrue(title.strip().startswith("Ubiquitty"), f"the banner reads {title!r}")
                self.assertEqual(screen.text(1), title, "the two halves of the banner differ")
                self.assertTrue(screen.text(rows - 1).endswith(leave), f"the key bar reads {screen.text(rows - 1)!r}")

    def test_the_section_list_fits_and_keeps_its_quit_key(self):
        self.check(in_section=False, leave="q quit")

    def test_an_entry_list_fits_and_keeps_its_back_key(self):
        self.check(in_section=True, leave="Esc back")

    def test_the_reported_size_draws_the_whole_banner(self):
        screen = self.render(38, 64, in_section=False)
        self.assertEqual(screen.scrolls, 0)
        self.assertEqual(screen.text(0).strip(), "Ubiquitty Showcase")
        self.assertEqual(screen.text(1).strip(), "Ubiquitty Showcase")


if __name__ == "__main__":
    unittest.main()
