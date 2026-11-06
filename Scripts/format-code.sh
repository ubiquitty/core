#!/usr/bin/env bash
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

#
# format-code.sh — apply repository clang-format style to owned C/H source.
#
# Usage:
#   ./Scripts/format-code.sh              format files in place
#   ./Scripts/format-code.sh --check      dry-run; exit 1 if anything would change
#   ./Scripts/format-code.sh --swift-only  the Swift half alone
#   ./Scripts/format-code.sh --help       show this help
#
# --swift-only exists because the two halves need different machines. The
# C-family half is pinned to one clang-format major and the Swift half needs
# swift-format, which ships with the Swift toolchain and is on no Linux runner
# here — so the job holding the pinned clang-format cannot check Swift, and the
# job holding a Swift toolchain has a clang-format of its own that would
# disagree with the pin. Each half runs where its formatter is.
#
# Owned surface: Source/, Tests/, Support/Host/ (C and H files only).
# Excludes generated files (emoji_atlas_data.c), build artifacts, baselines,
# Resource files and Markdown are not C and are not touched by clang-format.

set -euo pipefail

usage() {
    sed -n '/^#!/d; /^[^#]/q; s/^# \{0,1\}//; p' "$0"
}

CHECK_ONLY=0
SWIFT_ONLY=0
for arg in "$@"; do
    case "$arg" in
        --check) CHECK_ONLY=1 ;;
        --swift-only) SWIFT_ONLY=1 ;;
        --help|-h) usage; exit 0 ;;
        *) echo "format-code.sh: unknown argument: $arg" >&2; exit 2 ;;
    esac
done

# The tree formatted is the checkout holding this script, or the repository that
# carries it as a submodule: the pin and the rules are ubiquitty-core's, the files
# are the repository's own.
LIBRARY="$(cd "$(dirname "$0")/.." && pwd)"
ROOT="$(git -C "$LIBRARY" rev-parse --show-superproject-working-tree 2>/dev/null)"
[ -n "$ROOT" ] || ROOT="$LIBRARY"
cd "$ROOT"

# The C-family half runs only the pinned clang-format major. A different major
# reflows differently, so an off-pin run rewrites files the pre-commit hook and
# CI then judge against the pin; the hook names this script as the remedy for
# drift, which an off-pin formatter would turn into a whole-tree reformat. The
# search is the hook's, and the pin is read from the one file both read.
# --swift-only does not touch C-family files, so it needs no clang-format.
CLANG_FORMAT_PIN="$(tr -d '[:space:]' < "$LIBRARY/Scripts/clang-format-pin")"
CLANG_FORMAT="${CLANG_FORMAT:-}"
if [ "$SWIFT_ONLY" -eq 0 ]; then
    if [ -z "$CLANG_FORMAT" ]; then
        for candidate in "clang-format-$CLANG_FORMAT_PIN" clang-format; do
            command -v "$candidate" >/dev/null 2>&1 && { CLANG_FORMAT="$candidate"; break; }
        done
    fi
    if [ -z "$CLANG_FORMAT" ] && command -v xcrun >/dev/null 2>&1 \
            && xcrun -f clang-format >/dev/null 2>&1; then
        CLANG_FORMAT="$(xcrun -f clang-format)"
    fi
    CLANG_FORMAT_VERSION=""
    [ -n "$CLANG_FORMAT" ] && CLANG_FORMAT_VERSION="$("$CLANG_FORMAT" --version 2>/dev/null | head -1)"
    if ! printf '%s\n' "$CLANG_FORMAT_VERSION" | grep -qE "version $CLANG_FORMAT_PIN\."; then
        echo "format-code.sh: clang-format $CLANG_FORMAT_PIN required (have: ${CLANG_FORMAT:-none}${CLANG_FORMAT_VERSION:+, $CLANG_FORMAT_VERSION}); pip install 'clang-format==$CLANG_FORMAT_PIN.*'" >&2
        exit 3
    fi
fi

