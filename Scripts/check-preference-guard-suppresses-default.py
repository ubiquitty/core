#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo

# SPDX-License-Identifier: Apache-2.0

"""A presence guard around an accessor read suppresses the default it supplies.

    if (c.ligatures.has_value())
        renderer.usesLigatures = Effective::ligatures(c);

The accessor answers `value_or(Defaults::ligatures)`. It has a default for
exactly the case the guard skips, so the guard can only ever prevent the default
from being applied — never change what happens when the key is set.

What that costs is not obvious from the line. It is a key deleted from
config.toml: the reload runs, the guard skips, and the last value the settings
pane wrote stays on screen until the next launch. The file says one thing and
the terminal shows another, with nothing to indicate which is which.

The comment beside `applyFontFeatures` says why:

    Unset means the file names no features, which is a value: a reader deleting
    the key expects the face's own defaults back, and skipping the call would
    leave the ones the pane last wrote standing.

`if (x.has_value())` is correct and common on its own -- around a `*x`, around
work that has no default to fall back to, around anything that is not this. Only
the pairing is refused: a guard on a key, and a body that reads that same key
through the accessor. That combination is redundant when it is
harmless and a deleted key that does not take effect when it is not.
"""

import argparse
import pathlib
import re
import sys
import tempfile
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import ubq_roots  # noqa: E402

# No `$` anchor: the guarded statement may sit on the same line.
GUARD = re.compile(r'\bif\s*\(\s*([A-Za-z_]\w*)\.([A-Za-z_]\w*)\.has_value\(\)\s*\)')

# The body may sit on the next line or on the same one after the guard.
def accessorNames(root: pathlib.Path) -> set:
    """Every key `Preferences::Effective::` can answer with a default.

    A guard around `*c.x` is the CORRECT idiom when nothing can supply a default
    for x. It is the same defect as a guard around the accessor only when an
    accessor exists — so the rule needs to know which keys have one, and the
    header is where that is recorded rather than a list kept beside it.
    """
    header = root / 'Source' / 'Core' / 'Preferences' / 'PreferencesAccessors.h'
    if not header.is_file():
        # A repository that carries Core as a library reads Core's header.
        header = pathlib.Path(ubq_roots.core_root(str(root))) / 'Source' / 'Core' / 'Preferences' / 'PreferencesAccessors.h'
    if not header.is_file():
        return set()
    return set(re.findall(r'\b([a-z]\w*)\(const Values& ?\w*\)\s*;',
                          header.read_text(errors='replace')))


# An APPLY site: a message that pushes a value into the running app. The deref
# rule is confined to these, because a guard around `*x` is correct everywhere
# else and the alternatives are not rare -- an accessor implementing its own
# default (`if (c.blurRadius.has_value()) return *c.blurRadius;`), a serializer
# emitting only the keys a profile states, and a profile overlay falling back to
# the global rather than to the default. All three read identically and all three
# are right. Only pushing a value into a live window is wrong to skip.
APPLY_SITE = re.compile(r'\[\s*[A-Za-z_]\w*\s+(apply|set)[A-Z]\w*:')

# ...and only when the guard is on the global set. A Profile overlays what the
# global already applied, so a profile that states nothing must leave that
# standing rather than reset to the built-in — which is what
# `hideTitleBar(const Values&, const Profile&)` says by falling back to the
# global rather than to Defaults. Same guard, opposite meaning, and only the
# owner tells them apart.
GLOBAL_OWNERS = ('c', 'cfg', 'config', 'configuration', 'live', 'values')


def dereferenceRead(field: str) -> re.Pattern:
    """Matches a dereference of `field`, which skips the default it could have."""
    return re.compile(r'\*\s*[A-Za-z_]\w*\.' + re.escape(field) + r'\b')


def accessorRead(field: str) -> re.Pattern:
    """Matches a read of `field` through the effective-value accessor."""
    # No lookbehind on `:`. An ObjC message puts one immediately before the
    # namespace — `[wc applyLigatures:Effective::ligatures(c)]` — and a
    # lookbehind meant to reject `Foo::Effective::` rejected that instead,
    # which is every site this rule exists for.
    return re.compile(r'Effective::' + re.escape(field) + r'\s*\(')


