#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0
"""check-findings-on-board.py — a finding is tracked on the board, not in a doc.

A sweep's findings are board items, each carrying its own evidence, verdict and
resolution. The board answers "what is left" by being queried; a catalogue in the
tree answers it by being kept in step by hand, and needs a second file listing
which of its entries are done.

Prose is not a catalogue. A design doc citing a finding by number is how a reader
follows a decision back to its cause. So the rule is a threshold: a handful of
references is a document that mentions findings, and a file carrying THRESHOLD or
more numbered finding headings — or that many burn-down checkboxes — is a
document that has become the tracker.

Stdlib only.

    python3 Scripts/check-findings-on-board.py
    python3 Scripts/check-findings-on-board.py --self-test
"""

import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gate_report  # noqa: E402 — the path above is what makes it importable

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ubq_roots  # noqa: E402
REPO = ubq_roots.subject_of(__file__)
DOCS = os.path.join(REPO, "Docs")

# Below this a file is citing findings; at or above it the file is the list.
THRESHOLD = 5

# `## Finding 12 — …` and `### Finding 12 — …`. The number is what makes it a
# catalogue entry rather than a section about findings in general.
HEADING = re.compile(r"^#{2,4} Finding \d+\b", re.M)

# `- [ ] finding 12 (small) …` and its ticked form: a burn-down row.
BURNDOWN = re.compile(r"^\s*- \[[ xX]\] finding \d+\b", re.M)


def scan(text):
    """The verdict for one document.

    - Parameters:
      - text: The document's full contents.
    - Returns: `None` when the file is fine, otherwise a reason to print.
    """
    headings = len(HEADING.findall(text))
    if headings >= THRESHOLD:
        return "%d numbered finding headings — this file is a catalogue" % headings

    boxes = len(BURNDOWN.findall(text))
    if boxes >= THRESHOLD:
        return "%d burn-down checkboxes — this file is a burn-down list" % boxes

    return None


def check(documents):
    """Applies `scan` to every document.

    - Parameters:
      - documents: `(path, text)` pairs, paths relative to the repository root.
    - Returns: `(offenders, examined)`.
    """
    offenders = []
    for path, text in documents:
        reason = scan(text)
        if reason is not None:
            offenders.append("%s: %s" % (path, reason))
    return offenders, len(documents)


def _self_test():
    """The rule against documents it must catch and one it must not."""
    cases = (
        ("\n".join("## Finding %d — something" % n for n in range(1, 9)),
         True, "a catalogue"),
        ("\n".join("- [ ] finding %d (small) something" % n for n in range(1, 9)),
         True, "a burn-down"),
        ("Discussed under finding 30 and finding 84.\n\n"
         "## Finding 30 — the one this doc is about\n",
         False, "a doc citing findings"),
    )

    failures = []
    for text, must_catch, what in cases:
        caught = scan(text) is not None
        if caught != must_catch:
            failures.append("%s was %s" % (what, "missed" if must_catch else "wrongly caught"))

    if failures:
        return gate_report.failed("check-findings-on-board --self-test",
                                  "self-test case(s) wrong", failures)

    print("check-findings-on-board: self-test ok (%d case(s))" % len(cases))
    return 0


def main():
    if "--self-test" in sys.argv[1:]:
        return _self_test()

    if not os.path.isdir(DOCS):
        print("check-findings-on-board: FAILED — no Docs/ directory at %s, so this "
              "examined nothing." % DOCS, file=sys.stderr)
        return 1

    documents = []
    for root, _dirs, files in os.walk(DOCS):
        for name in sorted(files):
            if not name.endswith(".md"):
                continue
            full = os.path.join(root, name)
            with open(full, encoding="utf-8") as handle:
                documents.append((os.path.relpath(full, REPO), handle.read()))

    if not documents:
        print("check-findings-on-board: FAILED — no Markdown under Docs/, so this "
              "examined nothing.", file=sys.stderr)
        return 1

    offenders, examined = check(documents)

    if offenders:
        status = gate_report.failed("check-findings-on-board",
                                    "document(s) tracking findings in the tree",
                                    offenders)
        print("\n  Findings are board items. Open GitHub issues with\n"
              "  evidence, scope, and a completion criterion for each. Citing a\n"
              "  finding is fine — under %d headings passes. What this rejects is a\n"
              "  file that has become the list." % THRESHOLD, file=sys.stderr)
        return status

    print("check-findings-on-board: ok (%d document(s), none tracking findings in "
          "the tree)" % examined)
    return 0


if __name__ == "__main__":
    sys.exit(main())
