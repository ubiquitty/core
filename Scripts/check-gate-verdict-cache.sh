#!/usr/bin/env bash
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

# check-gate-verdict-cache.sh — the cache's key is checked, not trusted.
#
# Scripts/gate-verdict-cache.sh replays a gate's pass when the tree has not
# changed, so an input missing from the key turns into a gate that did not run
# reporting as a gate that found nothing. That failure is silent by
# construction: the run is green and faster, which is what it would look like if
# it were working.
#
# So each thing the key claims to cover is asserted here against the live
# repository, with a temporary ref for the cases that need one. The arms use the
# real checkout rather than a scratch `git init`, because a temporary repository
# created under a hook inherits GIT_DIR and quietly answers for the wrong tree.
#
#     ./Scripts/check-gate-verdict-cache.sh --self-test

set -uo pipefail

cd "$(dirname "$0")/.." || exit 2
# shellcheck source=Scripts/gate-verdict-cache.sh
. Scripts/gate-verdict-cache.sh

PROBE_DIRECTORY="$(mktemp -d "${TMPDIR:-/tmp}/ubq-verdict-probe.XXXXXX")" || exit 2
PROBE_REF="refs/ubiquitty/verdict-probes/${PROBE_DIRECTORY##*/}"
PROBE_REF_TIP=""
PROBE_FILE="Source/zz_verdict_cache_${PROBE_DIRECTORY##*/}.h"
PROBE_FILE_OWNED=0

cleanup() {
    if [ -n "$PROBE_REF_TIP" ]; then
        git update-ref -d "$PROBE_REF" "$PROBE_REF_TIP" 2>/dev/null || true
    fi
    if [ "$PROBE_FILE_OWNED" -eq 1 ]; then
        rm -f "$PROBE_FILE"
    fi
    rmdir "$PROBE_DIRECTORY"
}
trap cleanup EXIT

setProbeRef() {
    local resolved
    resolved="$(git rev-parse --verify "$1^{commit}")" || return 2
    git update-ref "$PROBE_REF" "$resolved" "$PROBE_REF_TIP" || return 2
    PROBE_REF_TIP="$resolved"
}

createProbeFile() {
    if ! (set -C; printf '// probe\n' > "$PROBE_FILE"); then
        echo "check-gate-verdict-cache: cannot create owned probe $PROBE_FILE" >&2
        return 2
    fi
    PROBE_FILE_OWNED=1
}

failures=0
check() { # <claim> <yes|no>
    if [ "$2" = yes ]; then
        printf '  ok       %s\n' "$1"
    else
        printf '  BROKEN   %s\n' "$1"
        failures=$((failures + 1))
    fi
}

held() { # <a> <b> <same|differ>
    if [ "$3" = same ]; then
        [ "$1" = "$2" ] && echo yes || echo no
    else
        [ "$1" != "$2" ] && echo yes || echo no
    fi
}

# This gate asserts that the key covers what it claims, which is a property of
# the code rather than of whether this run happens to be using the cache. A run
# with caching switched off still has to know the key is sound, so the arms run
# either way.
# Exported rather than set: gate_verdict_init reads it from the sourced
# library, and the gates this runs read it too.
export UBIQUITTY_GATE_CACHE=on
gate_verdict_init
if [ "$GATE_VERDICT_ENABLED" != 1 ]; then
    echo "check-gate-verdict-cache: the cache does not initialise here — nothing asserted" >&2
    exit 2
fi

# A gate reading a ref asks about the fork point, so someone else landing on
# main leaves this branch's verdicts standing and a rebase retires them.
#
# The key records `git merge-base <ref> HEAD`, so a "moved" fork point has to
# be a commit whose merge base with HEAD is not the fork point itself. Any
# ancestor of HEAD that is also a descendant of the fork point resolves back to
# it, and any ancestor of the fork point resolves to itself; the fork point's
# parent is the nearest commit of the second kind. Without a parent (a root
# commit, or a depth-1 checkout) a commit of the empty tree stands in: unrelated
# to HEAD, so the key records the commit itself, which is a fork point that
# moved just the same.
movedForkPoint() { # <forkPoint>
    git rev-parse --verify --quiet "$1^^{commit}" \
        || git -c user.name=probe -c user.email=probe@ubiquitty commit-tree \
            4b825dc642cb6eb9a060e54bf8d69288fbee4904 -m 'verdict-cache probe'
}

forkPoint="$(git merge-base origin/main HEAD 2>/dev/null || true)"
if [ -n "$forkPoint" ]; then
    key() { gate_verdict_key python3 ./Scripts/check-slop.py --since "$PROBE_REF" .; }
    setProbeRef "$forkPoint" || exit 2
    atFork="$(key)"
    setProbeRef origin/main || exit 2
    advanced="$(key)"
    setProbeRef "$(movedForkPoint "$forkPoint")" || exit 2
    rebased="$(key)"
    check "main advancing past the fork point keeps this branch's verdicts" \
        "$(held "$atFork" "$advanced" same)"
    check "a fork point that moves retires them" \
        "$(held "$atFork" "$rebased" differ)"

    # The choice must hold whatever the branch's depth over main. HEAD~N is the
    # fork point of a branch carrying exactly N commits, and the moved point
    # must not resolve back to it.
    for depth in 0 1 5; do
        shapeFork="$(git rev-parse --verify --quiet "HEAD~$depth^{commit}" || true)"
        [ -n "$shapeFork" ] || continue
        setProbeRef "$shapeFork" || exit 2
        shapeAtFork="$(key)"
        setProbeRef "$(movedForkPoint "$shapeFork")" || exit 2
        shapeRebased="$(key)"
        check "a moved fork point differs on a branch $depth commit(s) deep" \
            "$(held "$shapeAtFork" "$shapeRebased" differ)"
    done
