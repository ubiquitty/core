#!/usr/bin/env bash
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

# Build standalone static-musl ubq binaries with Docker. The source checkout
# is mounted read-only; UBQ_AGENT_LINUX_SOURCE may name a release worktree.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/Scripts/build-root.sh"
. "$ROOT/Scripts/build-jobs.sh"
JOBS="$(ubq_build_jobs)"
SOURCE="${UBQ_AGENT_LINUX_SOURCE:-$ROOT}"
SOURCE="$(cd "$SOURCE" && pwd)"
OUT="${UBQ_AGENT_LINUX_OUTPUT:-$UBQ_BUILD_ROOT/agent-linux}"
mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"
IMAGE=alpine:3.23@sha256:ff7f0c727dd796aa36787bfcd9a64bf35a6ce72a73451b3359fde849b6703d63
VERSION="$("$SOURCE/Scripts/build-version.sh" version)"
IDENTITY="$("$SOURCE/Scripts/build-version.sh" identity)"
ARCHES=("$@")
if [ ${#ARCHES[@]} -eq 0 ]; then ARCHES=(x86_64 arm64); fi
for arch in "${ARCHES[@]}"; do
    case "$arch" in
        x86_64) platform=linux/amd64 ;;
        arm64) platform=linux/arm64 ;;
        *) echo "build-agent-linux: expected x86_64 or arm64" >&2; exit 2 ;;
    esac
    mkdir -p "$OUT/$arch"
    docker run --rm --platform "$platform" --security-opt label=disable \
        -e "UBIQUITTY_BUILD_VERSION=$VERSION" -e "UBIQUITTY_BUILD_IDENTITY=$IDENTITY" \
        -e "UBQ_BUILD_JOBS=$JOBS" -e "CMAKE_BUILD_PARALLEL_LEVEL=$JOBS" \
        -e "UBQ_RELEASE_ARCH=$arch" -e "UBQ_OWNER=$(id -u):$(id -g)" \
        -v "$SOURCE:/src:ro" -v "$OUT/$arch:/out" "$IMAGE" sh -euc '
            apk add --no-cache build-base cmake ninja git bash python3 file >/dev/null
            c++ --version | head -1
            trap '\''chown -R "$UBQ_OWNER" /out'\'' EXIT
            cmake -S /src -B /out/build -G Ninja \
                -DUBIQUITTY_FRONTEND=Agent -DCMAKE_BUILD_TYPE=Release \
                -DUBIQUITTY_PGO=OFF -DUBIQUITTY_WERROR=ON \
                -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DCMAKE_EXE_LINKER_FLAGS=-static
            cmake --build /out/build -j"$UBQ_BUILD_JOBS"
            readelf -l /out/build/ubq > /out/elf-program-headers.txt
            if grep -q INTERP /out/elf-program-headers.txt; then
                echo "build-agent-linux: ubq is not statically linked" >&2
                exit 1
            fi
            /out/build/ubq --help > /out/help.txt
            /out/build/ubq api list > /out/api.txt
            strip /out/build/ubq
            install -m 0755 /out/build/ubq "/out/ubq-linux-$UBQ_RELEASE_ARCH"
            file "/out/ubq-linux-$UBQ_RELEASE_ARCH"
            sha256sum "/out/ubq-linux-$UBQ_RELEASE_ARCH"
        '
    cp "$OUT/$arch/ubq-linux-$arch" "$OUT/ubq-linux-$arch"
done
cp "$SOURCE/LICENSE" "$SOURCE/NOTICE" "$OUT/"
echo "Standalone ubq binaries: $OUT"
