#!/usr/bin/env bash
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

# pgo-staleness.sh — shared staleness nudge for the PGO consumers
# (the macOS app's build, run-bench.sh). Sourced, not executed.
#
# The tolerance flags make a drifted profile degrade SILENTLY (functions fall
# back to plain -O3 placement one by one), so nothing ever surfaces the decay.
# pgo_staleness_warning counts commits touching Source/ since the profile's
# last commit and warns past a threshold — a nudge to rerun
# Scripts/pgo-train.sh, never a failure. Non-git trees (tarballs) and an
# untracked profile skip silently; a path that does not exist warns, because
# that is the caller getting it wrong rather than a tree without the profile;
# a profile trained at a commit this history lacks says, in one line, that its
# drift cannot be counted here.

# The sidecar naming the tree a profile was TRAINED on, beside the profile.
pgo_trained_at_path() { printf '%s.trained-at' "${1%.profdata}"; }

# The sidecar naming the Source/ TREE a profile describes.
pgo_trained_tree_path() { printf '%s.trained-tree' "${1%.profdata}"; }

# Whether the profile describes <ref>'s Source/ exactly.
#
# A commit name cannot answer this. The trained-at sidecar records one written
# in a worktree before the merge, and a rebase merge rewrites every commit it
# replays — so that sidecar names an object the tree may never have had, and
# the count below silently becomes a lower bound rather than a measurement. A
# tree hash is the content the replay preserves, so it compares the thing the
# question is actually about.
#
# pgo_profile_describes_tree <repo-root> <profile-path> <ref>
pgo_profile_describes_tree() {
    local treeFile trainedTree currentTree
    treeFile="$1/$(pgo_trained_tree_path "$2")"
    [ -f "$treeFile" ] || return 1
    trainedTree=$(tr -d '[:space:]' < "$treeFile")
    [ -n "$trainedTree" ] || return 1
    currentTree=$(git -C "$1" rev-parse "$3:Source" 2>/dev/null) || return 1
    [ "$trainedTree" = "$currentTree" ]
}

# The commit whose tree the profile describes, as of <ref>.
#
# The profile's own commit is the wrong reference and reads clean in exactly the
# case that matters. A profile trained in a worktree and merged with a rebase
# lands ON TOP of commits it was trained before, so the file's last-touched
# commit is newer than the counters inside it and the drift count reports zero
# while the artifact is stale.
#
# So the trained-at sidecar wins where it exists, and the file's commit is the
# fallback for a profile that predates it. A sidecar naming a commit this
# repository lacks answers nothing, and the fallback would answer wrongly: in a
# clone of a history that starts after the training, the file's commit is that
# history's first commit, and every commit of drift before it goes uncounted. That case returns 2 and prints
# nothing, so a caller can say the drift is unmeasurable here rather than small.
#
# pgo_profile_commit <repo-root> <profile-path> <ref>
pgo_profile_commit() {
    local trainedAtFile trainedAt
    trainedAtFile="$1/$(pgo_trained_at_path "$2")"
    if [ -f "$trainedAtFile" ]; then
        trainedAt=$(tr -d '[:space:]' < "$trainedAtFile")
        if [ -n "$trainedAt" ]; then
            git -C "$1" cat-file -e "$trainedAt^{commit}" 2>/dev/null || return 2
            printf '%s' "$trainedAt"
            return 0
        fi
    fi
    git -C "$1" log -1 --format=%H "$3" -- "$2" 2>/dev/null
}

# Source/ commits between the profile <ref> was built with and <ref> itself.
# Returns 2, printing nothing, when the trained-at commit is not in this history.
#
# pgo_drift_count <repo-root> <profile-path> <ref>
pgo_drift_count() {
    local profileCommit status=0
    profileCommit=$(pgo_profile_commit "$1" "$2" "$3") || status=$?
    [ "$status" -eq 0 ] || return "$status"
    [ -n "$profileCommit" ] || return 1
    git -C "$1" rev-list --count "$profileCommit".."$3" -- Source/ 2>/dev/null
}

# pgo_staleness_warning <repo-root> <profile-path> [threshold=10]
pgo_staleness_warning() {
    local repositoryRoot="$1" profilePath="$2" threshold="${3:-10}"
    local driftCount
    # A path that is absent from disk is a caller error: the git query below
    # cannot tell it from a tarball checkout and would skip silently. Callers
    # pass the path they feed -fprofile-use.
    if [ ! -f "$repositoryRoot/$profilePath" ]; then
        echo "PGO:        WARNING - staleness check points at '$profilePath', which does not exist." >&2
        echo "PGO:        The drift nudge is OFF until the caller passes the profile it compiles with." >&2
        return 0
    fi
    # An exact answer where one exists: a profile describing this Source/ tree
    # is not stale by any count, and a rebase that moved every commit beneath
    # it has not changed what it describes.
    if pgo_profile_describes_tree "$repositoryRoot" "$profilePath" HEAD; then
        return 0
    fi
    local status=0
    driftCount=$(pgo_drift_count "$repositoryRoot" "$profilePath" HEAD) || status=$?
    if [ "$status" -eq 2 ]; then
        echo "PGO:        drift cannot be measured from history here: $(pgo_trained_at_path "$profilePath") names a commit this repository lacks; Scripts/check-pgo-applies.py --recorded-budget measures it from the compiler." >&2
        return 0
    fi
    [ "$status" -eq 0 ] || return 0
    if [ "${driftCount:-0}" -ge "$threshold" ]; then
        echo "PGO:        WARNING - profile is $driftCount Source/ commits old; drifted functions" >&2
        echo "PGO:        silently lose their profile placement. Retrain: Scripts/pgo-train.sh" >&2
    fi
}