# The same set the pre-commit hook gates, derived from the same fact rather than
# restated: every tracked C-family file outside vendored ThirdParty. The hook
# judges STAGED paths with no directory allowlist at all, so any list here is a
# list that can be shorter than the one being enforced — and a file the hook
# gates but this script skips is one where the fix the hook names ("run
# ./Scripts/format-code.sh") does nothing and the commit is blocked with no way
# forward but --no-verify. A derived set cannot fall short of the hook's.
#
# `git ls-files` rather than `find`, so a build directory and an unconfigured
# checkout are outside the set without an exclusion to maintain — `--others
# --exclude-standard` keeps that property, because anything gitignored stays out.
#
# `--others` is what puts a file the tree does not track yet INSIDE the set. The
# hook judges staged paths, so a file being added is already in its subject
# while a tracked-only list has never heard of it: the author runs this script,
# is told the tree is clean, commits, and lands drift in the one kind of file
# that has never been formatted: a new one.
#
# Swift is formatted by swift-format against the repo's .swift-format, not by
# clang-format, which has no Swift language mode. Two formatters rather than one
# because the tree is two languages; the shape of the check is identical.
#
# Drift only, matching what this does for C-family. swift-format also lints —
# force-unwraps, naming, one-line doc summaries — and those are judgements the
# house conventions already rule on, in Docs/coding-conventions.md §3.4 and §2.
# A second opinion enforced by a tool would contradict the first wherever they
# disagree, and the one a reader is told to follow would lose.
#
# Objective-C++ is included — clang-format's ObjC language mode handles .mm/.h.
# The index still lists a file deleted in the working tree until the deletion is
# staged, and `git apply` of a patch that removes one leaves it exactly there.
# clang-format is then handed a path with nothing behind it, reports a diff
# against emptiness, and the run names a file that does not exist as needing
# formatting — which reads as this script being broken rather than as the tree
# being mid-apply. Ask the filesystem, which is what clang-format will do.
FILES=$(
    git ls-files --cached --others --exclude-standard -- '*.c' '*.cpp' '*.h' '*.hpp' '*.mm' \
        ':!:Source/ThirdParty/**' \
        ':!:Source/Core/Rendering/emoji_atlas_data.c' \
        | while IFS= read -r f; do [ -f "$f" ] && printf '%s\n' "$f"; done
)

# Swift's own list and formatter. Absent, this is a hard failure rather than a
# skip: a formatter that is not found and says nothing reports the tree clean
# without having read it, which is how drift lands with every gate green.
#
# The same selector as the C-family list above, and for the reason recorded
# there: the hook judges STAGED paths, so a file being ADDED is inside the
# enforcer's population and outside a tracked-only one. That file is the one most
# likely to be misformatted — it has never been through a formatter — and the one
# a clean run most confidently exonerates.
SWIFT_FILES=$(
    git ls-files --cached --others --exclude-standard -- '*.swift' \
        ':!:Source/ThirdParty/**' \
        | while IFS= read -r f; do [ -f "$f" ] && printf '%s\n' "$f"; done
)
SWIFT_COUNT=$(printf '%s\n' "$SWIFT_FILES" | grep -c . || true)
SWIFT_FORMAT="$(xcrun --find swift-format 2>/dev/null || command -v swift-format || true)"
# A machine without swift-format (every Linux box: it ships with the Swift
# toolchain) still gets its C-family verdict. Failing the whole run here made
# one missing tool block every push from that machine while the check it could
# run never looked; a gate whose tool is absent says SKIPPED and names what it
# did not see, and the C-half verdict stands on its own.
if [ -n "$SWIFT_FILES" ] && [ -z "$SWIFT_FORMAT" ]; then
    if [ "$SWIFT_ONLY" -eq 1 ]; then
        # Asked for this half alone and the formatter is absent: standing down
        # would be a green covering nothing at all, which is what --swift-only
        # exists to stop.
        echo "format-code.sh: FAILED — --swift-only asked for the Swift half and swift-format is not on this machine; $SWIFT_COUNT file(s) unchecked" >&2
        exit 1
    fi
    echo "format-code.sh: SKIPPED — swift-format not found, $SWIFT_COUNT Swift file(s) unchecked (C-family still checked)" >&2
fi

# --swift-only empties the C-family list rather than branching around the halves
# below: the counts, the verdict and the exit status are then the same code for
# both callers, and a half that examined nothing says so with its own count.
if [ "$SWIFT_ONLY" -eq 1 ]; then
    FILES=""
fi

if [ -z "$FILES" ] && [ "$SWIFT_ONLY" -eq 0 ]; then
    echo "format-code.sh: no candidate files found" >&2
    exit 0
fi

FILE_COUNT=$(printf '%s\n' "$FILES" | grep -c . || true)

