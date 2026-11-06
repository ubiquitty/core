#!/bin/bash
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

# Scripts/check-merge-ready.sh — Pre-merge validation script
#
# Usage:
#   ./Scripts/check-merge-ready.sh
#
# Validates:
#   1. Branch is rebased onto latest main
#   2. No uncommitted changes
#   3. LSP index is up-to-date
#   4. All naming checks pass
#   5. Module boundaries hold (no Cocoa/Metal/POSIX in Core/portable modules)

set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# Build output lives outside the checkout; see Scripts/build-root.sh.
# shellcheck source=Scripts/build-root.sh
. "$ROOT/Scripts/build-root.sh"
cd "$ROOT"

echo "=== Pre-Merge Checklist ==="

# 1. Check rebase status
MAIN_BRANCH=$(git rev-parse --abbrev-ref origin/HEAD | sed 's|origin/||')
CURRENT_BRANCH=$(git rev-parse --abbrev-ref HEAD)

if [ "$CURRENT_BRANCH" = "$MAIN_BRANCH" ]; then
    echo "Already on $MAIN_BRANCH — merge to what?"
    exit 1
fi

AHEAD=$(git rev-list --count HEAD "^origin/$MAIN_BRANCH" || echo "0")
BEHIND=$(git rev-list --count "origin/$MAIN_BRANCH" "^HEAD" || echo "0")

echo "[1/5] Rebase status:"
if [ "$BEHIND" -gt 0 ]; then
    echo "  FAIL: $BEHIND commits behind origin/$MAIN_BRANCH"
    echo "  Run: git rebase origin/$MAIN_BRANCH"
    exit 1
else
    echo "  PASS: up-to-date with origin/$MAIN_BRANCH ($AHEAD commit(s) ahead)"
fi

# 2. Check uncommitted changes
echo "[2/5] Working tree:"
if ! git diff-index --quiet HEAD --; then
    echo "  FAIL: Uncommitted changes detected"
    echo "  Stage and commit before merging"
    exit 1
else
    echo "  PASS: Clean working tree"
fi

# 3. Check LSP index freshness
echo "[3/5] LSP index:"
if [ ! -f "compile_commands.json" ]; then
    echo "  Missing, rebuilding..."
    cd "$UBQ_BUILD_ROOT/macos" && cmake -DCMAKE_EXPORT_COMPILE_COMMANDS=ON . > /dev/null 2>&1
    cp compile_commands.json ../../ && cd ../..
    echo "  PASS: Rebuilt"
else
    echo "  PASS: Current"
fi

# 4. Run naming checks
echo "[4/5] Naming checks:"
if ./Scripts/check-acronyms.py > /dev/null 2>&1; then
    echo "  PASS: All checks pass"
else
    echo "  FAIL: Naming violations detected"
    ./Scripts/check-acronyms.py
    exit 1
fi

# 5. Module-boundary tripwire: no Cocoa/Metal/POSIX above the seam.
echo "[5/5] Module boundaries:"
if ./Scripts/check-boundaries.py > /dev/null 2>&1; then
    echo "  PASS: Boundaries hold"
else
    echo "  FAIL: Module-boundary violation detected"
    ./Scripts/check-boundaries.py
    exit 1
fi

echo
echo "=== Ready to merge ==="
