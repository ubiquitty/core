#!/usr/bin/env bash
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

# check-core-layer-partition.sh — the L0/L1/L2 partition of the portable-core
# source list, classified by the rules in Docs/package-layers.md.
#
# Scripts/portable-core-sources.txt stays the single source of truth for what
# the core lib compiles; this checker classifies every SSOT line into its
# package layer by rule (module prefix + three exact Platform paths), so there
# is no second file list to drift. The partition must be TOTAL and UNAMBIGUOUS:
# an SSOT line matching no rule (a new module, a new Protocol/ subtree, a new
# Platform TU) FAILS here until Docs/package-layers.md assigns it a layer —
# that is the guard, not an inconvenience.
#
# Layer rules (Docs/package-layers.md):
#   L0  foundation + PAL contract:  Support/ (no TUs in the SSOT), and the
#       portable PAL sources: PAL/PAL.cpp, PAL/TaskQueue.cpp, PAL/OutputQueue.cpp,
#       PAL/Process.cpp, PAL/ProcessSignal.cpp and PAL/RemoteTarget.cpp.
#   L1  model:      Core/ Unicode/ Codec/ Image/ Rendering/ Protocol/Tmux/
#                   Diagnostics/ Ubiquitty/
#   L2  behaviour:  App/ Preferences/ Themes/ Terminfo/ Input/ Runtime/
#                   Selection/ Protocol/Sync/ Transport/ Network/ Session/
#   L3b bundled backend: Platform/POSIX/ and the Platform/Wasm/ TUs that
#       replace it — architecturally backends (L3), bundled into the core lib as
#       the default PAL; kept distinct here so the model/behaviour split never
#       counts them as model code.
#       Both spellings are classified because
#       Scripts/cmake/UbiquittyCoreSources.cmake swaps one set for the other,
#       and the partition is checked against whichever the target builds.
#
# There is no generic Protocol/ rule, because Protocol is split across layers
# (Tmux=L1, Sync=L2): a catch-all would file a new Protocol/<other>/ TU under
# whichever layer it named, so each must be classified explicitly instead.
#
# Usage:
#   Scripts/check-core-layer-partition.sh              verify; per-layer counts
#   Scripts/check-core-layer-partition.sh --emit L1    print one layer's TUs
#   Scripts/check-core-layer-partition.sh --sources F  override the SSOT (tests)

set -uo pipefail
cd "$(dirname "$0")/.." || exit 1

SOURCES="Scripts/portable-core-sources.txt"
EMIT=""
EMIT_MODULES=""
EMIT_FILES=""
while [ $# -gt 0 ]; do
    case "$1" in
        --help|-h) sed -n '/^#!/d; /^[^#]/q; s/^# \{0,1\}//; p' "$0"; exit 0 ;;
        --emit) EMIT="${2:?--emit needs L0|L1|L2|L3b}"; shift 2 ;;
        --emit-modules) EMIT_MODULES="${2:?--emit-modules needs L1|L2}"; shift 2 ;;
        --emit-files) EMIT_FILES="${2:?--emit-files needs L0|L1|L2|L3b}"; shift 2 ;;
        --sources) SOURCES="${2:?--sources needs a path}"; shift 2 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

# The module prefixes that DEFINE each layer, relative to Source/. This is the
# single source of truth for layer membership: classify() below assigns TUs from
# them, and Scripts/cmake/UbiquittyCoreLibrary.cmake reads them back through
# --emit-modules to stage the layer headers. Two copies drift: a pair that
# matches behaviour and model in opposite orders stages the headers correctly
# while misclassifying the TUs, and the L1/L2 split goes unenforced.
#
# Behaviour is matched before model wherever these are used, and the ordering is
# not cosmetic: `Core` is a model prefix and every behaviour subtree lives under
# it, so matching model first swallows all of them.
BEHAVIOUR_MODULES="Core/App Core/Preferences Core/Themes Core/Terminfo Core/Input
Core/Runtime Core/Selection Core/Transport Core/Network Core/Session
Core/Protocol/Sync Core/Protocol/Local Core/Protocol/Control Core/Security"
MODEL_MODULES="Base UbiquittyCore Core Core/Unicode Core/Codec Core/Image
Core/Rendering Core/Diagnostics Core/Protocol/Tmux"

if [ -n "$EMIT_MODULES" ]; then
    case "$EMIT_MODULES" in
        L1) printf '%s\n' $MODEL_MODULES ;;
        L2) printf '%s\n' $BEHAVIOUR_MODULES ;;
        *) echo "--emit-modules takes L1 or L2" >&2; exit 2 ;;
    esac
    exit 0
fi

[ -f "$SOURCES" ] || { echo "no SSOT at $SOURCES" >&2; exit 2; }

# A module prefix that names nothing matches nothing, and a prefix that matches
# nothing is indistinguishable from one whose subtree is clean. The TU side of
# this is already loud — a path no prefix claims lands in UNCLASSIFIED and fails
# — but the header staging reads these same lists through --emit-modules, and
# there a dead name stages an empty set without complaint. So the list is
# checked against the tree before it is used to judge anything.
#
# This fails at the moment a module moves rather than at the moment someone
# relies on the layer it stopped defining.
for _module in $BEHAVIOUR_MODULES $MODEL_MODULES; do
    [ -d "Source/$_module" ] || {
        echo "layer module names a directory that does not exist: Source/$_module" >&2
        exit 2
    }
