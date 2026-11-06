#!/usr/bin/env bash
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

# pgo-retrain-check.sh — is a retrain due, and the retrain itself, left on a branch.
#
#   Scripts/pgo-retrain-check.sh                     # the profile's drift against its budget
#   Scripts/pgo-retrain-check.sh --retrain           # train on origin/main in a worktree; commit on pgo/retrain-<date>
#   Scripts/pgo-retrain-check.sh --retrain --base REF
#
# The gates refuse a drifted profile and nothing retrains one: the push that
# finds the budget spent is refused, and the retrain it asks for is a build
# somebody has to start. This is the recurring job. Run it weekly, and as soon
# as the pre-push gate prints its warning line. The report says whether a
# retrain is due, and `--retrain` performs it by the procedure: Scripts/pgo-train.sh
# in a fresh worktree of origin/main, then the profile and its sidecars
# (.trained-at, .trained-tree, .drift-budget) in one commit and nothing else
# (Scripts/check-pgo-commit-shape.py), on a branch named for the date. It never
# pushes. The branch is pushed by hand and lands as its own commit, so the
# code-generation and performance effects of the profile stay attributable.
#
# Not a CI job, and Support/pgo/README.md says why: the profile is trained on
# the machine class that consumes it, and the workflows run with read-only
# tokens and no path back to the repository.
#
# Report mode exits 0 within budget, 1 when a retrain is due, 2 when nothing
# could be measured. It trains the compiler target's profile (Scripts/pgo-flags.sh):
# portable on macOS, architecture-specific release profiles on Linux.
# UBQ_BUILD_JOBS bounds the probe compile; UBQ_PGO_RETRAIN_WORKTREE names the
# worktree, UBQ_PGO_RETRAIN_NICE the priority the training build runs at.
set -euo pipefail

# Run from a hook, GIT_DIR names the pushing worktree and overrides the
# directory a git command runs in, `-C` included; the worktree this adds has
# to land where it is named.
unset GIT_DIR GIT_WORK_TREE GIT_INDEX_FILE GIT_NAMESPACE GIT_COMMON_DIR \
    GIT_OBJECT_DIRECTORY GIT_ALTERNATE_OBJECT_DIRECTORIES GIT_CEILING_DIRECTORIES GIT_PREFIX

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
case "${UBQ_PGO_PLATFORM:-$(uname -s)}:${UBQ_CI_LINUX_IMAGE_ID:-}" in
    Linux:) exec python3 "$ROOT/Scripts/pgo_toolchain.py" run "$ROOT/Scripts/pgo-retrain-check.sh" "$@" ;;
esac
if [ -n "${UBQ_CI_LINUX_IMAGE_ID:-}" ]; then
    export PATH="${UBQ_PGO_LLVM:?release LLVM directory is required}:$PATH"
    export CC=clang CXX=clang++
fi
CXX="${CXX:-c++}"
JOBS="${UBQ_BUILD_JOBS:-4}"
# shellcheck source=Scripts/pgo-flags.sh
source "$ROOT/Scripts/pgo-flags.sh"
# shellcheck source=Scripts/pgo-staleness.sh
source "$ROOT/Scripts/pgo-staleness.sh"

MODE=report
BASE=origin/main
while [ "$#" -gt 0 ]; do
    case "$1" in
        --retrain) MODE=retrain ;;
        --base)
            [ "$#" -ge 2 ] || { echo "pgo-retrain-check: --base needs a ref" >&2; exit 2; }
            BASE="$2"
            shift
            ;;
        -h | --help)
            sed -n '/^#!/d; /^[^#]/q; s/^# \{0,1\}//; /^Copyright (C)/d; /^SPDX-License/d; p' "$0"
            exit 0
            ;;
        *)
            echo "pgo-retrain-check: unknown argument '$1'" >&2
            exit 2
            ;;
    esac
    shift
done

# The profile is LLVM's format; a compiler that does not read it cannot measure
# drift against it, and training with it would fail at the first object.
if ! ubiquitty_compiler_reads_llvm_profile "$CXX"; then
    echo "pgo-retrain-check: SKIPPED — '$CXX' does not read LLVM's profile format; set CXX=clang++." >&2
    exit 0
fi

PROFILE="$(ubiquitty_default_pgo_profile "$ROOT" "$CXX")"
PROFILE="${PROFILE#"$ROOT"/}"
STEM="${PROFILE%.profdata}"
case "$STEM" in
    */linux-x86_64-core) TARGET="Linux x86-64" ;;
    */linux-aarch64-core) TARGET="Linux aarch64" ;;
    *) TARGET="portable" ;;
esac

