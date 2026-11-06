#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""One concept, one word: the store Core reads is Preferences and nothing else.

Three vocabularies for one thing -- `Config` in a namespace, `Settings` in a key
table, `Preferences` at the PAL seam -- split along who wrote the file rather
than along any distinction a reader can use. `SettingsSchema` as Core data and
`SettingsRow` as macOS UI would share a word; `Config` as Core data and
`Preferences` as the seam would not.

One word is worth nothing without something that holds it. A word costs
nothing to reintroduce, reads correct in the file it lands in, and is invisible
until the tree has three names again -- so this refuses the two retired words in
the code that carries the store, and says where the boundary is.

What is not a violation, because each is the name of something else:

  - `config.toml`, its `[section]`s and its keys. The file belongs to the person
    editing it and its name is theirs. A rename that reached the TOML would
    break every configuration on disk.
  - The Sync wire. `MessageType::Config` gates a frame named CONFIG, and the
    enumerator follows the protocol rather than our vocabulary.
  - The macOS Settings window and every string a person reads. `SettingsView`
    names a window whose menu item says "Settings...", which is Apple's word for
    that window, not a second word for our store.
  - `configure`, `configuration`, `configured` as ordinary English about
    something other than the store.

So the rule is scoped to identifiers, in every tree that can name the store:
Source, Tests, Tools, Scripts and Docs.
"""

import argparse
import pathlib
import re
import sys
import tempfile

import gate_corpus
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import ubq_roots  # noqa: E402

# The retired names, listed rather than swept for.
#
# A sweep for either word is wrong, which is why this is a list.
#
# Sweeping for "Setting" catches real words: `FontFeatureSettings` and
# `FontVariationSettings` are OpenType's own spelling, `decrqssSettingReport` is
# a VT report, `LaunchSettings` is a bundle of resolved values rather than a name
# for the place they came from. The singular is not listed at all -- "Setting it
# mid-session is not an error" is a sentence, and the store was never called
# `Setting` -- while `SettingKey` and its siblings are listed by name.
#
# Sweeping for "Config" catches other people's configuration: wuffs answers a
# `wuffs_base__image_config`, TIFF has a PlanarConfiguration tag, and a SOCKS
# proxy has a `ProxyConfig`. None of those is this store, and demanding they be
# misspelled teaches a reader to bypass the gate rather than obey it.
#
# So the population is the names that named this store, and it can only shrink:
# an entry comes off when nothing spells it, never because a new file wants one.
RETIRED = re.compile(
    r'\b('
    r'Config|ConfigColor|ConfigTheme|ConfigKeyBinding|ConfigLinkHandler'
    r'|ConfigTrigger|ConfigMacro|ConfigCharacterClass|ConfigLanguageFallback'
    r'|ConfigRangeMap|ConfigSkippedKey|ConfigSkipReason'
    r'|ConfigAccessors|ConfigDefaults|ConfigWriter|ConfigFileTemplate|ConfigInterop'
    r'|loadConfig|loadConfigIntoDefaults|profileFromConfig|appearanceModeFromConfig'
    r'|remoteOptionsFromConfig|configWriteKey'
    # Families, not names: a list that holds the siblings of `themeFromConfig`
    # and `registerConfigThemes` and not them passes both, and `ThemesConfig`
    # needs its own entry because a word boundary after `Config` cannot match
    # the `s` of `Themes`. `configWriteSetting*` is
    # the same shape: an overload listed by name leaves every unlisted overload
    # to pass a green gate. An overload set is one name, and is matched as one.
    r'|\w+FromConfig|register\w*ConfigThemes|reloadConfigFromFile'
    r'|configWriteSetting\w*'
    r'|ThemesConfig|RemoteOptionsFromConfig'
    r'|SettingKey|SettingsKeys|SettingsSchema'
    r')\b'
    r'|Core/Config/')

# What the rule is scoped to: the whole tree.
#
# Not only Source/Core and Tests/Core, where the three vocabularies collide: a
# Swift module, installed headers, the App seam and comments elsewhere carry the
# old names too. A gate scoped to Core reads Core as clean and says nothing
# about anywhere else, which is the shape of a scan whose scope excludes the
# answer.
SCOPED = ('Source', 'Tests', 'Tools', 'Scripts', 'Docs')

# Two populations, because the two words are not equally unambiguous.
#
# `Config` and its compounds name nothing else here, so they are refused
# everywhere. Bare `Settings` is Apple's name for the macOS window, and
# SettingsView/Catalog/Pane/Category are its UI — refusing those tree-wide would
# demand a frontend misname a window after our store. So bare `Settings` is
# retired only where the aggregate itself lived.
NARROW = re.compile(r'\b(Settings|SettingsCategory)\b')
NARROW_SCOPE = ('Source/Core', 'Tests/Core')

# A line that is exempt says why, in the line itself. Rather than a path list
# somewhere else, which is a second file to keep true, and which cannot say
# which of a file's forty occurrences it meant.
EXEMPTION = re.compile(r'preferences-vocabulary-ok:')

# Names that are not ours to choose, matched before the retired words are
# looked for. Each is the name of a thing outside this repository's vocabulary:
# a protocol frame, a file a person edits, a platform's own window.
NOT_OURS = (
    re.compile(r'MessageType::Config\b'),
    # `Scope::Settings` is the command palette's scope for the Settings window,
    # the same shape as the wire enumerator above: a name another surface chose.
    re.compile(r'Scope::(Config|Settings)\b'),
    # The palette's own command group, which populates that scope, and the
    # enumerator declaring the scope itself. Both name the window, and the
    # group is reached unqualified from inside the palette's namespace.
    re.compile(r'CommandPalette::Settings\b'),
    re.compile(r'(?<![:\w])Settings::(formatNumber|rangeHint|commands)\b'),
    re.compile(r'^\s*Settings,\s*//'),
    re.compile(r'\bconfig\.toml\b'),
    re.compile(r'\bencodeConfig\b|\bapplyConfig\b|\bserializeConfig\b'),  # the CONFIG frame's codec
    re.compile(r'\bcapabilityConfig\b'),
    re.compile(r'\bXDG_CONFIG_HOME\b'),
)


def isNarrowlyScoped(path: pathlib.Path, root: pathlib.Path) -> bool:
    """Whether `path` is where bare `Settings` is this store's retired word."""
    relative = str(path.relative_to(root))
    return relative.startswith(NARROW_SCOPE)


