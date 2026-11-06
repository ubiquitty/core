#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""check-osc-payload-view — the OSC handlers take the payload the dispatcher measured.

`Terminal::parser.osc` is a ByteBuffer holding `length` beside `data`. Two
spellings throw that length away and take a `const char*` that is a C string only
because the buffer happens to be NUL-terminated:

    payload.data()        a handler still declared `const char*`, handed the whole
                          measured payload by its wrapper.
    x.substr(...).data()  a handler declared `std::string_view` that re-derives a
                          C-string suffix and hands that to its callees — the same
                          defect one layer down, at the sub-dispatch.

Each is a budget, two-sided: it cannot rise, and a commit that lowers it must
lower the number here in the same commit. A budget with slack cannot fail, which
is the shape that lets a migration stall while reading as in progress.

Counted across every OSC handler file, not one — a handler that moves to a
sibling file, or peels its pointer under a name other than `payload`, is the
drift a single-file, single-name count reads right past.

The two budgets fail differently when the patterns stop matching, which is why
only one of them needs proving. The peel budget is 8, so a spelling this stops
recognising drops the count to zero, below the budget, and fails — the two-sided
rule catches its own blindness. The whole budget is 0, and a pattern that matches
nothing also counts zero: a finished migration and a broken scan print the same
line. The self-test runs both patterns over a fixture holding one of each, so the
rule reporting absence is shown to still recognise its subject.

    ./Scripts/check-osc-payload-view.py
    ./Scripts/check-osc-payload-view.py --self-test
"""

import os
import pathlib
import re
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gate_framework import Gate

#: The whole measured payload handed on as a bare pointer.
WHOLE = re.compile(r"payload\.data\(\)")

#: A C string re-derived from a view, which is the same defect at the sub-dispatch.
PEEL = re.compile(r"\.substr\([^;]*\)\.data\(\)")


class OSCPayloadView(Gate):
    name = "check-osc-payload-view"
    subject = "the OSC handlers this reads"

    #: The whole-payload pointer is gone, and one peel remains: handleInlineImage
    #: takes a `const char*` and its conversion moves the NUL semantics of a
    #: fuzzed image-decode path, so it is its own change rather than a rider.
    wholeBudget = 0
    peelBudget = 1

    directory = pathlib.Path("Source/Core/OSC")

    def examine(self):
        handlers = pathlib.Path(self.root) / self.directory
        if not handlers.is_dir():
            return self.failed(f"{self.directory} is missing, so nothing was counted")

        whole = self.count(handlers, WHOLE)
        peel = self.count(handlers, PEEL)

        findings = []
        findings += self.against(whole, self.wholeBudget,
            "handler(s) take the whole payload as a pointer (payload.data())")
        findings += self.against(peel, self.peelBudget,
            "site(s) peel a C string from the view (.substr(...).data())")
        if findings:
            for finding in findings:
                self.finding(str(self.directory), finding)
            return self.failed("budget(s) missed", findings)

        return self.ok(f"{whole} whole-payload pointer(s) at budget {self.wholeBudget}, "
                       f"{peel} view-peel(s) at budget {self.peelBudget}")

    @staticmethod
    def count(directory, pattern):
        """How many times `pattern` occurs across the .cpp files under `directory`."""
        found = 0
        for source in sorted(directory.rglob("*.cpp")):
            found += len(pattern.findall(source.read_text(encoding="utf-8", errors="replace")))
        return found

    @staticmethod
    def against(count, budget, what):
        """The finding a two-sided budget yields, which is one per direction and neither at rest."""
        if count > budget:
            return [f"{count} {what} against a budget of {budget}"
                    " — a new one appeared; it does not join the ones still being converted"]
        if count < budget:
            return [f"{count} {what} against a budget of {budget}"
                    f" — lower its budget to {count} in this commit, because a budget with slack"
                    " cannot fail, so leaving it high spends the progress rather than banking it"]
        return []

    def selfTest(self):
        """Each pattern catches its case, and neither fires on a converted handler.

        The whole-payload budget is zero, so a pattern that stopped matching would
        report the same line as a finished migration. This is what tells them
        apart: a fixture holding one of each, then the same fixture with them
        removed.
        """
        failures = []
        with tempfile.TemporaryDirectory() as directory:
            fixture = pathlib.Path(directory)
            (fixture / "whole.cpp").write_text(
                "void h(ByteBuffer payload) { take(payload.data()); }\n", encoding="utf-8")
            (fixture / "peel.cpp").write_text(
                "void h(std::string_view v) { take(v.substr(3).data()); }\n", encoding="utf-8")
            # What a converted handler looks like: the view passed whole, and a
            # substr kept as a view. Counting either would make the budgets fall
            # as handlers convert, which reads as progress and is its opposite.
            (fixture / "clean.cpp").write_text(
                "void h(std::string_view v) { take(v); take(v.substr(3)); }\n", encoding="utf-8")

            whole = self.count(fixture, WHOLE)
            peel = self.count(fixture, PEEL)
            if whole != 1:
                failures.append(f"the whole-payload pattern counted {whole}, not 1")
            if peel != 1:
                failures.append(f"the peel pattern counted {peel}, not 1")

            (fixture / "whole.cpp").unlink()
            (fixture / "peel.cpp").unlink()
            whole = self.count(fixture, WHOLE)
            peel = self.count(fixture, PEEL)
            if whole != 0:
                failures.append(f"a converted handler counted as a whole payload ({whole})")
            if peel != 0:
                failures.append(f"a view-only substr counted as a peel ({peel})")

        if failures:
            return self.failed("self-test finding(s)", failures)
        return self.ok("each pattern catches its case, neither fires on a converted handler")


if __name__ == "__main__":
    raise SystemExit(OSCPayloadView.main())
