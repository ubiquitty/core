#!/usr/bin/env bash
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

#
# test-bench-baseline.sh — check-bench-baseline.sh recognises an unfair pair.
#
# Built against a synthetic repository rather than this one, so the answers do
# not move when someone retrains the profile or lands a commit. The history is
# the shape that caused the confusion: a retrain commit, then Source/ commits
# after it, so the retrain has zero drift by construction and everything since
# has more.
#

set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$(dirname "$0")/shell-assertions.sh"
. "$(dirname "$0")/git-fixture.sh"
# Before any git runs, and not as a side effect of the first fixture call. git
# exports GIT_DIR and friends into a hook's environment and they override the
# repository a git command discovers from its directory — `git -C` included,
# because -C sets the working directory and an explicit GIT_DIR skips discovery.
# This file runs `reset --hard` and `checkout -b` against the fixture, and under
# an inherited GIT_DIR those land on the worktree the push came from.
ubq_isolate_git_environment
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT



# ---- a repository with the shape under test --------------------------------
REPO="$WORK/repo"
ubq_make_fixture_repo "$REPO" || exit 1
mkdir -p "$REPO/Scripts" "$REPO/Source" "$REPO/Support/pgo"
cp "$ROOT/Scripts/check-bench-baseline.sh" "$ROOT/Scripts/pgo-staleness.sh" \
    "$ROOT/Scripts/pgo-flags.sh" "$ROOT/Scripts/pgo_localize.py" "$REPO/Scripts/"
# The original history describes the portable profile regardless of the test
# host. Other target selection is exercised separately below, without compiling.
printf '#!/bin/sh\nprintf "#define __APPLE__ 1\\n#define __aarch64__ 1\\n"\n' > "$WORK/apple-cxx"
printf '#!/bin/sh\nprintf "#define __linux__ 1\\n#define __x86_64__ 1\\n"\n' > "$WORK/linux-cxx"
chmod +x "$WORK/apple-cxx" "$WORK/linux-cxx"
export CXX="$WORK/apple-cxx"
unset UBQ_PGO_PROFILE

commit_source() { # <n>
    echo "$1" > "$REPO/Source/file$1.cpp"
    git -C "$REPO" add -A
    git -C "$REPO" commit --quiet -m "Source change $1"
}

echo "seed" > "$REPO/Source/seed.cpp"
echo "profile v1" > "$REPO/Support/pgo/portable-core.profdata"
git -C "$REPO" add -A
git -C "$REPO" commit --quiet -m "Initial with profile"
RETRAIN_OLD="$(git -C "$REPO" rev-parse HEAD)"

for n in 1 2 3; do commit_source "$n"; done
NEAR="$(git -C "$REPO" rev-parse HEAD)"          # 3 commits of drift
for n in 4 5 6 7 8 9 10 11 12 13 14 15; do commit_source "$n"; done
FAR="$(git -C "$REPO" rev-parse HEAD)"           # 15 commits of drift

echo "test-bench-baseline: an unfair pair is refused, a fair one is not"

# 1. The trap: baseline is the retrain, other arm is 15 commits adrift.
OUT="$("$REPO/Scripts/check-bench-baseline.sh" "$RETRAIN_OLD" "$FAR" 2>&1)"
STATUS=$?
check "a retrain-commit baseline is refused" "1" "$STATUS"
contains "and the drift gap is named" "differ by 15 Source/ commits" "$OUT"
contains "and the retrain commit is pointed at" "this commit IS the retrain" "$OUT"

# 2. Two arms sharing one profile, close together: comparable.
OUT="$("$REPO/Scripts/check-bench-baseline.sh" "$NEAR" "$FAR" 2>&1)"
STATUS=$?
check "arms 12 apart are still refused at tolerance 10" "1" "$STATUS"

OUT="$(UBQ_BASELINE_DRIFT_TOLERANCE=20 "$REPO/Scripts/check-bench-baseline.sh" "$NEAR" "$FAR" 2>&1)"
STATUS=$?
check "the tolerance is what decides it" "0" "$STATUS"
contains "and says how far apart they were" "differ by 12" "$OUT"

# 3. --warn measures anyway but still reports.
OUT="$("$REPO/Scripts/check-bench-baseline.sh" --warn "$RETRAIN_OLD" "$FAR" 2>&1)"
STATUS=$?
check "--warn reports and exits 0" "0" "$STATUS"
contains "--warn still explains the problem" "NOT COMPARABLE" "$OUT"

