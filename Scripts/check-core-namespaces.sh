#!/usr/bin/env bash
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

# check-core-namespaces.sh — a Core header declares the namespace its folder names.
#
# Source/Core/Screen/Marks.h belongs in Ubiquitty::Screen. Two ways it fails to
# be: declaring no nested namespace at all, which puts every name in the umbrella
# Ubiquitty:: alongside two hundred unrelated ones; or declaring some other name,
# which makes the folder and the namespace two independent filing systems a
# reader has to hold at once. Both cost the same thing — you cannot tell what a
# symbol is from where it lives, or find where it lives from what it is.
#
# The pairing is also what makes a module extractable. A folder whose contents
# share a namespace can move to its own repository as a unit; one whose contents
# name six namespaces has no boundary to cut along.
#
# The list is a ledger of known debt, not a permission slip. A header not on it
# must be correct, and a header ON it must still be wrong — an entry that has
# been fixed without being removed is a standing exemption for the next header
# to land in the same state.
#
# Headers directly in Source/Core/ are reported but not gated: they name no
# module, so there is no folder for a namespace to agree with. Giving them one
# is a question of which module they belong to, not of what to call them.
#
# A THIRD failure, worse than either and once indistinguishable from the first: a
# declaration outside every namespace block, in the true global one. The umbrella
# collides with two hundred sibling Core names; the global namespace collides with
# the platform SDK, with every library the frontends link, and with anything a
# consumer of this repository defines — and no `using` or alias moves a name out
# of it later. Counted and ratcheted below, both directions, and currently zero.

set -euo pipefail
ROOT="${1:-$(cd "$(dirname "$0")/.." && pwd)}"
cd "$ROOT"

# Headers that declare no namespace because they declare nothing a namespace can
# hold. A macro is not scoped by one, so wrapping these says nothing and implies
# a containment that does not exist. Permanent, and separate from the debt below
# so that list can reach zero.
EXEMPT="\
Source/Core/Codec/WuffsCodec.h                      macros pinning the vendored wuffs configuration
Source/Core/Diagnostics/PerfDiag.h                  macros the diagnostic build compiles away"

# Headers whose namespace differs from their folder, each carrying the reason the
# folder cannot name it. Permanent, and stale-checked like the debt below: an
# entry that becomes correct fails, so a decision that stops being true cannot
# sit here unread.
#
# path                                              what it declares, and why the folder cannot name it
DECIDED="\
Source/Core/Keys/KittyKeyboard.h                   Kitty — the kitty keyboard ENCODER, which needs Input/KeyEvent.h and calls Keymap::encodeKeyEvent as its fallback. Its parser-side sibling Core/Kitty/KeyboardState.h states the rule that keeps them apart: Core must not include Input/. Moving this file to Core/Kitty/ to match its namespace creates a Core/Input <-> Core/Kitty include cycle, measured with graph.sh (5 -> 6)."

# path                                              what it declares instead of its folder
KNOWN="\
Source/PAL/BellMode.h                              (none)
Source/PAL/Capability.h                            (none)
Source/PAL/TaskQueue.h                             (none)
Source/PAL/TerminalClients.h                       (none)
Source/PAL/TextShaping.h                           (none)"

