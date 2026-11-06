#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""check-profile-orphans.py — namespaces the PGO profile knows and the tree no longer has.

A PGO profile is keyed on mangled names, and a mangled name contains the
namespace path. Moving a function into a namespace renames the key, so the
counters stop being found — and under `-fprofile-use` a function with no entry
does not compile neutrally, it compiles COLD. The move demotes it on every build
until someone retrains.

Nothing else can see this. The move is codegen-neutral by construction, so
identical `__text` and identical symbol counts are both true and both beside the
point; what changed is the profile's ability to find the symbol, and no build,
test, or style gate reads the profile. `pgo-staleness.sh` counts commits since
the last retrain, which answers how old the profile is rather than what it has
stopped matching.

Why NAMESPACE paths and not symbols. Comparing the profile's symbols against the
symbols a build defines answers a different question and answers it wrongly in
two directions: an inlined function has no out-of-line symbol yet keeps a valid
profile entry, and the training corpus runs benches whose symbols are in no Core
archive at all. Both read as orphans. A namespace path is neither inlined nor
corpus-dependent — it exists in the tree or it does not — and a namespace move is
the operation that orphans counters in bulk.

The move the path comparison cannot see, and `--moves`. A path comparison is
blind whenever the old path survives the move: `Ubiquitty::scrollbackAppend`
becoming `Ubiquitty::Screen::scrollbackAppend` orphans every one of its counters
while `Ubiquitty` still exists, so the check stays green through the whole
regression. `--moves` closes that by comparing the profile's qualified names
against the ones a BUILD emits, and reports a function only when its bare name
is found under a different qualifier — so it can name both ends of every move
it reports.

That second condition is what makes the build comparison usable here after it
failed as a general orphan test. A function inlined away, one the corpus alone
defines, and one that was deleted all fail it and produce silence rather than a
finding. `--moves` needs archives and is therefore not in the hook; the path
comparison needs nothing and is.

The signature change neither catches, and `--signatures`. A conversion across a
widely-taken parameter type — `Terminal*` to `Terminal&` — remangles every
function taking one without moving it or renaming it. Its namespace path is
present and its qualified name is present, so both comparisons above stay green
while every counter it has is keyed on a symbol nothing emits. `--signatures`
reports a function only when its qualified name is still emitted and none of the
parameter lists emitted for it match the profile's, which is the same asymmetry
`--moves` relies on and inherits the same silence for inlined, deleted and
corpus-only symbols.

It prints a count and returns 0 whatever it finds. What a reader wants is how
much of the profile has stopped matching; a non-zero number is a reason to run
Scripts/pgo-train.sh and commit the result, which is somebody's afternoon, and a
gate here would block every push in the tree until they had spent it.

A machine whose LLVM cannot read the profile stands the gate down by name. The
pre-commit hook runs it on every commit in every clone, and the profile is in
the format of the LLVM that trained it: an older `llvm-profdata` refuses that
format outright, and a machine with no LLVM has nothing to read it with. Both are
a toolchain this machine lacks rather than a fact about the tree, so each prints
a SKIPPED line and returns 0. Any other failure to read the profile still
fails, because a tool that is present and rejects the file is the file's fault.

What still nothing catches. A function renamed in place. The retrain that
follows any rename is the answer.

The default profile follows CXX's target, with UBQ_PGO_PROFILE overriding it.
Use `--profile PATH` to inspect a particular artifact; relative paths are
resolved against the optional repository root argument.
"""

import argparse
import collections
import os
import re
import subprocess
import sys

import os.path as _p, sys as _s
_s.path.insert(0, _p.dirname(_p.abspath(__file__)))
import build_root

# The default root is the repository, anchored to this script's location rather
# than the caller's cwd. From a foreign cwd a default of "." points the profile
# path at a directory that does not hold it, and the gate prints "no profile"
# and returns 0 — a silent skip of the whole check, the
# two-verdicts-by-cwd class measured in check-sync-struct-defaults. An explicit
# path argument still overrides.
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# A conversion across a widely-taken parameter type remangles hundreds of
# functions at once, and a reader who needs the number does not need the list.
# The count line below carries the total, so the sample is there to recognise
# which conversion happened rather than to enumerate what it touched.
SIGNATURE_SAMPLE = 10
# Where a build that produced the Core archives is likely to be, most-built
# first. `build/core` is read by run-tests.sh's snapshots stanza
# (PORTABLE_SNAPSHOT_BIN) but no script configures it, so defaulting to it alone
# tells a normal worktree to run a command that fails with "not a directory" — a
# dead end dressed as an instruction.
# Relative to the build root, which lives outside the checkout.
ARCHIVE_CANDIDATES = ("BuildHostTests/ctest", "core", "tests-iter")
# The TU list the Core archives are built from. Staleness measured against all
# of Source/ makes a touched Platform/macOS file permanently newer than an
# archive that never contained it, which is a refusal no rebuild can clear.
CORE_SOURCES_SSOT = "Scripts/portable-core-sources.txt"

FUNCTION_HEADER = re.compile(r'^  (\S+):$')
BLOCK_COUNTS = re.compile(r'^\s*Block counts:\s*\[([^\]]*)\]')
# A file-local symbol is keyed as `<source path>;<mangled name>`, because two
# translation units may each define a static of the same name.
LOCAL_PREFIX = re.compile(r'^[^;]*;')
# A symbol an archive defines: T/t for text, W/w for a weak definition. An
# archive member carries no link address, so the first field is dashes.
DEFINED = re.compile(r'^[-0-9a-fA-F]*\s*[TtWw]\s+(\S+)$')

# `namespace X {` or `namespace A::B {` at column zero.
NAMESPACE_OPEN_NESTED = re.compile(r'^namespace\s+([A-Za-z_][\w:]*)\s*\{')
# Braces inside a comment or a string literal are not structure; counting them
# drifts the depth and the drift never corrects itself.
# `\\.` matches ONE backslash and the character it escapes. Doubling it required
# two backslashes to begin an escape, so `\"` inside a string ended the match at
# the backslash and the rest of the literal survived into the stripped line. A
# brace surviving that way moves the depth tracked below, and every type after it
# is recorded under a namespace path nobody declares -- the failure that depth
# tracking exists to avoid. Measured against the spelling
# check-name-collisions.py uses: 9440 lines strip differently across the tree,
# and 38 files carry one whose brace delta the two disagree about.
STRINGS_AND_COMMENTS = re.compile(r'//.*|/\*.*?\*/|"(?:[^"\\]|\\.)*"|\'(?:[^\'\\]|\\.)*\'')
# A class or struct qualifies its members exactly as a namespace does, and an
# aggregate opened inside another is qualified by both, which is where a
# mangled name puts a nested type (`LineStore::BlockDirectory::pushBack`).
# An attribute macro may sit between the keyword and the name
# (`class UBIQUITTY_NONCOPYABLE GlyphSlotCache {`), so the name is the LAST
# identifier before the brace rather than the first after the keyword.
AGGREGATE = re.compile(r'^\s*(?:class|struct)\s+(?:[A-Z_][A-Z0-9_]*\s+)*([A-Za-z_]\w*)\s*(?:final\s*)?[:{]')

SOURCE_SUFFIXES = (".h", ".hpp", ".cpp", ".mm", ".m")
IDENTIFIER = re.compile(r"\b[A-Za-z_]\w*\b")

# Symbols the profile counts whose name the tree no longer spells anywhere. The
# budget is two-sided so it can only fall: one under it fails as loudly as one
# over, because a budget with slack cannot report the next arrival.
#
# The one standing entry is `Ubiquitty::Kitty::appendBytes(char**, int*, int*,
# ...)`, which stopped existing when the three Kitty wire buffers became
# ByteBuffer and its pointer triple went with them. Its counts are keyed to a name
# nothing produces; the next pgo-train.sh run clears it and this drops to zero.
UNSPELLED_BUDGET = 0
UNSPELLED_REPORTED = 10


# What `llvm-profdata` says when the profile was written by a newer LLVM than
# itself. It names the reader's age rather than a defect in the file, which is
# what separates it from every other way a read can fail.
UNSUPPORTED_FORMAT = "unsupported instrumentation profile format version"


class StandDown(Exception):
    """This machine's LLVM cannot read the profile, so the gate judges nothing."""