# 4. A retrain between the arms puts them on DIFFERENT profiles, which is the
#    case the drift numbers alone would call fair — both near their own retrain.
echo "profile v2" > "$REPO/Support/pgo/portable-core.profdata"
git -C "$REPO" add -A
git -C "$REPO" commit --quiet -m "Update PGO"
RETRAIN_NEW="$(git -C "$REPO" rev-parse HEAD)"
for n in 16 17; do commit_source "$n"; done
AFTER="$(git -C "$REPO" rev-parse HEAD)"

OUT="$("$REPO/Scripts/check-bench-baseline.sh" "$RETRAIN_OLD" "$AFTER" 2>&1)"
STATUS=$?
check "two freshly-trained arms compare as fair" "0" "$STATUS"
contains "even though the profiles differ" "${RETRAIN_NEW:0:9}" "$OUT"

# 5. A commit no branch or tag reaches. It resolves and it builds, so nothing
#    else notices — but nobody else can check it out and a gc removes it. Rebase
#    merges rewrite every SHA, which is how a baseline comes to name one.
commit_source 18
ORPHAN="$(git -C "$REPO" rev-parse HEAD)"
git -C "$REPO" reset --hard --quiet HEAD~1
REACHABLE="$(git -C "$REPO" rev-parse HEAD)"

"$REPO/Scripts/check-bench-baseline.sh" "$ORPHAN" "$REACHABLE" >/dev/null 2>&1
check "an unreachable arm cannot be scored" "2" "$?"
OUT="$("$REPO/Scripts/check-bench-baseline.sh" "$ORPHAN" "$REACHABLE" 2>&1)"
contains "and says nobody else can reproduce it" "reached by no branch or tag" "$OUT"

# The same subject reachable elsewhere is the usual cause, so it is offered.
# The content differs from the orphan's on purpose: git derives a commit's hash
# from its tree, parent, message and timestamp, so recreating the orphan's
# content under its own subject within the same second reproduces its SHA
# exactly — which makes the orphan reachable and the case under test disappear.
git -C "$REPO" checkout --quiet -b landed
echo "rebased onto a different tree" > "$REPO/Source/file18.cpp"
git -C "$REPO" add -A
git -C "$REPO" commit --quiet -m "Source change 18"
OUT="$("$REPO/Scripts/check-bench-baseline.sh" "$ORPHAN" "$REACHABLE" 2>&1)"
contains "and points at the reachable counterpart" "same subject is reachable" "$OUT"
git -C "$REPO" checkout --quiet -

# 6. --history lists the retrain commits rather than one unlucky pick.
OUT="$("$REPO/Scripts/check-bench-baseline.sh" --history 2>&1)"
STATUS=$?
check "--history exits 0" "0" "$STATUS"
contains "--history names the first retrain" "Initial with profile" "$OUT"
contains "--history names the second retrain" "Update PGO" "$OUT"

# 7. Questions it cannot answer are exit 2, not a verdict.
"$REPO/Scripts/check-bench-baseline.sh" "$RETRAIN_OLD" no-such-ref >/dev/null 2>&1
check "an unknown ref cannot be scored" "2" "$?"

UBQ_PGO_PROFILE=Support/pgo/absent.profdata \
    "$REPO/Scripts/check-bench-baseline.sh" "$RETRAIN_OLD" "$FAR" >/dev/null 2>&1
check "a profile no commit touches cannot be scored" "2" "$?"

# The history check and provenance must describe the profile the Linux consumer
# actually selects, while an explicit profile remains authoritative.
printf 'linux profile\n' > "$REPO/Support/pgo/linux-x86_64-core.profdata"
printf 'linux-trained-source\n' > "$REPO/Support/pgo/linux-x86_64-core.trained-at"
git -C "$REPO" add -A
git -C "$REPO" commit --quiet -m "Linux PGO profile"
OUT="$(CXX="$WORK/linux-cxx" "$REPO/Scripts/check-bench-baseline.sh" --history 2>&1)"
check "Linux profile history exits 0" "0" "$?"
contains "Linux history selects its profile" "Support/pgo/linux-x86_64-core.profdata" "$OUT"
contains "Linux history names its own retrain" "Linux PGO profile" "$OUT"
absent "Linux history omits portable-only retrains" "Update PGO" "$OUT"
OUT="$(CXX="$WORK/linux-cxx" UBQ_PGO_PROFILE=Support/pgo/portable-core.profdata \
    "$REPO/Scripts/check-bench-baseline.sh" --history 2>&1)"