else
    echo "  skipped  no origin/main in this checkout, so the fork-point arms cannot run"
fi

# The fingerprint is taken once, before any gate runs.
# Scripts/check-gate-subjects.sh plants an untracked file and removes it again,
# and a key that moved underneath the run would store verdicts under keys that
# never recur.
during="$GATE_VERDICT_TREE"
createProbeFile || exit 2
check "the fingerprint does not move while the run is going" \
    "$(held "$during" "$GATE_VERDICT_TREE" same)"

# A file the tree does not track yet can carry the violation, so a run that
# starts with it present sees a different tree.
withProbe="$(gate_verdict_fingerprint)"
rm -f "$PROBE_FILE"
PROBE_FILE_OWNED=0
withoutProbe="$(gate_verdict_fingerprint)"
check "an untracked file changes the fingerprint" \
    "$(held "$withProbe" "$withoutProbe" differ)"
check "removing it again restores the earlier fingerprint" \
    "$(held "$withoutProbe" "$during" same)"

# Some gates answer about compiled output rather than source, and those bytes are
# gitignored so the tree fingerprint cannot see them. With the build present
# check-core-subset-symbols --self-test passes; with it gone the same tree says
# "no object defined a single symbol, so the nm pass is reading nothing" -- and
# a cache keyed on the tree alone replays the pass.
#
# Asserted by varying the digest rather than by moving the build. Renaming
# BuildHostTests out of the way and back is not a gate's business -- interrupted
# between the two renames it leaves the build somewhere the user has to go find
# -- and it reads the real tree twice, so a build running in another terminal
# changes the answer in between and fails the arm on a cache that is working.
# Scripts/gate_corpus.py --self-test holds the
# digest itself to noticing a file appear, grow, and be rebuilt, on a probe root
# it creates and removes.
# The digest is SAVED and put back, never read a second time. The "differ" arm
# uses a synthetic value and is immune; the "same" arm restoring by calling
# gate_verdict_build_state() again is the double read above in another form.
# Any build touching a root between the two reads moves the digest, so the arm
# fails on a cache that is working. Under `ctest -j8` something is almost
# always building.
#
# The claim does not weaken. "The same build gives the same key" is about the
# KEY being determined by its inputs, and feeding back the identical digest
# tests exactly that. Whether the DIGEST is stable is a different claim, and it
# already lives where the comment above says: gate_corpus.py --self-test, on a
# probe root it owns.
originalBuild="$GATE_VERDICT_BUILD"
withBuild="$(gate_verdict_key ./Scripts/check-core-subset-symbols.py --self-test)"
GATE_VERDICT_BUILD="a-different-build"
differentBuild="$(gate_verdict_key ./Scripts/check-core-subset-symbols.py --self-test)"
GATE_VERDICT_BUILD="$originalBuild"
restored="$(gate_verdict_key ./Scripts/check-core-subset-symbols.py --self-test)"
check "a build that changed retires the verdicts that read it" \
    "$(held "$withBuild" "$differentBuild" differ)"
check "the same build gives the same key" \
    "$(held "$withBuild" "$restored" same)"

# A digest that cannot be computed is the empty string, which is the same on
# every build -- the failure above wearing the fix's clothes.
check "the build digest was actually computed" \
    "$([ -n "$GATE_VERDICT_BUILD" ] && echo yes || echo no)"

# The same script asked a different question is a different verdict.
plain="$(gate_verdict_key ./Scripts/check-acronyms.py)"
withFlag="$(gate_verdict_key ./Scripts/check-acronyms.py --full)"
check "the command's arguments are inside the key" \
    "$(held "$plain" "$withFlag" differ)"

# Only a pass is remembered: a red verdict is re-asked rather than replayed.
stored="$(find "$GATE_VERDICT_DIR" -name '*.pass' 2>/dev/null | wc -l | tr -d ' ')"
gate_verdict "ubq-failing-probe" sh -c 'echo no; exit 1' > /dev/null 2>&1
storedAfter="$(find "$GATE_VERDICT_DIR" -name '*.pass' 2>/dev/null | wc -l | tr -d ' ')"
check "a failing gate leaves nothing behind to replay" \
    "$(held "$stored" "$storedAfter" same)"

# A gate that stood down checked nothing, and says so with a "<gate>: SKIPPED"
# line while exiting zero. The key covers the tree and some build roots, not
# every artefact a gate looks for, so a stand-down kept as a pass was replayed
# after the build it missed appeared: the macOS and GTK smokes read SKIPPED
# through a full check-style run with both builds present.
stored="$(find "$GATE_VERDICT_DIR" -name '*.pass' 2>/dev/null | wc -l | tr -d ' ')"
gate_verdict "ubq-standing-down-probe" sh -c 'echo "ubq-standing-down-probe: SKIPPED - no build here"' > /dev/null 2>&1
gate_verdict "ubq-nested-standing-down-probe" sh -c 'echo "section: ok"; echo "  ubq-inner-probe: SKIPPED - no tool"' > /dev/null 2>&1
storedAfter="$(find "$GATE_VERDICT_DIR" -name '*.pass' 2>/dev/null | wc -l | tr -d ' ')"
check "a gate that stood down leaves nothing behind to replay" \
    "$(held "$stored" "$storedAfter" same)"

if [ "$failures" -ne 0 ]; then
    echo "check-gate-verdict-cache: $failures claim(s) the key does not keep" >&2
    exit 1
fi
echo "check-gate-verdict-cache: ok (every input the key claims to cover moves it)"
