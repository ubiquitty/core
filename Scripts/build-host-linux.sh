#!/usr/bin/env bash
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

# Bound the compile fan-out. `cmake --build -j` with no number hands ninja its
# default of cores + 2, and several checkouts may build on one machine at
# once, so the unbounded form asks for far more processes than there are cores
# and the machine stops responding. UBQ_BUILD_JOBS overrides.
BUILD_JOBS="${UBQ_BUILD_JOBS:-$( (sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4) )}"

#
# build-host-linux.sh — build static-musl ubiquitty-host for Linux (x86_64 +
# arm64) with Docker (OrbStack or Docker Desktop on macOS), into $UBQ_BUILD_ROOT/host-cross/ —
# the directory build-macos.sh bundles into the .app's Contents/Resources for
# auto-deploy to a fresh Linux host.
#
# Static + musl (Alpine toolchain): the result has no libc/libstdc++ dependency,
# so one binary per arch runs on any Linux — Debian/Ubuntu (glibc), Alpine
# (musl), and old-glibc hosts alike — with no version skew. QUIC is built in:
# AWS-LC + ngtcp2 compile in-container from source (the toolchain installs the
# go + perl + git + bash AWS-LC's build needs), statically linked into the host.
# (BSD is not covered: Docker runs only Linux containers; build a BSD host on the
# target instead.)
#
#   ./Scripts/build-host-linux.sh             both arches (x86_64 + arm64)
#   ./Scripts/build-host-linux.sh arm64       just one
#
# Output: $UBQ_BUILD_ROOT/host-cross/ubiquitty-host-linux-{x86_64,arm64}
#
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# Build output lives outside the checkout; see Scripts/build-root.sh.
# shellcheck source=Scripts/build-root.sh
. "$ROOT/Scripts/build-root.sh"
OUT="$UBQ_BUILD_ROOT/host-cross"
QUIC="${UBIQUITTY_QUIC:-ON}"

# The base image, from Scripts/linux-images.txt, pinned there by digest.
#
# This script produces the ubiquitty-host binaries
# bundled into the shipped macOS app, so a Mac can stand up a host on a fresh
# Linux box over ssh. A tag is a mutable ref, so two releases built from one
# commit could ship binaries built on different systems with nothing saying so.
# The digest is a manifest list, which is what lets --platform still select amd64
# or arm64 from it.
#
# The value lives beside the other container this tree builds in. Two scripts
# build in Linux, on two distributions, for two reasons, and reading one of them
# tells you nothing about the other — so one can sit a whole GCC release below
# the toolchain floor while the sibling is raised to meet it. That file also
# records which of the two is the floor, and it is this one.
ALPINE_IMAGE="${UBIQUITTY_LINUX_HOST_IMAGE:-"$(sed -n 's/^host=//p' "$(dirname "$0")/linux-images.txt")"}"
[ -n "$ALPINE_IMAGE" ] || {
    echo "build-host-linux.sh: no host= entry in Scripts/linux-images.txt" >&2
    exit 1
}

# The version, derived here and handed in. The container is given the checkout
# and not the repository behind it: a linked worktree's repository lies outside
# the mount, and the container's user does not own the files, so git inside
# answers for nothing and the build would read as the VERSION file's fallback.
# The host this binary is bundled with must state the version the app does.
VERSION_ENVIRONMENT=()
BUILD_VERSION=""
BUILD_IDENTITY=""
if command -v cmake >/dev/null 2>&1; then
    BUILD_VERSION="$("$ROOT/Scripts/build-version.sh" version)"
    BUILD_IDENTITY="$("$ROOT/Scripts/build-version.sh" identity)"
    VERSION_ENVIRONMENT=(-e "UBIQUITTY_BUILD_VERSION=$BUILD_VERSION" -e "UBIQUITTY_BUILD_IDENTITY=$BUILD_IDENTITY")
else
    echo "build-host-linux.sh: no cmake on this machine to derive the version with; the container derives its own." >&2
fi

DOCKER=""

