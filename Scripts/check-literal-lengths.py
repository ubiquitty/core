#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""A hand-written length beside a literal has to be the literal's length.

`std::string_view("…", N)` states a count the compiler will not check against
the string it sits next to. Where the two disagree the test still passes, so the
disagreement survives: the scene the literal describes is not the scene the code
feeds, and every assertion downstream is about the shorter or longer one.

@ubiquitty-layers found eleven of these while converting the VT ingest seam to
take one value. Nine fed less than they read as and two fed one byte more,
reading the literal's terminating NUL into a parser. The worst was a pair
writing `EDIT-A` and `EDIT-B` to tell two rows apart, at a length that stopped
after `EDI` — the names discriminated and the bytes did not.

The count is decidable from the literal, which is what makes this a gate rather
than a convention. C's `\\x` escape consumes as many hex digits as follow it, so
the decoder below stops at the first non-hex character exactly as a compiler
does; `\\0` through `\\7` take up to three octal digits.

    ./Scripts/check-literal-lengths.py

Exits 1 naming every site where the two disagree, with both numbers. Stdlib
only.
"""

import os
import re
import subprocess
import sys


HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import ubq_roots  # noqa: E402

REPO = ubq_roots.subject_root()

SUFFIXES = (".cpp", ".cc", ".h", ".hpp", ".mm", ".m")

# The literal and the count that claims to be its length.
SIZED_LITERAL = re.compile(r'std::string_view\(\s*("(?:[^"\\]|\\.)*")\s*,\s*([0-9]+)\s*\)')


def literalLength(literal):
    """The byte count a compiler gives the literal, escapes resolved.

    - Parameter literal: The source text of the literal, quotes included.
    - Returns: Its length in bytes.
    """
    body = literal[1:-1]
    index = 0
    length = 0
    while index < len(body):
        if body[index] != "\\":
            index += 1
            length += 1
            continue
        marker = body[index + 1]
        if marker == "x":
            # Hexadecimal takes every hex digit that follows, however many.
            end = index + 2
            while end < len(body) and body[end] in "0123456789abcdefABCDEF":
                end += 1
            index = end
        elif marker in "01234567":
            end = index + 1
            digits = 0
            while end < len(body) and digits < 3 and body[end] in "01234567":
                end += 1
                digits += 1
            index = end
        else:
            index += 2
        length += 1
    return length


def disagreements(paths):
    """Sites whose stated count is not the length of the literal beside it.

    - Parameter paths: Repository-relative files to read.
    - Returns: `(path, line, stated, actual, literal)` for each disagreement.
    """
    found = []
    for path in paths:
        if not path.endswith(SUFFIXES):
            continue
        try:
            with open(os.path.join(REPO, path), encoding="utf-8", errors="replace") as handle:
                lines = handle.read().split("\n")
        except OSError:
            continue
        for number, line in enumerate(lines, 1):
            for literal, stated in SIZED_LITERAL.findall(line):
                actual = literalLength(literal)
                if actual != int(stated):
                    found.append((path, number, int(stated), actual, literal))
    return found


def main():
    listing = subprocess.run(["git", "-C", REPO, "ls-files", "--cached", "--others",
                              "--exclude-standard"], capture_output=True, text=True)
    paths = [path for path in listing.stdout.split("\n")
             if path and not path.startswith("Source/ThirdParty/")]
    if not paths:
        print("check-literal-lengths: no files listed, so nothing was compared",
              file=sys.stderr)
        return 2

    found = disagreements(paths)
    scanned = sum(1 for path in paths if path.endswith(SUFFIXES))
    if not found:
        print("check-literal-lengths: ok (%d file(s); every stated length is its "
              "literal's)" % scanned)
        return 0

    print("check-literal-lengths: FAILED — %d stated length(s) disagree with the "
          "literal" % len(found), file=sys.stderr)
    for path, number, stated, actual, literal in found:
        print("  %s:%d: states %d, literal is %d  %s"
              % (path, number, stated, actual, literal[:56]), file=sys.stderr)
    print("  The count wins at runtime, so the bytes fed are not the bytes "
          "written down.", file=sys.stderr)
    print("  Drop the count where the literal is the scene, or correct it where "
          "a prefix is.", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
