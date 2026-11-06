#!/usr/bin/env bash
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

# Scripts/test-install-hooks.sh — the hook install, against a throwaway clone
# with a linked worktree. No real build, seconds to run.
#
# Every case here is a way for a hook to fail OPEN: to be installed, appear
# installed, and gate nothing. That is the failure this install is shaped
# around, so it is the failure that has to be pinned.
set -uo pipefail

# git exports these into a hook's environment and they OVERRIDE the repository a
# git command discovers from its directory — `git -C <tree>` included, because -C
# sets the working directory and an explicit GIT_DIR skips discovery entirely. A
# push from a linked worktree would aim the destructive git below at the PUSHING
# worktree instead of the one named here.
unset GIT_DIR GIT_WORK_TREE GIT_INDEX_FILE GIT_OBJECT_DIRECTORY \
    GIT_ALTERNATE_OBJECT_DIRECTORIES GIT_COMMON_DIR GIT_NAMESPACE

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
. "$(cd "$(dirname "$0")/../Tests" && pwd)/shell-assertions.sh"
. "$(cd "$(dirname "$0")/../Tests" && pwd)/git-fixture.sh"

# A clone with the two scripts and one hook that records where it ran from.
MAIN="$TMP/main"
mkdir -p "$MAIN/Scripts/hooks"
cd "$MAIN" || exit 1
ubq_make_fixture_repo "$MAIN"
git config user.email t@example.com
git config user.name t
cp "$ROOT/Scripts/install-hooks.sh" "$ROOT/Scripts/hooks-dispatch" Scripts/
chmod +x Scripts/install-hooks.sh
# shellcheck disable=SC2016  # $TMP_MARK expands when the hook runs
printf '#!/usr/bin/env bash\necho "MAIN-HOOK" > "$TMP_MARK"\nexit 0\n' > Scripts/hooks/pre-push
chmod +x Scripts/hooks/pre-push
git add -A
git commit -qm init

./Scripts/install-hooks.sh > /dev/null

# Absolute: --git-common-dir answers relative to the cwd, and this path is used
# from inside a worktree where a relative `.git` is a file rather than the dir.
HOOKS="$(cd "$(git rev-parse --git-common-dir)" && pwd)/hooks"

# 1. The installed entry is a regular file, not a symlink into a working tree.
#    A symlink is what makes removal of one worktree break the whole clone.
if [ -L "$HOOKS/pre-push" ]; then
    fail "installed hook is a symlink"
else
    pass "installed hook is a regular file, not a symlink into a worktree"
fi

# 2. A linked worktree runs its own hook, not the main checkout's.
git worktree add -q "$TMP/wt" -b wt
cd "$TMP/wt" || exit 1
# shellcheck disable=SC2016  # $TMP_MARK expands when the hook runs
printf '#!/usr/bin/env bash\necho "WORKTREE-HOOK" > "$TMP_MARK"\nexit 0\n' > Scripts/hooks/pre-push
export TMP_MARK="$TMP/mark"
bash "$HOOKS/pre-push" < /dev/null
check "a worktree runs its own copy of the hook" "WORKTREE-HOOK" "$(cat "$TMP/mark" 2>/dev/null)"

cd "$MAIN" || exit 1
bash "$HOOKS/pre-push" < /dev/null
check "the main checkout runs its own copy" "MAIN-HOOK" "$(cat "$TMP/mark" 2>/dev/null)"

# 3. Removing the worktree the install was not run from leaves hooks working.
git worktree remove --force "$TMP/wt"
rm -f "$TMP/mark"
bash "$HOOKS/pre-push" < /dev/null
check "hooks survive removal of a worktree" "MAIN-HOOK" "$(cat "$TMP/mark" 2>/dev/null)"

# 4. A missing hook body fails CLOSED. Git skips a dangling symlink silently,
#    which is indistinguishable from a gate that ran and passed.
mv Scripts/hooks/pre-push "$TMP/stashed-hook"
if bash "$HOOKS/pre-push" < /dev/null 2>"$TMP/err"; then
    fail "a missing hook body passed silently"
else
    pass "a missing hook body fails closed"
fi
grep -q "does not exist" "$TMP/err" || fail "the missing-body failure does not name the path"
mv "$TMP/stashed-hook" Scripts/hooks/pre-push

# 5. Installing from a worktree does not repoint the clone at that worktree.
git worktree add -q "$TMP/wt2" -b wt2
cd "$TMP/wt2" || exit 1
./Scripts/install-hooks.sh > /dev/null
if [ -L "$HOOKS/pre-push" ]; then
    fail "installing from a worktree left a symlink into it"
else
    pass "installing from a worktree does not capture the clone"
fi
cd "$MAIN" || exit 1
rm -f "$TMP/mark"
bash "$HOOKS/pre-push" < /dev/null
check "the main checkout still runs its own hook afterwards" "MAIN-HOOK" "$(cat "$TMP/mark" 2>/dev/null)"

# Retiring a repository dispatcher must not delete a user-owned hook.
cp Scripts/hooks-dispatch "$HOOKS/post-commit"
printf '#!/bin/sh\nexit 0 # user-owned\n' > "$HOOKS/post-merge"
./Scripts/install-hooks.sh > /dev/null
if [ -e "$HOOKS/post-commit" ]; then
    fail "a retired repository dispatcher remains installed"
else
    pass "a retired repository dispatcher is removed"
fi
contains "a user-owned hook is preserved" "user-owned" "$(cat "$HOOKS/post-merge")"
rm "$HOOKS/post-merge"
ln -s "$TMP/user-hook" "$HOOKS/post-merge"
./Scripts/install-hooks.sh > /dev/null
if [ -L "$HOOKS/post-merge" ]; then
    pass "a user-owned hook symlink is preserved"
else
    fail "a user-owned hook symlink was removed"
fi

echo
report "test-install-hooks" "the hook install fails closed in every way tried"
