#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""check-ownership-analysis-on — every tree a run built compiled with the lock analysis.

UBIQUITTY_OWNERSHIP_ANALYSIS turns Clang's thread-safety and function-effect
analyses into errors (Scripts/cmake/UbiquittyToolchain.cmake). It defaults on
for a Clang that has both and off for any other compiler, saying so in the
configure output only. A build that lost it compiles green over a lock the
analysis would have refused, and two merges to main did exactly that: verified
by builds that never ran the analysis, then failing in the first build that did.

So a run that is meant to have compiled with the analysis proves it after the
fact, from what CMake recorded: every build tree under the given roots whose
cache defines the option must hold it ON. A tree whose cache does not define it
was configured by another project (the QUIC dependencies) and is not counted.
Finding no tree that defines it fails too: a run that built nothing under the
analysis has proved nothing about it.

    python3 Scripts/check-ownership-analysis-on.py <build root>...
    python3 Scripts/check-ownership-analysis-on.py --self-test
"""

import argparse
import os
import pathlib
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import gate_report  # noqa: E402 — the path above is what makes it importable

GATE = "check-ownership-analysis-on"
OPTION = "UBIQUITTY_OWNERSHIP_ANALYSIS"
# Build roots hold trees a few levels down (host/, BuildHostTests/ctest/,
# shell/<stanza>/); deeper caches belong to a tree's own external projects.
DEPTH = 4


def caches(root):
    """The CMakeCache.txt files under `root`, at most DEPTH directories down."""
    base = pathlib.Path(root)
    found = []
    for directory, subdirectories, files in os.walk(base):
        depth = len(pathlib.Path(directory).relative_to(base).parts)
        if depth >= DEPTH:
            subdirectories[:] = []
        if "CMakeCache.txt" in files:
            found.append(pathlib.Path(directory) / "CMakeCache.txt")
    return sorted(found)


def setting(cache):
    """The option's cached value in `cache`, or None when the cache does not define it."""
    try:
        text = cache.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return None
    for line in text.splitlines():
        name, separator, value = line.partition("=")
        if separator and name.split(":", 1)[0] == OPTION:
            return value.strip()
    return None


def judge(roots):
    """(trees with the option ON, trees with it otherwise) across `roots`."""
    on, off = [], []
    for root in roots:
        for cache in caches(root):
            value = setting(cache)
            if value is None:
                continue
            (on if value.upper() == "ON" else off).append((cache.parent, value))
    return on, off


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("roots", nargs="*", help="build roots whose trees must have compiled with the analysis")
    parser.add_argument("--self-test", action="store_true", help="judge planted caches and exit")
    arguments = parser.parse_args(argv)
    if arguments.self_test:
        return self_test()
    if not arguments.roots:
        parser.error("name at least one build root")
    on, off = judge(arguments.roots)
    if off:
        return gate_report.failed(GATE, f"build tree(s) configured with {OPTION} off",
                                  [f"{tree}: {OPTION}={value}" for tree, value in off])
    if not on:
        return gate_report.failed(GATE, f"build trees: none under {', '.join(arguments.roots)} defines {OPTION}, "
                                        "so nothing here compiled under the analysis")
    return gate_report.ok(GATE, f"{len(on)} build tree(s) compiled with {OPTION}=ON: "
                                + ", ".join(str(tree) for tree, _ in on))


def self_test():
    failures = []

    def plant(root, relative, value):
        directory = pathlib.Path(root) / relative
        directory.mkdir(parents=True, exist_ok=True)
        lines = ["CMAKE_BUILD_TYPE:STRING=Debug"]
        if value is not None:
            lines.append(f"{OPTION}:BOOL={value}")
        (directory / "CMakeCache.txt").write_text("\n".join(lines) + "\n")

    with tempfile.TemporaryDirectory() as root:
        plant(root, "host", "ON")
        plant(root, "BuildHostTests/ctest", "ON")
        plant(root, "quic-deps/build/ngtcp2", None)
        plant(root, "a/b/c/d/e", "OFF")
        on, off = judge([root])
        if (len(on), off) != (2, []):
            failures.append(f"two ON trees, a foreign tree and one too deep: got {len(on)} on, {off} off")
        plant(root, "agent", "OFF")
        if [value for _, value in judge([root])[1]] != ["OFF"]:
            failures.append("a tree configured with the option off is not named")
    with tempfile.TemporaryDirectory() as root:
        plant(root, "quic-deps", None)
        if judge([root]) != ([], []):
            failures.append("a root with no tree defining the option is not empty-handed")
    if failures:
        for failure in failures:
            print(f"{GATE} --self-test: {failure}", file=sys.stderr)
        return 1
    print(f"{GATE} --self-test: ok (4 cases)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
