#!/usr/bin/env bash
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

#
# check-bench-baseline.sh — do these two commits carry comparable PGO profiles?
#
# An A/B between two commits builds each with the profile checked in at that
# commit, so the arms differ by their code and by how each one's profile places
# it. Equal drift cancels that. Unequal drift does not, and no amount of
# pairing, warmup or repetition separates it out — both arms are internally
# consistent and the difference is real, it just is not the difference anyone
# meant to measure.
#
# The sign is not predictable, which is why this refuses rather than corrects.
# A profile whose counters no longer match compiles the unmatched bodies COLD
# rather than leaving them alone, so drift does not decay toward neutral — it
# places code confidently wrong. On this tree PGO has helped at one commit and
# hurt at another on the same benchmark: whichever arm carries more drift,
# neither the size nor the direction of what it carries can be assumed.
#
# The shape that costs a day. A retrain commit is an attractive baseline: it is
# a known-good point, often tagged or titled for it. It is also the one commit
# in the history whose profile drift is zero by construction, because the commit
# is the retrain. Comparing anything against it puts the two arms under
# different codegen, and the delta reads as code.
#
# Usage:
#   ./Scripts/check-bench-baseline.sh <old-ref> <new-ref>
#   ./Scripts/check-bench-baseline.sh --warn <old-ref> <new-ref>   report, exit 0
#   ./Scripts/check-bench-baseline.sh --history                    list the traps
#
# Exit 0 comparable, 1 not comparable, 2 the question could not be asked.
#

set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# shellcheck source=Scripts/pgo-flags.sh
. "$ROOT/Scripts/pgo-flags.sh"
PROFILE="$(ubiquitty_pgo_profile "$ROOT" "${CXX:-c++}")" || exit 2
PROFILE="${PROFILE#"$ROOT"/}"
# A GUESS, and there is no measurement to key it off. What a drift gap costs
# depends on which symbols drifted and on how well the training corpus resembles
# the workload being timed — two commits apart can invalidate the hottest
# function in the build via a rename, and sixty apart can touch nothing that
# matters. Commits are a proxy for none of that.
#
# So this is set to catch zero-against-sixty-three rather than to arbitrate
# twelve against nineteen. A tight threshold on a proxy this loose is how a gate
# earns the reputation that gets it routed around.
TOLERANCE="${UBQ_BASELINE_DRIFT_TOLERANCE:-10}"
WARN_ONLY=0

[ "${1:-}" = "--warn" ] && { WARN_ONLY=1; shift; }

# Every commit that retrained the profile, which is every commit whose drift is
# zero by construction. Each is an attractive baseline and each biases an A/B
# against anything later. Printed as a list because the trap is not one unlucky
# commit but a class the history regenerates at every retrain.
if [ "${1:-}" = "--history" ]; then
    . "$ROOT/Scripts/pgo-staleness.sh"
    echo "Commits that retrained $PROFILE — each has zero drift against itself:"
    git -C "$ROOT" log --all --format='%h %ad %s' --date=short -- "$PROFILE" |
        while IFS= read -r line; do printf '  %s\n' "$line"; done
    echo
    echo "Comparing anything later against one of these measures profile freshness"
    echo "alongside code. Pass two refs to check a specific pair."
    exit 0
fi

if [ "$#" -ne 2 ]; then
    sed -n '/^#!/d; /^[^#]/q; s/^# \{0,1\}//; p' "$0"
    exit 2
fi

OLD="$1"
NEW="$2"

# shellcheck source=Scripts/pgo-staleness.sh
. "$ROOT/Scripts/pgo-staleness.sh"

for ref in "$OLD" "$NEW"; do
    if ! git -C "$ROOT" rev-parse --verify --quiet "$ref^{commit}" >/dev/null; then
        echo "check-bench-baseline: '$ref' is not a commit in this repository" >&2
        exit 2
    fi
done

