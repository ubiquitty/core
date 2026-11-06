#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""check-build-dirs.py — one build directory configured as two build types.

A CMake cache records the build type that created it. Two scripts pointing at
one directory with different `-DCMAKE_BUILD_TYPE` do not conflict, do not warn,
and do not fail: whichever ran last wins, and the binary sitting at that path
afterwards is whatever the loser's user did not ask for.

The failure never surfaces as a wrong binary. A daemon built `-O0` answers every
request correctly and takes longer, so it is indistinguishable across the
wire from a slow one — the cost lands as frame rate, and measurements taken
against it read as facts about the machine rather than about the build. Two
people lost an hour each to one such directory before anyone suspected the path.

What this compares is the pair (directory, build type) across every script,
because a single script is self-consistent by construction and the collision
only exists between two of them.

Variables are resolved in file order: `HOST_BUILD="$UBQ_BUILD_ROOT/host-tests"`
followed by `cmake -B "$HOST_BUILD"` names build/host-tests. A stanza that
reassigns the variable rebinds it from that line on, which matches how the
shell reads it.
"""

import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import build_root
import gate_report

GATE = "check-build-dirs"

# A `cmake -B <dir>` anywhere on the logical line, and the build type anywhere
# on that same logical line. They are written adjacent but wrap across
# continuations, so the line is joined before either is looked for.
CONFIGURE = re.compile(r'\bcmake\s+-B\s+"?([^"\s]+)"?')
# The type is written as a literal at most sites and as "$BUILD_TYPE" at the
# rest. Matching only literals reads a variable site as having no type at all,
# and two such sites then agree with each other — so the one comparison that
# matters is the one that never happens.
BUILD_TYPE = re.compile(r'-DCMAKE_BUILD_TYPE=(?:"([^"]*)"|([^\s;"]+))')
ASSIGNMENT = re.compile(r'^\s*([A-Z][A-Z0-9_]*)="([^"]*)"\s*$')


def logical_lines(text):
    """Yields (line number of the first physical line, joined text).

    A configure call spans continuations, so the directory and the build type
    sit on different physical lines. Joining is what lets one regex see both.
    """
    number, buffer, start = 0, "", 0
    for number, line in enumerate(text.split("\n"), 1):
        if not buffer:
            start = number
        stripped = line.rstrip()
        if stripped.endswith("\\"):
            buffer += stripped[:-1] + " "
            continue
        yield start, buffer + stripped
        buffer = ""
    if buffer:
        yield start, buffer


def expand(value, variables):
    """Substitutes `$VAR` and `${VAR}` from what has been seen above."""
    def one(match):
        name = match.group(1) or match.group(2)
        return variables.get(name, match.group(0))
    for _ in range(4):  # $A -> $B -> literal; four rounds is past any real chain
        expanded = re.sub(r'\$\{([A-Za-z_]\w*)\}|\$([A-Za-z_]\w*)', one, value)
        if expanded == value:
            break
        value = expanded
    return value


def configures_in(path, root):
    """Yields (directory, build type, line number) for each configure call."""
    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        text = handle.read()

    # ROOT is computed with `cd`/`pwd` rather than assigned a literal, so it is
    # seeded here; an unresolved one would make every directory look distinct
    # and the check would pass by comparing nothing.
    # UBQ_BUILD_ROOT is established by sourcing Scripts/build-root.sh, which
    # this parser does not follow, so it is seeded beside ROOT. Left
    # unresolved, every configure call looks distinct and the collision check
    # compares nothing.
    variables = {"ROOT": root,
                 "UBQ_BUILD_ROOT": build_root.build_root(root)}

    for number, line in logical_lines(text):
        assignment = ASSIGNMENT.match(line)
        if assignment:
            variables[assignment.group(1)] = expand(assignment.group(2), variables)
            continue
        if line.lstrip().startswith("#"):
            continue
        configure = CONFIGURE.search(line)
        if not configure:
            continue
        directory = expand(configure.group(1), variables)
        if "$" in directory:
            continue  # a directory this cannot resolve is not one it can judge
        match = BUILD_TYPE.search(line)
        if match:
            build_type = expand(match.group(1) or match.group(2), variables)
            # A type set from an argument or a conditional assignment cannot be
            # followed here. Naming it after its text would let two such sites
            # compare equal, which is the one answer this must never give: the
            # question is whether they agree, and an unknown does not.
            if "$" in build_type:
                build_type = "unresolved"
        else:
            build_type = "unset"
        yield os.path.normpath(directory), build_type, number


def main(argv):
    root = argv[1] if len(argv) > 1 and not argv[1].startswith("--") else "."
    root = os.path.abspath(root)
    scripts = os.path.join(root, "Scripts")

    # directory -> build type -> list of "file:line"
    seen = {}
    for name in sorted(os.listdir(scripts)):
        if not name.endswith(".sh"):
            continue
        path = os.path.join(scripts, name)
        for directory, build_type, number in configures_in(path, root):
            where = "%s:%d" % (os.path.relpath(path, root), number)
            seen.setdefault(directory, {}).setdefault(build_type, []).append(where)

    if not seen:
        print(f"{GATE}: no cmake configure calls found - the parser is", file=sys.stderr)
        print("  matching nothing, which is not the same as nothing being wrong.", file=sys.stderr)
        return gate_report.refused(GATE, "no CMake configure calls found", location="Scripts")

    # Two sites disagree, or one of them is a type this cannot follow and there
    # is a second site to disagree with it. A lone unresolved site has nothing
    # to contradict, so it is left alone rather than made permanently red.
    def isConflicting(types):
        sites = sum(len(where) for where in types.values())
        return len(types) > 1 or ("unresolved" in types and sites > 1)

    conflicts = {d: types for d, types in seen.items() if isConflicting(types)}
    for directory, types in sorted(conflicts.items()):
        relative_directory = os.path.relpath(directory, root)
        for build_type in sorted(types):
            for where in types[build_type]:
                gate_report.finding(
                    where, GATE,
                    f"{relative_directory} is configured as {build_type}; "
                    f"the same directory also has {', '.join(sorted(set(types) - {build_type}))}")

    if conflicts:
        print("", file=sys.stderr)
        print(f"{GATE}: {len(conflicts)} directory/directories hold two build types.",
              file=sys.stderr)
        print("  A CMake cache holds one. Whichever script ran last decides what the", file=sys.stderr)
        print("  binary at that path is, and nothing at the point of use says which.", file=sys.stderr)
        print("  Give the variant its own directory, as build/host-noquic does.", file=sys.stderr)
        return gate_report.failed_count(GATE, len(conflicts),
                                        "build directories hold two build types")

    return gate_report.ok(GATE, f"{len(seen)} build directories, one build type each")


if __name__ == "__main__":
    sys.exit(main(sys.argv))