def isExempt(line: str) -> bool:
    """Whether `line` may carry a retired word."""
    if EXEMPTION.search(line):
        return True
    return any(pattern.search(line) for pattern in NOT_OURS)


def sources(root: pathlib.Path, eligible=None):
    """Every file the scan reads, so an empty result can be told from an empty root."""
    found = []
    # This file names every word it refuses, so it matches itself on every line
    # of the list. Excluded, and the selftest below is what makes that safe: it
    # asserts the list still holds `Config` and `loadConfig` and
    # `configWriteSettingBool`, so a tree-wide sweep that rewrites them inside
    # this file fails ARM A rather than passing silently with a list that bans
    # the new names.
    ownPath = pathlib.Path(__file__).resolve()
    suffixes = ('.h', '.hpp', '.cpp', '.mm', '.m', '.swift', '.py', '.sh',
                '.md', '.modulemap', '.js', '.mjs', '.txt', '.tsv', '.toml', '.cmake')
    if eligible is not None:
        candidates = (root / path for path in eligible)
        found.extend(path for path in candidates
                     if path.relative_to(root).parts[0] in SCOPED
                     and (path.name == 'CMakeLists.txt' or path.suffix in suffixes)
                     and 'ThirdParty' not in path.parts
                     and path.resolve() != ownPath)
        return sorted(found)

    for directory in SCOPED:
        base = root / directory
        if not base.is_dir():
            continue
        for suffix in tuple(f'*{item}' for item in suffixes) + ('CMakeLists.txt',):
            found.extend(p for p in base.rglob(suffix)
                         if 'ThirdParty' not in p.parts
                         and p.resolve() != ownPath)
    return sorted(found)


def findings(root: pathlib.Path, eligible=None):
    """Yield (path, line number, text) for each retired word in the store's code."""
    for path in sources(root, eligible):
        # Whether the file sits in the narrow scope is a property of the file, so
        # it is answered once for it. Asked per line, it built a
        # Path.relative_to for every line of the tree -- each one normalises
        # case and walks segments -- for an answer that could not change
        # between two lines of the same file.
        narrow = isNarrowlyScoped(path, root)
        for number, text in enumerate(path.read_text(errors='replace').splitlines(), 1):
            # A retired word is what makes a line a candidate, and only a
            # candidate can be exempt. Asked first, the exemption ran every
            # exemption pattern over every line in the tree to answer a question
            # that decides nothing for a line carrying no retired word at all.
            if not (RETIRED.search(text) or (narrow and NARROW.search(text))):
                continue
            if isExempt(text):
                continue
            yield path, number, text.strip()


