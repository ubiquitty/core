#!/usr/bin/env bash
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

# check-shell.sh — shellcheck over every tracked shell script, gated at zero.
#
# The tree already carried `# shellcheck source=…` directives in five scripts,
# annotations written for a tool no gate ran. What that cost is measurable: a
# refactor of check-comments.sh moved its records from a variable to a file and
# converted three of four modes, leaving `--staged` assigning to RECORDS while
# every consumer read RECORDS_FILE. shellcheck reports that as SC2034, and the
# pre-commit comment gate silently enforced nothing until someone noticed.
#
# Severity is `warning`. At `info` and `style` shellcheck reports preferences,
# and a finding a reader can reasonably reject is one that gets rejected.
#
# EXEMPTION: a script carries `# shellcheck-skip: <reason>` only where shellcheck
# cannot analyse it at all — it implements sh, bash, dash and ksh, so a zsh file
# has no dialect to check against and SC2148 is unanswerable. The count of
# skipped files is printed, because an exemption nobody sees is one that spreads.
#
# Usage:
#   Scripts/check-shell.sh          gate every tracked *.sh
#   Scripts/check-shell.sh PATH …   gate the named files
#
# Exit 0 = clean, 1 = findings.

set -uo pipefail
SCRIPTS="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# The subject is the tree git reports for the directory the gate runs in, which is
# not this script's own when it runs as a library's copy.
ROOT="$(git rev-parse --show-toplevel 2>/dev/null || (cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd))"
cd "$ROOT" || exit 1

# shellcheck source=Scripts/gate-report-lib.sh
source "$SCRIPTS/gate-report-lib.sh"

GATE="check-shell"

case "${1:-}" in
    --help|-h) sed -n '/^#!/d; /^[^#]/q; s/^# \{0,1\}//; p' "$0"; exit 0 ;;
esac

if ! command -v shellcheck >/dev/null 2>&1; then
    # Loud, not silent. A missing linter that prints nothing is indistinguishable
    # from a clean tree, and this gate exists because of exactly that shape.
    echo "$GATE: SKIPPED — shellcheck is not installed (brew install shellcheck)" >&2
    exit 0
fi

FILES=()
if [ "$#" -gt 0 ]; then
    for path in "$@"; do
        [ -f "$path" ] && FILES+=("$path")
    done
else
    # By shebang as well as by extension. Enumerating '*.sh' reads complete,
    # because every file it names really is a shell script -- but the population
    # is every file that is one, and the two differ by the six that matter most:
    # Scripts/hooks/* and hooks-dispatch carry no extension, so the scripts that
    # enforce every other gate were the ones this gate could not see.
    #
    # Built into a file rather than a process substitution: `<(...)` ends at the
    # first `)` it meets, and both a case pattern and a grep alternation contain
    # one. The failure is a parse error, not a wrong answer -- but the run that
    # follows it prints "no shell script to scan", which is a clean-looking verdict.
    LIST="$(mktemp -t check-shell-list.XXXXXX)"
    trap 'rm -f "$LIST"' EXIT
    # `--cached --others --exclude-standard`. Measured rather than reasoned:
    # a bare `git ls-files` already includes a staged file, because staging puts it
    # in the index — so on the pre-commit path, which stages first, it was
    # already adequate. What it misses is a file that is neither committed nor
    # staged, which is every hand-run of this gate on a dirty tree.
    #
    # That is the narrower claim and it is the true one. A gate a person runs
    # before `git add` should judge what they are about to add.
    git ls-files --cached --others --exclude-standard '*.sh' > "$LIST"
    # A shell script that is not named .sh is found by its shebang, and the
    # question is only about the first line. Asking it with a head and a grep per
    # candidate is two processes for each tracked file, to find a handful.
    #
    # `git grep` narrows to files carrying a shebang ANYWHERE, in one process.
    # That is a superset of the ones carrying it on line one, so the precise test
    # still runs — over a few files rather than the whole tree.
    { git grep -l -I -E '^#!.*(sh|bash|dash|ksh|zsh)' -- ':!*.sh' 2>/dev/null
      git ls-files --others --exclude-standard | grep -v '\.sh$' ; } \
        | while IFS= read -r candidate; do
            [ -f "$candidate" ] || continue
            if head -1 "$candidate" 2>/dev/null | grep -qE '^#!.*(sh|bash|dash|ksh|zsh)'; then
                echo "$candidate"
            fi
    done >> "$LIST"
    while IFS= read -r path; do
        [ -n "$path" ] && [ -f "$path" ] && FILES+=("$path")
    done < "$(grep -vE '(^|/)ThirdParty/' "$LIST" | sort -u > "$LIST.f"; echo "$LIST.f")"
    rm -f "$LIST.f"
