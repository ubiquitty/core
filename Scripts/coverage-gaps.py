#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""coverage-gaps.py — which Core files no test reaches, and which only look that way.

The covering-test index is built from per-test llvm profiles, so it holds only
tests ctest runs with instrumentation. A shell stanza compiles its own link set
without coverage flags, so a file exercised solely by one is absent from the
index — and absent reads exactly like untested.

That is a proxy verdict in the coverage tooling itself: the index answers "which
INSTRUMENTED test executes this file" and gets read as "which test executes this
file". Acting on the difference means writing a second test for something already
covered, which is worse than the gap it was meant to close — the duplicate has to
be maintained and afterwards neither copy names itself as the redundant one.

So each absent file is sorted, and the sort decides what the work is:

  UNTESTED    no test source names it at all
  UNREACHED   an instrumented test names it, runs, and executes none of it
  UNMEASURED  a test names it, but only a shell stanza builds it (uninstrumented)
  UNLINKED    no instrumented test binary links it, and no test names it

UNMEASURED and UNREACHED both start from a test naming the file, which is weaker
than exercising it: the match is on the stem, so a mention in a comment counts.
That is the right weakness to have — it errs toward "look before you write
another test", and the alternative is the confident wrong answer this script
replaced.

What separates them is whether the namer runs. A `fuzz_*.cpp` is outside the
CMake glob, which takes `test_*.cpp`, so it reaches only the syntax-only
`shell_fuzz_harnesses_compile` stanza: naming a file there says the file
compiles. Reporting that as UNMEASURED blames the instrumentation for a gap that
is missing execution, and the two cost different work — one is a build change,
the other is a test.

A fourth list, for a file that is in the index and still looks worse than it is.
The whole-tree report passes every test binary as `-object`; code inline in a
header arrives once per binary that kept it, llvm-cov scores the first record for
a name and drops the rest, and nothing says which object it kept. For a `.cpp`
that is harmless — one object, one record. For a header the survivor may come
from an object that only INCLUDES it:

  MISATTRIBUTED  one covering test alone reaches more than the whole tree reports

    Source/Core/Rendering/CursorMotion.h   19.08% whole-tree, 94.92% against
    test_cursor_motion, which calls update() 37 times across all four modes.

Ordering by the whole-tree number sends a reader at whichever header looks worst,
so the one output whose job is "where should I write a test" is the one this
misdirects — the same substitution as UNMEASURED, one layer down.

Usage:
  ./Scripts/coverage-gaps.py            after ./Scripts/run-coverage.sh
"""

import os
import re
import subprocess
import sys

import os.path as _p, sys as _s
_s.path.insert(0, _p.dirname(_p.abspath(__file__)))
import build_root


def tracked(root, pattern):
    result = subprocess.run(["git", "ls-files", pattern], cwd=root,
                            capture_output=True, text=True)
    return [f for f in result.stdout.split() if f.endswith((".cpp", ".mm"))]


def runsInstrumented(root, testSource):
    """Whether a test source becomes a binary the instrumented run executes.

    A source under Tests/ is not automatically a test that runs: the CMake glob
    takes `test_*.cpp`, so every `fuzz_*.cpp` is outside it and reaches only a
    syntax-only stanza.

    - Parameters:
      - root: The repository root.
      - testSource: A repository-relative path under Tests/.
    - Returns: Whether the coverage build holds a binary of that stem.
    """
    stem = os.path.basename(testSource)
    stem = stem[:-4] if stem.endswith(".cpp") else stem
    return os.path.exists(os.path.join(build_root.build_root(root),
                                       "BuildHostTests/coverage/build/Tests", stem))


def emitsNothing(root, path):
    """Why a translation unit can have no coverage record without being a gap.

    Four rows of six in this tree's first run were files like these, and a list
    that is mostly non-gaps is one a reader learns to skip — which costs more
    than the two real rows are worth.

    - Parameters:
      - root: The repository root.
      - path: A repository-relative source path.
    - Returns: A reason, or an empty string when the file genuinely emits code.
    """
    full = os.path.join(root, path)
    objects = []
    build = os.path.join(build_root.build_root(root), "BuildHostTests/coverage/build")
    for directory, _, names in os.walk(build):
        if os.path.basename(path) + ".o" in names:
            objects.append(os.path.join(directory, os.path.basename(path) + ".o"))
    if not objects:
        return "no object in this configuration — it is not compiled here"

    # A definition line is the cheapest sound signal that the TU has code of its
    # own. WuffsCodec.cpp is thirteen lines, two of them static_assert, and its
    # 514 text symbols all come from a vendored header the report filters out.
    text = read(full)
    if not re.search(r"\)\s*(?:const\s*)?\{\s*$", text, re.M):
        return "defines no function of its own; its symbols come from what it includes"

    symbols = subprocess.run(["nm", "-U", objects[0]], capture_output=True, text=True)
    executable = len([line for line in symbols.stdout.split("\n") if re.search(r" [tT] ", line)])
    if executable <= 2:
        return "its object carries %d text symbol(s) — a data or wiring TU" % executable
    return ""


def summaryRows(text):
    """Parse llvm-cov's table into `path -> (lines, missed)`.

    - Parameter text: A report's stdout.
    - Returns: A dict keyed by repository-relative path.
    """
    rows = {}
    for line in text.split("\n"):
        parts = line.split()
        if len(parts) < 10 or "Source/" not in parts[0]:
            continue
        try:
            rows["Source/" + parts[0].split("Source/", 1)[1]] = (int(parts[7]), int(parts[8]))
        except ValueError:
            continue
    return rows


def toolPath(name):
    """The toolchain's copy, matching the compiler that wrote the profiles."""
    found = subprocess.run(["xcrun", "--find", name], capture_output=True, text=True)
    return found.stdout.strip() or name


