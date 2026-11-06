#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0
"""host-tests.py — build and run a subset of the host tests in this checkout's own build root.

`Scripts/run-tests.sh` hands the CMake-covered tests to ctest only on a full
run, and a full run builds every test in the tree before it answers a question
about three of them. Iterating on a change means the configure line the runner
uses, the build of just the targets the question needs, and `ctest -R`; each
worktree derives its own build root and a fresh one needs the QUIC prefix a
sibling checkout already built. That is four things spelled by hand per
worktree, and the spellings drift.

This does them once, the runner's way:

  Tools/host-tests.py 'vt100_modes|sixel_lifecycle'   # build those, run those
  Tools/host-tests.py --build-only test_terminfo       # the target, no run
  Tools/host-tests.py --list 'sync'                    # what the pattern selects

The configure is the one `run-tests.sh` performs for its ctest handoff (Core
frontend, tests on, Debug, compile commands), issued only when the build root
has no cache yet or its cache belongs to another checkout. The QUIC prefix is
the one every build script names, this checkout's build root, so a checkout
never builds against a directory another worktree happens to hold. Targets are
the tests the pattern selects whose names are build targets; shell stanzas are
run by ctest through their own fixture. The setup fixture that rebuilds every
test is excluded, because the targets were just built.

Exit status is ctest's. Stdlib only.
"""
from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

SETUP_FIXTURE = "ubq_build"

# The nicest a process can be on Linux and on macOS alike.
LOWEST_PRIORITY = 19

CONFIGURE_FLAGS = [
    "-G", "Ninja",
    "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
    "-DUBIQUITTY_FRONTEND=Core",
    "-DUBIQUITTY_BUILD_TESTS=ON",
    "-DCMAKE_BUILD_TYPE=Debug",
]


def buildRoot() -> Path:
    """The build root the scripts derive for this checkout.

    Asked of `Scripts/build_root.py` rather than derived again here: a path
    spelled twice is a path that can disagree with itself.

    - Returns: the checkout's build root.
    """
    completed = subprocess.run(
        [sys.executable, str(ROOT / "Scripts" / "build_root.py"), str(ROOT)],
        capture_output=True, text=True)
    if completed.returncode != 0 or not completed.stdout.strip():
        raise SystemExit("host-tests: the build root did not resolve")
    return Path(completed.stdout.strip())


def quicPrefix() -> Path:
    """Where the QUIC dependencies are built for this checkout.

    The prefix build-host.sh and Scripts/cmake/UbiquittyToolchain.cmake name:
    UBIQUITTY_QUIC_PREFIX when
    set, else the checkout's build root — never another worktree's copy, which
    would make one checkout's build depend on what another happens to hold.

    - Returns: the prefix directory, whether or not it has been built yet.
    """
    named = os.environ.get("UBIQUITTY_QUIC_PREFIX", "")
    return Path(named) if named else buildRoot() / "quic-deps"


def cacheBelongsHere(cache: Path) -> bool:
    """Whether a CMake cache was written for this checkout.

    - Parameter cache: the CMakeCache.txt to read.
    - Returns: true when its home directory is this checkout.
    """
    if not cache.is_file():
        return False
    for line in cache.read_text(errors="replace").splitlines():
        if line.startswith("CMAKE_HOME_DIRECTORY:INTERNAL="):
            return Path(line.split("=", 1)[1]).resolve() == ROOT
    return False


def configure(ctestDirectory: Path) -> None:
    """Configures the test build root the way run-tests.sh does, once.

    - Parameter ctestDirectory: the build directory ctest reads.
    """
    if cacheBelongsHere(ctestDirectory / "CMakeCache.txt"):
        return
    command = ["cmake", "-S", str(ROOT), "-B", str(ctestDirectory)] + CONFIGURE_FLAGS
    command.append(f"-DUBIQUITTY_QUIC_PREFIX={quicPrefix()}")
    print(f"host-tests: configuring {ctestDirectory}", flush=True)
    completed = subprocess.run(command, cwd=ROOT)
    if completed.returncode != 0:
        raise SystemExit("host-tests: configure failed")


def selectedTests(ctestDirectory: Path, pattern: str) -> list[str]:
    """The test names a pattern selects, as ctest reads them.

    - Parameters:
      - ctestDirectory: the build directory ctest reads.
      - pattern: the `ctest -R` regular expression.
    - Returns: the selected names, in ctest's order.
    """
    completed = subprocess.run(
        ["ctest", "-N", "-R", pattern], cwd=ctestDirectory, capture_output=True, text=True)
    names = re.findall(r"^\s*Test\s+#\d+:\s+(\S+)", completed.stdout, re.M)
    # ctest lists the setup fixture beside the tests that require it; it is
    # the build, not a test, and the targets above are what it would do.
    return [name for name in names if name != SETUP_FIXTURE]


def buildTargets(ctestDirectory: Path) -> set[str]:
    """Every build target the generator knows, so a test name can be told from a stanza.

    - Parameter ctestDirectory: the build directory.
    - Returns: the target names.
    """
    completed = subprocess.run(
        ["ninja", "-t", "targets", "all"], cwd=ctestDirectory, capture_output=True, text=True)
    return {line.split(":", 1)[0] for line in completed.stdout.splitlines() if ":" in line}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("pattern", help="a ctest -R regular expression over test names")
    parser.add_argument("--list", action="store_true", help="print what the pattern selects and stop")
    parser.add_argument("--build-only", action="store_true", help="build the selected targets, run nothing")
    parser.add_argument("--jobs", type=int, default=6, help="parallel compile jobs (default 6, the machine is shared)")
    arguments = parser.parse_args()

    ctestDirectory = buildRoot() / "BuildHostTests" / "ctest"
    configure(ctestDirectory)
    names = selectedTests(ctestDirectory, arguments.pattern)
    if not names:
        print(f"host-tests: the pattern {arguments.pattern!r} selects no test", file=sys.stderr)
        return 2
    if arguments.list:
        print("\n".join(names))
        return 0
    targets = [name for name in names if name in buildTargets(ctestDirectory)]
    if targets:
        completed = subprocess.run(
            ["cmake", "--build", str(ctestDirectory), "-j", str(arguments.jobs), "--target", *targets], cwd=ROOT)
        if completed.returncode != 0:
            return completed.returncode
    if arguments.build_only:
        return 0
    completed = subprocess.run(
        ["ctest", "--output-on-failure", "-R", arguments.pattern, "--fixture-exclude-setup", SETUP_FIXTURE],
        cwd=ctestDirectory)
    return completed.returncode


def lowerPriority() -> None:
    """Drops this process to the lowest priority the box allows.

    ``os.nice`` takes an INCREMENT, so a process already niced -- every build
    here is launched under ``nice -n 19`` -- asks for a value past the ceiling
    and the call fails outright rather than clamping. The increment is
    therefore the distance left to the floor, and a refusal is survivable: the
    tests run at whatever priority the caller set.
    """
    try:
        current = os.nice(0)
        if current < LOWEST_PRIORITY:
            os.nice(LOWEST_PRIORITY - current)
    except OSError:
        pass


if __name__ == "__main__":
    lowerPriority()
    sys.exit(main())
