#!/usr/bin/env bash
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

#
# Tests/Host/soak.sh — one daemon, a thousand clients: nothing kept, nothing leaked.
#
# A daemon outlives every client that attaches to it, so whatever an attach
# leaves behind is kept for the life of the host. The persistence smoke attaches
# a handful of times and cannot see that. This attaches and detaches the same
# session ROUNDS times through the production transport (`ubiquitty-host
# --attach`'s stdio, the smoke client on the other end), each client leaving
# with a normal Bye as a closing window does, and holds the daemon to two
# bounds:
#
#   - its resident size after the last round is within RSS_GROWTH_KIB of what
#     it was once the first rounds had warmed every cache an attach fills;
#   - when its last session ends it exits on its own, and its log carries no
#     AddressSanitizer, LeakSanitizer or UndefinedBehaviorSanitizer report.
#     The second bound means something only for a daemon built with
#     -DUBIQUITTY_SANITIZERS (Scripts/run-sanitized-tests.sh builds one); LSan
#     runs at a clean exit on Linux and has no runtime on macOS arm64.
#
# Hermetic: an isolated $UBIQUITTY_HOME under a temp dir, as the smoke has.
#
# Env:
#   UBIQUITTY_HOST_BIN            path to ubiquitty-host
#   UBIQUITTY_SMOKE_BUILD         Core CMake build directory for smoke_client
#   UBIQUITTY_SMOKE_TMPDIR        short temp root for Unix socket paths (default /tmp)
#   UBIQUITTY_SOAK_ROUNDS         attaches after the first (default 1000)
#   UBIQUITTY_SOAK_WARMUP         rounds before the first size sample (default 50)
#   UBIQUITTY_SOAK_RSS_GROWTH_KIB the most the daemon may grow (default 16384)

set -uo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
# shellcheck source=Scripts/build-root.sh
. "$ROOT/Scripts/build-root.sh"
HOST_BIN="${UBIQUITTY_HOST_BIN:-$UBQ_BUILD_ROOT/host/ubiquitty-host}"
OUT="${UBIQUITTY_SMOKE_BUILD:-$UBQ_BUILD_ROOT/host-smoke}"
CLIENT="$OUT/Tests/ubq-host-smoke-client"
ROUNDS="${UBIQUITTY_SOAK_ROUNDS:-1000}"
WARMUP="${UBIQUITTY_SOAK_WARMUP:-50}"
RSS_GROWTH_KIB="${UBIQUITTY_SOAK_RSS_GROWTH_KIB:-16384}"

fail() { echo "soak: FAIL: $*" >&2; exit 1; }

[ -x "$HOST_BIN" ] || fail "ubiquitty-host not built at $HOST_BIN (run Scripts/build-host.sh)"
[ "$WARMUP" -lt "$ROUNDS" ] || fail "UBIQUITTY_SOAK_WARMUP ($WARMUP) must be below UBIQUITTY_SOAK_ROUNDS ($ROUNDS)"

echo "soak: building smoke_client"
cmake -S "$ROOT" -B "$OUT" -G Ninja \
    -DUBIQUITTY_FRONTEND=Core -DUBIQUITTY_BUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Debug >/dev/null \
    || fail "smoke_client configuration failed"
cmake --build "$OUT" --target ubq-host-smoke-client -j "${UBQ_BUILD_JOBS:-4}" >/dev/null \
    || fail "smoke_client did not compile"

SOAK_TMPDIR="${UBIQUITTY_SMOKE_TMPDIR:-/tmp}"
WORK="$(mktemp -d "$SOAK_TMPDIR/ubiquitty-host-soak.XXXXXX")"
export UBIQUITTY_HOME="$WORK/.ubiquitty"

cleanup() {
    local f p
    for f in "$UBIQUITTY_HOME"/host*.pid; do
        [ -f "$f" ] || continue
        p="$(cat "$f" 2>/dev/null || true)"
        [ -n "${p:-}" ] && kill -9 "$p" 2>/dev/null || true
    done
    rm -rf "$WORK"
}
trap cleanup EXIT