if [ "$CHECK_ONLY" -eq 1 ]; then
    # In parallel, because this is one clang-format process per file and the
    # rewrite path below already batches through xargs. Serially it is one of
    # the larger items in the push gate; the work, the verdict and the drifting
    # set are identical either way, only the wall clock changes.
    #
    # Each worker prints only the names that drift, so the list is built from
    # output rather than from exit codes — xargs reports one status for the
    # whole run and cannot carry a per-file count.
    FORMAT_JOBS="${UBQ_FORMAT_JOBS:-$( (sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null) || echo 8 )}"
    # Guarded on the list being non-empty: `grep -v` finds nothing in an empty
    # one and exits 1, which under this file's pipefail ends the run with no
    # verdict at all — a half that examined nothing must say so, not vanish.
    DRIFTING_FILES=""
    if [ -n "$FILES" ]; then
        DRIFTING_FILES="$(printf '%s\n' "$FILES" | grep -v '^$' \
            | xargs -P "$FORMAT_JOBS" -I{} sh -c \
                '"$0" --dry-run --Werror "$1" >/dev/null 2>&1 || printf "%s\n" "$1"' \
                "$CLANG_FORMAT" {} | sort)"
    fi
    DRIFT_COUNT=$(printf '%s' "$DRIFTING_FILES" | grep -c . || true)
    [ -n "$DRIFTING_FILES" ] && DRIFTING_FILES="$DRIFTING_FILES"$'\n' 
    # Swift drift is a content comparison rather than a --dry-run flag:
    # swift-format has no check mode, so the formatted output is diffed against
    # the file. Same verdict, one process per file as above. Only with the
    # formatter present — its absence was reported as SKIPPED above.
    SWIFT_DRIFT=""
    SWIFT_DRIFT_COUNT=0
    if [ -n "$SWIFT_FORMAT" ] && [ -n "$SWIFT_FILES" ]; then
        SWIFT_DRIFT="$(printf '%s\n' "$SWIFT_FILES" | grep -v '^$' \
            | xargs -P "$FORMAT_JOBS" -I{} sh -c \
                '"$0" format --configuration .swift-format "$1" 2>/dev/null | diff -q - "$1" >/dev/null 2>&1 || printf "%s\n" "$1"' \
                "$SWIFT_FORMAT" {} | sort)"
        SWIFT_DRIFT_COUNT=$(printf '%s' "$SWIFT_DRIFT" | grep -c . || true)
    fi
    # Both languages are scanned before either reports. Failing at the C-family
    # list leaves the Swift files unread, so the count names one language's
    # drift while claiming the tree's: a person formats what they were shown,
    # re-runs, and is told about files the first verdict already knew nothing
    # about.
    if [ "$DRIFT_COUNT" -ne 0 ] || [ "$SWIFT_DRIFT_COUNT" -ne 0 ]; then
        if [ "$DRIFT_COUNT" -ne 0 ]; then
            echo "format-code.sh: $DRIFT_COUNT C-family file(s) need formatting:"
            printf '%s' "$DRIFTING_FILES" | sed 's/^/  /'
        fi
        if [ "$SWIFT_DRIFT_COUNT" -ne 0 ]; then
            echo "format-code.sh: $SWIFT_DRIFT_COUNT Swift file(s) need formatting:"
            printf '%s\n' "$SWIFT_DRIFT" | sed 's/^/  /'
        fi
        exit 1
    fi
    if [ "$SWIFT_ONLY" -eq 1 ]; then
        echo "format-code.sh: all $SWIFT_COUNT Swift file(s) already formatted (C-family not asked for)"
    elif [ -n "$SWIFT_FORMAT" ]; then
        echo "format-code.sh: all $FILE_COUNT C-family and $SWIFT_COUNT Swift file(s) already formatted"
    else
        echo "format-code.sh: all $FILE_COUNT C-family file(s) already formatted (Swift unchecked: swift-format not found)"
    fi
    exit 0
fi

if [ -n "$FILES" ]; then
    printf '%s\n' "$FILES" | xargs "$CLANG_FORMAT" -i
fi
if [ -n "$SWIFT_FORMAT" ]; then
    printf '%s\n' "$SWIFT_FILES" | grep -v '^$' \
        | xargs -I{} "$SWIFT_FORMAT" format --configuration .swift-format --in-place {}
    echo "format-code.sh: formatted $FILE_COUNT C-family and $SWIFT_COUNT Swift file(s)"
else
    echo "format-code.sh: formatted $FILE_COUNT C-family file(s) (Swift left untouched: swift-format not found)"
fi
