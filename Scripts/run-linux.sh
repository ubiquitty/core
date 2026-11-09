#!/usr/bin/env bash
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

#
# run-linux.sh — run a repo command on Linux from macOS, natively.
#
# The Linux frontend needs GTK 4 and Vulkan development files, which do not
# exist on macOS, so the Linux build and its tests are unrunnable here without a
# machine. This gives one: a native Ubuntu container with the prerequisites
# the Linux build's prerequisite check names, the working tree bind-mounted
# rather than copied, and the compiler cache kept on the host.
#
# The host's own architecture, because a foreign image runs under emulation and
# a full build takes long enough that nobody runs it twice. Native is the whole
# point: arm64 on Apple silicon, amd64 on an x86_64 Linux machine.
#
# Usage:
#   Scripts/run-linux.sh                    build the GTK frontend
#   Scripts/run-linux.sh test               run the host test suite on Linux
#   Scripts/run-linux.sh shell              interactive shell in the container
#   Scripts/run-linux.sh <command…>         run any command in the repo root
#
# The build directory and the compiler cache live under build/, which is
# gitignored, and are written as the invoking user rather than as root — a
# root-owned object file inside the tree is worse than a slow build.
#

set -euo pipefail

SCRIPTS="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# The checkout mounted at /src: this one, or the consumer that carries it. A
# consumer names itself in UBIQUITTY_LINUX_ROOT (ubiquitty-kit's
# Scripts/run-linux.sh does), and a repository that carries this script as its
# Libraries/ubiquitty-core, or as its Kit's, is mounted when the script is run
# from it.
ROOT="${UBIQUITTY_LINUX_ROOT:-}"
if [ -z "$ROOT" ]; then
    ROOT="$(cd "$SCRIPTS/.." && pwd)"
    ubqToplevel="$(git rev-parse --show-toplevel 2>/dev/null || true)"
    if [ -n "$ubqToplevel" ] && [ "$ubqToplevel" != "$ROOT" ]; then
        for ubqMount in Libraries/ubiquitty-core Libraries/ubiquitty-kit/Libraries/ubiquitty-core; do
            if [ "$(cd "$ubqToplevel/$ubqMount" 2>/dev/null && pwd -P)" = "$(cd "$ROOT" && pwd -P)" ]; then
                ROOT="$ubqToplevel"
            fi
        done
    fi
fi
# Build output lives outside the checkout; see Scripts/build-root.sh.
# shellcheck source=Scripts/build-root.sh
. "$SCRIPTS/build-root.sh"
CCACHE_HOST_DIR="$UBQ_BUILD_ROOT/linux-container/linux-ccache"

if ! command -v docker >/dev/null 2>&1; then
    echo "run-linux.sh: no docker on PATH." >&2
    echo "  Install a Docker provider such as OrbStack or Docker Desktop." >&2
    exit 1
fi

if ! docker info >/dev/null 2>&1; then
    echo "run-linux.sh: the docker daemon is not answering — start your Docker provider (OrbStack or Docker Desktop) first." >&2
    exit 1
fi

# The image is built from a here-doc rather than a tracked Dockerfile so the
# prerequisite list lives in exactly one place: this script, beside the check
# that names the same packages.
#
# golang-go and perl are AWS-LC's, not ours. `run-linux.sh test` builds the QUIC
# host, whose CMake fetches AWS-LC, whose own build wants both — without them it
# stops at "Could not find Go" during configure, and every stanza that links the
# host reports BUILD-BROKEN for a reason that names none of this.
# The release is pinned for its compiler, not for its age: `<print>` and the rest
# of the C++23 library arrived in libstdc++ 14, and 24.04 carries 13.3 with no 14
# in its archive. 26.04 makes GCC 15 the default `g++`, so the Linux arm compiles
# the same standard the macOS and Web arms already do. A portable Core file is
# written once and built by all three; the oldest of them decides what the other
# two are allowed to use.
#
# An LTS rather than the interim release that first carried GCC 15: an interim
# leaves the archive after nine months, and a container recipe whose packages
# have moved to old-releases fails at `apt-get update` on a machine that never
# cached the image — which is every machine except the one that built it.
GTK_IMAGE="${UBIQUITTY_LINUX_GTK_IMAGE:-"$(sed -n 's/^gtk=//p' "$SCRIPTS/linux-images.txt")"}"
[ -n "$GTK_IMAGE" ] || { echo "run-linux: no gtk= entry in Scripts/linux-images.txt" >&2; exit 1; }
# `llvm` is here for `llvm-profdata`, which the PGO profile checks
# (check-profile-orphans.py) read the profile with.
#
# `zsh` is here for test_pty, which drives a real zsh through prompt hooks and
# the jobs a close asks about, and fails where there is none to drive.
#
# Two adjacent strings: the first line is expanded, the recipe below it stays
# single-quoted so the `\` line continuations reach docker as written. Under
# double quotes the shell eats backslash-newline and hands docker one long line.
DOCKERFILE_TEXT="FROM ${GTK_IMAGE}
"'ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update -qq && apt-get install -y -qq \
      cmake ninja-build g++ pkg-config libgtk-4-dev libvulkan-dev git ccache \
      golang-go perl libwebp-dev llvm \
      fonts-noto-cjk=1:20240730+repack1-1build1 \
      fonts-noto-core fonts-noto-color-emoji \
      xvfb python3 python3-pil mesa-vulkan-drivers glslc zsh \
      libx11-dev libxfixes-dev libwayland-dev wayland-protocols \
    && rm -rf /var/lib/apt/lists/*
