#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""covering-tests.py — the tests that cover what this branch changed.

Choosing which tests to run by thinking about the change is how a change
lands with its own test unrun. A change to the box diagonals can run the
stroke profile, the rasterizer and the ranges — every box test its author
reasons their way to — and not `test_boxglyph_golden`, which is the one that
records the draw commands themselves, so it goes red on main instead of on the
branch. `Tests/coverage/covering-tests.tsv` already knows: it lists that test
against `Source/Core/Rendering/BoxDrawCommands.cpp`, and this reads it.

    Tools/covering-tests.py                    the tests covering this branch
    Tools/covering-tests.py --since <ref>      against another base
    Tools/covering-tests.py <path>...          for named files
    Tools/covering-tests.py --run              run them, building first
    Tools/covering-tests.py --uncovered        only the changed files nothing covers

Two limits, both printed with the answer rather than left to be discovered.

The index is built by the coverage job, so it lags main. A stale one names
fewer tests than the change deserves, never more — its answer is a floor and
never a ceiling, which is the safe direction, and it means the drift makes
this weaker rather than wrong.

The index holds unit tests and no snapshot case, so it cannot answer which
goldens a change moves. A branch can be clean on every test named here and
still move a golden on an arm nobody ran, including a case whose subject it
never touches. The snapshot arms are selected separately, by folder, by the
pre-push gate runner.
"""

import argparse
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
INDEX = ROOT / "Tests" / "coverage" / "covering-tests.tsv"


def git(arguments, check=True):
    """Runs git in the repository and returns its stdout, stripped."""
    done = subprocess.run(["git", "-C", str(ROOT), *arguments],
                          capture_output=True, text=True, check=False)
    if check and done.returncode != 0:
        raise SystemExit(f"covering-tests: git {' '.join(arguments)}: "
                         f"{(done.stderr or done.stdout).strip()}")
    return done.stdout.strip()


def indexEntries():
    """The index as a mapping from source path to the tests covering it.

    - Returns: A pair of the mapping and the commit the index was built at,
      which is None when the header does not carry one.
    """
    if not INDEX.exists():
        raise SystemExit(f"covering-tests: no index at {INDEX.relative_to(ROOT)} — "
                         f"the coverage job writes it (./Scripts/run-coverage.sh)")
    covering = {}
    builtAt = None
    for line in INDEX.read_text().splitlines():
        if line.startswith("#"):
            head = line.lstrip("# ").split(",")[0].strip()
            if head and all(c in "0123456789abcdef" for c in head):
                builtAt = head
            continue
        source, _, tests = line.partition("\t")
        if source and tests:
            covering[source] = tests.split()
    return covering, builtAt


def changedFiles(since, paths):
    """The paths to ask the index about: committed and uncommitted alike.

    The working tree counts, because the moment to ask is before the commit
    rather than after it. A tool that answered only about landed commits would
    name the right tests one push too late.

    - Parameters:
      - since: The base to diff the branch against.
      - paths: Explicit paths, which answer instead of the diff when given.
    - Returns: The paths, deduplicated, in a stable order.
    """
    if paths:
        return list(paths)
    base = git(["merge-base", since, "HEAD"], check=False) or since
    found = [p for p in git(["diff", "--name-only", f"{base}...HEAD"]).splitlines() if p]
    # `git status` rather than a second diff: it carries staged, unstaged and
    # untracked in one answer, and an untracked file is the shape a new test
    # arrives in.
    for line in git(["status", "--porcelain", "--untracked-files=all"]).splitlines():
        name = line[3:].strip()
        # A rename reads as `old -> new`, and the new name is the one to ask about.
        if " -> " in name:
            name = name.split(" -> ", 1)[1]
        if name:
            found.append(name.strip('"'))
    seen, ordered = set(), []
    for name in found:
        if name not in seen:
            seen.add(name)
            ordered.append(name)
    return ordered


def staleness(builtAt):
    """How far the index lags this branch, as a sentence or None."""
    if not builtAt:
        return "the index carries no commit in its header, so how far it lags is unknown"
    if not git(["cat-file", "-e", f"{builtAt}^{{commit}}"], check=False) and \
            subprocess.run(["git", "-C", str(ROOT), "cat-file", "-e", f"{builtAt}^{{commit}}"],
                           capture_output=True).returncode != 0:
        return f"the index was built at {builtAt}, which is not in this clone"
    behind = git(["rev-list", "--count", f"{builtAt}..HEAD"], check=False)
    if behind and behind != "0":
        return (f"the index was built at {builtAt}, {behind} commit(s) back — a source added "
                f"since is absent from it, so this list is a floor and never a ceiling")
    return None


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("paths", nargs="*", help="files to ask about; omitted, the branch's")
    parser.add_argument("--since", default="origin/main", help="the base to diff against")
    parser.add_argument("--run", action="store_true", help="run the tests it names")
    parser.add_argument("--uncovered", action="store_true",
                        help="print only the changed files the index covers with nothing")
    arguments = parser.parse_args()

    covering, builtAt = indexEntries()
    changed = changedFiles(arguments.since, arguments.paths)
    if not changed:
        print("covering-tests: this branch changes no file")
        return 0

    tests, uncovered = set(), []
    for path in changed:
        named = covering.get(path)
        if named:
            tests.update(named)
        elif path.startswith(("Source/", "Tests/")):
            uncovered.append(path)

    lag = staleness(builtAt)
    if arguments.uncovered:
        for path in sorted(uncovered):
            print(path)
        print(f"covering-tests: {len(uncovered)} of {len(changed)} changed file(s) "
              f"are under Source/ or Tests/ and named by no test in the index",
              file=sys.stderr)
        if lag:
            print(f"covering-tests: {lag}", file=sys.stderr)
        return 0

    for test in sorted(tests):
        print(test)
    print(f"covering-tests: {len(tests)} test(s) over {len(changed)} changed file(s); "
          f"{len(uncovered)} file(s) named by none", file=sys.stderr)
    if lag:
        print(f"covering-tests: {lag}", file=sys.stderr)
    # Said every time, because the omission is invisible in the answer: a list
    # of unit tests looks complete whether or not a golden moved.
    print("covering-tests: unit tests only — the index holds no snapshot case, so a clean "
          "list here says nothing about which goldens this moves", file=sys.stderr)

    # Flushed before anything else writes: the names go to stdout and the
    # counts to stderr, and a block-buffered stdout put the list after the
    # ctest output it is supposed to introduce.
    sys.stdout.flush()
    if not arguments.run:
        return 0
    if not tests:
        print("covering-tests: nothing to run", file=sys.stderr)
        return 0
    buildRoot = subprocess.run([sys.executable, str(ROOT / "Scripts" / "build_root.py"), "."],
                               capture_output=True, text=True, check=False).stdout.strip()
    if not buildRoot:
        raise SystemExit("covering-tests: Scripts/build_root.py named no build root")
    tree = Path(buildRoot) / "BuildHostTests" / "ctest"
    # Built first, because ctest runs whatever binary is there and a stale one
    # answers about the tree as it was — the shape of green this whole tool
    # exists to avoid.
    targets = [t for t in sorted(tests) if not t.startswith("shell_")]
    if targets:
        subprocess.run(["cmake", "--build", str(tree), "--target", *targets], check=False)
    pattern = "^(" + "|".join(sorted(tests)) + ")$"
    return subprocess.run(["ctest", "--test-dir", str(tree), "-R", pattern,
                           "--output-on-failure"], check=False).returncode


if __name__ == "__main__":
    sys.exit(main())