def tool(name):
    """The llvm binary, preferring the Homebrew LLVM the profile was trained with."""
    homebrew = "/opt/homebrew/opt/llvm/bin/" + name
    return homebrew if os.path.exists(homebrew) else name


def run_tool(name, arguments, text_input=None):
    """Runs one LLVM tool and captures what it prints.

    - Parameters:
      - name: The tool, resolved through `tool`.
      - arguments: Its arguments.
      - text_input: What to write to its standard input, or None.
    - Returns: The completed process.
    - Throws: `StandDown` when the tool is not installed.
    """
    try:
        return subprocess.run([tool(name)] + arguments, input=text_input,
                              capture_output=True, text=True)
    except FileNotFoundError:
        raise StandDown("no %s, neither Homebrew's nor one on PATH, so the profile "
                        "was not read" % name) from None


def tool_version(name):
    """The first line `<name> --version` prints, or "unknown version"."""
    try:
        result = run_tool(name, ["--version"])
    except StandDown:
        return "unknown version"
    lines = [line.strip() for line in result.stdout.split("\n") if line.strip()]
    return lines[0] if lines else "unknown version"


def selected_profile(root, explicit=None):
    """Resolves an explicit artifact or the shared compiler-target default."""
    profile = explicit
    if profile is None:
        profile = subprocess.check_output(
            ["bash", "-c", '. "$1"; ubiquitty_pgo_profile "$2" "$3"', "pgo-profile",
             os.path.join(root, "Scripts", "pgo-flags.sh"), root, os.environ.get("CXX") or "c++"],
            text=True).strip()
    return os.path.join(root, profile)


def profiled_symbols(profile):
    """Yields (mangled name, peak block count) for every function in the profile."""
    result = run_tool("llvm-profdata", ["show", "--all-functions", "--counts", profile])
    if result.returncode != 0:
        if UNSUPPORTED_FORMAT in result.stderr:
            raise StandDown("llvm-profdata (%s) is older than the format of %s, so the "
                            "profile was not read"
                            % (tool_version("llvm-profdata"), profile))
        print("check-profile-orphans: llvm-profdata could not read %s" % profile, file=sys.stderr)
        print("  %s" % (result.stderr.strip().split("\n") or [""])[0], file=sys.stderr)
        sys.exit(2)

    name, peak = None, 0
    for line in result.stdout.split("\n"):
        header = FUNCTION_HEADER.match(line)
        if header:
            if name:
                yield name, peak
            name, peak = LOCAL_PREFIX.sub("", header.group(1)), 0
            continue
        counts = BLOCK_COUNTS.match(line)
        if counts and name:
            for value in counts.group(1).split(","):
                value = value.strip()
                if value.isdigit():
                    peak = max(peak, int(value))
    if name:
        yield name, peak


def walk_all(roots):
    """os.walk over several roots, skipping any that is absent."""
    for path in roots:
        if os.path.isdir(path):
            for entry in os.walk(path):
                yield entry


# Both scans below walk the same roots with the same skip list, read the same
# files, and strip strings and comments from every line. Doing it once per scan
# reads the tree twice and runs the strip twice for one answer each. The strip
# is what costs; the cache holds its result.
_SOURCE_CACHE = {}


def source_files(roots):
    """Each source file under the roots, with its lines and their stripped forms.

    - Parameter roots: the directories to walk, in order.
    - Returns: a list of `(path, text, lines, stripped)`, where `stripped` holds
      each line with its strings and comments removed.
    """
    key = tuple(roots)
    cached = _SOURCE_CACHE.get(key)
    if cached is not None:
        return cached
    collected = []
    for directory, subdirectories, names in walk_all(roots):
        subdirectories[:] = [d for d in subdirectories if d not in ("ThirdParty", "compat", "build")]
        for name in sorted(names):
            if not name.endswith(SOURCE_SUFFIXES):
                continue
            path = os.path.join(directory, name)
            with open(path, "r", encoding="utf-8", errors="replace") as handle:
                text = handle.read()
            lines = text.split("\n")
            collected.append((path, text, lines,
                              [STRINGS_AND_COMMENTS.sub("", line) for line in lines]))
    _SOURCE_CACHE[key] = collected
    return collected


def namespace_paths(root):
    """Every namespace and class path the tree declares, as `A::B::C` strings.

    Classes belong here as well as namespaces: the qualifier of
    `Ubiquitty::CellGrid::markDirtyRange` is a class, and a set holding only
    namespaces reports every member function in the codebase as orphaned.
    """
    paths = set()
    # Tests/ counts as the tree. The PGO corpus is the bench harness, so the
    # namespaces it declares are in the profile by design; a scan of Source/
    # alone reports them orphaned on every run forever, and a check that cannot
    # reach zero is one nobody can act on.
    roots = [os.path.join(root, "Source"), os.path.join(root, "Tests")]
    for _path, _text, lines, stripped in source_files(roots):
            # Namespace nesting is tracked by brace DEPTH rather than by the
            # shape of the closing line. Both closer styles appear in this tree
            # and some files mix them, so a pattern that recognises one leaves
            # the stack too deep for the rest of the file and every type after
            # that point is recorded under a path nobody declares.
            depth, stack = 0, []
            for line, bare in zip(lines, stripped):
                opened = NAMESPACE_OPEN_NESTED.match(line)
                if opened:
                    stack.append((depth, opened.group(1).split("::")))
                    paths.add("::".join(n for _, group in stack for n in group))
                    depth += bare.count("{") - bare.count("}")
                    continue
                aggregate = AGGREGATE.match(line)
                if aggregate:
                    prefix = [n for _, group in stack for n in group]
                    paths.add("::".join(prefix + [aggregate.group(1)]))
                    # The aggregate scopes what it nests until its brace closes,
                    # exactly as a namespace does. One whose brace opens on a
                    # later line leaves the stack at once and scopes nothing.
                    stack.append((depth, [aggregate.group(1)]))
                depth += bare.count("{") - bare.count("}")
                while stack and depth <= stack[-1][0]:
                    stack.pop()
    return paths


