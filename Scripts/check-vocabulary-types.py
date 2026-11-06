#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""A pointer and a count are two parameters that can disagree; a C array is a size
that binds to nothing.

check-style.py already refuses both shapes on lines a branch ADDS. That judges a
branch against its merge base, which leaves two ways in and no way down:

  - a branch cut before the added-line rule landed does not carry it, so a
    pointer+length pair can reach main through it;
  - a shape that reaches main is never judged again, so the backlog is invisible
    and nothing obliges anyone to shrink it.

This weighs the whole tree instead, against a budget pinned two-sided. The count
cannot rise, and a commit that lowers it must lower the budget in the same
commit -- a budget with slack in it cannot fail, and an added-line rule alone
sits next to a backlog nobody measures.
"""

import argparse
import pathlib
import re
import subprocess
import sys
import tempfile

import gate_corpus
import ubq_roots

# A pointer and a length as two parameters. Matches a declaration, not a call:
# both parts carry a type, which a call site does not.
POINTER_LENGTH = re.compile(
    r'\(([^)]*, *)?(const )?(char|void|std::uint8_t|uint8_t|std::byte)\*+ *'
    r'[a-zA-Z_][A-Za-z0-9_]*, *(const )?(int|std::size_t|size_t|unsigned) '
    r'[a-zA-Z_]*([Ll]en|[Ll]ength|[Cc]ount|[Ss]ize)')

# A C array declaration. std::array carries its own size, so a range-for and
# .size() work and it decays only where asked. The trailing `=` or `;` is what
# separates a declaration from a parameter, which decays either way.
C_ARRAY = re.compile(
    r'^[ \t]*(static |const |constexpr |inline )*'
    r'(char|int|float|double|bool|std::uint[0-9]+_t|uint[0-9]+_t|unsigned char) '
    r'[a-zA-Z_][A-Za-z0-9_]*\[[0-9A-Za-z_]+\][ \t]*(=|;)')

SUBJECTS = (('span', 'pointer + length parameter pair (use std::span)', POINTER_LENGTH),
            ('array', 'C array declaration (use std::array)', C_ARRAY))

# A declaration with C language linkage cannot take a std::span: the C ABI has no
# such type, and the callers are a page's JavaScript or a C library's function
# table. Counting those sites puts a population in the budget that no one can
# convert, which is worse than counting nothing -- it turns every new export into
# a ratchet loosening, and the number stops meaning "work a person could do".
#
# The exemption is the linkage, not the file: a C array inside such a function is
# ordinary C++ and stays counted, and the rule applies again below the block.
C_LINKAGE_BLOCK = re.compile(r'extern *"C" *\{')
C_LINKAGE_DECLARATION = re.compile(r'extern *"C" ')
LITERAL = re.compile(r'"(\\.|[^"\\])*"|\'(\\.|[^\'\\])*\'|//.*$')


def cLinkageLines(text: str) -> set:
    """The 1-based line numbers under `extern "C"` in `text`.

    Brace depth, counted with the string and character literals removed so a
    brace inside one cannot open or close a block that is not there.
    """
    lines = set()
    depth = 0
    for number, line in enumerate(text.splitlines(), 1):
        # The linkage is read from the line as written -- `"C"` is itself a
        # string literal, and a scan that strips literals first cannot see it.
        # Braces are counted from the line with them removed.
        bare = LITERAL.sub('', line)
        if depth > 0:
            lines.add(number)
        elif C_LINKAGE_BLOCK.search(line) and '{' in bare:
            lines.add(number)
            depth = 1
            bare = bare[bare.index('{') + 1:]
        elif C_LINKAGE_DECLARATION.search(line):
            lines.add(number)
            continue
        if depth > 0:
            depth += bare.count('{') - bare.count('}')
    return lines

# Generated tables are written by a script and re-written on every build, so a
# finding in one is a finding about the generator's output format rather than
# about anything a person can edit here.
EXCLUDED = re.compile(r'ThirdParty|\.tables\.inc')


def sources(root: pathlib.Path, eligible=None):
    """Every file the scan reads, so an empty result can be told from an empty root."""
    if eligible is not None:
        repository = pathlib.Path(gate_corpus.root())
        candidates = (repository / path for path in eligible)
    else:
        candidates = (list(root.rglob('*.h')) + list(root.rglob('*.cpp'))
                      + list(root.rglob('*.mm')))
    return sorted(path for path in candidates if not EXCLUDED.search(str(path)))


def findings(root: pathlib.Path, key: str, eligible=None):
    """Yield (path, line number, text) for every site of `key` under `root`."""
    pattern = dict((k, p) for k, _, p in SUBJECTS)[key]
    for path in sources(root, eligible):
        contents = path.read_text(errors='replace')
        exempt = cLinkageLines(contents) if key == 'span' else set()
        for number, text in enumerate(contents.splitlines(), 1):
            if pattern.search(text) and number not in exempt:
                yield path, number, text.strip()


CASES = (
    # (key, source line, must be reported)
    ('span', 'void feed(const char* bytes, int length);', True),
    ('span', 'int decode(const uint8_t* payload, std::size_t length);', True),
    ('span', 'bool read(Terminal& t, const std::byte* data, size_t byteCount);', True),
    # A call site names no types, so the pair it passes is not a declaration.
    ('span', '    feed(bytes, length);', False),
    # Already converted.
    ('span', 'void feed(std::span<const char> bytes);', False),
    # The second parameter is a count of something else entirely.
    ('span', 'void resize(char* buffer, int rows);', False),
    # A pointer to a type this rule says nothing about.
    ('span', 'void draw(const Cell* cells, int cellCount);', False),
    # C language linkage, where the type does not exist. Both spellings.
    ('span', 'extern "C" {\nvoid feed(const char* bytes, int length);\n}', False),
    ('span', 'extern "C" void feed(const char* bytes, int length);', False),
    # The control on the exemption: the block ends, and the rule applies below
    # it. Without this arm a scan that never leaves the block reads as a tree
    # with nothing left to convert.
    ('span', 'extern "C" {\nvoid ready(void) { }\n}\nvoid feed(const char* bytes, int length);', True),
    # A brace inside a literal opens nothing.
    ('span', 'const char* brace = "{";\nvoid feed(const char* bytes, int length);', True),
    ('array', '    char buffer[32];', True),
    ('array', 'static constexpr int limits[4] = { 1, 2, 3, 4 };', True),
    ('array', '    uint8_t mark[maxRows];', True),
    # A parameter decays to a pointer whatever it is written as, so the size in
    # the brackets binds to nothing and there is nothing here to convert.
    ('array', 'void write(char buffer[32], int length);', False),
    # Already converted.
    ('array', '    std::array<char, 32> buffer;', False),
    # An indexing expression is not a declaration.
    ('array', '    buffer[index] = 0;', False),
    # The linkage exempts a parameter list, not a body: this array is ordinary
    # C++ and converts like any other.
    ('array', 'extern "C" {\nvoid ready(void)\n{\n    char buffer[32];\n}\n}', True),
)


def selftest(verbose: bool) -> int:
    """Run every case in its own tree. Returns the failure count."""
    failures = 0
    for index, (key, line, expected) in enumerate(CASES):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            # One file per case, named for the case. A lookup that succeeds when
            # any case at all matches cannot tell which one did.
            (root / f'case{index}.h').write_text(line + '\n')
            got = len(list(findings(root, key))) > 0
        arm = 'A' if expected else 'B'
        # A case may span lines, and a verdict that wraps is one a reader skims.
        rendered = ' | '.join(part.strip() for part in line.splitlines())
        if got != expected:
            failures += 1
            print(f'  FAIL  ARM {arm} {key}: {rendered}\n'
                  f'        reported={got} expected={expected}', file=sys.stderr)
        elif verbose:
            print(f'  PASS  ARM {arm} {key}: {rendered}')
    positives = sum(1 for _, _, e in CASES if e)
    print(f'selftest: {positives} ARM A + {len(CASES) - positives} ARM B = '
          f'{len(CASES)} cases, {failures} failures')
    return failures


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    # The default anchors to the repository this runs in, not the invoker's
    # directory: rglob over a missing 'Source' yields an EMPTY population
    # silently, and a census of nothing reads as a tree with nothing to find.
    parser.add_argument('--root',
        default=str(pathlib.Path(ubq_roots.subject_root()) / 'Source'))
    parser.add_argument('--gate', action='store_true',
                        help='exit non-zero unless each count equals its budget exactly')
    parser.add_argument('--span-budget', type=int, default=None)
    parser.add_argument('--array-budget', type=int, default=None)
    parser.add_argument('--list', choices=[k for k, _, _ in SUBJECTS],
                        help='print every site of one subject and exit')
    parser.add_argument('--selftest', action='store_true')
    args = parser.parse_args()

    if args.selftest:
        return 1 if selftest(verbose=True) else 0

    # A gate whose predicate is untrusted must not report: an ARM A miss lowers
    # a budget over sites it stopped seeing, and the backlog then reads as drained.
    if args.gate and selftest(verbose=False):
        print('check-vocabulary-types: FAIL - predicate selftest does not pass; '
              'refusing to report. Run --selftest.', file=sys.stderr)
        return 2

    root = pathlib.Path(args.root)
    if not root.is_dir():
        print(f'check-vocabulary-types: FAIL - {root} is not a directory', file=sys.stderr)
        return 2
    eligible = gate_corpus.eligible_under(root, kinds=('.h', '.cpp', '.mm'))
    scanned = sources(root, eligible)
    if not scanned:
        print(f'check-vocabulary-types: FAIL - no source files under {root}; the scan '
              f'matched nothing rather than finding nothing', file=sys.stderr)
        return 2

    if args.list:
        for path, number, text in findings(root, args.list, eligible):
            print(f'{path}:{number}: {text}')
        return 0

    # The budgets are the ledger's unless a flag overrides them: each repository
    # holds its own sites to its own count.
    ledgerBudgets = ubq_roots.ledger(ubq_roots.subject_root())[0]
    budgets = {'span': args.span_budget if args.span_budget is not None
               else ledgerBudgets.get('vocabulary-span'),
               'array': args.array_budget if args.array_budget is not None
               else ledgerBudgets.get('vocabulary-array')}
    counts = {key: len(list(findings(root, key, eligible)))
              for key, _, _ in SUBJECTS}
    if not args.gate:
        for key, description, _ in SUBJECTS:
            print(f'{counts[key]:5d}  {description}')
        print(f'{len(scanned)} files scanned under {root}')
        return 0

    status = 0
    for key, description, _ in SUBJECTS:
        budget, count = budgets[key], counts[key]
        if budget is None:
            print(f'check-vocabulary-types: FAIL - --gate requires --{key}-budget or '
                  f'a vocabulary-{key} budget in a Scripts/gate-ledger.<repository>.txt',
                  file=sys.stderr)
            return 2
        if count > budget:
            print(f'check-vocabulary-types: FAIL - count {count} != budget {budget} for '
                  f'{description}. The budget is an equality, not a ceiling: a change that '
                  f'adds or removes sites must move it in the same commit. Use the '
                  f'vocabulary type, or raise the budget to {count} knowing this is the '
                  f'shape the gate exists to remove.', file=sys.stderr)
            status = 1
        elif count < budget:
            print(f'check-vocabulary-types: FAIL - count {count} != budget {budget} for '
                  f'{description}. The budget is an equality, not a ceiling: lower it to '
                  f'{count} in this commit, since a budget with slack in it cannot fail.',
                  file=sys.stderr)
            status = 1
    if status == 0:
        print(f'check-vocabulary-types: ok - {counts["span"]} pointer+length, '
              f'{counts["array"]} C array, both at budget over {len(scanned)} files')
    return status


if __name__ == '__main__':
    sys.exit(main())
