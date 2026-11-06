#!/usr/bin/env bash
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

# gate-verdict-cache.sh — a gate's verdict is a fact about the tree it read.
#
# A check-style run's time is not where it looks: not per-gate startup, and not
# reading the tree once per section, but each gate applying its own rules to the
# same tree -- not duplicated work but different questions, so there is no
# faster way to ask them all.
#
# What there is, is not asking again. A gate reads the tree and its own script
# and produces a verdict, so the same bytes give the same verdict. The commit
# gate and the push gate ask overlapping questions minutes apart about a tree
# that did not move between them, and a second push after a failure elsewhere
# asks every one of them again.
#
#   . Scripts/gate-verdict-cache.sh
#   gate_verdict_init
#   gate_verdict "acronyms" ./Scripts/check-acronyms.py
#
# What goes in the key, and why each part has to.
#
#   the tree's content       the answer is about these bytes, tracked and
#                            untracked alike: a file the tree does not track yet
#                            can carry the violation, and a key blind to it
#                            would replay a pass taken before the file existed
#   the build's state        some gates answer about compiled output rather than
#                            source -- nm over BuildHostTests, an executable
#                            under build/ -- and those bytes are gitignored, so
#                            the tree fingerprint cannot see them. Path, size
#                            and mtime, because reading every object would cost
#                            more than the gates this is saving
#   the fork point           a gate given a ref asks about what this branch
#                            changed, which is the merge base and not the ref's
#                            tip, so a teammate landing on main leaves these
#                            verdicts standing and a rebase retires them
#   the command              the same script with different arguments asks a
#                            different question, and the script's own bytes are
#                            in the tree fingerprint already
#   the toolchain            shellcheck, clang-format and python differ between
#                            machines and versions
#
# The fingerprint covers everything, so any edit anywhere retires every verdict.
# That is the point rather than a limitation: a scope derived per gate from the
# paths its script names comes out narrower than what the gate reads --
# check-shell.sh reads every .sh in the tree while naming only Scripts/, and a
# gate handed "." reads all of it whatever it names.
# A scope too narrow replays a pass over files that changed, which is the silent
# failure this file exists to avoid, so there are no scopes.
#
# It is taken once, before any gate runs, for the same reason:
# Scripts/check-gate-subjects.sh plants an untracked file and removes it again,
# and a fingerprint taken per gate would move underneath the run.
#
# Only a pass is remembered. A red verdict is re-asked on the next run, which is
# the same choice Scripts/pre-push-cache.sh makes and for the same reason: a
# retry after a failure is someone asking whether it is fixed, and answering
# that from a cache answers a different question.
#
# Scripts/check-gate-verdict-cache.sh asserts each of these against the live
# repository, and each of its arms is checked against a broken cache.

# shellcheck source=Scripts/pre-push-cache.sh
. "${BASH_SOURCE[0]%/*}/pre-push-cache.sh"

GATE_VERDICT_ENABLED=0
GATE_VERDICT_DIR=""
GATE_VERDICT_ENVIRONMENT=""
GATE_VERDICT_TREE=""
GATE_VERDICT_BUILD=""
GATE_VERDICT_LOG="${UBIQUITTY_GATE_CACHE_LOG:-}"

# Everything a gate could read, in one digest.
#
# Untracked files are inside the question and are hashed by content. Files git
# is told to ignore are not: a gate that reads build output would be answering
# about something the tree does not carry, and every gate here reads sources.
gate_verdict_fingerprint() {
    local root="$UBQ_GATE_CACHE_ROOT"
    {
        git -C "$root" ls-tree -r HEAD
        git -C "$root" diff --no-ext-diff --binary HEAD
        while IFS= read -r -d '' path; do
            printf 'untracked:%s\0' "$path"
            git -C "$root" hash-object --no-filters "$path" 2>/dev/null || printf 'unreadable\0'
        done < <(git -C "$root" ls-files --others --exclude-standard -z)
    } | ubq_gate_cache_digest
}

# The build tree's state, for the gates that read it.
#
# Asked of Scripts/gate_corpus.py rather than computed here. The shell form
# needed `stat`, which spells a per-file format two ways -- and a machine can
# have both, GNU coreutils shadowing the BSD one on PATH, where -f means
# --file-system. The digest that resulted was free-space counters: a number that
# changes on its own between two runs of the same tree.
gate_verdict_build_state() {
    python3 "${BASH_SOURCE[0]%/*}/gate_corpus.py" --build-state 2>/dev/null
}