report() {
    echo "pgo-retrain-check: profile $PROFILE ($TARGET)"
    if pgo_profile_describes_tree "$ROOT" "$PROFILE" HEAD; then
        echo "  describes HEAD's Source/ tree exactly"
    else
        local trainedAt drift
        trainedAt="$(pgo_profile_commit "$ROOT" "$PROFILE" HEAD || true)"
        drift="$(pgo_drift_count "$ROOT" "$PROFILE" HEAD || true)"
        local shown="${trainedAt:0:9}"
        [ -n "$shown" ] || shown="a commit this history lacks"
        echo "  trained at $shown; ${drift:-an unknown number of} Source/ commit(s) since"
    fi
    local status=0
    if [ -f "$STEM.drift-budget" ]; then
        echo "  budget sidecar: $(tr '\n' ' ' < "$STEM.drift-budget")"
        python3 "$ROOT/Scripts/check-pgo-applies.py" --recorded-budget --jobs "$JOBS" || status=$?
    else
        # No budget was recorded for this profile, so the total is measured
        # against the allowance a retrain would record above a baseline of
        # zero: the same bound the next retrain starts under.
        local allowance
        allowance="$(python3 "$ROOT/Scripts/check-pgo-applies.py" --print-drift-allowance)"
        echo "  no drift budget beside the profile; measuring against the allowance a retrain records, $allowance"
        python3 "$ROOT/Scripts/check-pgo-applies.py" --max-discarded "$allowance" --jobs "$JOBS" || status=$?
    fi
    case "$status" in
        0) echo "pgo-retrain-check: ok — the profile is within its budget; no retrain is due" ;;
        1) echo "pgo-retrain-check: a retrain is due — $0 --retrain" >&2 ;;
        *) echo "pgo-retrain-check: REFUSED — the drift could not be measured; the lines above say why" >&2 ;;
    esac
    return "$status"
}

retrain() {
    local date branch worktree sidecar message
    date="$(date -u +%Y%m%d)"
    branch="${UBQ_PGO_RETRAIN_BRANCH:-pgo/retrain-$date}"
    worktree="${UBQ_PGO_RETRAIN_WORKTREE:-${UBQ_BUILD_ROOT:-$ROOT/..}/ubiquitty-pgo-retrain-$date}"
    worktree="${worktree%/}"
    case "$BASE" in
        origin/*) git fetch --quiet origin ;;
    esac
    if git rev-parse --verify --quiet "refs/heads/$branch" >/dev/null; then
        echo "pgo-retrain-check: branch $branch exists; land it or delete it before training again." >&2
        exit 2
    fi
    if [ -e "$worktree" ]; then
        echo "pgo-retrain-check: $worktree exists; remove it (git worktree remove) or set UBQ_PGO_RETRAIN_WORKTREE." >&2
        exit 2
    fi

    # A fresh worktree of the base rather than this checkout: a profile
    # describes the tree it lands on, and this checkout may be on a branch,
    # carrying edits, or behind. The worktree's own scripts run, so the base
    # decides which trainer and which gate the profile is made with.
    git worktree add --quiet -b "$branch" "$worktree" "$BASE"
    echo "pgo-retrain-check: training in $worktree at $(git -C "$worktree" rev-parse --short HEAD) ($BASE) ..." >&2
    (cd "$worktree" && UBQ_BUILD_JOBS="$JOBS" nice -n "${UBQ_PGO_RETRAIN_NICE:-10}" ./Scripts/pgo-train.sh)

    local sidecars="profdata trained-at trained-tree drift-budget"
    [ -z "${UBQ_CI_LINUX_IMAGE_ID:-}" ] || sidecars="$sidecars trained-toolchain"
    for sidecar in $sidecars; do
        if [ ! -f "$worktree/$STEM.$sidecar" ]; then
            echo "pgo-retrain-check: FAILED — pgo-train.sh left no $STEM.$sidecar; nothing is committed" >&2
            exit 1
        fi
        git -C "$worktree" add "$STEM.$sidecar"
    done
    python3 "$worktree/Scripts/check-pgo-commit-shape.py"

    message="$(mktemp)"
    {
        echo "Retrain the $TARGET Core profile at $(git -C "$worktree" rev-parse --short HEAD)"
        echo
        echo "Trained on $(git -C "$worktree" rev-parse HEAD), Source/ tree"
        echo "$(tr -d '[:space:]' < "$worktree/$STEM.trained-tree"). Every portable-core"
        echo "translation unit compiles against the profile with zero discarded"
        echo "counters (Scripts/check-pgo-applies.py --max-discarded 0), and the drift"
        echo "budget recorded beside it is $(awk '$1 == "drift_budget" { print $2 }' "$worktree/$STEM.drift-budget") counters."
    } > "$message"
    git -C "$worktree" commit --quiet -F "$message"
    rm -f "$message"

    echo "pgo-retrain-check: $(git -C "$worktree" rev-parse --short HEAD) on $branch, in $worktree"
    echo "  Review it, then push the branch: git push origin $branch"
    echo "  This script never pushes."
}

case "$MODE" in
    report) report ;;
    retrain) retrain ;;
esac
