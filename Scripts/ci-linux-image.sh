#!/usr/bin/env bash
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

#
# ci-linux-image.sh — run a CI battery in the pinned Linux image on a
# GitHub-hosted runner, or directly on a self-hosted one.
#
#   Scripts/ci-linux-image.sh <command…>
#   Scripts/ci-linux-image.sh --recipe-digest    the digest the image is tagged by
#
# With UBQ_CI_LINUX_IMAGE=true, which the CI workflow sets when the job landed
# on a GitHub-hosted runner, the command runs in the image. Anything else runs
# it here: a self-hosted runner carries its own toolchain.
#
# A hosted runner starts with no images, so without help every job builds this
# one before its first gate. UBQ_CI_IMAGE_ARCHIVE names a `docker save` archive
# that the CI workflow restores from the Actions cache, keyed by
# --recipe-digest: an absent image is loaded from it when it is there, and saved
# into it after a build, so the image is rebuilt only when the recipe changes.
#
# A hosted runner is GitHub's ubuntu-24.04 image. It has no llvm-profdata, no
# GTK 4 and no g++ 15, so a battery run on it tests whatever GitHub installed.
# The image is run-linux.sh's instead: the gtk= base pinned in
# Scripts/linux-images.txt and the package list printed by
# `run-linux.sh dockerfile`. On top of that come the tools a battery otherwise
# installs through sudo: clang for the fuzz and sanitizer stages, make,
# python3-venv for the pinned clang-format, and the shellcheck binary. file
# comes for appimagetool, which stops without it when packaging the
# AppImage in this image.
#
# The image's clang is LLVM 21, which refuses the format version of the Linux
# training profile: Support/pgo/linux-x86_64-core.profdata is written by LLVM
# 22. So clang-22 and llvm-22 come too, and UBQ_PGO_LLVM names their directory
# of unversioned tools, which the Linux pgo stage and the release AppImage put
# first on PATH. A retrain under a newer LLVM major raises the two packages and
# UBQ_PGO_LLVM with it; the pgo stage fails until they name that LLVM.
#
# Every runner path is mounted at the same path, not under /src. The musl host
# build runs `docker run -v "$ROOT":/src` against the runner's daemon, which
# resolves that path on the runner, so ROOT has to be the same string on both
# sides. The checkout is also mounted at /src, because the recipe registers the
# shipped fonts from /src/Resources/fonts or
# /src/Libraries/ubiquitty-core/Resources/fonts.

set -euo pipefail

SCRIPTS="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
sourceArguments=()
ROOT="" CORE_ROOT="" KIT_ROOT=""
while [ "$#" -gt 0 ]; do
    case "$1" in
        --source-root|--core-source-root|--kit-source-root)
            [ "$#" -ge 2 ] || { echo "ci-linux-image: $1 requires a source directory" >&2; exit 2; }
            sourceArguments+=("$1" "$2"); shift 2 ;;
        --) shift; break ;;
        *) break ;;
    esac
done
sourceContext="$(python3 "$SCRIPTS/ubq_roots.py" shell-context ${sourceArguments[@]+"${sourceArguments[@]}"})"
# The shared resolver emits only fixed variable names with shlex-quoted values.
eval "$sourceContext"
cd "$ROOT"
if [ "${1:-}" != --recipe-digest ] && [ "${UBQ_CI_LINUX_IMAGE:-}" != true ]; then
    exec "$@"
fi

