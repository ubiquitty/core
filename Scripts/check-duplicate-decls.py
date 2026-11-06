#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""check-duplicate-decls.py — one declaration, one home.

A free function declared in two headers has no home, and the second copy is not
a convenience: it is a second source of truth that nothing reconciles.

    Core/Modes/Emulation.h       Emulation emulation(const Terminal* t);
    Core/TerminalOperations.h    Emulation emulation(const Terminal* t);

When one header includes the other, both copies are live in every TU and the
compiler is content. What decays instead is everything a declaration carries
beyond its signature: the copies' doc comments diverge, one describes a
behaviour the other does not mention, and a third copy elsewhere names a file
that has since been renamed. A reader who finds one copy has no
way to know the other exists, and a module boundary drawn around the first
header is fiction while the second one stands.

Signatures drift too, just later. Change the copy in the module header and the
build stays green until some TU includes both — or, when the two modules
re-declare instead of including so neither depends on the other's header, until
the linker fails to find a symbol whose parameter list moved under it.

The fix for a name two modules genuinely share is a seam header both include,
not two spellings kept in step by hand.

## What this does not catch

Under-reports rather than guessing. Each case below is one a text scan cannot
tell apart from a legitimate second name, so none of them is reported:

  * Only declarations written on one line and ending in `);`. A signature broken
    across lines is invisible here.
  * Only free functions. A member declared inside a class or struct is skipped,
    since two classes may legitimately declare the same member name.
  * Only textual identity of the qualified name — this is a text scan, not a
    compile. It cannot see a duplicate whose two copies spell a parameter type
    differently, and it does not know overloads from redeclarations, so two
    genuine overloads in different headers would be reported. There are none in
    the tree; the day there are, that pair is the exemption to argue about.
  * Nothing about DEFINITIONS. Two functions with byte-identical bodies under
    different names are two declarations, and no declaration check can see the
    duplication. A saturating decimal accumulate defined once for OSC and once
    for VT under two names is such a pair; finding one takes a person reading.

So a pass means "no name is declared in two headers", not "the headers are
clean". It is a floor.
"""

import collections
import pathlib
import re
import sys

NAMESPACE_OPEN = re.compile(r"\bnamespace\s+([A-Za-z_][A-Za-z0-9_:]*)\s*\{")
ANONYMOUS_OPEN = re.compile(r"\bnamespace\s*\{")
FUNCTION_NAME = re.compile(r"([A-Za-z_][A-Za-z0-9_]*)\s*\(")
ATTRIBUTE = re.compile(r"\[\[[^\]]*\]\]")


def strip_noncode(line):
    """Comments and string bodies removed, so neither can open a brace."""
    line = re.sub(r"//.*$", "", line)
    line = re.sub(r"/\*.*?\*/", " ", line)
    return re.sub(r'"(\\.|[^"\\])*"', '""', line)


def declarations(path):
    """Yield (qualified name, line number) for one-line free-function declarations."""
    # Namespaces and every other brace share one stack: a declaration is at
    # namespace scope only when no plain brace is open above it, which is what
    # separates a free function from a member without parsing class bodies.
    scopes = []
    for number, raw in enumerate(path.read_text(errors="replace").splitlines(), 1):
        code = strip_noncode(raw)
        position = 0
        while position < len(code):
            namespace = NAMESPACE_OPEN.search(code, position)
            anonymous = ANONYMOUS_OPEN.search(code, position)
            opening = code.find("{", position)
            closing = code.find("}", position)
            if opening == -1 and closing == -1:
                break
            if closing != -1 and (opening == -1 or closing < opening):
                if scopes:
                    scopes.pop()
                position = closing + 1
                continue
            if namespace and namespace.end() - 1 == opening:
                scopes.append(("namespace", namespace.group(1)))
            elif anonymous and anonymous.end() - 1 == opening:
                scopes.append(("namespace", None))
            else:
                scopes.append(("brace", None))
            position = opening + 1

        statement = code.strip()
        if not statement.endswith(");"):
            continue
        if statement.startswith(("//", "*", "#", "return", "}")):
            continue
        if any(kind == "brace" for kind, _ in scopes):
            continue
        # An unnamed namespace is per-TU by definition, so two headers declaring
        # the same name in one are two different functions.
        if any(kind == "namespace" and name is None for kind, name in scopes):
            continue
        normalised = re.sub(r"\s+", " ", ATTRIBUTE.sub("", statement)).strip()
        before_parameters = normalised.split("(")[0]
        # A return type is the second word: one word is a call, not a declaration.
        # `=`, `.` and `->` mean an initialiser or an expression statement.
        if len(before_parameters.split()) < 2:
            continue
        if any(token in before_parameters for token in ("=", ".", "->")):
            continue
        name = FUNCTION_NAME.search(normalised)
        if not name:
            continue
        scope = "::".join(n for kind, n in scopes if kind == "namespace")
        yield f"{scope}::{name.group(1)}", number


def main():
    root = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else ".")
    sites = collections.defaultdict(list)
    for header in sorted((root / "Source").rglob("*.h")):
        if "ThirdParty" in header.parts:
            continue
        for qualified, number in declarations(header):
            sites[qualified].append((header.relative_to(root).as_posix(), number))

    duplicates = {
        name: places
        for name, places in sites.items()
        if len({path for path, _ in places}) > 1
    }
    if not duplicates:
        print(f"check-duplicate-decls: PASSED — {len(sites)} declarations, each in one header")
        return 0

    print(f"check-duplicate-decls: FAILED — {len(duplicates)} declared in more than one header\n")
    for name in sorted(duplicates):
        print(f"  {name}")
        for path, number in duplicates[name]:
            print(f"      {path}:{number}")
    print("\nGive the name one home. If two modules share it, that home is a seam")
    print("header both include — Core/VT/Shared.h is the worked example.")
    return 1


if __name__ == "__main__":
    sys.exit(main())
