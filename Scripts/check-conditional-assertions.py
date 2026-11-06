#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""check-conditional-assertions.py — cases that assert nothing on some path.

A case whose assertions all sit inside an `if` reports a pass on every run where
the condition is false. It reads as covered from every angle except execution:
it has assertions, they are about the right subject, and they are never
reached.

    const ClearToMarkOutcome outcome = clearToPreviousMark(t, ...);
    if (outcome == ClearToMarkOutcome::ClearedToMark) {
        CHECK_MSG(target.sent.empty(), ...);
        CHECK_MSG(target.viewResets == 1, ...);
    }

When the outcome is the other value on every run, a case named for the mark
path exercises only the fallback, and an off-by-one in the caller it is meant
to cover goes unseen.

Why a detector and not a GREP. The same shape is often written with an escape
hatch that announces itself:

    if (n == 0) { printf("XFAIL: ...not implemented\\n"); return; }
    CHECK(...);

Grepping XFAIL finds those only because someone chose to print a marker. The
defect is the unasserted path, and that has no
keyword. This looks for the shape.

Two shapes, REPORTED SEPARATELY, because they differ in how often they are wrong:

  every-assertion-conditional  The case has no assertion outside a conditional,
                               so some path through it asserts nothing at all.

  branch-asserts-nothing       An if/else where one branch asserts and the other
                               runs statements without asserting. The else is
                               reachable by construction -- it was written -- and
                               nothing there is checked.

