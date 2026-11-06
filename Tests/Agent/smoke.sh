#!/usr/bin/env bash
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

#
# Tests/Agent/smoke.sh — end-to-end check of the ubq Sync client.
#
# Starts an isolated ubiquitty-host daemon (private $UBIQUITTY_HOME) and runs
# `ubq selftest`, which attaches directly to the daemon socket (no
# `--attach` relay), runs a command, and reads its output back off the
# projected screen. Proves the headless Sync seam works end to end without
# the macOS app.
#
# Hermetic: a short temp $UBIQUITTY_HOME, so versioned daemon socket paths fit
# sockaddr_un and never touch a real daemon the developer is using.
#
# Env:
#   UBIQUITTY_HOST_BIN    path to the built ubiquitty-host  (else build/host/...)
#   UBQ_BIN   path to the built ubq (else under UBQ_BUILD_ROOT)
#
# Exit 0 iff the agent attached, the command ran, and its output reached the
# projected screen.

set -uo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
# shellcheck source=Scripts/build-root.sh
. "$ROOT/Scripts/build-root.sh"
HOST="${UBIQUITTY_HOST_BIN:-$UBQ_BUILD_ROOT/host/ubiquitty-host}"
AGENT="${UBQ_BIN:-$UBQ_BUILD_ROOT/agent/ubq}"

fail() { echo "agent-smoke: FAIL: $*" >&2; exit 1; }

# as_person <command...> — runs a command on a pseudo-terminal, as a person at
# a prompt does, so its --yes answers a confirm; prints what it printed and
# exits with its status. A process with no terminal cannot answer one.
as_person() {
    python3 "$ROOT/Tests/Agent/on_a_terminal.py" "$@" </dev/null | tr -d '\r'
    return "${PIPESTATUS[0]}"
}

[ -x "$HOST" ]  || fail "ubiquitty-host not built at $HOST (run Scripts/build-host.sh)"
[ -x "$AGENT" ] || fail "ubq not built at $AGENT (run Scripts/build-agent.sh)"

WORK="$(mktemp -d "/tmp/ubq-smoke.XXXXXX")"
export UBIQUITTY_HOME="$WORK/.ubiquitty"

cleanup() {
    [ -n "${RELAY_PID:-}" ] && kill "$RELAY_PID" 2>/dev/null
    for pidfile in "$UBIQUITTY_HOME"/host*.pid; do
        [ -f "$pidfile" ] || continue
        p="$(cat "$pidfile" 2>/dev/null || true)"
        if [ -n "${p:-}" ] && ps -p "$p" -o comm= 2>/dev/null | grep -q 'ubiquitty-host'; then
            kill -CONT "$p" 2>/dev/null || true
            kill "$p" 2>/dev/null || true
            sleep 0.2
            kill -9 "$p" 2>/dev/null || true
        fi
    done
    rm -rf "$WORK"
}
trap cleanup EXIT

echo "agent-smoke: ensure-daemon (UBIQUITTY_HOME=$UBIQUITTY_HOME)"
"$HOST" --ensure-daemon --columns 80 --rows 24 || fail "--ensure-daemon returned nonzero"
SOCK="$(find "$UBIQUITTY_HOME" -maxdepth 1 -type s -name 'host*.sock' -print -quit 2>/dev/null)"
[ -n "$SOCK" ] || fail "daemon socket not present after ensure-daemon"

# The daemon refuses a tokenless agent by default, so the smoke presents a
# token the way a user does: issued by the host, carrying every scope.
UBIQUITTY_AGENT_TOKEN="$("$HOST" token issue --scope all --label agent-smoke 2>/dev/null | tail -1)"
[ -n "$UBIQUITTY_AGENT_TOKEN" ] || fail "could not mint the smoke's agent token"
export UBIQUITTY_AGENT_TOKEN

# selftest and imagetest make a session and type into it, so the policy
# screens them as `new` and as `<selftest>`: a policy that denies them makes
# nothing, and one that allows them lets them run.
ALLOW_POLICY="$WORK/allow.conf"
printf 'default allow\n' >"$ALLOW_POLICY"
printf 'default allow\ndeny <selftest>\ndeny <imagetest>\n' >"$WORK/deny-tests.conf"
for test in selftest imagetest; do
    UBIQUITTY_AGENT_POLICY="$WORK/deny-tests.conf" "$AGENT" "$test" >"$WORK/denied.out" 2>"$WORK/denied.err"
    rc=$?
    [ "$rc" -eq 4 ] || fail "$test under a policy that denies it exited $rc, not 4: $(cat "$WORK/denied.err")"
    grep -q "blocked by policy (deny): <$test>" "$WORK/denied.err" || fail "$test was not refused by its deny rule: $(cat "$WORK/denied.err")"