gate_verdict_init() {
    GATE_VERDICT_ENABLED=0
    [ "${UBIQUITTY_GATE_CACHE:-on}" != "off" ] || return 0
    command -v shasum >/dev/null 2>&1 || command -v sha256sum >/dev/null 2>&1 || return 0
    GATE_VERDICT_DIR="$(git rev-parse --git-path ubiquitty-gate-verdicts 2>/dev/null || true)"
    [ -n "$GATE_VERDICT_DIR" ] || return 0
    case "$GATE_VERDICT_DIR" in
        /*) ;;
        *) GATE_VERDICT_DIR="$(git rev-parse --show-toplevel)/$GATE_VERDICT_DIR" ;;
    esac
    mkdir -p "$GATE_VERDICT_DIR" 2>/dev/null || return 0
    # Exported: ubq_gate_cache_environment reads it from
    # Scripts/pre-push-cache.sh.
    export UBQ_GATE_CACHE_TIER="gate-verdict"
    UBQ_GATE_CACHE_ROOT="$(git rev-parse --show-toplevel)"
    GATE_VERDICT_ENVIRONMENT="$(ubq_gate_cache_environment)" || return 0
    [ -n "$GATE_VERDICT_ENVIRONMENT" ] || return 0
    GATE_VERDICT_TREE="$(gate_verdict_fingerprint)" || return 0
    [ -n "$GATE_VERDICT_TREE" ] || return 0
    GATE_VERDICT_BUILD="$(gate_verdict_build_state)" || return 0
    [ -n "$GATE_VERDICT_BUILD" ] || return 0
    # A gate can source this file and initialise again -- the one that checks
    # this cache does exactly that -- and truncating the log there would drop
    # the tally the outer run had built. The first init claims the log and the
    # nested ones append to it.
    if [ -n "$GATE_VERDICT_LOG" ] && [ -z "${GATE_VERDICT_LOG_OWNED:-}" ]; then
        : > "$GATE_VERDICT_LOG"
        export GATE_VERDICT_LOG_OWNED=1
    fi
    GATE_VERDICT_ENABLED=1
}

gate_verdict_key() { # <command...>
    local root="$UBQ_GATE_CACHE_ROOT"
    {
        printf '%s\0' "$GATE_VERDICT_ENVIRONMENT" "$GATE_VERDICT_TREE" \
            "$GATE_VERDICT_BUILD" "$@"
        # A gate told to check "since origin/main" reads a ref, and the name in
        # the command stays the same when the ref moves. What such a gate asks
        # about is the fork point -- Scripts/check-style.py takes
        # `git merge-base "$STYLE_SINCE" HEAD` and check-slop.py takes
        # `REF...HEAD`, both of which resolve there -- so the key records the
        # merge base rather than the ref's tip.
        local argument resolved
        for argument in "$@"; do
            case "$argument" in
                -*|.|./*|/*) continue ;;
            esac
            resolved="$(git -C "$root" rev-parse --verify --quiet "$argument^{commit}" 2>/dev/null || true)"
            [ -n "$resolved" ] || continue
            printf 'forkpoint:%s=%s\0' "$argument" \
                "$(git -C "$root" merge-base "$resolved" HEAD 2>/dev/null || printf '%s' "$resolved")"
        done
    } | ubq_gate_cache_digest
}

# A gate runs in its own subshell, so a counter variable would not survive it.
# The tally is a file for that reason, and naming each gate makes a cache that
# stopped reusing anything say which gate stopped.
gate_verdict_record() { # <reused|asked> <gate>
    [ -n "$GATE_VERDICT_LOG" ] || return 0
    printf '%s\t%s\n' "$1" "$2" >> "$GATE_VERDICT_LOG"
}

# Runs a gate, or replays the verdict it already gave about this tree.
#
# Output is replayed on stdout exactly as it was captured, so a caller that
# redirects into a file sees what it would have seen. The status is the gate's.
gate_verdict() { # <name> <command...>
    local name="$1"; shift
    if [ "$GATE_VERDICT_ENABLED" != 1 ]; then
        "$@"
        return $?
    fi
    local key
    key="$(gate_verdict_key "$@")" || key=""
    if [ -n "$key" ] && [ -f "$GATE_VERDICT_DIR/$key.pass" ]; then
        cat "$GATE_VERDICT_DIR/$key.pass"
        gate_verdict_record reused "$name"
        return 0
    fi
    gate_verdict_record asked "$name"
    local captured status
    captured="$(mktemp)"
    "$@" > "$captured" 2>&1
    status=$?
    cat "$captured"
    # A stand-down is not a pass: a gate that found no tool or no build exits
    # zero having checked nothing, and what it looked for can sit outside the
    # key, so it is asked again rather than replayed. The same line
    # Scripts/gate_verdict.py matches.
    if [ "$status" -eq 0 ] && [ -n "$key" ] \
        && ! grep -Eq '^[[:space:]]*[a-z][a-z0-9.-]*: SKIPPED' "$captured"; then
        local temporary
        temporary="$(mktemp "$GATE_VERDICT_DIR/.pass.XXXXXX")" \
            || { rm -f "$captured"; return "$status"; }
        cat "$captured" > "$temporary"
        mv "$temporary" "$GATE_VERDICT_DIR/$key.pass"
    fi
    rm -f "$captured"
    return "$status"
}

# What the run reused, for a summary line. A cache nobody can see the effect of
# is one nobody can tell has stopped working.
gate_verdict_summary() {
    [ "$GATE_VERDICT_ENABLED" = 1 ] || { printf 'verdict cache off'; return 0; }
    [ -f "$GATE_VERDICT_LOG" ] || { printf 'verdict cache on'; return 0; }
    printf '%d reused, %d asked' \
        "$(grep -c '^reused' "$GATE_VERDICT_LOG" 2>/dev/null || echo 0)" \
        "$(grep -c '^asked' "$GATE_VERDICT_LOG" 2>/dev/null || echo 0)"
}