def symbol_parts(readable):
    """The qualifier and bare name of a demangled symbol, as a pair.

    Returns ("", "") when the symbol has no name the source spells — a lambda,
    an `operator`, a destructor. Those are compiler-formed, so asking whether
    the tree spells them answers about the mangling, not about the code.

    - Parameters:
      - readable: One demangled symbol.
    - Returns: `(scope, leaf)`, where `scope` is the qualified path enclosing
      the name and `leaf` is the name itself.
    """
    leaf = symbol_leaf(readable)
    if not leaf:
        return "", ""
    text = strip_signature(readable)
    parts = text.split("::")
    return "::".join(parts[:-1]), leaf


def nestedSourceNamePrefix(mangled, declared):
    """Recovers an ordinary function template's explicit scope and source name.

    Only length-prefixed identifiers before the first template argument are
    read. Substitutions, operators and templated owners remain unknown; no
    signature is recovered. Anonymous namespaces do not add a named source
    scope, matching the source declaration scan.

    - Parameters:
      - mangled: An Itanium nested name the full demangler could not read.
      - declared: The namespace and class paths declared by the source tree.
    - Returns: A `(scope, leaf)` pair, or None for an unsupported prefix.
    """
    if not mangled.startswith("_ZN"):
        return None
    offset, parts = 3, []
    while offset < len(mangled) and "0" <= mangled[offset] <= "9":
        start = offset
        while offset < len(mangled) and "0" <= mangled[offset] <= "9":
            offset += 1
        digits = mangled[start:offset]
        # Bound integer conversion by the input length, including malformed
        # lengths with thousands of digits.
        if digits[0] == "0" or len(digits) > len(str(len(mangled))):
            return None
        end = offset + int(digits)
        if end > len(mangled):
            return None
        identifier = mangled[offset:end]
        if not re.fullmatch(r"[A-Za-z_][A-Za-z_0-9]*", identifier):
            return None
        if identifier != "_GLOBAL__N_1":
            parts.append(identifier)
        offset = end
    if len(parts) < 2 or mangled[offset:offset + 1] != "I" or offset + 2 >= len(mangled):
        return None
    # Source functions use lowerCamelCase; an uppercase leaf can be the
    # templated owner whose member name follows its arguments. Declared
    # owners are also rejected regardless of their spelling.
    if not ("a" <= parts[-1][0] <= "z") or "::".join(parts) in declared:
        return None
    return "::".join(parts[:-1]), parts[-1]


def strip_signature(readable):
    """A demangled symbol with its return type, template arguments and
    parameter list removed, leaving the qualified name alone.

    Each of the three carries `::` and `(` of its own, so none of them can be
    cut by searching for those characters directly.

    - Parameters:
      - readable: One demangled symbol.
    - Returns: The qualified name, with no signature around it.
    """
    # libstdc++ ABI tags describe linkage, not identifiers spelled in source.
    # Retain the function name so a tagged function's deletion is still judged.
    text = re.sub(r"\[abi:[^\]]+\]", "", readable.strip())
    # The parameter list opens at the first `(` outside any template argument.
    depth, cut = 0, len(text)
    for index, character in enumerate(text):
        if character == "<":
            depth += 1
        elif character == ">":
            depth -= 1
        elif character == "(" and depth == 0:
            cut = index
            break
    text = text[:cut]
    kept, depth = [], 0
    for character in text:
        if character == "<":
            depth += 1
        elif character == ">":
            depth -= 1
        elif depth == 0:
            kept.append(character)
    text = "".join(kept).strip()
    # A return type is present only on templates, and ends at the last space
    # before the qualified name.
    if " " in text:
        text = text.rsplit(" ", 1)[-1]
    return text


def symbol_leaf(readable):
    """The bare name of a demangled symbol, or "" when it has none to check.

    A demangled name carries a return type, template arguments and a parameter
    list, and all three contain `::` and `(` of their own — so the leaf is not
    the text after the last `::`. Taking it that way reports 30 of 273 symbols
    absent from a tree that declares every one of them.

    - Parameters:
      - readable: One demangled symbol.
    - Returns: The leaf identifier, or "" for a lambda, `operator`, destructor
      or other entity whose name is not an identifier the source spells.
    """
    leaf = strip_signature(readable).split("::")[-1].strip()
    if not leaf or leaf[0].isdigit() or leaf.startswith("~") or leaf.startswith("operator"):
        return ""
    if "{" in leaf or "(" in leaf:
        return ""
    return leaf


def scope_identifiers(root):
    """The identifiers spelled in the files that declare each scope.

    Keyed by the same `A::B::C` paths `namespace_paths` builds, so a symbol is
    asked about where it lives rather than anywhere in the tree. A tree-wide set
    answers "is this word written down", which any unrelated namesake satisfies:
    `resize`, `resolve`, `reset` and `at` are each declared under several
    qualifiers here, and on this profile only 37 of 239 judged symbols carry a
    leaf rare enough that deleting the function removes the word. Scoped, all
    239 are judged against the files that would have to change.

    Files rather than declarations, because a declaration needs a signature
    parser and a file needs a scope stack. A member and its out-of-line
    definition are usually two files, both of which declare the scope, so the
    union covers both without either being parsed.

    - Parameters:
      - root: The repository root.
    - Returns: A dict from scope path to the set of identifiers its files spell.
    """
    files = collections.defaultdict(set)
    words = {}
    roots = [os.path.join(root, "Source"), os.path.join(root, "Tests")]
    for path, text, lines, stripped in source_files(roots):
            words[path] = set(IDENTIFIER.findall(text))
            depth, stack = 0, []
            for line, bare in zip(lines, stripped):
                opened = NAMESPACE_OPEN_NESTED.match(line)
                if opened:
                    stack.append((depth, opened.group(1).split("::")))
                    files["::".join(n for _, group in stack for n in group)].add(path)
                    depth += bare.count("{") - bare.count("}")
                    continue
                aggregate = AGGREGATE.match(line)
                if aggregate:
                    prefix = [n for _, group in stack for n in group]
                    files["::".join(prefix + [aggregate.group(1)])].add(path)
                depth += bare.count("{") - bare.count("}")
                while stack and depth <= stack[-1][0]:
                    stack.pop()
    return {scope: set().union(*(words[p] for p in paths)) for scope, paths in files.items()}


def newest_object(archiveRoot):
    """The newest object mtime under a build directory, or 0 when it holds none.

    Zero rather than an error: a directory holding archives and no objects is a
    prebuilt or copied tree, and the archives' own mtimes still date it.

    - Parameter archiveRoot: The build directory the archives were linked in.
    - Returns: The newest `.o` mtime, or 0.0.
    """
    newest = 0.0
    for directory, _subdirectories, files in os.walk(archiveRoot):
        for name in files:
            if name.endswith(".o"):
                newest = max(newest, os.path.getmtime(os.path.join(directory, name)))
    return newest


def newest_core_source(root):
    """The most recent mtime among the sources the Core archives are built from.

    Scoped to the directories the SSOT names rather than to Source/ as a whole,
    because the archives hold the portable Core only. Measured against
    everything, a touched Platform file is permanently newer than an archive
    that never contained it.
    """
    ssot = os.path.join(root, CORE_SOURCES_SSOT)
    directories = set()
    if os.path.exists(ssot):
        with open(ssot, "r", encoding="utf-8", errors="replace") as handle:
            for line in handle:
                line = line.strip()
                if line.startswith("Source/"):
                    directories.add(os.path.join(root, os.path.dirname(line)))
    if not directories:
        directories = {os.path.join(root, "Source", "Core")}

    newest = 0.0
    for directory in directories:
        if not os.path.isdir(directory):
            continue
        for name in os.listdir(directory):
            if name.endswith(SOURCE_SUFFIXES):
                path = os.path.join(directory, name)
                if os.path.isfile(path):
                    newest = max(newest, os.path.getmtime(path))
    return newest


