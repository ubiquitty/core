#!/bin/bash
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

# identify-artefact.sh — say which commit a file's CONTENT is, or that it is not
# committed at all.
#
#   ./Scripts/identify-artefact.sh <path> [<path> ...]
#
# A finding cites an artefact; a reader opens a path. Those are the same thing
# right up until a working tree diverges — and then a measurement gets attributed
# to a commit that never produced it. A checksum does not help: taking it twice
# proves the same file was read twice, not which file it is.
#
# The question this answers is what the content is, via `git log --find-object`,
# rather than what the path currently holds. A build artefact restored from an
# old commit belongs to that old commit however recently it was written, and an
# mtime would call it "today".
#
# Every git call passes --literal-pathspecs. A path is not a pattern: git matches
# pathspecs with wildmatch, so a file named `weird[1].txt` also selects
# `weird1.txt`, ls-files returns two lines, and the later lookups get one mangled
# argument and find nothing — reporting a committed file as produced locally.
# Confident, wrong, exit 0, which is what this script exists to refuse.
#
# Exit 0 when every path matches HEAD, 1 when any is uncommitted or untracked.
# Made for binary artefacts nobody can read by eye — profiles, atlases, golden
# images — where a wrong file looks exactly like a right one.
set -uo pipefail

[ $# -ge 1 ] || { echo "identify-artefact: need at least one <path>" >&2; exit 2; }

status=0

for file in "$@"; do
    if [ ! -f "$file" ]; then
        echo "identify-artefact: no such file: $file" >&2
        status=2
        continue
    fi

    blob="$(git hash-object "$file")"
    tracked="$(git --literal-pathspecs ls-files --full-name -- "$file" 2>/dev/null)"

    printf 'path   %s\n' "$file"
    printf 'blob   %s\n' "$blob"
    printf 'mtime  %s\n' "$(stat -f '%Sm' "$file" 2>/dev/null || stat -c '%y' "$file" 2>/dev/null)"

    if [ -z "$tracked" ]; then
        printf 'state  UNTRACKED — no commit produced this\n'
        status=1
        printf '\n'
        continue
    fi

    # Content, not path. --all so an artefact from an unmerged branch is still
    # named rather than reported as unknown.
    origin="$(git --literal-pathspecs log --all --format='  %h %ad %s' --date=short --find-object="$blob" -- "$tracked" 2>/dev/null | head -3)"
    if [ -n "$origin" ]; then
        printf 'content last committed by:\n%s\n' "$origin"
    else
        printf 'content appears in no commit — this file was produced locally\n'
    fi

    if git --literal-pathspecs diff --quiet HEAD -- "$tracked" 2>/dev/null; then
        printf 'state  matches HEAD\n'
    else
        printf 'state  UNCOMMITTED — differs from HEAD\n'
        printf '       Cite the commit above, not this path: another checkout, another\n'
        printf '       hour, or a build step may have written it, and nothing in the\n'
        printf '       file says which.\n'
        status=1
    fi
    printf '\n'
done

exit "$status"
