#!/usr/bin/env bash
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

# Scripts/merge-pr.sh — rebase-merge a pull request only when it cannot undo
# what main gained while it was open.
#
#   Scripts/merge-pr.sh <number>
#
# Refuses unless the PR head's merge-base with origin/main is origin/main (the
# branch sits on the current tip), then runs Scripts/check-stale-landing.py over
# origin/main..<head>. The verdict is posted as the `stale-landing` commit status
# on the head, so branch protection can require it, and only a pass merges.
# Rebasing is necessary and not sufficient: a conflict resolved by taking a whole
# file rebases cleanly and still removes main's lines, which the check catches.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

if [ "$#" -ne 1 ] || ! [[ "$1" =~ ^[0-9]+$ ]]; then
    echo "usage: Scripts/merge-pr.sh <pull request number>" >&2
    exit 2
fi
number="$1"

repository="$(gh repo view --json nameWithOwner -q .nameWithOwner)"
read -r head branch < <(gh pr view "$number" --json headRefOid,headRefName \
    -q '.headRefOid + " " + .headRefName')
[ -n "$head" ] || { echo "merge-pr: no head for #$number" >&2; exit 2; }

git fetch -q origin main
git fetch -q origin "pull/$number/head"
fetched="$(git rev-parse FETCH_HEAD)"
if [ "$fetched" != "$head" ]; then
    echo "merge-pr: fetched $fetched but GitHub reports head $head; retry." >&2
    exit 2
fi

tip="$(git rev-parse origin/main)"
base="$(git merge-base "$tip" "$head")"
if [ "$base" != "$tip" ]; then
    echo "merge-pr: #$number ($branch) is not on the current main." >&2
    echo "  Its base is ${base:0:9}; origin/main is ${tip:0:9}. The author runs:" >&2
    echo "    git fetch origin && git rebase origin/main" >&2
    echo "    # resolve each conflict hunk by hunk, never by taking a whole file" >&2
    echo "    git push --force-with-lease origin $branch" >&2
    exit 1
fi

status=0
report="$(python3 "$ROOT/Scripts/check-stale-landing.py" --base "$tip" --tip "$head" 2>&1)" || status=$?
printf '%s\n' "$report"
if [ "$status" -eq 0 ]; then
    state=success
    description="No commit takes back recent work it does not name."
else
    state=failure
    # A status description holds 140 characters; the first table row names the
    # commit and what it undoes, and the full table is in the merger's output.
    description="$(printf '%s\n' "$report" | grep -m 2 -E '^ *[0-9]+  [0-9a-f]{9}|^[0-9a-f]{9} ' \
        | tr -s ' ' | paste -sd ';' - | cut -c1-140)"
    [ -n "$description" ] || description="check-stale-landing.py failed (status $status)."
fi
gh api --silent -X POST "repos/$repository/statuses/$head" \
    -f state="$state" -f context=stale-landing -f description="$description"

if [ "$state" != success ]; then
    echo "merge-pr: #$number not merged; the stale-landing status is $state." >&2
    exit 1
fi
gh pr merge "$number" --rebase --match-head-commit "$head"