def resolve_archives(root, requested):
    """The archive directory to read: the one asked for, else the first built."""
    candidates = [requested] if requested else list(ARCHIVE_CANDIDATES)
    for relative in candidates:
        directory = relative if os.path.isabs(relative) else os.path.join(root, relative)
        if not os.path.isdir(directory):
            continue
        archives = sorted(os.path.join(directory, name) for name in os.listdir(directory)
                          if name.endswith(".a"))
        if archives:
            return directory, archives
    return None, []


def parameters_of(readable):
    """The parameter list of a demangled symbol, or None when it has none.

    The opening parenthesis is the first one outside template and array
    brackets — the same cut ``qualified_name_of`` makes. From there the close is
    found by counting parentheses alone, so a parameter that is itself a
    function pointer (`void (*)(int)`) closes at its own paren rather than at the
    list's.

    - Parameters:
      - readable: One demangled symbol.
    - Returns: The text between the outermost parentheses, or None.
    """
    depth, opened = 0, -1
    for index, character in enumerate(readable):
        if character in "<[":
            depth += 1
        elif character in ">]":
            depth -= 1
        elif character == "(" and depth == 0:
            opened = index
            break
    if opened < 0:
        return None
    depth = 0
    for index in range(opened, len(readable)):
        if readable[index] == "(":
            depth += 1
        elif readable[index] == ")":
            depth -= 1
            if depth == 0:
                return readable[opened + 1:index].strip()
    return None


def archive_names(archives):
    """(qualified names the archives define, bare name -> qualified, qualified -> parameter lists).

    Read from a build rather than from source because a qualified name is what
    the mangler produced, and reconstructing that from declaration text is the
    part that guesses. The third map holds every parameter list seen for a
    qualified name, because overloads share one.
    """
    result = run_tool("llvm-nm", ["--defined-only"] + archives)
    if result.returncode != 0:
        print("check-profile-orphans: llvm-nm could not read the archives", file=sys.stderr)
        sys.exit(2)
    mangled = set()
    for line in result.stdout.split("\n"):
        match = DEFINED.match(line.strip())
        if match:
            name = match.group(1)
            mangled.add(name[1:] if name.startswith("__Z") else name)

    itanium = sorted(name for name in mangled if name.startswith("_Z"))
    if not itanium:
        return set(), {}, {}
    readable = run_tool("llvm-cxxfilt", [], "\n".join(itanium)).stdout.strip().split("\n")

    qualified, byBareName, byQualifiedName = set(), {}, {}
    for text in readable:
        name = qualified_name_of(text)
        if not name or "::" not in name:
            continue
        qualified.add(name)
        byBareName.setdefault(name.rsplit("::", 1)[1], set()).add(name)
        parameters = parameters_of(text)
        if parameters is not None:
            byQualifiedName.setdefault(name, set()).add(parameters)
    return qualified, byBareName, byQualifiedName


def qualified_name_of(readable):
    """A demangled name with its argument list and return type removed."""
    depth, cut = 0, len(readable)
    for index, character in enumerate(readable):
        if character in "<[":
            depth += 1
        elif character in ">]":
            depth -= 1
        elif character == "(" and depth == 0:
            cut = index
            break
    qualified = readable[:cut]

    depth, last_space = 0, -1
    for index, character in enumerate(qualified):
        if character in "<([":
            depth += 1
        elif character in ">)]":
            depth -= 1
        elif character == " " and depth == 0:
            last_space = index
    return qualified[last_space + 1:].strip()


def qualifier_of(readable):
    """The namespace path of a demangled name, or None if it carries none.

    A demangled name may lead with a return type (`int Ubiquitty::f()`) and may
    carry template arguments holding their own `::`. The qualifier is taken from
    the text before the argument list, after the last space outside brackets, so
    neither reads as part of the path.
    """
    # Only angle and square brackets nest here. Counting the parenthesis as a
    # bracket raises the depth on the very character being looked for, so the
    # cut never happens and the whole argument list stays in the path.
    depth, cut = 0, len(readable)
    for index, character in enumerate(readable):
        if character in "<[":
            depth += 1
        elif character in ">]":
            depth -= 1
        elif character == "(" and depth == 0:
            cut = index
            break
    qualified = readable[:cut]

    depth, last_space = 0, -1
    for index, character in enumerate(qualified):
        if character in "<([":
            depth += 1
        elif character in ">)]":
            depth -= 1
        elif character == " " and depth == 0:
            last_space = index
    qualified = qualified[last_space + 1:]

    # Drop the template argument list before splitting off the function name. A
    # template argument can itself be a qualified name — `measure<A::B::f()::$_0>`
    # is one the bench harness produces — and `rsplit` would then cut inside it,
    # yielding a path the tree cannot declare and reporting a symbol the training
    # run had just emitted as orphaned.
    depth, cut = 0, len(qualified)
    for index, character in enumerate(qualified):
        if character == "<":
            if depth == 0:
                cut = index
                break
            depth += 1
        elif character == ">":
            depth -= 1
    qualified = qualified[:cut]

    if "::" not in qualified:
        return None
    return qualified.rsplit("::", 1)[0]


