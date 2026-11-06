#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""An `std::optional<bool>` in a boolean context answers ENGAGEMENT, not its value.

`if (c.vsync)` and `c.vsync ? YES : NO` are true whenever the field is SET, so a
stored `false` reads as `true` and the setting is silently inert. The compiler
says nothing: optional's explicit `operator bool` makes both well-formed.

This is not hypothetical. Converting 181 `T value; bool valueSet;` pairs to
`std::optional` left 27 of these across the macOS frontend -- `[render] vsync =
false`, `ligatures = false`, `brighten_bold = false` and nine more were inert on
the shipping arm, each reading as the opposite of what the file said. A survey
found them; no test did, because the arm that draws is the one nobody asserts
pixel-by-pixel.

Only `optional<bool>` is a defect. For any other T, `x ? a : b` is a presence
test and is the idiom -- `settings.fontSize ? use(*settings.fontSize) : fallback`
asks exactly the right question. That is why this keys on the DECLARED TYPE
rather than on the shape of the expression: the shape is identical and only the
type separates a correct presence test from a value read that lost its value.

The spellings that are always right, and what they mean:

    x.has_value()        is it set
    x.value_or(default)  its value, or the default
    *x                   its value, under a guard that proved it is there
    x == true            its value is true, and it is set
"""

import argparse
import pathlib
import re
import sys
import tempfile
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import ubq_roots  # noqa: E402

# Where the fields are declared. A field is a subject of this rule only if one of
# these headers declares it `std::optional<bool>`.
DECLARING_HEADERS = (
    'Source/Core/Preferences/Preferences.h',
    'Source/Core/Preferences/Profile.h',
)

DECLARATION = re.compile(r'std::optional<bool>\s+([a-zA-Z_][A-Za-z0-9_]*)\s*;')

# The owners a preference is read through. Keyed on the owner as well as the
# field because `in.brightenBold` on a Rendering::BuildInputs is a plain bool
# with the same name, and flagging it would send a reader to fix nothing.
OWNERS = ('c', 'cfg', 'p', 'profile', 'resolved', 'config', 'configuration',
          'live', 'values', 'settings', 'preferences', 'out', 'q')

# What makes a read a BOOLEAN read. Keyed on what FOLLOWS rather than on what
# does not, because the correct spellings outnumber the wrong ones and a rule
# written as "anything but these" grows a false positive every time someone finds
# a new correct way to use an optional.
#
# `f(x, p.field)` passes the optional along and `saved = c.field;` copies it;
# both end in a delimiter and neither asks a boolean question. A bare `)` is a
# boolean read only inside an `if` or a `while`.
BOOLEAN_CONTEXT = re.compile(r'^\s*(\?|&&|\|\|)')
CLOSING = re.compile(r'^\s*\)')
CONDITIONAL = re.compile(r'\b(if|while)\s*\(')

# `!x` negates the ENGAGEMENT, so `x.has_value() && !x` never runs its branch.
NEGATED = re.compile(r'!\s*$')

# A line may state its reason, the same escape the vocabulary gate uses.
EXEMPTION = re.compile(r'optional-bool-ok:')


def subjectFields(root: pathlib.Path) -> set:
    """Every `std::optional<bool>` field the preference headers declare."""
    fields = set()
    for relative in DECLARING_HEADERS:
        path = root / relative
        if path.is_file():
            fields |= set(DECLARATION.findall(path.read_text(errors='replace')))
    return fields


def closesACondition(text: str, at: int) -> bool:
    """Whether the `)` right after `at` closes an `if`'s or `while`'s own paren.

    Depth, not the bare character. `if (!effectiveFollowSystem(c.field))` also
    ends in `)`, and there the field is an ARGUMENT whose function returns the
    bool being tested — the optional itself is never converted. Counting parens
    from the `if (` separates the two: the condition's own closer is the one that
    brings depth back to 1.
    """
    if not CLOSING.match(text[at:]):
        return False
    opener = CONDITIONAL.search(text)
    if not opener:
        return False
    depth = 0
    for character in text[opener.end() - 1:at]:
        if character == '(':
            depth += 1
        elif character == ')':
            depth -= 1
    return depth == 1


def sources(root: pathlib.Path):
    """Every file the scan reads, so an empty result can be told from an empty root."""
    excluded = re.compile(r'ThirdParty|/build/|BuildHostTests|BuildAgent|BuildWeb')
    found = []
    for suffix in ('*.h', '*.hpp', '*.cpp', '*.mm', '*.m'):
        # Judged by the path below the root: a checkout that sits under a
        # directory named build is not a build directory.
        for area in ('Source', 'Tests', 'Tools'):
            found.extend(p for p in (root / area).rglob(suffix)
                         if not excluded.search('/' + str(p.relative_to(root))))
    return sorted(found)


def findings(root: pathlib.Path, fields=None):
    """Yield (path, line number, text) for each optional<bool> read as a bool."""
    fields = subjectFields(root) if fields is None else fields
    if not fields:
        return
    pattern = re.compile(
        r'(?<![\*\w.])(' + '|'.join(OWNERS) + r')\.('
        + '|'.join(sorted(fields, key=len, reverse=True)) + r')\b')
    for path in sources(root):
        for number, text in enumerate(path.read_text(errors='replace').splitlines(), 1):
            stripped = text.lstrip()
            if stripped.startswith(('//', '///', '*', '/*')) or EXEMPTION.search(text):
                continue
            for match in pattern.finditer(text):
                after = text[match.end():]
                before = text[:match.start()]
                # `!x.has_value()` is a correct negation of a correct read, so
                # a leading `!` only counts when nothing follows the field.
                isNegatedValue = NEGATED.search(before) and not after.startswith('.')
                isBoolean = (BOOLEAN_CONTEXT.match(after)
                             or isNegatedValue
                             or closesACondition(text, match.end()))
                if not isBoolean:
                    continue
                yield path, number, text.strip()
                break


CASES = (
    # (source line, must be reported)
    ('    [wc applyVSync:c.vsync ? YES : NO];', True),
    ('    if (c.vsync)', True),
    ('    return c.vsync && somethingElse;', True),
    ('    out += p.vsync ? "true" : "false";', True),
    # The guard is on the same line, and the inner read is still the defect.
    ('    x = c.vsync.has_value() ? (c.vsync ? YES : NO) : YES;', True),
    # `has_value() && !x` negates the ENGAGEMENT, so the branch never runs.
    ('    if (c.vsync.has_value() && !c.vsync) {', True),
    # The correct spellings.
    ('    [wc applyVSync:c.vsync.value_or(true) ? YES : NO];', False),
    ('    if (c.vsync.has_value())', False),
    ('    [wc applyVSync:*c.vsync ? YES : NO];', False),
    ('    CHECK(c.vsync == true);', False),
    ('    c.vsync.reset();', False),
    ('    c.vsync = true;', False),
    ('    CHECK(!c.vsync.has_value());', False),
    # The field is an ARGUMENT; the `!` negates the call's return, not it.
    ('    if (!effectiveFollowSystem(c.vsync))', False),
    # A plain bool of the same name on another type is not this rule's business.
    ('    resolveCell(pen, palette, false, in.vsync);', False),
    # A comment naming the shape is prose.
    ('    // c.vsync ? YES : NO would read engagement, not the value', False),
    # Passing the optional along, and copying it, are not boolean reads.
    ('    g_print("%s", stated(c.vsync, render).c_str());', False),
    ('    const std::optional<bool> saved = c.vsync;', False),
    ('    CHECK(q.vsync == p.vsync);', False),
    # A line that states its reason.
    ('    if (c.vsync) { } // optional-bool-ok: presence is the question here', False),
)


def selftest(verbose: bool) -> int:
    """Run every case in its own tree. Returns the failure count."""
    failures = 0
    for index, (line, expected) in enumerate(CASES):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            (root / 'Source' / 'Core' / 'Preferences').mkdir(parents=True)
            (root / 'Source' / 'Core' / 'Preferences' / 'Preferences.h').write_text(
                'struct Values {\n    std::optional<bool> vsync;\n};\n')
            (root / 'Tests').mkdir()
            (root / 'Tools').mkdir()
            # One file per case, named for the case: a lookup that succeeds when
            # any case at all matches cannot tell which one did.
            (root / 'Source' / f'case{index}.mm').write_text(line + '\n')
            got = any(f[0].name == f'case{index}.mm' for f in findings(root))
        arm = 'A' if expected else 'B'
        if got != expected:
            failures += 1
            print(f'  FAIL  ARM {arm}: {line.strip()}\n'
                  f'        reported={got} expected={expected}', file=sys.stderr)
        elif verbose:
            print(f'  PASS  ARM {arm}: {line.strip()}')
    positives = sum(1 for _, e in CASES if e)
    print(f'selftest: {positives} ARM A + {len(CASES) - positives} ARM B = '
          f'{len(CASES)} cases, {failures} failures')
    return failures


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    # Anchored to this script rather than to the invoker's directory: rglob over
    # a missing tree yields an EMPTY population silently, and a census of
    # nothing reads as a tree with nothing to find.
    parser.add_argument('--root', default=ubq_roots.subject_of(__file__))
    parser.add_argument('--selftest', action='store_true')
    args = parser.parse_args()

    if args.selftest:
        return 1 if selftest(verbose=True) else 0

    # A gate whose predicate is untrusted must not report: an ARM A miss passes
    # a tree where a setting has gone quietly inert.
    if selftest(verbose=False):
        print('check-optional-bool-reads: FAIL - predicate selftest does not pass; '
              'refusing to report. Run --selftest.', file=sys.stderr)
        return 2

    root = pathlib.Path(args.root)
    # The fields are declared by Core; the reads judged are the repository's own.
    fields = subjectFields(pathlib.Path(ubq_roots.core_root(str(root))))
    if not fields:
        print(f'check-optional-bool-reads: FAIL - no std::optional<bool> field found in '
              f'{", ".join(DECLARING_HEADERS)}; the scan has no subjects rather than '
              f'a clean tree', file=sys.stderr)
        return 2
    scanned = sources(root)
    if not scanned:
        print(f'check-optional-bool-reads: FAIL - no source files under {root}',
              file=sys.stderr)
        return 2

    found = list(findings(root, fields))
    if not found:
        print(f'check-optional-bool-reads: ok ({len(fields)} optional<bool> preference(s) '
              f'across {len(scanned)} file(s), every read asks for a value)')
        return 0

    print(f'check-optional-bool-reads: FAILED — {len(found)} read(s) of an '
          f'optional<bool> in a boolean context:', file=sys.stderr)
    for path, number, text in found[:40]:
        print(f'  {path.relative_to(root)}:{number}: {text}', file=sys.stderr)
    if len(found) > 40:
        print(f'  … and {len(found) - 40} more', file=sys.stderr)
    print('\n  Each of these is true whenever the field is SET, so a stored `false`\n'
          '  reads as `true` and the setting is inert. Say which you meant:\n'
          '    x.has_value()        is it set\n'
          '    x.value_or(default)  its value, or the default\n'
          '    *x                   its value, under a guard\n'
          '    x == true            its value is true, and it is set', file=sys.stderr)
    return 1


if __name__ == '__main__':
    sys.exit(main())
