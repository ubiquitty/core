#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""Generate Source/Core/Unicode/Normalize.tables.inc from the Unicode Character
Database (UCD).

Ubiquitty's Normalize module implements the four Unicode normalization
forms (NFC / NFD / NFKC / NFKD) from Unicode Standard Annex #15. The
canonical/compatibility decomposition mappings and the canonical
combining classes that drive the algorithm are too large to hand-curate,
so this script turns them into a deterministic build from authoritative
Unicode source data.

Inputs (downloaded from https://www.unicode.org/Public/UCD/):
  UnicodeData.txt             — canonical combining class (field 3) and
                                decomposition mapping (field 5)
  CompositionExclusions.txt   — code points barred from recomposition

Hangul (U+AC00..U+D7A3) decomposition/composition is algorithmic per
UAX #15 section 18 and lives in Normalize.cpp directly; it is not
tabulated here.

Output is a C++ header fragment that defines:
  - combiningClassRanges[]   sorted {first,last,ccc} runs
  - decompData[]             flat code-point pool for all mappings
  - decompMappings[]         sorted {codePoint, offset, length, compat}
  - compositionPairs[]       sorted-by-(starter,combining) {a,b,result}

Usage:
  Scripts/build-normalization-tables.py \\
      --unicodedata UnicodeData.txt \\
      --exclusions CompositionExclusions.txt \\
      --unicode-version 15.1.0 \\
      --output Source/Core/Unicode/Normalize.tables.inc
"""

import argparse
import sys


def parse_unicodedata(path):
    ccc = {}            # cp -> combining class
    decomp = {}         # cp -> (compat: bool, [cps])
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.rstrip("\n")
            if not line:
                continue
            fields = line.split(";")
            cp = int(fields[0], 16)
            klass = int(fields[3])
            if klass != 0:
                ccc[cp] = klass
            dm = fields[5].strip()
            if dm:
                compat = dm.startswith("<")
                if compat:
                    # Drop the <tag> prefix, keep the code points.
                    dm = dm[dm.index(">") + 1:].strip()
                cps = [int(x, 16) for x in dm.split()]
                decomp[cp] = (compat, cps)
    return ccc, decomp


def parse_exclusions(path):
    excl = set()
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.split("#", 1)[0].strip()
            if not line:
                continue
            if ".." in line:
                lo, hi = line.split("..")
                for cp in range(int(lo, 16), int(hi, 16) + 1):
                    excl.add(cp)
            else:
                excl.add(int(line, 16))
    return excl


def coalesce(items):
    """items: sorted list of (cp, value) -> list of (first, last, value)."""
    ranges = []
    for cp, value in items:
        if ranges and ranges[-1][2] == value and ranges[-1][1] == cp - 1:
            ranges[-1] = (ranges[-1][0], cp, value)
        else:
            ranges.append((cp, cp, value))
    return ranges


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--unicodedata", required=True)
    ap.add_argument("--exclusions", required=True)
    ap.add_argument("--unicode-version", required=True)
    ap.add_argument("--output", required=True)
    args = ap.parse_args()

    ccc, decomp = parse_unicodedata(args.unicodedata)
    exclusions = parse_exclusions(args.exclusions)

    # --- combining-class ranges -------------------------------------
    ccc_items = sorted(ccc.items())
    ccc_ranges = coalesce(ccc_items)

    # --- decomposition pool + index ---------------------------------
    # Mappings are stored RAW (one decomposition level only) with their
    # canonical/compatibility tag; Normalize.cpp recurses. This keeps a
    # canonical mapping whose results still carry compatibility mappings
    # (e.g. U+0385 -> 00A8 0301, and 00A8 -> <compat> 0020 0308) correct for
    # both NFD (00A8 stays) and NFKD (00A8 expands), which a pre-expanded
    # table can't capture in one row.
    pool = []
    mappings = []           # (cp, offset, length, compat)
    for cp in sorted(decomp.keys()):
        compat, seq = decomp[cp]
        offset = len(pool)
        pool.extend(seq)
        mappings.append((cp, offset, len(seq), 1 if compat else 0))

    # --- composition pairs (for NFC/NFKC) ---------------------------
    # A primary canonical composition: cp has a 2-char canonical
    # decomposition [a,b], a is a starter, cp is not a composition
    # exclusion, and cp is not itself excluded (singleton/non-starter).
    pairs = []
    for cp in sorted(decomp.keys()):
        compat, cps = decomp[cp]
        if compat or len(cps) != 2:
            continue
        a, b = cps
        if cp in exclusions:
            continue
        # A non-starter first element bars composition (UAX #15: only
        # starters compose). Singletons (len 1) already filtered above.
        if ccc.get(a, 0) != 0:
            continue
        pairs.append((a, b, cp))
    pairs.sort(key=lambda p: (p[0], p[1]))

    # --- emit -------------------------------------------------------
    lines = ["// Copyright (C) 2026 Stanislav Yaglo",
             "// Copyright © 1991-2026 Unicode, Inc.",
             "// SPDX-License-Identifier: Apache-2.0 AND Unicode-3.0", ""]
    lines.append("/* Unicode normalization tables — generated, pinned at "
                 f"Unicode {args.unicode_version}.")
    lines.append(" * Source: UnicodeData.txt + CompositionExclusions.txt.")
    lines.append(" * Regenerate with Scripts/build-normalization-tables.py; "
                 "do NOT hand-edit.")
    lines.append(" * Hangul is algorithmic (see Normalize.cpp), not tabulated "
                 "here.")
    lines.append(" */")
    lines.append("")

    lines.append("static const CombiningClassRange combiningClassRanges[] = {")
    for first, last, klass in ccc_ranges:
        lines.append(f"    {{ 0x{first:06X}, 0x{last:06X}, {klass} }},")
    lines.append("};")
    lines.append("")

    lines.append("static const std::uint32_t decompData[] = {")
    row = []
    for v in pool:
        row.append(f"0x{v:06X}")
        if len(row) == 8:
            lines.append("    " + ", ".join(row) + ",")
            row = []
    if row:
        lines.append("    " + ", ".join(row) + ",")
    lines.append("};")
    lines.append("")

    lines.append("static const DecompMapping decompMappings[] = {")
    for cp, offset, length, compat in mappings:
        lines.append(f"    {{ 0x{cp:06X}, {offset}, {length}, {compat} }},")
    lines.append("};")
    lines.append("")

    lines.append("static const CompositionPair compositionPairs[] = {")
    for a, b, result in pairs:
        lines.append(f"    {{ 0x{a:06X}, 0x{b:06X}, 0x{result:06X} }},")
    lines.append("};")
    lines.append("")

    with open(args.output, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))

    sys.stderr.write(
        f"wrote {args.output}: {len(ccc_ranges)} ccc ranges, "
        f"{len(mappings)} decomp mappings, {len(pool)} pool entries, "
        f"{len(pairs)} composition pairs\n")


if __name__ == "__main__":
    main()
