#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""Generate Source/Core/Unicode/Width.tables.inc from Unicode Character
Database (UCD) source files.

Ubiquitty's Width module classifies each codepoint as zero-width
(combining marks, format controls), narrow (default), or wide
(East Asian Wide + Full-width + default-emoji-presentation). This
script builds the classification deterministically from authoritative
Unicode source data.

Inputs (one of each, downloaded from https://www.unicode.org/Public/UCD/):
  EastAsianWidth.txt          — wide-vs-narrow primary signal
  emoji-data.txt              — Emoji_Presentation property
  DerivedGeneralCategory.txt  — Mn / Me / Cf for zero-width
  DerivedCoreProperties.txt   — Default_Ignorable_Code_Point for zero-width

Output is a C++ header fragment that defines two sorted, coalesced
range arrays (zeroWidthRanges[], wideRanges[]) plus a Unicode-version
banner. Width.cpp #includes the fragment and exposes the public API.

Usage:
  Scripts/build-unicode-tables.py \\
      --eaw EastAsianWidth.txt \\
      --emoji emoji-data.txt \\
      --gencat DerivedGeneralCategory.txt \\
      --coreprops DerivedCoreProperties.txt \\
      --unicode-version 16.0.0 \\
      --output Source/Core/Unicode/Width.tables.inc

Policy notes:
  - FORCE_WIDE_RANGES widens code points AGAINST their EAW, as a product
    decision rather than a UCD reading. Each entry is EAW=N upstream and
    would be dropped by a clean regeneration without it.
  - EAW Ambiguous (A) is treated as narrow per Width.cpp's documented
    contract. Changing it takes a CLI gate; hand-editing the
    generated file is overwritten by the next run.
  - The Mn/Me/Cf set has well-known exceptions (ARABIC TATWEEL
    U+0640 is Mn but visible). The exceptions list is encoded in
    EXCLUDE_FROM_ZERO_WIDTH below; bump it when Unicode adds new
    exceptions.
  - Zero-width also absorbs Default_Ignorable_Code_Point=Yes (from
    DerivedCoreProperties). DI is a superset of the relevant Cf
    formats plus non-Cf invisibles the general category misses —
    SOFT HYPHEN U+00AD, the Hangul fillers U+115F/U+1160/U+3164/
    U+FFA0, the E0000 tag block. A renderer must not paint or
    advance the cursor for a lone DI codepoint (tofu / ghost cell);
    grapheme-relevant DI codepoints (ZWJ, variation selectors, the
    tag block) are still consumed by clustering first, so this only
    governs the standalone case.
  - U+1F3FB..U+1F3FF (emoji modifier Fitzpatrick skin tones) are
    forced to zero width — terminals render them as glyph composers
    that combine with the preceding base emoji, not as standalone
    cells.

Regeneration:
  1. Download the four UCD files for the target Unicode version
     from https://www.unicode.org/Public/UCD/<version>/ucd/.
  2. Run this script.
  3. Diff the new inc file against the checked-in version.
  4. Commit the diff if it matches the UCD change; a diff that does not is a
     bug in this script.
"""

import argparse
import re
import sys
from pathlib import Path

# Codepoints that DerivedGeneralCategory marks as Mn / Me / Cf but
# which renderers treat as visible single-cell glyphs (or which are
# handled by adjacent zero-width ranges already and would just create
# noisy duplicates after coalescing). Curated; add new entries as the
# Unicode spec evolves.
EXCLUDE_FROM_ZERO_WIDTH = {
    0x0640,   # ARABIC TATWEEL — Mn but visible
}

# Codepoints that should always be zero width regardless of their
# general category. Skin-tone modifiers compose with a preceding base
# emoji; the renderer paints the combined glyph in one cell.
FORCE_ZERO_WIDTH_RANGES = [
    (0x1F3FB, 0x1F3FF),    # Emoji modifier Fitzpatrick types 1-2..6
    (0xFE00, 0xFE0F),      # Variation Selectors
    (0xFE20, 0xFE2F),      # Combining Half Marks
    (0xE0100, 0xE01EF),    # Variation Selectors Supplement
    (0x2028, 0x2029),      # LINE / PARAGRAPH SEPARATOR — Zl/Zp, so neither
                           # the Mn/Me/Cf set nor Default_Ignorable covers
                           # them, but a terminal paints no cell for either
]


# Code points this product renders two cells wide AGAINST their East_Asian_Width.
#
# This is not a reading of the UCD; every range below is EAW=N (Neutral) upstream, which
# the standard, wcwidth and other terminals all render one cell. Ubiquitty
# renders them wide as a product decision, so a regeneration from a clean UCD
# would silently drop the widening unless it is stated here — which is why this
# list exists rather than a hand edit to the generated file.
#
# A width that contradicts the UCD with no note beside it reads as a bug and
# gets "corrected" by the next reader. The note is the point.
FORCE_WIDE_RANGES = [
]


def parse_eaw(path):
    """EastAsianWidth.txt lines:
        XXXX;K # comment             (single codepoint)
        XXXX..YYYY;W # comment       (range)
    Width values: A, F, H, N, Na, W. Return the set of codepoints
    that are W or F."""
    wide = set()
    rng = re.compile(r'^([0-9A-Fa-f]+)(?:\.\.([0-9A-Fa-f]+))?\s*;\s*(\w+)')
    for line in path.read_text(encoding='utf-8').splitlines():
        line = line.split('#', 1)[0].strip()
        if not line:
            continue
        m = rng.match(line)
        if not m:
            continue
        first = int(m.group(1), 16)
        last = int(m.group(2), 16) if m.group(2) else first
        width = m.group(3)
        if width in ('W', 'F'):
            for cp in range(first, last + 1):
                wide.add(cp)
    return wide


def parse_emoji_presentation(path):
    """emoji-data.txt entries with property `Emoji_Presentation`. Same
    line format as EAW (range or single + ; PROP # comment)."""
    out = set()
    rng = re.compile(r'^([0-9A-Fa-f]+)(?:\.\.([0-9A-Fa-f]+))?\s*;\s*(\w+)')
    for line in path.read_text(encoding='utf-8').splitlines():
        line = line.split('#', 1)[0].strip()
        if not line:
            continue
        m = rng.match(line)
        if not m:
            continue
        if m.group(3) != 'Emoji_Presentation':
            continue
        first = int(m.group(1), 16)
        last = int(m.group(2), 16) if m.group(2) else first
        for cp in range(first, last + 1):
            out.add(cp)
    return out


def parse_general_category(path):
    """DerivedGeneralCategory.txt — returns the union of Mn (Nonspacing
    Mark), Me (Enclosing Mark), Cf (Format) codepoints."""
    out = set()
    rng = re.compile(r'^([0-9A-Fa-f]+)(?:\.\.([0-9A-Fa-f]+))?\s*;\s*(\w+)')
    keep = {'Mn', 'Me', 'Cf'}
    for line in path.read_text(encoding='utf-8').splitlines():
        line = line.split('#', 1)[0].strip()
        if not line:
            continue
        m = rng.match(line)
        if not m:
            continue
        if m.group(3) not in keep:
            continue
        first = int(m.group(1), 16)
        last = int(m.group(2), 16) if m.group(2) else first
        for cp in range(first, last + 1):
            out.add(cp)
    return out


def parse_default_ignorable(path):
    """DerivedCoreProperties.txt — return the set of codepoints with
    property `Default_Ignorable_Code_Point`. Same line format as the
    other UCD property files (range or single + ; PROP # comment)."""
    out = set()
    rng = re.compile(r'^([0-9A-Fa-f]+)(?:\.\.([0-9A-Fa-f]+))?\s*;\s*(\w+)')
    for line in path.read_text(encoding='utf-8').splitlines():
        line = line.split('#', 1)[0].strip()
        if not line:
            continue
        m = rng.match(line)
        if not m:
            continue
        if m.group(3) != 'Default_Ignorable_Code_Point':
            continue
        first = int(m.group(1), 16)
        last = int(m.group(2), 16) if m.group(2) else first
        for cp in range(first, last + 1):
            out.add(cp)
    return out


def coalesce(cps):
    """Take a set of codepoints, return a sorted list of (first, last)
    inclusive ranges. Adjacent codepoints (cp, cp+1) merge into the
    same range; gaps create new ranges."""
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
    fh.write(f'static const WidthRange {name}[] = {{\n')
    for first, last in ranges:
        fh.write(f'    {{ 0x{first:06X}, 0x{last:06X} }},\n')
    fh.write('};\n\n')


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--eaw', required=True, type=Path)
    ap.add_argument('--emoji', required=True, type=Path)
    ap.add_argument('--gencat', required=True, type=Path)
    ap.add_argument('--coreprops', required=True, type=Path)
    ap.add_argument('--unicode-version', required=True)
    ap.add_argument('--output', required=True, type=Path)
    args = ap.parse_args(argv)

    for p in (args.eaw, args.emoji, args.gencat, args.coreprops):
        if not p.exists():
            print(f'error: {p} does not exist', file=sys.stderr)
            return 1

    eaw_wide = parse_eaw(args.eaw)
    emoji_pres = parse_emoji_presentation(args.emoji)
    mark_format = parse_general_category(args.gencat)
    default_ignorable = parse_default_ignorable(args.coreprops)

    # Wide = EAW W ∪ EAW F ∪ Emoji_Presentation ∪ the product's forced-wide
    # ranges. Subtract anything that is also force-zero-width (skin tones,
    # variation selectors).
    forced_zero = set()
    for first, last in FORCE_ZERO_WIDTH_RANGES:
        forced_zero.update(range(first, last + 1))
    forced_wide = set()
    for first, last in FORCE_WIDE_RANGES:
        forced_wide.update(range(first, last + 1))
    wide_set = (eaw_wide | emoji_pres | forced_wide) - forced_zero

    # Zero-width = (Mn ∪ Me ∪ Cf ∪ Default_Ignorable) − exceptions + forced
    # ranges. Default_Ignorable adds the non-Cf invisibles the general category
    # misses (SOFT HYPHEN, the Hangul fillers, the E0000 tag block) so a lone
    # DI codepoint occupies no cell.
    zero_set = ((mark_format | default_ignorable) - EXCLUDE_FROM_ZERO_WIDTH) | forced_zero

    # Zero-width wins over wide for any overlap: the Hangul Choseong Filler
    # U+115F is EAW Wide yet Default_Ignorable, and must collapse to no cell.
    wide_set -= zero_set

    # Sanity: a codepoint must not be both zero-width and wide.
    overlap = wide_set & zero_set
    if overlap:
        sample = sorted(overlap)[:5]
        print(f'error: {len(overlap)} codepoints classified as both '
              f'wide and zero-width: {[hex(x) for x in sample]}',
              file=sys.stderr)
        return 1

    zero_ranges = coalesce(zero_set)
    wide_ranges = coalesce(wide_set)
    # Emoji_Presentation as its own array. The width tables already consume this
    # parse, and the property has a second consumer — font routing asks whether
    # a code point presents as emoji, which is not answerable from a width. A
    # hand snapshot of it sat beside these tables and drifted: 14 code points
    # Unicode added after it were wide by the generated ranges and not emoji by
    # the snapshot, so they took two cells and a text-presentation font.
    # Skin tones stay out for the reason they leave wide_set: they are combining
    # marks rather than standalone emoji.
    emoji_presentation_ranges = coalesce(emoji_pres - forced_zero)

    with args.output.open('w', encoding='utf-8') as fh:
        fh.write('// Copyright (C) 2026 Stanislav Yaglo\n')
        fh.write('// Copyright © 1991-2026 Unicode, Inc.\n')
        fh.write('// SPDX-License-Identifier: Apache-2.0 AND Unicode-3.0\n\n')
        fh.write(f'/* Generated from UCD {args.unicode_version} — regenerated rather than edited.\n')
        fh.write(f' * Regenerate via Scripts/build-unicode-tables.py;\n')
        fh.write(f' * inputs: EastAsianWidth.txt, emoji-data.txt,\n')
        fh.write(f' * DerivedGeneralCategory.txt, DerivedCoreProperties.txt\n')
        fh.write(f' * from the Unicode UCD. */\n\n')
        emit_table('zeroWidthRanges', zero_ranges, fh)
        emit_table('wideRanges', wide_ranges, fh)
        emit_table('emojiPresentationRanges', emoji_presentation_ranges, fh)

    print(f'wrote {args.output}: {len(zero_ranges)} zero-width ranges, '
          f'{len(wide_ranges)} wide ranges, '
          f'{len(emoji_presentation_ranges)} emoji-presentation ranges')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
