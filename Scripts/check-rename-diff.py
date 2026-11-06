#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""check-rename-diff.py — every line a rename changed is explained by the rename.

A rename tool that miscomputes offsets writes correct-length spans at wrong
positions. `clang-refactor local-rename -i` does this on any rename with a
length delta: `Cursor::Cursor::CursorStyle` is 8 bytes longer than `Cursor::CursorStyle`, so
after the first replacement every later offset in that file is 8 bytes stale and
the tool overwrites 11-byte spans wherever the arithmetic points.

    -    bool visible; // blinkAlpha >= 0.5 — discrete on/off gate
    +    bool visible; //Cursor::CursorStyle >= 0.5 — discrete on/off gate

The renames in the same file were correct. Three targeted checks passed —
literal qualifiers, the protocol token, the five known damage classes — because
each was aimed at a hazard someone had already named, and this landed in the
sixth thing. Checking the hazards you can name is a proxy for "the change is
clean".

This asks the question that is not a proxy: undo the rename on every added line
and require the result to be a line that was removed. A line the rename explains
survives that round trip; a line the tool corrupted does not, whatever it
corrupted and wherever.

    ./Scripts/check-rename-diff.py --old Cursor::CursorStyle --new Cursor::Cursor::CursorStyle
    ./Scripts/check-rename-diff.py --old X --new Y --staged

Two questions, because passing the first alone is how a crash gets through.
Whether every changed line is EXPLAINED is the round trip above. Whether the
change is COMPLETE is separate: clang-refactor stack-overflows on multi-TU
invocations after writing the first file, and the files it never reached have no
diff at all. Every line it did write un-renames correctly, so a round trip over
the diff passes a half-finished rename clean — the damage is not in any line.

Remaining occurrences of the old name answer that, and are reported rather than
assumed wrong: a rename is not always meant to be total — one namespace's symbol
can be renamed while another namespace keeps the same spelling. What must not
happen is passing silently either way.

What it cannot ask. The round trip confirms a changed line is EXPLAINED by the
rename. It cannot say the rename should have touched that line at all — a
comment rewritten by a bare-identifier sweep un-renames onto its own removed
line perfectly and passes:

    -    // preserving the current blink (odd = blink, 0 = default)
    +    // preserving the current isBlinking (odd = isBlinking, 0 = default)

That is a correct rename of prose that should not have been renamed, and it is
indistinguishable here from a correct rename of code. Bare-identifier sweeps
reach comments; qualified ones mostly do not, which is why this bites the
predicate-led renames a naming lint asks for rather than the namespace moves.
Nothing below replaces reading a diff that touches comments.

It is one-directional on purpose: it can pass damage that happens to de-rename
onto some other removed line, and it never invents a finding. Run it against the
whole working tree after a mechanical rewrite, before reading the diff.
"""

import argparse
import re
import subprocess
import sys


def changedLines(staged):
    """Maps each changed file to (removed lines, added lines)."""
    command = ["git", "diff", "--unified=0", "--no-color"]
    if staged:
        command.insert(2, "--cached")
    diff = subprocess.run(command, capture_output=True, text=True)
    files = {}
    path = None
    for line in diff.stdout.split("\n"):
        if line.startswith("+++ b/"):
            path = line[6:]
            files.setdefault(path, ([], []))
        elif path and line.startswith("-") and not line.startswith("---"):
            files[path][0].append(line[1:])
        elif path and line.startswith("+") and not line.startswith("+++"):
            files[path][1].append(line[1:])
    return files


def main(argv):
    parser = argparse.ArgumentParser(add_help=True)
    parser.add_argument("--old", required=True, help="the name before the rename")
    parser.add_argument("--new", required=True, help="the name after it")
    parser.add_argument("--staged", action="store_true", help="check the index, not the worktree")
    arguments = parser.parse_args(argv[1:])

    # Longest-first so `Cursor::Cursor::CursorStyle` is undone before a bare
    # `Cursor::CursorStyle` inside it turns the line into something neither spelling.
    undo = [(arguments.new, arguments.old)]
    if arguments.new.endswith(arguments.old) and arguments.new != arguments.old:
        undo.append((arguments.old, arguments.old))

    # The new name usually CONTAINS the old one, and `git grep -w` happily
    # matches inside it because `::` is not a word character — so every
    # successfully renamed occurrence would count as one left behind. Blanking
    # the new name first is what makes the count mean what it says.
    tracked = subprocess.run(["git", "ls-files"], capture_output=True, text=True)
    survivor = re.compile(r"\b%s\b" % re.escape(arguments.old))
    remaining = []
    for path in tracked.stdout.split("\n"):
        if not path or path == "Scripts/check-rename-diff.py":
            continue
        try:
            with open(path, encoding="utf-8", errors="replace") as handle:
                body = handle.read()
        except OSError:
            continue
        for number, line in enumerate(body.split("\n"), 1):
            if survivor.search(line.replace(arguments.new, "")):
                remaining.append("%s:%d:%s" % (path, number, line.strip()))

    unexplained = []
    for path, (removed, added) in sorted(changedLines(arguments.staged).items()):
        pool = {}
        for line in removed:
            pool[line] = pool.get(line, 0) + 1
        for line in added:
            restored = line
            for source, target in undo:
                # Word-boundary, not substring. A pluralising rename puts the
                # new name inside identifiers that already carried the plural —
                # Theme -> Themes turns registerThemesFromValues back into
                # registerConfigTheme, and the restored line then matches
                # nothing that was removed. The finding is benign in sign and
                # expensive in attention: three correct lines flagged at the
                # moment a reviewer is least able to tell.
                restored = re.sub(r"\b%s\b" % re.escape(source), target, restored)
            if pool.get(restored, 0) > 0:
                pool[restored] -= 1
                continue
            # A line that is genuinely new (not a rewrite) has no pre-image, and
            # is reported: after a mechanical rename there should not be any.
            unexplained.append((path, line))

    for path, line in unexplained:
        print("%s: not explained by the rename:" % path, file=sys.stderr)
        print("    %s" % line.strip()[:110], file=sys.stderr)

    if remaining:
        print("", file=sys.stderr)
        print("check-rename-diff: %d occurrence(s) of %s remain."
              % (len(remaining), arguments.old), file=sys.stderr)
        for line in remaining[:8]:
            print("    %s" % line[:110], file=sys.stderr)
        print("  A rename the tool abandoned part-way leaves files with NO diff, so the", file=sys.stderr)
        print("  round trip above passes it clean. If the rename is meant to be partial,", file=sys.stderr)
        print("  say which of these are meant to keep the old name; otherwise the tool", file=sys.stderr)
        print("  did not reach them.", file=sys.stderr)

    if unexplained:
        print("", file=sys.stderr)
        print("check-rename-diff: %d changed line(s) the rename does not account for."
              % len(unexplained), file=sys.stderr)
        print("  Undoing %s -> %s on each did not reproduce a line that was removed."
              % (arguments.new, arguments.old), file=sys.stderr)
        return 1
    if remaining:
        return 1
    print("check-rename-diff: every changed line is explained by %s -> %s, "
          "and no occurrence of %s remains" % (arguments.old, arguments.new, arguments.old))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
