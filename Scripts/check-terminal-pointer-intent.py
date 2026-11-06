#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""check-terminal-pointer-intent.py — a Terminal* that promises nothing.

Core is converting `Terminal*` parameters to `Terminal&`. Mid-transition the
pointer spelling means two different things at once:

    void softReset(Terminal* t)        { if (!t) return; ... }   null is meaningful
    void resetTabStops(Terminal* t)    { t->tabStops.fill(0); }  not converted yet

A reader cannot tell those apart, and neither can a tool, so every remaining
pointer is ambiguous rather than merely unfinished. That ambiguity is the defect
this gate removes -- not the conversion itself. A function whose body never tests
its terminal is reported: either it converts, or it is an entry point that ought
to be checking and is not.

The rule is one-directional on purpose. A body that tests its parameter is
never a finding, so the frontend entry points that legitimately take a pointer
and defend against null need no exemption list, no annotation, and nothing that
can outlive its subject.

Why a detector and not a grep. The condition has no single spelling. Measured
across Source/Core, a null test is written at least eight ways:

    if (!t)                       if (!t || !cell || max <= 0)
    if (t)                        return t ? t->modes.syncOutput : false
    return t && t->locatorEnabled return ready && t
    if (t == nullptr)             if (nullptr == t)

A predicate matching only the first misses most of its own subject, and every
miss is a function it would tell someone to convert. The inverse mistake is just
as easy: `!t->grid` contains the characters `!t` and tests a MEMBER, not the
pointer, and reading it as a null test miscounts every function that does so.

The control below catches both; reading the predicate catches neither.
`--selftest` runs 23 cases in two arms: every spelling of a null test must be
detected, and every shape that merely resembles one must not be. Arm A is the
dangerous arm -- a miss there produces a finding that instructs someone to delete
a working null check -- so `--gate` refuses to run at all unless it passes.

What this does not claim: that a reported function is wrong today. Most are
not converted yet, so the default exit status is 0 and the findings are a
worklist to read. `--gate` enforces the ratchet.

