# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

# shellcheck shell=bash
#
# Tests/git-fixture.sh — make a throwaway repository that is certainly throwaway.
#
# `git init` in a directory does not necessarily create a repository there. GIT_DIR
# names a repository outright and overrides discovery, so with it set,
# `git -C /some/scratch/dir init` re-initialises the repository GIT_DIR names,
# creates nothing at the path given, and exits 0. Every later `git -C` in that
# directory then runs with someone else's git directory and this fixture's files
# as its work tree.
#
# Git exports GIT_DIR into a hook's environment when a push originates from a
# LINKED WORKTREE — and only then, which is why it never shows up in ordinary
# use. A push from a worktree runs the suite through pre-push -> the gate
# runner -> ctest -> run-tests.sh, none of which clears the
# environment, so an unguarded fixture commits to the pusher's branch, creates
# a branch there, resets it, and writes the fixture's `user.email` into the
# repo's shared config.
#
# Two defences, because the first one cannot be complete:
#
#   The environment is cleared. That handles every variable known today.
#
#   The result is CHECKED. A fixture asserts its git directory is inside itself
#   before anything is committed, so an escape by some future variable, or by a
#   mechanism nobody has thought of, stops here instead of at someone's branch.
#
# The second is the one that matters. Every guard in this tree checks that a
# command failed; this one exists because the command SUCCEEDED and did something
# else. A setup step that reports success while acting elsewhere is invisible to
# exit statuses, to `set -e`, and to every assertion written about the result.
#

# Removes the inherited git environment from this shell and everything it spawns.
ubq_isolate_git_environment() {
    unset GIT_DIR GIT_WORK_TREE GIT_INDEX_FILE GIT_NAMESPACE GIT_COMMON_DIR
    unset GIT_OBJECT_DIRECTORY GIT_ALTERNATE_OBJECT_DIRECTORIES
    unset GIT_CEILING_DIRECTORIES GIT_PREFIX GIT_INDEX_VERSION
}

# Creates a repository at <directory> and proves it landed there.
#
# - Parameter directory: Where the fixture goes. Created if absent.
ubq_make_fixture_repo() { # <directory>
    local target="$1"
    if [ -z "$target" ]; then
        echo "ubq_make_fixture_repo: no directory given; refusing to init the current one" >&2
        return 1
    fi
    ubq_isolate_git_environment
    mkdir -p "$target" || return 1
    git -C "$target" init --quiet || return 1

    # The positive control. `git init` reporting success is not evidence that a
    # repository exists here — resolve the git directory and require it to be
    # inside the fixture.
    local resolved
    resolved="$(git -C "$target" rev-parse --absolute-git-dir 2>/dev/null)"
    local expected
    expected="$(cd "$target" && pwd -P)"
    case "$resolved" in
        "$expected"/*) ;;
        *)
            echo "ubq_make_fixture_repo: git init did not create a repository at $target" >&2
            echo "  git resolves its directory to: ${resolved:-<none>}" >&2
            echo "  Refusing to continue — the next commit would land there." >&2
            return 1
            ;;
    esac

    git -C "$target" config user.email test@example.com
    git -C "$target" config user.name Test
}
