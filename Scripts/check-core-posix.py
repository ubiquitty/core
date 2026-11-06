#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""check-core-posix.py — Core does not talk to a POSIX kernel.

Core is the portable half. A POSIX header included there is a syscall the Linux,
macOS, web and headless frontends each have to answer for, and the one that
cannot answer is the one that stops building. The QUIC carrier is the worked
example: its contract lives in Core in spans and callbacks with no fd and no
sockaddr, and its sockets live in Platform/POSIX/QUICClient.cpp. An entry added
back to the ledger below needs a reason that survives that example.

  Scripts/check-core-posix.py [root]

Exit 0 = clean, 1 = a new dependency or a stale ledger entry.
"""

import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import gate_corpus

GATE = "check-core-posix"

# Headers that mean "this file talks to a POSIX kernel". The pattern omits
# <sys/types.h> and <cstdio>: they are portable enough that flagging them would
# train the reader to ignore this check.
PATTERN = re.compile(
    r"#include <(unistd|sys/socket|sys/un|sys/wait|sys/stat|sys/mman|poll"
    r"|fcntl|pthread|signal|termios|dirent|netinet/[a-z]+|arpa/inet)\.h>")

# file -> why its implementation cannot live in Platform.
#
# A ledger that outlives the debt is its own bug: an entry naming a file that no
# longer includes POSIX means someone fixed it without telling this script, and
# the next reader trusts a stale count. Both directions are checked below.
KNOWN = {}


def including_files(root):
    """Files under Source/Core that include a POSIX header, and their lines."""
    found = {}
    paths = [path for path in gate_corpus.tracked_and_untracked()
             if path.startswith("Source/Core/")]
    for path, text in gate_corpus.texts(sorted(paths)):
        hits = [(number, line)
                for number, line in enumerate(text.split("\n"), start=1)
                if PATTERN.search(line)]
        if hits:
            found[path] = hits
    return found


def main(argv):
    if argv and argv[0] in ("--help", "-h"):
        print(__doc__.strip())
        return 0
    os.chdir(argv[0] if argv else gate_corpus.root())

    found = including_files(".")

    new = 0
    for path in sorted(found):
        if path in KNOWN:
            continue
        print(f"{GATE}: NEW POSIX dependency in Core: {path}", file=sys.stderr)
        for number, line in found[path]:
            # Line and text without the path: the heading above already names
            # the file, so each line reads as a single-file `grep -n` prints it.
            print(f"    {number}:{line}", file=sys.stderr)
        new += 1

    stale = 0
    for path in sorted(KNOWN):
        if not os.path.isfile(path):
            print(f"{GATE}: ledger names a file that no longer exists: {path}",
                  file=sys.stderr)
            stale += 1
        elif path not in found:
            print(f"{GATE}: {path} no longer needs POSIX — remove it from the "
                  f"ledger", file=sys.stderr)
            stale += 1

    if new or stale:
        print(f"{GATE}: FAILED ({new} new, {stale} stale)", file=sys.stderr)
        return 1

    print(f"{GATE}: PASSED — {len(found)} known, 0 new "
          f"(see Scripts/check-core-posix.py for the ledger)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
