#!/usr/bin/env bash
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0
#
# Scripts/pre-push-gates-lib.sh — what every repository's Scripts/pre-push-gates.sh
# shares: the tier, the runner and its verdict cache, the artifact root, and the
# summary that ends a run.
#
# Sourced by a repository's own Scripts/pre-push-gates.sh, which is the list of
# gates that repository answers for. That script sources this file, then the gate
# files of the repositories it holds, calls the gates of the tier, and ends with
# `ubq_pre_push_finish`:
#
#     . "$UBQ_CORE_ROOT/Scripts/pre-push-gates-lib.sh"
#     . "$UBQ_CORE_ROOT/Scripts/pre-push-gates-core.sh"
#     ...
#     ubq_pre_push_finish
#
# The checkout judged is the one holding the sourcing script. A gate is named by
# the `./Scripts/` path it has in the repository that owns it, and `run` points
# the command at the checkout that holds it (Scripts/ubq-roots.sh), so a gate of
# ubiquitty-core runs from the library in a repository that carries it as a
# submodule. `run_for` runs a gate only in the repositories it names and counts
# the rest, so a checkout that does not hold a gate's subject says so instead of
# passing it vacuously.
#
# The gates a push answers to, run on this machine before the
# push. A workflow that is disabled or queued is indistinguishable from one that
# passed: nothing goes red, so the tree looks green while nothing has executed.
# These gates are the ones that decide whether a push is sound.
#
#   TIER=lint    every gate that needs no build                       no toolchain
#   TIER=quick   style + SSOT + PGO drift + host build + the arms the
#                push's changes reach                                 DEFAULT
#   TIER=ctest   quick + the ctest suite                              ON REQUEST
#   TIER=linux   quick + Core and the GTK frontend on real Linux
#   TIER=full    linux + macOS app + snapshot regressions
#
# How long a tier takes depends on what the push changes, because each arm runs
# only when the push reaches its sources. A push that changes CMakeLists.txt or
# a module it includes reaches every iOS gate, and the iOS UI tests on a
# simulator dominate quick from a cold build root. The PGO total-drift gate
# compiles every portable-core unit on every run.
#
# UBIQUITTY_PREPUSH selects the tier for one push; `git config
# ubiquitty.prepushTier <tier>` sets this clone's standing choice, for a machine
# whose limits are a property of the machine rather than of the push. There is no
# tier that runs nothing — see the note on the TIER assignment below.
#
# Successful gates are cached in this worktree's Git metadata. The key covers
# the committed and uncommitted source snapshot, origin/main, command, tier,
# relevant build environment, and toolchain identity; failures and skips are
# never cached. Set UBIQUITTY_PREPUSH_CACHE=off to force every selected gate.
#
# The linux tier runs through Scripts/run-linux.sh, so the Linux-only breaks a
# macOS compiler never sees are caught here: libstdc++ against libc++, GNU ld's
# left-to-right archive scan against Apple's whole-set search, -Werror arms gcc
# raises and clang does not, all of which reach main when nothing compiles that
# path.
#
# UNCOVERED, and named rather than dropped: musl static, coverage floors,
# bounded fuzz, and the bench gate. Bench is excluded for correctness, not cost —
# a contended machine against a baseline measured on an idle runner is wrong in
# both directions.
#
# wasm32 is not on that list: gate_wasm runs from the quick tier. Its coverage
# is conditional twice over, so read it as "covered when it can matter" rather
# than "always" — the gate returns early when the branch touches none of
# Source/Core, Source/Base or Source/Platform/Web, and reports SKIPPED,
# counted as having checked nothing, on a machine without emsdk.
set -uo pipefail

