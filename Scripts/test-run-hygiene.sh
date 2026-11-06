#!/usr/bin/env bash
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

# test-run-hygiene.sh — sourced by a script that runs tests which start
# processes: what the run may leave behind is nothing.
#
# A test that starts a daemon and fails, hangs or is interrupted before its
# own teardown leaves the daemon running, and a daemon started detached
# (--ensure-daemon, or a client's bridge redialling) is in a session of its own
# where no process-group kill reaches it, so such daemons collect in build
# roots. And a test that names no state directory writes into the one of
# whoever runs it: their sockets, pid files, tokens and log.
#
# So a run marks its environment, which everything it starts inherits, and
# gives what it runs a scratch state directory by default. When it ends it
# asks which marked processes are still alive outside its own tree, ends them,
# and fails if a completed run had any, or if the real state directory gained
# an entry the run may have made. An entry of a daemon outside the run (the
# person's app, a session they opened over SSH) is reported as a note instead.
# A watcher that outlives the run does the ending when the run is killed
# outright, as a job's limit does, since no trap answers that.
# Scripts/test-run-hygiene.py holds the mechanics.
#
# A stanza whose daemon starts a login shell runs it under
# ubq_with_hermetic_shell, so the shell reads none of that person's dotfiles:
# the hooks in them start processes that outlive the session.
#
# The mark is by run, not by build root: ctest runs stanzas side by side from
# one root, and a developer's own daemon may be running from that root's host.
#
#   ubq_hygiene_begin          mark the run, point UBIQUITTY_HOME at scratch,
#                              record the real state directory, start the watcher
#   ubq_hygiene_judge NAME     say what a completed run left; status 1 if anything
#   ubq_hygiene_reap           end what is left and remove the scratch
#   ubq_with_hermetic_shell COMMAND…
#                              run COMMAND with /bin/sh as the shell and an
#                              empty scratch HOME
#
# The caller owns its traps: judge on a completed exit, reap on every exit.

UBQ_HYGIENE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/test-run-hygiene.py"
UBQ_RUN_SCRATCH=""

ubq_hygiene_begin() {
    UBQ_REAL_STATE_DIR="${UBQ_HYGIENE_REAL_STATE_DIR:-$HOME/.ubiquitty}"
    UBQ_RUN_SCRATCH="$(mktemp -d /tmp/ubq-run.XXXXXX)"
    export UBQ_TEST_RUN="${UBQ_TEST_RUN:+$UBQ_TEST_RUN/}$$.${UBQ_RUN_SCRATCH##*.}"
    export UBIQUITTY_HOME="$UBQ_RUN_SCRATCH/.ubiquitty"
    python3 "$UBQ_HYGIENE" snapshot "$UBQ_REAL_STATE_DIR" "$UBQ_RUN_SCRATCH/real-state.listing"
    env -u UBQ_TEST_RUN UBQ_WATCHED_RUN="$UBQ_TEST_RUN" python3 "$UBQ_HYGIENE" watch $$ "$UBQ_RUN_SCRATCH"
}

ubq_hygiene_judge() {
    local name="$1" left gained added outside verdict=0
    [ -n "$UBQ_RUN_SCRATCH" ] || return 0
    left="$(python3 "$UBQ_HYGIENE" survivors $$ || true)"
    if [ -n "$left" ]; then
        {
            echo "$name: FAILED — $(printf '%s\n' "$left" | wc -l | tr -d ' ') process(es) left running by this run, pid $(printf '%s\n' "$left" | cut -f1 | tr '\n' ' ')(pid, started, command):"
            printf '%s\n' "$left" | sed 's/^/  /'
            echo "  Each was ended. A test reaps what it starts on every path, not only its last line."
        } >&2
        verdict=1
    fi
    gained="$(python3 "$UBQ_HYGIENE" added "$UBQ_REAL_STATE_DIR" "$UBQ_RUN_SCRATCH/real-state.listing" || true)"
    added="$(printf '%s\n' "$gained" | awk -F '\t' '$1 == "made" { print $2 }')"
    outside="$(printf '%s\n' "$gained" | awk -F '\t' '$1 == "outside" { print "  " $2 ": " $3 }')"
    if [ -n "$outside" ]; then
        {
            echo "$name: note — the state directory $UBQ_REAL_STATE_DIR gained $(printf '%s\n' "$outside" | wc -l | tr -d ' ') entr(ies) from a daemon outside this run, not counted against it:"
            printf '%s\n' "$outside"
        } >&2
    fi
    if [ -n "$added" ]; then
        {
            echo "$name: FAILED — the state directory $UBQ_REAL_STATE_DIR gained $(printf '%s\n' "$added" | wc -l | tr -d ' ') entr(ies) during this run: $(printf '%s\n' "$added" | tr '\n' ' ')"
            echo "  A test's daemon belongs under UBIQUITTY_HOME, which this run points at its scratch."
            echo "  (An entry no live daemon outside the run serves, and that is not named for the"
            echo "  installed host, counts here even when something else on this machine wrote it.)"
        } >&2
        verdict=1
    fi
    return "$verdict"
}

ubq_with_hermetic_shell() {
    # The shell a daemon starts is $SHELL, as a login shell, and it reads the
    # startup files under HOME (ZDOTDIR for zsh, XDG_CONFIG_HOME for fish).
    # /bin/sh with an empty HOME reads the system's /etc/profile and nothing of
    # the person's; the XDG directories move with HOME, so the daemon reads no
    # config.toml of theirs either. ENV and BASH_ENV would name a file an
    # interactive or non-interactive sh reads, and go.
    local home
    if [ -z "$UBQ_RUN_SCRATCH" ]; then
        echo "ubq_with_hermetic_shell: the run has no scratch directory (ubq_hygiene_begin has not run)" >&2
        return 1
    fi
    home="$(mktemp -d "$UBQ_RUN_SCRATCH/home.XXXXXX")" || return 1
    env -u ENV -u BASH_ENV HOME="$home" ZDOTDIR="$home" SHELL=/bin/sh \
        XDG_CONFIG_HOME="$home/.config" XDG_DATA_HOME="$home/.local/share" \
        XDG_STATE_HOME="$home/.local/state" XDG_CACHE_HOME="$home/.cache" "$@"
}

ubq_hygiene_reap() {
    local verdict=0
    [ -n "$UBQ_RUN_SCRATCH" ] || return 0
    python3 "$UBQ_HYGIENE" reap $$ >&2 || verdict=1
    rm -rf "$UBQ_RUN_SCRATCH"
    UBQ_RUN_SCRATCH=""
    return "$verdict"
}
