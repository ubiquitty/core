#!/usr/bin/env bash
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

#
# check-forks.sh — one implementation, checked against the symbol tables.
#
#   ./Scripts/check-forks.sh BUILDDIR [BUILDDIR...]
#
# Docs/coding-conventions.md §10 says portable policy lives in Core and every
# frontend routes through it. A fork of that policy is a security question, not
# a style one: with two byte-identical copies of an ownership gate, the test
# suite can link the copy without the attack surface while the copy that
# accepts a file descriptor from a peer it did not spawn goes untested.
#
# A fork is visible in the object files whether or not anyone notices it in the
# source: two translation units defining the same strong symbol. That is what
# this reads.
#
# Why the symbol table and not the source. Two copies of a function drift in
# whitespace, in comments, in a variable name, and a textual comparison stops
# seeing them. A linker does not care: the mangled name is the same or it is
# not. The check inherits that precision and costs one nm per object.
#
# Why more than one build directory. A Core-only configure compiles neither
# frontend, so a fork BETWEEN frontends — which is the shape that hides longest,
# because no single build sees both halves — is invisible to it. Pass every
# configuration the host can build and the union is what a reader of the whole
# tree would see. A pass over one configuration is a statement about that
# configuration and not about the tree; the summary names the builds it read.
#
# Weak symbols are not forks. An inline function, a template instantiation and a
# vtable are emitted in every translation unit that needs them, and the linker
# folds them. Only strong definitions — one owner by construction — say anything
# about how many implementations exist.
#
# What a pass here does not mean. Keying on the mangled name buys the precision
# above and pays for it in one place: two implementations under DIFFERENT names
# are not a symbol collision, so nothing here can see them. A saturating
# decimal accumulate defined once for OSC and once for VT under two names,
# byte-identical, is such a pair, and this check is green across every
# configuration while it stands.
#
# So a pass says no symbol is defined twice. It does not say the tree holds one
# implementation of anything, and reading it as "no duplication" is reading a
# narrower answer as a broader one. Finding the different-name kind takes a
# person reading.
#

set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"

if [ "$#" -eq 0 ]; then
    sed -n '/^#!/d; /^[^#]/q; s/^# \{0,1\}//; p' "$0"
    exit 2
fi

for dir in "$@"; do
    [ -d "$dir" ] || { echo "check-forks: not a directory: $dir" >&2; exit 2; }
done

command -v nm >/dev/null || { echo "check-forks: nm not found" >&2; exit 2; }

# `stat` is the one tool here with two dialects: BSD spells mtime `-f '%m'`,
# GNU spells it `-c '%Y'` — and GNU's `-f` means "file system status", so on a
# PATH where coreutils' gnubin shadows /usr/bin (a common Homebrew setup), the BSD
# spelling silently returns fs-status prose, every timestamp parse fails, and
# the newest-object scan reports "no .o found" against a fully built tree.
# Probe once against this script itself and commit to whichever dialect answers.
if stat -f '%m' "$0" >/dev/null 2>&1; then
    STAT_MTIME=(-f '%m')
else
    STAT_MTIME=(-c '%Y')
fi

# The objects have to be newer than the sources, because every answer here is a
# statement about them and none of it says which build it read.
#
# A stale tree gets this exactly backwards in both directions: a fork already
# merged away still has two definitions in the old objects and is reported, and a
# fork introduced since the build has one definition and is not. Both readings
# look like findings about the working tree.
#
# Refusing rather than warning, because this is typed by hand after a build and
# the recovery is to run the build again. An mtime is not identity — objects from
# another branch can be newer than every source here — but it catches the case
# that happens, which is a build that was not re-run.
# Each object against the source that produced it, not against the newest source
# in the tree. `git ls-files Source/` stands in for "the sources this build
# compiles", and the two agree until a commit touches a file this build has no
# translation unit for — then the guard refuses forever and rebuilding cannot
# clear it, because ninja is right to do nothing.
#
# ubiquitty-layers hit that in check-linux-smokes' guard after a macOS-only
# commit: a Metal renderer source is in no Linux compile database, so the newest source
# outran a binary that was current for everything Linux builds.
#
# An object names its own source — the path ends in `<source>.o` — so the exact
# comparison is available here and no approximation of the population is needed.
newestObject=0
for dir in "$@"; do
    stamp="$(find "$dir" -name '*.o' -exec stat "${STAT_MTIME[@]}" {} + 2>/dev/null | sort -rn | head -1)"
    [ -n "$stamp" ] && [ "$stamp" -gt "$newestObject" ] && newestObject="$stamp"
done
if [ "$newestObject" -eq 0 ]; then
    echo "check-forks: FAILED — no .o found under $* — nothing was read." >&2
    exit 1
