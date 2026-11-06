#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""One source line, changed in a way a correct test must notice.

A mutant is a defect injected into a line of the tester's choosing. If the suite
still passes with it applied, no assertion was watching that line — the line is
executed (coverage says so) and unguarded (this says so). The two are different
questions and only the second one catches a test that cannot fail.

What is mutated, and why these. Each operator changes behaviour without changing
the shape of the code, so the mutant compiles and the failure is a wrong answer
rather than a build break:

    a boundary        <  ->  <=      an off-by-one that a loop or a clamp hides
    a comparison      == ->  !=      an equality test nothing reads back
    a condition       &&  ->  ||     a guard that stops guarding
    a return          true -> false  a predicate whose answer nobody checks

Boundary and negation are the operators that would have caught what this tree
lost by hand: a margin wide enough to accept the wrong mechanism, a guard whose
two halves were never distinguished.

What is not mutated. A comment, a string literal, an assertion inside the code
under test, and any line the diff did not touch. The point is the change in
front of you, not the tree's history — a gate that reports a thousand
pre-existing survivors is one nobody reads.
"""

import argparse
import json
import re
import sys

# (name, pattern, replacement). Order matters: the first match on a line wins,
# so the more specific operator has to come first or the general one eats it.
OPERATORS = [
    # `<=` before `<`, `>=` before `>`: the two-character forms would otherwise
    # be found by the one-character pattern and mutated into nonsense.
    ('boundary-le', re.compile(r'(?<![<>=!])<=(?!=)'), '<'),
    ('boundary-ge', re.compile(r'(?<![<>=!])>=(?!=)'), '>'),
    ('boundary-lt', re.compile(r'(?<![<>=!-])<(?![<=])'), '<='),
    ('boundary-gt', re.compile(r'(?<![<>=!-])>(?![>=])'), '>='),
    ('equality', re.compile(r'(?<![<>=!])==(?!=)'), '!='),
    ('inequality', re.compile(r'(?<![<>=!])!=(?!=)'), '=='),
    ('condition-and', re.compile(r'&&'), '||'),
    ('condition-or', re.compile(r'\|\|'), '&&'),
    ('return-true', re.compile(r'\breturn true\b'), 'return false'),
    ('return-false', re.compile(r'\breturn false\b'), 'return true'),
]

# A line that cannot carry a meaningful mutant: it declares rather than decides,
# or its "comparison" is punctuation in another grammar.
SKIP = re.compile(
    r'^\s*(//|/\*|\*|#\s*(include|define|if|endif|pragma))'   # comment, preprocessor
    r'|^\s*(template|using|typedef|namespace)\b'              # declaration
    r'|\bstatic_assert\s*\('                                  # compile-time, not behaviour
    r'|\boperator\s*(<|>|=|!|\+|-)'                           # an operator's own name
)


def strip_uninteresting(line):
    """Blank out the spans an operator must not reach: strings, chars, comments.

    Replaced with spaces rather than removed so every column index still lines
    up with the original — a mutant is applied by offset, and shifting them
    would corrupt an unrelated part of the line.
    """
    out = list(line)
    i, n = 0, len(line)
    while i < n:
        c = line[i]
        if c in '"\'':
            quote, j = c, i + 1
            while j < n:
                if line[j] == '\\':
                    j += 2
                    continue
                if line[j] == quote:
                    j += 1
                    break
                j += 1
            for k in range(i, min(j, n)):
                out[k] = ' '
            i = j
            continue
        if line.startswith('//', i):
            for k in range(i, n):
                out[k] = ' '
            break
        if line.startswith('/*', i):
            j = line.find('*/', i + 2)
            j = n if j < 0 else j + 2
            for k in range(i, j):
                out[k] = ' '
            i = j
            continue
        i += 1
    return ''.join(out)


def mutants_for_line(path, lineno, line):
    """Every mutant this line admits, as dicts ready to apply."""
    if SKIP.search(line):
        return []
    searchable = strip_uninteresting(line)
    found = []
    for name, pattern, replacement in OPERATORS:
        for m in pattern.finditer(searchable):
            found.append({
                'file': path,
                'line': lineno,
                'column': m.start(),
                'operator': name,
                'original': line.rstrip('\n'),
                'was': m.group(0),
                'now': replacement,
            })
    return found


def apply_mutant(text, mutant):
    """Return `text` with one mutant applied, by line and column."""
    lines = text.split('\n')
    index = mutant['line'] - 1
    line = lines[index]
    start, was = mutant['column'], mutant['was']
    if line[start:start + len(was)] != was:
        raise ValueError(
            f"{mutant['file']}:{mutant['line']} no longer reads {was!r} at column {start}")
    lines[index] = line[:start] + mutant['now'] + line[start + len(was):]
    return '\n'.join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--file', required=True, help='source file to read')
    parser.add_argument('--lines', required=True,
                        help='comma-separated line numbers, or "all"')
    parser.add_argument('--apply', type=int,
                        help='apply mutant N (0-based) and print the result')
    args = parser.parse_args()

    text = open(args.file, encoding='utf-8').read()
    lines = text.split('\n')

    if args.lines == 'all':
        wanted = range(1, len(lines) + 1)
    else:
        wanted = [int(n) for n in args.lines.split(',') if n.strip()]

    found = []
    for lineno in wanted:
        if 1 <= lineno <= len(lines):
            found.extend(mutants_for_line(args.file, lineno, lines[lineno - 1]))

    if args.apply is not None:
        if not 0 <= args.apply < len(found):
            print(f'mutate: no mutant {args.apply} (found {len(found)})', file=sys.stderr)
            return 2
        sys.stdout.write(apply_mutant(text, found[args.apply]))
        return 0

    json.dump(found, sys.stdout, indent=2)
    sys.stdout.write('\n')
    return 0


if __name__ == '__main__':
    sys.exit(main())