check "explicit profile history exits 0" "0" "$?"
contains "explicit history override selects portable" "Update PGO" "$OUT"

# 8. A trained-at naming a commit this history lacks, as every sidecar does in
#    a clone whose history starts after the training, makes the drift
#    unmeasurable. Counting from the profile file's own commit instead would
#    leave every commit before it out, so the nudge says so once and counts
#    nothing.
staleness() { # <threshold>
    bash -c '. "$1/Scripts/pgo-staleness.sh"; pgo_staleness_warning "$1" Support/pgo/portable-core.profdata "$2"' \
        staleness "$REPO" "$1" 2>&1
}
printf '%040d\n' 7 > "$REPO/Support/pgo/portable-core.trained-at"
OUT="$(staleness 1)"
contains "an unknown trained-at is unmeasurable" "drift cannot be measured from history here" "$OUT"
contains "and the gate that measures it is named" "check-pgo-applies.py --recorded-budget" "$OUT"
absent "and nothing is counted from the file's commit" "commits old" "$OUT"
printf '%s\n' "$RETRAIN_OLD" > "$REPO/Support/pgo/portable-core.trained-at"
OUT="$(staleness 1)"
contains "a trained-at in this history is counted from" "Source/ commits old" "$OUT"
rm -f "$REPO/Support/pgo/portable-core.trained-at"

python3 - "$ROOT" "$REPO" "$WORK/apple-cxx" "$WORK/linux-cxx" <<'PY'
import hashlib
import importlib.util
import os
from pathlib import Path
import sys
from unittest.mock import patch

root, repository, apple, linux = sys.argv[1:]
sys.path.insert(0, str(Path(root) / 'Scripts'))


def load(name):
    specification = importlib.util.spec_from_file_location(name, Path(root) / 'Scripts' / (name + '.py'))
    module = importlib.util.module_from_spec(specification)
    specification.loader.exec_module(module)
    return module


stamp = load('stamp-bench-baseline')
applies = load('check-pgo-applies')
portable = 'Support/pgo/portable-core.profdata'
native = 'Support/pgo/linux-x86_64-core.profdata'
search_path = os.environ.get('PATH', os.defpath)
with patch.dict(os.environ, {}, clear=True):
    # Use the invoking process's search path so bash can find python3.
    os.environ['PATH'] = search_path
    assert applies.default_consumer_profile(repository, apple) == portable
    assert applies.default_consumer_profile(repository, linux) == native
    with patch.dict(os.environ, {'UBQ_PGO_PROFILE': str(Path(repository) / portable)}):
        assert applies.default_consumer_profile(repository, linux) == portable
    with patch.dict(os.environ, {'UBQ_PGO_PROFILE': '/external/profile.profdata'}):
        assert applies.default_consumer_profile(repository, linux) == '/external/profile.profdata'

    def identity(path, trained='unrecorded'):
        digest = hashlib.sha1((Path(repository) / path).read_bytes()).hexdigest()[:12]
        return f'{digest}, trained at {trained}'

    with patch.object(stamp.platform, 'system', return_value='Linux'), \
            patch.object(stamp.platform, 'machine', return_value='x86_64'):
        assert stamp.profile_identity(repository) == identity(native, 'linux-traine')
        assert stamp.profile_identity(os.path.relpath(repository)) == identity(native, 'linux-traine')
        with patch.dict(os.environ, {'UBQ_PGO_PROFILE': portable}):
            assert stamp.profile_identity(repository) == identity(portable)
        with patch.dict(os.environ, {'UBIQUITTY_PGO': 'OFF'}):
            assert stamp.profile_identity(repository).startswith('none ')
    with patch.object(stamp.platform, 'system', return_value='Darwin'), \
            patch.object(stamp.platform, 'machine', return_value='arm64'):
        assert stamp.profile_identity(repository) == identity(portable)
print('profile metadata: target selection, override, sidecar, and PGO-off checks passed')
PY
check "profile metadata follows the selected profile" "0" "$?"

report "test-bench-baseline" "the check separates a fair pair from an unfair one"
