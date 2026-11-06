#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""check-memcmp-fold.py — no memcmp against a literal at a runtime offset into a
constant array.

GCC folds `memcmp(&constantArray[runtimeIndex], constant, n)` as though the index
were zero, so a search over a constant array silently never matches. Reproduced
on 13.3 and 15.2, on both aarch64 and x86_64 — including the g++ 15.2 of the
image CI's Linux jobs build in. Clang emits a real load at the offset and is
correct, so a build on macOS alone never shows it.

The second operand may be a string literal or another constant array; PR 95189
describes a related fold that needs a zero byte in one operand, and this one does
not — an array with no zero bytes against a needle with no terminator misfolds
identically. So the shape is keyed on both operands being readable at compile
time, not on the literal.

`-fno-builtin-memcmp` cures it and costs 45% on a 32-byte compare, the shape of
cellEqual, so the flag is not applied globally and the shape is kept out of the
source instead. A translation unit that genuinely needs it can carry the flag
through set_source_files_properties.

Scoped to a first operand the compiler can evaluate: only a constant array can be
misfolded, so a runtime buffer at a runtime offset is correct and must not be
reported. A gate that fires on correct code is the one people learn to bypass.

Compare the bytes, or bind the address to a variable first. Both are correct
everywhere; the first says why it is written that way.
"""

import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import gate_corpus

GATE = "check-memcmp-fold"

# `&name[expr]` or `name + expr` as the first argument; second operand either a
# string literal or a bare identifier that may itself be a constant array.
CALL = re.compile(
    r'(?:\b|__builtin_)(?:mem|strn)cmp\(\s*'
    r'(?:&\s*(\w+)\s*\[[^]]+\]|(\w+)\s*\+\s*\w+)\s*,\s*(?:"|(\w+)\s*[,)])')

SOURCES = (".cpp", ".h", ".mm")


def without_comments(text):
    """The text with comment bodies removed and its line numbering intact.

    Comments describing the defect are not the defect.
    """
    stripped = re.sub(r"/\*.*?\*/",
                      lambda match: "\n" * match.group(0).count("\n"),
                      text, flags=re.S)
    return "\n".join(re.sub(r"//.*$", "", line) for line in stripped.split("\n"))


def is_constant_array(identifier, text):
    """An array whose contents the compiler can read at the call site.

    Const-qualified with an initializer, in any spelling — `static const`, plain
    `const`, `constexpr`, and a function-local const array all misfold; a mutable
    array does not. The initializer is what separates a real constant array from
    a `const T name[]` PARAMETER, which is a pointer to bytes the compiler cannot
    see and is correct.
    """
    if not identifier:
        return False
    return bool(re.search(
        r"\b(?:const|constexpr)\b[^;\n]*?\b" + re.escape(identifier)
        + r"\s*\[[^]]*\]\s*=", text))


def offenders(paths):
    """Every misfolding comparison in the corpus, as (path, line, text, name)."""
    found = []
    for path, text in gate_corpus.texts(paths):
        stripped = without_comments(text)
        for number, line in enumerate(stripped.split("\n"), start=1):
            match = CALL.search(line)
            if not match:
                continue
            name = match.group(1) or match.group(2)
            needle = match.group(3)
            # Only operands the compiler can read through can be misfolded. A
            # string literal always qualifies; a named needle has to be a
            # constant array.
            if not is_constant_array(name, stripped):
                continue
            if needle is not None and not is_constant_array(needle, stripped):
                continue
            found.append((path, number, line.strip(), name))
    return found


def main(argv):
    if argv and argv[0] in ("--help", "-h"):
        print(__doc__.strip())
        return 0
    os.chdir(gate_corpus.root())

    if argv and argv[0] == "--staged":
        paths = [path for path in gate_corpus.staged()
                 if path.endswith(SOURCES)]
    else:
        # tracked, not tracked-and-untracked: the --staged pass above is what
        # guards the commit and it carries added files, so this pass is a survey
        # of what the tree already holds.
        paths = [path for path in gate_corpus.tracked(["*.cpp", "*.h", "*.mm"])
                 if not path.startswith("Source/ThirdParty/")]

    if not paths:
        print(f"{GATE}: no source files to scan")
        return 0

    found = offenders(paths)
    if found:
        for path, number, line, name in found:
            print(f"  {path}:{number}: {line}")
            print(f"      `{name}` is a constant array, so this comparison "
                  f"folds to the wrong answer")
        print()
        print(f"{GATE}: memcmp/strncmp against a literal at a runtime offset into a")
        print("  constant array (above). GCC folds these to the offset-zero answer — one")
        print("  specific wrong value at every index, not a failure to match.")
        print("  Compare the bytes, or bind the address to a variable first.")
        print("  __builtin_memcmp misfolds identically and is not an escape.")
        return 1

    print(f"{GATE}: ok ({len(paths)} source file(s) scanned)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