def profile_against_archives(root, profile, archiveRoot):
    """The profile's counted symbols beside what a build emits, or an int on refusal.

    Shared by ``--moves`` and ``--signatures`` because both compare the same two
    populations and both are wrong in the same way against a stale build. A
    second spelling of the staleness rule is a second thing to keep true.

    - Parameters:
      - root: The repository root.
      - profile: Path to the profdata.
      - archiveRoot: A caller-named archive directory, or None to search.
    - Returns: `(counted, demangled, qualified, byBareName, byQualifiedName)`, or
      an exit code when the archives cannot answer.
    """
    archiveRoot, archives = resolve_archives(root, archiveRoot)
    if not archives:
        print("check-profile-orphans: no Core archives found. Looked in %s"
              % ", ".join(ARCHIVE_CANDIDATES), file=sys.stderr)
        print("  Build one: ./Scripts/build-tests.sh   (writes BuildHostTests/ctest)",
              file=sys.stderr)
        print("  or name one: --archives <dir>", file=sys.stderr)
        print("  Reporting no moves without them would mean only that nothing was read.",
              file=sys.stderr)
        return 2

    # An archive older than the sources answers for a tree nobody has, and the
    # symbol a move just renamed is exactly the one it still emits under the old
    # qualifier — so a stale build reports the move as not having happened.
    # The NEWEST link, not the oldest. An archive whose inputs did not change is
    # not relinked, so it keeps an old mtime while being perfectly current — and
    # taking the minimum lets that one archive report the whole build as stale
    # on every run, which is a refusal nobody can clear by rebuilding.
    # The OBJECTS, not only the archives. An archive is not relinked when
    # recompiling produces byte-identical objects, so a pass that rewrites a
    # source without changing it — clang-format over an already-formatted tree —
    # moves every source mtime past an archive that is perfectly current.
    # Refusing there is a refusal no rebuild clears, because the rebuild ninja
    # correctly declines to do is the one being asked for. An object is
    # per-source and is rewritten whenever its source is newer, so the newest
    # object dates the compile; ninja relinks whenever an object's content
    # actually changes, so current objects with an older archive mean the
    # archive already holds them.
    built = max([os.path.getmtime(path) for path in archives] + [newest_object(archiveRoot)])
    newest = newest_core_source(root)
    if newest > built:
        print("check-profile-orphans: %s predates the portable Core sources."
              % os.path.relpath(archiveRoot, root), file=sys.stderr)
        print("  It answers for a tree that no longer exists, and the symbol a move just",
              file=sys.stderr)
        print("  renamed is the one it still emits under the old qualifier.", file=sys.stderr)
        print("  Rebuild: cmake --build %s" % os.path.relpath(archiveRoot, root), file=sys.stderr)
        return 2

    counted = [(name, peak) for name, peak in profiled_symbols(profile)
               if peak > 0 and name.startswith("_ZN9Ubiquitty")]
    demangled = run_tool("llvm-cxxfilt", [],
                         "\n".join(name for name, _ in counted)).stdout.strip().split("\n")

    qualified, byBareName, byQualifiedName = archive_names(archives)
    if not qualified:
        print("check-profile-orphans: the archives define no qualified names.", file=sys.stderr)
        return 2

    unreadable = [n for (n, _), text in zip(counted, demangled) if text == n]
    if unreadable:
        print("check-profile-orphans: %d profiled symbol(s) did not demangle."
              % len(unreadable), file=sys.stderr)
        print("  llvm-cxxfilt echoes what it cannot read, so these would be dropped", file=sys.stderr)
        print("  silently and the list would look shorter and cleaner.", file=sys.stderr)
        return 2

    return counted, demangled, qualified, byBareName, byQualifiedName


def report_moves(root, profile, archiveRoot):
    """Profiled functions the build now emits under a different qualifier.

    The namespace check above compares PATHS, so it is blind to a move that
    leaves the old path standing: `Ubiquitty::scrollbackAppend` becoming
    `Ubiquitty::Screen::scrollbackAppend` orphans every one of its counters
    while `Ubiquitty` itself still exists, and the check stays green through
    the whole regression.

    Reported only when the bare name is found under a DIFFERENT qualifier. That
    asymmetry is what makes this usable: a function that was inlined away, one
    the corpus alone defines, and one that was deleted all fail the second
    condition and produce silence rather than a finding. Anything this misses
    stays a retrain's job; anything it reports, it can name both ends of.
    """
    read = profile_against_archives(root, profile, archiveRoot)
    if isinstance(read, int):
        return read
    counted, demangled, qualified, byBareName, _ = read

    moved = []
    for (_, peak), text in zip(counted, demangled):
        name = qualified_name_of(text)
        if not name or "::" not in name or name in qualified:
            continue
        bare = name.rsplit("::", 1)[1]
        elsewhere = sorted(byBareName.get(bare, set()) - {name})
        if elsewhere:
            moved.append((peak, name, elsewhere[0]))
    moved.sort(reverse=True)

    if not moved:
        print("check-profile-orphans: ok (no profiled function is emitted under a new qualifier)")
        return 0

    print("Profiled functions the build now emits under a different qualifier:", file=sys.stderr)
    for peak, was, now in moved:
        print("  %13s  %s" % ("{:,}".format(peak), was), file=sys.stderr)
        print("  %13s  -> %s" % ("", now), file=sys.stderr)
    print("", file=sys.stderr)
    print("check-profile-orphans: %d moved function(s), %s counters."
          % (len(moved), "{:,}".format(sum(peak for peak, _, _ in moved))), file=sys.stderr)
    print("  The profile keys on the old name, so each compiles COLD until a retrain.",
          file=sys.stderr)
    return 1


def report_signatures(root, profile, archiveRoot):
    """How many profiled functions the build now emits with a different parameter list.

    The third blind spot this file's header names, after the path comparison and
    the move comparison: a signature change remangles a function without moving
    it or renaming it, so its qualified name is present, its namespace path is
    present, and every counter it has is keyed on a symbol nothing emits. A
    `Terminal*` to `Terminal&` conversion does it to every function taking one.

    Reported only when the qualified name is still emitted and no parameter
    list emitted for it matches the profile's. That is the same asymmetry
    ``report_moves`` relies on, for the same reason: a function inlined away, one
    the corpus alone defines, and one that was deleted all fail the first
    condition and produce silence rather than a finding.

    A count rather than a gate, and it returns 0 whatever it finds. The number a
    reader wants is how much of the profile has stopped matching, and a non-zero
    one is a reason to run Scripts/pgo-train.sh and commit the result — somebody's
    afternoon, which a gate here would block every push in the tree on.

    - Parameters:
      - root: The repository root.
      - profile: Path to the profdata.
      - archiveRoot: A caller-named archive directory, or None to search.
    - Returns: 0, or 2 when the archives cannot answer.
    """
    read = profile_against_archives(root, profile, archiveRoot)
    if isinstance(read, int):
        return read
    counted, demangled, qualified, _, byQualifiedName = read

    remangled, countersLost, comparable, unemitted = [], 0, 0, 0
    for (_, peak), text in zip(counted, demangled):
        name = qualified_name_of(text)
        # `operator()` puts its own parentheses where the parameter list opens,
        # so the qualified name stops at `operator` and the list read back is the
        # operator's own. Comparing those compares two different things.
        if not name or "::" not in name or name.endswith("operator"):
            continue
        parameters = parameters_of(text)
        if parameters is None:
            continue
        emitted = byQualifiedName.get(name)
        if name not in qualified or not emitted:
            # Moved, deleted, or inlined with no out-of-line symbol left. Each is
            # a real reason a profiled name is absent and none of them is a
            # signature change, so they are counted and not judged.
            unemitted += 1
            continue
        comparable += 1
        if parameters in emitted:
            continue
        remangled.append((peak, name, parameters, sorted(emitted)[0]))
        countersLost += peak
    remangled.sort(reverse=True)

    # The denominator, always, and on the passing line too: a bare count of
    # changed symbols reads as a count over the whole profile, and the two differ
    # by however many the build no longer emits at all.
    population = ("%d of %d profiled symbol(s) whose qualified name the build still emits"
                  " (%d it does not, which --moves judges)" % (len(remangled), comparable, unemitted))
    if not remangled:
        print("check-profile-orphans: ok (%s changed shape)" % population)
        return 0

    print("Profiled functions the build now emits with a different parameter list:")
    for peak, name, was, now in remangled[:SIGNATURE_SAMPLE]:
        print("  %13s  %s(%s)" % ("{:,}".format(peak), name, was))
        print("  %13s  -> (%s)" % ("", now))
    if len(remangled) > SIGNATURE_SAMPLE:
        print("  ... and %d more" % (len(remangled) - SIGNATURE_SAMPLE))
    print("")
    print("check-profile-orphans: %s changed shape since this profile was trained, %s counters."
          % (population, "{:,}".format(countersLost)))
    print("  Each compiles COLD until a retrain: Scripts/pgo-train.sh")
    return 0