done
[ -z "$(UBIQUITTY_AGENT_POLICY="$ALLOW_POLICY" "$AGENT" ls)" ] || fail "a denied self-test made a session"
UBIQUITTY_AGENT_POLICY="$WORK/no-such-policy.conf" "$AGENT" selftest </dev/null >/dev/null 2>&1
[ $? -eq 4 ] || fail "selftest under the default confirm ran without a person's --yes"
echo "agent-smoke: running ubq selftest"
SELFTEST_ERR="$(UBIQUITTY_AGENT_POLICY="$ALLOW_POLICY" "$AGENT" selftest 2>&1)" || fail "selftest failed: $SELFTEST_ERR"
echo "$SELFTEST_ERR" | grep -q 'marker=FOUND' || fail "selftest marker not found"
# The echo command's screen changes ride Sync updates atop the first
# checkpoint — at least one must have applied.
UPDATES="$(printf '%s\n' "$SELFTEST_ERR" | grep -oE 'updates=[0-9]+' | grep -oE '[0-9]+' | head -1)"
{ [ -n "$UPDATES" ] && [ "$UPDATES" -gt 0 ]; } || fail "selftest applied no screen-plane updates (updates=${UPDATES:-?})"
echo "agent-smoke: selftest OK (screen-plane updates applied: updates=$UPDATES)"

# --- typing + policy gate + audit log --------------------------------------
# The marker is assembled by command substitution so it appears only in the
# OUTPUT, never in the echoed command line.
echo "agent-smoke: send types a command line (proven by reading the screen)"
S="$(as_person "$AGENT" --yes new 2>/dev/null)"
[ -n "$S" ] || fail "new returned no session id"
as_person "$AGENT" --yes send -s "$S" --enter 'echo RUNMARK_$(echo ok)' >/dev/null 2>/dev/null \
    || fail "send --yes (allow path) exited nonzero"
# A ceiling, not a delay: the wait returns as soon as the text is on screen,
# and a login shell's startup files can take seconds on a loaded machine.
"$AGENT" wait -s "$S" --for text:RUNMARK_ok --timeout 30 >/dev/null 2>"$WORK/wait.err" \
    || fail "the typed command's output never reached the screen: $(cat "$WORK/wait.err")"
"$AGENT" read -s "$S" --screen 2>/dev/null | grep -q 'RUNMARK_ok' \
    || fail "read --screen does not show the command's output"

# --- a write the daemon never confirms is a failure, not a success ----------
# The relay stops the daemon before forwarding marked text. The sender can
# hand over the input, but no write acknowledgement can return: send must
# report the unconfirmed write and exit 9.
echo "agent-smoke: send fails when the daemon never confirms writing the text"
python3 -B "$ROOT/Tests/Agent/test_unconfirmed_send_relay.py" \
    || fail "the unconfirmed-write relay checks failed"
DAEMON_PID="$(cat "$UBIQUITTY_HOME"/host*.pid)"
# A client holds a daemon to the socket it reached it at: the Welcome names the
# socket the daemon serves, and a relay at any other name is refused. So the
# relay takes the daemon's own path, and the daemon, whose listener survives a
# rename, is reached at a path beside it.
RELAY="$SOCK"
DAEMON_SOCK="$SOCK.relayed"
mv "$SOCK" "$DAEMON_SOCK" || fail "the daemon's socket could not be moved aside for the relay"
python3 "$ROOT/Tests/Agent/unconfirmed_send_relay.py" "$RELAY" "$DAEMON_SOCK" "$DAEMON_PID" UNCONFIRMED_MARK "$WORK/frozen" "$WORK/relay.ready" 2>"$WORK/relay.err" &
RELAY_PID=$!
for _ in $(seq 1 600); do [ -e "$WORK/relay.ready" ] && break; sleep 0.05; done
[ -e "$WORK/relay.ready" ] || fail "the relay did not listen"
SENT_AT=$SECONDS
as_person "$AGENT" --yes send -s "$S" --enter 'echo UNCONFIRMED_MARK' >"$WORK/unconfirmed.out" 2>&1
rc=$?
SEND_SECONDS=$((SECONDS - SENT_AT))
kill "$RELAY_PID" 2>/dev/null
wait "$RELAY_PID" 2>/dev/null
kill -CONT "$DAEMON_PID" 2>/dev/null
rm -f "$RELAY"
mv "$DAEMON_SOCK" "$SOCK" || fail "the daemon's socket could not be put back"
[ -e "$WORK/frozen" ] || fail "the relay never saw the text, so the daemon was never stopped (send exited $rc after ${SEND_SECONDS}s): $(cat "$WORK/unconfirmed.out") $(cat "$WORK/relay.err" 2>/dev/null)"
[ "$rc" -eq 9 ] || fail "send exited $rc, not 9, when the daemon never confirmed writing the text: $(cat "$WORK/unconfirmed.out")"
grep -q 'did not confirm writing the text' "$WORK/unconfirmed.out" || fail "send's refusal does not say the write went unconfirmed: $(cat "$WORK/unconfirmed.out")"
"$AGENT" wait -s "$S" --for idle --timeout 30 >/dev/null 2>"$WORK/wait.err" || fail "the daemon did not answer after it resumed: $(cat "$WORK/wait.err")"

