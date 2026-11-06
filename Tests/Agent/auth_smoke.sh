#!/usr/bin/env bash
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

#
# Tests/Agent/auth_smoke.sh — the ubq capability-token auth path.
#
# Starts an isolated ubiquitty-host daemon with UBIQUITTY_REQUIRE_TOKEN=1, mints a
# token via `ubiquitty-host token issue`, and drives the agent CLI with no / valid /
# bogus tokens — asserting the client presents the token and maps AUTH_DENIED to
# the right exit code (7) and the daemon's enforcement actually gates a session.
#
# Hermetic: a short temp $UBIQUITTY_HOME, so versioned daemon socket paths fit
# sockaddr_un and never touch a real daemon.
#
# Env: UBIQUITTY_HOST_BIN / UBQ_BIN (else under UBQ_BUILD_ROOT).
# Exit 0 iff: no-token → 7, valid → 0, bogus → 7.

set -uo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
# shellcheck source=Scripts/build-root.sh
. "$ROOT/Scripts/build-root.sh"
HOST="${UBIQUITTY_HOST_BIN:-$UBQ_BUILD_ROOT/host/ubiquitty-host}"
AGENT="${UBQ_BIN:-$UBQ_BUILD_ROOT/agent/ubq}"

fail() { echo "auth-smoke: FAIL: $*" >&2; exit 1; }

[ -x "$HOST" ]  || fail "ubiquitty-host not built at $HOST (run Scripts/build-host.sh)"
[ -x "$AGENT" ] || fail "ubq not built at $AGENT (run Scripts/build-agent.sh)"

WORK="$(mktemp -d "/tmp/ubiquitty-auth-smoke.XXXXXX")"
export UBIQUITTY_HOME="$WORK/.ubiquitty"
# The policy allows creating a session, so each case is about the token alone.
export UBIQUITTY_AGENT_POLICY="$WORK/policy.conf"
printf 'default allow\n' >"$UBIQUITTY_AGENT_POLICY"
mkdir -p "$UBIQUITTY_HOME"

cleanup() {
    for pidfile in "$UBIQUITTY_HOME"/host*.pid; do
        [ -f "$pidfile" ] || continue
        p="$(cat "$pidfile" 2>/dev/null || true)"
        if [ -n "${p:-}" ] && ps -p "$p" -o comm= 2>/dev/null | grep -q 'ubiquitty-host'; then
            kill "$p" 2>/dev/null || true
            sleep 0.2
            kill -9 "$p" 2>/dev/null || true
        fi
    done
    rm -rf "$WORK"
}
trap cleanup EXIT

# Issuing mints a live credential, so an argument the parser does not know
# refuses the whole request; `--help` asks for the usage and mints nothing.
echo "auth-smoke: token issue refuses an unknown argument and mints nothing"
"$HOST" token issue --scope observe --bogus >"$WORK/unknown.out" 2>"$WORK/unknown.err"
rc=$?
[ "$rc" -eq 2 ] || fail "an unknown argument did not exit 2 (got $rc)"
[ ! -s "$WORK/unknown.out" ] || fail "an unknown argument still printed a token"
grep -q 'nothing was issued' "$WORK/unknown.err" || fail "the refusal does not say nothing was issued"
"$HOST" token issue --help >"$WORK/help.out" 2>"$WORK/help.err"
rc=$?
[ "$rc" -eq 0 ] || fail "token issue --help did not exit 0 (got $rc)"
grep -q '^usage:' "$WORK/help.out" || fail "token issue --help printed no usage"
"$HOST" token issue --label >/dev/null 2>&1
rc=$?
[ "$rc" -eq 2 ] || fail "a bare --label did not exit 2 (got $rc)"
"$HOST" token list | grep -q 'no tokens issued' || fail "a refused issue left a token behind"

echo "auth-smoke: mint a token (scope all)"
TOK="$("$HOST" token issue --scope all --label auth-smoke 2>/dev/null | tail -1)"
[ -n "$TOK" ] || fail "token issue produced no token"
echo "auth-smoke: token ${TOK:0:12}…"

echo "auth-smoke: start daemon with UBIQUITTY_REQUIRE_TOKEN=1"
UBIQUITTY_REQUIRE_TOKEN=1 "$HOST" --ensure-daemon --columns 80 --rows 24 || fail "--ensure-daemon returned nonzero"
SOCK="$(find "$UBIQUITTY_HOME" -maxdepth 1 -type s -name 'host*.sock' -print -quit 2>/dev/null)"
[ -n "$SOCK" ] || fail "daemon socket not present after ensure-daemon"

# 1. No token → AUTH_DENIED(missing), exit 7.
echo "auth-smoke: no token is refused (exit 7)"
( unset UBIQUITTY_AGENT_TOKEN; "$AGENT" new ) >"$WORK/no.out" 2>"$WORK/no.err"
rc=$?
[ "$rc" -eq 7 ] || fail "no-token did not exit 7 (got $rc)"
grep -qi 'authentication required' "$WORK/no.err" || fail "no-token missing 'authentication required' message"

# 2. Valid token → attach succeeds, exit 0.
echo "auth-smoke: valid token attaches (exit 0)"
UBIQUITTY_AGENT_TOKEN="$TOK" "$AGENT" new >"$WORK/ok.out" 2>"$WORK/ok.err"
rc=$?
[ "$rc" -eq 0 ] || fail "valid-token did not exit 0 (got $rc): $(cat "$WORK/ok.err")"

# 3. Bogus token → AUTH_DENIED(invalid), exit 7.
echo "auth-smoke: bogus token is refused (exit 7)"
UBIQUITTY_AGENT_TOKEN="deadbeefdeadbeefdeadbeef" "$AGENT" new >"$WORK/bad.out" 2>"$WORK/bad.err"
rc=$?
[ "$rc" -eq 7 ] || fail "bogus-token did not exit 7 (got $rc)"
grep -qi 'not recognized' "$WORK/bad.err" || fail "bogus-token missing 'not recognized' message"

echo "auth-smoke: PASS — token auth gates the agent (no→7, valid→0, bogus→7)"
