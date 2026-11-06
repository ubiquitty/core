#!/usr/bin/env bash
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

#
# Tests/Host/smoke.sh — ubiquitty-host PERSISTENCE smoke test.
#
# Proves the tmux model end to end on the local machine: start a detached
# daemon, attach over the same transport the real client uses
# (`ubiquitty-host --attach`'s stdio), drive a marker command into the
# session, confirm it reaches the screen, DROP the attach, then reattach
# and confirm the same live session still shows the marker (the PTY +
# Core Ubiquitty::Terminal survived the client going away). A second marker typed on the
# reattached client confirms input still drives the surviving shell.
#
# The "client" is Tests/Host/smoke_client — a tiny headless Sync-protocol
# speaker (not the macOS app) that greets, reads checkpoints into a passive
# Ubiquitty::Terminal via the real applySnapshot, and scans the visible grid. Its stdio
# is bridged to the daemon by a child `ubiquitty-host --attach`, wired here
# with two FIFOs, exactly as ssh would bridge it on a remote host.
#
# Hermetic: runs against an isolated $UBIQUITTY_HOME under a temp dir, so it
# never touches the developer's real ~/.ubiquitty or a daemon they're using.
#
# Env:
#   UBIQUITTY_HOST_BIN   path to ubiquitty-host (default: shared build root/host)
#   UBIQUITTY_SMOKE_BUILD Core CMake build directory for smoke_client
#   UBIQUITTY_SMOKE_TMPDIR short temp root for Unix socket paths (default /tmp)
#
# Exit 0 iff: handshake + first marker visible, and after reattach the
# marker is still visible on the same session, and a post-reattach marker
# lands too.

set -uo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
# shellcheck source=Scripts/build-root.sh
. "$ROOT/Scripts/build-root.sh"
HOST_BIN="${UBIQUITTY_HOST_BIN:-$UBQ_BUILD_ROOT/host/ubiquitty-host}"
OUT="${UBIQUITTY_SMOKE_BUILD:-$UBQ_BUILD_ROOT/host-smoke}"
CLIENT="$OUT/Tests/ubq-host-smoke-client"

fail() { echo "smoke: FAIL: $*" >&2; exit 1; }

[ -x "$HOST_BIN" ] || fail "ubiquitty-host not built at $HOST_BIN (run Scripts/build-host.sh)"

# --version prints the build's version, then its build identity in parentheses.
# The shape is what is held here: a host the build system did not version
# prints `0.0.0-dev`, which is not one. The number is not compared with the
# tree's, because it moves with every commit and this smoke also runs against
# a host an earlier commit built, where nothing the host compiles has changed
# since.
host_version="$("$HOST_BIN" --version)"
printf '%s\n' "$host_version" \
    | grep -Eq '^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(\.[1-9][0-9]*)? \([A-Za-z0-9 ,.+-]+\)$' \
    || fail "ubiquitty-host --version reported '$host_version', expected 'MAJOR.MINOR.PATCH[.N] (<build identity>)'"

# The Core target supplies its generated headers and transitive dependencies.
# The smoke client must link that same graph to exercise the real deserializer.
echo "smoke: building smoke_client"
cmake -S "$ROOT" -B "$OUT" -G Ninja \
    -DUBIQUITTY_FRONTEND=Core -DUBIQUITTY_BUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Debug \
    || fail "smoke_client configuration failed"
cmake --build "$OUT" --target ubq-host-smoke-client -j "${UBQ_BUILD_JOBS:-4}" \
    || fail "smoke_client did not compile"

# --- isolated control dir ----------------------------------------------
SMOKE_TMPDIR="${UBIQUITTY_SMOKE_TMPDIR:-/tmp}"
WORK="$(mktemp -d "$SMOKE_TMPDIR/ubiquitty-host-smoke.XXXXXX")"
export UBIQUITTY_HOME="$WORK/.ubiquitty"

cleanup() {
    # Kill the daemon(s) and any stragglers, drop the temp tree. The pidfile is
    # versioned (host-<id>.pid) alongside the versioned socket, so glob it.
    local f p
    for f in "$UBIQUITTY_HOME"/host*.pid; do
        [ -f "$f" ] || continue
        p="$(cat "$f" 2>/dev/null || true)"
        [ -n "${p:-}" ] && kill "$p" 2>/dev/null || true
    done
    sleep 0.2
    for f in "$UBIQUITTY_HOME"/host*.pid; do
        [ -f "$f" ] || continue
        p="$(cat "$f" 2>/dev/null || true)"
        [ -n "${p:-}" ] && kill -9 "$p" 2>/dev/null || true
    done
    rm -rf "$WORK"
}
trap cleanup EXIT