echo "agent-smoke: a 'confirm' command without --yes is refused"
"$AGENT" run -s "$S" -- 'echo nope' >/dev/null 2>/dev/null
[ $? -eq 4 ] || fail "confirm default did NOT block an unapproved run with exit 4"
echo "agent-smoke: a 'confirm' command with --yes from no terminal is refused"
"$AGENT" --yes run -s "$S" -- 'echo nope' </dev/null >/dev/null 2>/dev/null
[ $? -eq 4 ] || fail "--yes from a process with no terminal approved a confirm"

echo "agent-smoke: policy deny blocks even with --yes"
export UBIQUITTY_AGENT_POLICY="$WORK/policy.conf"
printf 'deny rm\ndefault allow\n' > "$UBIQUITTY_AGENT_POLICY"
if as_person "$AGENT" --yes run -s "$S" -- 'rm -rf /nonexistent_xyz' >"$WORK/deny.err" 2>&1; then
    fail "policy: 'rm' was NOT blocked"
fi
grep -q 'blocked by policy' "$WORK/deny.err" || fail "policy: deny gave no policy message"
# Under 'default allow', a non-denied line is typed without --yes.
"$AGENT" send -s "$S" --enter 'echo allowed_marker_X1' >/dev/null 2>/dev/null \
    || fail "default-allow send failed"
unset UBIQUITTY_AGENT_POLICY

# The audit log recorded the gated events.
[ -s "$UBIQUITTY_HOME/agent-audit.log" ] || fail "audit log empty"
grep -q 'deny: rm' "$UBIQUITTY_HOME/agent-audit.log" || fail "audit log missing deny record"
echo "agent-smoke: send + policy + audit OK"

# --- command events + region reads + control ---------------------------------
# A precise exit code and last-command output need OSC 133 marks; the test
# command emits them itself, so this verifies the consumers regardless of the
# login shell's own configuration.
echo "agent-smoke: wait --for idle (already at prompt)"
WAITED="$("$AGENT" wait -s "$S" --for idle --timeout 30 2>&1)"
echo "$WAITED" | grep -q 'idle' || fail "wait --for idle did not report idle: $WAITED"

echo "agent-smoke: run propagates the OSC 133 exit code"
as_person "$AGENT" --yes run -s "$S" -- 'printf "\033]133;C\007"; echo SMOKE_RUN_OK; printf "\033]133;D;5\007"' >"$WORK/run.out" 2>/dev/null
rc=$?
[ "$rc" -eq 5 ] || fail "run did not propagate exit 5 (got $rc)"
grep -q 'SMOKE_RUN_OK' "$WORK/run.out" || fail "run output missing marker"

echo "agent-smoke: read --last-command returns the last command output"
"$AGENT" read -s "$S" --last-command 2>/dev/null | grep -q 'SMOKE_RUN_OK' || fail "read --last-command missing last-command output"

echo "agent-smoke: ls lists the session, kill terminates one"
"$AGENT" ls 2>/dev/null | cut -f1 | grep -qx "$S" || fail "ls did not list session $S"
# ls is a control-only client: it must not spawn a throwaway session per call.
N1="$("$AGENT" ls 2>/dev/null | grep -c .)"
N2="$("$AGENT" ls 2>/dev/null | grep -c .)"
[ "$N1" = "$N2" ] || fail "ls grew the session count ($N1 -> $N2): control-only HELLO broken"
KS="$(as_person "$AGENT" --yes new 2>/dev/null)"
as_person "$AGENT" --yes kill -s "$KS" >/dev/null || fail "kill did not terminate"
"$AGENT" ls 2>/dev/null | cut -f1 | grep -qx "$KS" && fail "a killed session is still listed"
echo "agent-smoke: command-events + region + control OK"

# --- inline-image Sync: pixels travel as IMAGE frames -----------------------
# The client (a passive Sync client with no VT parser) makes the session's
# shell emit a graphics escape; the daemon's Core parses and stores the image
# and serializes it as IMAGE / IMAGE_PLACEMENTS frames. imagetest waits for a
# placement to land in its mirrored Terminal, renders a screenshot, and
# asserts the image's red pixels appear in the PNG.
echo "agent-smoke: inline-image IMAGE frames reach the screenshot"
IMG_ERR="$(UBIQUITTY_AGENT_POLICY="$ALLOW_POLICY" "$AGENT" imagetest 2>&1)" \
    || fail "imagetest failed: $IMG_ERR"
echo "$IMG_ERR" | grep -q 'IMAGETEST PASS' || fail "imagetest did not pass: $IMG_ERR"
echo "agent-smoke: inline-image OK ($(echo "$IMG_ERR" | grep -o 'redPixels=[0-9]*' | head -1))"

echo "agent-smoke: PASS"
exit 0
