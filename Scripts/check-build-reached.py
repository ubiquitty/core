#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""check-build-reached.py — did the build compile what your change reaches?

`N of M translation units compiled` is a claim about the RUN. It still asks the
reader to notice that N < M matters, and it cannot say whether the units it
skipped were the ones the change could break. A header two includes away leaves
a file untouched, the run is green, and the file that will not compile was never
offered to the compiler.

The claim worth making is about the CHANGE: *the three translation units
reaching CellCanvas.h were rebuilt*. Ninja already knows the answer — its
dependency database records, per object, every header the compiler actually
opened. Not the includes a file declares, and not what a grep of include lines
would guess: what was read.

    ./Scripts/check-build-reached.py <build-dir> [changed files...]

With no files named, the changed set is taken from git against origin/main.
A change reaching nothing is reported as reaching nothing, rather than as
success: a header no object depends on is either new, unused, or named wrong,
and all three are worth a sentence.

What is compared, and why not this run's compile list. An object is checked when
it is newer than every header it reads — by whichever run compiled it, not
necessarily this one. Asking instead whether this run compiled it makes a
correct, fully-built tree fail: the run after a successful build compiles
nothing, and every object is current precisely because the previous run did the
work. That is the normal state of every checkout's build directory, so such a
gate goes red for everyone whose tree is right.
"""

import os
import re
import subprocess
import sys

# `path/to.o: #deps 243, deps mtime … (VALID)`
DEPS_HEADER = re.compile(r'^(\S+): #deps')

HEADER_SUFFIXES = (".h", ".hpp")


def dependency_map(buildDirectory):
    """header path -> set of objects whose compile actually read it."""
    result = subprocess.run(["ninja", "-t", "deps"], cwd=buildDirectory,
                            capture_output=True, text=True)
    if result.returncode != 0 or not result.stdout.strip():
        return None

    byHeader, current = {}, None
    for line in result.stdout.split("\n"):
        header = DEPS_HEADER.match(line)
        if header:
            current = header.group(1)
            continue
        if current and line.startswith("    "):
            byHeader.setdefault(line.strip(), set()).add(current)
    return byHeader


def changed_files(root):
    """What this branch changed, plus anything not yet committed."""
    names = set()
    for command in (["git", "diff", "--name-only", "origin/main...HEAD"],
                    ["git", "diff", "--name-only"],
                    ["git", "diff", "--cached", "--name-only"]):
        result = subprocess.run(command, cwd=root, capture_output=True, text=True)
        if result.returncode == 0:
            names.update(name for name in result.stdout.split("\n") if name)
    return sorted(names)


def stale_objects(buildDirectory, reaching):
    """Objects older than the header they read, or missing entirely.

    Compared against the path the compiler opened, not the path in Source/. A
    header is staged into the build directory and the staged copy is refreshed
    write-if-different, so editing nothing but the mtime of the original leaves
    the staged copy — and every object that reads it — correctly untouched.
    Comparing against the original calls those objects stale forever, which is
    the wrong-oracle shape one level in from the one this check exists to catch.

    Mtime rather than a compile list: the question is whether the artefact
    reflects what it read, which is true however it came to be true. An object
    that does not exist has never been built and is stale by the widest margin.
    """
    stale = []
    for name, sources in reaching.items():
        path = os.path.join(buildDirectory, name)
        if not os.path.exists(path):
            stale.append(name)
            continue
        newest = 0.0
        for source in sources:
            try:
                newest = max(newest, os.path.getmtime(source))
            except OSError:
                continue
        if newest and os.path.getmtime(path) < newest:
            stale.append(name)
    return sorted(stale)


def objects_reaching(byHeader, relative):
    """Objects that read this source path, matched by suffix.

    A translation unit includes the STAGED copy under the build directory, not
    the file in Source/ — so an exact path comparison finds nothing for exactly
    the headers this is asked about. Matching on the tail of the path finds both
    the staged copy and the original.
    """
    tail = relative[len("Source/"):] if relative.startswith("Source/") else relative
    reaching = {}
    for path, objects in byHeader.items():
        if path.endswith("/" + tail) or path.endswith("/" + os.path.basename(tail)):
            for name in objects:
                reaching.setdefault(name, set()).add(path)
    return reaching


def main(argv):
    if len(argv) < 2:
        print("usage: check-build-reached.py <build-dir> [changed files...]", file=sys.stderr)
        return 2
    buildDirectory = argv[1]
    root = os.path.abspath(".")
    named = [a for a in argv[2:] if not a.startswith("--")]

    if not os.path.isdir(buildDirectory):
        print("check-build-reached: no build directory at %s" % buildDirectory, file=sys.stderr)
        return 2

    byHeader = dependency_map(buildDirectory)
    if not byHeader:
        print("check-build-reached: ninja reported no dependencies for %s."
              % buildDirectory, file=sys.stderr)
        print("  Nothing can be said about what the change reaches, which is not the",
              file=sys.stderr)
        print("  same as the change reaching nothing.", file=sys.stderr)
        return 2

    headers = [name for name in (named or changed_files(root))
               if name.endswith(HEADER_SUFFIXES)]
    if not headers:
        print("check-build-reached: no changed header to trace.")
        return 0

    problems = 0
    for relative in sorted(headers):
        reaching = objects_reaching(byHeader, relative)
        if not reaching:
            print("%s: no translation unit reads it — new, unused, or the path is wrong."
                  % relative)
            continue
        stale = stale_objects(buildDirectory, reaching)
        if stale:
            problems += 1
            print("%s: %d of %d translation units reaching it are older than what they read:"
                  % (relative, len(stale), len(reaching)), file=sys.stderr)
            for name in stale[:6]:
                print("    %s" % name, file=sys.stderr)
        else:
            print("%s: all %d translation units reaching it are current."
                  % (relative, len(reaching)))

    if problems:
        print("", file=sys.stderr)
        print("check-build-reached: a translation unit this change can break is stale.",
              file=sys.stderr)
        print("  Its object predates a header it reads, so nothing has compiled it", file=sys.stderr)
        print("  against the current source. Build again, or run", file=sys.stderr)
        print("  UBQ_TESTS_CLEAN=1 ./Scripts/build-tests.sh if the graph is not tracking it.",
              file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