def selfTest():
    """Checks that the strip and the depth it feeds still catch their own cases.

    The scan tracks namespace nesting by counting braces on the stripped line, so
    everything here turns on the strip removing a literal whole. A spelling that
    ends the match early leaves a brace behind, the depth drifts, and the drift
    never corrects itself -- the gate then reports paths nobody declares, which
    reads as a finding rather than as a broken instrument.

    - Returns: 0 when every case holds, 1 otherwise.
    """
    cases = [
        # (source line, what the strip must leave behind)
        ('int x = 1; // a comment', 'int x = 1; '),
        ('const char* s = "hello";', 'const char* s = ;'),
        # The case the doubled spelling got wrong: one backslash escapes the
        # quote, so the literal continues past it and the brace inside is not
        # structure.
        (r'const char* s = "a \" brace { here";', 'const char* s = ;'),
        (r'const char* s = "{";', 'const char* s = ;'),
        (r"char c = '{';", 'char c = ;'),
        ('/* block { */ int y;', ' int y;'),
        ('namespace A {', 'namespace A {'),
    ]
    failures = []
    for line, expected in cases:
        actual = STRINGS_AND_COMMENTS.sub("", line)
        if actual != expected:
            failures.append(f"{line!r} stripped to {actual!r}, wanted {expected!r}")

    # The brace delta is what the depth actually consumes, so it is asserted
    # rather than inferred from the text above.
    braced = r'const char* s = "{{{";'
    delta = STRINGS_AND_COMMENTS.sub("", braced).count("{") - STRINGS_AND_COMMENTS.sub("", braced).count("}")
    if delta != 0:
        failures.append(f"braces inside a literal moved the depth by {delta:+d}")

    symbols = [
        ("Ubiquitty::Selection::selectedText(Ubiquitty::Selection::Session&, Ubiquitty::Terminal&)",
         ("Ubiquitty::Selection", "selectedText")),
        ("Ubiquitty::Selection::selectedText[abi:cxx11](Ubiquitty::Selection::Session&, Ubiquitty::Terminal&)",
         ("Ubiquitty::Selection", "selectedText")),
        ("Ubiquitty::Selection::removedText[abi:cxx11](Ubiquitty::Selection::Session&, Ubiquitty::Terminal&)",
         ("Ubiquitty::Selection", "removedText")),
    ]
    for readable, expected in symbols:
        actual = symbol_parts(readable)
        if actual != expected:
            failures.append(f"{readable!r} has source name {actual!r}, wanted {expected!r}")

    nestedLambda = (
        "_ZN9Ubiquitty9Rendering12_GLOBAL__N_117SizedBlockWindows5admitIRKZZZNS0_"
        "18buildCellInstancesERKNS0_11BuildInputsERKNS0_12GlyphBackendERKNS0_"
        "15InstanceBuffersEENK3$_1clINSt3__117integral_constantINS1_9WalkClassELSH_"
        "2EEENSG_IbLb0EEESJ_EEDaT_T0_T1_ENKUlvE5_clEvEUliiE0_EEPKNS0_10AtlasGlyph"
        "EPNS0_14GlyphSlotCacheEiiNS1_13SizedBlockArmENS1_15SizedWindowRowsEiOSL_"
    )
    recoveredScope = "Ubiquitty::Rendering::SizedBlockWindows"
    declaredPrefixes = {"Ubiquitty", "Ubiquitty::Rendering", recoveredScope,
                        "Ubiquitty::box"}
    prefixCases = [
        (nestedLambda, (recoveredScope, "admit")),
        ("_ZN9Ubiquitty4drawIiEEvT_", ("Ubiquitty", "draw")),
        ("_ZN9Ubiquitty3BoxIiE7removedEv", None),
        ("_ZN9Ubiquitty3boxIiE7removedEv", None),
        ("_ZN9Ubiquitty5admi", None),
        ("_ZN9Ubiquitty0IivE", None),
        ("_ZN9Ubiquitty05admitIivE", None),
        ("_ZN9Ubiquitty5ad-itIivE", None),
        ("_ZN9Ubiquitty4drawI", None),
        ("_ZNK9Ubiquitty4drawIiEEvT_", None),
        ("_ZN9UbiquittyS_4drawIiEEvT_", None),
        ("_ZN9UbiquittyclIiEEvT_", None),
    ]
    for mangled, expected in prefixCases:
        actual = nestedSourceNamePrefix(mangled, declaredPrefixes)
        if actual != expected:
            failures.append(f"{mangled!r} has source prefix {actual!r}, wanted {expected!r}")
    movedScope = nestedSourceNamePrefix(
        nestedLambda.replace("17SizedBlockWindows", "17MovedBlockWindows"), declaredPrefixes)
    if not movedScope or movedScope[0] in declaredPrefixes:
        failures.append("a renamed owner escaped the recovered namespace check")
    movedLeaf = nestedSourceNamePrefix(nestedLambda.replace("5admitI", "7removedI"), declaredPrefixes)
    if not movedLeaf or movedLeaf[1] in {"admit"}:
        failures.append("a renamed function escaped the recovered source-name check")

    # A type nested in a class is qualified by the class, as its mangled name
    # is; recording it at the namespace alone reports every member of it as
    # orphaned the first time a profile counts one.
    import tempfile
    with tempfile.TemporaryDirectory() as scratch:
        source = os.path.join(scratch, "Source")
        os.makedirs(source)
        with open(os.path.join(source, "Nested.h"), "w", encoding="utf-8") as nested:
            nested.write("namespace Ubiquitty {\n"
                         "class LineStore {\n"
                         "public:\n"
                         "    struct BlockDirectory {\n"
                         "        void pushBack();\n"
                         "    };\n"
                         "};\n"
                         "struct Lone : Base {\n"
                         "    int field;\n"
                         "};\n"
                         "} // namespace Ubiquitty\n")
        declared = namespace_paths(scratch)
        for path in ("Ubiquitty", "Ubiquitty::LineStore", "Ubiquitty::LineStore::BlockDirectory", "Ubiquitty::Lone"):
            if path not in declared:
                failures.append(f"{path} is declared by the nested-aggregate source and was not recorded")
        if "Ubiquitty::Lone::BlockDirectory" in declared or "Ubiquitty::BlockDirectory" in declared:
            failures.append("a nested aggregate was recorded outside the class that encloses it")

        # Exercise the verdict, including both failure paths, when the full
        # demangler echoes a compiler-generated name it cannot read.
        import contextlib
        import io
        from unittest.mock import patch
        fixture = os.path.join(source, "Cell.cpp")
        profile = os.path.join(scratch, "fixture.profdata")
        with open(profile, "wb"):
            pass
        declaration = ("namespace Ubiquitty {\nnamespace Rendering {\nnamespace {\n"
                       "class SizedBlockWindows {\npublic:\n"
                       "    template<typename Window> void admit(Window&&);\n"
                       "};\n}\n}\n}\n")
        verdictCases = [
            ("current function", declaration, 0),
            ("removed owner", declaration.replace("SizedBlockWindows", "MovedBlockWindows"), 1),
            ("removed function", declaration.replace("admit", "removed"), 1),
        ]
        for label, contents, expected in verdictCases:
            with open(fixture, "w", encoding="utf-8") as handle:
                handle.write(contents)
            echoed = subprocess.CompletedProcess([], 0, nestedLambda + "\n", "")
            verdictOutput = io.StringIO()
            with patch.dict(globals(), {
                    "_SOURCE_CACHE": {},
                    "profiled_symbols": lambda path: [(nestedLambda, 7)],
                    "run_tool": lambda *arguments: echoed,
            }), contextlib.redirect_stdout(verdictOutput), contextlib.redirect_stderr(verdictOutput):
                actual = check(["check-profile-orphans", scratch, "--profile", profile])
            if actual != expected:
                failures.append(f"{label} returned verdict {actual}, wanted {expected}: {verdictOutput.getvalue().strip()}")

    readers = readerFailures()
    failures.extend(readers[1])
    profiles = profileSelectionFailures()
    failures.extend(profiles[1])

    for failure in failures:
        print("check-profile-orphans --self-test: " + failure, file=sys.stderr)
    if failures:
        return 1
    print(f"check-profile-orphans --self-test: ok ({len(cases)} strip case(s), "
          f"a literal's braces leave the depth alone, a nested aggregate keeps its class, {readers[0]} reader case(s), "
          f"{profiles[0]} profile selection case(s), {len(symbols)} source symbol case(s), "
          f"{len(prefixCases)} source prefix case(s), recovered owner and name changes)")
    return 0


