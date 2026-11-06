#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""Whether a built binary is newer than everything it was actually compiled from.

The snapshot lane refuses a verdict when its renderer predates the sources it
renders with, and that refusal is right: a diff against a renderer nobody is
running reads as a golden regression. Comparing the newest mtime under
Source/Core and Source/Base against the binary is a file-level "anything under
Core is newer" test, when the question is whether any translation unit the
binary links reads that file.

A refusal and a rebuild that share that model agree with each other without
being right: when no TU in ubiquitty-snapshot includes the edited header,
ninja's "nothing to do" is correct, the refusal is a false positive, and the
relink it forces changes nothing but an mtime.

The real prerequisite set is on disk. ninja records what each compile actually
opened, and `-t deps` reports it from the dependency log without re-running
CMake — which matters, because CONFIGURE_DEPENDS globs make `ninja -n` re-run
CMake on any source change and that re-run masks the target's own verdict.

Measured on the snapshot tree: 1759 prerequisites, and touching a header outside
that set correctly moves nothing.

    ./Scripts/snapshot-binary-is-current.py <build-dir> <binary>

Exit 0 when current, 1 when a prerequisite is newer or the binary is absent.
Prints the first offending prerequisite, because "something changed" sends a
reader to the whole tree. Stdlib only.
"""

import json
import os
import subprocess
import sys


def prerequisites(build):
    """Every path ninja recorded a compile as having opened, absolute."""
    result = subprocess.run(["ninja", "-C", build, "-t", "deps"],
                            capture_output=True, text=True)
    if result.returncode != 0:
        return None
    found = set()
    for line in result.stdout.split("\n"):
        # `-t deps` indents each prerequisite under the target that opened it.
        if not line.startswith("    "):
            continue
        path = line.strip()
        if not path:
            continue
        # Absolute, so a caller passing a relative build directory still yields
        # paths the source list can be compared against by identity.
        found.add(os.path.abspath(path if os.path.isabs(path) else os.path.join(build, path)))
    return found


def binaryInputs(build, binary):
    """Every file ninja builds `binary` from, through the libraries it links, absolute.

    Returns None when ninja cannot say: a ninja without the `inputs` tool, or a
    binary no edge of this build produces. The compile database names every
    target the build defines, and the snapshot renderer's tree also defines the
    core-consumer sample, which building the renderer alone never compiles; the
    renderer's own inputs leave it out.
    """
    target = os.path.relpath(os.path.abspath(binary), os.path.abspath(build))
    result = subprocess.run(["ninja", "-C", build, "-t", "inputs", target],
                            capture_output=True, text=True)
    if result.returncode != 0:
        return None
    return {os.path.abspath(path if os.path.isabs(path) else os.path.join(build, path))
            for path in (line.strip() for line in result.stdout.split("\n")) if path}


def compiledSources(build):
    """The sources this build compiles, from the compile database it exported.

    Returns absolute paths, or None when the build exported none. This is the
    set the never-compiled check below must range over: what the BINARY links,
    not the whole portable Core. Every frontend takes a subset of that list
    through ubiquitty_core_subset(), and a source the subset trims is not in
    the binary — comparing against the full list named such a source as
    "linked but never compiled" for every macOS app build, and the snapshot
    tier that asked stood down on every run.
    """
    path = os.path.join(build, "compile_commands.json")
    try:
        with open(path, "r", encoding="utf-8") as handle:
            entries = json.load(handle)
    except (OSError, ValueError):
        return None
    # C-family only: the dependency log this is compared against records what
    # clang opened, and swiftc leaves it nothing, so a Swift source can never
    # appear there and would read as never compiled on every run.
    compiled = (".c", ".cc", ".cpp", ".cxx", ".m", ".mm")
    sources = set()
    for entry in entries:
        file = entry.get("file")
        if not file or not file.endswith(compiled):
            continue
        directory = entry.get("directory", build)
        sources.add(os.path.abspath(file if os.path.isabs(file) else os.path.join(directory, file)))
    return sources


def listedSources(root):
    """The portable Core translation units, from the list CMake links.

    Returns absolute paths, or None when the list cannot be read — an absent
    list is a different fact from an empty one and must not read as "nothing to
    check".
    """
    listing = os.path.join(root, "Scripts", "portable-core-sources.txt")
    try:
        with open(listing, "r", encoding="utf-8") as handle:
            lines = handle.readlines()
    except OSError:
        return None
    sources = []
    for line in lines:
        entry = line.strip()
        if entry and not entry.startswith("#"):
            sources.append(os.path.abspath(os.path.join(root, entry)))
    return sources


def main():
    if len(sys.argv) < 3:
        print("usage: snapshot-binary-is-current.py <build-dir> <binary>", file=sys.stderr)
        return 2
    build, binary = sys.argv[1], sys.argv[2]

    if not os.path.exists(binary):
        print("%s does not exist" % binary)
        return 1

    found = prerequisites(build)
    if found is None:
        # No dependency log is a different fact from a stale binary, and
        # answering "current" here would hand back a pass nothing established.
        print("no ninja dependency log in %s; staleness is undecided here" % build,
              file=sys.stderr)
        return 1
    if not found:
        print("the dependency log in %s names no prerequisite, so this compared "
              "nothing" % build, file=sys.stderr)
        return 1

    # A source the binary links but no compile ever opened. The dependency log
    # answers what was compiled, so a translation unit added since the last
    # build has no entry in it and every mtime comparison below skips a file
    # that is not in the binary at all — the predicate reports current and the
    # rebuild it gates never runs, while the snapshot lane's own mtime scan sees
    # the new file and refuses. Never compiled is not current, so it is named
    # here rather than inferred from a timestamp it has no entry to carry.
    # The build's own compile database, narrowed to the binary's inputs where
    # ninja names them, says what this binary is made of; the portable list
    # stands in only for a build that exported none, and then every portable
    # source is assumed linked, as before.
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    listed = compiledSources(build)
    if listed is not None:
        inputs = binaryInputs(build, binary)
        if inputs is not None:
            listed &= inputs
    if listed is None:
        listed = listedSources(root)
    if listed is None:
        print("no compile database in %s and no Scripts/portable-core-sources.txt "
              "under %s; staleness is undecided here" % (build, root), file=sys.stderr)
        return 1
    for source in sorted(listed):
        if source not in found and os.path.exists(source):
            print("%s is linked but was never compiled into %s"
                  % (os.path.relpath(source), os.path.relpath(binary)))
            return 1

    built = os.path.getmtime(binary)
    for path in sorted(found):
        if os.path.exists(path) and os.path.getmtime(path) > built:
            print("%s is newer than %s" % (os.path.relpath(path), os.path.relpath(binary)))
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