# Headers that belong at Source/Core/ root, with the reason. These are the
# VOCABULARY the modules speak rather than a module of their own: every subsystem
# operates on a Cell, a Pen, a row view. `Ubiquitty::Image` and `Ubiquitty::VT`
# name things you can discuss separately; `Ubiquitty::Grid::Cell` would add a
# qualifier for filing and none for meaning, and it would put the model one level
# below the modules that exist to serve it.
#
# Stale-checked like every other ledger here: an entry naming a header that has
# moved fails, so this cannot quietly become a list of everything anyone left.
MODEL_ROOT="\
Source/Core/Cell.h                    the cell — what every module reads and writes
Source/Core/CellContent.h             a cell's payload, inline or interned
Source/Core/RunTextStore.h            the interned text a run's anchor cell names — cell payload, read by Screen, Selection and Sync
Source/Core/CellPen.h                 the pen a cell carries
Source/Core/CellScan.h                scanning a row of them
Source/Core/CellGrid.h                the grid of them, and the paint bookkeeping
Source/Core/Pen.h                     SGR state, 20 bytes, in every cell
Source/Core/RenderCell.h              a cell resolved for drawing
Source/Core/GridBounds.h              row/column bounds over that grid
Source/Core/CaseSensitivity.h         whether a match distinguishes case — search AND triggers ask it
Source/Core/TermPalette.h             the live colour palette — 35 consumers, the widest read in the tree
Source/Core/CommandLine.h             argv splitting; Triggers is L1 and a Runtime home would be L2
Source/Core/SGR.h                     byte-hot parse path; a namespace move renames the mangled symbol
Source/Core/LineStore.h               the scrollback the grid spills into
Source/Core/LineAttributeView.h       DECDWL/DECDHL per row
Source/Core/LineWrappedView.h         soft-wrap joins per row
Source/Core/RowMeta.h                 per-row model state
Source/Core/RowView.h                 a row, addressed
Source/Core/RowMarkView.h             OSC 133 marks per row
Source/Core/PaintRun.h                one same-pen run the painter walks
Source/Core/PaintSession.h            a frame's walk over those runs
Source/Core/ClusterPool.h             grapheme clusters a cell cannot hold inline
Source/Core/EffectGate.h              how any module reaches a frontend client at all
Source/Core/TextSizeTable.h           text-size specs a cell cannot hold inline
Source/Core/StrongIndex.h             Row and Column, so the two never swap
Source/Core/ReplyBuilder.h            the reply buffer OSC and VT both answer through; either module owning it makes the other reach across
Source/Core/BuildVersion.h            the build's version, which VT's and OSC's reports and Sync's handshake all state; Sync is L2 and VT cannot reach it
Source/Core/Color.h                   the colour vocabulary the pen indexes into
Source/Core/PaletteRGB.h              a resolved palette entry
Source/Core/CellColor.h               resolves a pen against a palette; a theme produces one, it is not one
Source/Core/Terminal.h                the object all of the above hangs from
Source/Core/TerminalOperations.h      its public surface
Source/Core/TerminalShared.h          what the terminal-wide seam needs
Source/Core/Terminfo.h                the capability lookup — Core/Terminfo/ is L2 behaviour and this is L1 model, so one folder cannot hold both
Source/Core/TerminfoSource.h          the source FORMAT the lookup above decodes: pure functions of the text, beside the header whose entry they are applied to"

# Root headers that are not vocabulary — each belongs in a module and has not been
# moved yet. The budget is two-sided so this can only fall: a new header at root
# fails immediately rather than joining the list, and the commit that empties one
# has to write the smaller number down.
#
# Every one of these is in the `Ubiquitty` umbrella today, so moving it into a
# folder is also a namespace change — which is why they accumulated. That makes
# each move a rename across its consumers, not a `git mv`.
ROOT_STRAY_BUDGET=0

# Declarations Core puts in the TRUE global namespace — outside every namespace
# block, not merely outside a module's. Zero, and the budget is two-sided, so the
# next one to arrive fails rather than joining a backlog.
# Twelve: the retain/release pairs for GlyphSlotCache, FrameCapture,
# TerminalSession, WorkspaceSession and HostedAppRuntime. HostedAppRuntime is
# header-only, so its pair is declared ahead of the class and defined inline
# after it, and counts as four declarations. The Swift importer resolves the
# functions named by SWIFT_SHARED_REFERENCE at global scope and nowhere else —
# a namespace-scoped pair is reported as "cannot find retain function", which
# reads like a missing declaration rather than a scope rule. Moving them into
# a namespace means dropping the annotation.
#
# Two-sided: a thirteenth global declaration fails, and so does removing one
# of these twelve.
GLOBAL_SCOPE_BUDGET=12