What this does not claim. A conditional is not automatically a defect: a case may
branch on a platform, a capability the build does not carry, or a fixture that
legitimately models one arm. The output is a list to read, not a verdict, which
is why the default exit status is 0. `--gate` makes it fail, for a tree that has
been through the list once and wants to hold the line.
"""

import os
import re
import sys

ASSERTION = re.compile(r'\bCHECK\w*\s*\(')
# `if (wrong) { CHECK_MSG(false, "why"); }` is CHECK_MSG(!wrong, "why") written
# out. The branch is the failure arm rather than a path that skips checking, so
# the conditional around it is an assertion and not a gap.
NEGATIVE_ASSERTION = re.compile(r'\bCHECK\w*\s*\(\s*(?:false|0)\s*[,)]')
# An early return inside a conditional ends the case, so the assertions BELOW it
# are the ones that go unreached -- the escape-hatch shape. Tracked separately
# because the branch itself is bare by design.
BARE_RETURN = re.compile(r'\breturn\b\s*;')

CASE_START = re.compile(
    r'^(?:TEST\(\s*(\w+)\s*\)|(?:static\s+)?void\s+(\w*[Tt]est\w*)\s*\([^;{]*\))\s*$',
    re.M)

SKIP_DIRECTORIES = {"Snapshots", "Fixtures"}


def strip_noise(text):
    """Blanks comments and literal contents, preserving length and newlines.

    Brace counting is the whole mechanism here, so a `{` inside a string or a
    comment has to stop existing before any of it runs. Length is preserved so
    every offset still maps to the original line.
    """
    out = list(text)
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == '/' and i + 1 < n and text[i + 1] == '/':
            while i < n and text[i] != '\n':
                out[i] = ' '
                i += 1
        elif c == '/' and i + 1 < n and text[i + 1] == '*':
            out[i] = out[i + 1] = ' '
            i += 2
            while i + 1 < n and not (text[i] == '*' and text[i + 1] == '/'):
                if text[i] != '\n':
                    out[i] = ' '
                i += 1
            if i + 1 < n:
                out[i] = out[i + 1] = ' '
                i += 2
        elif c in '"\'':
            quote = c
            i += 1
            while i < n and text[i] != quote:
                if text[i] == '\\':
                    out[i] = ' '
                    i += 1
                    if i < n:
                        out[i] = ' '
                        i += 1
                    continue
                if text[i] != '\n':
                    out[i] = ' '
                i += 1
            if i < n:
                i += 1
        else:
            i += 1
    return ''.join(out)


def matching_brace(text, start):
    """Index just past the `}` closing the `{` at `start`, or None."""
    depth = 0
    for i in range(start, len(text)):
        if text[i] == '{':
            depth += 1
        elif text[i] == '}':
            depth -= 1
            if depth == 0:
                return i + 1
    return None


def case_bodies(clean):
    """Yields (name, body start index, body end index) for each case."""
    for match in CASE_START.finditer(clean):
        name = match.group(1) or match.group(2)
        brace = clean.find('{', match.end())
        if brace < 0:
            continue
        end = matching_brace(clean, brace)
        if end:
            yield name, brace, end


def conditionals(clean, start, end):
    """Yields (if-body span, else-body span or None) inside [start, end).

    Outermost only. A conditional nested inside another is already accounted for
    by the branch that holds it, and counting both would report one assertion
    twice — which reads as a case having more coverage under a condition than it
    owns outright.
    """
    covered = 0
    for match in re.finditer(r'\bif\s*\(', clean[start:end]):
        head = start + match.start()
        if head < covered:
            continue
        paren = clean.find('(', head)
        depth, i = 0, paren
        while i < end:
            if clean[i] == '(':
                depth += 1
            elif clean[i] == ')':
                depth -= 1
                if depth == 0:
                    break
            i += 1
        body = i + 1
        while body < end and clean[body] in ' \t\n':
            body += 1
        if body >= end or clean[body] != '{':
            continue  # a single-statement branch holds no assertion block
        stop = matching_brace(clean, body)
        if not stop or stop > end:
            continue
        after = stop
        while after < end and clean[after] in ' \t\n':
            after += 1
        other = None
        if clean.startswith('else', after):
            tail = after + 4
            while tail < end and clean[tail] in ' \t\n':
                tail += 1
            if tail < end and clean[tail] == '{':
                shut = matching_brace(clean, tail)
                if shut and shut <= end:
                    other = (tail, shut)
        covered = other[1] if other else stop
        yield (body, stop), other


def line_of(text, index):
    return text.count('\n', 0, index) + 1


def examine(path, root):
    """Returns (every-assertion-conditional findings, bare-branch findings)."""
    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        raw = handle.read()
    clean = strip_noise(raw)
    relative = os.path.relpath(path, root)

    conditional_only, bare_branch = [], []
    for name, start, end in case_bodies(clean):
        body = clean[start:end]
        total = len(ASSERTION.findall(body))
        if total == 0:
            continue  # a case with no assertions at all is a different finding

        # Assertions the case owns on every path: the ones outside any
        # conditional, plus the ones under a conditional that checks both ways.
        # An if/else asserting in each branch is total over its condition — there
        # is no path through it that skips checking — so its assertions are not
        # conditional in the sense this looks for.
        conditioned = 0
        for (ifStart, ifStop), other in conditionals(clean, start, end):
            ifBody = clean[ifStart:ifStop]
            ifCount = len(ASSERTION.findall(ifBody))
            elseCount = 0
            elseBody = ""
            if other:
                elseBody = clean[other[0]:other[1]]
                elseCount = len(ASSERTION.findall(elseBody))
            total_over_condition = (ifCount and elseCount) \
                or NEGATIVE_ASSERTION.search(ifBody) or NEGATIVE_ASSERTION.search(elseBody)
            if not total_over_condition:
                conditioned += ifCount + elseCount

            if NEGATIVE_ASSERTION.search(ifBody) or NEGATIVE_ASSERTION.search(elseBody):
                continue
            # One branch checks, the other runs and checks nothing. An empty else
            # is someone writing the shape out; a populated one is a path that
            # does work nobody looks at.
            if ifCount and not elseCount and elseBody.strip(' \n\t{}'):
                bare_branch.append((relative, line_of(clean, other[0]), name,
                                    "else branch runs statements and asserts nothing"))
            elif elseCount and not ifCount and ifBody.strip(' \n\t{}'):
                bare_branch.append((relative, line_of(clean, ifStart), name,
                                    "if branch runs statements and asserts nothing"))

        # Every assertion the case owns sits under a condition that can go the
        # other way, so the case has a path that asserts nothing. An early
        # `return` in a branch is the escape-hatch spelling of the same thing.
        if conditioned == total:
            hatch = " (escape hatch: a branch returns)" if BARE_RETURN.search(body) else ""
            conditional_only.append((relative, line_of(clean, start), name,
                                     "all %d assertion(s) sit inside a conditional%s" % (total, hatch)))
    return conditional_only, bare_branch


def sources(root):
    for base, directories, names in os.walk(os.path.join(root, "Tests")):
        directories[:] = [d for d in directories if d not in SKIP_DIRECTORIES]
        for name in sorted(names):
            if name.endswith(".cpp"):
                yield os.path.join(base, name)


def main(argv):
    root = os.path.abspath(argv[1] if len(argv) > 1 and not argv[1].startswith("--") else ".")
    gate = "--gate" in argv

    conditional_only, bare_branch = [], []
    scanned = 0
    for path in sources(root):
        scanned += 1
        a, b = examine(path, root)
        conditional_only.extend(a)
        bare_branch.extend(b)

    if not scanned:
        print("check-conditional-assertions: no test sources found under Tests/ -", file=sys.stderr)
        print("  the walk matched nothing, which is not the same as nothing being wrong.",
              file=sys.stderr)
        return 2

    for title, findings in (("every assertion inside a conditional", conditional_only),
                            ("a branch that asserts nothing", bare_branch)):
        if not findings:
            continue
        print("\n%s — %d:" % (title, len(findings)))
        for relative, line, name, why in sorted(findings):
            print("  %s:%d  %s" % (relative, line, name))
            print("      %s" % why)

    total = len(conditional_only) + len(bare_branch)
    print("\ncheck-conditional-assertions: %d case(s) across %d file(s) have a path that "
          "asserts nothing." % (total, scanned))
    if not total:
        return 0
    print("  Each is a case that reports a pass without checking anything on that path.")
    print("  Read them: a branch on a platform or an unbuilt capability is legitimate,")
    print("  and a branch on the behaviour under test is the defect this looks for.")
    return 1 if gate else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
