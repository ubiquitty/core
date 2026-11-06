#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

# check-source-file-names — every tracked file under Source/ carries an
# extension the build can see.
#
# Every other gate here keys on extensions: the boundary checks glob *.h and
# *.cpp, the build registers named suffixes, the content-loss check counts what
# it can classify. A file with no extension is invisible to all of them at
# once: a duplicate of a protocol source committed by an add sweep under an
# extensionless name passes every gate in the chain, referenced by nothing and
# flagged by nothing. The name is the one fact every gate shares; this checks
# it.

import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import ubq_roots  # noqa: E402
ROOT = Path(ubq_roots.subject_of(__file__))

# The extensions the tree legitimately holds, from a census of every tracked
# file under Source/. An addition here is a review question: a new suffix
# means a new kind of artifact, and the gates that glob by suffix need to
# know whether it is theirs.
ALLOWED_SUFFIXES = {
    ".h", ".cpp", ".swift", ".mm", ".cc", ".c", ".m",
    ".vert", ".frag", ".metal", ".inc",
    ".md", ".txt", ".js", ".html", ".el", ".lua",
    # Browser stylesheets, staged by the web build, carry checked SPDX headers.
    ".css",
    ".toml", ".sdef", ".modulemap", ".in",
    # The iOS simulator's code-signing entitlements; CMakeLists.txt names the
    # file whole for the simulator target's signing step.
    ".entitlements",
    # The iOS app's privacy manifest; CMakeLists.txt names the file whole as a
    # bundle resource.
    ".xcprivacy",
}

# Extensionless names with a stated reason. A name that is not here and has
# no allowed suffix fails, whatever its content.
ALLOWED_BARE_NAMES = {
    "LICENSE": "vendored and platform licence texts",
    "CMakeLists.txt": "named whole, never by suffix",
    "Package.resolved": "SwiftPM's pins for the UbiquittySSH package, read by check-licenses.py",
}


def main() -> int:
    listing = subprocess.run(
        ["git", "ls-files", "Source/"],
        cwd=ROOT, capture_output=True, text=True, check=True)
    tracked = listing.stdout.splitlines()
    # An empty listing is a broken run, not a clean tree: Source/ carries
    # hundreds of files, so zero means the scan looked at nothing — a wrong
    # directory, a renamed tree, a git that answered nothing. Passing over it is
    # the silence this gate exists to break, so it refuses rather than reports ok.
    if not tracked:
        print("check-source-file-names: FAILED — git ls-files Source/ returned no files",
              file=sys.stderr)
        print("  Source/ holds hundreds of tracked files; zero means the scan reached "
              "nothing", file=sys.stderr)
        print("  and checked nothing, not that the tree is clean.", file=sys.stderr)
        return 2

    # The dash rule runs over the WHOLE tree, unlike the suffix rule above it.
    # A name beginning with `-` is an argument every shell command reads as an
    # option, so `rm --help` and `grep -n pattern --backend` do something other
    # than what they say — and no legitimate file is named that way.
    #
    # Three were tracked at the repo root: `--backend` and `--columns` (PNGs)
    # and `--help` (26 KB of compare-bench JSON, "platform": "Darwin-arm64").
    # Each is a tool run where a flag landed in an output-path argument, and
    # each was committed by an add sweep — the same way the 3302-line
    # protocol source duplicate above arrived. The suffix rule could not see
    # them because it is scoped to Source/ and they sat at the root.
    everything = subprocess.run(
        ["git", "ls-files"],
        cwd=ROOT, capture_output=True, text=True, check=True).stdout.splitlines()
    if not everything:
        print("check-source-file-names: FAILED — git ls-files returned no files at "
              "all, so the dash rule checked nothing", file=sys.stderr)
        return 2
    dashed = [p for p in everything if Path(p).name.startswith("-")]
    if dashed:
        for path in dashed:
            print(f"check-source-file-names: {path} begins with a dash, so every "
                  "command reads it as an option")
        print()
        print("  Almost always a flag that landed where an output path belonged.")
        print("  Delete it, or rename it so a shell can name it without `./`.")
        return 1

    offenders = []
    for line in tracked:
        name = Path(line).name
        if name in ALLOWED_BARE_NAMES:
            continue
        suffix = Path(name).suffix
        if suffix in ALLOWED_SUFFIXES:
            continue
        offenders.append((line, suffix))
    if offenders:
        for path, suffix in offenders:
            what = f"suffix {suffix!r}" if suffix else "no suffix"
            print(f"check-source-file-names: {path} has {what}, which no gate or build rule reads")
        print()
        print("  A file the suffix globs cannot see is invisible to every check at")
        print("  once. Name it with an allowed suffix, or add the suffix (or the")
        print("  bare name) here with its reason.")
        return 1
    print(f"check-source-file-names: ok ({len(tracked)} tracked file(s) under Source/)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