KNOWN="$KNOWN" DECIDED="$DECIDED" EXEMPT="$EXEMPT" MODEL_ROOT="$MODEL_ROOT" ROOT_STRAY_BUDGET="$ROOT_STRAY_BUDGET" GLOBAL_SCOPE_BUDGET="$GLOBAL_SCOPE_BUDGET" python3 - <<'PY'
import os
import re
import sys
import pathlib

root = pathlib.Path(".")
core = root / "Source" / "Core"

# The classifier lives in Scripts/core_namespaces.py so a probe measuring a
# proposed change to this rule runs the same code the rule runs. Reimplementing
# it to measure it answers about the reimplementation, and one that disagrees
# with this on any header makes the comparison meaningless.
sys.path.insert(0, str(root / "Scripts"))
from core_namespaces import NAMESPACE, FORWARD_ONLY, heaviest_namespace_path  # noqa: E402

# A declaration at brace depth 0 — outside every namespace block, in the TRUE
# global namespace. Distinct from and worse than sitting in the `Ubiquitty::`
# umbrella, which is what the check below measures. Reported identically as
# "(none)", a header that opens no namespace at all would read as a mild filing
# complaint rather than as a header putting every name it declares in the
# global one.
#
# Ratcheted on a count rather than a path list: a path list is a standing
# exemption for every entry on it, which is what the KNOWN ledger above exists
# to avoid, while a number that may only fall cannot exempt anything.
DECLARATION = re.compile(
    r'^\s*(?:template\s*<[^>]*>\s*)?'
    r'(?:constexpr|inline|static|extern|using|enum\s+class|enum|struct|class|union|typedef)\b')


# Braces and quoted text, so depth counts real nesting. Reading `}` at column 0
# as a namespace close instead breaks on a struct or enum whose `};` sits at
# column 0 — which is every one of them in this house style — dropping the depth
# to zero and making the whole remainder of the file read as global, so headers
# that sit inside a namespace block are named as offenders.
BRACES = re.compile(r'//.*$|/\*.*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])\'')


def global_scope_declarations(text):
    """Line numbers of declarations sitting outside every namespace block.

    A forward declaration is a mention of someone else's type rather than this
    header's own, so it does not count — the same distinction `FORWARD_ONLY`
    draws above, for the same reason.
    """
    depth, open_namespaces, found = 0, [], []
    for number, line in enumerate(text.splitlines(), 1):
        code = BRACES.sub("", line)
        opening = NAMESPACE.match(line)
        stripped = line.lstrip()
        # `depth == 0` as well as no open namespace, so a `static constexpr`
        # member of a global struct does not count. Such a member is reachable
        # only through the struct; what pollutes the global namespace is the
        # struct itself, once, and counting each of its members as an offence
        # both inflates the number and points at the wrong thing to fix.
        if (not open_namespaces and depth == 0
                and not stripped.startswith(("//", "#", "*", "/*"))
                and DECLARATION.match(line)
                and not FORWARD_ONLY.match(re.sub(r'//.*$', '', line))):
            found.append(number)
        if opening:
            open_namespaces.append(depth)
        depth += code.count("{") - code.count("}")
        while open_namespaces and depth <= open_namespaces[-1]:
            open_namespaces.pop()
    return found


def ledger(name):
    entries = {}
    for line in os.environ[name].splitlines():
        if not line.strip():
            continue
        path, note = line.split(None, 1)
        entries[path] = note.strip()
    return entries

GLOBAL_SCOPE_BUDGET = int(os.environ["GLOBAL_SCOPE_BUDGET"])

known = ledger("KNOWN")
decided = ledger("DECIDED")
exempt = ledger("EXEMPT")
modelRoot = ledger("MODEL_ROOT")
ROOT_STRAY_BUDGET = int(os.environ["ROOT_STRAY_BUDGET"])