The budget is pinned two-sided. A gate that fails only when the count rises
cannot notice it falling, and a budget with slack in it cannot fail at
all. Converting functions is therefore expected to fail this gate until the
budget is lowered in the same commit -- that is the ratchet working, not a bug.
"""

import argparse
import collections
import pathlib
import re
import sys

# A function definition taking a Terminal pointer in any parameter position. A
# call site carries no type, so this cannot match one.
#
# Matching only the first parameter would be a blind spot rather than a backlog:
# nothing stops a terminal being taken in position two, and
# `kittyFillPlacement(TerminalImagePlacement*, Terminal*, ...)` and
# `pushNormalizedCell(JoinedText*, Terminal*, ...)` take one there. Both are
# ordinary functions that a reader would expect this to weigh.
SIGNATURE = re.compile(r'\b(\w+)\s*\(([^;{)]*)\)')

# A lambda is a body that receives its parameters like any other, and one taking
# a Terminal pointer has the same question to answer. It carries no name before
# the paren, so the matcher above cannot see it.
LAMBDA = re.compile(r'\[[^\]]*\]\s*\(([^;{)]*)\)\s*(?:mutable\s*)?(?:->\s*[\w:<>,\s*&]+\s*)?\{')

# The Terminal pointer inside one parameter list, and the name it is bound to.
TERMINAL_PARAMETER = re.compile(
    r'(?:^|,)\s*(?:const\s+)?(?:Ubiquitty::)?Terminal\s*\*\s*(\w+)\s*$')


def tests_null(body: str, v: str) -> bool:
    """Does `body` test parameter `v` against null, in any spelling?"""
    V = re.escape(v)
    # The bare identifier alone. `!t->grid`, `t.rows ? a : b` and `t[i]` test
    # something reached THROUGH the pointer, not the pointer.
    B = rf'\b{V}\b(?!\s*(?:->|\.|\[|\())'
    return any(re.search(p, body) for p in (
        rf'!\s*{B}',
        rf'{B}\s*(?:==|!=)\s*(?:nullptr|NULL|0)\b',
        rf'(?:nullptr|NULL)\s*(?:==|!=)\s*{B}',
        rf'{B}\s*\?',
        rf'\bif\s*\(\s*{B}\s*\)',
        rf'\bwhile\s*\(\s*{B}\s*\)',
        # An operand of && or ||, either side, terminated by anything --
        # `return ready && t;` ends in a semicolon, not a paren.
        rf'(?:&&|\|\|)\s*{B}',
        rf'{B}\s*(?:&&|\|\|)',
    ))


def sources(root: pathlib.Path):
    """Every file the scan reads, so an empty result can be told from an empty root."""
    return sorted(list(root.rglob('*.h')) + list(root.rglob('*.cpp'))
                  + list(root.rglob('*.mm')) + list(root.rglob('*.m')))


def definitions(root: pathlib.Path):
    """Yield (path, line, name, param, body) for every Terminal* definition.

    Headers are scanned as well as sources: a module defined entirely as
    `static inline` in a header has no .cpp, and a scan that skips headers
    reports it as zero findings rather than as unexamined. Objective-C++ for the
    same reason — Core has none, so it costs nothing here and means `--root
    Source/Platform` weighs the macOS frontend rather than quietly skipping it.
    """
    for path in sources(root):
        text = path.read_text(errors='replace')
        for m in list(SIGNATURE.finditer(text)) + list(LAMBDA.finditer(text)):
            named = m.re is SIGNATURE
            parameter = None
            for argument in (m.group(2) if named else m.group(1)).split(','):
                found = TERMINAL_PARAMETER.search(',' + argument)
                if found:
                    parameter = found.group(1)
                    break
            if parameter is None:
                continue
            if named:
                k = m.end()
                while k < len(text) and text[k] in ' \t\r\n':
                    k += 1
                if k >= len(text) or text[k] != '{':
                    continue                   # a declaration, not a definition
            else:
                k = m.end() - 1                # LAMBDA consumes its opening brace
            depth, j = 0, k
            while j < len(text):
                if text[j] == '{':
                    depth += 1
                elif text[j] == '}':
                    depth -= 1
                    if depth == 0:
                        break
                j += 1
            yield (path, text.count('\n', 0, m.start()) + 1,
                   m.group(1) if named else '<lambda>', parameter, text[k:j])


# Every ARM A spelling below was taken from a real body in Source/Core, and a
# predicate that matches only `if (!t)` misses four of them.
_ARM_A = [
    ("plain if",               "{ if (!t) return; t->x = 1; }"),
    ("compound, first term",   "{ if (!t || !cell || max <= 0) return 0; }"),
    ("compound, later term",   "{ if (cell && !t) return 0; }"),
    ("bare truthiness",        "{ if (t) t->allowTitleOperations = true; }"),
    ("ternary condition",      "{ return t ? t->modes.syncOutput : false; }"),
    ("left operand of &&",     "{ return t && t->locatorEnabled; }"),
    ("right operand of &&",    "{ return ready && t; }"),
    ("equality with nullptr",  "{ if (t == nullptr) return; }"),
    ("nullptr on the left",    "{ if (nullptr == t) return; }"),
    ("inequality",             "{ if (t != nullptr) use(t); }"),
    ("while",                  "{ while (t) t = t->next; }"),
    ("header-inline guard",    "{ if (!t || !t->bellPending) return BellMode::None; }"),
]

# ARM B keeps a test of something reached through the pointer from reading as a
# test of the pointer. Without it, `if (!t->grid)` counts as a guard on t.
_ARM_B = [
    ("plain dereference",      "{ t->x = 1; return t->rows; }"),
    ("another var negated",    "{ if (!cell) return 0; return t->rows; }"),
    ("another var ternary",    "{ return cell ? t->rows : 0; }"),
    ("member negated",         "{ if (!t->grid) return; }"),
    ("member ternary",         "{ return t->modes.autoRepeat ? 1 : 2; }"),
    ("member in &&",           "{ return t->a && t->b; }"),
    ("member subscript",       "{ if (!t->tabStops[c]) return; }"),
    ("name is a prefix",       "{ if (!term) return; return t->rows; }"),
    ("name is a suffix",       "{ if (!outT) return; return t->rows; }"),
    ("negated call result",    "{ if (!isActive(t)) return false; }"),
    ("member compared to 0",   "{ if (t->rows == 0) return; }"),
]


# The matcher's own cases. Arm A and Arm B weigh the predicate against a body;
# these weigh SIGNATURE against a declaration, so a terminal in any position is
# shown to be seen.
# BEYOND_THE_BODY must never appear in an extracted body: a matcher that mis-reads
# where a body ends swallows it, and every case is written so that it would.
BEYOND_THE_BODY = "sentinelOutsideTheBody"

_MATCHER = [
    ("terminal first",
     "void alpha(Ubiquitty::Terminal* t, int n)\n{\n    t->x = n;\n}\nint sentinelOutsideTheBody;\n", "alpha", "t"),
    ("terminal second",
     "void beta(Cell* c, Ubiquitty::Terminal* t)\n{\n    t->x = 1;\n}\nint sentinelOutsideTheBody;\n", "beta", "t"),
    ("terminal last",
     "void gamma(int a, int b, Ubiquitty::Terminal* term)\n{\n    term->x = a + b;\n}\nint sentinelOutsideTheBody;\n", "gamma", "term"),
    ("const, not first",
     "int delta(int a, const Ubiquitty::Terminal* t)\n{\n    return t->rows + a;\n}\nint sentinelOutsideTheBody;\n", "delta", "t"),
    ("a lambda parameter",
     "void host()\n{\n    run([&](Ubiquitty::Terminal* t) { t->x = 1; });\n}\nint sentinelOutsideTheBody;\n", "<lambda>", "t"),
]
# Shapes the matcher must never read as a Terminal parameter.
_NOT_A_PARAMETER = [
    ("a call", "void caller()\n{\n    callee(t, 1);\n}\n"),
    ("a Terminal by value", "void byValue(Ubiquitty::Terminal t)\n{\n    (void)t;\n}\n"),
    ("a Terminal reference", "void byReference(Ubiquitty::Terminal& t)\n{\n    (void)t;\n}\n"),
    ("a pointer to another type", "void other(TerminalImage* t)\n{\n    (void)t;\n}\n"),
]


def matcher_selftest(verbose: bool = True) -> int:
    """Weighs SIGNATURE against declarations, one case per tree.

    One tree per case, and each case names its own function: four cases sharing a
    directory and the name `f` cannot tell which of them matched, so the whole set
    passes when any one does. That version reported five passes under the
    first-parameter narrowing it was written to catch.
    """
    import tempfile
    failures = []
    for name, source, function, expected in _MATCHER:
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            (root / 'case.h').write_text(source)
            found = {d[2]: (d[3], d[4]) for d in definitions(root)}
        parameter, body = found.get(function, (None, ""))
        overrun = BEYOND_THE_BODY in body
        ok = parameter == expected and not overrun
        if verbose:
            if parameter != expected:
                detail = f" (wanted {expected!r}, read {parameter!r})"
            elif overrun:
                detail = " (body ran past its closing brace)"
            else:
                detail = ""
            print(f"   {'PASS' if ok else 'FAIL'}  matcher: {name}{detail}")
        if not ok:
            failures.append(name)
    for name, source in _NOT_A_PARAMETER:
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            (root / 'case.h').write_text(source)
            seen = list(definitions(root))
        ok = not seen
        if verbose:
            print(f"   {'PASS' if ok else 'FAIL'}  matcher: {name} is not a Terminal parameter")
        if not ok:
            failures.append(name)
    return len(failures)


def selftest(verbose: bool = True) -> int:
    failures = []
    for arm, cases, expect in (('A', _ARM_A, True), ('B', _ARM_B, False)):
        for name, body in cases:
            if tests_null(body, 't') is not expect:
                failures.append((arm, name))
    matcher = matcher_selftest(verbose)
    if verbose:
        print(f"selftest: {len(_ARM_A)} ARM A + {len(_ARM_B)} ARM B + "
              f"{len(_MATCHER) + len(_NOT_A_PARAMETER)} matcher = {len(_ARM_A) + len(_ARM_B) + len(_MATCHER) + len(_NOT_A_PARAMETER)} cases, "
              f"{len(failures) + matcher} failures")
        for arm, name in failures:
            print(f"  ARM {arm} FAILED: {name}")
    return len(failures) + matcher


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--root', default='Source/Core',
                    help='tree to scan (default: Source/Core)')
    ap.add_argument('--gate', action='store_true',
                    help='exit non-zero unless the count equals --budget exactly')
    ap.add_argument('--budget', type=int, default=None,
                    help='expected number of findings; pinned two-sided')
    ap.add_argument('--selftest', action='store_true',
                    help='run the predicate control and exit')
    ap.add_argument('--by-module', action='store_true',
                    help='summarise per directory instead of listing every site')
    args = ap.parse_args()

    if args.selftest:
        return 1 if selftest() else 0

    # A gate whose predicate is untrusted must not emit findings: ARM A misses
    # produce advice to delete working null checks.
    if args.gate and selftest(verbose=False):
        print('check-terminal-pointer-intent: FAIL - predicate selftest does not pass; '
              'refusing to report findings. Run --selftest.', file=sys.stderr)
        return 2

    root = pathlib.Path(args.root)
    if not root.is_dir():
        print(f'check-terminal-pointer-intent: FAIL - {root} is not a directory',
              file=sys.stderr)
        return 2

    scanned = sources(root)
    defs = list(definitions(root))
    if not defs:
        # Zero findings and zero definitions examined print the same "0" unless
        # something separates them. Two facts do: the selftest above proves the
        # predicate and the parser work on a tree with known answers, and the
        # file count proves this root had something to read. A root that
        # satisfies both and yields nothing has genuinely reached zero -- the
        # state this gate exists to reach, so it must be reportable as a pass
        # rather than as a scan that failed.
        if not scanned:
            print(f'check-terminal-pointer-intent: FAIL - no source files under {root}; '
                  f'the scan matched nothing rather than finding nothing', file=sys.stderr)
            return 2
        print(f'check-terminal-pointer-intent: ok - {len(scanned)} files under {root} '
              f'declare no Terminal* parameter at all')
        return 0

    findings, checked = [], 0
    for path, line, name, param, body in defs:
        if tests_null(body, param):
            checked += 1
        else:
            findings.append((path, line, name))

    if args.by_module:
        per = collections.Counter(str(p.parent) for p, _, _ in findings)
        for mod, n in sorted(per.items(), key=lambda kv: -kv[1]):
            print(f'{n:5}  {mod}')
    else:
        for path, line, name in findings:
            print(f'{path}:{line}: {name} takes Terminal* and never tests it')

    print(f'\n{len(defs)} definitions examined under {root}, {checked} test their '
          f'terminal, {len(findings)} do not')

    if not args.gate:
        return 0
    if args.budget is None:
        print('check-terminal-pointer-intent: FAIL - --gate requires --budget',
              file=sys.stderr)
        return 2
    if len(findings) > args.budget:
        # Two causes reach this branch and they want different fixes: a new
        # unchecked pointer was written, or an existing one lost its null check.
        # The second is the one worth naming -- it is a working guard going
        # missing, and a message about additions sends the reader looking for
        # the wrong diff.
        print(f'check-terminal-pointer-intent: FAIL - {len(findings)} findings, '
              f'budget {args.budget}. Either a new Terminal* that promises nothing was '
              f'added, or an existing one lost the null check that justified it.',
              file=sys.stderr)
        return 1
    if len(findings) < args.budget:
        print(f'check-terminal-pointer-intent: FAIL - {len(findings)} findings, '
              f'budget {args.budget}. Lower the budget to {len(findings)} in this commit; '
              f'a budget with slack in it cannot fail.', file=sys.stderr)
        return 1
    print(f'check-terminal-pointer-intent: ok ({len(findings)} = budget, under {root})')
    return 0


if __name__ == '__main__':
    sys.exit(main())
