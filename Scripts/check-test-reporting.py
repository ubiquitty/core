#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""check-test-reporting.py — every test reports through the harness.

UBIQUITTY_TEST_MAIN() returns the harness's own count. A private counter is
never read, so every assertion can fail and the process still exits 0 — CTest
reports the test green and a regression ships. A file that owns its own main()
and returns its own counter is fine: the discriminator is who owns main, not who
counts.

Four rules, in the order the report lists them:
  - a file that ends through the harness, prints a FAIL of its own, and never
    calls CHECK
  - a file that owns main(), asserts with CHECK, and returns a literal 0 without
    reading the count — the same defect by the other route: it cannot fail
  - __FILE__ turned into a path
  - an assertion the harness already provides, defined again

  Scripts/check-test-reporting.py            # whole tree
  Scripts/check-test-reporting.py --staged   # staged files only

Exit 0 = clean, 1 = findings, 2 = the scan could not key on what it greps for.
"""

import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import gate_corpus

GATE = "check-test-reporting"

SUBJECT = re.compile(r"^Tests/.*\.(cpp|mm)$")

HARNESS_MAIN = "UBIQUITTY_TEST_MAIN()"
HARNESS_MACRO = "UBIQUITTY_TEST_MAIN"
OWN_FAIL = re.compile(r'"[^"]*FAIL')
CALLS_CHECK = re.compile(r"\bCHECK(_MSG|_EQ|_STR_EQ|_PTR_EQ|_NEAR)?[ \t]*\(")
# A verdict that consults the harness's count: the summary macro, or the counter
# read by hand the way a file owning its own main is allowed to.
READS_VERDICT = re.compile(r"UBIQUITTY_TEST_SUMMARY|\bfailures\b")
# `return 0;` written as the last word of a test's own main.
RETURNS_ZERO = re.compile(r"^[ \t]*return[ \t]+0[ \t]*;", re.M)

# __FILE__ is a diagnostic string, not a path.
#
# The compiler cache rewrites absolute source paths so that builds at different
# checkouts share one cache, and __FILE__ follows the command line — so it goes
# relative with everything else and then resolves against a working directory
# the test binary does not run in. A test that locates a fixture through it
# stops finding the fixture, and says so as a filesystem error naming a path
# nobody wrote.
#
# Printing it is fine and is what every use here does. Turning it into a path is
# the case: take the location from the build system, which still knows the
# absolute root, as a compile definition.
FILE_AS_PATH = re.compile(
    r"(filesystem::path|ifstream|ofstream|fopen|opendir|parent_path"
    r"|std::string[ \t]*\()[^;]*__FILE__")

DEFINE = re.compile(r"^#[ \t]*define[ \t]+([A-Z][A-Z_0-9]*)")

# A second name for an assertion the harness already has. EXPECT is the one
# everyone reaches for out of habit, and a file that defines it gets a private
# primitive that can drift from the harness — which is exactly how twenty tests
# came to print FAIL and exit 0. Naming the equivalent is the point: a rule that
# only says "no" teaches nothing.
REDEFINITIONS = {}
for name in ("CHECK", "CHECK_MSG", "CHECK_EQ", "CHECK_NE", "CHECK_CSTR_EQ",
             "CHECK_STR_EQ", "CHECK_PTR_EQ", "CHECK_CONTAINS",
             "CHECK_NOT_CONTAINS", "CHECK_NEAR"):
    REDEFINITIONS[name] = "Tests/Harness.h already defines it"
for name in ("EXPECT", "EXPECT_EQ", "EXPECT_TRUE", "EXPECT_FALSE", "ASSERT",
             "ASSERT_EQ", "ASSERT_TRUE", "REQUIRE"):
    REDEFINITIONS[name] = "the harness spells this CHECK"
for name in ("CHECK_STREQ", "EXPECT_STREQ", "EXPECT_CSTR_EQ"):
    REDEFINITIONS[name] = "the harness spells this CHECK_CSTR_EQ"
for name in ("EXPECT_STR_EQ", "CHECK_EQ_STR", "CHECK_STRING_EQ",
             "EXPECT_EQ_STR"):
    REDEFINITIONS[name] = "the harness spells this CHECK_STR_EQ"
for name in ("EXPECT_NEAR", "CHECK_CLOSE"):
    REDEFINITIONS[name] = "the harness spells this CHECK_NEAR"

SILENT_RULES = """
check-test-reporting: the file(s) above report a failure the harness never hears.

  UBIQUITTY_TEST_MAIN() returns the harness's own count. A private counter is
  never read, so every assertion can fail and the process still exits 0 — CTest
  reports the test green and a regression ships.

  Assert through Tests/Harness.h: CHECK, CHECK_MSG, CHECK_EQ. A local
  EXPECT/check() wrapper is the same rule written twice, which is how the two
  came apart in the first place; delete it rather than wiring it up.

  A file that owns its own main() and returns its own counter is fine and is
  not flagged — the discriminator is who owns main, not who counts.
"""

REDEFINED_RULES = """
check-test-reporting: an assertion the harness already provides, defined again above.

  A private copy is free to drift from the harness, and drift is how a test
  stops reporting at all. Use the harness macro; if it is missing something the
  test needs, add it there once rather than here each time.

  Tests/Harness.h: CHECK, CHECK_MSG, CHECK_EQ, CHECK_NE, CHECK_CSTR_EQ,
  CHECK_STR_EQ, CHECK_PTR_EQ, CHECK_CONTAINS, CHECK_NOT_CONTAINS, CHECK_NEAR.