# Source/PAL as well as Source/Core, because the rule is about a folder naming
# its namespace and PAL is a folder. A walk rooted at Source/Core alone leaves
# `Ubiquitty::PAL` being the OS seam's namespace enforced by nothing but habit,
# and a PAL header can put its names in the umbrella instead.
#
# PAL differs from Core in one way that matters. A header directly in
# Source/Core/ names no module, so it is reported and not gated. A header
# directly in Source/PAL/ does name one — PAL itself — so the module for
# anything under that root is the root's own name plus whatever nests below it.
ROOTS = [(core, False), (root / "Source" / "PAL", True)]

bad, rooted, strays, globals_by_header = {}, [], [], {}
for scan_root, root_names_a_module in ROOTS:
  for header in sorted(scan_root.rglob("*.h")):
    relative = header.relative_to(root).as_posix()
    text = header.read_text(errors="replace")
    at_global = global_scope_declarations(text) if scan_root is core else []
    if at_global:
        globals_by_header[relative] = at_global
    # The block that NAMES the header is the one holding the most of its own
    # declarations, not the first one opened. Position accepted
    # Core/Modes/TerminalModes.h into Core/Modes/ on a block holding one constexpr
    # predicate, while the enums the header exists for — 19 of them — sat in the
    # umbrella. Weight says umbrella, which is where it belongs.
    heaviest = heaviest_namespace_path(text)
    nested = heaviest[0] if heaviest else []
    folder = header.parent.relative_to(scan_root).as_posix()

    if folder == "." and not root_names_a_module:
        # A header at the module root names no folder, so there is no namespace
        # for it to agree with — but "no rule applies" is not the same as "this
        # is fine", and reporting them without judging them is how 48 of them
        # arrived. Vocabulary is listed and accepted; anything else is debt.
        (rooted if relative in modelRoot else strays).append(relative)
        continue

    parts = [scan_root.name] if root_names_a_module and folder == "." else folder.split("/")
    if root_names_a_module and folder != ".":
        parts = [scan_root.name] + parts
    # The FULL folder path, not its leaf. Accepting the leaf too lets a nested
    # module skip its parent — `Image/Sixel/Decoder.h` declaring `Ubiquitty::Sixel`
    # passed for exactly that reason, putting a decoder in the umbrella beside two
    # hundred unrelated names while the check reported it correct. Measured before
    # removing the arm: 26 nested-folder headers match the full path and 2 passed
    # only via the leaf, and both of those were the defect, so it excused nothing.
    # A header nesting DEEPER than its folder is still fine.
    if nested[: len(parts)] == parts:
        continue
    if relative in exempt:
        continue
    bad[relative] = ("::".join(nested) if nested else "(none)", "::".join(parts))

accepted = {**known, **decided}
new = {p: d for p, d in bad.items() if p not in accepted}
stale = [p for p in accepted if p not in bad]

if new:
    names = sorted(new)
    print("check-core-namespaces: FAILED — %d header(s) do not declare the namespace their folder names: %s"
          % (len(names), ", ".join(names[:3]) + (" (+%d more)" % (len(names) - 3) if len(names) > 3 else "")))
    for path, (declares, want) in sorted(new.items()):
        print(f"  {path}\n      declares {declares}, folder names Ubiquitty::{want}")
    print("\n  Put it in its module's namespace. Adding it to this script's KNOWN list is")
    print("  for pre-existing debt only — a new header has no reason to arrive wrong.")

if stale:
    print("check-core-namespaces: FAILED — %d KNOWN/DECIDED entry(s) name a header that is already correct: %s"
          % (len(stale), ", ".join(stale[:3]) + (" (+%d more)" % (len(stale) - 3) if len(stale) > 3 else "")))
    for path in sorted(stale):
        print(f"  {path}")
    print("\n  Remove it from its list in the commit that fixed it. An entry left behind is a")
    print("  standing exemption for the next header to land in the same state.")

