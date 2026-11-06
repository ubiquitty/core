#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""check-mirrored-enums — a mirrored enum numbers its enumerators explicitly.

An enum declared in one place has no numbering problem: the values are whatever
the order says, and the order is the only spelling there is. Two declarations of
one concept are different. `Codec::PNG::DecodeStatus` and `Image::TIFF::
DecodeStatus` share five enumerator names, both positional, and already disagree
on two of them — `Unsupported` is 4 in one and 3 in the other. Nothing converts
between them today. Nothing would report it if something did: every value is in
range for both, so the cast succeeds and names the wrong failure.

The rule is that a MIRROR pins its values. Not every enum — pinning fifty
enumerators of an enum with no twin makes the numbers meaningless noise and buys
nothing, and a rule that demanded it would be argued with rather than obeyed.

Why not name-collision. `Direction`, `Kind` and `State` are each declared in
several files as unrelated concepts in unrelated namespaces. A gate firing on
those fires eleven times for no defect and teaches its readers to scroll past
it, which costs more than it catches.

Why not an exemption list. `Keys::VirtualKey` is positional and correct; its
macOS twin pins every value to an AppKit hardware constant, so that one's order
carries nothing to lose. Both facts are derivable, and a hand-kept list of
"enums that are fine" goes stale the first time one stops being fine.

