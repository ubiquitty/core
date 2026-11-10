#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""Tests Scripts/ci_pairing.py's reading of a pull request's pairing line.

The description is text anyone opening a pull request writes, so each case is a
way it could name something the job should not build, or name nothing and be
read as naming something.
"""

import importlib.util
from pathlib import Path
import sys
import unittest

SPECIFICATION = importlib.util.spec_from_file_location(
    "ci_pairing", Path(__file__).resolve().parent / "ci_pairing.py")
pairing = importlib.util.module_from_spec(SPECIFICATION)
SPECIFICATION.loader.exec_module(pairing)

KIT = "ubiquitty/kit"
HEAD = "0123456789abcdef0123456789abcdef01234567"


def open_pull_request(repository="someone/kit", sha=HEAD):
    return {"state": "open", "merged_at": None, "head": {"repo": {"full_name": repository}, "sha": sha}}


class PairingTests(unittest.TestCase):
    def choose(self, body, read=None, default_ref="main"):
        return pairing.choose(body, KIT, default_ref, read or (lambda number: self.fail("nothing to read")))

    def test_no_pull_request_or_no_line_builds_the_default(self):
        for body in (None, "", "Fixes the cursor.\n\nPairs-With: ubiquitty/ubiquitty#4\n"):
            with self.subTest(body=body):
                self.assertEqual(self.choose(body), {"repository": KIT, "ref": "main", "pull-request": ""})
        self.assertEqual(self.choose(None, default_ref=""), {"repository": KIT, "ref": "", "pull-request": ""})

    def test_an_open_pairing_builds_its_head_commit_in_its_head_repository(self):
        reads = []
        def read(number):
            reads.append(number)
            return open_pull_request()
        chosen = self.choose("Change.\n\nPairs-With: ubiquitty/kit#123\n", read)
        self.assertEqual(chosen, {"repository": "someone/kit", "ref": HEAD, "pull-request": "123"})
        self.assertEqual(reads, [123])
        # GitHub stores a description written in its editor with CRLF endings.
        self.assertEqual(self.choose("Change.\r\n\r\nPairs-With: ubiquitty/kit#123\r\n", read)["pull-request"], "123")
        # The repository is matched without regard to case, as GitHub does.
        self.assertEqual(self.choose("Pairs-With: Ubiquitty/Kit#123", read)["pull-request"], "123")

    def test_a_merged_pairing_builds_main_and_a_closed_one_refuses(self):
        merged = dict(open_pull_request(), state="closed", merged_at="2026-11-10T00:00:00Z")
        self.assertEqual(self.choose("Pairs-With: ubiquitty/kit#5", lambda number: merged, default_ref=""),
                         {"repository": KIT, "ref": "main", "pull-request": "5"})
        closed = dict(open_pull_request(), state="closed")
        with self.assertRaises(pairing.PairingError):
            self.choose("Pairs-With: ubiquitty/kit#5", lambda number: closed)

    def test_only_a_whole_line_of_the_exact_shape_pairs(self):
        for body in ("see Pairs-With: ubiquitty/kit#5", "Pairs-With: ubiquitty/kit#5 and #6",
                     "Pairs-With: ubiquitty/kit#05", "Pairs-With: ubiquitty/kit#0",
                     "Pairs-With: ubiquitty/kit#5; rm -rf /", "Pairs-With: ubiquitty/kit#12345678901",
                     "pairs-with: ubiquitty/kit#5"):
            with self.subTest(body=body):
                self.assertEqual(self.choose(body)["pull-request"], "")

    def test_two_different_pairings_refuse_and_a_repeated_one_does_not(self):
        with self.assertRaises(pairing.PairingError):
            self.choose("Pairs-With: ubiquitty/kit#5\nPairs-With: ubiquitty/kit#6\n")
        chosen = self.choose("Pairs-With: ubiquitty/kit#5\nPairs-With: ubiquitty/kit#5\n", lambda number: open_pull_request())
        self.assertEqual(chosen["pull-request"], "5")

    def test_a_head_that_names_nothing_buildable_refuses(self):
        for paired in (dict(open_pull_request(), head={"repo": None, "sha": HEAD}),
                       open_pull_request(repository="someone/kit; echo"),
                       open_pull_request(sha="main"),
                       open_pull_request(sha=HEAD + "\nref=evil")):
            with self.subTest(paired=paired):
                with self.assertRaises(pairing.PairingError):
                    self.choose("Pairs-With: ubiquitty/kit#5", lambda number, paired=paired: paired)


if __name__ == "__main__":
    sys.exit(unittest.main())
