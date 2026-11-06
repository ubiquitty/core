#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0
"""remint-fingerprint.py — record a wire body's new fingerprint after its version moved.

`Tests/Core/Protocol/Sync/test_sync_wire_fingerprint.cpp` pins each Sync body
(snapshot, row delta, scrollback, config, image) to a length and a digest under
its version constant. A change to a body's encoding is legitimate only with a
version bump, and the test says which happened: a bumped version prints the
new length and digest to record; a standing version with moved bytes is a
refusal, because a peer built before the change would decode the new bytes as
the old layout.

This runs the test and does the recording, which by hand is a build, a run, a
read of two hex numbers and an edit of a line that must not be the wrong one:

  Tools/remint-fingerprint.py              # every body the test reports as moved
  Tools/remint-fingerprint.py snapshotVersion

A body whose version stood still is reported and left alone: the fix for that
is the version constant, not the recorded digest. After the edit the test is
built and run again, and the exit status is that run's.

Stdlib only; builds through Tools/host-tests.py.
"""
from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TEST_SOURCE = ROOT / "Tests" / "Core" / "Protocol" / "Sync" / "test_sync_wire_fingerprint.cpp"
TEST_TARGET = "test_sync_wire_fingerprint"

MOVED = re.compile(r"^(\w+): moved (\d+) -> (\d+); re-mint with length=(\d+) digest=(0x[0-9a-f]+)", re.M)
STOOD_STILL = re.compile(r"^(\w+): the body changed but the version is still (\d+)\.", re.M)


def buildAndRun() -> tuple[int, str]:
    """Builds the fingerprint test and runs it.

    - Returns: the test's exit status and everything it printed.
    """
    built = subprocess.run(
        [sys.executable, str(ROOT / "Tools" / "host-tests.py"), "--build-only", f"^{TEST_TARGET}$"])
    if built.returncode != 0:
        raise SystemExit("remint-fingerprint: the test did not build")
    root = subprocess.run(
        [sys.executable, str(ROOT / "Scripts" / "build_root.py"), str(ROOT)], capture_output=True, text=True).stdout.strip()
    binary = Path(root) / "BuildHostTests" / "ctest" / "Tests" / TEST_TARGET
    completed = subprocess.run([str(binary)], cwd=ROOT, capture_output=True, text=True)
    return completed.returncode, completed.stdout + completed.stderr


def recordLine(name: str) -> re.Pattern[str]:
    """The source line that records one body's fingerprint.

    - Parameter name: the version constant the body is pinned under.
    - Returns: a pattern over `checkFingerprint({ "<name>", version, length, digest }`.
    """
    return re.compile(r'checkFingerprint\(\{ "' + re.escape(name) + r'", (\d+), (\d+), (0x[0-9a-f]+)ull \}')


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("bodies", nargs="*", help="version constants to re-mint (default: every body the test reports as moved)")
    arguments = parser.parse_args()

    status, output = buildAndRun()
    if status == 0:
        print("remint-fingerprint: every body matches its record; nothing to re-mint")
        return 0
    stoodStill = STOOD_STILL.findall(output)
    for name, version in stoodStill:
        print(f"remint-fingerprint: {name} changed its bytes under version {version}; bump the constant before re-minting", file=sys.stderr)
    moved = [m for m in MOVED.findall(output) if not arguments.bodies or m[0] in arguments.bodies]
    if not moved:
        if not stoodStill:
            sys.stderr.write(output)
        return 1
    source = TEST_SOURCE.read_text()
    for name, was, now, length, digest in moved:
        line = recordLine(name)
        if len(line.findall(source)) != 1:
            print(f"remint-fingerprint: {name} is recorded {len(line.findall(source))} times in {TEST_SOURCE.name}; expected one", file=sys.stderr)
            return 1
        source = line.sub(f'checkFingerprint({{ "{name}", {now}, {length}, {digest}ull }}', source)
        print(f"remint-fingerprint: {name} {was} -> {now}: length={length} digest={digest}")
    TEST_SOURCE.write_text(source)
    status, output = buildAndRun()
    if status != 0:
        sys.stderr.write(output)
    return 1 if stoodStill else status


if __name__ == "__main__":
    sys.exit(main())
