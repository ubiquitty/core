#!/usr/bin/env bash
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

# Bound the compile fan-out. `cmake --build -j` with no number hands ninja its
# default of cores + 2, and several checkouts may build on one machine at
# once, so the unbounded form asks for far more processes than there are cores
# and the machine stops responding. UBQ_BUILD_JOBS overrides.
BUILD_JOBS="${UBQ_BUILD_JOBS:-$( (sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4) )}"

#
# build-agent.sh — configure + build ubq, the headless AI-agent
# frontend (a Sync client of the ubiquitty-host daemon).
#
#   ./Scripts/build-agent.sh            # release (-O3 + ThinLTO via CMake Release)
#   ./Scripts/build-agent.sh --debug    # debug (-O0, asserts)
#   ./Scripts/build-agent.sh --clean    # wipe $UBQ_BUILD_ROOT/agent/ first
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# Build output lives outside the checkout; see Scripts/build-root.sh.
# shellcheck source=Scripts/build-root.sh
. "$ROOT/Scripts/build-root.sh"
BUILD="$UBQ_BUILD_ROOT/agent"
BUILD_TYPE=Release

for arg in "$@"; do
    case "$arg" in
        --debug) BUILD_TYPE=Debug ;;
        --clean) rm -rf "$BUILD" ;;
        *) echo "build-agent: unknown arg: $arg" >&2; exit 2 ;;
    esac
done

# shellcheck source=Scripts/build-provenance.sh
source "$ROOT/Scripts/build-provenance.sh"
build_provenance_warning "$ROOT" "$BUILD" "Scripts/build-agent.sh --clean"

GEN="Unix Makefiles"
command -v ninja >/dev/null 2>&1 && GEN="Ninja"

# UBIQUITTY_QUIC is not passed. The Agent branch of CMakeLists.txt references it
# nowhere and no source under Source/Agent names QUICClient, so pinning it here
# set a variable this build does not read — and pinned it OFF, which read as a
# decision that the agent ships without QUIC rather than as the fact that it has
# none. Omitting it means the project default (ON) applies, so the day the agent
# grows a QUIC carrier it is on without anyone remembering this line. Nothing is
# fetched meanwhile: ubiquitty_ensure_quic_deps() is called only from the macOS
# and Host branches.
#
# UBIQUITTY_PGO=OFF builds without the training profile (Support/pgo/README.md).
# Passed on each Release configure, ON when unset, so an OFF left in the CMake
# cache by one run cannot outlive the run that asked for it. A Debug configure
# never reads the variable and is not handed it, so CMake has no unused variable
# to report.
PGO_ARGUMENTS=()
if [ "$BUILD_TYPE" = "Release" ]; then
    PGO_ARGUMENTS=(-DUBIQUITTY_PGO="${UBIQUITTY_PGO:-ON}")
fi
cmake -B "$BUILD" -S "$ROOT" -G "$GEN" \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    -DUBIQUITTY_FRONTEND=Agent \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    ${PGO_ARGUMENTS[@]+"${PGO_ARGUMENTS[@]}"}
cmake --build "$BUILD" -j"$BUILD_JOBS"
build_provenance_stamp "$ROOT" "$BUILD"

echo "build-agent: built $BUILD/ubq"
