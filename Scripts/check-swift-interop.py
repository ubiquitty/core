#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""check-swift-interop.py — the Swift toolchain is reached through one header.

<swift/bridging> ships only with the Swift toolchain, so a direct include
compiles on a Mac and breaks every Linux translation unit. A bare SWIFT_*
attribute is worse than an error: it is defined only when that header was found,
so where it is absent the annotation sits there doing nothing.

Both go through Source/Base/SwiftInterop.h, which includes the header behind
__has_include and republishes the attributes under UBIQUITTY_* spellings that
expand to nothing when the toolchain is not there.

  Scripts/check-swift-interop.py

Exit 0 = clean, 1 = sites reaching the toolchain directly, 2 = the scan reached
nothing.
"""

import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import gate_corpus
import ubq_roots

GATE = "check-swift-interop"

# The one file that may name the bridging header, and the only place a bare
# SWIFT_* attribute belongs.
GATEWAY = "Source/Base/SwiftInterop.h"

KINDS = (".h", ".hpp", ".cpp", ".mm", ".m")
EXEMPT = ("Tests/", "Source/ThirdParty/")

RAW_INCLUDE = re.compile(r"#include <swift/bridging>")
BARE_ATTRIBUTE = re.compile(
    r"(^|[^A-Za-z0-9_])SWIFT_(UNSAFE_REFERENCE|NONCOPYABLE|SHARED_REFERENCE"
    r"|IMMORTAL_REFERENCE|NAME|COMPUTED_PROPERTY|RETURNS_[A-Z]+)\b")
GUARDED_INCLUDE = re.compile(r"^[ \t]*#include <swift/bridging>", re.M)
HAS_INCLUDE = re.compile(r"__has_include\(<swift/bridging>\)")


def main(argv):
    if argv and argv[0] in ("--help", "-h"):
        print(__doc__.strip())
        return 0
    os.chdir(gate_corpus.root())

    # Tracked and untracked, so a file being added is inside the subject. The
    # hook judges staged paths, which include new files; a tracked-only listing
    # has never heard of the file whose first version carries the defect.
    sources = [path for path in gate_corpus.tracked_and_untracked()
               if path.endswith(KINDS) and not path.startswith(EXEMPT)]

    # An empty listing is a broken run, not a clean tree. The gateway check
    # below reads its file from disk, so a listing that answered nothing would
    # let every rule pass over an empty set while SwiftInterop.h still sits
    # there. The tree carries hundreds of these sources; zero means the scan
    # reached nothing.
    if not sources:
        print(f"{GATE}: the source listing returned nothing, so the rules",
              file=sys.stderr)
        print("  scanned nothing — a broken run (wrong directory, a git "
              "failure),", file=sys.stderr)
        print("  not a clean tree.", file=sys.stderr)
        return 2

    failures = 0
    rawIncludes = []
    bareAttributes = []
    # One walk for both rules. The shell form spawned processes per file, and
    # process startup was nearly all of its time.
    for path, number, line in gate_corpus.lines(sources):
        if path == GATEWAY:
            continue
        if RAW_INCLUDE.search(line):
            rawIncludes.append(f"{path}:{number}:{line}")
        if BARE_ATTRIBUTE.search(line):
            bareAttributes.append(f"{path}:{number}:{line}")

    if rawIncludes:
        print(f"  FAILED   {len(rawIncludes)} direct include(s) of "
              f"<swift/bridging>, first at {rawIncludes[0]}", file=sys.stderr)
        print("           That header ships only with the Swift toolchain, so "
              "this", file=sys.stderr)
        print("           compiles on a Mac and breaks every Linux TU. Include",
              file=sys.stderr)
        print(f"           {GATEWAY} instead.", file=sys.stderr)
        failures += 1

    if bareAttributes:
        print(f"  FAILED   {len(bareAttributes)} bare SWIFT_* attribute(s), "
              f"first at {bareAttributes[0]}", file=sys.stderr)
        print(f"           Use the UBIQUITTY_* spelling from {GATEWAY}, which "
              f"expands", file=sys.stderr)
        print("           to nothing when the Swift toolchain is absent.",
              file=sys.stderr)
        failures += 1

    # The gateway's own include has to stay conditional. Unguarded, the one
    # legitimate site becomes the break, and every file including it inherits
    # that.
    # The gateway is Core's; a repository that carries Core reads it there.
    gatewayPath = next((candidate for candidate in
                        (GATEWAY, os.path.join(ubq_roots.core_root(os.getcwd()), GATEWAY))
                        if os.path.isfile(candidate)), GATEWAY)
    if os.path.isfile(gatewayPath):
        with open(gatewayPath, errors="replace") as handle:
            gateway = handle.read()
        if GUARDED_INCLUDE.search(gateway) and not HAS_INCLUDE.search(gateway):
            print(f"  FAILED   {GATEWAY} includes <swift/bridging> with no "
                  f"__has_include guard", file=sys.stderr)
            print("           Every Linux TU that reaches this header stops "
                  "compiling.", file=sys.stderr)
            failures += 1
    else:
        # Named rather than skipped: with the gateway gone every rule above
        # still passes, on a tree where nothing republishes the attributes at
        # all.
        print(f"{GATE}: {GATEWAY} does not exist, so the rules above",
              file=sys.stderr)
        print("  compared nothing against nothing.", file=sys.stderr)
        return 2

    print()
    sys.stdout.flush()
    if failures:
        print(f"{GATE}: FAILED — {failures} site(s) reach the Swift toolchain "
              f"directly", file=sys.stderr)
        return 1

    print(f"{GATE}: ok ({len(sources)} file(s); <swift/bridging> reached "
          f"through {GATEWAY} alone)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
