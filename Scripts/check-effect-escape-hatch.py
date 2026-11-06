#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""check-effect-escape-hatch — count every use of the capability escape hatch.

EffectGate.h routes an effect to its frontend client through a policy check,
except one door: clientForEffectThatEscapesNothing returns the client without
consulting the policy, for an effect that reveals no host state and changes
none. Its own comment says a use of it "for anything that touches the clipboard,
the filesystem, the window or a notification is a bug that review has to catch,
because the compiler will not" — the one remaining hole in a mechanism that is
otherwise compile-enforced. The name is long and unlike anything else so grep
finds every use; nothing was counting them.

This does. Each use is recorded here with the client method it calls and why
that method escapes nothing, and a use whose (file, method) is not on the list
fails the gate — a new door into the escape hatch earns the review the compiler
will not give it. The list is held to the tree in both directions: a use that
leaves forces its line out too, so the record cannot drift into a stale allowlist
naming methods no call reaches.

The bar a new entry must clear, read from the four that are here: a method either
reveals and changes nothing past the rendered screen (invalidate — a repaint), or
its capability was already consulted at the call site before the client was
reached (resize behind WindowMutate, setDisplayPower behind DisplayPower — the
escape hatch is the client access, not the gate), or it is an internal hint the
frontend acts on defensively with no capability of its own (setPasswordPromptActive).
A method that writes the clipboard, reads a file or pushes a notification clears
none of these and is the bug the header names.
"""

import pathlib
import re
import sys

sys.path.insert(0, str(pathlib.Path(__file__).parent))
import gate_report

HATCH = re.compile(r"\bclientForEffectThatEscapesNothing\s*\(")
# The client method called on the value the hatch returns, on the use line or
# just below it: `... = clientForEffectThatEscapesNothing(...)) c->METHOD(`.
METHOD = re.compile(r"->\s*(\w+)\s*\(")

# The files that define the hatch, not call it — its declaration and its inline
# body name the selector without being a use.
DEFINITION_FILES = {
    "Source/Core/EffectGate.h",
    "Source/Core/Terminal.h",
}

# Every recorded use, (relative path, client method). A use not here fails; an
# entry here that no use reaches fails too.
RECORDED = {
    ("Source/Core/DragDrop/Client.cpp", "sendCommand"),  # revocation only: close grants and release owned resources, even after denial
    ("Source/Core/OSC/Color.cpp", "invalidate"),        # OSC colour change -> repaint
    ("Source/Core/OSC/Notify.cpp", "invalidate"),       # progress + tab-status -> repaint (gated above)
    ("Source/Core/OSC/Notify.cpp", "reportProgress"),   # decoded metadata only; external taskbar consumers check ProgressIndicator
    ("Source/Core/VT/CSI.cpp", "resize"),               # CSI 8 t -> WindowMutate checked at the case
    ("Source/Core/VT/CSI.cpp", "setDisplayPower"),      # CSI 13 ] -> DisplayPower checked at the case
    ("Source/Core/VT/Init.cpp", "invalidate"),          # reset -> repaint
    ("Source/Core/VT/VT.cpp", "setPasswordPromptActive"),  # heuristic hint, no capability of its own
}

SCAN_ROOTS = ["Source/Core"]


def methods_in(lines):
    """The (line-1-indexed, method) for each hatch use in one file's lines.

    The method is the first `->name(` at or below the use, within a few lines —
    the hatch's result is dereferenced immediately at every real site.
    """
    uses = []
    for index, line in enumerate(lines):
        if not HATCH.search(line):
            continue
        method = None
        for follow in lines[index:index + 4]:
            match = METHOD.search(follow)
            if match:
                method = match.group(1)
                break
        uses.append((index + 1, method))
    return uses


def _source_files():
    import os
    for root in SCAN_ROOTS:
        for base, _, names in os.walk(root):
            for name in names:
                if name.endswith((".cpp", ".mm", ".h")):
                    path = os.path.join(base, name)
                    if path not in DEFINITION_FILES:
                        yield path


def observed_pairs():
    pairs = set()
    for path in _source_files():
        with open(path, encoding="utf-8") as handle:
            lines = handle.read().splitlines()
        for _, method in methods_in(lines):
            pairs.add((path, method or "?"))
    return pairs


def selfTest():
    seeded = [
        "if (auto* c = Ubiquitty::clientForEffectThatEscapesNothing(t.windowClient))",
        "    c->invalidate();",
        "if (auto* c = Ubiquitty::clientForEffectThatEscapesNothing(t.windowClient)) {",
        "    c->resize(a, b);",
        "}",
    ]
    got = methods_in(seeded)
    ok = got == [(1, "invalidate"), (3, "resize")]
    print("  %s the client method is read off each use" % ("ok " if ok else "FAIL"))
    print("check-effect-escape-hatch: self-test %s" % ("ok" if ok else "FAILED"))
    return 0 if ok else 1


def main():
    if "--self-test" in sys.argv:
        return selfTest()

    observed = observed_pairs()
    added = sorted(observed - RECORDED)
    gone = sorted(RECORDED - observed)

    # The verdict names the first offender on its own line rather than deferring
    # to the list below it, so a reader who sees only the summary learns a site.
    status = 0
    if added:
        status = gate_report.failed("check-effect-escape-hatch",
            "escape-hatch use(s) not recorded", ["%s -> %s" % pair for pair in added])
        for path, method in added:
            print("    %s -> %s: judge whether it escapes nothing, gate its capability at the"
                  " call site, or record it here with why" % (path, method), file=sys.stderr)
    if gone:
        status = gate_report.failed("check-effect-escape-hatch",
            "recorded use(s) no call reaches — delete", ["%s -> %s" % pair for pair in gone])
        for path, method in gone:
            print("    %s -> %s" % (path, method), file=sys.stderr)
    if status:
        return status

    print("check-effect-escape-hatch: ok (%d recorded escape-hatch use(s), each accounted for)"
          % len(RECORDED))
    return 0


if __name__ == "__main__":
    sys.exit(main())
