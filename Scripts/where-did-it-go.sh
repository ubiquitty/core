#!/usr/bin/env bash
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

#
# where-did-it-go.sh — a path that no longer exists, and where it went.
#
#   ./Scripts/where-did-it-go.sh PATH...          report the successor of each
#   ./Scripts/where-did-it-go.sh --fix FILE...    rewrite dead paths in FILE
#
# A hand-maintained list of source paths — a shell driver's link set, a CMake
# source list, a doc pointing at a file — goes stale the moment a source moves,
# and the check that notices can only say the path is gone. The person then goes
# looking, which is the expensive part and the part git already did.
#
# `git log --diff-filter=D` names the commit that removed the path, and
# `git show -M` on that commit reports the rename it was part of. That works
# across a rename as well as a move: Source/Core/OSC.cpp -> OSC/Dispatch.cpp is
# 99% similar, so git pairs them even though no part of the name survived.
#
# What it will not do. A path deleted without a successor gets no suggestion,
# and a rewrite is offered only when git names exactly one target. Guessing by
# basename would map OSC.cpp onto whichever OSC.cpp happened to exist, which is
# how a stale list becomes a wrong list instead of one that fails to resolve.
#

set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT" || exit 1

MODE="report"
if [ "${1:-}" = "--fix" ]; then
    MODE="fix"; shift
fi
if [ "$#" -eq 0 ]; then
    sed -n '/^#!/d; /^[^#]/q; s/^# \{0,1\}//; p' "$0"
    exit 2
fi

# successor_of <path> -> prints "<new path>\t<commit>\t<subject>" if git knows one
successor_of() {
    local path="$1" commit line
    commit=$(git log -1 --diff-filter=D --format=%H -- "$path" 2>/dev/null)
    [ -n "$commit" ] || return 1
    line=$(git show -M --name-status --format= "$commit" 2>/dev/null \
           | awk -v p="$path" '$1 ~ /^R/ && $2 == p { print $3; exit }')
    [ -n "$line" ] || return 1
    printf '%s\t%s\t%s\n' "$line" "$(git rev-parse --short "$commit")" \
           "$(git log -1 --format=%s "$commit")"
}

STATUS=0

if [ "$MODE" = "report" ]; then
    for path in "$@"; do
        if [ -e "$path" ]; then
            echo "$path: still here"
            continue
        fi
        if found=$(successor_of "$path"); then
            IFS=$'\t' read -r target commit subject <<< "$found"
            printf '%s\n  moved to %s\n  in %s "%s"\n' "$path" "$target" "$commit" "$subject"
        else
            printf '%s\n  no successor: git has it deleted, not renamed\n' "$path"
            STATUS=1
        fi
    done
    exit "$STATUS"
fi

# --fix: every path-shaped token in FILE that no longer exists and has exactly
# one successor is rewritten to it.
for file in "$@"; do
    [ -f "$file" ] || { echo "where-did-it-go: no such file: $file" >&2; STATUS=2; continue; }
    changed=0
    while IFS= read -r path; do
        [ -n "$path" ] || continue
        [ -e "$path" ] && continue
        if found=$(successor_of "$path"); then
            IFS=$'\t' read -r target commit subject <<< "$found"
            # A path is rewritten whole, so a prefix like Source/Core/OSC.cpp
            # cannot half-match Source/Core/OSC.cpp.orig.
            python3 - "$file" "$path" "$target" <<'PY'
import io, re, sys
path, old, new = sys.argv[1], sys.argv[2], sys.argv[3]
text = io.open(path, encoding='utf-8').read()
io.open(path, 'w', encoding='utf-8').write(
    re.sub(re.escape(old) + r'(?![A-Za-z0-9_./-])', new, text))
PY
            printf '  %s\n    -> %s   (%s "%s")\n' "$path" "$target" "$commit" "$subject"
            changed=$((changed + 1))
        else
            printf '  %s\n    -> no successor; fix by hand\n' "$path"
            STATUS=1
        fi
    # Only repo-rooted paths. A bare basename in prose ("see OSC.cpp") is
    # not a path this can check, and reporting it as unresolvable buries the
    # ones that are.
    done < <(grep -ohE '(\$ROOT/|\$\{ROOT\}/)?(Source|Tests|Tools|Scripts|Samples|Docs)/[A-Za-z0-9_./-]+\.(cpp|mm|c|m|h|hpp|swift)' "$file" \
             | sed -E 's|^\$\{?ROOT\}?/||' | sort -u)
    if [ "$changed" -gt 0 ]; then
        echo "$file: rewrote $changed path(s)"
    else
        echo "$file: nothing to rewrite"
    fi
done
exit "$STATUS"