# One attach round: bridge smoke_client's stdio to the daemon through a
# child `ubiquitty-host --attach` (the production transport), wired with two
# FIFOs. $1.. are smoke_client args. Echoes the client's stderr (the
# status / expect line) and returns the client's exit code.
attach_round() {
    local up down rc
    up="$WORK/up.$$.$RANDOM"      # client -> attach stdin -> daemon
    down="$WORK/down.$$.$RANDOM"  # daemon -> attach stdout -> client
    mkfifo "$up" "$down" || fail "mkfifo failed"

    # The relay: stdin from $up, stdout to $down. Backgrounded.
    # Redirection ORDER matters: open `down` (write) before `up` (read) so
    # this matches the client's order below (it opens `down` for reading
    # first). Both processes then rendezvous on `down`, then on `up`. The
    # original `< up > down` opened a DIFFERENT FIFO first than the client,
    # so each parked in open() waiting for the other — a circular deadlock.
    "$HOST_BIN" --attach > "$down" < "$up" &
    local relay=$!

    # The client: stdin from $down, stdout to $up. Foreground; its stderr
    # carries the human-readable status line (incl. sessionId=N). Capture it to
    # a file the caller can grep, while still echoing it.
    "$CLIENT" "$@" < "$down" > "$up" 2> "$WORK/round.err"
    rc=$?
    cat "$WORK/round.err" >&2

    wait "$relay" 2>/dev/null || true
    rm -f "$up" "$down"
    return $rc
}

# --- 1. start the detached daemon --------------------------------------
echo "smoke: ensure-daemon"
"$HOST_BIN" --ensure-daemon --columns 80 --rows 24 || fail "--ensure-daemon returned nonzero"

# Confirm it actually came up (socket present) and is detached (its own
# session, not a child of this shell — ensure-daemon already waited for
# the bind, so the socket existing is the liveness signal). The socket +
# pidfile are VERSIONED (host-<id>.sock from the binary's content hash);
# this hermetic $UBIQUITTY_HOME holds exactly one daemon, so glob resolves it.
SOCK_FILE="$(ls "$UBIQUITTY_HOME"/host*.sock 2>/dev/null | head -1)"
PID_FILE="$(ls "$UBIQUITTY_HOME"/host*.pid 2>/dev/null | head -1)"
[ -n "$SOCK_FILE" ] && [ -S "$SOCK_FILE" ] || fail "daemon socket not present after ensure-daemon"
DPID="$(cat "$PID_FILE" 2>/dev/null || true)"
[ -n "$DPID" ] || fail "no pidfile after ensure-daemon"
kill -0 "$DPID" 2>/dev/null || fail "daemon pid $DPID not alive"
echo "smoke: daemon pid=$DPID sock=$SOCK_FILE"

# --- 2. first attach: type a unique marker, expect it on screen --------
MARK1="UBHOST_$$_$RANDOM"
echo "smoke: attach #1 — type marker '$MARK1' and expect its echo"
# `printf %s ... \r` runs the command; the shell echoes "echo MARK" on the
# input line (and the result line), so the marker is on the grid regardless
# of the shell flavor.
attach_round expect --input "printf '<<%s>>' $MARK1 ; echo
" --wait-ms 800 "<<$MARK1>>" \
    || fail "attach #1 did not see marker '$MARK1' (session/echo broken)"

# The multiplexer assigned a session id (Greet sessionId 0 → new session). The
# reattach rounds must echo it (sessionId 0 would create a SECOND session, not
# reattach the first).
SID="$(grep -oE 'sessionID=[0-9]+' "$WORK/round.err" | head -1 | cut -d= -f2)"
[ -n "$SID" ] && [ "$SID" != "0" ] || fail "attach #1 reported no session id"
DID="$(grep -oE 'daemonInstanceID=[0-9]+' "$WORK/round.err" | head -1 | cut -d= -f2)"
[ -n "$DID" ] && [ "$DID" != "0" ] || fail "attach #1 reported no serving daemon identity"
echo "smoke: session id = $SID, daemon identity = $DID"