staleModelRoot = [pathName for pathName in modelRoot if pathName not in rooted]
if staleModelRoot:
    names = sorted(staleModelRoot)
    print("check-core-namespaces: FAILED — %d MODEL_ROOT entry(s) name a header no longer at the root: %s"
          % (len(names), ", ".join(names[:3]) + (" (+%d more)" % (len(names) - 3) if len(names) > 3 else "")))
    for pathName in sorted(staleModelRoot):
        print("  %s" % pathName)
    print("\n  Remove it in the commit that moved it. An entry left behind accepts the next")
    print("  header to land at the root under a name nobody chose.")

if len(strays) > ROOT_STRAY_BUDGET:
    names = sorted(strays)
    print("check-core-namespaces: FAILED — %d root header(s) name no module, over the budget of %d: %s"
          % (len(names), ROOT_STRAY_BUDGET,
             ", ".join(names[:3]) + (" (+%d more)" % (len(names) - 3) if len(names) > 3 else "")))
    for pathName in sorted(strays)[:10]:
        print("  %s" % pathName)
    print("\n  A header at Source/Core/ root belongs to no module. Move it into the one it")
    print("  serves — which is also a namespace change, since these sit in the umbrella —")
    print("  or add it to MODEL_ROOT with the reason it is vocabulary rather than a module.")

if len(strays) < ROOT_STRAY_BUDGET:
    print("check-core-namespaces: FAILED — root strays are down to %d, under the budget of %d."
          % (len(strays), ROOT_STRAY_BUDGET))
    print("\n  Set ROOT_STRAY_BUDGET=%d in this script, in the commit that earned it." % len(strays))

global_headers = len(globals_by_header)
global_declarations = sum(len(v) for v in globals_by_header.values())

if global_declarations > GLOBAL_SCOPE_BUDGET:
    print(f"check-core-namespaces: FAILED — {global_declarations} declarations sit in the TRUE "
          f"global namespace, over the budget of {GLOBAL_SCOPE_BUDGET}:")
    for path, at in sorted(globals_by_header.items(), key=lambda row: -len(row[1]))[:8]:
        print(f"  {len(at):>4}  {path}:{at[0]}")
    print("\n  Not the Ubiquitty:: umbrella — outside every namespace, where a name")
    print("  collides with the platform SDK and with every library the frontends link.")
    print("  Put the new declarations in a namespace, or lower the budget in this script")
    print("  if you moved some out.")

# Two-sided for the same reason KNOWN is: a budget with slack in it is an
# allowance the next change can spend without anyone deciding to. The commit that
# earns the lower number is the one that has to write it down.
if global_declarations < GLOBAL_SCOPE_BUDGET:
    print(f"check-core-namespaces: FAILED — global scope is down to {global_declarations}, "
          f"under the budget of {GLOBAL_SCOPE_BUDGET}.")
    print(f"\n  Set GLOBAL_SCOPE_BUDGET={global_declarations} in this script, in the commit that")
    print("  earned it. Slack left in the budget is room the next header can fill for free.")

if new or stale or staleModelRoot or len(strays) != ROOT_STRAY_BUDGET or global_declarations != GLOBAL_SCOPE_BUDGET:
    sys.exit(1)

print(f"check-core-namespaces: PASSED — {len(known)} known, {len(decided)} decided, 0 new "
      f"({len(exempt)} exempt)")
print(f"  Source/Core/ root: {len(rooted)} vocabulary headers accepted, {len(strays)} strays "
      f"awaiting a module (budget {ROOT_STRAY_BUDGET}; ratchet down, never up)")
print(f"  global scope: {global_declarations} declarations across {global_headers} headers "
      f"(budget {GLOBAL_SCOPE_BUDGET}; ratchet down, never up)")
PY