def misattributedHeaders(root, coveringTests, wholeTree):
    """Headers the whole-tree report scores differently from a binary that runs them.

    One llvm-cov invocation per distinct covering test rather than one per
    header, because the binary is what the measurement is keyed to.

    - Parameters:
      - root: The repository root.
      - coveringTests: `path -> [test name, …]` from the index.
      - wholeTree: `path -> (lines, missed)` from the whole-tree summary.
    - Returns: `(path, wholeTreeMissed, wholeTreeLines, missed, lines, test)` for
      each header the two disagree about.
    """
    build = os.path.join(root, "BuildHostTests/coverage/build")
    raw = os.path.join(build, "coverage-raw")
    if not os.path.isdir(raw):
        return []

    byTest = {}
    for path, (lines, missed) in wholeTree.items():
        if not path.endswith((".h", ".hpp")):
            continue
        tests = coveringTests.get(path) or []
        if tests:
            byTest.setdefault(tests[0], []).append(path)

    profdata, cov = toolPath("llvm-profdata"), toolPath("llvm-cov")
    found = []
    for test, paths in byTest.items():
        binary = os.path.join(build, "Tests", test)
        raws = [os.path.join(raw, name) for name in os.listdir(raw)
                if name.startswith(test + "-")]
        if not os.path.exists(binary) or not raws:
            continue
        merged = "/tmp/coverage-gaps-%s.profdata" % test
        if subprocess.run([profdata, "merge", "-sparse", *raws, "-o", merged],
                          capture_output=True).returncode != 0:
            continue
        result = subprocess.run(
            [cov, "report", "-object", binary, "-instr-profile=" + merged,
             "-ignore-filename-regex=(Tests/|/compat/|BuildHostTests/|ThirdParty/)"],
            capture_output=True, text=True)
        rows = summaryRows(result.stdout)
        try:
            os.unlink(merged)
        except OSError:
            pass
        for path in paths:
            if path not in rows:
                continue
            lines, missed = rows[path]
            treeLines, treeMissed = wholeTree[path]
            # Only fewer misses proves it. One test is a lower bound on what the
            # suite reaches, and the whole-tree record merges counters from every
            # profile — so the whole-tree number being BETTER is the union doing
            # its job, not a fault. A lower bound that beats the reported number
            # is the contradiction, and it can only mean the record scored came
            # from a translation unit the profiles do not describe.
            if missed < treeMissed:
                found.append((path, treeMissed, treeLines, missed, lines, test))
    found.sort(key=lambda row: row[1] - row[3], reverse=True)
    return found


def read(path):
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as handle:
            return handle.read()
    except OSError:
        return ""


