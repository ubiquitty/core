#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""check-enum-switches.py — an exhaustive switch that a new enumerator broke.

Adding an enumerator to a Core enum is a cross-frontend change. Every switch
over that enum with no `default:` stops compiling under -Werror,-Wswitch — which
is the good outcome, and it happens only in a build someone runs. The frontend
that switches on it is the slowest to build and the easiest to skip, and a macOS
translation unit cannot even be syntax-checked on its own: it stops at
Ubiquitty-Swift.h, which exists only after a Swift build.

So this reads the enumerator list out of the declaration and the cases out of
the switch, and needs neither a compiler nor a frontend toolchain.

The rule mirrors -Wswitch exactly: a switch with no `default:` whose cases are
qualified with one scoped enum must name every enumerator of it. Anything
ambiguous is left alone, because a gate nobody trusts gets bypassed.
"""

import os
import re
import sys

SOURCE_SUFFIXES = (".h", ".hpp", ".cpp", ".mm", ".m", ".c")

# `enum class Name : underlying {` — the opening line of a scoped enum with a
# body. A forward declaration has no `{` and names no enumerator.
ENUM_DECLARATION = re.compile(
    r"\benum\s+(?:class|struct)\s+([A-Za-z_]\w*)\s*(?::\s*[^{;]+?)?\s*\{")

# `case Qualified::Member:` — a scoped enumerator cannot appear unqualified, so
# this is the only spelling that reaches one.
CASE_LABEL = re.compile(r"\bcase\s+((?:[A-Za-z_]\w*\s*::\s*)+)([A-Za-z_]\w*)\s*:")

DEFAULT_LABEL = re.compile(r"\bdefault\s*:")

SWITCH_KEYWORD = re.compile(r"\bswitch\s*\(")


def strip_comments_and_strings(text):
    """Blanks comments and literals, keeping every byte offset and newline.

    Offsets have to survive so a finding can name the line it came from, and a
    `case` inside a string or a commented-out block is not a case.
    """
    out = list(text)
    index = 0
    length = len(text)
    while index < length:
        char = text[index]
        if char == "/" and index + 1 < length and text[index + 1] == "/":
            while index < length and text[index] != "\n":
                out[index] = " "
                index += 1
        elif char == "/" and index + 1 < length and text[index + 1] == "*":
            out[index] = out[index + 1] = " "
            index += 2
            while index + 1 < length and not (text[index] == "*" and text[index + 1] == "/"):
                if text[index] != "\n":
                    out[index] = " "
                index += 1
            if index + 1 < length:
                out[index] = out[index + 1] = " "
                index += 2
        elif char in "\"'":
            quote = char
            index += 1
            while index < length and text[index] != quote:
                if text[index] == "\\":
                    out[index] = " "
                    index += 1
                    if index < length:
                        out[index] = " "
                        index += 1
                    continue
                if text[index] != "\n":
                    out[index] = " "
                index += 1
            if index < length:
                index += 1
        else:
            index += 1
    return "".join(out)


def enumerators_of(body):
    """The enumerator names in an enum body, dropping any `= value`."""
    names = []
    for entry in body.split(","):
        entry = entry.split("=")[0].strip()
        if re.fullmatch(r"[A-Za-z_]\w*", entry):
            names.append(entry)
    return names


def matching_brace(text, open_index):
    """Index of the `}` closing the `{` at open_index, or -1."""
    depth = 0
    for index in range(open_index, len(text)):
        if text[index] == "{":
            depth += 1
        elif text[index] == "}":
            depth -= 1
            if depth == 0:
                return index
    return -1


def collect_enums(paths):
    """Maps a scoped enum's simple name to every declaration carrying it.

    `State`, `Kind` and `Direction` each name several unrelated enums here, and a
    leaf name cannot tell them apart. Resolving by name alone is the trap: it
    reports a switch against whichever declaration was seen first. So every
    candidate is kept, and `resolve` picks by the cases the switch actually
    names.

    - Returns: name -> list of (enumerators, declaring file).
    """
    enums = {}
    for path in paths:
        try:
            text = strippedSource(path)
        except OSError:
            continue
        for match in ENUM_DECLARATION.finditer(text):
            close = matching_brace(text, match.end() - 1)
            if close < 0:
                continue
            members = enumerators_of(text[match.end():close])
            if not members:
                continue
            candidates = enums.setdefault(match.group(1), [])
            if not any(set(existing) == set(members) for existing, _ in candidates):
                candidates.append((members, os.path.normpath(path)))
    return enums


def resolve(enums, name, members):
    """The one declaration of `name` whose enumerators cover `members`.

    A switch naming cases no single declaration contains is not a switch over one
    scoped enum, and two declarations that both cover them cannot be told apart
    from this switch — both return None, because reporting either would be a
    guess.

    The first of those also swallows a switch with one misspelled case label,
    which then goes unchecked. That is the compiler's: a case naming an
    enumerator that does not exist has never built.

    - Returns: (enumerators, declaring file), or None.
    """
    viable = [entry for entry in enums.get(name, []) if members.issubset(set(entry[0]))]
    return viable[0] if len(viable) == 1 else None


def read(path):
    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        return handle.read()


# The declaration sweep and the switch sweep both strip the same file, so
# without this every source is stripped twice — the strip is a per-character
# walk and dominates the run.
_stripped = {}


def strippedSource(path):
    """The file with comments and literals blanked, stripped at most once.

    - Parameter path: the source file.
    - Returns: the blanked text, byte offsets preserved.
    """
    text = _stripped.get(path)
    if text is None:
        text = strip_comments_and_strings(read(path))
        _stripped[path] = text
    return text


def switch_bodies(text):
    """Yields (body, offset) for each switch body, innermost bodies included."""
    for match in SWITCH_KEYWORD.finditer(text):
        # Step over the condition to the body's `{`, which is the first brace
        # after the condition's closing parenthesis.
        depth = 0
        index = match.end() - 1
        while index < len(text):
            if text[index] == "(":
                depth += 1
            elif text[index] == ")":
                depth -= 1
                if depth == 0:
                    break
            index += 1
        brace = text.find("{", index)
        if brace < 0:
            continue
        close = matching_brace(text, brace)
        if close < 0:
            continue
        yield text[brace:close], brace


def top_level_spans(body):
    """Offsets in `body` that sit at the switch's own brace depth.

    A nested switch or a lambda carries its own `default:` and its own cases,
    and neither belongs to this switch.
    """
    depth = 0
    spans = []
    start = 0
    for index, char in enumerate(body):
        if char == "{":
            if depth == 1:
                spans.append((start, index))
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 1:
                start = index
    spans.append((start, len(body)))
    return spans


def findings_for(path, enums):
    text = strippedSource(path)
    results = []
    for body, offset in switch_bodies(text):
        own = "".join(body[start:end] for start, end in top_level_spans(body))
        if DEFAULT_LABEL.search(own):
            continue
        qualifiers = set()
        members = set()
        for case in CASE_LABEL.finditer(own):
            qualifiers.add(case.group(1).replace(" ", "").strip(":").split("::")[-1])
            members.add(case.group(2))
        # One enum, known, and every case belongs to it. Two qualifiers means
        # this is not a plain switch over one scoped enum, and no qualifier means
        # the cases are integers or unscoped — neither is this check's subject.
        if len(qualifiers) != 1:
            continue
        enum_name = qualifiers.pop()
        resolved = resolve(enums, enum_name, members)
        if resolved is None:
            continue
        expected, home = resolved
        missing = [name for name in expected if name not in members]
        if missing:
            line = text.count("\n", 0, offset) + 1
            results.append((line, enum_name, missing, home))
    return results


def sources(root):
    for directory, subdirectories, files in os.walk(root):
        subdirectories[:] = [d for d in subdirectories
                             if d not in ("ThirdParty", "compat", "build", ".git")]
        for name in files:
            if name.endswith(SOURCE_SUFFIXES):
                yield os.path.join(directory, name)


def staged_files(root):
    import subprocess
    result = subprocess.run(
        ["git", "diff", "--cached", "--name-only", "--diff-filter=ACMR"],
        cwd=root, capture_output=True, text=True)
    if result.returncode != 0:
        return None
    return {os.path.normpath(os.path.join(root, name))
            for name in result.stdout.split("\n") if name}


def main(argv):
    staged_only = "--staged" in argv[1:]
    positional = [argument for argument in argv[1:] if not argument.startswith("--")]
    root = positional[0] if positional else "."

    paths = sorted(sources(os.path.join(root, "Source")))
    enums = collect_enums(paths)
    declarations = sum(len(candidates) for candidates in enums.values())

    # A change's blast radius is not the set of files it edits. Adding an
    # enumerator breaks switches in frontends the author never opened, so a
    # finding is attributed to this change when either the switch's file or the
    # declaring file of the enum the switch resolved to is in it.
    changed = staged_files(root) if staged_only else None

    blocking = []
    pre_existing = []
    for path in paths:
        try:
            results = findings_for(path, enums)
        except OSError:
            continue
        for line, enum_name, missing, home in results:
            entry = (os.path.relpath(path, root), line, enum_name, missing)
            if changed is None or os.path.normpath(path) in changed or home in changed:
                blocking.append(entry)
            else:
                pre_existing.append(entry)

    for relative, line, enum_name, missing in blocking:
        print("%s:%d: switch over %s has no default: and no case for %s"
              % (relative, line, enum_name, ", ".join(missing)), file=sys.stderr)
    for relative, line, enum_name, missing in pre_existing:
        print("%s:%d: switch over %s has no default: and no case for %s (pre-existing)"
              % (relative, line, enum_name, ", ".join(missing)), file=sys.stderr)

    if blocking:
        print("", file=sys.stderr)
        print("check-enum-switches: %d exhaustive switch(es) missing a case."
              % len(blocking), file=sys.stderr)
        print("  Add the case, or a default: if the switch is meant to handle a subset.",
              file=sys.stderr)
        return 1
    if pre_existing:
        print("check-enum-switches: ok for this change (%d pre-existing above)"
              % len(pre_existing))
        return 0
    print("check-enum-switches: ok (%d scoped enum declarations, %d files)"
          % (declarations, len(paths)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