fi
# The sources each configuration actually compiles, read from its own compile
# database. An object whose source is absent from every database is a LEFTOVER
# from an older configuration — the target stopped compiling that file and the
# .o stayed on disk, unreferenced by any edge.
#
# The two states need different actions and the message for one cannot clear the
# other: "rebuild" is right for a stale object and impossible for an orphan,
# because nothing rebuilds an object no edge produces. Conflating them leaves
# this gate refusing on any long-lived build directory with advice that cannot
# work, and a gate like that runs only when someone types it.
#
# A directory with no database cannot be classified, so its objects stay in the
# staleness comparison rather than being quietly excused as orphans.
# One database per directory, never pooled. A union lets one configuration vouch
# for another's leftovers: build/macos compiles OverlaySpanGather.cpp and
# build/host does not, so a pooled list calls build/host's orphaned object live
# and reports its age as a finding about the tree. The question is whether this
# directory builds this source, and only its own database can answer it.
databaseDir="$(mktemp -d)"
for dir in "$@"; do
    [ -f "$dir/compile_commands.json" ] || continue
    grep -oE '"file"[[:space:]]*:[[:space:]]*"[^"]+"' "$dir/compile_commands.json" 2>/dev/null \
        | sed -E 's/.*"([^"]+)"$/\1/' | sort -u > "$databaseDir/$(printf '%s' "$dir" | tr '/' '_')"
done

orphanList="$(mktemp)"
stalePairs="$(
    for dir in "$@"; do
        find "$dir" -name '*.o' 2>/dev/null | sed "s|^|$(printf '%s' "$dir" | tr '/' '_')\t|"
    done | while IFS="$(printf '\t')" read -r objectDirectoryKey object; do
        # The object path carries the source PATH, not just its name:
        # .../ubiquitty-core.dir/Source/Platform/POSIX/Spawn.cpp.o. Keying on the
        # basename and taking the first git lists picks the wrong file wherever a
        # name repeats, and eleven do — Spawn.cpp, Socket.cpp, Poll.cpp,
        # SystemPAL.cpp and SocketPath.cpp each exist under both POSIX and Wasm,
        # and main.cpp four times.
        #
        # A wrong pairing here compares an object with a source it was not built
        # from, and the answer is a refusal or a pass with equal confidence and no
        # way to tell which. A suffix match inflates a header consumer count the
        # same way: a name is a prefix of the answer, never the answer.
        source="${object#*/Source/}"
        source="Source/${source%.o}"
        [ -f "$ROOT/$source" ] || continue
        [ -n "$source" ] || continue
        sourceStamp="$(stat "${STAT_MTIME[@]}" "$ROOT/$source" 2>/dev/null || echo 0)"
        objectStamp="$(stat "${STAT_MTIME[@]}" "$object" 2>/dev/null || echo 0)"
        [ "$objectStamp" -lt "$sourceStamp" ] || continue
        # Orphan rather than stale: no configuration here compiles this source,
        # so its age says nothing about the tree and rebuilding cannot move it.
        database="$databaseDir/$objectDirectoryKey"
        if [ -f "$database" ] && ! grep -qxF "$ROOT/$source" "$database"; then
            printf '%s\n' "$source" >> "$orphanList"
            continue
        fi
        printf '%s\t%s\n' "$source" "$object"
    done
)"
orphanCount="$(wc -l < "$orphanList" 2>/dev/null | tr -d ' ')"
if [ "${orphanCount:-0}" -gt 0 ]; then
    echo "check-forks: note — $orphanCount object(s) belong to no current configuration, first $(head -1 "$orphanList")" >&2
    echo "  A target stopped compiling that source and its .o stayed on disk. Not a fork" >&2
    echo "  and not a stale build: rebuilding cannot move an object no edge produces." >&2
    echo "  Delete the build directory to clear them." >&2
fi
rm -rf "$orphanList" "$databaseDir"

if [ -n "$stalePairs" ]; then
    count="$(printf '%s\n' "$stalePairs" | wc -l | tr -d ' ')"
    first="$(printf '%s\n' "$stalePairs" | head -1 | cut -f1)"
    echo "check-forks: FAILED — $count object(s) older than the source they were compiled from, first $first" >&2
    printf '%s\n' "$stalePairs" | head -5 | while IFS="$(printf '\t')" read -r s o; do
        echo "    $s  →  $(basename "$o")" >&2
    done
    echo "  A fork merged away still has two definitions in these objects, and one" >&2
    echo "  added since the build has one. Rebuild, then read this." >&2
    exit 1
fi

python3 - "$ROOT" "$@" <<'PYTHON'
import collections
import os
import re
import subprocess
import sys

root, build_dirs = sys.argv[1], sys.argv[2:]

# A strong definition on Darwin: an external symbol in a section, without the
# "weak" that marks a linker-foldable emission.
#
# The section is (__TEXT,__text) for a compiled object and (LTO,CODE) for a
# bitcode one. A build with link-time optimisation emits only the latter, so a
# pattern matching Mach-O sections alone reads nothing from it and reports a
# clean result for a configuration it never looked at.
DARWIN_STRONG = re.compile(
    r'^[-0-9a-f]+ \((?:__[A-Z]+,__[a-z_]+|LTO,[A-Z]+)\) external (.+)$')
# The ELF equivalent: an uppercase type letter is a strong definition, and the
# lowercase ones plus V and W are local or weak.
ELF_STRONG = re.compile(r'^[0-9a-f]* ([TDBR]) (.+)$')


