#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0
"""run-gate-self-tests.py — every gate that advertises a self-test runs it.

A gate here reports nothing on a clean tree, which is the same output it would
produce if its rule had stopped matching anything at all. That is why so many of
them carry a `--self-test`: the self-test runs the rule against a construct it
must catch and one it must not, so a clean verdict is evidence rather than an
absence. `check-empty-array-expansion` says it outright — the self-test is what
"settles which".

A self-test nothing executes is unwatched rather than rotten, and the
difference cannot be seen until it runs. A control nobody runs is not evidence
that it works, and the day one of them stops catching its own planted defect is
the day it stops being worth its lines — silently, because the gate it guards
still prints ok.

Discovered rather than listed. A per-gate stanza lets unrun self-tests
accumulate: adding a gate gives nobody a reason to edit a list of gates, so
each omission is invisible on its own and the total is never counted. Anything
named `check-*` that mentions `--self-test` in its own text is run here, so a
new gate is covered without anyone remembering to add it.

Discovery that finds nothing must fail rather than pass quietly, which is the
same defect this file exists to close one level up: a driver reporting "0 of 0
self-tests passed" reads exactly like a clean run. The canaries below are the
floor — gates whose self-tests `run-tests.sh` already depends on, so a pattern
that stops matching them is a broken scan and not a shrinking tree.

Stdlib only.

    python3 Scripts/run-gate-self-tests.py
    python3 Scripts/run-gate-self-tests.py --list
    python3 Scripts/run-gate-self-tests.py --self-test
"""

import os
from concurrent.futures import ThreadPoolExecutor
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import gate_report  # noqa: E402 — the path above is what makes it importable
import ubq_roots  # noqa: E402

# The gates judged are the subject's own: the repository this runs in, which is
# not the checkout holding this file when ubiquitty-core's copy runs for another
# repository.
REPO = ubq_roots.subject_root()


# Both spellings in the tree, and the hyphen is not the majority by enough to
# call the other a typo: 55 uses of `--self-test`, 5 of `--selftest`. A scan for
# the common one alone silently drops the gates using the other, and its count
# of gates, every one passing, says nothing about the ones it never ran.
SELFTEST_FLAG = re.compile(r"--self[-_]?tests?\b")


def advertises(path):
    """The self-test flag a script offers, or None.

    Read from the file rather than probed by running it with the flag: a gate
    that does not know the flag may treat it as a path argument and do real work.

    Returns the spelling found, because it is the one the gate parses. Both
    `--selftest` gates here reject the other spelling with an argparse usage
    error and exit 2, so normalising would fail loudly rather than quietly —
    measured, not assumed. It is still the wrong thing to send: a gate parsing
    its own arguments by hand has no such backstop, and this file cannot tell
    which does from the outside.
    """
    try:
        with open(path, encoding="utf-8", errors="replace") as handle:
            match = SELFTEST_FLAG.search(handle.read())
    except OSError:
        return None
    return match.group(0) if match else None


# Gates carrying no self-test at all. Discovery above reports on the ones that
# advertise one, so a gate with none is not reported as unverified -- it is not
# reported at all, which reads the same as a tree with nothing to say. That is
# the defect this file closes, one level further out: the argument
# in the header for why an unwatched control is not evidence applies with more
# force to a control that was never written.
#
# A number rather than a list of exempt gates. A list is a standing exemption for
# every name on it and grows by one edit nobody has to justify; a count that may
# only fall cannot exempt anything, and the commit that adds a self-test has to
# write the smaller number down. Each repository keeps its number in its own
# ledger, Scripts/gate-ledger.<repository>.txt.
#
# Two-sided on purpose. Over the budget means a gate arrived without a self-test.
# Under it means one was written and the budget was not lowered, and a budget
# with slack in it is one that cannot report the next arrival.


def gates():
    """Every gate this repository ships, whether or not it advertises a self-test.

    - Returns: the sorted file names of the `check-*` scripts under Scripts/.
    """
    scripts = os.path.join(REPO, "Scripts")
    return sorted(name for name in os.listdir(scripts)
                  if name.startswith("check-")
                  and name.endswith((".py", ".sh"))
                  and os.path.isfile(os.path.join(scripts, name)))


def discover():
    """Every gate offering a self-test, as [(name, flagSpelling), ...]."""
    scripts = os.path.join(REPO, "Scripts")
    found = []
    for name in sorted(os.listdir(scripts)):
        if not name.startswith("check-"):
            continue
        if not name.endswith((".py", ".sh")):
            continue
        path = os.path.join(scripts, name)
        if not os.path.isfile(path):
            continue
        flag = advertises(path)
        if flag:
            found.append((name, flag))
    return found


