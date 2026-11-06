#!/usr/bin/env bash
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

# A successful gate is reusable only for the exact source snapshot, comparison
# base, command, and toolchain environment that produced it. Failed and skipped
# gates never enter the cache, so retrying a red push reruns every unresolved
# verdict while preserving the work already proved by that same snapshot.

ubq_gate_cache_digest() {
    if command -v shasum >/dev/null 2>&1; then
        shasum -a 256 | awk '{ print $1 }'
        return
    fi
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum | awk '{ print $1 }'
        return
    fi
    return 1
}

# A git command aimed at a repository other than the one a hook is running for must
# not inherit the variables that name that one.
ubq_foreign_git() {
    env -u GIT_DIR -u GIT_WORK_TREE -u GIT_INDEX_FILE -u GIT_PREFIX -u GIT_COMMON_DIR \
        -u GIT_OBJECT_DIRECTORY -u GIT_ALTERNATE_OBJECT_DIRECTORIES -u GIT_QUARANTINE_PATH git "$@"
}

ubq_gate_cache_environment() {
    {
        printf 'tier=%s\n' "$UBQ_GATE_CACHE_TIER"
        printf 'path=%s\n' "$PATH"
        printf 'cc=%s\n' "${CC:-}"
        printf 'cxx=%s\n' "${CXX:-}"
        printf 'sdkroot=%s\n' "${SDKROOT:-}"
        # A UBIQUITTY_ variable changes what a gate does, so it belongs in the
        # key -- except the ones that steer this cache. Those change where a
        # verdict is stored and whether it is consulted, never what the gate
        # would say, and leaving them in makes turning the log on invalidate
        # every entry the log was meant to explain.
        env | LC_ALL=C sort | sed -n '/^UBIQUITTY_/p' | grep -v '^UBIQUITTY_GATE_CACHE' || true
        uname -srm
        # A library this repository carries is read under its own rules, and the
        # repository's snapshot sees a submodule as the commit it pins, so an edit
        # inside the library's checkout is part of the key here.
        local library
        for library in "${UBQ_CORE_ROOT:-}" "${UBQ_KIT_ROOT:-}"; do
            if [ -n "$library" ] && [ "$library" != "$UBQ_GATE_CACHE_ROOT" ]; then
                printf 'library=%s\n' "$library"
                ubq_foreign_git -C "$library" rev-parse HEAD 2>&1 || true
                ubq_foreign_git -C "$library" status --porcelain 2>&1 || true
                ubq_foreign_git -C "$library" diff HEAD 2>&1 || true
            fi
        done
        for tool in git cmake c++ swiftc python3 emcc; do
            local executable
            executable="$(command -v "$tool" 2>/dev/null || true)"
            printf '%s=%s\n' "$tool" "$executable"
            if [ -n "$executable" ]; then
                "$executable" --version 2>&1 | head -1 || true
            fi
        done
    } | ubq_gate_cache_digest
}

ubq_gate_cache_snapshot() {
    {
        git -C "$UBQ_GATE_CACHE_ROOT" rev-parse HEAD origin/main
        git -C "$UBQ_GATE_CACHE_ROOT" diff --no-ext-diff --binary HEAD --
        while IFS= read -r -d '' path; do
            printf 'untracked:%s\0' "$path"
            git -C "$UBQ_GATE_CACHE_ROOT" hash-object --no-filters "$path"
        done < <(git -C "$UBQ_GATE_CACHE_ROOT" ls-files --others --exclude-standard -z)
    } | ubq_gate_cache_digest
}

