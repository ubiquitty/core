#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""check-aggregate-cases.py — one registered case must not hide many tests.

A case whose body is a list of calls to other test functions registers one case
for all of them, and the registry exists to prevent exactly that. `Harness.h`
gives three reasons for it, and an aggregate defeats all three:

    ctest -R and a name filter match the aggregate, never the function
    --shard cannot partition a single case
    a fatal signal names the aggregate, so a crash names nothing

The third is not hypothetical. `test_display.cpp` registered one case for 131
functions, and probing an unrelated question crashed it into
`test-harness: FATAL — signal in case displayCases`.

What counts as one. A call to a same-file `void name()` whose own body contains
an assertion. The callee asserting is the whole discriminator: a case calling
`evReset` twice is calling a reset helper, and a raw call count cannot tell the
two apart. Measured across Tests/ while the last of these were being split:

    cases calling 0 asserting same-file helpers   2693
                  1                                  1
                  2 or more                          6   every one an aggregate

So the threshold is two, and the gap under it is one case wide. A case that
calls a single asserting helper is left alone — that is a test delegating a
shared check, not a list standing in for a registry.

What this does not see. A helper taking arguments, and a body that inlines its
scenarios instead of calling out to them. Both were present in the twenty-six
found by hand, and neither is a call this can count. It holds a line rather than
finding every instance, which is what it is for: the line was reached once, by
reading, and nothing here is going to re-derive that.
"""

import os
import re
import sys

import gate_report

CASE = re.compile(r'\bTEST\(\s*(\w+)\s*\)\s*\n\s*\{')
BARE_CALL = re.compile(r'^\s*(\w+)\(\);\s*$', re.M)
ASSERTION = 'CHECK'

SKIP_DIRECTORIES = {"Snapshots", "Fixtures"}
THRESHOLD = 2
GATE = "check-aggregate-cases"


def body_after(text, index):
    """The braced block starting at or after `index`, without its braces."""
    start = text.find('{', index)
    if start < 0:
        return ''
    depth = 0
    for i in range(start, len(text)):
        if text[i] == '{':
            depth += 1
        elif text[i] == '}':
            depth -= 1
            if depth == 0:
                return text[start + 1:i]
    return ''


def asserting_callees(text, body):
    """Same-file `void name()` callees whose own body asserts."""
    found = set()
    for name in set(BARE_CALL.findall(body)):
        definition = re.search(
            r'(?:^|\n)(?:static\s+)?void\s+%s\s*\(\s*\)\s*\n?\s*\{' % re.escape(name), text)
        if definition and ASSERTION in body_after(text, definition.end() - 1):
            found.add(name)
    return sorted(found)


def sources(root):
    for base, directories, names in os.walk(os.path.join(root, "Tests")):
        directories[:] = [d for d in directories if d not in SKIP_DIRECTORIES]
        for name in sorted(names):
            if name.endswith(".cpp"):
                yield os.path.join(base, name)


def scan(root):
    """Returns the number of sources and every aggregate registry case."""
    scanned, findings = 0, []
    for path in sources(root):
        scanned += 1
        with open(path, "r", encoding="utf-8", errors="replace") as handle:
            text = handle.read()
        for match in CASE.finditer(text):
            callees = asserting_callees(text, body_after(text, match.end() - 1))
            if len(callees) >= THRESHOLD:
                findings.append((os.path.relpath(path, root),
                                 text.count('\n', 0, match.start()) + 1,
                                 match.group(1), callees))
    return scanned, findings


def main(argv):
    root = os.path.abspath(argv[1] if len(argv) > 1 and not argv[1].startswith("--") else ".")
    scanned, findings = scan(root)

    if not scanned:
        return gate_report.failed(GATE, "no test sources found under Tests/")

    for relative, line, name, callees in sorted(findings):
        gate_report.finding(
            f"{relative}:{line}", GATE,
            f"TEST({name}) registers one case for {len(callees)} test functions")
        for callee in callees[:6]:
            print("      %s" % callee, file=sys.stderr)
        if len(callees) > 6:
            print("      … and %d more" % (len(callees) - 6), file=sys.stderr)

    if findings:
        return gate_report.failed_count(GATE, len(findings),
                                        "case(s) stand in for the registry")

    return gate_report.ok(GATE, f"{scanned} test sources, no aggregate cases")


if __name__ == "__main__":
    sys.exit(main(sys.argv))
