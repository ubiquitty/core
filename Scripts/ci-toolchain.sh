#!/usr/bin/env bash
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

# Scripts/ci-toolchain.sh — decide whether a CI job needs to install anything.
#
# A runner that keeps its packages between runs would spend a package-manager
# metadata load and a dependency resolution on each install step to conclude
# that every package is already present. Presence alone is the wrong question to
# skip on: it cannot tell a toolchain that moved under the job from one that did
# not, and a compiler that changed major version is the thing a job most wants
# named.
#
# So what is compared is the resolved version of every tool the job asked for,
# against what the last successful install recorded. The stamp lives on the
# runner, which is the thing whose state is being remembered; a runner with no
# stamp installs once and records. A GitHub-hosted image starts every job
# without one, so there `check` costs one version probe and the install runs.
#
# The workflow files are compared too. They are where the package list is
# written, and a package that provides no command of its own -- a header set, a
# driver -- changes what a job needs without changing any version this can read.
# Any edit to a workflow costs one install per runner, which is the conservative
# side of a question that cannot be answered by looking at the binaries.
#
#   ci-toolchain.sh check  <group> <tool>...   0 = satisfied, skip the install
#   ci-toolchain.sh record <group> <tool>...   write the stamp after installing
#
# `check` prints what it found, because a job that skipped its install and then
# failed to compile needs the version it skipped on in the same log.

set -uo pipefail

mode="${1:-}"
group="${2:-}"
shift 2 2>/dev/null || true

if [ -z "$mode" ] || [ -z "$group" ] || [ "$#" -eq 0 ]; then
    echo "usage: ci-toolchain.sh check|record <group> <tool>..." >&2
    exit 2
fi

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# Whichever digest this machine has. The value only has to be stable across two
# runs on one runner, so which of the two answers it is does not matter.
ubq_digest() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | cut -d" " -f1
    else
        shasum -a 256 "$1" | cut -d" " -f1
    fi
}

# The same digest over stdin, shortened: this one names a file rather than
# proving a file, so collision resistance past a few bytes buys nothing.
ubq_digest_stdin() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum | cut -c1-12
    else
        shasum -a 256 | cut -c1-12
    fi
}

stamp_dir="${UBQ_CI_TOOLCHAIN_DIR:-${XDG_CACHE_HOME:-${HOME:-/tmp}/.cache}/ubiquitty-ci}"
# The stamp is keyed by the tool list as well as the group, because the CI
# workflow reuses one group across jobs that ask for different tools:
# build-tools covers a five-tool job, a four-tool one and a three-tool one, and
# clang-llvm covers a three-tool job and a one-tool one. The fingerprint carries
# a line per tool, so those jobs never match each other -- keyed by group alone
# they would each overwrite the last one's stamp and every job would install on
# every run, which is the behaviour this exists to remove.
stamp="$stamp_dir/$group-$(printf '%s\n' "$@" | sort | ubq_digest_stdin)"

# One line per tool: the name, then the first line the tool says about itself.
# The first line is what carries the version across cmake, clang, ninja, make
# and pkg-config alike, and a tool that is absent says so in the same field
# rather than dropping out of the list -- a shorter list would otherwise
# fingerprint the same as a different set of tools.
fingerprint() {
    local tool path version spec
    # The declaration side: what the workflows say to install.
    for spec in "$root"/.github/workflows/*.yml; do
        [ -f "$spec" ] || continue
        printf 'workflow:%s\t%s\n' "${spec##*/}" "$(ubq_digest "$spec")"
    done
    for tool in "$@"; do
        path="$(command -v "$tool" 2>/dev/null)"
        if [ -z "$path" ]; then
            printf '%s\tABSENT\n' "$tool"
            continue
        fi
        version="$("$tool" --version 2>/dev/null | head -1)"
        [ -n "$version" ] || version="(no --version)"
        printf '%s\t%s\n' "$tool" "$version"
    done
}

current="$(fingerprint "$@")"

case "$mode" in
check)
    if printf '%s\n' "$current" | grep -q '	ABSENT$'; then
        echo "$group: a tool is absent, so the install runs:"
        printf '%s\n' "$current" | grep '	ABSENT$' | sed 's/^/  /'
        exit 1
    fi
    if [ ! -f "$stamp" ]; then
        echo "$group: no stamp on this runner, so the install runs once and records"
        exit 1
    fi
    if [ "$current" = "$(cat "$stamp" 2>/dev/null)" ]; then
        echo "$group: unchanged since the last install, so nothing is installed"
        printf '%s\n' "$current" | sed 's/^/  /'
        exit 0
    fi
    echo "$group: the toolchain moved, so the install runs. What changed:"
    diff <(cat "$stamp" 2>/dev/null) <(printf '%s\n' "$current") | sed 's/^/  /'
    exit 1
    ;;
record)
    if printf '%s\n' "$current" | grep -q '	ABSENT$'; then
        echo "$group: still absent after the install, so nothing is recorded:" >&2
        printf '%s\n' "$current" | grep '	ABSENT$' | sed 's/^/  /' >&2
        exit 1
    fi
    mkdir -p "$stamp_dir" || exit 1
    printf '%s\n' "$current" > "$stamp" || exit 1
    echo "$group: recorded"
    printf '%s\n' "$current" | sed 's/^/  /'
    ;;
*)
    echo "usage: ci-toolchain.sh check|record <group> <tool>..." >&2
    exit 2
    ;;
esac
