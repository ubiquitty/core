#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""check-log-writes.py — no diagnostic write that aborts the process when it
fails.

std::print and std::println throw on a failed write, and nothing here catches.
A daemon whose stderr goes away — a closed pipe, a full disk — then dies inside
a log line rather than logging that it could not log. Ubiquitty::PAL::logLine
and logText (Source/PAL/Log.h) write the same text and return.

  Scripts/check-log-writes.py
  Scripts/check-log-writes.py --self-test

Exit 0 = clean, 1 = throwing writes found.
"""

import os
import re
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import gate_corpus

GATE = "check-log-writes"

# A line CALLING a throwing print. The open paren is what separates a call from
# prose naming the function, which the self-test holds this to.
THROWING = re.compile(r"std::printl?n?[ \t]*\(")

NON_THROWING = re.compile(r"PAL::logLine|PAL::logText")

# Vendored third-party code keeps whatever it does.
VENDORED = "/ThirdParty/"

SUBJECTS = ("Source/", "Tools/")


def throwing_sites(paths):
    """Every call to a throwing print, as `path:line:text`."""
    sites = []
    for path, number, line in gate_corpus.lines(paths):
        if VENDORED in "/" + path:
            continue
        if THROWING.search(line):
            sites.append(f"{path}:{number}:{line}")
    return sites


def _self_test():
    """The rule against a tree that has the shape, and one that only names it."""
    status = 0
    with tempfile.TemporaryDirectory() as fixture:
        clean = os.path.join(fixture, "clean")
        dirty = os.path.join(fixture, "dirty")
        os.makedirs(os.path.join(clean, "PAL"))
        os.makedirs(dirty)
        # The replacement, and the two shapes of prose that name it without
        # calling it.
        written = {
            os.path.join(clean, "a.cpp"):
                'void f() { Ubiquitty::PAL::logLine(stderr, "x {}", 1); }\n',
            os.path.join(clean, "PAL", "Log.h"):
                "// std::println throws, which is why this file exists.\n",
            os.path.join(clean, "floor.h"):
                "// C++23. `if consteval`, `std::print` and deducing this are "
                "the floor.\n",
            # The shape that aborts a daemon on a failed write.
            os.path.join(dirty, "a.cpp"):
                'void f() { std::println(stderr, "x {}", 1); }\n',
        }
        for path, body in written.items():
            with open(path, "w") as handle:
                handle.write(body)

        cleanPaths = sorted(written)[:3]
        dirtyPaths = [os.path.join(dirty, "a.cpp")]
        if not throwing_sites([p for p in cleanPaths if clean in p]):
            print(f"{GATE} self-test: ok — the replacement, and prose naming "
                  f"what it replaces, pass")
        else:
            print(f"{GATE} self-test: FAIL — a clean tree was reported dirty")
            status = 1
        if throwing_sites(dirtyPaths):
            print(f"{GATE} self-test: ok — a throwing print is found")
        else:
            print(f"{GATE} self-test: FAIL — a throwing print went unreported")
            status = 1
    return status


def main(argv):
    if argv and argv[0] in ("--help", "-h"):
        print(__doc__.strip())
        return 0
    if argv and argv[0] in ("--self-test", "--selftest"):
        return _self_test()

    os.chdir(gate_corpus.root())
    paths = [path for path in gate_corpus.tracked_and_untracked()
             if path.startswith(SUBJECTS)]

    sites = throwing_sites(paths)
    if sites:
        for site in sites:
            print(f"  {site}")
        # Flushed before the verdict goes to stderr. A caller that captures the
        # gate with `> out 2>&1` — which is how Scripts/check-style.py captures
        # every one of them — sees the two streams in the order they were
        # written only if the buffered one is emptied first.
        sys.stdout.flush()
        print(f"{GATE}: FAILED — {len(sites)} diagnostic write(s) that abort "
              f"the process when they fail.", file=sys.stderr)
        print("  std::print/std::println throw, and nothing here catches. Use",
              file=sys.stderr)
        print("  Ubiquitty::PAL::logLine or logText (Source/PAL/Log.h).",
              file=sys.stderr)
        return 1

    onPath = sum(1 for _, text in gate_corpus.texts(paths)
                 if NON_THROWING.search(text))
    print(f"{GATE}: ok (no throwing diagnostic write in Source or Tools; "
          f"{onPath} file(s) on the non-throwing path)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