def strong_symbols(path):
    """Every strong definition in one object file, by mangled name."""
    darwin = subprocess.run(['nm', '-m', path], capture_output=True, text=True)
    if darwin.returncode == 0 and ('(__' in darwin.stdout or '(LTO,' in darwin.stdout):
        return {m.group(1) for m in
                (DARWIN_STRONG.match(l.strip()) for l in darwin.stdout.split('\n')) if m}
    elf = subprocess.run(['nm', '--defined-only', path], capture_output=True, text=True)
    return {m.group(2) for m in
            (ELF_STRONG.match(l.strip()) for l in elf.stdout.split('\n')) if m}


# The sources the tree actually has. A build directory is not cleaned when a
# source moves, so the object of a file that no longer exists sits there and
# keeps answering nm — and its symbols look exactly like a second implementation
# of the file that replaced it. Every such object is a false fork, and believing
# one costs more than the check saves.
tracked = subprocess.run(['git', '-C', root, 'ls-files'], capture_output=True, text=True)
live_sources = {os.path.basename(p) for p in tracked.stdout.split()}

# Every object CMake produced, keyed by the source that produced it. An object
# path ends in <source>.o, which is the only place the source name survives.
symbol_sources = collections.defaultdict(set)
object_count = 0
stale = collections.Counter()
for build in build_dirs:
    for dirpath, _, filenames in os.walk(build):
        for name in filenames:
            if not name.endswith('.o'):
                continue
            source = name[:-2]
            if source not in live_sources:
                stale[source] += 1
                continue
            object_count += 1
            for symbol in strong_symbols(os.path.join(dirpath, name)):
                symbol_sources[symbol].add(source)

# An entry point per binary is what a binary is, not a second implementation.
symbol_sources.pop('_main', None)
symbol_sources.pop('main', None)

duplicates = {s: v for s, v in symbol_sources.items() if len(v) > 1}

# The two sanctioned shapes, each recognised by structure rather than by name.
#
# A PAL contract is DECLARED once and IMPLEMENTED once per embedder — that is
# what a platform abstraction layer is, and it is the exception §10 names.
#
# A test double replaces a Core symbol at link time to pin behaviour the real
# one cannot produce on demand. It is a second definition on purpose and it
# never ships.
PAL_CONTRACT = re.compile(r'Ubiquitty[0-9]*3PAL[0-9]')


def is_test_source(source):
    return source.startswith(('test_', 'bench_', 'perf_', 'profile_'))


forks, contracts, doubles = [], [], []
for symbol, sources in sorted(duplicates.items()):
    if PAL_CONTRACT.search(symbol):
        contracts.append((symbol, sources))
    elif any(is_test_source(s) for s in sources):
        doubles.append((symbol, sources))
    else:
        forks.append((symbol, sources))


def demangle(symbols):
    if not symbols:
        return {}
    out = subprocess.run(['c++filt'], input='\n'.join(symbols),
                         capture_output=True, text=True)
    return dict(zip(symbols, out.stdout.split('\n'))) if out.returncode == 0 else {}


def report(title, entries, detail):
    if not entries:
        return
    print(f'{title}')
    print(f'  {detail}')
    names = demangle([s for s, _ in entries])
    grouped = collections.defaultdict(list)
    for symbol, sources in entries:
        grouped[tuple(sorted(sources))].append(names.get(symbol, symbol))
    for sources, symbols in sorted(grouped.items(), key=lambda kv: -len(kv[1])):
        print(f'\n  {" + ".join(sources)}  — {len(symbols)} symbol(s)')
        for name in sorted(symbols)[:8]:
            print(f'      {name}')
        if len(symbols) > 8:
            print(f'      ... and {len(symbols) - 8} more')
    print()


print(f'check-forks: {object_count} objects from {len(build_dirs)} build '
      f'director{"y" if len(build_dirs) == 1 else "ies"}')
if len(build_dirs) == 1:
    print('  One configuration only, so a fork between two frontends cannot appear here.')
if stale:
    print(f'  {sum(stale.values())} stale object(s) skipped, from {len(stale)} source(s) '
          f'the tree no longer has: {", ".join(sorted(stale)[:4])}'
          f'{" ..." if len(stale) > 4 else ""}')
    print('  A build directory keeps the object of a moved source, and that object')
    print('  keeps answering nm. Clear them with:')
    for build in build_dirs:
        print(f'    cmake --build {build} --target clean && cmake --build {build}')
print()

report('Sanctioned — one PAL contract, one implementation per embedder:', contracts,
       'The exception §10 names. Nothing to do.')
report('Sanctioned — a test replaces a symbol to pin behaviour it cannot otherwise reach:',
       doubles, 'A second definition that only a test binary links; no shipping target sees it.')
report('FORKS — one behaviour, more than one implementation:', forks,
       'Docs/coding-conventions.md §10: portable policy lives in Core and every '
       'frontend routes through it.')

if forks:
    print('A fork is two places to fix a bug and one place someone will forget. Move')
    print('the behaviour to the layer both callers already depend on, and delete the')
    print('copy — a test proving they agree today does not stop them diverging.')
    sys.exit(1)

print('check-forks: no forks')
PYTHON
