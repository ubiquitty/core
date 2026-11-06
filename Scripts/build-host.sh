#!/usr/bin/env bash
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

# Bound the compile fan-out. `cmake --build -j` with no number hands ninja its
# default of cores + 2, and several checkouts may build on one machine at
# once, so the unbounded form asks for far more processes than there are cores
# and the machine stops responding. UBQ_BUILD_JOBS overrides.
BUILD_JOBS="${UBQ_BUILD_JOBS:-$( (sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4) )}"

#
# build-host.sh — build the ubiquitty-host daemon (the portable terminal
# Core relocated into a persistent server) via CMake.
#
# ubiquitty-host is a CMake target alongside the macOS frontend: portable
# Core + Source/Core/Protocol/Sync + the POSIX PTY + the host main, with no Cocoa /
# Metal / Swift. It builds with clang on macOS (local testing) and gcc
# on Linux.
#
#   ./Scripts/build-host.sh            release (-O3 + ThinLTO via CMake Release) build
#   ./Scripts/build-host.sh --debug    debug (-O0, asserts) build
#   ./Scripts/build-host.sh --clean    wipe $UBQ_BUILD_ROOT/host/ first
#
# The persistence smoke is the host_smoke stanza (./Scripts/run-tests.sh
# host_smoke), which runs it against this build.
#
# Output: $UBQ_BUILD_ROOT/host/ubiquitty-host (resolved by build-root.sh)
#
# To run after building:
#   export UBQ_BUILD_ROOT="$(python3 Scripts/build_root.py .)"
#   "$UBQ_BUILD_ROOT/host/ubiquitty-host" --ensure-daemon
#   "$UBQ_BUILD_ROOT/host/ubiquitty-host" --attach

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# Build output lives outside the checkout; see Scripts/build-root.sh.
# shellcheck source=Scripts/build-root.sh
. "$ROOT/Scripts/build-root.sh"
BUILD="$UBQ_BUILD_ROOT/host"
CLEAN=0
BUILD_TYPE="Release"
for arg in "$@"; do
    case "$arg" in
        --clean)   CLEAN=1 ;;
        --debug)   BUILD_TYPE="Debug" ;;
        --release) BUILD_TYPE="Release" ;;
        -h|--help) sed -n '2,30p' "$0"; exit 0 ;;
        *) echo "build-host.sh: unknown arg: $arg" >&2; exit 2 ;;
    esac
done

[ "$CLEAN" = "1" ] && rm -rf "$BUILD"

# shellcheck source=Scripts/build-provenance.sh
source "$ROOT/Scripts/build-provenance.sh"
build_provenance_warning "$ROOT" "$BUILD" "Scripts/build-host.sh --clean"

# Plain "Unix Makefiles" generator — ubiquitty-host has no Swift, so we do
# not need (or want) the Ninja requirement the macOS build carries. Falls
# back automatically to whatever CMake picks if make is unavailable.
GEN="Unix Makefiles"
command -v ninja >/dev/null 2>&1 && GEN="Ninja"

# UBIQUITTY_PGO=OFF builds without the training profile (Support/pgo/README.md).
# Passed on each Release configure, ON when unset, so an OFF left in the CMake
# cache by one run cannot outlive the run that asked for it. A Debug configure
# never reads the variable and is not handed it, so CMake has no unused variable
# to report.
PGO_ARGUMENTS=()
if [ "$BUILD_TYPE" = "Release" ]; then
    PGO_ARGUMENTS=(-DUBIQUITTY_PGO="${UBIQUITTY_PGO:-ON}")
fi

# QUIC is the always-on transport; force it ON every configure so a stale CMake
# cache can't silently produce a non-QUIC host that falls back to ssh-stdio.
# UBIQUITTY_RELEASE_SIGNING=ON builds a release's host, which trusts only the
# release team's code signature; passed on every configure, OFF when unset,
# so a release's setting left in the cache cannot reach a development build.
cmake -B "$BUILD" -S "$ROOT" -G "$GEN" \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    -DUBIQUITTY_FRONTEND=Host \
    -DUBIQUITTY_QUIC=ON \
    -DUBIQUITTY_RELEASE_SIGNING="${UBIQUITTY_RELEASE_SIGNING:-OFF}" \
    -DUBIQUITTY_QUIC_PREFIX="${UBIQUITTY_QUIC_PREFIX:-$UBQ_BUILD_ROOT/quic-deps}" \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    ${PGO_ARGUMENTS[@]+"${PGO_ARGUMENTS[@]}"}
cmake --build "$BUILD" -j"$BUILD_JOBS"
build_provenance_stamp "$ROOT" "$BUILD"

BIN="$BUILD/ubiquitty-host"
echo
echo "Built: $BIN"