EXEMPTION = re.compile(r'guard-ok:')

EXCLUDED = re.compile(r'ThirdParty|/build/|BuildHostTests|BuildAgent|BuildWeb')


def sources(root: pathlib.Path):
    """Every file the scan reads, so an empty result can be told from an empty root."""
    found = []
    for area in ('Source', 'Tests', 'Tools'):
        base = root / area
        if not base.is_dir():
            continue
        for suffix in ('*.h', '*.hpp', '*.cpp', '*.mm', '*.m'):
            found.extend(p for p in base.rglob(suffix)
                         if not EXCLUDED.search('/' + str(p.relative_to(root))))
    return sorted(found)


def guardedStatements(lines, number, text, at):
    """The lines the guard controls: the rest of its own, then its body.

    A brace matters here. One line past the guard is the whole body only when
    there is exactly one statement and no braces -- and adding a second
    statement is precisely what makes someone reach for braces. Reading one
    line misses `if (x.has_value()) {` with the read three lines down, so the
    rule would hold only until the code it watches grew.
    """
    rest = text[at:]
    if rest.strip() and not rest.strip().startswith('{'):
        yield rest
        return
    if '{' not in rest:
        # Unbraced: the single statement on the following line.
        if number < len(lines):
            yield lines[number]
        return
    depth = rest.count('{') - rest.count('}')
    index = number
    while index < len(lines) and depth > 0:
        line = lines[index]
        yield line
        depth += line.count('{') - line.count('}')
        index += 1


def findings(root: pathlib.Path):
    """Yield (path, line number, guard, body) for each suppressing guard."""
    accessors = accessorNames(root)
    for path in sources(root):
        lines = path.read_text(errors='replace').splitlines()
        for number, text in enumerate(lines, 1):
            if EXEMPTION.search(text):
                continue
            match = GUARD.search(text.rstrip())
            if not match:
                continue
            field = match.group(2)
            # A dereference counts only when an accessor could have answered.
            # The accessor name is not always the field name -- `lineSpacingPoints`
            # is served by `lineSpacing` -- so both spellings are asked about.
            served = field in accessors or field.removesuffix('Points') in accessors
            for body in guardedStatements(lines, number, text, match.end()):
                if EXEMPTION.search(body):
                    break
                isGlobalApply = (match.group(1) in GLOBAL_OWNERS
                                 and APPLY_SITE.search(body)
                                 and dereferenceRead(field).search(body))
                if accessorRead(field).search(body) or (served and isGlobalApply):
                    yield path, number, text.strip(), body.strip()
                    break


CASES = (
    # (source text, must be reported)
    ('if (c.ligatures.has_value())\n    [wc applyLigatures:Effective::ligatures(c) ? YES : NO];', True),
    ('if (cfg.vsync.has_value())\n    [view setVSyncEnabled:Effective::vsync(cfg) ? YES : NO];', True),
    # Same line rather than the next one.
    ('if (c.folderIcon.has_value()) apply(Effective::folderIcon(c));', True),
    # Braced, which is what someone reaches for when the body grows. The first
    # version of this rule read exactly one line past the guard and missed both,
    # so it held only until the code it watches gained a second statement.
    ('if (c.ligatures.has_value()) {\n    renderer.usesLigatures = Effective::ligatures(c);\n}', True),
    ('if (c.ligatures.has_value()) {\n    log();\n    note();\n    renderer.usesLigatures = Effective::ligatures(c);\n}', True),
    # Past the closing brace, so the guard does not control it.
    ('if (c.ligatures.has_value()) {\n    log();\n}\nrenderer.usesLigatures = Effective::ligatures(c);', False),
    # A guard around a dereference, where an accessor could have answered. Same
    # defect wearing a different spelling: ten numeric setters read this way on
    # the macOS arm after the bool ones were routed.
    ('if (c.ligatures.has_value())\n    [wc applyLigatures:*c.ligatures ? YES : NO];', True),
    ('if (c.lineSpacingPoints.has_value())\n    [wc applyLineSpacing:static_cast<CGFloat>(*c.lineSpacingPoints)];', True),
    # The three correct shapes that read identically to the defect.
    ('if (c.ligatures.has_value())\n    return *c.ligatures;', False),
    ('if (p.lineSpacing.has_value()) {\n    out += tomlDouble(*p.lineSpacing);\n}', False),
    ('if (profile.lineSpacing.has_value())\n    resolved.lineSpacing = *profile.lineSpacing;', False),
    # A profile overlays the global: unset means inherit, not reset.
    ('if (profile.lineSpacing.has_value())\n    [self applyLineSpacing:static_cast<CGFloat>(*profile.lineSpacing)];', False),
    # A guard around a deref of a key no accessor serves is the correct idiom:
    # there is no default to suppress.
    ('if (c.themeName.has_value())\n    registerTheme(*c.themeName);', False),
    # A guard around a DIFFERENT key's accessor is not this rule's business.
    ('if (c.ligatures.has_value())\n    [wc applyBrightenBold:Effective::brightenBold(c) ? YES : NO];', False),
    # A guard around work with no default to fall back to.
    ('if (c.themeName.has_value())\n    registerTheme(*c.themeName);', False),
    # The accessor read on its own, which is the shape this rule wants.
    ('[wc applyLigatures:Effective::ligatures(c) ? YES : NO];', False),
    # A line that states its reason.
    ('if (c.ligatures.has_value()) // guard-ok: the caller re-applies on its own\n    use(Effective::ligatures(c));', False),
)