fi

if [ "${#FILES[@]}" -eq 0 ]; then
    gate_ok "$GATE" "no shell script to scan"
    exit 0
fi

CHECKED=()
SKIPPED=()
for path in "${FILES[@]}"; do
    # Anchored. An unanchored match let this file exempt itself, because the
    # paragraph above that documents the marker contains the marker.
    if grep -qE '^# shellcheck-skip:' "$path"; then
        SKIPPED+=("$path")
    else
        CHECKED+=("$path")
    fi
done

WORK="$(mktemp -d -t check-shell.XXXXXX)"
trap 'rm -rf "$WORK"' EXIT

# One shellcheck per batch across all cores, not one invocation over 123 files:
# the work is per-file parsing and the serial form leaves every core but one
# idle. Output is sorted because the batches finish out of order and a gate whose
# findings move between runs is one nobody can diff.
if [ "${#CHECKED[@]}" -gt 0 ]; then
    printf '%s\n' "${CHECKED[@]}" \
        | xargs -P "$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)" -n 8 \
            shellcheck -S warning -f gcc 2>/dev/null \
        | sed "s|^$ROOT/||" | sort > "$WORK/out.txt"
fi
# A linter is not a shell. `$( case x in *.sh) … esac )` lints clean, and bash
# ends the command substitution at the case pattern's `)` and cannot run the
# file. Measured: zero findings from the linter, a syntax error from `bash -n`.
# So the parse is a second arm rather than a stricter setting of the first.
PARSE_FAILURES=0
for path in "${CHECKED[@]}"; do
    case "$path" in
        *.zsh) continue ;;
    esac
    if ! bash -n "$path" 2>>"$WORK/parse.txt"; then
        PARSE_FAILURES=$((PARSE_FAILURES + 1))
    fi
done
if [ "$PARSE_FAILURES" -ne 0 ]; then
    sed "s|^$ROOT/||" "$WORK/parse.txt" >&2
    gate_failed_count "$GATE" "$PARSE_FAILURES" "shell script(s) bash cannot parse (shellcheck does not report these)"
    exit 1
fi

COUNT=$(wc -l < "$WORK/out.txt" 2>/dev/null | tr -d ' ')
COUNT="${COUNT:-0}"

reportSkipped() {
    [ "${#SKIPPED[@]}" -eq 0 ] && return 0
    echo "  note: ${#SKIPPED[@]} file(s) carry # shellcheck-skip and were not scanned:" >&2
    printf '    %s\n' "${SKIPPED[@]}" >&2
}

if [ "$COUNT" != "0" ]; then
    while IFS= read -r finding; do
        location="${finding%%: *}"
        gate_finding "${location%:*}" "shellcheck" "${finding#*: }"
    done < <(head -40 "$WORK/out.txt")
    [ "$COUNT" -gt 40 ] && echo "  … and $((COUNT - 40)) more." >&2
    reportSkipped
    gate_failed_count "$GATE" "$COUNT" "shellcheck finding(s) at severity warning"
    exit 1
fi

reportSkipped
gate_ok "$GATE" "${#CHECKED[@]} shell script(s) clean at severity warning"
exit 0