ARCHES=("$@")
[ ${#ARCHES[@]} -eq 0 ] && ARCHES=(x86_64 arm64)

mkdir -p "$OUT"
# Keep dependency libraries and host objects under a compiler, image and
# dependency-script key. Ninja tracks source changes within each build tree.
DEPS="$OUT/deps"
mkdir -p "$DEPS"
OWNER="$(id -u):$(id -g)"
for arch in "${ARCHES[@]}"; do
    case "$arch" in
        x86_64 | amd64) arch=x86_64; plat=linux/amd64; leaf=ubiquitty-host-linux-x86_64 ;;
        arm64 | aarch64) arch=arm64; plat=linux/arm64; leaf=ubiquitty-host-linux-arm64 ;;
        *) echo "build-host-linux.sh: unknown arch: $arch (use x86_64 or arm64)" >&2; exit 2 ;;
    esac
    artifact_key() {
        python3 "$ROOT/Scripts/linux-host-artifact.py" key --arch "$arch" \
            --image "$ALPINE_IMAGE" --quic "$QUIC" --version "$BUILD_VERSION" --identity "$BUILD_IDENTITY"
    }
    INPUT_KEY="$(artifact_key)"
    if python3 "$ROOT/Scripts/linux-host-artifact.py" check --arch "$arch" \
        --binary "$OUT/$leaf" --key "$INPUT_KEY" 2>/dev/null; then
        echo "Linux host: verified $OUT/$leaf"
        continue
    fi
    if [ -z "$DOCKER" ]; then
        command -v docker >/dev/null 2>&1 || {
            echo "build-host-linux.sh: install OrbStack or Docker Desktop to build the Linux hosts." >&2
            exit 1
        }
        DOCKER=docker
        if ! docker info >/dev/null 2>&1; then
            if sudo -n docker info >/dev/null 2>&1; then
                DOCKER="sudo docker"
            else
                echo "build-host-linux.sh: the Docker daemon is not reachable; start it or allow this user to access it." >&2
                exit 1
            fi
        fi
    fi
    echo "==> building static-musl ubiquitty-host for $plat${BUILD_VERSION:+ ($BUILD_VERSION)}"
    # Alpine = musl toolchain; -static drops every shared-lib dependency. The
    # repo is mounted read-only (out-of-source build under /out/build),
    # so nothing leaks into the working tree. Installed mode 0644: the binary
    # ships as DATA — streamed over ssh and chmod'd on the remote.
    # label=disable rather than a :z mount option: this host runs SELinux with
    # a labelling docker (Fedora), where an unlabelled bind mount is unreadable
    # inside the container -- /src reads EMPTY, and cmake reports the source
    # directory as missing a CMakeLists.txt that is right there. Relabelling
    # instead (:z) would write new labels onto the checkout this build only
    # reads. On a host without SELinux the option is accepted and does nothing.
    $DOCKER run --rm --platform "$plat" --security-opt label=disable \
        ${VERSION_ENVIRONMENT[@]+"${VERSION_ENVIRONMENT[@]}"} \
        -e "UBQ_BUILD_JOBS=$BUILD_JOBS" -e "CMAKE_BUILD_PARALLEL_LEVEL=$BUILD_JOBS" -e "GOMAXPROCS=$BUILD_JOBS" \
        -v "$ROOT":/src:ro -v "$OUT":/out -v "$DEPS":/deps \
        "$ALPINE_IMAGE" nice -n 19 sh -euc '
            # The mount, before anything reads it: an unreadable bind mount
            # presents an empty directory, and every tool below then reports
            # its own thing missing instead of the mount.
            [ -f /src/CMakeLists.txt ] || {
                echo "build-host-linux: /src is empty inside the container -- the bind mount of the checkout did not arrive (SELinux labelling, or a path the daemon cannot read)." >&2
                exit 1
            }
            # python3: Scripts/build-root.sh resolves the build root through
            # Scripts/build_root.py, so a container without it configures nothing.
            # Handover uses socket pidfds from the kernel headers, including
            # when libc does not declare SO_PEERPIDFD.
            apk add --no-cache build-base linux-headers cmake ninja git go perl bash python3 >/dev/null
            # The toolchain this artefact was built with, in the log that built
            # it. apk resolves from the release branch at run time, and that
            # branch takes security updates, so the compiler can move under a
            # pinned image. Pinning apk versions instead would break the build the
            # day Alpine rotates one out of the repository, which is a worse
            # failure than a drift nobody can see. Recording it makes it visible.
            printf "build-host-linux: toolchain %s, cmake %s\n" \
                "$(cc --version | head -1)" "$(cmake --version | head -1)" >&2
            key="$(printf "%s\n%s\n%s\n" "$(cc --version | head -1)" '"$ALPINE_IMAGE"' \
                "$(sha256sum /src/Scripts/fetch-quic-deps.sh | cut -c1-64)" | sha256sum | cut -c1-12)"
            deps="/deps/'"$leaf"'-$key"
            build="/out/build/'"$leaf"'-$key"
            mkdir -p "$build"
            trap '\''chown -R '"$OWNER"' /deps "$build"'\'' EXIT
            if [ -f "$deps/lib/libngtcp2.a" ]; then
                echo "build-host-linux: QUIC deps kept from an earlier run at $deps" >&2
            else
                echo "build-host-linux: QUIC deps build into $deps, kept for the next run" >&2
            fi
            cmake -S /src -B "$build" -G Ninja \
                -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
                -DUBIQUITTY_FRONTEND=Host -DCMAKE_BUILD_TYPE=Release \
                -DUBIQUITTY_QUIC='"$QUIC"' -DUBIQUITTY_QUIC_PREFIX="$deps" \
                -DCMAKE_EXE_LINKER_FLAGS="-static"
            cmake --build "$build" -j'"$BUILD_JOBS"'
            strip "$build/ubiquitty-host"
            install -m 0644 "$build/ubiquitty-host" "/out/'"$leaf"'.pending"
        '
    [ "$INPUT_KEY" = "$(artifact_key)" ] || {
        echo "build-host-linux.sh: source changed during the build; rerun before bundling." >&2
        exit 1
    }
    python3 "$ROOT/Scripts/linux-host-artifact.py" record --arch "$arch" \
        --binary "$OUT/$leaf.pending" --key "$INPUT_KEY"
    mv -f "$OUT/$leaf.pending" "$OUT/$leaf"
    mv -f "$OUT/$leaf.pending.json" "$OUT/$leaf.json"
    echo "    -> $OUT/$leaf"
done

echo
echo "Bundled into the app on the next: ./Scripts/build-macos.sh"
file "$OUT/ubiquitty-host-linux-x86_64" "$OUT/ubiquitty-host-linux-arm64" 2>/dev/null || true