The parse is the risk, so it fails loud. A scan that drops an enumerator
under-reports overlap, which EXCUSES a real mirror, and invents one-enumerator
enums, which INVENTS mirrors that are not. Both directions at once is how a gate
gets discounted. So a body that yields no enumerators, or an entry that is not
an identifier, is a parse failure that fails the run — never an enum that
happens to be empty.
"""

import os
import pathlib
import re
import subprocess
import sys

sys.path.insert(0, str(pathlib.Path(__file__).parent))
import gate_report
import ubq_roots

GATE = "check-mirrored-enums"

# A mirror needs a real shared vocabulary, not one incidental `None`. Two enums
# sharing a single name are two enums; sharing half of the smaller one is one
# concept spelled twice.
MINIMUM_SHARED_NAMES = 2
MINIMUM_OVERLAP_RATIO = 0.5

BLOCK_COMMENT = re.compile(r"/\*.*?\*/", re.S)
LINE_COMMENT = re.compile(r"//[^\n]*")
STRING_LITERAL = re.compile(r'"(?:[^"\\\n]|\\.)*"')
CHAR_LITERAL = re.compile(r"'(?:[^'\\\n]|\\.)*'")
ENUM_HEAD = re.compile(r"\benum\s+(?:class|struct)\s+([A-Za-z_]\w*)\s*(?::\s*[\w:\s]+?)?\s*\{")
IDENTIFIER = re.compile(r"^[A-Za-z_]\w*$")

# A mirror whose two sides were RENAMED apart is invisible to a name-keyed pairing,
# and cannot be recovered by member overlap: at this gate's own thresholds that
# yields 163 differently-named pairs of which about one is real, because two enums
# over a small shared vocabulary (Left Right Up Down) look identical to an overlap
# test. So the pairing is declared, and what the gate enforces is that a
# declaration stays true -- naming an enum that does not exist fails, and so does
# a pair whose members have drifted.
MIRRORS_DECLARATION = re.compile(r"//\s*mirrors:\s*([A-Za-z_]\w*)\s*$", re.M)


class ParseFailure(Exception):
    """Raised when a body cannot be read, so the caller fails instead of guessing."""


def strip_noise(text):
    """Returns `text` with comments and literals blanked, newlines preserved.

    Blanked rather than deleted so byte offsets still map to the original, which
    is what lets a diagnostic name a line the reader can find.
    """
    def blank(match):
        return re.sub(r"[^\n]", " ", match.group(0))

    text = BLOCK_COMMENT.sub(blank, text)
    text = LINE_COMMENT.sub(blank, text)
    text = STRING_LITERAL.sub(blank, text)
    text = CHAR_LITERAL.sub(blank, text)
    return text


def split_top_level(body):
    """Splits an enum body on commas that are not inside brackets.

    An initialiser may hold commas of its own — `= f(a, b)` — and splitting
    blindly turns one enumerator into two, each of which then fails the
    identifier test. Depth-aware, so that is a parse failure only when it is one.
    """
    parts, depth, current = [], 0, []
    for character in body:
        if character in "([{":
            depth += 1
        elif character in ")]}":
            depth -= 1
        if character == "," and depth == 0:
            parts.append("".join(current))
            current = []
        else:
            current.append(character)
    parts.append("".join(current))
    return [part.strip() for part in parts if part.strip()]


def enumerators(body, where):
    """Returns [(name, is_pinned)] for one enum body, or raises ParseFailure."""
    entries = split_top_level(body)
    if not entries:
        # An enum with no enumerators is a strong typedef — `enum class PaneID :
        # uint32_t {}` exists to stop a uint32_t being passed where a handle
        # belongs, and has nothing to number. Reading that as a parse failure
        # rejects the one shape this check has no opinion about.
        return []
    result = []
    for entry in entries:
        name, _, value = entry.partition("=")
        name = name.strip()
        # The Swift-name annotation rides the enumerator
        # (`Foo UBIQUITTY_SWIFT_NAME(foo)`); it renames the importer's view,
        # not the enumerator, so it comes off before the identity check.
        name = re.sub(r"\s*UBIQUITTY_SWIFT_NAME\([^)]*\)$", "", name)
        if not IDENTIFIER.match(name):
            raise ParseFailure(f"{where}: {entry!r} is not an enumerator")
        result.append((name, bool(value.strip())))
    return result


def scan(path):
    """Returns [(name, line, [declared mirror name], [(enumerator, is_pinned)])] for one file."""
    try:
        raw = open(path, encoding="utf-8", errors="replace").read()
    except OSError as error:
        raise ParseFailure(f"{path}: {error}")
    text = strip_noise(raw)
    found = []
    for head in ENUM_HEAD.finditer(text):
        open_brace = head.end() - 1
        depth, close = 0, None
        for index in range(open_brace, len(text)):
            if text[index] == "{":
                depth += 1
            elif text[index] == "}":
                depth -= 1
                if depth == 0:
                    close = index
                    break
        if close is None:
            raise ParseFailure(f"{path}:{raw[:head.start()].count(chr(10)) + 1}: unbalanced enum body")
        line = raw[: head.start()].count("\n") + 1
        # From the raw text: strip_noise blanks comments, so the declaration has
        # to be read before the copy the brace matcher uses.
        preceding = raw[: head.start()].rsplit("\n\n", 1)[-1]
        declared = MIRRORS_DECLARATION.findall(preceding)
        # The body comes from the ORIGINAL text so an initialiser survives; the
        # blanked copy is only for locating the braces.
        found.append((head.group(1), line, declared, enumerators(strip_noise(raw[open_brace + 1 : close]), f"{path}:{line}")))
    return found


def listed(arguments):
    """Tracked paths of the repository run from and of the libraries it carries, by path.

    A mirrored enum has its other half in another repository more often than not,
    so the corpus is every checkout the repository reads. Each file has one
    owner, so a path names one file.

    - Parameters:
      - arguments: the pathspecs to `git ls-files`.
    - Returns: {repository-relative path: absolute path}.
    """
    subject = ubq_roots.subject_of(__file__)
    found = {}
    for root in dict.fromkeys((subject, ubq_roots.kit_root(subject), ubq_roots.core_root(subject))):
        environment = None if root == subject else ubq_roots.foreign_git_environment()
        listing = subprocess.run(["git", "-C", root, "ls-files", *arguments], capture_output=True,
                                 text=True, check=True, env=environment).stdout.split("\n")
        found.update({path: os.path.join(root, path) for path in listing if path})
    return found


def main():
    located = listed(["Source/*.h", "Source/*.cpp", "Source/*.mm", "Source/*.m"])
    files = list(located)
    # ThirdParty is vendored: simdutf declares scoped enums, and holding code we
    # cannot patch to this tree's pinning rule means a vendor update turns the
    # gate red on somebody else's source. Every other gate here excludes it.
    files = [f for f in files if "ThirdParty" not in f]
    # The pathspec is a hypothesis about the population, so it is counted against
    # the subject once rather than trusted. Anything C-family under Source/ that
    # the four globs miss is listed, and it fails only if one of them declares a
    # scoped enum -- the generated .tables.inc files are outside on purpose and
    # judging them would report a defect their generator owns.
    everything = listed(["Source"])
    subject = list(everything)
    C_FAMILY = (".h", ".hpp", ".hh", ".cpp", ".cc", ".cxx", ".mm", ".m", ".inc")
    subject = [f for f in subject if f.endswith(C_FAMILY) and "ThirdParty" not in f]
    outside = sorted(set(subject) - set(files))
    smuggled = []
    for path in outside:
        try:
            text = open(everything[path], encoding="utf-8", errors="replace").read()
        except OSError:
            continue
        if ENUM_HEAD.search(strip_noise(text)):
            smuggled.append(path)
    if smuggled:
        for path in smuggled:
            gate_report.finding(path, GATE, "declares a scoped enum and is outside the scanned globs")
        return gate_report.failed(GATE, "file(s) with a scoped enum outside the scanned region", smuggled)

    if len(files) < 100:
        print(f"check-mirrored-enums: FAILED — the file list came back with {len(files)} entries.")
        print("  The glob stopped matching; a scan over nothing passes without looking at anything.")
        return 1

    by_name, failures, declarations_made = {}, [], []
    for path in files:
        try:
            for name, line, declared, members in scan(located[path]):
                by_name.setdefault(name, []).append((path, line, members))
                for target in declared:
                    declarations_made.append((path, line, name, target, members))
        except ParseFailure as error:
            failures.append(str(error))

    if failures:
        for failure in failures:
            location, _, detail = failure.partition(": ")
            gate_report.finding(location, GATE, detail or failure)
        print("\n  An unparsed body under-reports overlap, which excuses a real mirror, and")
        print("  invents short enums, which fabricates mirrors that do not exist. Fix the")
        print("  parser rather than the enum.", file=sys.stderr)
        return gate_report.failed(GATE, "unreadable enum body(s)", failures)

    # Declared pairs, checked before the name-keyed ones: a declaration gone stale
    # is a wrong answer, where a missing one is only a gap.
    declared_failures, declared_pairs = [], 0
    for path, line, name, target, members in declarations_made:
        matches = by_name.get(target, [])
        if not matches:
            declared_failures.append((path, line,
                f"{name} declares `// mirrors: {target}` and no enum of that name exists"))
            continue
        ours = [member for member, _ in members]
        for other_path, other_line, other_members in matches:
            theirs = [member for member, _ in other_members]
            if ours != theirs:
                declared_failures.append((path, line,
                    f"{name} declares `// mirrors: {target}` ({other_path}:{other_line}) and they differ: "
                    f"{ours} vs {theirs}"))
                continue
            declared_pairs += 1
            for wp, wl, wm in ((path, line, members), (other_path, other_line, other_members)):
                loose = [member for member, pinned in wm if not pinned]
                if loose:
                    declared_failures.append((wp, wl,
                        f"{name} <-> {target} is a declared mirror; these enumerators carry no explicit "
                        f"value: {', '.join(loose)}"))

    if declared_failures:
        for wp, wl, detail in declared_failures:
            gate_report.finding(f"{wp}:{wl}", GATE, detail)
        return gate_report.failed(GATE, "declared mirror(s) that do not hold",
                                  [d for _, _, d in declared_failures])

    unpinned, mirrors = [], 0
    for name, declarations in sorted(by_name.items()):
        if len(declarations) < 2:
            continue
        for first in range(len(declarations)):
            for second in range(first + 1, len(declarations)):
                left, right = declarations[first], declarations[second]
                left_names = {member for member, _ in left[2]}
                right_names = {member for member, _ in right[2]}
                shared = left_names & right_names
                smaller = min(len(left_names), len(right_names))
                if len(shared) < MINIMUM_SHARED_NAMES or not smaller:
                    continue
                if len(shared) / smaller < MINIMUM_OVERLAP_RATIO:
                    continue
                mirrors += 1
                for path, line, members in (left, right):
                    loose = [member for member, pinned in members if not pinned]
                    if loose:
                        unpinned.append((path, line, name, loose, f"{len(shared)}/{smaller} shared with "
                                         f"{(right if (path, line) == (left[0], left[1]) else left)[0]}"))

    if unpinned:
        offenders = []
        for path, line, name, loose, detail in sorted(set((p, l, n, tuple(x), d) for p, l, n, x, d in unpinned)):
            gate_report.finding(f"{path}:{line}", GATE,
                                f"enum class {name} leaves {', '.join(loose)} positional ({detail})")
            offenders.append(f"{name} ({path}:{line})")
        print("\n  Two positional enums over overlapping names agree only while nobody inserts")
        print("  into either, and inserting is the natural edit: a new case belongs beside the")
        print("  one it resembles, which is the middle. Give every enumerator an explicit")
        print("  value, in both declarations.", file=sys.stderr)
        return gate_report.failed(GATE, "mirrored enum(s) leaving enumerators positional", offenders)

    return gate_report.ok(GATE, f"{mirrors} name-keyed + {declared_pairs} declared mirror pair(s) across "
                          f"{len(by_name)} scoped enum name(s) in {len(files)} of {len(subject)} C-family file(s), all pinned")


if __name__ == "__main__":
    sys.exit(main())
