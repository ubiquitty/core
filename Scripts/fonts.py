#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

#
# Scripts/fonts.py — the bundled fonts, checked against and refreshed from
# Resources/fonts/manifest.json.
#
#   ./Scripts/fonts.py check           every bundled file is the one recorded
#   ./Scripts/fonts.py check --strict  the above, and every hash is recorded
#   ./Scripts/fonts.py update NAME     re-fetch one font at its pinned version
#   ./Scripts/fonts.py stamp           record the hashes of what is on disk
#
# A font is a binary nobody reads in review. A swapped, truncated or
# silently-upgraded one passes every human check and moves every golden that
# renders text, so the thing worth gating is the bytes.
#
# `check` fails on a hash that does not match and on a missing licence file.
# Bundling redistributes the font, and the OFL requires the licence to travel
# with it; a licence that goes missing in a file move is not visible any other
# way.
#
# An UNRECORDED hash is reported but does not fail by default. Most of the
# bundle predates this manifest, and stamping it retroactively would record a
# provenance nobody established — the hash would then say the file is the one
# intended, on no evidence but its presence. `--strict` fails on an unrecorded
# hash, for a tree where every entry has been stamped against a known source.
#
# Axes are checked where recorded, and that check has a specific failure in
# mind: a variable face replaced by a static build of the same family renders
# perfectly, so the snapshots pinning the weight and width settings keep
# passing while the settings themselves stop reaching anything. Nothing else in
# the tree would notice.
#

import argparse
import hashlib
import json
import os
import shutil
import sys
import tempfile
import urllib.request
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FONT_DIRECTORY = os.path.join(ROOT, "Resources", "fonts")
MANIFEST = os.path.join(FONT_DIRECTORY, "manifest.json")


def load():
    with open(MANIFEST) as handle:
        return json.load(handle)


def resolved(relative):
    return os.path.normpath(os.path.join(FONT_DIRECTORY, relative))


