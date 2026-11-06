#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""Scripts/check-binary-footprint.py — report a binary's instruction count.

  check-binary-footprint.py <binary> [baseline-count]

Instruction count is deterministic: the same tree gives the same number on every
run, on any load. That makes it the one performance-adjacent figure a gate can
compare exactly, where a timing gate on a shared machine cannot.

It is worth tracking because a growing text section slows code that did not
change — 467 commits of renaming and file reorganisation grew this binary 4.6%
with no algorithmic change anywhere, and the parse path lost ~3.5% with it.
Timing lanes caught none of it; the count would have shown the drift.

Advisory. A number that rises for a good reason is normal, and a gate that fails
a feature over the bytes that feature needs teaches the bypass.
"""

import os
import re
import subprocess
import sys

# An objdump disassembly line opens with the address, in hex, then a colon.
INSTRUCTION = re.compile(rb"^[0-9a-fA-F]+:")

GROWTH_WORTH_A_LOOK = 2.0


def instruction_count(binary):
    """Instructions objdump decodes in a binary, or None when it decodes none."""
    try:
        finished = subprocess.run(["objdump", "-d", binary],
                                  capture_output=True, check=False)
    except OSError:
        return None
    count = sum(1 for line in finished.stdout.split(b"\n")
                if INSTRUCTION.match(line))
    return count or None


def main(argv):
    if not argv or argv[0] in ("--help", "-h"):
        print("usage: check-binary-footprint.py <binary> [baseline-count]",
              file=sys.stderr)
        return 0 if argv else 1
    binary = argv[0]
    if not os.path.isfile(binary):
        print(f"not a file: {binary}", file=sys.stderr)
        return 1

    count = instruction_count(binary)
    if count is None:
        print(f"no instructions decoded — is {binary} a Mach-O binary?",
              file=sys.stderr)
        return 1

    print(f"{os.path.basename(binary)}: {count} instructions")

    if len(argv) < 2 or not argv[1]:
        return 0
    try:
        baseline = int(argv[1])
    except ValueError:
        print(f"baseline is not a count: {argv[1]}", file=sys.stderr)
        return 1
    if baseline == 0:
        print("  baseline 0, no comparison possible")
        return 0
    delta = (count - baseline) / baseline * 100
    print(f"  baseline {baseline}, delta {delta:+.2f}%")
    if delta > GROWTH_WORTH_A_LOOK:
        print(f"  text grew over {GROWTH_WORTH_A_LOOK:g}%: expected here, or "
              f"worth a look?")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