def main(argv):
    root = argv[1] if len(argv) > 1 else "."
    index = os.path.join(root, "Tests/coverage/covering-tests.tsv")
    if not os.path.exists(index):
        print("coverage-gaps: no covering index — run ./Scripts/run-coverage.sh first",
              file=sys.stderr)
        return 2

    # The index is a snapshot of a build that has since been edited more often
    # than not, and a stale one reports a file as uncovered because the run that
    # would have covered it predates its test. Refusing is the only safe answer:
    # a warning here is read past, and every verdict below inherits the staleness.
    indexAge = os.path.getmtime(index)
    newer = [path for path in tracked(root, "Tests") + tracked(root, "Source")
             if os.path.exists(os.path.join(root, path))
             and os.path.getmtime(os.path.join(root, path)) > indexAge]
    if newer:
        print("coverage-gaps: the index predates %d source file(s), so every verdict "
              "below would be about a tree that no longer exists." % len(newer), file=sys.stderr)
        for path in sorted(newer)[:5]:
            print("    newer than the index: %s" % path, file=sys.stderr)
        print("  Re-run ./Scripts/run-coverage.sh first.", file=sys.stderr)
        return 2

    covered = {os.path.normpath(line.split("\t")[0]) for line in read(index).split("\n") if line}
    summary = read(os.path.join(root, "BuildHostTests/coverage/summary.txt"))
    driver = read(os.path.join(root, "Scripts/run-tests.sh"))
    testSources = tracked(root, "Tests")
    testText = {path: read(os.path.join(root, path)) for path in testSources}

    untested, unmeasured, unbuilt, structural, unreached = [], [], [], [], []
    for path in tracked(root, "Source/Core"):
        if any(c.endswith(os.path.normpath(path)) for c in covered):
            continue
        stem = os.path.basename(path)[:-4]

        # Asked before the three buckets, because a file that emits nothing has
        # no record for any of them to reason about and every verdict they give
        # is about the absence rather than about a test.
        reason = emitsNothing(root, path)
        if reason:
            structural.append((path, reason))
            continue

        # Whether a test NAMES it is asked first. Asking
        # "is it in the coverage summary" first and short-circuiting reports a
        # file as unbuilt whenever no instrumented binary links it — which is
        # what a shell-stanza-only file looks like, and is the very confusion
        # this script exists to end.
        # Which namers RUN decides the answer, and the three cases are different
        # work. The fuzz harnesses build under --with-fuzz, which defaults off,
        # and their ctest entry is shell_fuzz_harnesses_compile — syntax-only by
        # its own header. So a harness naming a file says the file compiles.
        named = [t for t, text in testText.items()
                 if re.search(r'\b%s\b' % re.escape(stem), text)]
        running = [t for t in named if runsInstrumented(root, t)]
        if running:
            # An instrumented test names it, runs, and the file still has no
            # record: the test enters the header and never this translation
            # unit. QUICBootstrap.h is at 95% while QUICBootstrap.cpp has no
            # record at all, and the out-of-line half ships unexecuted.
            unreached.append((path, running[0]))
            continue
        if named:
            unmeasured.append((path, named[0]))
            continue

        # Absent from the summary and named by no test. llvm-cov reports every
        # file an instrumented binary carries a mapping for, including at 0%, so
        # absence means no test binary links it at all.
        if os.path.basename(path) not in summary and stem not in summary:
            unbuilt.append(path)
            continue

        untested.append(path)

    def report(title, rows, note):
        print("%s (%d)" % (title, len(rows)))
        print("  %s" % note)
        for row in rows:
            print("    %s" % (row if isinstance(row, str) else "%s  <- %s" % row))
        print("")

    report("UNREACHED", unreached,
           "an instrumented test names these, runs, and enters none of them")
    report("UNTESTED", untested,
           "no test source names these at all")
    report("UNMEASURED", unmeasured,
           "a test names these; it builds without instrumentation, so the index cannot see it")
    report("UNLINKED", unbuilt,
           "no instrumented test binary links these, and no test source names them")
    report("NO RECORD TO HAVE", structural,
           "these emit nothing a profile could describe, so their absence is not a gap")

    coveringMap = {}
    for line in read(index).split("\n"):
        if not line or line.startswith("#") or "\t" not in line:
            continue
        path, tests = line.split("\t", 1)
        coveringMap[path] = [name for name in tests.split("\t") if name]
    wrong = misattributedHeaders(root, coveringMap, summaryRows(summary))

    print("MISATTRIBUTED (%d)" % len(wrong))
    print("  a single covering test reaches more of these than the whole-tree reports")
    for path, treeMissed, treeLines, missed, lines, test in wrong:
        print("    %s" % path)
        print("      whole-tree %4d missed of %4d   against %s %4d of %4d"
              % (treeMissed, treeLines, test, missed, lines))
    print("")

    print("coverage-gaps: %d unreached, %d untested, %d unmeasured, %d unlinked, "
          "%d misattributed, %d with no record to have"
          % (len(unreached), len(untested), len(unmeasured), len(unbuilt), len(wrong),
             len(structural)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
