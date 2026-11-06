#!/usr/bin/env bash
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

# build-core-snapshot.sh — the portable snapshot renderer, built and stamped.
#
# The snapshot tier compares two arms: this one and the macOS app. It verifies
# each arm was built from the tree it is about to judge, and that verification
# reads the stamp Scripts/build-provenance.sh writes. Every other CMake frontend
# has a script that writes one; this arm was configured by a bare cmake typed
# from an error message, so it carried no stamp and the tier could only compare
# mtimes.
#
# An mtime says the binary is newer than the sources. It does not say it was
# built from them: a binary built on another branch is newer than sources it
# never saw, passes that test, and the tier then reports pixel differences that
# describe neither tree. That is a confident wrong verdict rather than a
# missing one, which is the expensive direction.
#
# Usage: build-core-snapshot.sh [--clean] [--debug|--release]

set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# Build output lives outside the checkout; see Scripts/build-root.sh.
# shellcheck source=Scripts/build-root.sh
. "$ROOT/Scripts/build-root.sh"
BUILD="$UBQ_BUILD_ROOT/core"
BUILD_TYPE="RelWithDebInfo"
. "$ROOT/Scripts/build-jobs.sh"
BUILD_JOBS="$(ubq_build_jobs)"
CLEAN=0

for arg in "$@"; do
    case "$arg" in
        --clean)   CLEAN=1 ;;
        --debug)   BUILD_TYPE="Debug" ;;
        --release) BUILD_TYPE="Release" ;;
        -h|--help) sed -n '2,20p' "$0"; exit 0 ;;
        *) echo "build-core-snapshot.sh: unknown arg: $arg" >&2; exit 2 ;;
    esac
done

[ "$CLEAN" = "1" ] && rm -rf "$BUILD"

# shellcheck source=Scripts/build-provenance.sh
source "$ROOT/Scripts/build-provenance.sh"
build_provenance_warning "$ROOT" "$BUILD" "Scripts/build-core-snapshot.sh --clean"

GEN="Unix Makefiles"
command -v ninja >/dev/null 2>&1 && GEN="Ninja"

cmake -B "$BUILD" -S "$ROOT" -G "$GEN" \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    -DUBIQUITTY_FRONTEND=Core \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE"
cmake --build "$BUILD" --target ubiquitty-snapshot -j"$BUILD_JOBS"
build_provenance_stamp "$ROOT" "$BUILD"

# Freshness stamp. The snapshot arm's staleness check compares the binary's
# mtime against the newest source, while the build above is content-correct:
# a successful pass proves the binary matches the sources whether or not it
# relinked, and a rebase's re-checkout or a commit hook's reformat bumps
# source mtimes without changing what was compiled. Stamping here makes the
# mtime mean "verified current by this run", which is exactly the question
# the arm asks — and a source edited after this stamp still reads newer.
touch "$BUILD/ubiquitty-snapshot"

echo
echo "portable snapshot renderer: $BUILD/ubiquitty-snapshot"