def readerFailures():
    """Checks that a missing or outdated reader stands down and a bad profile does not.

    Three programs stand in for `llvm-profdata`: none at all, one refusing the
    profile's format as newer than itself, and one refusing the file as
    malformed. The first two are this machine's toolchain and must print a
    SKIPPED line and return 0. The third is the profile's fault and must still
    fail, or the stand-down hides the one failure a present tool can report.

    - Returns: `(cases, failures)`, the number of cases run and a description of
      each one that did not hold.
    """
    import contextlib
    import io
    import tempfile

    failures = []
    with tempfile.TemporaryDirectory() as scratch:
        profile = os.path.join(scratch, "Support/pgo/portable-core.profdata")
        os.makedirs(os.path.dirname(profile))
        with open(profile, "wb") as handle:
            handle.write(b"\xfflprofi\x81")

        def reader(name, complaint):
            path = os.path.join(scratch, name)
            with open(path, "w") as handle:
                handle.write('#!/bin/sh\n'
                             'if [ "$1" = --version ]; then echo "LLVM version 1.0.0"; exit 0; fi\n'
                             'echo "error: $4: %s" >&2\n'
                             'exit 1\n' % complaint)
            os.chmod(path, 0o755)
            return path

        # (case, the reader, the status wanted, a line the output must begin with,
        #  what that line must name)
        cases = [
            ("no llvm-profdata", os.path.join(scratch, "absent", "llvm-profdata"),
             0, "check-profile-orphans: SKIPPED", "no llvm-profdata"),
            ("an llvm-profdata older than the profile",
             reader("older", UNSUPPORTED_FORMAT),
             0, "check-profile-orphans: SKIPPED", "LLVM version 1.0.0"),
            ("an llvm-profdata rejecting the file",
             reader("rejecting", "invalid instrumentation profile data (bad magic)"),
             2, "check-profile-orphans: llvm-profdata could not read", "bad magic"),
        ]
        saved = globals()["tool"]
        try:
            for case, path, wantedStatus, opening, named in cases:
                globals()["tool"] = lambda name, path=path: path
                output = io.StringIO()
                with contextlib.redirect_stdout(output), contextlib.redirect_stderr(output):
                    try:
                        status = main([__file__, scratch, "--profile", profile])
                    except SystemExit as stop:
                        status = stop.code
                text = output.getvalue()
                if status != wantedStatus or not text.startswith(opening) or named not in text:
                    failures.append(f"{case}: exited {status} and printed {text.strip()!r}, "
                                    f"wanted {wantedStatus} and a line beginning "
                                    f"{opening!r} naming {named!r}")
        finally:
            globals()["tool"] = saved
    return len(cases), failures


def profileSelectionFailures():
    """Checks compiler-target selection and explicit-profile precedence without a build."""
    import tempfile
    from unittest.mock import patch

    failures = []
    cases = [
        ("Linux x86-64", "__linux__ __x86_64__", None, None,
         "Support/pgo/linux-x86_64-core.profdata"),
        ("Linux ARM", "__linux__ __aarch64__", None, None,
         "Support/pgo/linux-aarch64-core.profdata"),
        ("Apple x86-64", "__APPLE__ __x86_64__", None, None,
         "Support/pgo/portable-core.profdata"),
        ("Apple ARM", "__APPLE__ __aarch64__", None, None,
         "Support/pgo/portable-core.profdata"),
        ("relative environment override", "__linux__ __x86_64__", "custom.profdata", None,
         "custom.profdata"),
        ("absolute environment override", "__linux__ __x86_64__", "/external/profile.profdata", None,
         "/external/profile.profdata"),
        ("explicit profile wins", "__linux__ __x86_64__", "environment.profdata", "explicit.profdata",
         "explicit.profdata"),
        ("absolute explicit profile", "__linux__ __x86_64__", None, "/external/explicit.profdata",
         "/external/explicit.profdata"),
    ]
    with tempfile.TemporaryDirectory() as scratch:
        compiler = os.path.join(scratch, "compiler")
        for case, macros, override, explicit, expected in cases:
            with open(compiler, "w") as handle:
                handle.write("#!/bin/sh\n")
                for macro in macros.split():
                    handle.write("echo '#define %s 1'\n" % macro)
            os.chmod(compiler, 0o755)
            with patch.dict(os.environ, {"CXX": compiler, "UBQ_PGO_PROFILE": override or ""}):
                actual = selected_profile(REPO, explicit)
            wanted = os.path.join(REPO, expected)
            if actual != wanted:
                failures.append(f"{case}: selected {actual!r}, wanted {wanted!r}")
    return len(cases), failures


def main(argv):
    if "--self-test" in argv:
        return selfTest()
    try:
        return check(argv)
    except StandDown as reason:
        print("check-profile-orphans: SKIPPED — %s" % reason)
        return 0


