#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""Refuse a new `Unsafe*` or `__unsafe*` spelling in Swift.

A raw pointer in a Swift signature is not a Swift problem. It is the shape the
C++ importer falls back to when the declaration it is importing carries no
interop annotation, so every `UnsafeMutablePointer<Ubiquitty.Something>` in an
arm marks a Core declaration that has not said how Swift should see it. The
type to fix is the C++ one; the Swift spelling is only where it surfaces.

It also breaks things silently. A member whose signature is an OPTIONAL pointer
to a C++ type — or a reference the importer renders `inout` — is dropped from
the built module for any importing file that has not itself named that type.
The compile then fails as `value of type 'RuntimeDrive' has no member ...`,
naming the member rather than the type that could not be imported, and it fails
on one arm only. That cost a device build; see `SurfaceSelectionModel.withState`
for the shape that survives.

Some sites are not ours to remove: `MTLBuffer.contents()` returns
`UnsafeMutableRawPointer` and a `@convention(c)` callback context is a raw
pointer by definition. So this is a ratchet, not a ban — every site the tree
has today is recorded in the baseline beside it, and the gate refuses anything
the baseline does not already account for. Removing sites is always allowed;
re-record with `--write` when a file drops below its recorded count.

    python3 Scripts/check-swift-unsafe.py
    python3 Scripts/check-swift-unsafe.py --write

Stdlib only.
"""

import glob
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))

sys.path.insert(0, HERE)
import gate_corpus  # noqa: E402
import gate_report  # noqa: E402
import ubq_roots  # noqa: E402

# The Swift judged, and the sites recorded for it, are the repository's own.
REPO = ubq_roots.subject_root()
# One baseline per repository the checkout holds: Scripts/swift-unsafe-baseline.<repository>.txt.
BASELINES = sorted(glob.glob(os.path.join(REPO, "Scripts", "swift-unsafe-baseline.*.txt")))

GATE = "check-swift-unsafe.py"

# `Unsafe` as an identifier head, and the `__unsafe` attribute family. Bounded
# on the left so a word merely ending in them (a comment's "theUnsafe") is not
# a hit, and on the right by the identifier rule so the whole name is taken.
PATTERN = re.compile(r"(?<![A-Za-z0-9_])(__unsafe[A-Za-z0-9_]*|Unsafe[A-Za-z0-9_]*)")


def swift_sources():
    # Tracked and untracked files alike: a file being added is where a new
    # spelling arrives, and this gate's full-tree pass is the only guard.
    return sorted(p for p in gate_corpus.tracked_and_untracked() if p.endswith(".swift"))


def scan(lines):
    """Every hit in a sequence of lines, as (line number, identifier, text).

    Split from `occurrences` so the self-test can drive the rule without a file
    on disk — the rule is what needs pinning, not the reading.
    """
    found = []
    for number, text in enumerate(lines, start=1):
        # A doc comment naming the type it is explaining is prose, not a use.
        stripped = text.lstrip()
        if stripped.startswith("//"):
            continue
        for match in PATTERN.finditer(text):
            found.append((number, match.group(1), text.strip()))
    return found


def occurrences(path):
    """Every hit in one file."""
    full = os.path.join(REPO, path)
    try:
        with open(full, encoding="utf-8") as handle:
            return scan(handle.read().splitlines())
    except (OSError, UnicodeDecodeError):
        return []


def selfTest():
    caught = [
        "    let p: UnsafeMutablePointer<Ubiquitty.SelectionState>",
        "func f(_ b: UnsafeRawBufferPointer) {}",
        "    @__unsafe_no_objc_tagged_pointer var x = 0",
    ]
    # Swift 6 isolation annotations fall outside this rule: the pattern is
    # anchored to `Unsafe`/`__unsafe` as an identifier head, and
    # `nonisolated(unsafe)` carries neither. Pinned because a concurrency
    # sweep meets this gate and was told so.
    clear = [
        "    nonisolated(unsafe) static var shared = Registry()",
        "    let notUnsafeAtAll = 1",
        "    // UnsafeMutablePointer is what the importer falls back to",
    ]
    ok = (len(scan(caught)) == 3
          and [h[1] for h in scan(caught)] == ["UnsafeMutablePointer", "UnsafeRawBufferPointer", "__unsafe_no_objc_tagged_pointer"]
          and not scan(clear))
    print("  %s a raw pointer is caught, nonisolated(unsafe), a suffix and a comment are not"
          % ("ok " if ok else "FAIL"))
    print("check-swift-unsafe: self-test %s" % ("ok" if ok else "FAILED"))
    return 0 if ok else 1


def read_baseline():
    """The recorded count of every path, over the checkout's baselines.

    - Returns: (counts, home): the count per path, and the baseline file each
      path is recorded in.
    """
    counts, home = {}, {}
    for baseline in BASELINES:
        with open(baseline, encoding="utf-8") as handle:
            for line in handle:
                line = line.strip()
                if not line or line.startswith("#"):
                    continue
                count, _, path = line.partition(" ")
                path = path.strip()
                if path in counts:
                    raise SystemExit("%s: %s is recorded in %s as well" % (GATE, path, home[path]))
                counts[path] = int(count)
                home[path] = baseline
    return counts, home


def write_baseline(counts, home):
    """Re-records every baseline from the counts, each path in the file it is in.

    A path no baseline records is a new site, which a ratchet does not take in;
    it is refused here, and added by hand to the baseline of the repository that
    owns the file.
    """
    unrecorded = sorted(path for path in counts if counts[path] and path not in home)
    if unrecorded:
        raise SystemExit("%s: not recorded in any baseline, and a baseline takes in no new site: %s"
                         % (GATE, ", ".join(unrecorded)))
    for baseline in BASELINES:
        with open(baseline, encoding="utf-8") as handle:
            header = [line for line in handle if line.startswith("#")]
        mine = sorted(path for path in counts if counts[path] and home.get(path) == baseline)
        with open(baseline, "w", encoding="utf-8") as handle:
            handle.writelines(header)
            for path in mine:
                handle.write("%d %s\n" % (counts[path], path))


def main(argv):
    if "--self-test" in argv[1:]:
        return selfTest()

    writing = "--write" in argv[1:]
    baseline, home = read_baseline()
    current = {}
    detail = {}
    for path in swift_sources():
        hits = occurrences(path)
        if hits:
            current[path] = len(hits)
            detail[path] = hits

    if writing:
        write_baseline(current, home)
        total = sum(current.values())
        print("recorded %d sites across %d files" % (total, len(current)))
        return 0

    offenders = []
    for path in sorted(current):
        allowed = baseline.get(path, 0)
        if current[path] > allowed:
            for number, name, text in detail[path][allowed:]:
                offenders.append("%s:%d: %s — %s" % (path, number, name, text))

    if offenders:
        gate_report.failed(
            GATE,
            "Swift gained an Unsafe* spelling the baseline does not account for. "
            "Annotate the C++ declaration so the importer renders a real type, or "
            "reach it through a helper that names the type once "
            "(SurfaceSelectionModel.withState). If the site is an Apple API that "
            "hands out a raw pointer, re-record with --write and say why in review.",
            offenders,
        )
        return 1

    stale = [p for p in baseline if current.get(p, 0) < baseline[p]]
    total = sum(current.values())
    if stale:
        gate_report.ok(
            GATE,
            "no new Unsafe* in Swift (%d sites left; %d file(s) now below the "
            "baseline — re-record with --write)" % (total, len(stale)),
        )
        return 0
    gate_report.ok(GATE, "no new Unsafe* in Swift (%d sites left)" % total)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