def selftest(verbose: bool) -> int:
    """Run every case in its own tree. Returns the failure count."""
    failures = 0
    for index, (text, expected) in enumerate(CASES):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            (root / 'Source').mkdir()
            # The accessor header, because the deref rule asks it which keys
            # have a default to suppress. Without it every deref case passes
            # vacuously -- the selftest would be green over a rule that never
            # ran, which is the shape it exists to prevent.
            accessors = root / 'Source' / 'Core' / 'Preferences'
            accessors.mkdir(parents=True)
            (accessors / 'PreferencesAccessors.h').write_text(
                'bool ligatures(const Values& c);\n'
                'double lineSpacing(const Values& c);\n')
            # One file per case, named for the case: a lookup that succeeds when
            # any case at all matches cannot tell which one did.
            (root / 'Source' / f'case{index}.mm').write_text(text + '\n')
            got = any(f[0].name == f'case{index}.mm' for f in findings(root))
        arm = 'A' if expected else 'B'
        rendered = ' | '.join(part.strip() for part in text.splitlines())
        if got != expected:
            failures += 1
            print(f'  FAIL  ARM {arm}: {rendered}\n'
                  f'        reported={got} expected={expected}', file=sys.stderr)
        elif verbose:
            print(f'  PASS  ARM {arm}: {rendered}')
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
    # a tree where deleting a key has quietly stopped taking effect.
    if selftest(verbose=False):
        print('check-preference-guard-suppresses-default: FAIL - predicate selftest does '
              'not pass; refusing to report. Run --selftest.', file=sys.stderr)
        return 2

    root = pathlib.Path(args.root)
    scanned = sources(root)
    if not scanned:
        print(f'check-preference-guard-suppresses-default: FAIL - no source files under '
              f'{root}', file=sys.stderr)
        return 2

    found = list(findings(root))
    if not found:
        print(f'check-preference-guard-suppresses-default: ok ({len(scanned)} file(s), no '
              f'presence guard suppresses an accessor default)')
        return 0

    print(f'check-preference-guard-suppresses-default: FAILED — {len(found)} guard(s) '
          f'suppressing the default the accessor supplies:', file=sys.stderr)
    for path, number, guard, body in found[:30]:
        print(f'  {path.relative_to(root)}:{number}: {guard}\n      {body}', file=sys.stderr)
    if len(found) > 30:
        print(f'  … and {len(found) - 30} more', file=sys.stderr)
    print('\n  The accessor already answers value_or(Defaults::…), so the guard can\n'
          '  only suppress the default. What it costs is a key deleted from\n'
          '  config.toml: the reload skips, and the value the pane last wrote stays\n'
          '  on screen. Drop the guard, or say why with `guard-ok: <reason>`.',
          file=sys.stderr)
    return 1


if __name__ == '__main__':
    sys.exit(main())
