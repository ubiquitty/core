#!/usr/bin/env bash
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

#
# gen-portable-core-sources.sh - regenerate / verify the canonical
# libUbiquittyCore source list (Scripts/portable-core-sources.txt).
#
# The list is the single source of truth for the portable Core translation
# units, consumed by the CMake `ubiquitty-core` target and the bash perf
# drivers. This script derives it mechanically by globbing the portable
# modules (minus the external-dep QUIC carriers), so a file split can never
# silently drift hand-maintained copies again.
#
# Usage:
#   Scripts/gen-portable-core-sources.sh            # check: fail if stale
#   Scripts/gen-portable-core-sources.sh --write    # rewrite the list in place
#   Scripts/gen-portable-core-sources.sh --print    # print the derived list
#
# Exit 0 = list is current (or written), 1 = stale (check mode), 2 = usage.

set -euo pipefail

# The list is sorted by byte order, so the answer is the same on every machine:
# `sort` in a UTF-8 locale orders by collation, which differs between a person's
# shell, a hook's environment and CI.
export LC_ALL=C

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
LIST="$ROOT/Scripts/portable-core-sources.txt"

# The directories under Source/ whose every .cpp belongs in the dependency-free
# library. Together with the PAL contract and backend files derive() names one
# by one, this is check-boundaries.py Rule 7's portable-module set plus
# Protocol/Tmux, the control-mode parser Terminal embeds. Every one of those
# modules sits below Source/Core/ — Codec, Unicode, Image, Rendering, Runtime,
# Protocol/{Sync,Tmux}, Preferences, Themes, Selection, Input, Session,
# Terminfo, Diagnostics, Network, Transport — so Core alone covers them. Each
# entry has to be an existing directory; derive() refuses one that is not.
PORTABLE_MODULES=(Core)

# TUs excluded from the dependency-free set: each is opt-in, named by the
# frontends that want it. Keep this list tiny and explicit.
#
#   QUIC carriers    pull external ngtcp2 + AWS-LC.
EXCLUDE_RE='Transport/QUICClient\.cpp|Transport/QUICChannel\.cpp'

derive() {
    {
        for module in "${PORTABLE_MODULES[@]}"; do
            # Said out loud, because the alternative is silent: `find` on a
            # missing directory fails, `set -e` then ends this group early, and
            # the list comes out short while every check that reads it passes.
            [ -d "$ROOT/Source/$module" ] || {
                echo "gen-portable-core-sources.sh: PORTABLE_MODULES lists Source/$module, which is not a directory" >&2
                exit 1
            }
            find "$ROOT/Source/$module" -name '*.cpp'
        done
        # The PAL contract (portable hook storage + the submit/drain queue) and
        # the one bundled PAL backend (POSIX system services) the lib ships.
        echo "$ROOT/Source/PAL/PAL.cpp"
        echo "$ROOT/Source/PAL/TaskQueue.cpp"
        # The lossless write queue every non-blocking carrier sits behind. Its
        # sink is a callback, so it names no descriptor and no platform.
        echo "$ROOT/Source/PAL/OutputQueue.cpp"
        # The process queries belong to the contract rather than to a backend.
        # PAL::Process is called from Core's process subtree, title formatting
        # and session information, so the archive cannot link without both.
        printf '%s\n' "$ROOT/Source/PAL/Process.cpp" "$ROOT/Source/PAL/ProcessSignal.cpp"
        # PAL/Process.cpp names a session's foreground process, and what it
        # names depends on where that process leads.
        echo "$ROOT/Source/PAL/RemoteTarget.cpp"
        # The bundled POSIX backends: system services, readiness waiting, and
        # the local-socket carrier. Scripts/cmake/UbiquittyCoreSources.cmake
        # swaps each for its Wasm counterpart, so a new one added here needs the
        # same swap or the wasm build links a backend whose syscalls that target
        # has no answer for.
        echo "$ROOT/Source/Platform/POSIX/SystemPAL.cpp"
        echo "$ROOT/Source/Platform/POSIX/DropFiles.cpp"
        echo "$ROOT/Source/Platform/POSIX/ReceivedDropFiles.cpp"
        # Preferences is bundled for the same reason storage is: Core acquires
        # its configuration through this seam, so a consumer that had to supply
        # it could not read a config file until they had written a backend.
        echo "$ROOT/Source/Platform/POSIX/Preferences.cpp"
        echo "$ROOT/Source/Platform/POSIX/Poll.cpp"
        echo "$ROOT/Source/Platform/POSIX/Socket.cpp"
        echo "$ROOT/Source/Platform/POSIX/SocketPath.cpp"
        echo "$ROOT/Source/Platform/POSIX/Spawn.cpp"
        # Core/Terminfo/UbiquittyTerminfo.cpp calls PAL::processSpawnAndWait to
        # run tic, so the dependency-free archive does not link without a
        # backend for it — the same reason Process.cpp is here. iOS swaps this
        # one rather than trimming it (see
        # Scripts/cmake/UbiquittyCoreSources.cmake), which needs it present to
        # swap.
        echo "$ROOT/Source/Platform/POSIX/ProcessSpawnAndWait.cpp"
    } | sed "s#^$ROOT/##" | sort -u | grep -vE "$EXCLUDE_RE"
}

