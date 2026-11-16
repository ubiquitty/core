#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""Generate joining, presentation-form and strong-RTL tables from one UCD release.

Inputs from https://www.unicode.org/Public/<version>/ucd/:
  extracted/DerivedJoiningType.txt — complete Joining_Type, including defaults
  extracted/DerivedBidiClass.txt   — Bidi_Class R/AL, including @missing defaults
  UnicodeData.txt                 — contextual forms and lam-alef ligatures

Usage:
  Scripts/build-joining-tables.py \
      --joining DerivedJoiningType.txt --bidi DerivedBidiClass.txt \
      --unicodedata UnicodeData.txt --unicode-version 18.0.0 \
      --output Source/Core/Unicode/Joining.tables.inc
"""

import argparse
from pathlib import Path


JOINING_TYPES = {
    'U': 'NonJoining', 'T': 'Transparent', 'R': 'RightJoining',
    'L': 'LeftJoining', 'D': 'DualJoining', 'C': 'JoinCausing',
}
FORMS = ('<isolated>', '<final>', '<initial>', '<medial>')


def property_records(path):
    """Read explicit ranges and the ordered @missing defaults separately."""
    missing, explicit = [], []
    for line in path.read_text(encoding='utf-8').splitlines():
        if line.startswith('# @missing:'):
            body = line.split(':', 1)[1]
            records = missing
        else:
            body = line.split('#', 1)[0].strip()
            records = explicit
        if not body or ';' not in body:
            continue
        interval, value = (part.strip() for part in body.split(';')[:2])
        bounds = interval.split('..')
        records.append((int(bounds[0], 16), int(bounds[-1], 16), value))
    return missing, explicit


def coalesce(values, default):
    ranges = []
    for code_point, value in enumerate(values):
        if value == default:
            continue
        if ranges and ranges[-1][1] + 1 == code_point and ranges[-1][2] == value:
            ranges[-1] = (ranges[-1][0], code_point, value)
        else:
            ranges.append((code_point, code_point, value))
    return ranges


def presentation_forms(path):
    forms, ligatures = {}, {}
    for line in path.read_text(encoding='utf-8').splitlines():
        fields = line.split(';')
        code_point = int(fields[0], 16)
        parts = fields[5].split()
        if not parts or parts[0] not in FORMS:
            continue
        form = FORMS.index(parts[0])
        bases = [int(value, 16) for value in parts[1:]]
        if len(bases) == 1:
            forms.setdefault(bases[0], [0] * 4)[form] = code_point
        elif (len(bases) == 2 and bases[0] == 0x0644
              and bases[1] in (0x0622, 0x0623, 0x0625, 0x0627) and form < 2):
            ligatures.setdefault(bases[1], [0] * 2)[form] = code_point
    return forms, ligatures


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for argument in ('joining', 'bidi', 'unicodedata', 'output'):
        parser.add_argument('--' + argument, required=True, type=Path)
    parser.add_argument('--unicode-version', required=True)
    args = parser.parse_args()

    joining = ['U'] * 0x110000
    _, records = property_records(args.joining)
    for first, last, value in records:
        if value not in JOINING_TYPES:
            parser.error('unknown Joining_Type: ' + value)
        joining[first:last + 1] = [value] * (last - first + 1)
    joining_ranges = coalesce(joining, 'U')

    strong_rtl = bytearray(0x110000)
    missing, records = property_records(args.bidi)
    for first, last, value in missing + records:
        is_rtl = value in ('R', 'AL', 'Right_To_Left', 'Arabic_Letter')
        strong_rtl[first:last + 1] = bytes([is_rtl]) * (last - first + 1)
    rtl_ranges = coalesce(strong_rtl, 0)
    forms, ligatures = presentation_forms(args.unicodedata)

    lines = [
        '// Copyright (C) 2026 Stanislav Yaglo',
        '// Copyright © 1991-2026 Unicode, Inc.',
        '// SPDX-License-Identifier: Apache-2.0 AND Unicode-3.0', '',
        f'// Generated from UCD {args.unicode_version}; do not edit by hand.',
        '// Regenerate with Scripts/build-joining-tables.py from',
        '// DerivedJoiningType.txt, DerivedBidiClass.txt and UnicodeData.txt.', '',
        'struct JoinRange {',
        '    std::uint32_t first;', '    std::uint32_t last;',
        '    JoiningType type;', '};', '',
        'static const JoinRange joiningRanges[] = {',
    ]
    for first, last, value in joining_ranges:
        lines.append(f'    {{ 0x{first:06X}, 0x{last:06X}, JoiningType::{JOINING_TYPES[value]} }},')
    lines += ['};', '', 'struct DirectionRange {',
              '    std::uint32_t first;', '    std::uint32_t last;', '};', '',
              'static const DirectionRange strongRTLRanges[] = {']
    for first, last, _ in rtl_ranges:
        lines.append(f'    {{ 0x{first:06X}, 0x{last:06X} }},')
    lines += ['};', '', 'struct PresFormB {',
              '    std::uint32_t base;', '    std::uint32_t isol, fina, init, medi;',
              '};', '', 'static const PresFormB presFormB[] = {']
    for base, shapes in sorted(forms.items()):
        row = ', '.join(f'0x{value:06X}' for value in [base] + shapes)
        lines.append('    { ' + row + ' },')
    lines += ['};', '', 'struct LamAlef {', '    std::uint32_t alef;',
              '    std::uint32_t isol, fina;', '};', '',
              'static const LamAlef lamAlef[] = {']
    for base, shapes in sorted(ligatures.items()):
        row = ', '.join(f'0x{value:06X}' for value in [base] + shapes)
        lines.append('    { ' + row + ' },')
    lines += ['};', '']
    args.output.write_text('\n'.join(lines), encoding='utf-8')
    print(f'wrote {args.output}: {len(joining_ranges)} joining ranges, '
          f'{len(rtl_ranges)} strong-RTL ranges, {len(forms)} contextual letters, '
          f'{len(ligatures)} lam-alef ligatures')


if __name__ == '__main__':
    main()
