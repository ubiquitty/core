#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""check-ledger-ratchet — a ledger budget rises only with its reason.

A gate holds its count to the ledger two-sided: a new offender fails it, and
one removed must lower the budget in the same commit. Nothing held the budget
itself. A commit that added an offender could raise the budget beside it, and
a reviewer was the only check between the two. This gate reads each
`Scripts/gate-ledger.*.txt` at the merge base of HEAD and origin/main, and
holds every budget the merge base names to that number or lower.

A budget rises only through a line the branch adds:

    raised <budget> <from> <to> <reason>

<from> is the merge base's number and <to> the branch's, so the rise and its
reason stand in the ledger for the reviewer, and in history after it lands. A
`raised` line the merge base already holds is history and is not judged again.
A budget the merge base does not name is new, and starts at its count.

    ./Scripts/check-ledger-ratchet.py
    ./Scripts/check-ledger-ratchet.py --self-test
"""

import fnmatch
import os
import pathlib
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gate_framework import Gate
import ubq_roots

#: The branch every budget is compared against: its merge base with HEAD.
REFERENCE = "origin/main"


def parse(texts):
    """({budget: number}, [(budget, from, to, reason, line)]) from ledger texts.

    A checkout holding several repositories sums their budgets, as the ledger
    reader does.
    """
    budgets = {}
    raised = []
    for text in texts:
        for line in text.splitlines():
            words = line.split("#", 1)[0].split()
            if len(words) == 3 and words[0] == "budget":
                budgets[words[1]] = budgets.get(words[1], 0) + int(words[2])
            elif len(words) >= 5 and words[0] == "raised":
                raised.append((words[1], int(words[2]), int(words[3]), " ".join(words[4:]), " ".join(words)))
    return budgets, raised


def judge(base_texts, texts):
    """The findings for the ledgers at the merge base and on the branch."""
    base_budgets, base_raised = parse(base_texts)
    budgets, raised = parse(texts)
    inherited = {line for *_, line in base_raised}
    findings = []
    accepted = {}
    for name, before, after, reason, line in raised:
        if line in inherited:
            continue
        if name not in base_budgets:
            findings.append(f"`raised {name}`: the merge base names no budget `{name}`, so it starts at its count and needs no raise")
        elif before != base_budgets[name]:
            findings.append(f"`raised {name}` from {before}: the merge base's budget is {base_budgets[name]}")
        elif budgets.get(name) != after:
            findings.append(f"`raised {name}` to {after}: the budget is {budgets.get(name)}")
        elif after <= before:
            findings.append(f"`raised {name}` from {before} to {after} raises nothing; delete the line")
        else:
            accepted[name] = after
    for name, number in sorted(budgets.items()):
        before = base_budgets.get(name)
        if before is not None and number > before and accepted.get(name) != number:
            findings.append(f"budget `{name}` rose from {before} to {number} with no `raised {name} {before} {number} <reason>` line")
    return findings, {"budgets": len(budgets), "compared": len(set(budgets) & set(base_budgets)), "raised": len(accepted)}


def git(root, *arguments):
    """The command's standard output, or None when it fails."""
    finished = subprocess.run(["git", *arguments], cwd=root, capture_output=True, text=True)
    return finished.stdout if finished.returncode == 0 else None


class LedgerRatchet(Gate):
    name = "check-ledger-ratchet"
    subject = "the ledgers under Scripts/"

    def examine(self):
        root = pathlib.Path(self.root)
        base = git(root, "merge-base", "HEAD", REFERENCE)
        if not base:
            return self.skipped(f"no merge base with {REFERENCE} to compare the ledgers against")
        base = base.strip()
        listing = git(root, "ls-tree", "--name-only", base, "Scripts/") or ""
        base_texts = []
        for path in listing.split():
            if fnmatch.fnmatch(os.path.basename(path), ubq_roots.LEDGER_PATTERN):
                text = git(root, "show", f"{base}:{path}")
                if text is not None:
                    base_texts.append(text)
        texts = [path.read_text(encoding="utf-8") for path in sorted((root / "Scripts").glob(ubq_roots.LEDGER_PATTERN))]
        findings, counts = judge(base_texts, texts)
        if findings:
            for finding in findings:
                self.finding("Scripts", finding)
            return self.failed("ledger budget(s) raised without their reason", findings)
        return self.ok(f"{counts['compared']} of {counts['budgets']} budget(s) held to the merge base's, {counts['raised']} raised"
                       " with a reason")

    def selfTest(self):
        """Each shape, planted against a base the clean branch passes, fails it."""
        base = "budget holds 5\nbudget escapes 8\nraised holds 2 5 an earlier rise, already landed\n"
        clean = [
            ("a budget that fell", "budget holds 4\nbudget escapes 8\nraised holds 2 5 an earlier rise, already landed\n"),
            ("a rise with its reason", "budget holds 9\nbudget escapes 8\nraised holds 5 9 counts a second file\n"
                                       "raised holds 2 5 an earlier rise, already landed\n"),
            ("a new budget", "budget holds 5\nbudget escapes 8\nbudget waits 3\n"),
        ]
        planted = [
            ("a rise with no reason", "budget holds 6\nbudget escapes 8\n", "rose from 5 to 6"),
            ("a reason from the wrong number", "budget holds 6\nbudget escapes 8\nraised holds 4 6 counts more\n", "the merge base's budget is 5"),
            ("a reason to the wrong number", "budget holds 6\nbudget escapes 8\nraised holds 5 7 counts more\n", "the budget is 6"),
            ("a reason for a new budget", "budget holds 5\nbudget escapes 8\nbudget waits 3\nraised waits 0 3 new\n", "names no budget `waits`"),
            ("a rise paid for by a fall elsewhere", "budget holds 6\nbudget escapes 7\n", "rose from 5 to 6"),
        ]
        failures = []
        for name, text in clean:
            findings, _ = judge([base], [text])
            if findings:
                failures.append(f"{name} fails: {findings[0]}")
        for name, text, expected in planted:
            findings, _ = judge([base], [text])
            if not any(expected in finding for finding in findings):
                failures.append(f"{name} planted, and no finding names `{expected}` ({'; '.join(findings) or 'none'})")
        if failures:
            return self.failed("self-test finding(s)", failures)
        return self.ok(f"each of {len(planted)} planted shape(s) fails, and {len(clean)} clean ledger(s) pass")


if __name__ == "__main__":
    raise SystemExit(LedgerRatchet.main())
