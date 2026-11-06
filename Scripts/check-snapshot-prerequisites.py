#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""Scripts/check-snapshot-prerequisites.py — the snapshot binaries exist.

  check-snapshot-prerequisites.py [root] [portable|both]

Named rather than spelled out as two cmake lines: the build scripts stamp the
build directory with the revision they were built from, and the snapshot tier
verifies that stamp. A hand-typed cmake produces a binary the tier can only
date, not identify.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import build_root

import gate_corpus

PORTABLE = "core/ubiquitty-snapshot"
MACOS = ("macos/Source/Platform/macOS/Ubiquitty.app/Contents/MacOS/"
         "Ubiquitty")


def main(argv):
    if argv and argv[0] in ("--help", "-h"):
        print(__doc__.strip())
        return 0

    # Two roots, because the two questions have different answers. `root` is the
    # SOURCE tree -- it is what the advice has to name, since that is where the
    # build scripts live and what the reader will run. The BINARIES are under
    # the build root, which is where build-core-snapshot.sh puts them
    # ($UBQ_BUILD_ROOT/core) and where run-tests.sh looks for them.
    #
    # Joining the constants onto `root` is what broke: the callers pass the
    # source tree, so the check looked for the binary beside the sources and
    # reported it missing however many times it was built -- while telling the
    # reader to run a script that could not change the answer. `import
    # build_root` was already here, unused, from the half of that move that did
    # happen: the constants lost their `build/` segment (correctly, since
    # $UBQ_BUILD_ROOT already is the build root) and the lookup did not follow.
    #
    # build_root() honours $UBQ_BUILD_ROOT, so a caller wanting the lookup
    # pointed elsewhere -- the fixture, at a temp tree -- sets that.
    root = argv[0] if argv else gate_corpus.root()
    builds = build_root.build_root()
    which = argv[1] if len(argv) > 1 else "both"
    missing = 0

    portable = os.path.join(builds, PORTABLE)
    if not os.access(portable, os.X_OK):
        print(f"FAIL: portable snapshot executable is missing: {portable}",
              file=sys.stderr)
        print("Build it with:", file=sys.stderr)
        print(f'  "{root}/Scripts/build-core-snapshot.sh"', file=sys.stderr)
        missing = 1

    if which != "portable":
        macos = os.path.join(builds, MACOS)
        if not os.access(macos, os.X_OK):
            print(f"FAIL: macOS snapshot executable is missing: {macos}",
                  file=sys.stderr)
            print("Build it with:", file=sys.stderr)
            print(f'  "{root}/Scripts/build-macos.sh"', file=sys.stderr)
            missing = 1

    return missing


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