HEADER='# Scripts/portable-core-sources.txt - THE canonical libUbiquittyCore source set.
#
# Single source of truth for the portable Core translation units, read by both
# the CMake `ubiquitty-core` static-library target and the bash perf/test
# drivers (run-bench.sh / run-profile.sh) so the link list can never drift
# (the hand-maintained-list breakage this file prevents). Regenerate after a
# file split with: Scripts/gen-portable-core-sources.sh --write
#
# Contents: every .cpp under the portable modules (Core, Codec, Unicode, Image,
# Rendering, Runtime, Protocol/{Sync,Tmux}, Preferences, Themes, Selection, Input,
# Session, Terminfo, Preferences, Diagnostics, Network, Transport) PLUS the PAL
# contract (PAL/PAL.cpp, PAL/TaskQueue.cpp) and the bundled POSIX PAL
# backend (Platform/POSIX/SystemPAL.cpp). EXCLUDES the QUIC carriers
# (Platform/POSIX/QUICClient.cpp, Transport/QUICChannel.cpp) - they pull the external
# ngtcp2 + AWS-LC stack, so they are opt-in, not part of the dependency-free lib.
#
# Lines are repo-relative paths; blank lines and #-comments are ignored.
#
# Paths comprising the canonical set, NUL-rule: keep sorted (gen script enforces).
'

render() {
    printf '%s\n' "$HEADER"
    derive
}

# parse_list <file> - strip comments/blanks, emit the sorted source paths only.
parse_list() { grep -vE '^[[:space:]]*#|^[[:space:]]*$' "$1" | sort -u; }

MODE="check"
case "${1:-}" in
    --write) MODE="write" ;;
    --print) MODE="print" ;;
    --check|"") MODE="check" ;;
    -h|--help) sed -n '/^#!/d; /^[^#]/q; s/^# \{0,1\}//; p' "$0"; exit 0 ;;
    *) echo "gen-portable-core-sources.sh: unknown arg: $1" >&2; exit 2 ;;
esac

case "$MODE" in
    print)
        derive
        ;;
    write)
        render > "$LIST"
        echo "wrote $(derive | wc -l | tr -d ' ') source paths to $LIST" >&2
        ;;
    check)
        if ! diff <(parse_list "$LIST") <(derive) >/dev/null 2>&1; then
            echo "gen-portable-core-sources.sh: $LIST is STALE." >&2
            echo "  Re-run: Scripts/gen-portable-core-sources.sh --write" >&2
            echo "  Diff (checked-in < vs derived >):" >&2
            diff <(parse_list "$LIST") <(derive) | sed 's/^/    /' >&2 || true
            exit 1
        fi
        echo "portable-core-sources.txt: up to date ($(derive | wc -l | tr -d ' ') TUs)"
        ;;
esac