# Preserve the caller's scheduling priority. Coordinate compile jobs through
# the build-job limits and reserve a quiet machine for timing-sensitive tests.
UBQ_LIB="$(cd "${BASH_SOURCE[0]%/*}" && pwd)"
# shellcheck source=Scripts/gate-report-lib.sh
source "$UBQ_LIB/gate-report-lib.sh"
# shellcheck source=Scripts/pre-push-cache.sh
source "$UBQ_LIB/pre-push-cache.sh"
# shellcheck source=Scripts/pre-push-runner.sh
source "$UBQ_LIB/pre-push-runner.sh"

# The checkout judged is the one holding the script that sourced this file.
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT" || exit 1
# shellcheck source=Scripts/ubq-roots.sh
. "$UBQ_LIB/ubq-roots.sh"
# Build output lives outside the checkout; see Scripts/build-root.sh.
# shellcheck source=Scripts/build-root.sh
. "$UBQ_LIB/build-root.sh"

# The renderer-arm selection and the backlog-growth gate both diff against
# origin/main. A missing ref makes that diff fail, and ubq_renderer_arms then
# reads the failure as an empty change set and skips both renderer gates in
# silence rather than refusing. Verify the ref up front, so a checkout that has
# not fetched fails loud instead of running a subset that reads as a clean pass.
if ! git rev-parse --verify --quiet origin/main >/dev/null 2>&1; then
    echo "pre-push gates: origin/main is not resolvable — fetch it first (git fetch origin main)." >&2
    echo "  The renderer-arm diff and the backlog-growth gate both need it; without it they" >&2
    echo "  would skip in silence and the run would read as a pass." >&2
    exit 1
fi

# The quick tier runs focused checks; ctest requests the complete suite.
# Choose a tier using the per-invocation environment, then clone-local Git
# configuration, then the quick default. Unknown tiers fall back to quick;
# no tier disables every gate.
#
#   git config ubiquitty.prepushTier lint    this clone builds nothing on push
#   UBIQUITTY_PREPUSH=full git push          overrides it for one push
TIER="${UBIQUITTY_PREPUSH:-}"
if [ -z "$TIER" ]; then
    TIER="$(git config --get ubiquitty.prepushTier 2>/dev/null || true)"
    TIER_SOURCE="git config ubiquitty.prepushTier"
fi
TIER="${TIER:-quick}"
case "$TIER" in
    lint | quick | default | ctest | linux | full) ;;
    *)
        echo "pre-push gates: unknown tier '$TIER'${TIER_SOURCE:+ (from $TIER_SOURCE)} — running quick" >&2
        TIER=quick
        ;;
esac

# Limit CTest fan-out because each test job may itself start a build.
# UBIQUITTY_PREPUSH_JOBS overrides the default for the available machine budget.
# Read by the gate files the sourcing script adds.
# shellcheck disable=SC2034
JOBS="${UBIQUITTY_PREPUSH_JOBS:-$( n=$( (sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4) ); echo $(( n > 3 ? n / 2 : 2 )) )}"

ubq_gate_cache_init "$ROOT" "$TIER"

# One artifact root for this run. ci_batteries.py puts a battery's stage logs
# and its events.jsonl under UBQ_CI_ROOT/<battery>, and its report reads the
# same root, so everything this run starts agrees on one directory that no
# other run on this machine has. Without it the root is /tmp/ubiquitty-ci for
# every run on the machine, which two pushes gating at once would share.
# Removed when every gate passed; kept and named when one failed, since that
# is where a failed battery's logs are. UBQ_ rather than UBIQUITTY_ because
# the verdict cache's key covers every UBIQUITTY_ variable, and a path that is
# new on every run would make every verdict new on every run.
UBQ_CI_ROOT="${TMPDIR:-/tmp}"
UBQ_CI_ROOT="$(mktemp -d "${UBQ_CI_ROOT%/}/ubiquitty-ci-XXXXXX")" || exit 1
export UBQ_CI_ROOT

# On exit rather than at the two verdicts, so an interrupted push or a run
# that set -u ends does not leave a root behind either. An interrupt exits
# with its own status, so it reads as the failure it is rather than as a pass.
ubq_ci_root_finish() {
    local status="${1:-$?}"
    # Kept only when a failed run left something in it; an empty root names
    # nothing a reader could open.
    if [ "$status" -ne 0 ] && [ -n "$(ls -A "$UBQ_CI_ROOT" 2>/dev/null)" ]; then
        echo "  battery artifacts: $UBQ_CI_ROOT" >&2
        return
    fi
    rm -rf "$UBQ_CI_ROOT"
}
trap ubq_ci_root_finish EXIT
trap 'exit 130' INT

# The files the push changes against the merge base with origin/main, with each
# library this repository carries read as what lies between its pins: a library
# moves by its pinned commit, so the superproject's diff names the mount, not
# the files. A mount whose pins cannot be read in its checkout stays as its
# path, which a caller reads as "anything may have changed".
ubq_changed_with_libraries() {
    local changed library pinned pinnedHere expanded rest
    changed="$(git diff --name-only origin/main...HEAD 2>/dev/null)" || return 1
    for library in ubiquitty-core ubiquitty-kit; do
        grep -qxF "Libraries/$library" <<<"$changed" || continue
        pinned="$(git rev-parse --verify --quiet "origin/main:Libraries/$library" 2>/dev/null)" || continue
        pinnedHere="$(git rev-parse --verify --quiet "HEAD:Libraries/$library" 2>/dev/null)" || continue
        expanded="$(git -C "Libraries/$library" diff --name-only "$pinned" "$pinnedHere" 2>/dev/null)" || continue
        [ -n "$expanded" ] || continue
        rest="$(grep -vxF "Libraries/$library" <<<"$changed")" || rest=""
        changed="$rest"$'\n'"$expanded"
    done
    printf '%s\n' "$changed"
}

# The end of a run: what stood down, what warned, what failed and where its log
# is, and the line saying how much a pass covers. Returns the run's status.
ubq_pre_push_finish() {
    # Whether anyone has built what the last merges produced. A rebase merge
    # replays a branch onto a base it was never built against and pushes without
    # building the result, so the first tree in which two changes coexist is main
    # itself and no gate here has seen it — these gates measure the tree this
    # author has.
    #
    # Said at the end of a push because that is the moment someone is looking at
    # this output and has the repository in hand. It is not a failure: another
    # checkout's merge being unverified is not a reason to refuse this push. It
    # is said only where the script that verifies a merge is present.
    local verifiedFile="$UBQ_BUILD_ROOT/.merged-main-verified" mainTip verified behind
    ubqVerifier="$ROOT/Scripts/check-merged-main.sh"
    mainTip="$(git -C "$ROOT" rev-parse --verify --quiet origin/main 2>/dev/null || true)"
    verified="$(cat "$verifiedFile" 2>/dev/null || true)"
    if [ -e "$ubqVerifier" ] && [ -n "$mainTip" ] && [ "$mainTip" != "$verified" ]; then
        echo >&2
        if [ -z "$verified" ]; then
            echo "pre-push gates: no build has verified origin/main in this checkout." >&2
        else
            behind="$(git -C "$ROOT" rev-list --count "$verified..$mainTip" 2>/dev/null || echo "?")"
            echo "pre-push gates: origin/main is $behind commit(s) past the last build that" >&2
            echo "  verified it (${verified:0:9})." >&2
        fi
        echo "  A rebase merge builds nothing, so those commits have coexisted only here:" >&2
        echo "      ./Scripts/check-merged-main.sh" >&2
    fi

    echo >&2
    ubq_gate_report_skipped
    ubq_gate_report_warned
    ubq_gate_report_elsewhere
    local index=0 name log
    if [ "${#FAILED[@]}" -gt 0 ]; then
        gate_failed "pre-push gates" "gate(s) failed" "${FAILED[@]}"
        # The name of a gate is not a diagnosis. By the time this prints, the
        # detail is forty lines up and past whatever else ran; a reader who
        # scrolls to the bottom gets "ctest suite" and nothing to act on. So each
        # failure repeats the lines that name what broke, and where the whole log
        # is.
        for name in "${FAILED[@]}"; do
            log="${FAILED_LOG[$index]:-}"
            echo "  - $name" >&2
            if [ -n "$log" ] && [ -f "$log" ]; then
                # ctest names its casualties under a header; anything else falls
                # back to its own last lines rather than to silence.
                if grep -q 'The following tests FAILED:' "$log"; then
                    sed -n '/The following tests FAILED:/,$p' "$log" | sed -n '2,9p' \
                        | sed 's/^/      /' >&2
                else
                    tail -4 "$log" | sed 's/^/      /' >&2
                fi
                echo "      full log: $log" >&2
            fi
            index=$((index + 1))
        done
        echo >&2
        return 1
    fi
    echo "pre-push gates: ${#PASSED[@]} passed (${#CACHED[@]} cached). Uncovered here: musl, coverage," >&2
    echo "  fuzz, the bench gate, and every test this diff does not reach — the" >&2
    echo "  \"tests reaching the diff\" line above says which ran, and says so when none" >&2
    echo "  did. UBIQUITTY_PREPUSH=default adds the whole suite, =linux the Linux build." >&2
    return 0
}