# A commit no branch or tag reaches exists only in the local object store. It
# resolves, it builds, and it measures — and nobody else can check it out, so
# the result cannot be reproduced and a gc removes the arm entirely. Rebase
# merges rewrite every SHA, so the pre-merge commit of a landed change is the
# common way to end up holding one.
unreachable() { # <ref>
    local sha
    sha="$(git -C "$ROOT" rev-parse "$1")"
    [ -z "$(git -C "$ROOT" branch --all --contains "$sha" 2>/dev/null)" ] \
        && [ -z "$(git -C "$ROOT" tag --contains "$sha" 2>/dev/null)" ]
}

for ref in "$OLD" "$NEW"; do
    if unreachable "$ref"; then
        sha="$(git -C "$ROOT" rev-parse --short "$ref")"
        echo "check-bench-baseline: $sha ($ref) is reached by no branch or tag." >&2
        echo "  It is in this clone's object store and nowhere else, so no one can" >&2
        echo "  reproduce a measurement against it and a gc removes it." >&2
        subject="$(git -C "$ROOT" log -1 --format=%s "$ref")"
        landed="$(git -C "$ROOT" log --all --format='%h %s' --grep="^$(
            printf '%s' "$subject" | sed 's/[][\.*^$\/]/\\&/g')\$" | head -1)"
        [ -n "$landed" ] && echo "  A commit with the same subject is reachable: $landed" >&2
        exit 2
    fi
done

oldDrift="$(pgo_drift_count "$ROOT" "$PROFILE" "$OLD")"
newDrift="$(pgo_drift_count "$ROOT" "$PROFILE" "$NEW")"
if [ -z "$oldDrift" ] || [ -z "$newDrift" ]; then
    echo "check-bench-baseline: no commit in this history touches $PROFILE, or its" >&2
    echo "  $(pgo_trained_at_path "$PROFILE") names a commit this history lacks, so profile" >&2
    echo "  freshness cannot be compared. Pass UBQ_PGO_PROFILE if it moved." >&2
    exit 2
fi

oldProfile="$(pgo_profile_commit "$ROOT" "$PROFILE" "$OLD")"
newProfile="$(pgo_profile_commit "$ROOT" "$PROFILE" "$NEW")"

describe() { # <ref> <drift> <profile commit>
    local marker=""
    # A ref whose own commit retrained the profile is the trap this exists for,
    # so it is named rather than left to be read out of a zero.
    [ "$(git -C "$ROOT" rev-parse "$1")" = "$3" ] && marker="  <- this commit IS the retrain"
    printf '  %-24s profile %s  drift %4s Source/ commits%s\n' \
        "$(git -C "$ROOT" rev-parse --short "$1")  $1" "${3:0:9}" "$2" "$marker"
}

echo "check-bench-baseline: $PROFILE"
describe "$OLD" "$oldDrift" "$oldProfile"
describe "$NEW" "$newDrift" "$newProfile"

gap=$(( oldDrift > newDrift ? oldDrift - newDrift : newDrift - oldDrift ))

echo
if [ "$gap" -lt "$TOLERANCE" ]; then
    echo "comparable: the arms differ by $gap Source/ commits of profile drift"
    exit 0
fi

cat >&2 <<EXPLAIN
NOT COMPARABLE: the arms differ by $gap Source/ commits of profile drift.

Each arm builds with the profile checked in at its own commit, so this
comparison measures code AND profile placement together. Unmatched counters
compile a body cold rather than leaving it alone, so the arm carrying more
drift is not reliably the slower one — the sign is not predictable from the
gap.

To make the question answerable:

  UBIQUITTY_PGO=OFF on both arms isolates the code change from the profile,
  and its number is interpretable on its own.

Retraining is NOT the general answer. It puts both arms on one profile, which
removes the asymmetry, and it also changes what that profile is worth to each
tree — a retrain has measured slower here than the stale profile it replaced.
Reach for it to fix a build, not to fix a comparison.

Re-run with --warn to measure anyway, knowing what the number includes.
EXPLAIN

[ "$WARN_ONLY" -eq 1 ] && exit 0
exit 1