"""


def main(argv):
    if argv and argv[0] in ("--help", "-h"):
        print(__doc__.strip())
        return 0
    staged = bool(argv) and argv[0] == "--staged"
    if argv and not staged:
        print(f"{GATE}: unknown flag: {argv[0]}", file=sys.stderr)
        return 2

    os.chdir(gate_corpus.root())
    listing = gate_corpus.staged() if staged else gate_corpus.tracked()
    paths = [path for path in listing if SUBJECT.match(path)]
    if not paths:
        print(f"{GATE}: no test files to scan")
        return 0

    # One read per file, three questions asked of it.
    bodies = dict(gate_corpus.texts(paths))

    # Floor (whole-tree only): the primary check keys on the exact spelling
    # `UBIQUITTY_TEST_MAIN()`. Give the macro an argument (a suite name) or
    # rename it and that match finds nothing in every file — each one is skipped,
    # nothing is found, and the gate reports every test as reporting through the
    # harness over a population it never examined. When files still use the macro
    # in some form but none in the parenthesised form, the spelling drifted:
    # refuse rather than certify. Skipped under --staged, where an empty
    # population is real.
    if not staged:
        users = sum(1 for text in bodies.values() if HARNESS_MACRO in text)
        keyed = sum(1 for text in bodies.values() if HARNESS_MAIN in text)
        if users > 0 and keyed == 0:
            print(f"{GATE}: REFUSED — the harness macro is used but never as "
                  f"'{HARNESS_MAIN}'", file=sys.stderr)
            print(f"  ({users} file(s) name {HARNESS_MACRO}, 0 in the "
                  f"parenthesised form this gate greps for).", file=sys.stderr)
            print("  It took an argument or was renamed, so the who-owns-main "
                  "check skipped every file.", file=sys.stderr)
            print(f"  Update HARNESS_MAIN in this script to the new spelling.",
                  file=sys.stderr)
            return 2

    # The corpus order, so the report lists files in a stable order.
    silent = [path for path in paths
              if path in bodies
              and HARNESS_MAIN in bodies[path]
              and OWN_FAIL.search(bodies[path])
              and not CALLS_CHECK.search(bodies[path])]
    for path in silent:
        print(f"  {path}")

    # A file that owns main, asserts with CHECK, and returns a literal 0 without
    # ever consulting the count. CHECK records and carries on — that is the
    # documented design — so such a main exits 0 with every assertion failed,
    # and CTest reports the test green forever.
    #
    # Distinct from the private-counter rule above, which is about a count the
    # file keeps itself. This is about the harness's own count going unread, and
    # it is the same defect by the other route: the test cannot fail.
    unreported = [path for path in paths
                  if path in bodies
                  and HARNESS_MAIN not in bodies[path]
                  and "int main(" in bodies[path]
                  and CALLS_CHECK.search(bodies[path])
                  and RETURNS_ZERO.search(bodies[path])
                  and not READS_VERDICT.search(bodies[path])]
    for path in unreported:
        print(f"  {path}: own main() returns 0 without reading the failure count")

    if unreported:
        sys.stdout.flush()
        print(f"{GATE}: FAILED — {len(unreported)} test(s) cannot fail (above)",
              file=sys.stderr)
        print("  CHECK records and carries on, so a main() that returns a "
              "literal 0 exits", file=sys.stderr)
        print("  green with every assertion failed. Return "
              "UBIQUITTY_TEST_SUMMARY(), or the", file=sys.stderr)
        print("  harness counter: `return failures == 0 ? 0 : 1;`",
              file=sys.stderr)
        return 1

    fileAsPath = False
    for path in paths:
        text = bodies.get(path)
        if text is None:
            continue
        hits = [(number, line)
                for number, line in enumerate(text.split("\n"), start=1)
                if FILE_AS_PATH.search(line)]
        if hits:
            first = f"{hits[0][0]}:{hits[0][1].lstrip()}"
            print(f"  {path}: {len(hits)} use(s) of __FILE__ as a path, first "
                  f"at {first}")
            fileAsPath = True

    if fileAsPath:
        sys.stdout.flush()
        print(f"{GATE}: FAILED — __FILE__ used to build a path (above)",
              file=sys.stderr)
        print("  It is relative whenever the compiler cache rewrites source "
              "paths, so it", file=sys.stderr)
        print("  resolves against a directory the binary does not run in. Pass "
              "the", file=sys.stderr)
        print("  location from CMake instead:", file=sys.stderr)
        print('    set(test_X_EXTRA_DEFINES UBQ_X_DIR="${CMAKE_CURRENT_SOURCE_DIR}/...")',
              file=sys.stderr)
        return 1

    redefined = False
    for path in paths:
        text = bodies.get(path)
        if text is None:
            continue
        for line in text.split("\n"):
            match = DEFINE.match(line)
            if not match:
                continue
            reason = REDEFINITIONS.get(match.group(1))
            if reason:
                print(f"  {path}: #define {match.group(1)} — {reason}")
                redefined = True

    if redefined:
        sys.stdout.flush()
        print(REDEFINED_RULES, file=sys.stderr, end="")
        return 1

    if silent:
        sys.stdout.flush()
        print(SILENT_RULES, file=sys.stderr, end="")
        return 1

    print(f"{GATE}: every test reports through the harness")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