def check(argv):
    """Judges the profile against the tree, in whichever mode `argv` asks for.

    - Parameter argv: The command line, with an optional root after the script.
    - Returns: 0 when the profile matches, 1 on a finding, 2 when nothing could
      be judged for a reason this machine's toolchain does not explain.
    - Throws: `StandDown` when this machine's LLVM cannot read the profile.
    """
    parser = argparse.ArgumentParser(description="Check the selected PGO profile for orphaned symbols.")
    parser.add_argument("root", nargs="?", default=REPO)
    parser.add_argument("--profile", help="profile to inspect, absolute or relative to the repository root")
    parser.add_argument("--moves", action="store_true")
    parser.add_argument("--signatures", action="store_true")
    parser.add_argument("--archives")
    arguments = parser.parse_args(argv[1:])
    root = os.path.abspath(arguments.root)
    profile = selected_profile(root, arguments.profile)
    if not os.path.exists(profile):
        print("check-profile-orphans: no profile at %s" % os.path.relpath(profile, root))
        return 0  # a tree without a profile has nothing to orphan

    if arguments.moves or arguments.signatures:
        if arguments.signatures:
            return report_signatures(root, profile, arguments.archives)
        return report_moves(root, profile, arguments.archives)

    # A bench A/B that swaps arms by writing this tracked path turns the file
    # into whichever arm was running when it was read. Judging that is judging
    # somebody's loop iteration, so an unstaged modification gets no verdict at
    # all — and says so, rather than blocking every other checkout's commit on it.
    #
    # STAGED is the opposite case and must be judged. A staged profile is a
    # retrain being committed, and the retrain commit is the one change this
    # check exists to validate: abstaining there means the check is silent
    # exactly when it is the point. Nobody stages an A/B arm.
    staged = subprocess.run(["git", "diff", "--cached", "--quiet", "--", profile],
                            cwd=root, capture_output=True, text=True)
    modified = subprocess.run(["git", "diff", "--quiet", "--", profile],
                              cwd=root, capture_output=True, text=True)
    if staged.returncode == 0 and modified.returncode != 0:
        print("check-profile-orphans: %s is locally modified, so no verdict."
              % os.path.relpath(profile, root))
        print("  A profile being swapped between benchmark arms answers for the arm")
        print("  that happened to be resident, which is not a fact about the tree.")
        return 0

    counted = [(name, peak) for name, peak in profiled_symbols(profile)
               if peak > 0 and name.startswith("_ZN9Ubiquitty")]
    if not counted:
        print("check-profile-orphans: the profile holds no Ubiquitty symbols with counts -",
              file=sys.stderr)
        print("  the reader matched nothing, which is not the same as nothing being wrong.",
              file=sys.stderr)
        return 2

    demangled = run_tool("llvm-cxxfilt", [], "\n".join(name for name, _ in counted))
    readable = demangled.stdout.strip().split("\n")
    if demangled.returncode != 0 or len(readable) != len(counted) or any(not text for text in readable):
        print("check-profile-orphans: llvm-cxxfilt did not return one name per counted symbol.", file=sys.stderr)
        return 2

    declared = namespace_paths(root)
    if not declared:
        print("check-profile-orphans: no namespaces found under Source/.", file=sys.stderr)
        return 2

    # llvm-cxxfilt ECHOES a name it cannot demangle rather than failing, so an
    # unparseable symbol arrives looking like output. It carries no `::`, gets
    # skipped below, and the run reports a smaller and cleaner number for it.
    # Counted and named instead: a symbol nobody could read is unknown, not
    # absent.
    recovered, echoed = {}, []
    for index, ((mangled, _), text) in enumerate(zip(counted, readable)):
        if text != mangled:
            continue
        prefix = nestedSourceNamePrefix(mangled, declared)
        if prefix is None:
            echoed.append(mangled)
        else:
            recovered[index] = prefix

    mass, example = collections.Counter(), {}
    for index, ((mangled, peak), text) in enumerate(zip(counted, readable)):
        path = recovered[index][0] if index in recovered else qualifier_of(text)
        if not path or not path.startswith("Ubiquitty"):
            continue
        if mass[path] < peak:
            example[path] = "::".join(recovered[index]) if index in recovered else text.split("(")[0]
        mass[path] = max(mass[path], peak)

    if echoed:
        print("check-profile-orphans: %d profiled symbol(s) did not demangle and are"
              % len(echoed), file=sys.stderr)
        print("  judged by nothing. llvm-cxxfilt echoes what it cannot read, so these", file=sys.stderr)
        print("  arrived looking like output:", file=sys.stderr)
        for mangled in echoed[:5]:
            print("    %s" % mangled, file=sys.stderr)
        return 2

    if recovered:
        print("check-profile-orphans: recovered %d source scope/name prefix(es) without full signatures;"
              % len(recovered))
        print("  their counters remain included in the namespace and scoped-name checks.")

    orphans = sorted(((path, peak) for path, peak in mass.items() if path not in declared),
                     key=lambda pair: -pair[1])

    # A path is a class for a member and a bare namespace for a free function,
    # so the comparison above sees a member requalified out of its class and is
    # blind to everything about a free function: its namespace keeps all its
    # other members, so the path survives a rename, a move or a deletion, and
    # free functions are a large share of what the profile counts.
    scoped = scope_identifiers(root)
    if not scoped:
        print("check-profile-orphans: no scopes found under Source/.", file=sys.stderr)
        return 2
    unspelled = []
    for index, (text, (mangled, peak)) in enumerate(zip(readable, counted)):
        scope, leaf = recovered[index] if index in recovered else symbol_parts(text)
        if not leaf:
            continue
        spelled = scoped.get(scope)
        # A scope the tree does not declare is already the orphan the path
        # comparison reports, and asking a second question about it would name
        # the same symbol twice under two headings.
        if spelled is None:
            continue
        if leaf not in spelled:
            label = "::".join((scope, leaf)) + " [signature unreadable] " + mangled if index in recovered else text
            unspelled.append((peak, leaf, label))
    unspelled.sort(key=lambda row: -row[0])

    if unspelled:
        print("Symbols the profile counts and the tree no longer spells:", file=sys.stderr)
        for peak, leaf, text in unspelled[:UNSPELLED_REPORTED]:
            print("  %13s  %s" % ("{:,}".format(peak), text[:96]), file=sys.stderr)
        print("", file=sys.stderr)

    # Both halves are judged before either returns. They fail for unrelated
    # reasons — a requalified class and a name nothing spells — so returning on
    # the first hides the other, and the one hidden is whichever the reader
    # happens not to be looking for.
    failed = 0
    if len(unspelled) > UNSPELLED_BUDGET:
        print("check-profile-orphans: %d symbol(s) counted by the profile and spelled nowhere,"
              % len(unspelled), file=sys.stderr)
        print("  over the budget of %d. Each is a counter keyed to a name that no longer"
              % UNSPELLED_BUDGET, file=sys.stderr)
        print("  exists, so the code that replaced it compiles cold until pgo-train.sh runs.",
              file=sys.stderr)
        failed += 1
    elif len(unspelled) < UNSPELLED_BUDGET:
        print("check-profile-orphans: %d unspelled symbol(s), under the budget of %d."
              % (len(unspelled), UNSPELLED_BUDGET), file=sys.stderr)
        print("  A budget with slack cannot fail. Lower it to the new count.", file=sys.stderr)
        failed += 1

    if not orphans:
        if failed:
            return 1
        print("check-profile-orphans: ok (%d profiled namespace paths all declared, "
              "%d symbol(s) unspelled at budget)" % (len(mass), len(unspelled)))
        return 0

    print("Namespace paths the profile is keyed on and the tree no longer declares:",
          file=sys.stderr)
    for path, peak in orphans:
        print("  %13s  %-34s e.g. %s" % ("{:,}".format(peak), path, example[path]), file=sys.stderr)
    print("", file=sys.stderr)
    print("check-profile-orphans: %d orphaned namespace path(s)." % len(orphans), file=sys.stderr)
    print("  Every symbol under these is keyed on a name the tree no longer produces, so", file=sys.stderr)
    print("  under -fprofile-use each compiles COLD rather than neutrally - demoted on", file=sys.stderr)
    print("  every build until Scripts/pgo-train.sh runs again.", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
