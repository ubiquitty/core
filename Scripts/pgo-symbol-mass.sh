#!/usr/bin/env bash
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

# pgo-symbol-mass.sh — what a rename is about to cost the profile.
#
# The PGO profile is keyed on MANGLED names. A namespace move renames the
# mangled symbol without changing a single instruction, so the counters keyed to
# the old name stop matching anything the tree defines — and under
# -fprofile-use a function with no entry compiles COLD, actively demoted rather
# than left at plain -O3.
#
# That makes a namespace move a performance change even though it is not a
# codegen change, and nothing else in this repository can see it: the diff is
# codegen-neutral, so __text stays byte-identical, the symbol COUNT is
# unchanged, every frontend builds and every test passes. Moving a namespace
# that holds functions on the per-cell render path orphans their counters.
#
# Run this before moving anything. A non-empty answer does not forbid the move;
# it says the move has to be sequenced with a retrain after it lands, never
# before, or the retrain re-orphans on the next commit.
#
#   Scripts/pgo-symbol-mass.sh visibleCell getDisplayRow rowMark
#   Scripts/pgo-symbol-mass.sh --type Pen
#
# `--type` answers the other half, and it is the half that is easy to miss: a
# TYPE's move renames every profiled function that takes it, without touching a
# line of any of their bodies. Moving Ubiquitty::Pen out of the global namespace
# renames Rendering::resolveCell — `RK3Pen` becomes `RKN9Ubiquitty3PenE` — even
# though resolveCell is not moving and its own namespace path is unchanged.
#
# The cost is therefore the CALLERS' mass, not the type's, and the two are
# unrelated: one type can appear in profiled signatures that carry nothing,
# another in a handful that carry a great deal. Nothing about the mechanism
# predicts which, so it has to be measured per type.
#
# check-profile-orphans.py --moves cannot see this class at all: it matches on
# the moved name, and the profile keys on the callers.
#
# This is not a gate, and it cannot become one. The general question
# — "does the profile name a symbol the tree no longer defines" — has no cheap
# exact answer: Itanium mangling spells a class component and a namespace
# component identically, so `_ZN9Ubiquitty8CellGrid5clearEv` cannot be told from
# a namespace path by inspection, and asking a built artefact instead runs into
# the denominator problem (an archive omits inlined, static, dead-stripped and
# frontend-only symbols, so the orphaned share depends on which binary you
# ask). A gate built on either reports false positives and misses the real
# orphans.
#
# This asks a narrower question with an exact answer: does the profile mention
# this identifier. That is a text fact, it has no oracle, and it covers the case
# that actually bites — a rename whose old name you know.

set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# shellcheck source=Scripts/pgo-flags.sh
source "$ROOT/Scripts/pgo-flags.sh"
PROFILE="${UBIQUITTY_PROFILE:-$(ubiquitty_pgo_profile "$ROOT" "${CXX:-c++}")}"

MODE=name
if [ "${1:-}" = "--type" ]; then
    MODE="type"
    shift
fi

if [ "$#" -eq 0 ]; then
    sed -n '/^#!/d; /^[^#]/q; s/^# \{0,1\}//; p' "$0"
    exit 2
fi

if [ ! -f "$PROFILE" ]; then
    echo "pgo-symbol-mass: no profile at $PROFILE — nothing to lose, and nothing to check." >&2
    exit 0
fi

PROFDATA=""
for candidate in /opt/homebrew/opt/llvm/bin/llvm-profdata "$(command -v llvm-profdata 2>/dev/null || true)"; do
    if [ -n "$candidate" ] && [ -x "$candidate" ]; then
        PROFDATA="$candidate"
        break
    fi
done
if [ -z "$PROFDATA" ]; then
    # Skipping loudly and naming the tool, because a silent skip here reads as
    # "nothing to worry about" for precisely the change that needs worrying.
    echo "pgo-symbol-mass: SKIPPED — llvm-profdata not found (brew install llvm)." >&2
    echo "pgo-symbol-mass: the profile cost of this rename is UNMEASURED, not zero." >&2
    exit 0
fi

"$PROFDATA" show --all-functions --counts "$PROFILE" 2>/dev/null > "${TMPDIR:-/tmp}/pgo-symbol-mass.$$"
trap 'rm -f "${TMPDIR:-/tmp}/pgo-symbol-mass.$$"' EXIT

NAMES="$*" MODE="$MODE" python3 - "${TMPDIR:-/tmp}/pgo-symbol-mass.$$" <<'PY'
import os
import re
import shutil
import subprocess
import sys

names = os.environ["NAMES"].split()
mode = os.environ["MODE"]
dump = open(sys.argv[1]).read().splitlines()

# Itanium spells a nested name as a run of `<len><identifier>` components. Walk
# them rather than pattern-matching: a regex needs a lookahead to stop
# `visibleCell` matching `visibleCellAt`, and every lookahead that does so also
# rejects the `E` that terminates a nested name — `6coversEj` — so the pattern
# reports the very symbols it is asked about as absent. Reading the length
# prefix is exact and needs no lookahead at all.
def components(symbol):
    parts, rest = [], symbol
    while rest:
        match = re.search(r'(\d+)', rest)
        if not match:
            break
        length = int(match.group(1))
        start = match.end()
        parts.append(rest[start:start + length])
        rest = rest[start + length:]
    return parts