def digest(path):
    sha = hashlib.sha256()
    with open(path, "rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            sha.update(block)
    return sha.hexdigest()


def axesOf(path):
    """The variation axis tags a font file carries, read from its own `fvar`.

    Parsed out of the binary rather than asked of a tool. The obvious tool is
    fc-scan, and on this development host it answers `variable: False` for a
    face CoreText reads three axes from — a checker built on it would have
    reported every variable font static, which is the safe-looking direction
    and therefore the dangerous one. The table is the fact; a library's opinion
    of the table is not.

    `fvar` is a fixed-layout OpenType table: a header naming where the axis
    records start and how large each is, then one record per axis opening with
    its four-byte tag. Walking it needs no font library and cannot be absent.

    - Parameter path: The font file to read.
    - Returns: The set of axis tags, empty for a static face, or None when the
      file is not a single-font sfnt this can walk (a collection, say) — which
      the caller must read as "not checked" rather than "none".
    """
    import struct

    try:
        with open(path, "rb") as handle:
            data = handle.read()
    except OSError:
        return None
    if len(data) < 12:
        return None
    # A collection holds several fonts behind a different header; this walks one.
    if data[:4] == b"ttcf":
        return None
    if data[:4] not in (b"\x00\x01\x00\x00", b"OTTO", b"true", b"typ1"):
        return None

    tableCount = struct.unpack(">H", data[4:6])[0]
    fvarOffset = None
    for index in range(tableCount):
        record = 12 + index * 16
        if record + 16 > len(data):
            return None
        tag = data[record:record + 4]
        if tag == b"fvar":
            fvarOffset = struct.unpack(">I", data[record + 8:record + 12])[0]
            break
    if fvarOffset is None:
        return set()  # a static face: read the table, it is not there
    if fvarOffset + 16 > len(data):
        return None

    axesArrayOffset, _, axisCount, axisSize = struct.unpack(">HHHH", data[fvarOffset + 4:fvarOffset + 12])
    tags = set()
    for index in range(axisCount):
        record = fvarOffset + axesArrayOffset + index * axisSize
        if record + 4 > len(data):
            return None
        tags.add(data[record:record + 4].decode("ascii", "replace"))
    return tags


def check(strict):
    manifest = load()
    problems = []
    unstamped = 0
    checkedAxes = 0

    for font in manifest["fonts"]:
        name = font["name"]

        licence = font.get("licence_file")
        if licence:
            path = resolved(licence)
            if not os.path.exists(path):
                problems.append(f"{name}: licence file missing: {licence}")

        for entry in font["files"]:
            path = resolved(entry["path"])
            if not os.path.exists(path):
                problems.append(f"{name}: bundled file missing: {entry['path']}")
                continue
            recorded = entry.get("sha256", "")
            if not recorded:
                unstamped += 1
                if strict:
                    problems.append(f"{name}: no recorded hash for {entry['path']} (--strict)")
                continue
            actual = digest(path)
            if actual != recorded:
                problems.append(
                    f"{name}: {entry['path']} is not the recorded file\n"
                    f"    recorded {recorded}\n"
                    f"    on disk  {actual}")

        wanted = font.get("axes") or []
        if wanted and font["files"]:
            path = resolved(font["files"][0]["path"])
            if os.path.exists(path):
                present = axesOf(path)
                if present is None:
                    print(f"fonts: {name}: axes not checked ({font['files'][0]['path']} is not a single-font sfnt)")
                else:
                    checkedAxes += 1
                    missing = [tag for tag in wanted if tag not in present]
                    if missing:
                        problems.append(
                            f"{name}: bundled to carry {', '.join(wanted)} and is missing "
                            f"{', '.join(missing)} — a static build would render fine and "
                            f"leave the settings it pins unexercised")

    if problems:
        for problem in problems:
            print(f"fonts: FAILED — {problem}")
        print(f"fonts: {len(problems)} problem(s) across {len(manifest['fonts'])} font(s).")
        return 1

    note = f", {unstamped} file(s) with no recorded hash" if unstamped else ""
    axes = f", {checkedAxes} axis set(s) verified" if checkedAxes else ", no axis set verified"
    print(f"fonts: ok ({len(manifest['fonts'])} font(s), licences present{note}{axes})")
    return 0


def stamp():
    manifest = load()
    changed = 0
    for font in manifest["fonts"]:
        for entry in font["files"]:
            path = resolved(entry["path"])
            if not os.path.exists(path):
                continue
            actual = digest(path)
            if entry.get("sha256") != actual:
                entry["sha256"] = actual
                changed += 1
    with open(MANIFEST, "w") as handle:
        json.dump(manifest, handle, indent=2)
        handle.write("\n")
    print(f"fonts: stamped {changed} hash(es) from the files on disk.")
    print("fonts: a stamp records what is there — it does not verify where it came from.")
    return 0


def update(name):
    manifest = load()
    match = [f for f in manifest["fonts"] if f["name"].lower() == name.lower()]
    if not match:
        print(f"fonts: no font named {name!r}. Known: "
              + ", ".join(f["name"] for f in manifest["fonts"]))
        return 2
    font = match[0]

    if font.get("generated_by"):
        print(f"fonts: {font['name']} is generated by {font['generated_by']}, not downloaded.")
        print(f"fonts: run that script with the upstream source from {font['source']}.")
        return 2
    if font.get("url"):
        url = font["url"]
        asset = url.rsplit("/", 1)[-1]
        print(f"fonts: fetching {asset} from {url}")
    elif font.get("version") and font.get("asset"):
        url = f"{font['source']}/releases/download/{font['version']}/{font['asset']}"
        asset = font["asset"]
        print(f"fonts: fetching {asset} from {font['source']} at {font['version']}")
    else:
        print(f"fonts: {font['name']} records no pinned version and asset, and no url, so "
              f"there is nothing to re-fetch reproducibly. Add `version` and `asset`, or `url`.")
        return 2
    with tempfile.TemporaryDirectory() as work:
        archive = os.path.join(work, asset)
        try:
            urllib.request.urlretrieve(url, archive)
        except OSError as error:
            print(f"fonts: FAILED — could not fetch {url}: {error}")
            return 1
        # One archive, every file of the family: a per-file `member` names each,
        # and the font-level `member` is the first file's when it stands alone.
        targets = []
        for index, entry in enumerate(font["files"]):
            member = entry.get("member") or (font.get("member") if index == 0 else None)
            target = resolved(entry["path"])
            targets.append((target, entry))
            if member:
                with zipfile.ZipFile(archive) as bundle:
                    if member not in bundle.namelist():
                        print(f"fonts: FAILED — {member!r} is not in {asset}. "
                              f"The release layout moved; update `member`.")
                        return 1
                    with bundle.open(member) as source, open(target, "wb") as out:
                        shutil.copyfileobj(source, out)
            else:
                shutil.copyfile(archive, target)
    target, first = targets[0]
    actual = digest(target)
    recorded = first.get("sha256", "")
    if recorded and actual != recorded:
        print(f"fonts: FAILED — the fetched file does not match the recorded hash.")
        print(f"    recorded {recorded}")
        print(f"    fetched  {actual}")
        print("fonts: the pinned version now serves different bytes. Establish why "
              "before stamping over it.")
        return 1
    print(f"fonts: {font['name']} is at {font['version']}, {actual}")
    return 0


def main():
    parser = argparse.ArgumentParser(description="Check or refresh the bundled fonts.")
    sub = parser.add_subparsers(dest="command", required=True)
    checker = sub.add_parser("check", help="verify the bundle against the manifest")
    checker.add_argument("--strict", action="store_true",
                         help="also fail on a file with no recorded hash")
    sub.add_parser("stamp", help="record the hashes of the files on disk")
    updater = sub.add_parser("update", help="re-fetch one font at its pinned version")
    updater.add_argument("name")

    args = parser.parse_args()
    if args.command == "check":
        return check(args.strict)
    if args.command == "stamp":
        return stamp()
    return update(args.name)


if __name__ == "__main__":
    sys.exit(main())