# The client exited cleanly here — that DROPS the attach (its end of the
# relay closed). The daemon must keep the session + PTY alive.
echo "smoke: attach #1 dropped; daemon should still be live"
kill -0 "$DPID" 2>/dev/null || fail "daemon died after client disconnect (NOT persistent)"
[ -S "$SOCK_FILE" ] || fail "socket gone after client disconnect"

# --- 3. reattach: the same marker must still be on the live grid -------
# A brand-new attach gets a fresh checkpoint of the SURVIVING session.
# If the marker text is still there, the screen state persisted across the
# disconnect — the core persistence guarantee.
echo "smoke: attach #2 (reattach) — marker '$MARK1' must STILL be present"
attach_round expect --session "$SID" --daemon "$DID" "<<$MARK1>>" \
    || fail "reattach lost marker '$MARK1' — session did NOT survive the drop"
echo "smoke: reattach confirmed the surviving session still shows '$MARK1'"

# --- 4. reattach also accepts fresh input ------------------------------
MARK2="UBHOST2_$$_$RANDOM"
echo "smoke: attach #3 — type a NEW marker '$MARK2' into the survivor"
attach_round expect --session "$SID" --daemon "$DID" --input "printf '<<%s>>' $MARK2 ; echo
" --wait-ms 800 "<<$MARK2>>" \
    || fail "survivor did not accept new input '$MARK2' after reattach"

# --- 5. command-line propagation ---------------------------------------
# Drive VS Code shell-integration's explicit command line (OSC 633;E) plus a
# FinalTerm command-end (OSC 133;D;0 — what bumps the completion generation the
# daemon turns into a typed completion). The record the smoke_client receives must
# carry the exact command line. Proves the
# Core→daemon→wire→client path end to end on the real daemon.
CMDLINE="explicit-cmd-$$-$RANDOM"
echo "smoke: attach #4 — OSC 633;E command line '$CMDLINE' must ride a typed completion"
# probe mode (no expect substring) so the client waits the full window instead of
# breaking the moment the typed echo appears — the completion rides after the shell
# runs the printf. printf emits the OSC bytes: 633;E sets Terminal::commandLine; 133;D;0
# bumps the completion generation the daemon turns into a typed effect.
attach_round probe --session "$SID" --daemon "$DID" --input \
"printf '\033]633;E;$CMDLINE\007\033]133;C\007\033]133;D;0\007'
" --wait-ms 2000 \
    || fail "attach #4 probe handshake failed"
SAW_CMDLINE="$(grep -oE "smoke: cmdline='[^']*'" "$WORK/round.err" | head -1 | sed "s/smoke: cmdline='//;s/'$//")"
[ "$SAW_CMDLINE" = "$CMDLINE" ] \
    || fail "typed completion did not carry the OSC 633;E command line (got '$SAW_CMDLINE', want '$CMDLINE')"
echo "smoke: command line '$CMDLINE' propagated over Sync on a typed completion"

# A delayed report arrives after the initial checkpoint, with no printed cells.
# Both the live update and a fresh attachment must carry the activity metadata.
# The line holds the terminal for thirty seconds after its reports, past both
# reads: a shell whose prompt hook writes OSC 3008 contexts of its own (bash
# under systemd's profile script does) ends this one at its next prompt, and
# the newest context is then the shell's.
ACTIVITY="activity-$$-$RANDOM"
attach_round probe --session "$SID" --daemon "$DID" --input \
"sleep 0.3; printf '\033]3008;start=$ACTIVITY;type=app\007\033]7501;state=done:app=$ACTIVITY\007'; sleep 30
" --wait-ms 800 || fail "activity update probe failed"
grep -F "activity context='$ACTIVITY' app='$ACTIVITY'" "$WORK/round.err" >/dev/null \
    || fail "a metadata-only update did not reach the attached client"
attach_round probe --session "$SID" --daemon "$DID" --wait-ms 300 || fail "activity reattach failed"
grep -F "activity context='$ACTIVITY' app='$ACTIVITY'" "$WORK/round.err" >/dev/null \
    || fail "context or program status was lost on reattach"

echo
echo "smoke: PASS — session persisted across disconnect; reattach resumed"
echo "smoke:        the same live PTY/Terminal (markers '$MARK1' then '$MARK2'),"
echo "smoke:        and the OSC 633;E command line rode a typed completion ('$CMDLINE')"
exit 0
