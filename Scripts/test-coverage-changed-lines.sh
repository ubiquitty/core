#!/usr/bin/env bash
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

# test-coverage-changed-lines.sh — self-contained test for the changed-line
# coverage gate. Builds a throwaway git repo whose HEAD adds covered, uncovered,
# and exempt lines to a TU, crafts a matching lcov, and asserts the gate's verdict
# and exit code for each case. Deterministic; no network, no real build.

set -euo pipefail
TOOL="$(cd "$(dirname "$0")" && pwd)/coverage-changed-lines.py"
WORK="$(mktemp -d /tmp/cov-gate-test.XXXXXX)"
trap 'rm -rf "$WORK"' EXIT
# Resolved before the cd: $0 is relative, so it stops meaning anything once the
# working directory moves into the fixture.
UBQ_TESTS="$(cd "$(dirname "$0")/../Tests" && pwd)"
cd "$WORK"

. "$UBQ_TESTS/git-fixture.sh"
# Written out rather than left to ubq_make_fixture_repo, which also calls it: the
# `checkout -b` below is aimed at a fixture and would land on the pushing
# worktree under a hook's GIT_DIR, and protection that depends on what another
# helper happens to do is protection an unrelated edit can remove.
ubq_isolate_git_environment
ubq_make_fixture_repo "$WORK"
git config user.email t@t
git config user.name t
git -c init.defaultBranch=main checkout -q -b main 2>/dev/null || true

# base commit: a TU with one line.
mkdir -p Source/Demo
printf 'int base() { return 0; }\n' > Source/Demo/Demo.cpp
git add -A && git commit -qm base
BASE=$(git rev-parse HEAD)

# head commit: add four lines — covered, uncovered, uncovered-but-exempt, and a
# non-coverable brace line the lcov omits (must not be flagged).
cat > Source/Demo/Demo.cpp <<'EOF'
int base() { return 0; }
int covered(int x) { return x + 1; }
int uncovered(int x) { return x - 1; }
int spared(int x) { return x * 2; } // coverage-exempt(security): unreachable without a live socket
int bracey(int x)
{
    return x;
}
EOF
# A changed VENDORED TU with an uncovered line, absent from the lcov: the gate
# must SKIP it (ThirdParty is out of our coverage denominator), not flag it as
# an uninstrumented gap.
mkdir -p Source/ThirdParty/re2/re2
printf 'int vendored(int x) { return x + 7; }\n' > Source/ThirdParty/re2/re2/vendor.cc
git add -A && git commit -qm head
HEAD=$(git rev-parse HEAD)

# Craft an lcov: line 2 covered (hit), line 3 uncovered (0), line 4 uncovered (0)
# but exempt, line 7 covered. Lines 5/6/8 (signature/braces) have no da entry —
# llvm-cov would not mark them coverable, so the gate must ignore them.
cat > cov.lcov <<EOF
SF:$WORK/Source/Demo/Demo.cpp
DA:1,1
DA:2,5
DA:3,0
DA:4,0
DA:7,3
end_of_record
EOF

fail=0
check() { # <desc> <expected-exit> <actual-exit>
    if [ "$2" = "$3" ]; then echo "ok   — $1"; else echo "FAIL — $1 (want exit $2, got $3)"; fail=1; fi
}

# Case 1: the real run — one uncovered non-exempt line (3) must fail the gate.
set +e
OUT=$(python3 "$TOOL" --lcov cov.lcov --base "$BASE" --head "$HEAD" --root "$WORK" 2>&1)
rc=$?
set -e
check "uncovered changed line fails the gate" 1 "$rc"
echo "$OUT" | grep -q "UNCOVERED Source/Demo/Demo.cpp:3" \
    && echo "ok   — flags the exact uncovered line" || { echo "FAIL — missing UNCOVERED :3"; fail=1; }
echo "$OUT" | grep -q "EXEMPT Source/Demo/Demo.cpp:4.*security" \
    && echo "ok   — records the audited exemption" || { echo "FAIL — missing EXEMPT :4"; fail=1; }
echo "$OUT" | grep -q "Demo.cpp:2" && { echo "FAIL — flagged a COVERED line"; fail=1; } \
    || echo "ok   — covered line not flagged"
echo "$OUT" | grep -qE "Demo.cpp:(5|6|8)" && { echo "FAIL — flagged a non-coverable line"; fail=1; } \
    || echo "ok   — non-coverable brace/sig lines ignored"
echo "$OUT" | grep -qiE "ThirdParty|vendor\.cc" && { echo "FAIL — flagged a vendored ThirdParty TU"; fail=1; } \
    || echo "ok   — changed vendored ThirdParty TU excluded, not flagged"

# Case 2: mark line 3 covered too — the gate passes (exit 0), exemption still logged.
sed 's/DA:3,0/DA:3,2/' cov.lcov > cov-pass.lcov
set +e
python3 "$TOOL" --lcov cov-pass.lcov --base "$BASE" --head "$HEAD" --root "$WORK" >/dev/null 2>&1
rc=$?
set -e
check "all changed lines covered/exempt passes the gate" 0 "$rc"

# Case 3: a changed TU entirely absent from the lcov (no coverage) fails.
cat > cov-absent.lcov <<EOF
SF:$WORK/Source/Other/Other.cpp
DA:1,1
end_of_record
EOF
set +e
python3 "$TOOL" --lcov cov-absent.lcov --base "$BASE" --head "$HEAD" --root "$WORK" >/dev/null 2>&1
rc=$?
set -e
check "changed TU with no coverage at all fails the gate" 1 "$rc"

[ "$fail" = 0 ] && echo "PASS: coverage-changed-lines gate" \
    || { echo "test-coverage-changed-lines: FAILED — $fail case(s), each a FAIL line above" >&2; exit 1; }
