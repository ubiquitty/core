#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""check-acronym-rules.py — every naming rule is reachable and distinguishable.

Scripts/acronym_rules.py carries the rules and Scripts/acronym_scan.py asks a
cheap literal pass before asking any of them. Both can go wrong silently: a rule
whose literal is missing from that pass is never asked, and a rule no line
distinguishes is one no test would notice losing. Neither shows on a clean tree,
where every rule prints nothing.

So this synthesises a line that trips each rule and asserts four things: the
corpus is not empty, every rule has an example, the first pass admits every line
a rule reports, and dropping any one rule changes what is reported.

This began as a differential against the perl scanner the shell gate drove.
That comparison retired with the perl -- there is no second implementation to
compare against, and keeping the name while the subject disappeared would leave
a gate that runs, prints, and means nothing.

    Scripts/check-acronym-rules.py
    Scripts/check-acronym-rules.py --corpus   # print the synthesised corpus

Exit 0 = the rules are all reachable and distinguishable, 1 = they are not.
"""


import os
import re
import pathlib
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import acronym_rules
from acronym_rules import RULES, may_trip

GATE = "check-acronym-rules"

# Shapes an identifier takes in this tree. A rule keys on what surrounds its
# token — a capital, a word end, a digit, a lowercase lead — so a token is tried
# in each position rather than in one.
TEMPLATES = (
    "int {t}Value = 0;",
    "int x{t} = 0;",
    "int x{t}Value = 0;",
    "void f(int {t}) {{ }}",
    "int {t} = 0;",
    "int {t}8 = 0;",
    "int x{t}8Value = 0;",
    "class {t} {{ }};",
    "int {t}s = 0;",
    "int x{t}s = 0;",
    "uint32_t {t} = 0;",
    "return obj.{t};",
    # The lowercase-prefix rules (fg/bg/ul/cur) key on a capital right after the
    # token, and the colour-channel rule on a whole identifier ending in one.
    "int {t}Color = 0;",
    "int color{t} = 0;",
    "int foreground{t} = 0;",
    # `cp` is only a codepoint where the line says so, and only where it is not
    # a hex literal or arithmetic.
    "uint32_t {t} = value;",
    "int x{t}oint = 0;",
)

# The token each rule is about, read out of its own message: the messages are
# written as "Gpu → GPU" or "[name]Idx → [name]Index". The first run of letters
# is the token, and the run is taken whole -- a pattern led by `[a-z]*`
# swallows `fg` and leaves the rule to be tested as `g`.
TOKEN = re.compile(r"[A-Za-z][A-Za-z0-9]*")


def token_for(message):
    """The identifier fragment a rule's message names."""
    head = message.split("→")[0].split("->")[0].strip()
    head = head.replace("[name]", "").replace("[colour]", "").replace("[X]", "")
    head = head.replace("[N]", "8").replace("(um)", "")
    match = TOKEN.search(head)
    return match.group(0) if match else None


def corpus():
    """One line per rule that the rule reports, and the rules with none.

    - Returns: (lines, uncovered) where lines is a list of source lines and
      uncovered names the rules no template could trip.
    """
    lines = []
    uncovered = []
    for message, trips in RULES:
        token = token_for(message)
        found = None
        for candidate in ({token, token.lower(), token.upper()} if token else ()):
            if not candidate:
                continue
            for template in TEMPLATES:
                line = template.format(t=candidate)
                if trips(line):
                    found = line
                    break
            if found:
                break
        if found:
            lines.append(found)
        else:
            uncovered.append(message)
    return lines, uncovered


def _self_test():
    """The comparison would report a difference if there were one.

    Two scanners that agree prove nothing about the comparison between them, so
    each claim it rests on is asserted here: the corpus trips rules rather than
    being empty, every rule has an example, the first pass admits every line a
    rule reports, and a rule removed from the set changes what is reported.

    - Returns: 0 when the machinery can fail, 1 when it cannot.
    """
    failures = []

    def check(claim, held):
        print(f"  {'ok      ' if held else 'BROKEN  '} {claim}")
        if not held:
            failures.append(claim)

    lines, uncovered = corpus()
    check("the corpus trips at least one rule", bool(lines))
    check("every rule has an example", not uncovered)
    check("the first pass admits every line a rule reports",
          all(may_trip(line) for line in lines))

    # What each line reports, as the comparison would see it.
    def reported(rules):
        seen = []
        for line in lines:
            for message, trips in rules:
                if trips(line):
                    seen.append((line, message))
        return seen

    full = reported(RULES)
    check("the corpus reports something to compare", bool(full))

    # A scanner missing a rule reports less. If dropping one changed nothing,
    # the corpus would not be exercising that rule and the comparison would pass
    # over it in silence.
    blind = []
    for index in range(len(RULES)):
        thinner = RULES[:index] + RULES[index + 1:]
        if reported(thinner) == full:
            blind.append(RULES[index][0])
    check("dropping any one rule changes what is reported", not blind)
    for message in blind:
        print(f"             no line distinguishes: {message}", file=sys.stderr)

    if failures:
        print(f"{GATE} --self-test: {len(failures)} claim(s) the comparison "
              f"does not keep", file=sys.stderr)
        return 1
    print(f"{GATE} --self-test: ok (the comparison can fail: {len(lines)} "
          f"line(s), each rule distinguished by one of them)")
    return 0


def main(argv):
    if argv and argv[0] in ("--help", "-h"):
        print(__doc__.strip())
        return 0
    if argv and argv[0] == "--corpus":
        lines, uncovered = corpus()
        for line in lines:
            print(line)
        for message in uncovered:
            print(f"# UNCOVERED {message}", file=sys.stderr)
        return 0
    return _self_test()


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
