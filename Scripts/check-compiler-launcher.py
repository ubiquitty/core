#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""Scripts/check-compiler-launcher.py — every compile goes through the launcher.

CC and CXX name a compiler; UBQ_COMPILER_LAUNCHER names a wrapper such as
ccache. A script that invokes "$CXX" directly still builds — it just builds
uncached, so the only symptom is a slower suite that nobody attributes to a
change. That is why this is a gate and not a comment: the failure is invisible by
construction.

Compile through $CXX_TEST / $CC_LAUNCHED in the driver, or expand $launcher
beside "$cxx" as Scripts/portable-core-lib.sh does.
"""

import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import gate_corpus

GATE = "check-compiler-launcher"

# A compiler variable expanded and handed a flag: that is a compile.
COMPILE = re.compile(
    r'(^|[^_A-Za-z])"?\$\{?(CC|CXX|cxx)(:-[a-z+]+)?\}?"?[ \t]+-')

# The shell form filtered comments by matching `^\s*#` against the whole
# `path:line:text` string, which begins with the path and so never matched. No
# comment in the tree carries the shape today, so implementing it as it reads
# leaves the verdict where it was and removes a false positive that was waiting
# to happen: a comment naming `$CXX -c` would have been reported as a compile.
COMMENT = re.compile(r"^[ \t]*#")

# Naming the variable is not invoking it.
EXEMPT = (
    'CC="${CC:-',        # the assignments themselves
    'CXX="${CXX:-',
    "CXX_TEST",          # the launched forms
    "CC_LAUNCHED",
    "$launcher",
    "echo",              # naming one, not running it
    "SKIP:",
    "command -v",
)

WIDTH = 72


def offenders(paths):
    """Compiles that bypass the launcher, as (path, line number, text)."""
    found = []
    for path, number, line in gate_corpus.lines(paths):
        if COMMENT.match(line) or not COMPILE.search(line):
            continue
        if any(token in line for token in EXEMPT):
            continue
        found.append((path, number, line.lstrip()[:WIDTH]))
    return found


def main(argv):
    if argv and argv[0] in ("--help", "-h"):
        print(__doc__.strip())
        return 0
    os.chdir(gate_corpus.root())

    # The shell drivers are the corpus: a compile spelled in python is a
    # subprocess argument list, which cannot expand $CXX at all.
    paths = sorted(gate_corpus.tracked(["Scripts/*.sh"]))
    found = offenders(paths)

    for path, number, text in found:
        print(f"{path}:{number}: {text}")

    if found:
        print()
        print(f"{GATE}: {len(found)} compile(s) bypass the launcher.")
        print('  Use $CXX_TEST / $CC_LAUNCHED, or expand $launcher beside "$cxx".')
        return 1

    print(f"{GATE}: every compile carries the launcher")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