CASES = (
    # (source line, must be reported)
    ('struct Config {', True),
    ('    Config& current();', True),
    ('enum class SettingKey {', True),
    ('void apply(const Settings& s);', True),
    # The singular is a gerund far more often than a type.
    ('// Setting it mid-session is not an error and not a resize.', False),
    ('#include "Core/Config/Config.h"', True),
    # These four exist because a tree-wide sweep can rewrite them INSIDE this
    # file's own list, leaving a gate that bans the new names and passes the
    # old ones. The selftest is the only thing that can notice that, so every
    # name a sweep would plausibly hit gets a case.
    ('    loadConfig(path);', True),
    ('    configWriteSettingBool(key, value);', True),
    # The two overloads a by-name list misses. They are here rather than in the
    # list above because a case that only re-states a literal proves the literal;
    # these prove the family pattern the gate matches.
    ('    configWriteSettingDouble(key, value);', True),
    ('    configWriteSettingLegacyBoolOrToken(key, serialized);', True),
    ('    Theme themeFromConfig(const ThemeEntry&);', True),
    ('    void registerConfigThemes(const Values&);', True),
    # The store's own vocabulary.
    ('struct Values {', False),
    ('    Values& values();', False),
    ('enum class Key {', False),
    # Names another party chose.
    ('    case MessageType::Config:', False),
    ('// written to ~/.config/ubiquitty/config.toml on first run', False),
    ('void serializeConfig(const Values& c, std::vector<uint8_t>& out);', False),
    ('    if (const char* home = std::getenv("XDG_CONFIG_HOME"))', False),
    # Ordinary English about something other than the store.
    ('// configure the atlas before the first draw', False),
    ('    const int configuredWeight = weightFor(face);', False),
    # An exemption states itself.
    ('    int Settings = 0; // preferences-vocabulary-ok: the wire frame name', False),
)

# The files a build is written in. Every case above is written to a header, so
# none of them sees a kind of file the scan does not open, and a build keeps as
# much of its text in an included .cmake module as in a CMakeLists.txt. Each of
# these carries a retired word, and both the walk and the listed population
# must report it.
BUILD_FILES = ('Tests/CMakeLists.txt', 'Scripts/cmake/Module.cmake')


def selftest(verbose: bool) -> int:
    """Run every case in its own tree. Returns the failure count."""
    failures = 0
    for index, (line, expected) in enumerate(CASES):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            # One file per case, named for the case: a lookup that succeeds when
            # any case at all matches cannot tell which one did.
            scoped = root / 'Source' / 'Core'
            scoped.mkdir(parents=True)
            (scoped / f'case{index}.h').write_text(line + '\n')
            got = len(list(findings(root))) > 0
        arm = 'A' if expected else 'B'
        if got != expected:
            failures += 1
            print(f'  FAIL  ARM {arm}: {line}\n        reported={got} expected={expected}',
                  file=sys.stderr)
        elif verbose:
            print(f'  PASS  ARM {arm}: {line}')
    for relative in BUILD_FILES:
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            path = root / relative
            path.parent.mkdir(parents=True)
            path.write_text('# loadConfig reads the store\n')
            walked = len(list(findings(root))) > 0
            listed = len(list(findings(root, [relative]))) > 0
        if not (walked and listed):
            failures += 1
            print(f'  FAIL  ARM A: {relative} is not read\n'
                  f'        walked={walked} listed={listed} expected=True',
                  file=sys.stderr)
        elif verbose:
            print(f'  PASS  ARM A: {relative} is read')
    positives = sum(1 for _, e in CASES if e) + len(BUILD_FILES)
    total = len(CASES) + len(BUILD_FILES)
    print(f'selftest: {positives} ARM A + {total - positives} ARM B = '
          f'{total} cases, {failures} failures')
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
    # a tree that has grown a second vocabulary back.
    if selftest(verbose=False):
        print('check-preferences-vocabulary: FAIL - predicate selftest does not pass; '
              'refusing to report. Run --selftest.', file=sys.stderr)
        return 2

    root = pathlib.Path(args.root)
    eligible = gate_corpus.eligible_under(root)
    scanned = sources(root, eligible)
    if not scanned:
        print(f'check-preferences-vocabulary: FAIL - no source files under {root}; the scan '
              f'matched nothing rather than finding nothing', file=sys.stderr)
        return 2

    found = list(findings(root, eligible))
    if not found:
        print(f'check-preferences-vocabulary: ok (one vocabulary across '
              f'{len(scanned)} file(s) in {", ".join(SCOPED)})')
        return 0

    print(f'check-preferences-vocabulary: FAILED — {len(found)} retired word(s) in the '
          f'store\'s own code:', file=sys.stderr)
    for path, number, text in found[:40]:
        print(f'  {path.relative_to(root)}:{number}: {text}', file=sys.stderr)
    if len(found) > 40:
        print(f'  … and {len(found) - 40} more', file=sys.stderr)
    print('\n  Preferences is the word. `Values` is the parsed set, `Key` is a key\n'
          '  handle, `Schema` is the ranges. A line naming something another party\n'
          '  chose — a CONFIG wire frame, config.toml, a platform\'s own window —\n'
          '  carries `preferences-vocabulary-ok: <why>` and says which.', file=sys.stderr)
    return 1


if __name__ == '__main__':
    sys.exit(main())
