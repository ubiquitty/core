#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""check-frontend-default-literals — a frontend must not spell a default Core owns.

`c.ligatures.value_or(true)` in a frontend is a second spelling of
`Defaults::ligatures`, and the two agree only until somebody changes the
default. An `ambiguous_width` written `value_or(1)` by one arm and
`value_or(Defaults::ambiguousWidth)` by another is a width the two arms can
disagree about, and that places every later cell on the row differently.

The rule is narrow on purpose. It fires only where every one of these holds:

  - the file is under Source/Platform, so it is a frontend rather than Core;
  - the field names an `Effective::` accessor, so there is somewhere to route
    to and the routing is a one-line change;
  - the argument is a literal rather than `Defaults::x` or another call, so
    the site is restating a value rather than reading one;
  - the same optional does not guard the call, which would make the argument
    unreachable.

Three shapes read like the defect and are correct. Each has a case in the
selftest below rather than only a sentence here:

  a guarded deref — `if (s.fontSize) f = s.fontSize.value_or(0.0);` — where the
  argument cannot be reached and `value_or` stands in for `*`;

  a frontend's own native default, where the value is not Core's because the
  frontend draws at a different unit: the browser draws at Unifont's 16px cell
  rather than Core's 12pt, and the portable snapshot sizes for its Unifont
  raster. Annotate those `// native-default: <why>`;

  a field with no accessor, which has nowhere to route and is a gap in Core
  rather than a defect in the frontend.