# A self-test whose input does not exist on this machine stands down the way
# every gate does for a missing tool: it prints this line and exits 0. The
# style drivers tally the same shape, so a stood-down self-test reaches their
# "PASSED — with N gate(s) stood down" rather than their "every gate ran".
STOOD_DOWN = re.compile(r"^[a-z-]+: SKIPPED", re.M)

OK, SKIPPED, FAILED = "ok", "skipped", "failed"


def classify(returncode, output):
    """The verdict of one self-test, from its exit status and what it printed.

    - Parameters:
      - returncode: The self-test's exit status.
      - output: Its stdout and stderr, in that order.
    - Returns: (OK | SKIPPED | FAILED, detail), where detail is the stood-down
      line for SKIPPED, the whole output for FAILED, and empty for OK.

    Two facts that print alike are kept apart here. "The self-test's input does
    not exist here" is a stood-down line on a zero exit; "the self-test ran and
    missed its planted defect" is a non-zero exit. Only the second is a gate
    that can no longer tell a clean tree from one it stopped reading.
    """
    if returncode != 0:
        return FAILED, output.strip()
    stoodDown = STOOD_DOWN.search(output)
    if stoodDown:
        return SKIPPED, output[stoodDown.start():].split("\n", 1)[0].strip()
    return OK, ""


def lastLine(text):
    """The verdict a gate's self-test printed, for the line that names offenders."""
    lines = [line.strip() for line in text.split("\n") if line.strip()]
    return lines[-1] if lines else "no output"


def run(name, flag):
    path = os.path.join(REPO, "Scripts", name)
    command = ["python3", path, flag] if name.endswith(".py") else [path, flag]
    try:
        done = subprocess.run(command, cwd=REPO, capture_output=True, text=True)
    except OSError as error:
        return FAILED, "could not execute: %s" % error
    # The last non-empty line is where these print their verdict; the rest is the
    # detail a reader needs, so it goes through unabridged on failure.
    return classify(done.returncode, done.stdout + done.stderr)


def selfTest():
    """Proves the three shapes a self-test can print are told apart."""
    cases = (
        (0, "check-x --self-test: ok (3 case(s))\n", OK, ""),
        (0, "check-x: SKIPPED — no objects under build; ./Scripts/build-tests.sh builds them\n",
         SKIPPED, "check-x: SKIPPED — no objects under build; ./Scripts/build-tests.sh builds them"),
        (0, "reading fixtures\ncheck-x: SKIPPED — no c++ on PATH\ntrailing detail\n",
         SKIPPED, "check-x: SKIPPED — no c++ on PATH"),
        (1, "check-x --self-test: planted defect produced no finding\n",
         FAILED, "check-x --self-test: planted defect produced no finding"),
        # A stood-down line on a non-zero exit is still a failure: the exit
        # status is the verdict, and the line is detail.
        (2, "check-x: SKIPPED — no objects\ncheck-x: REFUSED — nothing read\n",
         FAILED, "check-x: SKIPPED — no objects\ncheck-x: REFUSED — nothing read"),
        # Only a line-initial stood-down verdict counts; a gate quoting the
        # word in its passing text has not stood down.
        (0, "check-x --self-test: ok (a SKIPPED fixture is classified)\n", OK, ""),
    )
    failures = []
    for returncode, output, expectedVerdict, expectedDetail in cases:
        verdict, detail = classify(returncode, output)
        if (verdict, detail) != (expectedVerdict, expectedDetail):
            failures.append("exit %d over %r classified as %s %r, expected %s %r"
                            % (returncode, output, verdict, detail,
                               expectedVerdict, expectedDetail))
    for failure in failures:
        print("run-gate-self-tests --self-test: " + failure, file=sys.stderr)
    if failures:
        return 1
    print("run-gate-self-tests --self-test: ok (%d shape(s); a stood-down line on a zero "
          "exit is skipped, a non-zero exit is failed, and only those)" % len(cases))
    return 0


