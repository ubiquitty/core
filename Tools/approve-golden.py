#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0
"""approve-golden.py — mark a golden as checked by eye, so nothing re-mints it.

    Tools/approve-golden.py Tests/Snapshots/golden/agree/coretext/specimen_faces.png
    Tools/approve-golden.py <golden>... --note "healed m and w, checked at 6x"
    Tools/approve-golden.py --list

Appends one line per golden to Tests/Snapshots/approved.tsv: the repository
path, the sha256 of the bytes as they are now, today's date, and the note.
From then on run.py refuses to write that golden and check-approved-goldens.py
fails a push whose bytes differ from the hash.

An already-approved golden whose bytes have not changed is a no-op. One whose
bytes have changed is refused: the approval on file names a different picture,
and the way to replace it is to delete that line in one change, land it, and
approve the new picture in another — never to rewrite the hash in place, which
the gate also refuses. Stdlib only.
"""

import argparse
import datetime
import hashlib
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MANIFEST = ROOT / "Tests" / "Snapshots" / "approved.tsv"
GOLDEN_ROOT = ROOT / "Tests" / "Snapshots" / "golden"


def sha256Of(path):
    """The hex sha256 of a file's bytes.

    - Parameter path: The file.
    - Returns: The digest, lower-case hex.
    """
    return hashlib.sha256(path.read_bytes()).hexdigest()


def readManifest(path=MANIFEST):
    """The approved entries, in file order.

    - Parameter path: The manifest.
    - Returns: A list of (golden, sha256, approved_on, note) tuples; comment and
      blank lines are skipped.
    """
    entries = []
    if not path.exists():
        return entries
    for line in path.read_text(encoding="utf-8").split("\n"):
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        fields = line.split("\t")
        while len(fields) < 4:
            fields.append("")
        entries.append(tuple(fields[:4]))
    return entries


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("goldens", nargs="*", help="golden PNGs under Tests/Snapshots/golden")
    parser.add_argument("--note", default="", help="what was checked, in a few words")
    parser.add_argument("--list", action="store_true", help="print the approved goldens and exit")
    arguments = parser.parse_args(argv)

    if arguments.list:
        for golden, digest, on, note in readManifest():
            print(f"{on}  {digest[:12]}  {golden}  {note}".rstrip())
        return 0
    if not arguments.goldens:
        parser.error("name at least one golden, or pass --list")
    if "\t" in arguments.note or "\n" in arguments.note:
        parser.error("the note is one line with no tabs; it becomes a TSV field")

    existing = {golden: (digest, on, note) for golden, digest, on, note in readManifest()}
    today = datetime.date.today().isoformat()
    added = []
    for name in arguments.goldens:
        path = Path(name).resolve()
        try:
            relative = path.relative_to(ROOT)
        except ValueError:
            print(f"approve-golden: {name} is outside the repository", file=sys.stderr)
            return 2
        if not path.is_relative_to(GOLDEN_ROOT) or path.suffix != ".png":
            print(f"approve-golden: {relative} is not a golden under Tests/Snapshots/golden",
                  file=sys.stderr)
            return 2
        if not path.exists():
            print(f"approve-golden: {relative} does not exist", file=sys.stderr)
            return 2
        digest = sha256Of(path)
        key = relative.as_posix()
        if key in existing:
            recorded, on, _ = existing[key]
            if recorded == digest:
                print(f"approve-golden: {key} is already approved ({on}), bytes unchanged")
                continue
            print(f"approve-golden: REFUSED — {key} was approved on {on} as a different picture.",
                  file=sys.stderr)
            print("  Delete its line from Tests/Snapshots/approved.tsv in one change, land that,",
                  file=sys.stderr)
            print("  and approve the new picture in another.", file=sys.stderr)
            return 1
        added.append((key, digest, today, arguments.note))

    if not added:
        return 0
    with MANIFEST.open("a", encoding="utf-8") as handle:
        for entry in added:
            handle.write("\t".join(entry).rstrip() + "\n")
    for key, digest, on, _ in added:
        print(f"approve-golden: approved {key} ({digest[:12]}, {on})")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
