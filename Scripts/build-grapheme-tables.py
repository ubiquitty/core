#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""Generate Source/Core/Unicode/GraphemeBreak.tables.inc from the Unicode
Character Database (UCD).

UAX #29 assigns every codepoint a Grapheme_Cluster_Break property.
GraphemeBreak.cpp needs the ranges for the classes that carry a rule:
Extend (GB9), SpacingMark (GB9a), Prepend (GB9b) and the Hangul jamo
classes L/V/T (GB6/7/8). ZWJ, Regional_Indicator and the precomposed
Hangul syllable block are single ranges the caller adds directly.

Inputs (downloaded from https://www.unicode.org/Public/UCD/):
  GraphemeBreakProperty.txt   — the Grapheme_Cluster_Break property
  DerivedCoreProperties.txt   — Indic_Conjunct_Break, for GB9c
  emoji-data.txt              — Extended_Pictographic, for GB11

Output is a C++ header fragment defining sorted, coalesced range arrays
plus a Unicode-version banner. GraphemeBreak.cpp #includes it.

Usage:
  Scripts/build-grapheme-tables.py \\
      --gcb GraphemeBreakProperty.txt \\
      --coreprops DerivedCoreProperties.txt \\
      --emoji-data emoji-data.txt \\
      --unicode-version 17.0.0 \\
      --output Source/Core/Unicode/GraphemeBreak.tables.inc

Every table the rules read comes from this one run, so they carry one UCD
version between them. Extended_Pictographic is emitted exactly, not widened:
a range reaching past the property reaches over assigned code points rather
than the reserved positions such widening is meant for, and each one it
swallows joins across a ZWJ under GB11.
"""

import argparse
import sys
from pathlib import Path

# Grapheme_Cluster_Break values that GraphemeBreak.cpp keeps a table for,
# mapped to the C array name.
EMITTED = [
    ('Extend', 'extend'),
    ('SpacingMark', 'spacingMark'),
    ('Prepend', 'prepend'),
    ('L', 'hangulL'),
    ('V', 'hangulV'),
    ('T', 'hangulT'),
]

# graphemeClassRanges[] in GraphemeBreak.cpp is fixed size, and entries past
# the end are dropped rather than reported. Fail here instead.
CLASS_RANGE_CAP = 1024
# ZWJ, Regional_Indicator and the Hangul syllable block are added by the caller.
CALLER_ADDED_RANGES = 3


def parse_gcb(path):
    """GraphemeBreakProperty.txt lines:
        XXXX          ; Extend # comment
        XXXX..YYYY    ; SpacingMark # comment
    Return {property: set of codepoints}."""
    props = {}
    for line in path.open(encoding='utf-8'):
        line = line.split('#')[0].strip()
        if not line or ';' not in line:
            continue
        rng, prop = [part.strip() for part in line.split(';')[:2]]
        if '..' in rng:
            first, last = rng.split('..')
        else:
            first = last = rng
        props.setdefault(prop, set()).update(
            range(int(first, 16), int(last, 16) + 1))
    return props


def parse_incb(path):
    """DerivedCoreProperties.txt lines for the Indic_Conjunct_Break property:
        XXXX          ; InCB; Linker # comment
    Return {value: set of codepoints}."""
    props = {}
    for line in path.open(encoding='utf-8'):
        line = line.split('#')[0].strip()
        if not line or ';' not in line:
            continue
        parts = [part.strip() for part in line.split(';')]
        if len(parts) < 3 or parts[1] != 'InCB':
            continue
        rng, value = parts[0], parts[2]
        if '..' in rng:
            first, last = rng.split('..')
        else:
            first = last = rng
        props.setdefault(value, set()).update(
            range(int(first, 16), int(last, 16) + 1))
    return props


def parse_emoji_data(path):
    """emoji-data.txt lines:
        XXXX          ; Extended_Pictographic # comment
        XXXX..YYYY    ; Emoji_Presentation # comment
    Return {property: set of codepoints}."""
    props = {}
    for line in path.open(encoding='utf-8'):
        line = line.split('#')[0].strip()
        if not line or ';' not in line:
            continue
        rng, prop = [part.strip() for part in line.split(';')[:2]]
        if '..' in rng:
            first, last = rng.split('..')
        else:
            first = last = rng
        props.setdefault(prop, set()).update(
            range(int(first, 16), int(last, 16) + 1))
    return props


def coalesce(cps):
    """Take a set of codepoints, return a sorted list of (first, last)
    inclusive ranges. Adjacent codepoints merge; gaps start a new range."""
    if not cps:
        return []
    ordered = sorted(cps)
    ranges = []
    start = prev = ordered[0]
    for cp in ordered[1:]:
        if cp == prev + 1:
            prev = cp
            continue
        ranges.append((start, prev))
        start = prev = cp
    ranges.append((start, prev))
    return ranges


def emit_table(name, ranges, fh):
    fh.write(f'const Range {name}[] = {{\n')
    for first, last in ranges:
        fh.write(f'    {{ 0x{first:06X}, 0x{last:06X} }},\n')
    fh.write('};\n\n')


def emit_incb_block_bitmap(tables, fh):
    """A bit per 256-codepoint block saying whether it holds any InCB
    consonant or linker. The GB9c probes test this first, so Latin, CJK,
    Hangul and emoji reject in a shift and a mask instead of a binary
    search over ranges that span from Devanagari into plane 1."""
    blocks = set()
    for name in ('incbConsonant', 'incbLinker'):
        for first, last in tables[name]:
            blocks.update(range(first >> 8, (last >> 8) + 1))
    highest = max(blocks)
    size = (highest >> 3) + 1
    bitmap = bytearray(size)
    for block in blocks:
        bitmap[block >> 3] |= 1 << (block & 7)
    lowest = min(first for first, _ in tables['incbLinker'])
    fh.write(f'constexpr std::uint32_t incbLowestLinker = 0x{lowest:06X};\n')
    fh.write(f'constexpr std::uint32_t incbHighestBlock = 0x{highest:03X};\n')
    fh.write('const std::uint8_t incbBlockBitmap[] = {\n')
    for i in range(0, size, 12):
        row = ', '.join(f'0x{b:02X}' for b in bitmap[i:i + 12])
        fh.write(f'    {row},\n')
    fh.write('};\n\n')


def main(argv):
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--gcb', required=True, type=Path)
    ap.add_argument('--coreprops', required=True, type=Path)
    ap.add_argument('--emoji-data', required=True, type=Path)
    ap.add_argument('--unicode-version', required=True)
    ap.add_argument('--output', required=True, type=Path)
    args = ap.parse_args(argv)

    for path in (args.gcb, args.coreprops, args.emoji_data):
        if not path.exists():
            print(f'error: {path} does not exist', file=sys.stderr)
            return 1

    props = parse_gcb(args.gcb)
    missing = [prop for prop, _ in EMITTED if prop not in props]
    if missing:
        print(f'error: {args.gcb} has no {", ".join(missing)} property',
              file=sys.stderr)
        return 1

    tables = [(name, coalesce(props[prop])) for prop, name in EMITTED]

    # GB9c needs Indic_Conjunct_Break. Consonant is not folded
    # into the GCB class byte: that byte drives isGraphemeRelevant(), and
    # marking 900-odd Indic consonants relevant would push ordinary Devanagari
    # off the caller's fast path. Extend is not emitted either — it is exactly
    # GCB Extend plus ZWJ minus ZWNJ, which the rule tests directly.
    incb = parse_incb(args.coreprops)
    for prop, name in (('Consonant', 'incbConsonant'), ('Linker', 'incbLinker')):
        if prop not in incb:
            print(f'error: {args.coreprops} has no InCB={prop}', file=sys.stderr)
            return 1
        tables.append((name, coalesce(incb[prop])))

    # GB11 needs Extended_Pictographic. Not a GCB class: it is probed directly
    # by the rule, and folding it into the class byte would mark every
    # pictograph grapheme-relevant and take plain emoji off the fast path.
    emoji = parse_emoji_data(args.emoji_data)
    if 'Extended_Pictographic' not in emoji:
        print(f'error: {args.emoji_data} has no Extended_Pictographic property',
              file=sys.stderr)
        return 1
    tables.append(('extPict', coalesce(emoji['Extended_Pictographic'])))

    # incbConsonant becomes a GCB class of its own, so it counts against the
    # cap; incbLinker and extPict are probed directly and do not.
    class_tables = [t for t in tables if t[0] not in ('incbLinker', 'extPict')]
    total = sum(len(ranges) for _, ranges in class_tables) + CALLER_ADDED_RANGES
    if total > CLASS_RANGE_CAP:
        print(f'error: {total} ranges exceeds graphemeClassRanges[{CLASS_RANGE_CAP}] '
              f'in GraphemeBreak.cpp; raise the cap alongside this table',
              file=sys.stderr)
        return 1

    with args.output.open('w', encoding='utf-8') as fh:
        fh.write('// Copyright (C) 2026 Stanislav Yaglo\n')
        fh.write('// Copyright © 1991-2026 Unicode, Inc.\n')
        fh.write('// SPDX-License-Identifier: Apache-2.0 AND Unicode-3.0\n\n')
        fh.write(f'/* Generated from UCD {args.unicode_version} — do not edit by hand.\n')
        fh.write(' * Regenerate via Scripts/build-grapheme-tables.py; inputs:\n')
        fh.write(' * GraphemeBreakProperty.txt, DerivedCoreProperties.txt and\n')
        fh.write(' * emoji-data.txt from the Unicode UCD. */\n\n')
        for name, ranges in tables:
            emit_table(name, ranges, fh)
        emit_incb_block_bitmap(dict(tables), fh)

    summary = ', '.join(f'{name} {len(r)}' for name, r in tables)
    print(f'wrote {args.output}: {summary} ({total} of {CLASS_RANGE_CAP} class ranges)')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
