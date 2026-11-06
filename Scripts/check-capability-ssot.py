#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""check-capability-ssot.py — the capability vocabulary has one source of truth.

Three checks, all keyed on the enum itself rather than on a hand-written list:

  1. no drifting parallel allow-list — a switch keyed on Capability outside the
     two files sanctioned to hold one
  2. coverage — every capability is gated through check()/respond(), or named in
     UNGATED_ALLOWLIST with a reason
  3. tierOf() totality — every enumerator has an explicit case rather than
     riding the fail-closed default

  Scripts/check-capability-ssot.py
  Scripts/check-capability-ssot.py --staged

Exit 0 = clean, 1 = violations, 2 = the enum could not be parsed.
"""

import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import gate_corpus
import gate_report
import ubq_roots

GATE = "check-capability-ssot"

ENUM_HEADER = "Source/PAL/Capability.h"

# The two files sanctioned to key a table on the Capability enum. Both are the
# SSOT itself: Capability.h (tierOf) and the macOS consent copy. Any other file
# with `case Capability::` is a candidate parallel allow-list.
SSOT_SWITCH_FILES = (
    "Source/PAL/Capability.h",
    "Source/Core/Security/CapabilityConsentText.h",
)

# Capabilities not gated through check()/respond() at Core-emit, WITH the reason.
# Adding an entry is a security decision (it asserts the capability is gated
# elsewhere or does not escape the grid). Keep in sync with
# Docs/capability-host-policy.md.
#
#   pull-state  — Core only stores Terminal-internal state; the host/frontend
#                 pulls it and applies policy at the surface boundary. No
#                 synchronous host act happens at parse time.
#   rendering   — tier Rendering: grid content, not a host escape (tierOf spine).
#   reserved    — enum value declared for the full vocabulary; no handler emits
#                 it yet. When its handler lands it must gain a gate or move to
#                 pull-state here — this lint will flag it the moment it is wired
#                 without one only if it is REMOVED from this list, so a handler
#                 author must consciously reclassify it.
UNGATED_ALLOWLIST = (
    # pull-state (host/frontend pulls Terminal state and gates at the surface):
    "SetWindowTitle",    # OSC 0/1/2 — stored in oscTitle; Benign display string
    # OSC 9 stores bounded pane/tab presentation metadata, not an external act.
    # A Dock/taskbar writer must check ProgressIndicator at its effect boundary;
    # this entry does not permit an external writer to consume it without a gate.
    "ProgressIndicator",
    # rendering (grid content, not a host escape):
    "ImageRender",       # sixel / Kitty / iTerm2 inline image — tier Rendering
)

# The reserved subset: entries excused because nothing in Source/ emits them yet.
#
# That reason is a measurement, not a policy, and a measurement that a later
# commit can falsify: once an emitting handler lands, the reason has quietly
# expired, and the allowlist entry suppresses the only check that would say so.
#
# So the claim is re-derived here on every run rather than trusted. A capability
# named below that acquires any reference outside the enum's own header fails
# the gate: the entry is not wrong, it has expired, and what it excused now
# needs a decision.
# Empty: every entry above states a reason that does not depend on nothing
# referencing it. A capability named here is excused only while no handler
# emits it, so the claim is re-derived on every run and expires the moment one
# does — which is what happened to InputModeReport when the consent vocabulary
# gained a line for every enumerator.
RESERVED_ALLOWLIST: tuple[str, ...] = ()

ENUM_OPENS = re.compile(r"enum class Capability[ ]*:")
ENUMERATOR = re.compile(r"^[ \t]*([A-Z][A-Za-z0-9]*)[ \t]*,")
COMMENT = re.compile(r"//.*")

PARALLEL_SWITCH = re.compile(r"case[ ]+(Ubiquitty::)?Capability::")

# Five spellings ask the policy, and all five count. check() is the policy call
# itself; respond() and respondBulk() are the reply chokepoints that wrap it —
# the small buffer and the growable channel run the same suppression rule;
# effectClient() and effectAllowed() (Core/EffectGate.h) are the effect
# chokepoint, which reaches a frontend client only for a capability the caller
# names. A site converted from the bare check() idiom to the gate is still a
# site that asks, and a pattern that saw only the first two would report it as
# ungated — which is what this gate does when the tree changes shape rather than
# losing a check.
ASKS = re.compile(r"(check|respond|respondBulk|effectClient|effectAllowed)\(")
NAMES_CAPABILITY = re.compile(r"Capability::([A-Za-z0-9]+)")

TIEROF_OPENS = re.compile(r"constexpr CapabilityTier tierOf")


def note(text):
    print(text, file=sys.stderr)


def enumerators(text):
    """Every enumerator of the Capability enum, parsed from the enum itself."""
    found = []
    inside = False
    for line in text.split("\n"):
        if not inside:
            if ENUM_OPENS.search(line):
                inside = True
            continue
        if line.startswith("};"):
            break
        match = ENUMERATOR.match(COMMENT.sub("", line))
        if match:
            found.append(match.group(1))
    return found


def tierof_body(text):
    """The body of tierOf(), where every enumerator must have an explicit case."""
    lines = []
    inside = False
    for line in text.split("\n"):
        if TIEROF_OPENS.search(line):
            inside = True
        if inside:
            lines.append(line)
            if line.startswith("}"):
                break
    return "\n".join(lines)


def main(argv):
    if argv and argv[0] in ("--help", "-h"):
        print(__doc__.strip())
        return 0
    staged = bool(argv) and argv[0] == "--staged"
    if argv and not staged:
        print(f"usage: {os.path.basename(sys.argv[0])} [--staged]", file=sys.stderr)
        return 2

    os.chdir(gate_corpus.root())

    # In --staged mode, only run when a capability-relevant file is actually
    # staged; the checks are whole-tree by nature, so a no-op fast-exit keeps
    # unrelated commits cheap (mirrors how the other gates scope themselves).
    if staged:
        if not any(re.search(r"(Capability|capability)", path)
                   for path in gate_corpus.staged()):
            return 0

    # The enum is Core's; a repository that carries Core reads it there.
    enumPath = next((candidate for candidate in
                     (ENUM_HEADER, os.path.join(ubq_roots.core_root(os.getcwd()), ENUM_HEADER))
                     if os.path.isfile(candidate)), ENUM_HEADER)
    try:
        with open(enumPath, errors="replace") as handle:
            header = handle.read()
    except OSError:
        return gate_report.refused(
            GATE, "could not read the Capability enum", location=ENUM_HEADER)

    names = enumerators(header)
    if not names:
        return gate_report.refused(
            GATE, "could not parse any Capability enumerator", location=ENUM_HEADER)

    sources = [path for path in gate_corpus.tracked_and_untracked()
               if path.startswith("Source/")]
    bodies = dict(gate_corpus.texts(sources))
    # A capability is gated by a site in any repository: Core answers most, the
    # frontends gate their own effects. Coverage is judged over every checkout
    # this one reads; the parallel allow-list rule below judges this one's files.
    reach = dict(bodies)
    subject = os.getcwd()
    for library in dict.fromkeys((ubq_roots.core_root(subject), ubq_roots.kit_root(subject))):
        if os.path.realpath(library) == os.path.realpath(subject):
            continue
        listing = subprocess.run(["git", "-C", library, "ls-files", "Source/"], capture_output=True,
                                 text=True, check=False, env=ubq_roots.foreign_git_environment())
        for relative in listing.stdout.split("\n"):
            if relative and relative not in reach:
                try:
                    with open(os.path.join(library, relative), encoding="utf-8", errors="replace") as handle:
                        reach[relative] = handle.read()
                except OSError:
                    continue

    fail = False

    # --- check #1: no drifting parallel allow-list ---------------------------
    parallel = []
    for path in sorted(bodies):
        if path in SSOT_SWITCH_FILES:
            continue
        for number, line in enumerate(bodies[path].split("\n"), start=1):
            if PARALLEL_SWITCH.search(line):
                parallel.append((path, number, line.strip()))
    if parallel:
        note(f"{GATE}: parallel allow-list — a switch/table keyed on Capability")
        note(f"  outside the SSOT ({' '.join(SSOT_SWITCH_FILES)}). Route the "
             f"classification")
        note("  through tierOf() / the consent SSOT instead of a second allow-list:")
        for path, number, line in parallel:
            gate_report.finding(
                f"{path}:{number}", GATE,
                f"parallel Capability classification outside the SSOT: {line}")
        fail = True

    # --- check #2: coverage --------------------------------------------------
    gated = set()
    for text in reach.values():
        for line in text.split("\n"):
            if not ASKS.search(line):
                continue
            for name in NAMES_CAPABILITY.findall(COMMENT.sub("", line)):
                gated.add(name)

    ungated = [name for name in names
               if name != "None"
               and name not in gated
               and name not in UNGATED_ALLOWLIST]
    if ungated:
        note(f"{GATE}: un-gated capability — no check()/respond() site and")
        note("  not on the pull-state/non-escaping allowlist. Either gate the "
             "side effect")
        note("  through CapabilityPolicy::check() (or the respond() choke-point), "
             "or — if")
        note("  it is pull-state / grid-content — add it to UNGATED_ALLOWLIST "
             "with a reason")
        note("  and record it in Docs/capability-host-policy.md:")
        for name in ungated:
            gate_report.finding(
                ENUM_HEADER, GATE,
                f"Capability::{name} has no check/respond site and is not allowlisted")
        fail = True

    # Stale-allowlist belt: an allowlisted capability that has since grown a
    # gate, or that no longer exists in the enum, should be pruned so the list
    # keeps naming exactly the capabilities that reach a host effect with no gate
    # in front of them.
    for name in UNGATED_ALLOWLIST:
        if name not in names:
            gate_report.finding(
                __file__, GATE,
                f"allowlist entry {name} is not a Capability enumerator; prune it")
            fail = True
        elif name in gated:
            gate_report.finding(
                __file__, GATE,
                f"allowlist entry {name} now has a gate; remove it from UNGATED_ALLOWLIST")
            fail = True

    # The reserved entries' reason, re-derived: "nothing emits it yet" is
    # checkable, so it is checked rather than read. Any reference outside the
    # enum's own header means something now emits it, and the excuse is spent.
    for name in RESERVED_ALLOWLIST:
        token = f"Capability::{name}"
        emitters = sorted(path for path, text in reach.items()
                          if token in text and path != ENUM_HEADER)
        if emitters:
            note(f"{GATE}: allowlist entry '{name}' is excused as having no emitting")
            note("  handler, and now has one. The reason has expired: gate the "
                 "side effect, or")
            note("  move the entry to the pull-state group with a reason that is "
                 "still true.")
            for path in emitters:
                gate_report.finding(
                    path, GATE,
                    f"Capability::{name} is emitted despite its reserved allowlist reason")
            fail = True

    # --- check #3: tierOf totality -------------------------------------------
    body = tierof_body(header)
    missing = [name for name in names
               if not re.search(rf"case[ ]+(Ubiquitty::)?Capability::{name}[ ]*:",
                                body)]
    if missing:
        note(f"{GATE}: tierOf() is not total — these enumerators ride the")
        note("  fail-closed default instead of an explicit case (add each to "
             "tierOf):")
        for name in missing:
            gate_report.finding(
                ENUM_HEADER, GATE,
                f"Capability::{name} has no explicit tierOf case")
        fail = True

    if fail:
        note("")
        return gate_report.failed(GATE, "capability SSOT violated")

    # A silent success hides the allowlist from anyone who has not read this
    # file. The change that broke this gate had added a respond() gate to a
    # capability and never looked for the list recording its absence -- and
    # nothing it ran named that list. A pass that states its own population is a
    # pass you can size.
    counted = [name for name in names if name != "None"]
    gatedCount = sum(1 for name in counted if name in gated)
    allowlistedCount = sum(1 for name in counted
                           if name not in gated and name in UNGATED_ALLOWLIST)
    return gate_report.ok(
        GATE, f"{gatedCount + allowlistedCount} capabilities; {gatedCount} gated, "
        f"{allowlistedCount} allowlisted as ungated")


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
