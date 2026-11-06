#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""check-snapshot-harness.py — run the snapshot catalog's own consistency tests
when the catalog or its loader is staged.

Tests/Snapshots/test_harness.py asserts what nothing else on the commit path
reads: the showcase-vs-cases count balance, the per-frame arm accounting, the
case schema, and the update-launder truth table. check-showcase-goldens pairs an
entry to its golden and stops there; the count and frame invariants live in the
loader (run.py) and the test that mirrors it, and a change to either can leave
them disagreeing: a change that unifies two encoders in the loader and leaves
test_harness.py counting the old number turns the count assertion red where
nothing on the commit path reads it.

It is a ctest (shell_test_snapshot_harness), and UBIQUITTY_PREPUSH=lint runs no
ctest, so a Tests/Snapshots-only change lands with the catalog inconsistent and
CI dark. Run it here instead: staged-scoped to the four files whose agreement it
checks, with no build.
"""

import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import gate_corpus

# The invariant is a property of these files together — the loader, the catalog
# it walks, the test that predicts its output, and the matrix generator that
# reads the same records — so any one of them staged warrants the whole check.
SUBJECTS = re.compile(
    r"^Tests/Snapshots/(cases\.json|run\.py|test_harness\.py|gen_matrix\.py)$")


def main(argv):
    if argv and argv[0] in ("--help", "-h"):
        print(__doc__.strip())
        return 0
    os.chdir(gate_corpus.root())
    # Deletions included: removing the loader a test mirrors breaks their
    # agreement exactly as editing it does.
    subjects = gate_corpus.staged(deletions=True)
    if not any(SUBJECTS.match(path) for path in subjects):
        return 0
    finished = subprocess.run([sys.executable, "-m", "unittest", "test_harness"],
                              cwd=os.path.join(os.getcwd(), "Tests/Snapshots"),
                              check=False)
    return finished.returncode


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