# libx11-dev/libxfixes-dev/libwayland-dev/wayland-protocols are named
# explicitly for the two CMake-optional backends relative pointer lock uses
# (Source/Platform/Linux/PointerLock.cpp) even though libgtk-4-dev already
# pulls all four in transitively on this image (a build with neither line
# added still compiled and linked both backends) — a transitive pull is a
# Recommends chain this Dockerfile does not otherwise rely on, and the next
# base-image bump is free to drop it without anything here noticing until a
# build silently loses a backend.
# The faces this project SHIPS, registered from the bind-mounted tree rather
# than installed from the archive. A distribution package would be a second
# copy of a font we already carry, free to drift from it and pinned separately;
# these are the exact files the app bundles, so the container matches what the
# product draws with by construction. The directory belongs to ubiquitty-core: at
# the root of the mount where the checkout is ubiquitty-core or holds every
# repository, under Libraries/ where the checkout carries it, and under
# Libraries/ubiquitty-kit/Libraries/ where the checkout carries Core through the
# Kit. fontconfig skips the ones that are absent.
#
# Without this, fontconfig SUBSTITUTES: `fc-match "JetBrains Mono"` answers
# DejaVu Sans, so every golden minted against a config naming a shipped face
# records a face nobody ships, and the config reads as though it had been
# honoured. A substitution is the failure that looks like success.
#
# Noto CJK stays, which we do not ship and which the shipped monospace faces
# do not carry. No emoji package: the arm draws colour emoji from the bundled
# Noto Color Emoji, and a container with no system emoji font is the proof of
# that — with a package present, a golden could be recording the package.
RUN printf "%s\\n" \
      "<?xml version=\"1.0\"?>" \
      "<!DOCTYPE fontconfig SYSTEM \"fonts.dtd\">" \
      "<fontconfig><dir>/src/Resources/fonts</dir><dir>/src/Libraries/ubiquitty-core/Resources/fonts</dir><dir>/src/Libraries/ubiquitty-kit/Libraries/ubiquitty-core/Resources/fonts</dir></fontconfig>" \
    > /etc/fonts/conf.d/99-ubiquitty-shipped-fonts.conf'

# `dockerfile` prints the recipe and stops. Scripts/ci-linux-image.sh builds the
# hosted CI image from it, so CI and this script install one package list.
if [ "${1:-}" = dockerfile ]; then
    printf '%s\n' "$DOCKERFILE_TEXT"
    exit 0
fi

# The tag carries a digest of the recipe, so editing the package list above is
# what rebuilds the image. A fixed tag makes the `docker image inspect` below
# report a hit for an image built from an older recipe, and the added package is
# then missing on every machine that ran this once before — a prerequisite that
# the script believes it installed and the container does not have.
case "$(uname -m)" in
    arm64|aarch64) ARCHITECTURE=arm64 ;;
    x86_64|amd64) ARCHITECTURE=amd64 ;;
    *) echo "run-linux.sh: no native image for $(uname -m)." >&2; exit 1 ;;
esac
IMAGE="ubq-linux:$ARCHITECTURE-$(printf '%s' "$DOCKERFILE_TEXT" | shasum -a 256 | cut -c1-12)"

if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
    echo "run-linux.sh: building $IMAGE (once per recipe; later runs reuse it) …"
    context="$(mktemp -d)"
    trap 'rm -rf "$context"' EXIT
    printf '%s\n' "$DOCKERFILE_TEXT" > "$context/Dockerfile"
    docker build --platform "linux/$ARCHITECTURE" -t "$IMAGE" "$context" >/dev/null
fi