# One attach, as smoke.sh's attach_round makes it: the relay's stdio is two
# FIFOs, opened in the order the client opens them.
attach_round() {
    local up="$WORK/up" down="$WORK/down" rc
    rm -f "$up" "$down"
    mkfifo "$up" "$down" || fail "mkfifo failed"
    "$HOST_BIN" --attach > "$down" < "$up" &
    local relay=$!
    "$CLIENT" "$@" < "$down" > "$up" 2> "$WORK/round.err"
    rc=$?
    wait "$relay" 2>/dev/null || true
    return $rc
}

# The daemon's resident size in KiB.
residentKiB() {
    if [ -r "/proc/$1/status" ]; then
        awk '/^VmRSS:/ { print $2 }' "/proc/$1/status"
    else
        ps -o rss= -p "$1" | tr -d ' '
    fi
}

"$HOST_BIN" --ensure-daemon --columns 80 --rows 24 || fail "--ensure-daemon returned nonzero"
PID_FILE="$(ls "$UBIQUITTY_HOME"/host*.pid 2>/dev/null | head -1)"
LOG_FILE="$(ls "$UBIQUITTY_HOME"/host*.log 2>/dev/null | head -1)"
DPID="$(cat "$PID_FILE" 2>/dev/null || true)"
[ -n "$DPID" ] && kill -0 "$DPID" 2>/dev/null || fail "no live daemon after ensure-daemon"

attach_round probe --wait-ms 200 || { cat "$WORK/round.err" >&2; fail "the first attach failed"; }
SID="$(grep -oE 'sessionID=[0-9]+' "$WORK/round.err" | head -1 | cut -d= -f2)"
DID="$(grep -oE 'daemonInstanceID=[0-9]+' "$WORK/round.err" | head -1 | cut -d= -f2)"
[ -n "$SID" ] && [ "$SID" != "0" ] && [ -n "$DID" ] || fail "the first attach reported no session"
echo "soak: daemon pid=$DPID session=$SID; $ROUNDS attaches"

started="$(date +%s)"
baseline=""
for round in $(seq 1 "$ROUNDS"); do
    attach_round probe --session "$SID" --daemon "$DID" --wait-ms 0 --detach --until-screen \
        || { cat "$WORK/round.err" >&2; fail "attach $round of $ROUNDS failed"; }
    kill -0 "$DPID" 2>/dev/null || fail "the daemon died at attach $round"
    [ "$round" = "$WARMUP" ] && baseline="$(residentKiB "$DPID")"
done
final="$(residentKiB "$DPID")"
[ -n "$baseline" ] && [ -n "$final" ] || fail "could not read the daemon's resident size"
growth=$((final - baseline))
echo "soak: $ROUNDS attaches in $(( $(date +%s) - started )) s; resident ${baseline} KiB after $WARMUP, ${final} KiB after $ROUNDS"
[ "$growth" -le "$RSS_GROWTH_KIB" ] \
    || fail "the daemon grew ${growth} KiB over $((ROUNDS - WARMUP)) attaches, past the ${RSS_GROWTH_KIB} KiB it may"

# Ending the one session ends the daemon, which is the exit a sanitizer reports at.
attach_round probe --session "$SID" --daemon "$DID" --input "exit
" --wait-ms 300 >/dev/null 2>&1 || true
for _ in $(seq 1 100); do
    kill -0 "$DPID" 2>/dev/null || break
    sleep 0.1
done
kill -0 "$DPID" 2>/dev/null && fail "the daemon did not exit after its last session ended"
if [ -n "$LOG_FILE" ] && grep -E 'ERROR: (AddressSanitizer|LeakSanitizer)|runtime error:' "$LOG_FILE" >&2; then
    fail "the daemon's log carries a sanitizer report ($LOG_FILE)"
fi

echo "soak: PASS — $ROUNDS attaches, ${growth} KiB of growth, a clean exit"
exit 0
