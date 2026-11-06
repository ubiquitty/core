#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""check-name-collisions.py — one name, two namespaces, two values.

    Ubiquitty::Preferences::defaultScrollbackLines = 10000
    Ubiquitty::VT::defaultScrollbackLines     = 800

Both are legal, both are wanted, and which one an unqualified use means
depends only on the namespace it is lexically inside. That is fine until
something rewrites the use — adding a qualifier, moving a declaration between
namespaces, or adding an include that brings the other one into scope. A
textual rewrite discards exactly the enclosing scope that was distinguishing
them, so it can repoint a use at the other constant and the result compiles,
links, passes every test, and ships a different number.

A sweep that qualifies a name cannot see that the name was already ambiguous.
Nor can a collision check written to look for a second DEFINITION of one symbol
— that is a different question, and it answers clean here, because these are two
symbols rather than one symbol twice.

Reported only when the VALUES differ. Two namespaces agreeing on a constant is
a duplicate worth tidying, not a hazard: picking the wrong one changes nothing.

Namespace-scope declarations only, at column zero, which is where this codebase
puts them. A class member is qualified by its class and cannot be reached by an
unqualified use from another namespace.

What the count is and is not. A floor, never a ceiling. This matches a
`constexpr` with an initialiser, so a collision expressed as an enum member, a
macro, a non-constexpr constant, or a class name is invisible to it — a
namespace-scope alias is `--aliases`' question — and a clean run means the
rest were not looked for rather than not there. A count that implies more
than it checked is the same defect as a budget above the truth, and it reads
just as green.
"""

import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gate_report  # noqa: E402
import ubq_roots  # noqa: E402

GATE = "check-name-collisions"

# `inline constexpr int name = value;` at column zero, the house spelling for a
# namespace-scope constant. Indentation is the discriminator against class
# members, which no unqualified cross-namespace use can reach.
CONSTANT = re.compile(
    r'^(?:inline\s+)?(?:static\s+)?constexpr\s+[A-Za-z_][\w:<>,\s*&]*?\b(\w+)\s*=\s*([^;]+);')

NAMESPACE_OPEN = re.compile(r'^namespace\s+([A-Za-z_][\w:]*)\s*\{')
STRINGS_AND_COMMENTS = re.compile(r'//.*|/\*.*?\*/|"(?:[^"\\]|\\.)*"|\'(?:[^\'\\]|\\.)*\'')

SOURCE_SUFFIXES = (".h", ".hpp")
# --types reads implementation files too: a stand-in type is defined in a bench
# or a test .cpp, which is exactly where it escapes every other check.
ALL_SUFFIXES = (".h", ".hpp", ".cpp", ".mm")

# An attribute macro may sit between the keyword and the name, so the name is
# the LAST identifier before the brace rather than the first after the keyword.
AGGREGATE = re.compile(
    r'^\s*(?:class|struct)\s+(?:[A-Z_][A-Z0-9_]*\s+)*([A-Za-z_]\w*)\s*(?:final\s*)?[:{]')


def declarations(root):
    """Yields (name, namespace path, value, relative path, line number)."""
    for directory, subdirectories, files in os.walk(os.path.join(root, "Source")):
        subdirectories[:] = [d for d in subdirectories
                             if d not in ("ThirdParty", "compat", "build")]
        for name in sorted(files):
            if not name.endswith(SOURCE_SUFFIXES):
                continue
            path = os.path.join(directory, name)
            with open(path, "r", encoding="utf-8", errors="replace") as handle:
                text = handle.read()

            # Nesting by brace depth, never by the shape of the closing line: a
            # struct closes with `};` at column zero exactly as a namespace
            # closes with `}`, so a reader keyed on the line drops to depth zero
            # at the first type in the file and reads the rest as global.
            depth, stack = 0, []
            for number, line in enumerate(text.split("\n"), 1):
                bare = STRINGS_AND_COMMENTS.sub("", line)
                opened = NAMESPACE_OPEN.match(line)
                if opened:
                    stack.append((depth, opened.group(1).split("::")))
                    depth += bare.count("{") - bare.count("}")
                    continue
                constant = CONSTANT.match(line)
                if constant and stack:
                    yield (constant.group(1),
                           "::".join(n for _, group in stack for n in group),
                           constant.group(2).strip(),
                           os.path.relpath(path, root), number)
                depth += bare.count("{") - bare.count("}")
                while stack and depth <= stack[-1][0]:
                    stack.pop()


def aggregates(root):
    """Yields (type name, namespace path, area, relative path, line number)."""
    for area in ("Source", "Tests"):
        base = os.path.join(root, area)
        if not os.path.isdir(base):
            continue
        for directory, subdirectories, files in os.walk(base):
            subdirectories[:] = [d for d in subdirectories
                                 if d not in ("ThirdParty", "compat", "build")]
            for name in sorted(files):
                if not name.endswith(ALL_SUFFIXES):
                    continue
                path = os.path.join(directory, name)
                with open(path, "r", encoding="utf-8", errors="replace") as handle:
                    text = handle.read()
                depth, stack = 0, []
                for number, line in enumerate(text.split("\n"), 1):
                    bare = STRINGS_AND_COMMENTS.sub("", line)
                    opened = NAMESPACE_OPEN.match(line)
                    if opened:
                        stack.append((depth, opened.group(1).split("::")))
                        depth += bare.count("{") - bare.count("}")
                        continue
                    aggregate = AGGREGATE.match(line)
                    if aggregate:
                        yield (aggregate.group(1),
                               "::".join(n for _, group in stack for n in group),
                               area, os.path.relpath(path, root), number)
                    depth += bare.count("{") - bare.count("}")
                    while stack and depth <= stack[-1][0]:
                        stack.pop()


def report_type_shadows(root):
    """A type in Tests/ sharing a Source/ type's name under a different qualifier.

    A bench defines `struct Pen` as a 20-byte stand-in for `Ubiquitty::Pen`.
    Both are legal and the compiler is content, because the qualifiers differ —
    until a sweep qualifies the stand-in's DEFINITION and turns it into a
    redefinition of the production type. That is a build failure if you are
    lucky and a silently different layout if you are not.

    Reported rather than resolved: a stand-in is often the right thing to have.
    The count is what must not grow unnoticed.
    """
    byName = {}
    for name, namespace, area, relative, number in aggregates(root):
        byName.setdefault(name, set()).add((namespace, area, relative, number))

    shadows = []
    for name, declarations in byName.items():
        areas = {area for _, area, _, _ in declarations}
        qualifiers = {namespace for namespace, _, _, _ in declarations}
        if "Tests" in areas and "Source" in areas and len(qualifiers) > 1:
            shadows.append((name, sorted(declarations)))
    shadows.sort()

    for name, declarations in shadows:
        for namespace, area, relative, number in declarations:
            gate_report.finding(
                f"{relative}:{number}", GATE,
                f"{name} in {namespace or 'the global namespace'} ({area}) is shared "
                "across Tests/ and Source/ under different qualifiers")
    if shadows:
        return gate_report.failed_count(
            GATE, len(shadows), "type names shared across Tests/ and Source/")
    return gate_report.ok(GATE, "no type name is shared across Tests/ and Source/")


# `struct X;` or `class X;` alone on a line — a forward declaration, which names
# a type without defining it and therefore without checking that it exists.
FORWARD = re.compile(r'^\s*(?:struct|class)\s+([A-Za-z_]\w*)\s*;\s*$')


def report_stale_forwards(root):
    """A forward declaration at one scope for a type defined only at another.

    This is the reference form that survives a namespace move. `struct Pen;` at
    global scope kept naming a type after `Pen` became `Ubiquitty::Pen`: it
    compiles, it stays inert wherever enclosing lookup finds the real one first,
    and it hands an incomplete `::Pen` to any consumer that names the type at
    namespace scope. The diagnostic there says INCOMPLETE TYPE rather than
    undeclared, which points away from the header that caused it.

    What makes it worse than the other five spellings is that it was CORRECT
    when written. Nothing about the declaration is wrong until someone moves the
    definition, and no build can tell that the two were meant to be one type.

    Reported only when the name is defined in the tree at some other path. A
    declaration for a type defined nowhere here — an opaque handle from
    FreeType, a pimpl defined in an implementation file this does not read — is
    exactly what a forward declaration is for, and has no other path to point at.
    """
    # Definitions are kept per area, and a declaration in Source/ is checked
    # against Source/ alone. A bench defines its own global `struct Pen` as a
    # stand-in, and pooling the two areas makes a global `struct Pen;` in a
    # production header look like a declaration of that — so the exact bug this
    # exists to catch reads as legitimate, vouched for by a test fixture.
    definitions = {"Source": {}, "Tests": {}}
    for name, namespace, area, _, _ in aggregates(root):
        definitions[area].setdefault(name, set()).add(namespace)

    def definedIn(area, name):
        """Paths defining `name` that a declaration in `area` may refer to."""
        paths = set(definitions["Source"].get(name, set()))
        if area == "Tests":
            paths |= definitions["Tests"].get(name, set())
        return paths

    stale = []
    for area in ("Source", "Tests"):
        base = os.path.join(root, area)
        if not os.path.isdir(base):
            continue
        for directory, subdirectories, files in os.walk(base):
            subdirectories[:] = [d for d in subdirectories
                                 if d not in ("ThirdParty", "compat", "build")]
            for name in sorted(files):
                if not name.endswith(ALL_SUFFIXES):
                    continue
                path = os.path.join(directory, name)
                with open(path, "r", encoding="utf-8", errors="replace") as handle:
                    text = handle.read()
                depth, stack = 0, []
                for number, line in enumerate(text.split("\n"), 1):
                    bare = STRINGS_AND_COMMENTS.sub("", line)
                    opened = NAMESPACE_OPEN.match(line)
                    if opened:
                        stack.append((depth, opened.group(1).split("::")))
                        depth += bare.count("{") - bare.count("}")
                        continue
                    # Only at namespace scope: an indented `struct X;` inside a
                    # class body declares a nested type, which is defined as
                    # `Outer::X` and collides with nothing.
                    namespaceDepth = stack[-1][0] + 1 if stack else 0
                    forward = FORWARD.match(line) if depth == namespaceDepth else None
                    if forward:
                        here = "::".join(n for _, group in stack for n in group)
                        elsewhere = definedIn(area, forward.group(1))
                        if elsewhere and here not in elsewhere:
                            stale.append((forward.group(1), here,
                                          os.path.relpath(path, root), number,
                                          sorted(elsewhere)))
                    depth += bare.count("{") - bare.count("}")
                    while stack and depth <= stack[-1][0]:
                        stack.pop()
    stale.sort()

    for name, here, relative, number, elsewhere in stale:
        gate_report.finding(
            f"{relative}:{number}", GATE,
            "forward declaration of %s in %s is defined only in %s"
            % (name, here or "the global namespace",
               ", ".join(w or "the global namespace" for w in elsewhere)))
    if stale:
        print("  This compiles. It stays inert wherever enclosing lookup finds the real", file=sys.stderr)
        print("  type first, and hands an incomplete one to any consumer that names it", file=sys.stderr)
        print("  at namespace scope - reported there as an incomplete type, in a file", file=sys.stderr)
        print("  that has nothing to do with the cause.", file=sys.stderr)
        return gate_report.failed_count(
            GATE, len(stale), "forward declarations name types defined elsewhere")
    return gate_report.ok(GATE, "no forward declaration names a type defined elsewhere")


# A free function declaration at column zero: return type, name, arguments,
# semicolon. Anchored at column zero because a member declaration is indented,
# and a member is qualified by its class rather than by the namespace.
FUNCTION = re.compile(
    r'^[A-Za-z_][\w:<>,&*\s]*?[\s&*]([A-Za-z_]\w*)\s*\(([^;]*)\)\s*(?:const\s*)?(?:noexcept\s*)?;')
NOT_A_FUNCTION = ("if", "for", "while", "switch", "return", "sizeof")


def parameter_count(arguments):
    """How many parameters a declaration's argument list holds.

    Commas inside templates and function-pointer types are not separators, so
    the count is taken at bracket depth zero. Arity is a PROXY for signature
    compatibility rather than the thing itself: a same-arity pair whose types
    differ is also a compile error, and reads here as the silent kind.
    """
    text = arguments.strip()
    if not text or text == "void":
        return 0
    depth, count = 0, 1
    for character in text:
        if character in "<([":
            depth += 1
        elif character in ">)]":
            depth -= 1
        elif character == "," and depth == 0:
            count += 1
    return count


def report_function_shadows(root):
    """A free function whose name is also declared in an ANCESTOR namespace.

    `Ubiquitty::clearBuffer` and `Ubiquitty::Screen::clearBuffer` are both
    reachable from an unqualified call inside `Screen`, and enclosing lookup
    picks the inner one. Move the outer in and they collide; move a caller out
    and it silently changes which function it means.

    Ancestor, not merely different. Two SIBLING namespaces declaring the same
    name — two bridges each defining their own `systemLightTheme` — cannot both
    be seen by any unqualified use, so the shared spelling is a bridging pattern rather than a hazard. Across
    Source/ the difference is 75 names against 3: reporting all of them is a
    gate nobody can act on, which is worse than not having one.

    Column zero, because a member declaration is indented and a member is
    qualified by its class. That distinction is what a call-shaped sweep cannot
    make, and it is why `resize` needed a two-step rename.
    """
    byName = {}
    for directory, subdirectories, files in os.walk(os.path.join(root, "Source")):
        subdirectories[:] = [d for d in subdirectories
                             if d not in ("ThirdParty", "compat", "build")]
        for name in sorted(files):
            if not name.endswith(SOURCE_SUFFIXES):
                continue
            path = os.path.join(directory, name)
            with open(path, "r", encoding="utf-8", errors="replace") as handle:
                text = handle.read()
            depth, stack, aggregates = 0, [], []
            for number, line in enumerate(text.split("\n"), 1):
                bare = STRINGS_AND_COMMENTS.sub("", line)
                opened = NAMESPACE_OPEN.match(line)
                if opened:
                    stack.append((depth, opened.group(1).split("::")))
                    depth += bare.count("{") - bare.count("}")
                    continue
                if AGGREGATE.match(line):
                    aggregates.append(depth)
                    depth += bare.count("{") - bare.count("}")
                    while aggregates and depth <= aggregates[-1]:
                        aggregates.pop()
                    continue
                if not aggregates:
                    function = FUNCTION.match(line)
                    if function and function.group(1) not in NOT_A_FUNCTION:
                        where = "::".join(n for _, group in stack for n in group)
                        byName.setdefault(function.group(1), {}).setdefault(
                            where, (os.path.relpath(path, root), number,
                                    parameter_count(function.group(2))))
                depth += bare.count("{") - bare.count("}")
                while aggregates and depth <= aggregates[-1]:
                    aggregates.pop()
                while stack and depth <= stack[-1][0]:
                    stack.pop()

    shadows = []
    for name, byNamespace in byName.items():
        paths = sorted(byNamespace)
        for index, outer in enumerate(paths):
            for inner in paths[index + 1:]:
                if inner.startswith(outer + "::") or (not outer and inner):
                    shadows.append((name, outer, inner,
                                    byNamespace[outer], byNamespace[inner]))
    shadows.sort()

    # C++ name hiding is what separates the two. An inner declaration hides the
    # outer rather than overloading it, so a bare call meaning the outer one
    # fails to compile — UNLESS the signatures are compatible, which is the only
    # shape that binds silently and keeps working while meaning something else.
    silent = [row for row in shadows if row[3][2] == row[4][2]]
    loud = [row for row in shadows if row[3][2] != row[4][2]]

    for name, outer, inner, outerAt, innerAt in silent:
        reason = (f"{name} in {inner} hides the declaration in "
                  f"{outer or 'the global namespace'} with the same arity")
        gate_report.finding(f"{outerAt[0]}:{outerAt[1]}", GATE, reason)
        gate_report.finding(f"{innerAt[0]}:{innerAt[1]}", GATE, reason)

    for name, outer, inner, outerAt, innerAt in loud:
        print("%s  %s:%d (%d args) and %s:%d (%d args) — hiding, so a wrong bare call "
              "does not compile"
              % (name, outerAt[0], outerAt[1], outerAt[2],
                 innerAt[0], innerAt[1], innerAt[2]))

    if silent:
        print("  Give the outer one anything to do beyond forwarding and every bare call", file=sys.stderr)
        print("  inside the inner namespace silently keeps the old meaning.", file=sys.stderr)
        return gate_report.failed_count(
            GATE, len(silent), "functions hide an enclosing declaration with matching arity")
    return gate_report.ok(
        GATE, f"{len(loud)} shadowed function names, none with matching arity")


# A namespace-scope type alias at column zero: `using Name = Target;`. Anchored
# at column zero because a member alias is indented and is qualified by its
# class. `using namespace` and using-declarations carry no `=`, so neither
# matches.
ALIAS = re.compile(r'^using\s+([A-Za-z_]\w*)\s*=\s*([^;]+);')


def report_alias_shadows(root):
    """A namespace-scope alias whose name an ancestor namespace also aliases.

    Two aliases such as `Rendering::CodePointSpan` and
    `Rendering::Font::CodePointSpan`, naming one span type from two namespaces,
    let every use compile whichever it spells — including the uses that spell
    the wrong one. Agreement is exactly what keeps such a pair quiet: the first
    edit that changes one of them splits a name every reader has been treating
    as one type, and that edit compiles
    at each use site that meant the other.

    Agreeing and differing targets both report, where the constants mode
    reports only differing values. Picking the wrong one of two agreeing
    constants changes nothing, ever; an agreeing alias pair is the latent
    form of a differing one, and the cheap moment to break the pair is when
    the second declaration appears rather than when the targets diverge.

    Ancestor, not merely different, for the reason the function mode gives: an
    unqualified use inside the inner namespace reaches both and enclosing
    lookup decides which it means, while siblings cannot see each other's
    declaration.
    """
    byName = {}
    for directory, subdirectories, files in os.walk(os.path.join(root, "Source")):
        subdirectories[:] = [d for d in subdirectories
                             if d not in ("ThirdParty", "compat", "build")]
        for name in sorted(files):
            if not name.endswith(SOURCE_SUFFIXES):
                continue
            path = os.path.join(directory, name)
            with open(path, "r", encoding="utf-8", errors="replace") as handle:
                text = handle.read()
            depth, stack = 0, []
            for number, line in enumerate(text.split("\n"), 1):
                bare = STRINGS_AND_COMMENTS.sub("", line)
                opened = NAMESPACE_OPEN.match(line)
                if opened:
                    stack.append((depth, opened.group(1).split("::")))
                    depth += bare.count("{") - bare.count("}")
                    continue
                alias = ALIAS.match(line)
                if alias:
                    where = "::".join(n for _, group in stack for n in group)
                    byName.setdefault(alias.group(1), {}).setdefault(
                        where, (" ".join(alias.group(2).split()),
                                os.path.relpath(path, root), number))
                depth += bare.count("{") - bare.count("}")
                while stack and depth <= stack[-1][0]:
                    stack.pop()

    shadows = []
    for name, byNamespace in byName.items():
        paths = sorted(byNamespace)
        for index, outer in enumerate(paths):
            for inner in paths[index + 1:]:
                if inner.startswith(outer + "::") or (not outer and inner):
                    shadows.append((name, outer, inner,
                                    byNamespace[outer], byNamespace[inner]))
    shadows.sort()

    for name, outer, inner, outerAt, innerAt in shadows:
        agreement = ("the same type, so a misspelled qualifier compiles today"
                     if outerAt[0] == innerAt[0] else "DIFFERENT types")
        for where, at in ((outer, outerAt), (inner, innerAt)):
            gate_report.finding(
                f"{at[1]}:{at[2]}", GATE,
                f"{(where + '::' + name) if where else name} aliases {at[0]}; "
                f"the nested and ancestor declarations resolve to {agreement}")

    if shadows:
        print("  An unqualified use inside the inner namespace reaches both, and enclosing", file=sys.stderr)
        print("  lookup decides which it means. An agreeing pair is one edit away from", file=sys.stderr)
        print("  the differing kind, and that edit compiles at every use site.", file=sys.stderr)
        return gate_report.failed_count(
            GATE, len(shadows), "alias names are redeclared in nested namespaces")
    return gate_report.ok(GATE, "no namespace-scope alias is redeclared in a nested namespace")


def main(argv):
    root = os.path.abspath(argv[1] if len(argv) > 1 and not argv[1].startswith("--") else ".")
    if not os.path.isdir(os.path.join(root, "Source")):
        return gate_report.refused(GATE, f"no Source/ under {root}", location=root)

    if "--types" in argv:
        return report_type_shadows(root)

    if "--functions" in argv:
        return report_function_shadows(root)

    if "--forward" in argv:
        return report_stale_forwards(root)

    if "--aliases" in argv:
        return report_alias_shadows(root)

    byName = {}
    total = 0
    for name, namespace, value, relative, number in declarations(root):
        total += 1
        byName.setdefault(name, {})[namespace] = (value, relative, number)

    if not total and "core" not in ubq_roots.repositories(root):
        return gate_report.ok(GATE, "no namespace-scope constant in this repository, so none can differ")
    if not total:
        return gate_report.refused(
            GATE, "no namespace-scope constants found; the reader matched nothing",
            location="Source")

    collisions = []
    for name, byNamespace in byName.items():
        if len(byNamespace) < 2:
            continue
        if len({value for value, _, _ in byNamespace.values()}) < 2:
            continue  # same value in both: picking the wrong one changes nothing
        collisions.append((name, byNamespace))
    collisions.sort()

    if not collisions:
        return gate_report.ok(
            GATE, f"{total} namespace-scope constants; no name has different values")

    for name, byNamespace in collisions:
        for namespace in sorted(byNamespace):
            value, relative, number = byNamespace[namespace]
            gate_report.finding(
                f"{relative}:{number}", GATE,
                f"{namespace}::{name} has value {value}; another namespace differs")

    print("", file=sys.stderr)
    print("  An unqualified use resolves by enclosing scope, so adding an include or", file=sys.stderr)
    print("  moving a declaration silently repoints it at the other value. Nothing", file=sys.stderr)
    print("  fails to compile and no test asserts the difference.", file=sys.stderr)
    return gate_report.failed_count(
        GATE, len(collisions), "names mean different things in different namespaces")


if __name__ == "__main__":
    sys.exit(main(sys.argv))