entries, current = [], None
for line in dump:
    match = re.match(r'^  (\S+):$', line)
    if match:
        current = match.group(1)
        continue
    match = re.match(r'^    Block counts: \[([^\]]*)\]', line)
    if match and current:
        counts = [int(value) for value in match.group(1).split(",") if value.strip().lstrip("-").isdigit()]
        entries.append((current, max(counts) if counts else 0))
        current = None

# Demangling splits a symbol where component-walking cannot: everything before
# the first `(` is the qualified function name, everything after is its
# signature. That is exactly the "is this the function itself, or a function that
# TAKES it" question, and it needs no knowledge of length prefixes,
# substitutions or nesting — the two things that made every hand-rolled probe for
# this wrong in a different way.
#
# `llvm-cxxfilt` ECHOES its input when it cannot demangle rather than failing, so
# a reader that matches on the output silently drops every symbol it could not
# parse and reports a smaller, cleaner-looking number for it. Unchanged output
# means undemangled; those are counted as unknown and reported, never dropped.
def demangle(symbols):
    """(demangled by symbol, C++ symbols the demangler could not parse)."""
    if not symbols:
        return {}, []
    tool = None
    for candidate in ("/opt/homebrew/opt/llvm/bin/llvm-cxxfilt", "llvm-cxxfilt", "c++filt"):
        if os.path.isfile(candidate) or shutil.which(candidate):
            tool = candidate
            break
    if not tool:
        return {}, list(symbols)
    # The profile prefixes a local symbol with its source path; the mangled name
    # is whatever follows the last `;`.
    mangled = [s.rsplit(";", 1)[-1] for s in symbols]
    result = subprocess.run([tool], input="\n".join(mangled), capture_output=True, text=True)
    if result.returncode != 0:
        return {}, list(symbols)
    out = result.stdout.splitlines()
    if len(out) != len(mangled):
        return {}, list(symbols)
    resolved, unknown = {}, []
    for symbol, raw, plain in zip(symbols, mangled, out):
        if plain != raw:
            resolved[symbol] = plain
        elif raw.startswith("_Z"):
            # A C++ symbol the demangler could not parse. A name that never began
            # with `_Z` is C or compiler-generated — wuffs entry points,
            # `__cxx_global_var_init` — and echoing those is correct.
            unknown.append(symbol)
    return resolved, unknown


def mentions(text, name):
    """Whether `name` appears in `text` as a type TOKEN rather than a substring.

    `Ubiquitty::Screen::rectReverseAttributes` carries the letters of
    `Attribute` and is not a use of it — right letters, wrong thing, the same
    class of error as matching a bare mangled component.

    The lookbehind excludes identifier characters and NOT `:`, which is the
    difference between finding `Ubiquitty::Pen` and reporting `Pen` absent from
    a profile that names it five times.
    """
    return re.search(r'(?<![A-Za-z0-9_])%s(?![A-Za-z0-9_])' % re.escape(name), text) is not None


demangled, undemangled = demangle([symbol for symbol, _ in entries])
if undemangled:
    print(f"pgo-symbol-mass: {len(undemangled)} of {len(entries)} symbols did not demangle.")
    print("Their contribution is UNKNOWN, not zero — a demangler that echoes its input")
    print("would otherwise drop them and report a cleaner number for it.")
    for symbol in undemangled[:5]:
        print(f"      {symbol}")
    print()

total = 0
found = False
for name in names:
    hits = [(symbol, mass) for symbol, mass in entries
            if (mentions(demangled[symbol], name) if symbol in demangled
                else name in components(symbol))]
    hits.sort(key=lambda row: -row[1])
    if not hits:
        print(f"  {name}: not in the profile — the move costs nothing")
        continue
    found = True
    subtotal = sum(mass for _, mass in hits)
    total += subtotal
    print(f"  {name}: {len(hits)} entr{'y' if len(hits) == 1 else 'ies'}, {subtotal:,} peak-block mass")
    for symbol, mass in hits:
        plain = demangled.get(symbol, symbol)
        # Before the first `(` the name is the function; after it, the function
        # merely takes it — which is the entire cost of moving a type.
        signature = plain.split("(", 1)
        role = ""
        if symbol in demangled and len(signature) == 2:
            in_name, in_params = mentions(signature[0], name), mentions(signature[1], name)
            role = "  [name]" if in_name and not in_params else (
                "  [signature]" if in_params and not in_name else "  [name+signature]")
        print(f"      {mass:>14,}  {plain}{role}")

if not found:
    print("\npgo-symbol-mass: none of these names is in the profile. Move freely.")
    sys.exit(0)

if not total:
    print("\npgo-symbol-mass: every entry above carries 0 counters — the corpus never runs")
    print("them, so they compile cold already and the move changes nothing. Move freely.")
    sys.exit(0)

subject = "these types' signatures" if mode == "type" else "these names"
print(f"\npgo-symbol-mass: {total:,} peak-block counters are keyed to {subject}.")
print("Renaming them orphans that mass, and an orphaned function compiles COLD")
print("under -fprofile-use. Sequence a retrain AFTER the move lands, not before.")
if mode == "type":
    print("These functions are NOT moving; only the mangling of a parameter changes,")
    print("which is why check-profile-orphans.py --moves stays silent on this class.")
PY