# The container gets its own build/ and the host keeps its own, because a CMake
# cache records the ABSOLUTE path it was configured at and the bind mount gives
# one tree two of them. Every cache the host wrote names a path that does not
# exist in here, CMake refuses to reuse it, and run-tests.sh resolves that the
# only way it can — `rm -rf` and reconfigure. Correct inside one environment,
# destructive across the mount: measured, a single Linux stanza deleted a
# complete macOS build, and the host's next run would delete these in return.
#
# Shadowing /src/build costs nothing on either side. Neither environment's
# objects are usable by the other, so there is nothing to share, and each keeps
# its own incremental state instead of paying a full rebuild every alternation.
CONTAINER_BUILD_DIR="$UBQ_BUILD_ROOT/linux-container"
mkdir -p "$CONTAINER_BUILD_DIR"

# The host test suite configures into BuildHostTests/, which is a sibling of
# build/ and so is not covered by the shadow above. Without its own directory the
# container reconfigures the host's cache to /src paths, the host's next run finds
# a cache naming a path it cannot see and resolves it by deleting the tree, and
# the container's next run does the same in return: a full rebuild on both sides
# of every alternation, charged to whichever one ran second.
CONTAINER_TESTS_DIR="$CONTAINER_BUILD_DIR/BuildHostTests"
mkdir -p "$CONTAINER_TESTS_DIR"

# The compiler cache outlives the tree it was filled from, which nothing else
# here does. Kept inside build/ it dies with the worktree — and linked worktrees
# are the common case, so the container would pay a full rebuild each time a
# branch is finished with, and the copies in each worktree cannot help one
# another.
#
# Outside the repo, so worktree churn does not reach it, and shared: inside the
# container the source is always /src whoever mounted it, so two checkouts compiling
# the same commit produce the same hash without any base_dir rewriting. That is
# the property a host cache needs base_dir to fake.
LINUX_CCACHE_DIR="${UBQ_LINUX_CCACHE:-$HOME/Library/Caches/ubiquitty-linux-ccache}"
mkdir -p "$LINUX_CCACHE_DIR"

# A linked worktree's .git is a FILE naming <main-repo>/.git/worktrees/<name>,
# which is outside the mount, so git does not resolve in here. The contributor
# workflow recommends a linked worktree, so this is the ordinary
# case rather than the exotic one. Mounting the common dir at the path the file
# already names is what makes it resolve without rewriting anything.
GIT_COMMON_DIR=""
if [ -f "$ROOT/.git" ]; then
    GIT_COMMON_DIR="$(git -C "$ROOT" rev-parse --path-format=absolute --git-common-dir 2>/dev/null || true)"
fi

# A repository cloned with --reference keeps most of its objects in someone
# else's store and records it in objects/info/alternates, by absolute path. The
# common dir alone is then a git dir whose objects are mostly elsewhere, which
# git reports as "unable to normalize alternate object path" rather than as a
# missing object — so each alternate has to come along too.
GIT_ALTERNATES=()
if [ -n "$GIT_COMMON_DIR" ] && [ -f "$GIT_COMMON_DIR/objects/info/alternates" ]; then
    while IFS= read -r alternate; do
        [ -n "$alternate" ] || continue
        case "$alternate" in \#*) continue ;; esac
        [ -d "$alternate" ] && GIT_ALTERNATES+=("$alternate")
    done < "$GIT_COMMON_DIR/objects/info/alternates"
fi

mkdir -p "$CCACHE_HOST_DIR"

# The container has no account entry for the invoking uid, so getpwuid returns
# null and anything reading the account database — PAL::systemHomeDirectory and
# its test — sees a machine no real Linux box resembles. Generated per run rather
# than baked into the image, because the uid belongs to whoever is running it.
PASSWD_FILE="$CONTAINER_BUILD_DIR/linux-passwd"
{
    echo "root:x:0:0:root:/root:/bin/bash"
    echo "builder:x:$(id -u):$(id -g):builder:/tmp:/bin/bash"
} > "$PASSWD_FILE"

case "${1:-build}" in
    build) command=(./Scripts/build-linux-gtk.sh) ;;
    test) command=(./Scripts/run-tests.sh) ;;
    shell) command=(bash) ;;
    *) command=("$@") ;;
esac

# Under an enforcing SELinux the container may not execute or write the bind
# mounts, whose host labels are not its own. Relabelling would rewrite the
# checkout's and the cache's labels; the container runs unconfined instead.
securityOptions=()
if command -v getenforce >/dev/null 2>&1 && [ "$(getenforce)" = Enforcing ]; then
    securityOptions=(--security-opt label=disable)
fi

# -t only when stdin is a terminal, so a scripted run does not wedge waiting for
# one and the output stays free of control sequences.
interactive=(-i)
[ -t 0 ] && interactive=(-it)

