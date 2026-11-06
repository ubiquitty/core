# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""Which namespace a header declares, by reading the header.

Imported by `check-core-namespaces.sh`, which enforces the rule, and by anything
measuring a proposed change to it. A probe that reimplements this answers about
its own approximation: two of seven headers disagreed with the gate the first
time I tried, so the numbers could not be compared with the ones the gate
produces.
"""

import re

# `namespace Foo {` opening a block, in declaration order. A namespace closed and
# reopened later in the same header (PNGDecoder does this) contributes both, and
# the first nesting under Ubiquitty is the one that names the header.
#
# The name may be nested — `namespace Protocol::Sync {` — and a pattern that stops
# at the first identifier matches nothing there, reporting a header that is
# already right as having no namespace at all. Captured whole and split, because
# the two spellings mean the same thing and only one of them is greppable.
NAMESPACE = re.compile(r'^namespace\s+([A-Za-z_][A-Za-z0-9_:]*)\s*\{', re.M)

# A block holding nothing but forward declarations of someone else's types does
# not name the header. `Runtime/TerminalRuntime.h` opens `namespace PAL { class
# Stream; }` so it can take a Stream by pointer without including PAL/Stream.h —
# 284 lines and three PAL headers, in a header whose whole purpose is to stay
# light enough for the Runtime layer. Reading that as "this header declares PAL"
# asks it to choose between a wrong verdict and a heavy include.
#
# The distinction is definition versus mention: `class Stream;` names a type
# declared elsewhere, while anything with a body, a value or a signature is this
# header's own. A block of the former is skipped when deciding which namespace
# names the file; it is not skipped anywhere else, so a header that forward-
# declares and defines under the same wrong name still fails.
FORWARD_ONLY = re.compile(
    r'^\s*(?:template\s*<[^>]*>\s*)?(?:struct|class|enum(?:\s+class)?|union)\s+'
    r'[A-Za-z_][A-Za-z0-9_]*(?:\s*:\s*[A-Za-z_:][A-Za-z0-9_:\s]*)?\s*;\s*$')


def _is_substantive(line):
    """Whether one line is this header's own declaration rather than scaffolding."""
    stripped = line.strip()
    if not stripped or stripped.startswith(("//", "*", "/*", "#")):
        return False
    if stripped in ("}",) or stripped.startswith("} //"):
        return False
    if NAMESPACE.match(line):
        return False
    if FORWARD_ONLY.match(re.sub(r'//.*$', '', line)):
        return False
    return True


def _blocks(text):
    """(name, first, last) for every namespace block, by brace balance."""
    lines = text.splitlines()
    opens = [(i, m.group(1)) for i, line in enumerate(lines)
             for m in [NAMESPACE.match(line)] if m]

    def body_span(start):
        depth = 0
        for i in range(start, len(lines)):
            depth += lines[i].count("{") - lines[i].count("}")
            if i > start and depth <= 0:
                return start + 1, i
        return start + 1, len(lines)

    return lines, [(name, *body_span(start)) for start, name in opens]


def namespaces_that_name(text):
    """Namespace names opened in `text`, in order, skipping forward-only blocks.

    A block counts when its own body holds anything that is not a forward
    declaration — a definition, a value, a signature, a using, a static_assert.
    Nested blocks are judged on their own bodies, so an outer namespace that
    merely contains a real inner one still counts.
    """
    lines, blocks = _blocks(text)
    return [name for name, first, last in blocks
            if any(_is_substantive(line) for line in lines[first:last])]


def heaviest_namespace_path(text):
    """The full path of the block holding the most declarations of its own.

    Which namespace NAMES a header is a question about weight, not position. A
    header whose enums sit in bare `Ubiquitty` and whose only nested block holds
    one `constexpr` predicate is named by the umbrella, however the blocks are
    ordered — `Core/Modes/TerminalModes.h` is exactly that, 19 declarations against 1,
    and reading the first nested block accepted it into `Core/Modes/`.

    The path is kept whole rather than reduced to the innermost name, because a
    header nesting DEEPER than its folder is correct: `Codec/JSON.h` declares
    `Ubiquitty::Codec::JSON` and the folder names only `Codec`. Returning `JSON`
    alone turns 42 correctly-placed headers into findings.

    - Parameter text: The header's contents.
    - Returns: `(path, count)` with `Ubiquitty` stripped, so an umbrella header
      yields an empty path; `None` when the text opens no namespace.
    """
    lines = text.splitlines()
    stack, blocks, depth = [], [], 0
    for line in lines:
        opening = NAMESPACE.match(line)
        if opening:
            stack.append((opening.group(1), depth))
            blocks.append([[name for name, _ in stack], 0])
        depth += line.count("{") - line.count("}")
        while stack and depth <= stack[-1][1]:
            stack.pop()
        if not opening and _is_substantive(line) and blocks:
            open_names = [name for name, _ in stack]
            for block in reversed(blocks):
                if block[0] == open_names:
                    block[1] += 1
                    break
    if not blocks:
        return None
    names, count = max(blocks, key=lambda block: block[1])
    parts = []
    for name in names:
        parts.extend(part for part in name.split("::") if part)
    return [part for part in parts if part != "Ubiquitty"], count


def substantive_counts(text):
    """Substantive declaration count per namespace block, in declaration order.

    A block's own lines only: a line inside a nested block is counted for that
    block, not for the one enclosing it, so an umbrella that contains a module
    does not inherit the module's weight.

    `namespaces_that_name` asks only whether a block has any such line, which
    makes a block holding one `constexpr` predicate weigh the same as one
    holding eight enums. Anything reasoning about which block NAMES a header
    needs the counts, not the booleans.
    """
    lines, blocks = _blocks(text)
    counts = []
    for index, (name, first, last) in enumerate(blocks):
        inner = [(f, l) for _, f, l in blocks[index + 1:] if f >= first and l <= last]
        own = 0
        for lineno in range(first, last):
            if any(f <= lineno < l for f, l in inner):
                continue
            if _is_substantive(lines[lineno]):
                own += 1
        counts.append((name, own))
    return counts