def main():
    found = discover()

    if "--self-test" in sys.argv:
        return selfTest()

    if "--list" in sys.argv:
        for name, flag in found:
            print("%s %s" % (name, flag))
        return 0

    budgets, canaries, _tools = ubq_roots.ledger(REPO)
    uncoveredBudget = budgets.get("gate-self-tests-uncovered")
    if uncoveredBudget is None:
        print("run-gate-self-tests: REFUSED — no Scripts/%s sets gate-self-tests-uncovered; "
              "without it nothing bounds the gates that carry no self-test."
              % ubq_roots.LEDGER_PATTERN, file=sys.stderr)
        return 2
    names = [name for name, _flag in found]
    missing = [c for c in canaries if c not in names]
    if missing:
        print("run-gate-self-tests: FAILED — the scan did not find %s. Every gate it "
              "reports on is unverified until the discovery is fixed; a run that finds "
              "nothing is not a run that passed."
              % ", ".join(missing), file=sys.stderr)
        return 1

    # Each self-test is a separate process that reads its own fixtures and
    # answers about its own rule, so they do not have to wait for each other.
    # Run in order, they were 50 sequential subprocess starts for a check that
    # blocks every push.
    #
    # Results are collected in the discovered order, not the order they finish,
    # so the report a reader sees does not depend on which gate happened to be
    # slow. Half the cores, matching the build scripts: several builds may share the
    # machine.
    workers = max(1, (os.cpu_count() or 4) // 2)
    failures = []
    stoodDown = []
    if len(found) < 4 or workers < 2:
        outcomes = [run(name, flag) for name, flag in found]
    else:
        with ThreadPoolExecutor(max_workers=workers) as pool:
            outcomes = list(pool.map(lambda pair: run(*pair), found))
    for (name, _flag), (verdict, detail) in zip(found, outcomes):
        if verdict == FAILED:
            failures.append((name, detail))
        elif verdict == SKIPPED:
            stoodDown.append((name, detail))

    if failures:
        status = gate_report.failed(
            "run-gate-self-tests",
            "of %d self-test(s) no longer catching their own planted defect" % len(found),
            ["%s: %s" % (name, lastLine(detail)) for name, detail in failures])
        # The verdict line above carries the names; the captured output follows,
        # because a self-test says which of its cases failed and that is what
        # tells a reader whether the gate or its subject moved.
        for name, detail in failures:
            print("\n  --- %s ---\n%s" % (name, detail), file=sys.stderr)
        print("\n  A gate whose self-test fails is not a gate that found a problem in "
              "the tree.\n  It is a gate that can no longer tell a clean tree from one "
              "it stopped reading.", file=sys.stderr)
        return status

    everyGate = gates()
    covered = {name for name, _flag in found}
    uncovered = sorted(name for name in everyGate if name not in covered)

    if len(uncovered) > uncoveredBudget:
        arrived = len(uncovered) - uncoveredBudget
        print("run-gate-self-tests: FAILED — %d gate(s) more than the budget carry no "
              "self-test." % arrived, file=sys.stderr)
        print("  A gate with no self-test is not reported here as unverified; it is "
              "not reported\n  at all, which reads exactly like a gate that passed.",
              file=sys.stderr)
        print("  %d of %d gate(s) carry none (budget %d):"
              % (len(uncovered), len(everyGate), uncoveredBudget), file=sys.stderr)
        for name in uncovered:
            print("      %s" % name, file=sys.stderr)
        return 1

    if len(uncovered) < uncoveredBudget:
        print("run-gate-self-tests: FAILED — %d gate(s) carry no self-test against a "
              "budget of %d." % (len(uncovered), uncoveredBudget), file=sys.stderr)
        print("  Set the budget gate-self-tests-uncovered to %d in the ledger Scripts/gate-ledger.<repository>.txt, in the commit that earned it. "
              "A budget\n  with slack in it cannot report the next gate that arrives "
              "without one." % len(uncovered), file=sys.stderr)
        return 1

    # A stood-down self-test is listed, never folded into "every one passing":
    # its line goes through verbatim so the style drivers count it among the
    # gates a green run does not cover, and the verdict line carries the count.
    if stoodDown:
        for _name, line in stoodDown:
            print(line)
        print("run-gate-self-tests: ok (%d gate self-test(s): %d passing, %d stood down for "
              "a missing input — listed above; %d of %d gate(s) carry none, at budget — "
              "ratchet down, never up)"
              % (len(found), len(found) - len(stoodDown), len(stoodDown),
                 len(uncovered), len(everyGate)))
        return 0

    print("run-gate-self-tests: ok (%d gate self-test(s), every one passing; %d of %d "
          "gate(s) carry none, at budget — ratchet down, never up)"
          % (len(found), len(uncovered), len(everyGate)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