# The worktree's own git dir sits under the common dir, so one mount at the path
# the .git file names covers both. Read-write: a build that writes an index lock
# into a read-only git dir fails in a way that names the build, not the mount.
gitMount=()
if [ -n "$GIT_COMMON_DIR" ]; then
    gitMount=(-v "$GIT_COMMON_DIR:$GIT_COMMON_DIR")
    # Read-only: an alternate is another repository's object store, and a
    # container writing into it corrupts a tree nobody was running here.
    for alternate in ${GIT_ALTERNATES+"${GIT_ALTERNATES[@]}"}; do
        gitMount+=(-v "$alternate:$alternate:ro")
    done
fi

# esctest2, when a checkout is present: the conformance suite runs INSIDE the
# terminal under test, so it has to exist in the container the terminal runs in.
# Read-only, because the suite is a checkout of someone else's repository and a
# run has no reason to write to it. Absent is not an error — every other use of
# this script is unaffected by whether the suite is on this machine.
esctestMount=()
esctestPath="${UBIQUITTY_ESCTEST:-$HOME/Projects/esctest2}"
if [ -d "$esctestPath" ]; then
    esctestMount=(-v "$esctestPath:/esctest2:ro")
fi

# The version, derived here and handed in, so the container's build states the
# version this machine's builds of the same tree do. Inside, the tree is owned
# by another user and git may refuse to answer for it, which would read as the
# VERSION file's fallback rather than as a failure.
versionEnvironment=()
if command -v cmake >/dev/null 2>&1; then
    versionEnvironment=(
        -e "UBIQUITTY_BUILD_VERSION=$(UBQ_VERSION_TREE="$ROOT" "$SCRIPTS/build-version.sh" version)"
        -e "UBIQUITTY_BUILD_IDENTITY=$(UBQ_VERSION_TREE="$ROOT" "$SCRIPTS/build-version.sh" identity)"
    )
fi

# A machine shared with a person sets UBQ_BUILD_JOBS to keep the compile to a
# few cores; the container's own count would be every core the VM has.
jobsEnvironment=()
if [ -n "${UBQ_BUILD_JOBS:-}" ]; then
    jobsEnvironment=(-e "UBQ_BUILD_JOBS=$UBQ_BUILD_JOBS")
fi

# The build root inside the container is the mounted /src/build, never a path
# under HOME: build_root.py derives its default from HOME, and HOME is /tmp
# here, so without UBQ_BUILD_ROOT the build lands in the container's own
# filesystem and the host-side shim and gates find nothing.
# A child init forwards signals and reaps children. In particular, Xvfb does
# not signal a PID 1 parent when it is ready, which leaves xvfb-run waiting.
docker run --rm --init "${interactive[@]}" \
    --platform "linux/$ARCHITECTURE" \
    ${securityOptions+"${securityOptions[@]}"} \
    ${esctestMount+"${esctestMount[@]}"} \
    -v "$ROOT:/src" \
    -v "$CONTAINER_BUILD_DIR:/src/build" \
    -v "$CONTAINER_TESTS_DIR:/src/BuildHostTests" \
    ${gitMount+"${gitMount[@]}"} \
    -v "$LINUX_CCACHE_DIR:/ccache" \
    -e CCACHE_DIR=/ccache \
    -e CCACHE_MAXSIZE=20G \
    -e "UBIQUITTY_LINUX_CONTAINER=1" \
    ${jobsEnvironment+"${jobsEnvironment[@]}"} \
    ${versionEnvironment+"${versionEnvironment[@]}"} \
    -e HOME=/tmp \
    -e UBQ_BUILD_ROOT=/src/build \
    -v "$PASSWD_FILE:/etc/passwd:ro" \
    -w /src \
    -u "$(id -u):$(id -g)" \
    "$IMAGE" \
    "${command[@]}"
status=$?

# A compile database written inside the container names /src, which is the bind
# mount and not a path any host-side tool can open. clang-refactor driven from it
# finds no files and exits 0, which reads as a rename that had nothing to do.
# Rewritten here rather than in build-linux-gtk.sh, because only this side knows
# both paths.
DB="$CONTAINER_BUILD_DIR/linux/compile_commands.json"
if [ -f "$DB" ] && grep -q '"/src/' "$DB" 2>/dev/null; then
    python3 - "$DB" "$ROOT" "$CONTAINER_BUILD_DIR" <<'REWRITE'
import json, sys
path, root, buildRoot = sys.argv[1], sys.argv[2], sys.argv[3]
entries = json.load(open(path))
for e in entries:
    for key in ("directory", "file", "output"):
        if key in e and isinstance(e[key], str):
            e[key] = e[key].replace("/src/build", buildRoot, 1).replace("/src", root, 1)
    if "command" in e:
        e["command"] = e["command"].replace("/src/build", buildRoot).replace("/src", root)
json.dump(entries, open(path, "w"), indent=1)
REWRITE
    echo "run-linux: rewrote compile_commands.json paths /src -> $ROOT"
fi

exit $status