recipe="$(UBIQUITTY_LINUX_ROOT="$ROOT" "$CORE_ROOT/Scripts/run-linux.sh" dockerfile)"
recipe+='
RUN apt-get update -qq && apt-get install -y -qq \
      clang make sudo shellcheck python3-venv ca-certificates curl xz-utils bzip2 unzip procps \
      emacs-nox neovim openssh-server lrzsz file clang-22 llvm-22 \
    && rm -rf /var/lib/apt/lists/*
ENV UBQ_PGO_LLVM=/usr/lib/llvm-22/bin
RUN git config --system --add safe.directory "*"
# actionlint for Scripts/check-workflows.sh, the version and archive digests
# that script pins, so a battery in this image lints without fetching.
RUN set -eu; v=1.7.12; case "$(dpkg --print-architecture)" in \
      amd64) a=linux_amd64; d=8aca8db96f1b94770f1b0d72b6dddcb1ebb8123cb3712530b08cc387b349a3d8 ;; \
      arm64) a=linux_arm64; d=325e971b6ba9bfa504672e29be93c24981eeb1c07576d730e9f7c8805afff0c6 ;; \
      *) echo "no pinned actionlint for $(dpkg --print-architecture)"; exit 1 ;; esac; \
    curl -fsSL -o /tmp/actionlint.tgz "https://github.com/rhysd/actionlint/releases/download/v$v/actionlint_${v}_$a.tar.gz"; \
    echo "$d  /tmp/actionlint.tgz" | sha256sum -c -; \
    tar -xzf /tmp/actionlint.tgz -C /usr/local/bin actionlint; rm -f /tmp/actionlint.tgz
ENV UBQ_ACTIONLINT=/usr/local/bin/actionlint
# The two hand-written Sync decoders run against the same frames the host
# writes, and their stanzas decline when the interpreter is absent; the
# file-transfer launch test skips without rz and sz. Naming them here is what
# turns that decline into a failure inside this image, where the packages above
# make that absence a fault in the image rather than the shape of a machine. An
# apostrophe here would end the single-quoted recipe.
ENV UBQ_REQUIRED_TOOLS="emacs nvim lrzsz"
ENV UBQ_SSH_SMOKE_REQUIRED=1'

# Tagged by the recipe's digest, as run-linux.sh tags its own image, so a
# changed package list builds a new image rather than reusing an older one.
digest="$(printf '%s' "$recipe" | sha256sum | cut -c1-12)"
if [ "${1:-}" = --recipe-digest ]; then
    printf '%s\n' "$digest"
    exit 0
fi
image="ubq-ci-linux:$digest"
archive="${UBQ_CI_IMAGE_ARCHIVE:-}"
# Ask the daemon: a remote engine need not share the client's security setup.
sharedLabel="$(docker info --format '{{json .SecurityOptions}}' | python3 -c '
import json, sys
options = json.load(sys.stdin)
if not isinstance(options, list) or any(not isinstance(option, str) for option in options):
    sys.exit("ci-linux-image: invalid daemon security options")
print("z" if any(option.split(",")[0] == "name=selinux" for option in options) else "")
')"

# Standalone invocations and linked worktrees retain the same paths inside the
# container, including git's shared object store and external build outputs.
export RUNNER_WORKSPACE="${RUNNER_WORKSPACE:-$ROOT}"
export RUNNER_TEMP="${RUNNER_TEMP:-${TMPDIR:-/tmp}/ubiquitty-ci}"
export RUNNER_TOOL_CACHE="${RUNNER_TOOL_CACHE:-$RUNNER_TEMP/tools}"
directoriesToCreate=("$RUNNER_TEMP" "$RUNNER_TOOL_CACHE")
extra_mounts=()
mountTargets=()
mountSources=()
mountModes=()
mountKinds=()
# Shared labels belong to scoped project data. Runner aliases can contain
# unrelated jobs, and the Docker client and socket must keep their host labels.
addMount() {
    local source="$1" target="$2" mode="${3:-}" kind="${4:-host}" index
    for index in ${mountTargets[@]+"${!mountTargets[@]}"}; do
        if [ "${mountTargets[$index]}" = "$target" ]; then
            if [ "${mountSources[$index]}" != "$source" ] || [ "${mountModes[$index]}" != "$mode" ]; then
                echo "ci-linux-image: conflicting sources or modes name the same container mount: $target" >&2
                return 2
            fi
            if [ "${mountKinds[$index]}" = host ] || [ "$kind" = source ]; then
                mountKinds[index]="$kind"
            fi
            return 0
        fi
    done
    mountTargets+=("$target")
    mountSources+=("$source")
    mountModes+=("$mode")
    mountKinds+=("$kind")
}
if [ -n "${UBQ_CI_LINUX_USER:-}" ]; then
    extra_mounts+=(--user "$UBQ_CI_LINUX_USER")
fi
addMount "$RUNNER_WORKSPACE" "$RUNNER_WORKSPACE"
addMount "$RUNNER_TEMP" "$RUNNER_TEMP" "" data
addMount "$RUNNER_TOOL_CACHE" "$RUNNER_TOOL_CACHE"
# /src is an additional read-only alias; the real workspace path remains
# writable for the runner's generated configurations and check results.
addMount "$ROOT" /src ro source
addMount /var/run/docker.sock /var/run/docker.sock "" system
dockerBinary="$(command -v docker)"
addMount "$dockerBinary" /usr/local/bin/docker ro system
sourceRoots=("$ROOT" "$CORE_ROOT")
[ -z "$KIT_ROOT" ] || sourceRoots+=("$KIT_ROOT")
for directory in "${sourceRoots[@]}"; do
    addMount "$directory" "$directory" "" source
    # A gitless source export needs no repository metadata. A linked worktree
    # still needs its own shared object store at the same absolute path.
    common="$(git -C "$directory" rev-parse --path-format=absolute --git-common-dir 2>/dev/null || true)"
    [ -z "$common" ] || addMount "$common" "$common" "" git
done
if [ -n "${UBQ_PGO_RETRAIN_WORKTREE:-}" ]; then
    directory="$(dirname "$UBQ_PGO_RETRAIN_WORKTREE")"
    directory="$(python3 -c 'from pathlib import Path; import sys; print(Path(sys.argv[1]).resolve())' "$directory")"
    directoriesToCreate+=("$directory")
    addMount "$directory" "$directory" "" data
fi
for directory in "${UBQ_BUILD_ROOT:-}" "${CCACHE_DIR:-}"; do
    if [ -n "$directory" ]; then
        directoriesToCreate+=("$directory")
        addMount "$directory" "$directory" "" data
    fi
done
if [ -n "${UBQ_PGO_TRAIN_OUT:-}" ]; then
    directory="$(dirname "$UBQ_PGO_TRAIN_OUT")"
    directory="$(python3 -c 'from pathlib import Path; import sys; print(Path(sys.argv[1]).resolve())' "$directory")"
    directoriesToCreate+=("$directory")
    addMount "$directory" "$directory" "" data
fi
if [ -n "$sharedLabel" ]; then
    # A broad workspace remains an alias. Only its scoped compiler cache needs
    # a writable container label; relabeling the workspace would reach other jobs.
    cacheDirectory="${CCACHE_DIR:-$RUNNER_WORKSPACE/ccache}"
    directoriesToCreate+=("$cacheDirectory")
    addMount "$cacheDirectory" "$cacheDirectory" "" data
    validation=()
    for index in "${!mountSources[@]}"; do
        validation+=("${mountSources[$index]}" "${mountKinds[$index]}")
    done
    python3 - "${sourceRoots[@]}" -- "$HOME" "${TMPDIR:-/tmp}" "$dockerBinary" /var/run/docker.sock -- ${validation[@]+"${validation[@]}"} <<'PY'
from pathlib import Path
import sys

arguments = sys.argv[1:]
separator = arguments.index("--")
roots = [Path(value).resolve() for value in arguments[:separator]]
arguments = arguments[separator + 1:]
separator = arguments.index("--")
protected = [Path(value).resolve() for value in arguments[:separator]]
broadRoots = [Path(value).resolve() for value in (
    "/", "/bin", "/boot", "/dev", "/etc", "/home", "/lib", "/lib64",
    "/media", "/mnt", "/opt", "/proc", "/root", "/run", "/sbin",
    "/srv", "/sys", "/tmp", "/usr", "/var", "/var/tmp", "/Users",
)]
protected += broadRoots
broadRoots += protected[:2]
systemTrees = [Path(value).resolve() for value in (
    "/bin", "/boot", "/dev", "/etc", "/lib", "/lib64", "/proc", "/run", "/sbin", "/sys", "/usr", "/var",
)]
variableData = Path("/var").resolve()
variableTemporary = Path("/var/tmp").resolve()
arguments = arguments[separator + 1:]
for value, kind in zip(arguments[::2], arguments[1::2]):
    path = Path(value).resolve()
    if not Path(value).is_absolute() or ":" in value or "\n" in value:
        sys.exit(f"ci-linux-image: invalid bind mount path: {value!r}")
    if kind == "system":
        continue
    if kind == "host":
        if not Path(value).is_absolute() or path in broadRoots:
            sys.exit(f"ci-linux-image: refusing a broad runner mount: {value}")
        continue
    if (any(item == path or path in item.parents for item in protected)
            or any((path == tree or tree in path.parents)
                   and not (tree == variableData and variableTemporary in path.parents)
                   for tree in systemTrees)
            or (kind != "source" and any(root == path or path in root.parents for root in roots))):
        sys.exit(f"ci-linux-image: refusing to relabel a broad or system path: {value}")
PY
fi
mkdir -p "${directoriesToCreate[@]}"
for index in "${!mountSources[@]}"; do
    mode="${mountModes[$index]}"
    case "${mountKinds[$index]}" in
        source|git|data) [ -z "$sharedLabel" ] || mode="${mode:+$mode,}$sharedLabel" ;;
    esac
    extra_mounts+=(-v "${mountSources[$index]}:${mountTargets[$index]}${mode:+:$mode}")
done
if ! docker image inspect "$image" >/dev/null 2>&1 && [ -n "$archive" ] && [ -s "$archive" ]; then
    # An archive the cache restored under another recipe's key would load an
    # image of another tag, which the inspect below then does not find.
    docker load -i "$archive" || echo "ci-linux-image: $archive did not load; building the image" >&2
fi
if ! docker image inspect "$image" >/dev/null 2>&1; then
    context="$(mktemp -d)"
    printf '%s\n' "$recipe" > "$context/Dockerfile"
    docker build -t "$image" "$context"
    rm -rf "$context"
    if [ -n "$archive" ]; then
        docker save -o "$archive" "$image" \
            || { echo "ci-linux-image: could not save $image to $archive; the next run builds it again" >&2; rm -f "$archive"; }
    fi
fi
image_id="$(docker image inspect --format '{{.Id}}' "$image")"

# The runner's own variables go in by name: GITHUB_OUTPUT and
# GITHUB_STEP_SUMMARY are files under RUNNER_TEMP that the battery appends to
# and the runner reads after the step.
passed=()
while IFS= read -r name; do
    passed+=(-e "$name")
done < <(compgen -e | grep -E '^(GITHUB_|RUNNER_|ACTIONS_|UBQ_|UBIQUITTY_|CCACHE_|CI$|GIT_(AUTHOR|COMMITTER)_(NAME|EMAIL)$)')

# The compiler cache lives beside the checkout, where the CI workflow restores
# and saves it between runs; under root's home in the container it would start
# empty on every run. 2G keeps it inside the repository's Actions cache
# allowance.
#
# Root inside: the battery's setup stages call sudo, and root has it without a
# password. git refuses a checkout owned by another uid unless it is marked
# safe, and the image marks every directory safe in its system config rather
# than through the environment, which a gate that runs git with its own
# environment does not pass on.
# --init puts an init process at pid 1 to reap orphans. Without one the command
# is pid 1 and never reaps, so a killed descendant lingers as a zombie and
# test-gate-framework's check that it is gone fails.
docker run --rm --init \
    ${extra_mounts[@]+"${extra_mounts[@]}"} \
    ${passed[@]+"${passed[@]}"} \
    -e LANG=C.UTF-8 \
    -e "CCACHE_DIR=${CCACHE_DIR:-$RUNNER_WORKSPACE/ccache}" \
    -e "CCACHE_MAXSIZE=${CCACHE_MAXSIZE:-2G}" \
    -e "UBQ_CI_LINUX_IMAGE_ID=$image_id" \
    -e "UBQ_CI_LINUX_RECIPE=$digest" \
    -w "$ROOT" \
    "$image" "$@"
