#!/usr/bin/env bash
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

#
# build-version.sh — print the version a build of this tree carries.
#
# The rule lives in Scripts/cmake/BuildVersion.cmake, which the build itself
# runs; this prints one of its answers for a script that needs it before or
# without a build: a release that names its archive, a wrapper that tells a
# container the version its mount cannot derive.
#
#   ./Scripts/build-version.sh              MAJOR.MINOR.PATCH.N (no .N at a named version)
#   ./Scripts/build-version.sh named        MAJOR.MINOR.PATCH
#   ./Scripts/build-version.sh commits      N
#   ./Scripts/build-version.sh bundle       the integer a bundle's CFBundleVersion holds
#   ./Scripts/build-version.sh identity     the short hash and what qualifies it
#
# UBQ_VERSION_TREE names another tree than the one this script sits in.
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
FIELD="${1:-version}"
case "$FIELD" in
    version|named|commits|bundle|identity) ;;
    -h|--help) sed -n '6,19p' "$0"; exit 0 ;;
    *) echo "build-version.sh: unknown field: $FIELD" >&2; exit 2 ;;
esac

exec cmake -DUBIQUITTY_VERSION_SOURCE_DIR="${UBQ_VERSION_TREE:-$ROOT}" \
    -DUBIQUITTY_VERSION_PRINT="$FIELD" \
    -P "$ROOT/Scripts/cmake/BuildVersion.cmake"