"""

import os
import re
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ubq_roots  # noqa: E402

# The frontends judged are the repository run from; the accessors they must route
# through are Core's, read from wherever the repository finds Core.
ROOT = ubq_roots.subject_of(__file__)
CORE = ubq_roots.core_root(ROOT)
ACCESSORS = "Source/Core/Preferences/PreferencesAccessors.h"
SCOPE = "Source/Platform"
SUFFIXES = (".cpp", ".mm", ".m", ".h")
ANNOTATION = "native-default:"

# `.field.value_or(arg)` with no nested parens in the argument -- a call as the
# argument is already routing through something and is not a restated literal.
CALL = re.compile(r"\.(\w+)\.value_or\(([^()]*)\)")
# What counts as restating a value: a number, a bool in either spelling, or a
# string. Anything qualified (`Defaults::x`, `Effective::x(c)`) is a read.
LITERAL = re.compile(r'^(true|false|YES|NO|-?\d+(\.\d+)?[fF]?|"[^"]*"|\'[^\']*\')$')


def accessorNames(root):
    """The `Effective::` accessors, read from the header rather than listed here.

    A list spelled out in this file would be rewritten by the same tree-wide
    rename that rewrites the code it guards, and the gate would then pass the
    old name and refuse the new one. Deriving them leaves nothing to rewrite.
    """
    path = os.path.join(CORE if root == ROOT else root, ACCESSORS)
    if not os.path.exists(path):
        return set()
    with open(path, encoding="utf-8", errors="replace") as handle:
        return set(re.findall(r"\b(\w+)\(const Values& \w+\)", handle.read()))


def isGuardedByTheSameOptional(lines, index, column, field):
    """Whether the same optional is tested before this call reads it.

    `if (s.fontSize) x = s.fontSize.value_or(0.0);` never reaches the argument,
    so the literal is a deref spelled the safe way rather than a default. Two
    lines back covers the wrapped form without walking scopes, because a guard
    has to be near enough to read as one.

    The ternary spells the same guard without an `if` --
    `s.fontSize ? s.fontSize.value_or(0.0) : fallback` -- and a rule that knows
    only the `if` reports it, while a selftest without this arm passes over a
    shape it never names. The guard must appear before the call, or
    `x.value_or(true) ? a : b` would read as guarding itself.
    """
    statement = re.compile(r"if\s*\([^)]*\.%s\b(?!\.value_or)" % re.escape(field))
    for offset in range(max(0, index - 2), index + 1):
        if statement.search(lines[offset]):
            return True
    ternary = re.compile(r"\.%s\b(?!\.value_or)\s*\?" % re.escape(field))
    return any(match.start() < column for match in ternary.finditer(lines[index]))


def findings(root, files):
    names = accessorNames(root)
    found = []
    for relativePath in files:
        path = os.path.join(root, relativePath)
        if not os.path.exists(path):
            continue
        with open(path, encoding="utf-8", errors="replace") as handle:
            lines = handle.read().splitlines()
        for index, line in enumerate(lines):
            # The marker counts on the call's own line or the one immediately
            # above it, which is where a reason long enough to be worth reading
            # has to go. No further: one annotation must not silence a block.
            if ANNOTATION in line or (index > 0 and ANNOTATION in lines[index - 1]):
                continue
            for match in CALL.finditer(line):
                field, argument = match.group(1), match.group(2).strip()
                if field not in names:
                    continue
                if not LITERAL.match(argument):
                    continue
                if isGuardedByTheSameOptional(lines, index, match.start(), field):
                    continue
                found.append((relativePath, index + 1, field, argument, line.strip()))
    return found


def scopedFiles(root):
    collected = []
    for directory, _, entries in os.walk(os.path.join(root, SCOPE)):
        for entry in entries:
            if entry.endswith(SUFFIXES):
                collected.append(os.path.relpath(os.path.join(directory, entry), root))
    return sorted(collected)


ARM_A = [
    # Each is a shape the gate must refuse. A name here that stops being caught
    # is the gate silently narrowing.
    ("bool on = c.ligatures.value_or(true);", "a bool default restated"),
    ("BOOL on = c.ligatures.value_or(YES);", "the ObjC spelling of the same"),
    ("double s = c.wheelSpeed.value_or(1.0);", "a numeric default restated"),
    ("int n = c.ambiguousWidth.value_or(1);", "the key this gate was written for"),
    ("auto v = state.copyOnSelect.value_or(false);", "any receiver, not only `c`"),
    ("if (other.has_value()) x = c.ligatures.value_or(true);", "a guard on a different optional"),
    ("x = c.other ? c.ligatures.value_or(true) : n;", "a ternary on a different optional"),
    ("x = c.ligatures.value_or(true) ? a : b;", "a ternary the call feeds rather than one guarding it"),
    ("// native-default: reason\nint a = 1;\nbool on = c.ligatures.value_or(true);",
     "an annotation two lines up, which would silence a block"),
]

ARM_B = [
    ("bool on = c.ligatures.value_or(Defaults::ligatures);", "the named default"),
    ("bool on = Effective::ligatures(c);", "the accessor, which is the fix"),
    ("bool on = c.notAPreference.value_or(true);", "a field with no accessor"),
    ("double s = grid.fontSize.value_or(16.0); // native-default: Unifont's cell", "an annotated native default"),
    ("// native-default: Unifont's own cell, not Core's 12pt\ndouble s = grid.fontSize.value_or(16.0);",
     "the annotation on the line above, where a real reason fits"),
    ("if (s.fontSize)\n    f = s.fontSize.value_or(0.0);", "a guarded deref"),
    ("if (s.fontSize) f = s.fontSize.value_or(0.0);", "the same guard on one line"),
    ("f = s.fontSize ? s.fontSize.value_or(0.0) : other;", "the ternary spelling of that guard"),
    ("f = s.fontSize ? (CGFloat)(s.fontSize.value_or(0.0)) : other;", "the ternary through a cast, as the macOS snapshot command wrote it"),
    ("x = c.ligatures.value_or(other(c));", "a call as the argument"),
]


def selftest():
    """Both arms, in a tree of this gate's own making.

    Arm B matters more than arm A here: every false positive costs somebody a
    round of argument about a correct line, and three of these shapes were
    found by being wrong about them first.
    """
    failures = 0
    with tempfile.TemporaryDirectory() as root:
        os.makedirs(os.path.join(root, os.path.dirname(ACCESSORS)))
        # The rule reads accessor names from this file. A selftest tree without
        # it leaves every name unknown, so nothing is ever a finding and both
        # arms pass while testing nothing.
        with open(os.path.join(root, ACCESSORS), "w", encoding="utf-8") as handle:
            handle.write(
                "bool ligatures(const Values& c);\n"
                "double wheelSpeed(const Values& c);\n"
                "int ambiguousWidth(const Values& c);\n"
                "bool copyOnSelect(const Values& c);\n"
                "double fontSize(const Values& c);\n")
        os.makedirs(os.path.join(root, SCOPE, "macOS"))

        def check(body, description, expected):
            nonlocal failures
            relative = os.path.join(SCOPE, "macOS", "Case.mm")
            with open(os.path.join(root, relative), "w", encoding="utf-8") as handle:
                handle.write(body + "\n")
            if bool(findings(root, [relative])) != expected:
                verdict = "missed" if expected else "false positive on"
                print("selftest: %s %s -- %s" % (verdict, description, body.replace("\n", " ")))
                failures += 1

        # A planted positive first: if the accessor header were unreadable every
        # case below would pass by never running the rule at all.
        check("bool on = c.ligatures.value_or(true);", "the planted control", True)
        for body, description in ARM_A:
            check(body, description, True)
        for body, description in ARM_B:
            check(body, description, False)

    if failures:
        print("check-frontend-default-literals: selftest FAILED (%d case(s))" % failures)
        return 1
    print("check-frontend-default-literals: selftest ok (%d arm A, %d arm B)" % (len(ARM_A), len(ARM_B)))
    return 0


def stagedFiles(root):
    result = subprocess.run(
        ["git", "diff", "--cached", "--name-only", "--diff-filter=ACMR"],
        cwd=root, capture_output=True, text=True, check=False)
    return [p for p in result.stdout.split("\n")
            if p.startswith(SCOPE + "/") and p.endswith(SUFFIXES)]


def main():
    if "--selftest" in sys.argv:
        return selftest()
    if selftest():
        return 1
    files = stagedFiles(ROOT) if "--staged" in sys.argv else scopedFiles(ROOT)
    found = findings(ROOT, files)
    if not found:
        print("check-frontend-default-literals: ok (%d frontend file(s) scanned)" % len(files))
        return 0
    for relativePath, line, field, argument, text in found:
        print("%s:%d: `%s.value_or(%s)` restates a default Core owns" % (relativePath, line, field, argument))
        print("  > %s" % text)
        print("  -> Effective::%s(...) — one answer, so the two cannot drift apart." % field)
    print("")
    print("check-frontend-default-literals: %d site(s) spell a default Core already owns." % len(found))
    print("  A frontend's copy agrees with Core until the default changes, and then")
    print("  one arm moves and the other does not. Route through the accessor.")
    print("  A value that deliberately is not Core's — a browser drawing at its")
    print("  bitmap cell — is fine annotated `// native-default: <why>`.")
    return 1


if __name__ == "__main__":
    sys.exit(main())
