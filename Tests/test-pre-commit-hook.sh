#!/usr/bin/env bash
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

# Tests/test-pre-commit-hook.sh — the hook lets a contributor's clone commit.
#
# Only a checkout named in UBIQUITTY_SHARED_CHECKOUT refuses commits. Sibling
# directories and the clone's name must not change a contributor's permissions.
# A missing tool is reported even when its gate permits the commit.
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

. "$ROOT/Tests/shell-assertions.sh"
. "$ROOT/Tests/git-fixture.sh"

# The caller's shell may already name a pull-only checkout, which would decide a
# case before the hook did.
unset UBIQUITTY_SHARED_CHECKOUT UBIQUITTY_MAIN_COMMIT_OK UBQ_GATE_VERBOSE
ubq_isolate_git_environment

CLONE="$TMP/ubiquitty"
ubq_make_fixture_repo "$CLONE" || exit 1
mkdir -p "$CLONE/Scripts/hooks"
cp "$ROOT/Scripts/hooks/pre-commit" "$CLONE/Scripts/hooks/pre-commit"
cp "$ROOT/Scripts/run-local-hooks.sh" "$CLONE/Scripts/run-local-hooks.sh"

# Every gate the hook names passes here, so a refusal can only be the rule's.
# The list is read out of the hook the way the hook reads it, so a gate the hook
# gains passes here too with no edit to this file.
for gate in $(grep -oE '\$ROOT/Scripts/check-[a-z0-9-]+\.(sh|py)' "$CLONE/Scripts/hooks/pre-commit" \
    | sed 's|^\$ROOT/||' | sort -u); do
    case "$gate" in
        *.py) printf '#!/usr/bin/env python3\nraise SystemExit(0)\n' > "$CLONE/$gate" ;;
        *.sh) printf '#!/bin/sh\nexit 0\n' > "$CLONE/$gate" ;;
    esac
    chmod +x "$CLONE/$gate"
done
git -C "$CLONE" add -A
git -C "$CLONE" -c commit.gpgsign=false commit -q --no-verify -m fixture || exit 1
CLONE_TOPLEVEL="$(git -C "$CLONE" rev-parse --show-toplevel)"

refusal="checkout is pull-only"

runHook() { # <directory> [NAME=value...]
    local directory="$1"
    shift
    (cd "$directory" && env "$@" bash Scripts/hooks/pre-commit) >"$TMP/out" 2>&1
}

runHook "$CLONE"
check "a contributor's clone commits" "0" "$?"
absent "and is not told it is pull-only" "$refusal" "$(<"$TMP/out")"

# A gate whose tool this machine lacks exits 0 with a SKIPPED line. The hook holds
# a passing gate's output back, and must not hold that line back with it.
stoodDown="check-profile-orphans: SKIPPED — no llvm-profdata on this machine"
printf '#!/usr/bin/env python3\nprint("%s")\n' "$stoodDown" > "$CLONE/Scripts/check-profile-orphans.py"
runHook "$CLONE"
check "a gate that stood down does not refuse the commit" "0" "$?"
contains "and the hook names it" "$stoodDown" "$(<"$TMP/out")"
git -C "$CLONE" checkout -q -- Scripts/check-profile-orphans.py

runHook "$CLONE" UBIQUITTY_SHARED_CHECKOUT="$CLONE_TOPLEVEL"
check "a checkout named in the setting is refused" "1" "$?"

# Unrelated sibling directories must not affect a contributor's clone.
mkdir -p "$TMP/tracker/backlog"
: > "$TMP/tracker/backlog/lint.py"

runHook "$CLONE"
check "a sibling directory does not refuse a contributor" "0" "$?"

runHook "$CLONE" UBIQUITTY_SHARED_CHECKOUT="$CLONE_TOPLEVEL"
check "a pull-only checkout is refused" "1" "$?"
contains "and the refusal says why" "$refusal" "$(<"$TMP/out")"
absent "without printing its own bypass" "UBIQUITTY_MAIN_COMMIT_OK" "$(<"$TMP/out")"

runHook "$CLONE" UBIQUITTY_SHARED_CHECKOUT="$CLONE_TOPLEVEL" UBIQUITTY_MAIN_COMMIT_OK=1
check "the emergency bypass lets it commit" "0" "$?"

# A linked worktree named plainly after the project, beside the sibling directory.
git -C "$CLONE" worktree add -q -b topic "$TMP/worktree/ubiquitty" || exit 1
runHook "$TMP/worktree/ubiquitty"
check "a linked worktree commits" "0" "$?"

report "test-pre-commit-hook" "a contributor's clone commits, a pull-only checkout does not, and a stand-down is named"