done

# classify <path> -> layer name on stdout; empty when no rule matches.
#
# The two prefix loops walk the prefixes spelled once above, which keeps this in
# step with the header staging by construction rather than by everyone
# remembering to edit both.
classify() {
    case "$1" in
        Source/PAL/PAL.cpp|Source/PAL/TaskQueue.cpp|Source/PAL/OutputQueue.cpp|Source/PAL/Process.cpp|Source/PAL/ProcessSignal.cpp|Source/PAL/RemoteTarget.cpp) echo "L0"; return ;;
        Source/Platform/POSIX/*|Source/Platform/Wasm/*) echo "L3b"; return ;;
        # Composes the screen clear with Core/Selection, which is L2, so it is
        # behaviour despite sitting beside the model operations in Core/Screen.
        # Its own header says a frontend that reproduces four of the five steps
        # looks correct, which is why the composition lives here rather than
        # being pushed back to the caller to keep the module boundary tidy. A
        # model classification puts it in an archive the linker scans before the
        # one defining Selection::clear, which only GNU ld reports.
        Source/Core/Screen/ClearBuffer.cpp) echo "L2"; return ;;
    esac
    for _module in $BEHAVIOUR_MODULES; do
        case "$1" in "Source/$_module/"*) echo "L2"; return ;; esac
    done
    for _module in $MODEL_MODULES; do
        case "$1" in "Source/$_module/"*) echo "L1"; return ;; esac
    done
    echo ""
}

# Every file classify() assigns to a layer, headers included.
#
# --emit-modules hands out prefixes, which is enough to stage headers but not to
# judge them: a caller that turns a prefix back into files has to decide for
# itself how deep to look, and the depth that keeps `Core` from swallowing
# `Core/Preferences` also keeps it from opening `Core/Screen`. Emitting the files
# themselves moves that decision here, where the per-file exceptions already
# live, so a rule cannot disagree with the partition about what a layer contains.
if [ -n "$EMIT_FILES" ]; then
    case "$EMIT_FILES" in
        L0|L1|L2|L3b) ;;
        *) echo "--emit-files takes L0, L1, L2 or L3b" >&2; exit 2 ;;
    esac
    find Source -type f \( -name '*.h' -o -name '*.cpp' \) | sort | while IFS= read -r _path; do
        [ "$(classify "$_path")" = "$EMIT_FILES" ] && printf '%s\n' "$_path"
    done
    exit 0
fi

FAIL=0
UNCLASSIFIED=""
COUNT_L0=0; COUNT_L1=0; COUNT_L2=0; COUNT_L3B=0
TOTAL=0
while IFS= read -r line; do
    case "$line" in ''|\#*) continue ;; esac
    TOTAL=$((TOTAL + 1))
    layer="$(classify "$line")"
    case "$layer" in
        L0)  COUNT_L0=$((COUNT_L0 + 1)) ;;
        L1)  COUNT_L1=$((COUNT_L1 + 1)) ;;
        L2)  COUNT_L2=$((COUNT_L2 + 1)) ;;
        L3b) COUNT_L3B=$((COUNT_L3B + 1)) ;;
        *)   UNCLASSIFIED="$UNCLASSIFIED$line"$'\n'; FAIL=1 ;;
    esac
    if [ -n "$EMIT" ] && [ "$layer" = "$EMIT" ]; then
        echo "$line"
    fi
done < "$SOURCES"

if [ -n "$EMIT" ]; then
    [ "$FAIL" -eq 0 ] || { echo "check-core-layer-partition: SSOT has unclassified lines" >&2; exit 1; }
    exit 0
fi

echo "portable-core layer partition: total=$TOTAL  L0=$COUNT_L0  L1=$COUNT_L1  L2=$COUNT_L2  L3b=$COUNT_L3B"
if [ "$FAIL" -ne 0 ]; then
    echo "FAIL: SSOT lines with NO layer rule — assign a layer in Docs/package-layers.md"
    echo "and add the rule here (a new module/subtree must be classified, never implied):"
    printf '%s' "$UNCLASSIFIED" | sed 's/^/  /'
    exit 1
fi
PARTITION_SUM=$((COUNT_L0 + COUNT_L1 + COUNT_L2 + COUNT_L3B))
if [ "$PARTITION_SUM" -ne "$TOTAL" ]; then
    echo "FAIL: partition sum $PARTITION_SUM != SSOT total $TOTAL"
    exit 1
fi
# The per-target trims are the other list that can drift from the tree. This
# file's whole argument is that there must be no second list maintained by
# hand; ubiquitty_core_subset(... EXCLUDE ...) is one, and it names paths.
python3 Scripts/check-core-subset-trims.py . || exit 1

# run-tests.sh's CORE_SRCS is the third such list: the subset the unit tests
# link to drive a headless Terminal, a hand-edited bash array. It partitions the
# same SSOT against Scripts/core-srcs-excluded.txt, so a Core source added to the
# tree lands in one list or the other rather than in a silent link break.
python3 Scripts/check-core-srcs-complete.py . || exit 1

echo "check-core-layer-partition.sh: PASSED (L0+L1+L2+L3b == SSOT, no unclassified TU)"
