#!/usr/bin/env bash
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

# build-root.sh — where a checkout's build output goes, for shell callers.
#
# Sourced, not run:
#
#     . "$(dirname "$0")/build-root.sh"
#     cmake -S "$ROOT" -B "$UBQ_BUILD_ROOT/macos"
#
# Outside the checkout. A build tree inside a worktree makes `git status` slow,
# makes every removal of a worktree a decision about gigabytes, and leaves the
# artefacts behind when the worktree goes, including in a worktree whose
# repository has already lost track of it.
#
# The layout is Xcode's, for Xcode's reason: a name a person can read, and a
# hash so two checkouts sharing a basename cannot collide.
#
#     ~/Library/Developer/Xcode/DerivedData/<basename>-<16 hex of the path's md5>
#
# Xcode's own directory, so Xcode's "Delete Derived Data" and the Storage pane
# in System Settings clear these too. Xcode names its own <Name>-<28 lowercase
# letters>; a hex digest cannot collide with that and says which entries are
# ours.
#
# On non-macOS hosts, the parent is $XDG_CACHE_HOME/ubiquitty/build, or
# ~/.cache/ubiquitty/build when XDG_CACHE_HOME is unset or relative.
#
# UBIQUITTY_DERIVED_DATA moves the whole tree; UBQ_BUILD_ROOT names one build
# root outright and skips the derivation.
#
# Scripts/build_root.py derives the identical path for python callers, and
# Scripts/check-build-root.py holds the two to agreeing: a gate fingerprinting
# one directory while the build writes to another reports a verdict about bytes
# nobody wrote.

ubq_build_root() { # [checkout] -> the build root for that checkout
    if [ -n "${UBQ_BUILD_ROOT:-}" ]; then
        printf '%s' "$UBQ_BUILD_ROOT"
        return 0
    fi
    local start="${1:-$PWD}"
    local root
    root="$(cd "$start" 2>/dev/null && git rev-parse --show-toplevel 2>/dev/null)"
    [ -n "$root" ] || root="$(cd "$start" 2>/dev/null && pwd)"
    # A library checked out as a submodule builds into its consumer's root, the
    # checkout a person builds and cleans; build_root.py does the same.
    local parent
    while parent="$(git -C "$root" rev-parse --show-superproject-working-tree 2>/dev/null)" && [ -n "$parent" ]; do
        root="$parent"
    done

    # Asked of Scripts/build_root.py rather than derived again here. Xcode's
    # encoding is base 26 over two 64-bit halves of an md5, and a second
    # implementation of it in shell would be a second thing to keep in step --
    # a build writing to one directory while a gate fingerprints another is a
    # verdict about bytes nobody wrote.
    python3 "${BASH_SOURCE[0]%/*}/build_root.py" "$root"
}

# Points the checkout's compile_commands.json at one build's compile database,
# for clangd, which looks for it in the source tree and never in the build root.
# A file, or a link that resolves, is left as it is, so the first build that
# claims the name keeps it; a link whose target is gone is claimed again. A
# build tree under the checkout is run-linux.sh's container bind mount, whose
# paths mean nothing on the host that owns the checkout, so it claims nothing.
ubq_link_compile_database() { # <build directory> <checkout>
    local database="$1/compile_commands.json" link="$2/compile_commands.json"
    case "$1/" in "$2"/*) return 0 ;; esac
    [ -f "$database" ] || return 0
    if [ -L "$link" ] && [ ! -e "$link" ]; then
        rm -f "$link"
    fi
    [ -e "$link" ] || ln -s "$database" "$link" 2>/dev/null || true
}

# The compiler-cache settings ride along here because this is what every script
# that builds already sources, and a cmake one of them launches inherits them.
# Ahead of the derivation below, which returns on failure: settings placed
# after it would be exported only when that succeeded, so a machine whose build
# root could not be derived would also quietly compile uncached.
# shellcheck source=Scripts/ccache-env.sh
. "${BASH_SOURCE[0]%/*}/ccache-env.sh"

# The value every sourcing script reads. Derived once here so a script that
# calls it in a loop pays for it once, and so `set -u` never meets it unset.
UBQ_BUILD_ROOT="$(ubq_build_root "${ROOT:-$PWD}")" || return 1
export UBQ_BUILD_ROOT
