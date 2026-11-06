#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0
"""check-mirror-modules-do-not-parse.py — a mirrored Terminal has no parser.

`Terminal::currentCapabilityOrigin` is value-initialised, and `Local` is the
first enumerator, so every Terminal in the tree carries Local unless something
says otherwise. Nothing ever stamps `Agent` or `Remote`: the four explicit stamps
are all Local. That is not an oversight, and this gate is what keeps it from
becoming one.

It is correct because of a structural fact rather than a stamp. A Terminal whose
parser runs is fed by its own session, so Local is true of it. A Terminal fed
over the sync protocol is a mirror: `SyncClientCore` applies snapshots and row
deltas straight into the grid and never calls `processBytes`, so a mirrored
Terminal reaches no capability gate site at all and its origin is never asked.
Local is then unread rather than wrong.

Route host bytes into a mirror's parser and both halves fail at once: the gate
sites become reachable, and they answer for a principal the wire does not carry.
The daemon knows who is driving; `CapabilityOrigin` has no field for it. The fix
at that point is a protocol change, which is a great deal larger than the line
that caused it.

So the rule is that a module holding a `SyncClientCore` does not feed a parser.
The modules are derived rather than listed, so a mirror added later is covered
without anyone remembering this file.

Web is the one exception, and the reason it needs its own gates rather than a
line here: it both mirrors a host and parses its own bytes, which is the pair
this rule forbids everywhere else. `Scripts/check-web-parser-entries.py` records each of its parser
entries with why the bytes are local, and `Scripts/check-web-host-bytes.py` holds
the page half. The exemption is a pointer at two gates that check it, not a hole.

Stdlib only.

    python3 Scripts/check-mirror-modules-do-not-parse.py
    python3 Scripts/check-mirror-modules-do-not-parse.py --self-test
"""

import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gate_report  # noqa: E402 — the path above is what makes it importable

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ubq_roots  # noqa: E402
REPO = ubq_roots.subject_of(__file__)

MIRROR = re.compile(r"\bSyncClientCore\b")
PARSER = re.compile(r"\bprocessBytes\s*\(")

# Where a parser entry is recorded with its reason. Named so a reader who hits
# this gate can see the exemption is checked rather than assumed.
WEB = os.path.join("Source", "Platform", "Web")
WEB_GATES = "Scripts/check-web-parser-entries.py and Scripts/check-web-host-bytes.py"

# Sync's own implementation holds the type and is what the rule is about, not a
# consumer of it.
SYNC = os.path.join("Source", "Core", "Protocol", "Sync")

# Shipping code only. A test drives a host Terminal with processBytes and mirrors
# it through SyncClientCore in one file — that pairing is the test's whole
# subject, and
# Tests/Core/Protocol/Sync/test_sync.cpp plus Tests/Performance/profile_stack.cpp
# do exactly that today. Reporting them would be reporting the coverage.
SHIPPING = "Source" + os.sep


def tracked():
    listing = subprocess.run(
        ["git", "-C", REPO, "ls-files", "--cached", "--others", "--exclude-standard"],
        capture_output=True, text=True)
    return [path for path in listing.stdout.split("\n") if path]


def sources():
    """Tracked C-family sources, as (repo-relative path, text)."""
    for path in tracked():
        if not path.endswith((".c", ".cpp", ".mm", ".m", ".h", ".hpp")):
            continue
        try:
            with open(os.path.join(REPO, path), encoding="utf-8", errors="replace") as handle:
                yield path, handle.read()
        except OSError:
            continue


def offenders(files):
    """Mirror files that also feed a parser, most useful first.

    - Parameter files: (path, text) pairs.
    - Returns: A sorted list of repo-relative paths.
    """
    found = []
    for path, text in files:
        if not path.startswith(SHIPPING):
            continue
        if path.startswith(SYNC) or path.startswith(WEB):
            continue
        if not MIRROR.search(text):
            continue
        # A comment naming processBytes is prose about the rule, not a call —
        # this file and the Web gates are full of exactly that sentence.
        code = "\n".join(line.split("//")[0] for line in text.split("\n"))
        if PARSER.search(code):
            found.append(path)
    return sorted(found)


def selfTest():
    ok = True
    clean = [("Source/Core/Session/RemoteSessionCore.cpp", "SyncClientCore m_sync;\napplySnapshot(x);")]
    if offenders(clean):
        print("self-test: a mirror that does not parse was reported", file=sys.stderr)
        ok = False

    dirty = [("Source/Core/Session/RemoteSessionCore.cpp",
              "SyncClientCore m_sync;\nUbiquitty::processBytes(&t, bytes);")]
    if offenders(dirty) != ["Source/Core/Session/RemoteSessionCore.cpp"]:
        print("self-test: a mirror that parses was not reported", file=sys.stderr)
        ok = False

    commented = [("Source/Core/Session/RemoteSessionCore.cpp",
                  "SyncClientCore m_sync;\n// never calls processBytes(...) — see the gate")]
    if offenders(commented):
        print("self-test: a comment naming processBytes was read as a call", file=sys.stderr)
        ok = False

    tests = [("Tests/Core/Protocol/Sync/test_sync.cpp",
              "SyncClientCore client;\nUbiquitty::processBytes(&host, bytes);")]
    if offenders(tests):
        print("self-test: a test driving both sides was reported as a violation", file=sys.stderr)
        ok = False

    exempt = [(os.path.join(WEB, "WebTerminal.cpp"),
               "SyncClientCore m_sync;\nUbiquitty::processBytes(&t, bytes);")]
    if offenders(exempt):
        print("self-test: Web was reported, though its entries are recorded elsewhere", file=sys.stderr)
        ok = False

    print("check-mirror-modules-do-not-parse: self-test %s" % ("ok" if ok else "FAILED"))
    return 0 if ok else 1


def main():
    if "--self-test" in sys.argv[1:]:
        return selfTest()

    files = list(sources())
    mirrors = [path for path, text in files
               if path.startswith(SHIPPING) and MIRROR.search(text)
               and not path.startswith(SYNC)]
    if not mirrors:
        print("check-mirror-modules-do-not-parse: FAILED — no file outside %s names "
              "SyncClientCore, so the rule was applied to nothing" % SYNC, file=sys.stderr)
        return 1

    bad = offenders(files)
    if bad:
        status = gate_report.failed("check-mirror-modules-do-not-parse",
                                    "mirror file(s) that feed a parser", bad)
        print("\n  A Terminal filled from the wire reaches no capability gate site, which is\n"
              "  why CapabilityOrigin::Local is never wrong on one. Feeding its parser makes\n"
              "  those sites reachable and they answer for a principal the wire does not\n"
              "  carry — the daemon knows who is driving and the protocol has no field for\n"
              "  it. Apply host bytes through SyncClientCore, or carry the origin on the\n"
              "  wire first.\n"
              "  Web is exempt here and checked by %s." % WEB_GATES, file=sys.stderr)
        return status

    print("check-mirror-modules-do-not-parse: ok (%d mirror file(s) outside Web, none "
          "feeds a parser; Web's entries are recorded by %s)" % (len(mirrors), WEB_GATES))
    return 0


if __name__ == "__main__":
    sys.exit(main())