# The content of one gate's declared inputs, and nothing else.
#
# A gate's verdict can only depend on what the gate reads, so a key over exactly
# the paths it is allowed to read is sound rather than approximate — as far as
# the declaration is true. Scripts/check-gate-scopes.py holds it to that: it
# walks each gate's command and the scripts that command names and fails when one
# reaches a path the scope omits. It reads text, so a computed path stays outside
# what it can see, and its own header says which check would close that.
#
# origin/main is absent from the key because no gate keyed this way reads it. The
# whole-tree key carries it for the gates that compare against the base branch; a
# scoped gate builds the tree in front of it, and pinning the base would
# invalidate every scope each time main moved for a reason none of them can see.
ubq_gate_cache_scoped_snapshot() {
    {
        git -C "$UBQ_GATE_CACHE_ROOT" ls-tree -r HEAD -- "$@"
        git -C "$UBQ_GATE_CACHE_ROOT" diff --no-ext-diff --binary HEAD -- "$@"
        while IFS= read -r -d '' path; do
            printf 'untracked:%s\0' "$path"
            git -C "$UBQ_GATE_CACHE_ROOT" hash-object --no-filters "$path"
        done < <(git -C "$UBQ_GATE_CACHE_ROOT" ls-files --others --exclude-standard -z -- "$@")
    } | ubq_gate_cache_digest
}

# The key for a gate whose inputs are declared. The scope is digested alongside
# them: a widened or narrowed declaration asks a different question, and must not
# read as a hit on the answer to the old one.
ubq_gate_cache_scoped_key() {
    [ "${UBQ_GATE_CACHE_ENABLED:-0}" = 1 ] || return 1
    local scope="$1"; shift
    local snapshot
    snapshot="$(ubq_gate_cache_scoped_snapshot $scope)" || return 1
    [ -n "$snapshot" ] || return 1
    {
        printf '%s\0' "scoped" "$scope" "$snapshot" "$UBQ_GATE_CACHE_ENVIRONMENT" "$@"
    } | ubq_gate_cache_digest
}

ubq_gate_cache_init() {
    UBQ_GATE_CACHE_ROOT="$1"
    UBQ_GATE_CACHE_TIER="$2"
    UBQ_GATE_CACHE_ENABLED=0
    UBQ_GATE_CACHE_ENVIRONMENT=""
    UBQ_GATE_CACHE_DIRECTORY=""

    [ "${UBIQUITTY_PREPUSH_CACHE:-on}" != "off" ] || return 0
    command -v shasum >/dev/null 2>&1 || command -v sha256sum >/dev/null 2>&1 || return 0

    UBQ_GATE_CACHE_DIRECTORY="${UBIQUITTY_PREPUSH_CACHE_DIR:-$(git -C "$UBQ_GATE_CACHE_ROOT" rev-parse --git-path ubiquitty-pre-push-cache 2>/dev/null || true)}"
    [ -n "$UBQ_GATE_CACHE_DIRECTORY" ] || return 0
    case "$UBQ_GATE_CACHE_DIRECTORY" in
        /*) ;;
        *) UBQ_GATE_CACHE_DIRECTORY="$UBQ_GATE_CACHE_ROOT/$UBQ_GATE_CACHE_DIRECTORY" ;;
    esac
    mkdir -p "$UBQ_GATE_CACHE_DIRECTORY" 2>/dev/null || return 0
    UBQ_GATE_CACHE_ENVIRONMENT="$(ubq_gate_cache_environment)" || return 0
    [ -n "$UBQ_GATE_CACHE_ENVIRONMENT" ] || return 0
    UBQ_GATE_CACHE_ENABLED=1
}

ubq_gate_cache_key() {
    [ "${UBQ_GATE_CACHE_ENABLED:-0}" = 1 ] || return 1
    local snapshot
    snapshot="$(ubq_gate_cache_snapshot)" || return 1
    [ -n "$snapshot" ] || return 1
    {
        printf '%s\0' "$snapshot" "$UBQ_GATE_CACHE_ENVIRONMENT" "$@"
    } | ubq_gate_cache_digest
}

ubq_gate_cache_has() {
    [ -n "${1:-}" ] && [ -f "$UBQ_GATE_CACHE_DIRECTORY/$1.passed" ]
}

ubq_gate_cache_store() {
    local key="$1"
    [ -n "$key" ] || return 0
    local temporary
    temporary="$(mktemp "$UBQ_GATE_CACHE_DIRECTORY/.passed.XXXXXX")" || return 0
    printf 'passed=%s\n' "$(date -u '+%Y-%m-%dT%H:%M:%SZ')" > "$temporary"
    mv "$temporary" "$UBQ_GATE_CACHE_DIRECTORY/$key.passed"
}
