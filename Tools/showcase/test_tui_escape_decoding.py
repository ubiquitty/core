#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""The showcase's escape decoding: which sequences are keys and which are replies.

The showcase enables focus reporting, locator reporting and mouse tracking to
demonstrate them. The terminal then ANSWERS -- and an answer decoded to
"unknown" is one `wait_key` accepts as a keypress and dismisses the page with.
The demo then exits precisely when the feature it is demonstrating starts
working, and user testing records it as a terminal defect:

  - "Focus reporting (DECSET 1004) exits the test on focus change"   (CSI I / CSI O)
  - "Hilite mouse tracking exits the test on release"

So the property under test is not cosmetic: it decides whether a correct
terminal is recorded as a broken one.
"""

import os
import importlib.util
import re
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__))))

from tui import Tui  # noqa: E402
import tui as toolkit  # noqa: E402


class DecodeEscape(unittest.TestCase):
    def decode(self, seq):
        # _decode_escape touches no terminal state, so an unconstructed
        # instance is enough and the test needs no tty.
        return Tui._decode_escape(Tui.__new__(Tui), seq)

    def test_focus_reports_are_not_keys(self):
        """CSI I and CSI O are focus in/out under DECSET 1004."""
        self.assertEqual(self.decode("[I"), ("report", "[I"))
        self.assertEqual(self.decode("[O"), ("report", "[O"))

    def test_solicited_replies_are_not_keys(self):
        """A reply the showcase asked for must not dismiss the page showing it."""
        self.assertEqual(self.decode("[24;80R")[0], "report")      # CPR
        self.assertEqual(self.decode("[?62;1;6c")[0], "report")    # device attributes
        self.assertEqual(self.decode("[0n")[0], "report")          # DSR
        self.assertEqual(self.decode("[8;24;80t")[0], "report")    # window ops
        self.assertEqual(self.decode("[1;2;3;4&w")[0], "report")   # DECELR locator

    def test_a_binding_always_wins_over_a_report_final(self):
        """CSI A is the up arrow, not a report, however the final is spelled.

        This is the ordering that matters: the key table is consulted first, so
        adding a final byte to the report set can never silently steal a key.
        """
        self.assertEqual(self.decode("[A"), "up")
        self.assertEqual(self.decode("[B"), "down")
        self.assertEqual(self.decode("[C"), "right")
        self.assertEqual(self.decode("[D"), "left")
        self.assertEqual(self.decode("[H"), "home")
        self.assertEqual(self.decode("[F"), "end")
        self.assertEqual(self.decode("[5~"), "pageup")

    def test_mouse_still_decodes(self):
        """SGR mouse reports stay input -- the menu navigates by them."""
        self.assertEqual(self.decode("[<0;10;5M"), ("mouse", 0, 10, 5, True))
        self.assertEqual(self.decode("[<0;10;5m"), ("mouse", 0, 10, 5, False))

    def test_genuinely_unknown_stays_unknown(self):
        """A sequence that is neither is still 'unknown', not a report.

        Reports are named, not assumed: classifying everything unrecognised as
        a report would hide a key this demo should have bound.
        """
        self.assertEqual(self.decode("[99Z"), "unknown")


class GraphicsAndMusic(unittest.TestCase):
    def test_animation_chunks_and_playback_use_protocol_keys(self):
        pixels = bytes([255, 0, 0, 255]) * 4096
        wire = toolkit.image_kitty_animation(64, 64, [(pixels, 400), (pixels, 250)])
        blocks = re.findall(r"\x1b_G(.*?)\x1b\\", wire)
        frame_blocks = [block.split(";", 1)[0] for block in blocks if block.startswith("a=f")]
        self.assertGreater(len(frame_blocks), 1)
        self.assertTrue(all("a=f" in block.split(",") for block in frame_blocks))
        self.assertIn("a=a,i=42,s=3,r=1,z=400,q=2", blocks[-1])

    def test_score_bars_fit_the_clock_and_resolve(self):
        path = os.path.join(os.path.dirname(__file__), "Examples", "evolution.py")
        spec = importlib.util.spec_from_file_location("evolution_score", path)
        evolution = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(evolution)
        self.assertEqual(len(evolution.SONG), 16)
        for chord, phrase in evolution.SONG + evolution.FINAL:
            self.assertIn(chord, evolution.CHORDS)
            self.assertEqual(sum(duration for _, duration in phrase), evolution.W)
        self.assertEqual([note for note, _ in evolution.SONG[0][1]],
                         [evolution.E6, evolution.E6, evolution.F6, evolution.G6])
        self.assertEqual(evolution.FINAL[-1][1], [(evolution.REST, evolution.W)])


if __name__ == "__main__":
    unittest.main(verbosity=2)
