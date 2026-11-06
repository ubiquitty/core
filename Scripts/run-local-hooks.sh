#!/usr/bin/env bash
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0
#
# Runs the checks a clone's owner added to one of its hooks.
#
#   Scripts/run-local-hooks.sh HOOK [ARGUMENT]...
#
# Every executable file in $GIT_COMMON_DIR/hooks.d/HOOK/ runs in name order from
# the top of the working tree, with the hook's arguments and a copy of the
# standard input this script was given. The first that exits non-zero stops the
# run, and this script exits with its status. Linked worktrees share the common
# directory, so one placement covers every worktree of a clone.
#
# The directory belongs to the clone: nothing in this repository writes it, and
# a clone without one runs nothing and prints nothing.
set -uo pipefail

[ "$#" -ge 1 ] && [ -n "$1" ] || { echo "usage: run-local-hooks.sh HOOK [ARGUMENT]..." >&2; exit 2; }
hook="$1"
shift
case "$hook" in
    */*|.|..) echo "run-local-hooks: not a hook name: $hook" >&2; exit 2 ;;
esac

common="$(git rev-parse --path-format=absolute --git-common-dir 2>/dev/null)" || exit 0
directory="$common/hooks.d/$hook"
[ -d "$directory" ] || exit 0

top="$(git rev-parse --show-toplevel 2>/dev/null)" || exit 0
cd "$top" || exit 1

# Each check reads the same input, so it is kept in a file and each reads its
# own copy; a check that exits without reading cannot fail the write. A
# terminal is not input.
input="$(mktemp "${TMPDIR:-/tmp}/ubq-local-hooks.XXXXXX")" || exit 1
trap 'rm -f "$input"' EXIT
[ -t 0 ] || cat >"$input"

for check in "$directory"/*; do
    [ -f "$check" ] && [ -x "$check" ] || continue
    "$check" "$@" <"$input"
    status=$?
    if [ "$status" -ne 0 ]; then
        echo "$hook: $check exited $status" >&2
        exit "$status"
    fi
done
exit 0
