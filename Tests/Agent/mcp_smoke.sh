#!/usr/bin/env bash
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

#
# Tests/Agent/mcp_smoke.sh — end-to-end check of `ubq mcp`.
#
# Drives the MCP server with real JSON-RPC over stdin and checks the
# handshake, the tool catalog, a tool call that actually runs a command
# (proven via a terminal_screen reattach, since run capture is best-effort),
# and policy enforcement through the MCP path. Hermetic: a short private
# $UBIQUITTY_HOME so versioned daemon socket paths fit sockaddr_un and never
# touch a real daemon.

set -uo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
# shellcheck source=Scripts/build-root.sh
. "$ROOT/Scripts/build-root.sh"
HOST="${UBIQUITTY_HOST_BIN:-$UBQ_BUILD_ROOT/host/ubiquitty-host}"
AGENT="${UBQ_BIN:-$UBQ_BUILD_ROOT/agent/ubq}"

fail() { echo "mcp-smoke: FAIL: $*" >&2; exit 1; }

[ -x "$HOST" ]  || fail "ubiquitty-host not built at $HOST (run Scripts/build-host.sh)"
[ -x "$AGENT" ] || fail "ubq not built at $AGENT (run Scripts/build-agent.sh)"

python3 "$ROOT/Tests/Agent/mcp_tree_list.py" "$AGENT" || fail "Sync tree listing fixture"
python3 "$ROOT/Tests/Agent/mcp_client_label.py" "$AGENT" || fail "the client's initialize name does not label its attach"
python3 "$ROOT/Tests/Agent/mcp_first_screen.py" "$AGENT" || fail "first-screen readiness"
python3 "$ROOT/Tests/Agent/mcp_run_timing.py" "$AGENT" "$HOST" || fail "run without shell integration"

WORK="$(mktemp -d "/tmp/ubiquitty-mcp-smoke.XXXXXX")"
export UBIQUITTY_HOME="$WORK/.ubiquitty"
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

echo "mcp-smoke: ensure-daemon (UBIQUITTY_HOME=$UBIQUITTY_HOME)"
# This smoke exercises the MCP AGENT-POLICY layer (gateAndAudit), not the daemon's
# capability-token gate — token enforcement is covered by token_auth. The agent
# is a non-Local peer, and a non-Local peer is held to a real token's scope
# ceiling whether or not tokens are required: turning enforcement off relaxes
# "a token is needed", not "this token may do that". So the smoke mints one
# carrying every scope, and the policy-layer assertions below are
# then about the policy rather than about authorization.
"$HOST" --ensure-daemon --columns 80 --rows 24 || fail "--ensure-daemon returned nonzero"
SOCK="$(find "$UBIQUITTY_HOME" -maxdepth 1 -type s -name 'host*.sock' -print -quit 2>/dev/null)"
[ -n "$SOCK" ] || fail "daemon socket not present"

UBIQUITTY_AGENT_TOKEN="$("$HOST" token issue --scope all --label smoke-agent 2>/dev/null | tail -1)"
[ -n "$UBIQUITTY_AGENT_TOKEN" ] || fail "could not mint the smoke's agent token"
export UBIQUITTY_AGENT_TOKEN

# One JSON-RPC request per server invocation: pipe it in, the server replies,
# stdin EOF exits it. (The read loop also handles many requests per pipe — see
# the multi-request check below.)
mcp() { printf '%s\n' "$1" | "$AGENT" mcp 2>/dev/null; }

# mcp_policy: run one MCP call with an ephemeral policy. The policy env var
# must reach the agent process (right side of pipe), not just printf.
mcp_policy() {
    local policy_content="$1"
    local json="$2"
    local pfile="$WORK/tmp-policy-$$.conf"
    printf '%s\n' "$policy_content" > "$pfile"
    printf '%s\n' "$json" | UBIQUITTY_AGENT_POLICY="$pfile" "$AGENT" mcp 2>/dev/null
    local rc=$?
    rm -f "$pfile"
    return $rc
}

# 0. A version this server does not implement is answered with the one it does.
# initialize is where a client learns which dialect is in force; repeating the
# requested string agrees to something this server cannot speak, and neither
# side can tell afterwards.
echo "mcp-smoke: initialize answers an unknown version with its own"
OUT="$(printf '%s\n' \
  '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2099-01-01","capabilities":{},"clientInfo":{"name":"smoke","version":"0"}}}' \
  | "$AGENT" mcp 2>/dev/null)"
echo "$OUT" | grep -q '"2024-11-05"' || fail "initialize: an unknown version was not answered with the server's ($OUT)"
if echo "$OUT" | grep -q '2099-01-01'; then
  fail "initialize: the server echoed a version it does not implement ($OUT)"
fi

# 1. Handshake + tool catalog in one invocation (exercises the streaming loop).
echo "mcp-smoke: initialize + tools/list (two requests, one pipe)"
OUT="$(printf '%s\n%s\n' \
  '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"smoke","version":"0"}}}' \
  '{"jsonrpc":"2.0","id":2,"method":"tools/list"}' \
  | "$AGENT" mcp 2>/dev/null)"
echo "$OUT" | grep -q '"serverInfo"'   || fail "initialize: no serverInfo ($OUT)"
echo "$OUT" | grep -q 'ubq'  || fail "initialize: no server name"
echo "$OUT" | grep -q '"id":1'         || fail "initialize: response id mismatch"
echo "$OUT" | grep -q '"2024-11-05"' || fail "initialize: the agreed version is not in the answer ($OUT)"
echo "$OUT" | grep -q 'terminal_run'   || fail "tools/list: terminal_run missing"
echo "$OUT" | grep -q 'terminal_screen' || fail "tools/list: terminal_screen missing"
# Verify approve property is absent from all tool schemas (never a self-approve flag).
# grep -vq is vacuous under BSD grep (exits 0 if any line lacks the pattern).
# Instead: assert that 'approve' does not appear anywhere in the tools/list output.
# Verify-by-revert: re-adding the approve property to a schema makes this FAIL.
! echo "$OUT" | grep -q '"approve"' || fail "tools/list: 'approve' property still present in schema"
[ "$(printf '%s\n' "$OUT" | grep -c '"jsonrpc"')" -eq 2 ] || fail "expected 2 responses on one pipe"

# 1b. The request parameter object is `params` alone: a call spelling it
# `parameters` names no tool.
echo "mcp-smoke: a parameters object under any key but params is not read"
R="$(mcp '{"jsonrpc":"2.0","id":7,"method":"tools/call","parameters":{"name":"terminal_list","arguments":{}}}')"
echo "$R" | grep -q 'missing tool name' || fail "tools/call: a 'parameters' object was read as the request's params ($R)"

# 2. terminal_new is gated — without a permissive policy the Confirm default blocks it.
#    This assertion would FAIL without the gateAndAudit call in terminal_new
#    (verify-by-revert: removing the gate makes terminal_new return a session
#    id here instead of "requires approval").
echo "mcp-smoke: terminal_new gate — no policy → requires approval"
R="$(mcp '{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"terminal_new","arguments":{}}}')"
echo "$R" | grep -q 'requires approval' || fail "terminal_new without allow policy was NOT gated ($R)"
grep -qE 'new .*x' "$UBIQUITTY_HOME/agent-audit.log" || fail "terminal_new was not audited"

# 2b. terminal_new with a deny policy must be blocked outright (no daemon needed).
#     The 'approve' arg is silently ignored by the server — deny always wins.
echo "mcp-smoke: terminal_new deny policy blocks creation (no agent action bypasses the gate)"
export UBIQUITTY_AGENT_POLICY="$WORK/policy-deny-all.conf"
printf 'default deny\n' > "$UBIQUITTY_AGENT_POLICY"
R="$(mcp '{"jsonrpc":"2.0","id":18,"method":"tools/call","params":{"name":"terminal_new","arguments":{}}}')"
echo "$R" | grep -q '"isError":true'    || fail "terminal_new deny-policy: result not flagged isError ($R)"
echo "$R" | grep -q 'blocked by policy' || fail "terminal_new deny-policy: no block message ($R)"
unset UBIQUITTY_AGENT_POLICY
echo "mcp-smoke: terminal_new gate+audit OK"

# 2c. terminal_new with 'default allow' policy succeeds and creates a session.
#     The human configures allow out-of-band; the agent does not pass approve:true.
echo "mcp-smoke: terminal_new with default-allow policy → session id"
R="$(mcp_policy 'default allow' '{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"terminal_new","arguments":{}}}')"
SID="$(printf '%s' "$R" | grep -oE 'session [0-9]+' | grep -oE '[0-9]+' | head -1)"
[ -n "$SID" ] || fail "terminal_new (default allow) returned no session id ($R)"
echo "mcp-smoke: session id = $SID"

# 2d. Under `default allow` a pipe or a chain is still held for a person, and
#     the refusal says so rather than asking for the `default allow` already
#     set. The gate answers before any attach, so the session need not exist.
echo "mcp-smoke: default allow holds a pipe, and the refusal names the pipe"
R="$(mcp_policy 'default allow' '{"jsonrpc":"2.0","id":31,"method":"tools/call","params":{"name":"terminal_run","arguments":{"session":'"$SID"',"command":"echo PIPE_HELD | cat"}}}')"
echo "$R" | grep -q '"isError":true' || fail "terminal_run with a pipe under default allow was not refused ($R)"
echo "$R" | grep -q 'pipes, chains' || fail "the refusal of a pipe under default allow does not name the pipe ($R)"
! echo "$R" | grep -q "set 'default allow'" || fail "the refusal asks for the default allow that is already set ($R)"

# --- read-only verbs must not implicitly create a session ---------------------
# terminal_screen / terminal_wait with no session id would send HELLO
# sessionID=0 → the daemon CREATEs → an unaudited login-shell spawn, letting an
# agent under a `deny new` policy spawn shells via a read tool. They must
# require an existing id (creation only via the gated terminal_new).
# Verify-by-revert: removing the `session == 0` guard makes these return screen
# text (having spawned a shell) instead of the error.
echo "mcp-smoke: read-only verbs never implicitly create a session"
# The registry's schema requires the id, so the refusal is a -32602 error naming
# the parameter, answered before any attach could create a session.
R="$(mcp '{"jsonrpc":"2.0","id":1366,"method":"tools/call","params":{"name":"terminal_screen","arguments":{}}}')"
echo "$R" | grep -q "'session' is required" || fail "terminal_screen with no session did not require an id ($R)"
echo "$R" | grep -q '"code":-32602'          || fail "terminal_screen no-session not refused as a parameter error ($R)"
echo "$R" | grep -q '"parameter":"/session"' || fail "the refusal does not name the parameter ($R)"
R="$(mcp '{"jsonrpc":"2.0","id":1367,"method":"tools/call","params":{"name":"terminal_wait","arguments":{"substring":"x"}}}')"
echo "$R" | grep -q "'session' is required" || fail "terminal_wait with no session did not require an id ($R)"
R="$(mcp '{"jsonrpc":"2.0","id":1369,"method":"tools/call","params":{"name":"terminal_screenshot","arguments":{}}}')"
echo "$R" | grep -q "'session' is required" || fail "terminal_screenshot with no session did not require an id ($R)"
echo "mcp-smoke: OK — read-only verbs (screen/wait/screenshot) require an existing session"

# The screenshot file-write is audited (code-verified in AgentMCP.cpp). Its
# smoke assertion needs a live rendered session, which this smoke cannot
# reliably create: the session/reattach red stops it at the id:5 terminal_run
# check.

# --- negative: approve:true in caller args must not self-approve -----------------
# Under the default Confirm policy, an MCP caller setting approve:true in its
# tool args must be blocked. The agent cannot self-approve; only a human-
# configured allow rule or 'default allow' opens the gate.
#
# Verify-by-revert: re-adding the approve→AutoApprove mapping in dispatchTool
# makes this test FAIL (terminal_run succeeds instead of being blocked).
echo "mcp-smoke: negative — approve:true in MCP args MUST NOT self-approve"
R="$(mcp "{\"jsonrpc\":\"2.0\",\"id\":937,\"method\":\"tools/call\",\"params\":{\"name\":\"terminal_run\",\"arguments\":{\"session\":$SID,\"approve\":true,\"command\":\"echo SELFAPPROVE_SHOULD_BLOCK\"}}}")"
echo "$R" | grep -q "'approve' is not a parameter" || fail "terminal_run with approve:true was NOT refused — agent self-approval reached the gate ($R)"
echo "$R" | grep -q '"code":-32602'    || fail "terminal_run with approve:true not refused as a parameter error ($R)"
! echo "$R" | grep -q 'SELFAPPROVE_SHOULD_BLOCK' || fail "terminal_run with approve:true executed — self-approval bypass ($R)"
echo "mcp-smoke: negative OK — approve:true in MCP args is correctly blocked"

# negative (send): approve:true in terminal_send args must not self-approve.
# Under the default Confirm policy, terminal_send with approve:true must be blocked.
# Verify-by-revert: restoring approve→AutoApprove in the terminal_send dispatch
# branch makes terminal_send succeed here instead of being blocked.
echo "mcp-smoke: negative (send) — approve:true in terminal_send args MUST NOT self-approve"
R="$(mcp "{\"jsonrpc\":\"2.0\",\"id\":938,\"method\":\"tools/call\",\"params\":{\"name\":\"terminal_send\",\"arguments\":{\"session\":$SID,\"approve\":true,\"text\":\"echo SEND_SELFAPPROVE_SHOULD_BLOCK\n\"}}}")"
echo "$R" | grep -q "'approve' is not a parameter" || fail "terminal_send with approve:true was NOT refused — agent self-approval reached the gate ($R)"
echo "$R" | grep -q '"code":-32602'    || fail "terminal_send with approve:true not refused as a parameter error ($R)"
! echo "$R" | grep -q 'SEND_SELFAPPROVE_SHOULD_BLOCK' || fail "terminal_send with approve:true executed — self-approval bypass ($R)"
echo "mcp-smoke: negative (send) OK — approve:true in terminal_send args is correctly blocked"

# negative: a gated verb that PASSES policy but then FAILS to attach must
# record a matching attach-incomplete — the bare "allow" must not stand alone in
# the trail for an action that never ran (gateAndAudit logs allow before
# attachSession runs). Force the attach failure with an empty UBIQUITTY_HOME
# (no daemon socket → connect() fails "no daemon"), a default-allow policy, and
# a private audit log; assert both the allow and the attach-incomplete land.
# Verify-by-revert: swapping attachGated back to attachSession in the terminal_run
# dispatch branch drops the second record and this test FAILS.
echo "mcp-smoke: negative — policy-allow + missing daemon records attach-incomplete"
NODAEMON_856="$WORK/nodaemon-856"; mkdir -p "$NODAEMON_856"
PFILE_856="$WORK/allow-856.conf"; printf 'default allow\n' > "$PFILE_856"
# env -u UBIQUITTY_HOST_BIN: without it the agent would auto-spawn a daemon on
# demand and the attach would succeed — we need the attach to genuinely fail.
printf '%s\n' '{"jsonrpc":"2.0","id":856,"method":"tools/call","params":{"name":"terminal_run","arguments":{"session":0,"command":"echo NEVER_RUNS_856"}}}' \
  | env -u UBIQUITTY_HOST_BIN UBIQUITTY_HOME="$NODAEMON_856" UBIQUITTY_AGENT_POLICY="$PFILE_856" "$AGENT" mcp 2>/dev/null >/dev/null
AUDIT_856="$NODAEMON_856/agent-audit.log"
grep -qE 'run allow: echo NEVER_RUNS_856' "$AUDIT_856" \
  || fail "policy-allow record missing ($AUDIT_856)"
grep -qE 'run incomplete: \[attach-incomplete\] echo NEVER_RUNS_856' "$AUDIT_856" \
  || fail "attach-incomplete record missing — allow stands alone for an action that never ran"
if grep -qE 'run deny:|\[attach-denied\]' "$AUDIT_856"; then
  fail "missing daemon was audited as a refusal"
fi
rm -f "$PFILE_856"
echo "mcp-smoke: negative OK — attach failure after allow is reconciled in the audit trail"
# -------------------------------------------------------------------------------

# 3. terminal_run on that session, then prove execution via terminal_screen.
#    The marker is assembled by printf so it appears only in OUTPUT, and the
#    command carries no shell indirection: a substitution would downgrade the
#    policy's allow to confirm, and the gate would refuse the very command that
#    proves the gate passes. Use 'default allow' (the legitimate human-configured path).
echo "mcp-smoke: tools/call terminal_run (default allow policy)"
mcp_policy 'default allow' "{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"tools/call\",\"params\":{\"name\":\"terminal_run\",\"arguments\":{\"session\":$SID,\"command\":\"printf MCPMARK_%s ok\"}}}" >/dev/null
sleep 0.8
R="$(mcp "{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"tools/call\",\"params\":{\"name\":\"terminal_screen\",\"arguments\":{\"session\":$SID}}}")"
echo "$R" | grep -q 'MCPMARK_ok' || fail "terminal_run did not execute (screen lacks marker): $R"

# 4. Policy deny enforced through the MCP path.
echo "mcp-smoke: policy deny via terminal_run"
export UBIQUITTY_AGENT_POLICY="$WORK/policy.conf"
printf 'deny rm\ndefault allow\n' > "$UBIQUITTY_AGENT_POLICY"
R="$(mcp "{\"jsonrpc\":\"2.0\",\"id\":6,\"method\":\"tools/call\",\"params\":{\"name\":\"terminal_run\",\"arguments\":{\"session\":$SID,\"command\":\"rm -rf /nonexistent_xyz\"}}}")"
echo "$R" | grep -q '"isError":true'    || fail "policy deny: result not flagged isError ($R)"
echo "$R" | grep -q 'blocked by policy' || fail "policy deny: no policy message ($R)"
unset UBIQUITTY_AGENT_POLICY

# 4b. terminal_changes: two calls in one process — first is the baseline
#     (full screen, has the marker), second reports no change since.
echo "mcp-smoke: tools/call terminal_changes (baseline then no-change)"
OUT="$(printf '%s\n%s\n' \
  "{\"jsonrpc\":\"2.0\",\"id\":8,\"method\":\"tools/call\",\"params\":{\"name\":\"terminal_changes\",\"arguments\":{\"session\":$SID}}}" \
  "{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"tools/call\",\"params\":{\"name\":\"terminal_changes\",\"arguments\":{\"session\":$SID}}}" \
  | "$AGENT" mcp 2>/dev/null)"
echo "$OUT" | grep -q 'MCPMARK_ok'   || fail "terminal_changes baseline missing screen content ($OUT)"
echo "$OUT" | grep -q '(no change)'  || fail "terminal_changes second call should report no change ($OUT)"

# 5. Unknown method → JSON-RPC error.
echo "mcp-smoke: unknown method → error"
R="$(mcp '{"jsonrpc":"2.0","id":7,"method":"no/such/method"}')"
echo "$R" | grep -q '"error"' || fail "unknown method did not return an error ($R)"

# 6. New agent-frame tools: catalog + list + precise exit + read_last.
echo "mcp-smoke: new tools present (list/kill/read_last/wait_idle)"
LIST="$(printf '%s\n' '{"jsonrpc":"2.0","id":10,"method":"tools/list"}' | "$AGENT" mcp 2>/dev/null)"
for t in terminal_list terminal_kill terminal_read_last terminal_wait_idle terminal_screenshot; do
    echo "$LIST" | grep -q "$t" || fail "tools/list missing $t"
done

echo "mcp-smoke: terminal_list"
R="$(mcp '{"jsonrpc":"2.0","id":11,"method":"tools/call","params":{"name":"terminal_list","arguments":{}}}')"
echo "$R" | grep -q '"text"' || fail "terminal_list returned no text ($R)"

echo "mcp-smoke: terminal_run precise exit (OSC-133 D;3) + terminal_read_last"
R="$(mcp_policy 'default allow' "{\"jsonrpc\":\"2.0\",\"id\":12,\"method\":\"tools/call\",\"params\":{\"name\":\"terminal_run\",\"arguments\":{\"session\":$SID,\"command\":\"printf '\\\\033]133\\\\073C\\\\007MCPLAST_OK\\\\n\\\\033]133\\\\073D\\\\0733\\\\007'\"}}}")"
echo "$R" | grep -q 'MCPLAST_OK' || fail "terminal_run output missing marker ($R)"
echo "$R" | grep -q '\[exit=3\]' || fail "terminal_run missing exit code ($R)"
R="$(mcp "{\"jsonrpc\":\"2.0\",\"id\":13,\"method\":\"tools/call\",\"params\":{\"name\":\"terminal_read_last\",\"arguments\":{\"session\":$SID}}}")"
echo "$R" | grep -q 'MCPLAST_OK' || fail "terminal_read_last missing output ($R)"

# A wait is measured against the clock, not against how many frames arrived:
# a session that prints a line every few milliseconds wakes the wait hundreds
# of times before the substring shows, and a wait that counted each wake as a
# tenth of a second gave up after about three seconds of it. The session stays
# busy until the wait has run for longer than that, and only then prints the
# marker, so how fast a loaded machine runs the script decides nothing. The
# printing runs as a script so the line the policy reads carries no shell
# indirection, and the marker is printed from parts so the typed line does not
# show it.
echo "mcp-smoke: terminal_wait on a busy session lasts its whole timeout"
cat >"$WORK/busy.sh" <<'EOF'
i=0
while [ ! -e "$1" ]; do echo tick$i; sleep 0.01; i=$((i+1)); done
printf 'BUSY_%s\n' DONE
EOF
BUSY_STOP="$WORK/busy.stop"
rm -f "$BUSY_STOP"
mcp_policy 'default allow' "{\"jsonrpc\":\"2.0\",\"id\":40,\"method\":\"tools/call\",\"params\":{\"name\":\"terminal_run\",\"arguments\":{\"session\":$SID,\"command\":\"sh $WORK/busy.sh $BUSY_STOP\",\"timeout_ms\":120000}}}" >"$WORK/busy-run.out" &
RUNNER=$!
# The wait under test starts once the session is busy: a wait issued before
# the script runs would measure how long the runner took to start.
BUSY=""
for _ in $(seq 1 240); do
  SCREEN="$(mcp "{\"jsonrpc\":\"2.0\",\"id\":39,\"method\":\"tools/call\",\"params\":{\"name\":\"terminal_screen\",\"arguments\":{\"session\":$SID}}}")"
  if printf '%s' "$SCREEN" | grep -q 'tick[0-9]'; then BUSY=1; break; fi
  kill -0 "$RUNNER" 2>/dev/null || break
  sleep 0.25
done
if [ -z "$BUSY" ]; then
  touch "$BUSY_STOP"
  wait "$RUNNER"
  fail "the busy script never started ($(cat "$WORK/busy-run.out"))"
fi
mcp "{\"jsonrpc\":\"2.0\",\"id\":41,\"method\":\"tools/call\",\"params\":{\"name\":\"terminal_wait\",\"arguments\":{\"session\":$SID,\"substring\":\"BUSY_DONE\",\"timeout_ms\":30000}}}" >"$WORK/busy-wait.out" &
WAITER=$!
sleep 5
touch "$BUSY_STOP"
wait "$WAITER"
wait "$RUNNER"
R="$(cat "$WORK/busy-wait.out")"
echo "$R" | grep -q 'FOUND' || fail "terminal_wait gave up on a busy session before its 30 s timeout ($R; runner: $(cat "$WORK/busy-run.out"))"

echo "mcp-smoke: terminal_screenshot returns an image block"
R="$(mcp "{\"jsonrpc\":\"2.0\",\"id\":14,\"method\":\"tools/call\",\"params\":{\"name\":\"terminal_screenshot\",\"arguments\":{\"session\":$SID}}}")"
echo "$R" | grep -q '"type":"image"' || fail "terminal_screenshot returned no image block"
echo "$R" | grep -q '"mimeType":"image/png"' || fail "terminal_screenshot wrong mimeType"

# 6b. terminal_resize is gated + audited — a state-changing tool (SIGWINCH+reflow)
#     must not bypass the gate-and-audit invariant the other tools hold. Without
#     an allow policy the default Confirm blocks it; with 'default allow' it passes.
#     Either way the resize is audited.
#     Note: there is no in-band 'approve' flag; the MCP path is always
#     RequireConfirmation. A human-configured allow policy → evaluate() returns
#     Allow, so the audit records 'allow: 100x40' (no [approved] prefix, since
#     the [approved] prefix only appears on the Confirm+AutoApprove path which is
#     CLI-only via --yes).
echo "mcp-smoke: terminal_resize is gated + audited (no agent action bypasses the gate)"
R="$(mcp "{\"jsonrpc\":\"2.0\",\"id\":16,\"method\":\"tools/call\",\"params\":{\"name\":\"terminal_resize\",\"arguments\":{\"session\":$SID,\"columns\":100,\"rows\":40}}}")"
echo "$R" | grep -q 'requires approval' || fail "resize without allow policy was NOT gated ($R)"
grep -qE 'resize .*100x40' "$UBIQUITTY_HOME/agent-audit.log" || fail "resize was not audited"
R="$(mcp_policy 'default allow' "{\"jsonrpc\":\"2.0\",\"id\":17,\"method\":\"tools/call\",\"params\":{\"name\":\"terminal_resize\",\"arguments\":{\"session\":$SID,\"columns\":100,\"rows\":40}}}")"
# With default allow, evaluate() returns Allow → audit records 'resize allow: 100x40'.
grep -qE 'resize allow: 100x40' "$UBIQUITTY_HOME/agent-audit.log" || fail "allowed resize not audited as carried out ($R; audit: $(grep -E 'resize' "$UBIQUITTY_HOME/agent-audit.log" | tail -3))"
echo "mcp-smoke: terminal_resize gate+audit OK"

# 6c. The allowed resize reaches the program: it is the agent's one-shot
#     override (pane.overrideGeometry), which the agent's own later typing
#     does not end, so the program still reads it from a second attach. A
#     session of its own: the one above is busy printing.
R="$(mcp_policy 'default allow' '{"jsonrpc":"2.0","id":18,"method":"tools/call","params":{"name":"terminal_new","arguments":{}}}')"
SIZED="$(printf '%s' "$R" | grep -oE 'session [0-9]+' | grep -oE '[0-9]+' | head -1)"
[ -n "$SIZED" ] || fail "terminal_new returned no session to resize ($R)"
mcp_policy 'default allow' "{\"jsonrpc\":\"2.0\",\"id\":19,\"method\":\"tools/call\",\"params\":{\"name\":\"terminal_resize\",\"arguments\":{\"session\":$SIZED,\"columns\":97,\"rows\":31}}}" >/dev/null
R="$(mcp_policy 'default allow' "{\"jsonrpc\":\"2.0\",\"id\":20,\"method\":\"tools/call\",\"params\":{\"name\":\"terminal_run\",\"arguments\":{\"session\":$SIZED,\"command\":\"stty size\"}}}")"
echo "$R" | grep -q '31 97' || fail "the program does not read the size terminal_resize set ($R)"
grep -qE "resize allow: 97x31" "$UBIQUITTY_HOME/agent-audit.log" || fail "the resize the host carried out was not audited as carried out"
echo "mcp-smoke: terminal_resize sets the size the program reads, across the agent's next attach"

# 6d. An agent that may only watch is refused by the host, and the tool says
#     so and audits it as not carried out: the answer is the host's, not the
#     policy's.
OBSERVER="$("$HOST" token issue --scope observe --allow-always --label smoke-observer 2>/dev/null | tail -1)"
[ -n "$OBSERVER" ] || fail "could not mint an observe-only token"
R="$(UBIQUITTY_AGENT_TOKEN="$OBSERVER" mcp_policy 'default allow' "{\"jsonrpc\":\"2.0\",\"id\":21,\"method\":\"tools/call\",\"params\":{\"name\":\"terminal_resize\",\"arguments\":{\"session\":$SIZED,\"columns\":61,\"rows\":17}}}")"
echo "$R" | grep -q 'the host did not resize the session' || fail "a resize the host refused was not reported as refused ($R)"
echo "$R" | grep -q '"isError":true' || fail "a resize the host refused was not an error ($R)"
grep -qE 'resize allow: 61x17' "$UBIQUITTY_HOME/agent-audit.log" && fail "a resize the host refused was audited as carried out"
grep -qE 'resize .*61x17' "$UBIQUITTY_HOME/agent-audit.log" || fail "a resize the host refused was not audited"
R="$(mcp_policy 'default allow' "{\"jsonrpc\":\"2.0\",\"id\":22,\"method\":\"tools/call\",\"params\":{\"name\":\"terminal_run\",\"arguments\":{\"session\":$SIZED,\"command\":\"stty size\"}}}")"
echo "$R" | grep -q '31 97' || fail "a resize the host refused moved the size ($R)"
echo "mcp-smoke: terminal_resize reports and audits the host's refusal"

# 7. terminal_kill audit accuracy: the audit must reflect the REAL outcome,
#    never a premature "allow" recorded before the terminate runs. A fresh home with
#    UBIQUITTY_HOST_BIN unset disables the on-demand daemon spawn, so connectControl
#    fails deterministically — a not-carried-out terminate. An audit written before
#    the terminate runs would log this as allow; it must log deny (and never allow).
echo "mcp-smoke: terminal_kill audits the real outcome, not a premature allow"
KHOME="$(mktemp -d /tmp/ubiquitty-killaudit.XXXXXX)/.ubiquitty"
mkdir -p "$KHOME"
# An allow policy so the request reaches the daemon at all: terminal_kill is
# policy-gated like every other mutating verb, and the default is confirm, so
# without this the gate blocks first and the outcome below is never reached —
# the case would pass while testing the gate instead of the audit.
KILL_ALLOW_POLICY="$WORK/kill-allow.conf"
printf 'default allow\n' > "$KILL_ALLOW_POLICY"
printf '%s\n' '{"jsonrpc":"2.0","id":15,"method":"tools/call","params":{"name":"terminal_kill","arguments":{"session":1}}}' \
  | env -u UBIQUITTY_HOST_BIN UBIQUITTY_HOME="$KHOME" UBIQUITTY_AGENT_POLICY="$KILL_ALLOW_POLICY" "$AGENT" mcp >/dev/null 2>&1 || true
KAUDIT="$KHOME/agent-audit.log"
[ -s "$KAUDIT" ] || fail "terminal_kill wrote no audit record"
grep -q 'kill deny:' "$KAUDIT" || fail "kill that could not run not audited deny: $(cat "$KAUDIT" 2>/dev/null)"
! grep -q 'kill allow:' "$KAUDIT" || fail "kill audited as premature allow: $(cat "$KAUDIT")"
rm -rf "$(dirname "$KHOME")"

# 7b. terminal_kill is screened by the policy like every other mutating verb.
#     A Deny rule must block it before the daemon is contacted, and the block must
#     be audited — the same gate the CLI's `kill` passes, held on the MCP
#     surface. Hermetic: the gate short-circuits before any daemon contact.
echo "mcp-smoke: terminal_kill is blocked by a deny policy (never reaches the daemon)"
KDHOME="$(mktemp -d /tmp/ubiquitty-killdeny.XXXXXX)/.ubiquitty"
mkdir -p "$KDHOME"
KILL_DENY_POLICY="$WORK/kill-deny.conf"
printf 'default deny\n' > "$KILL_DENY_POLICY"
KDOUT="$(printf '%s\n' '{"jsonrpc":"2.0","id":16,"method":"tools/call","params":{"name":"terminal_kill","arguments":{"session":1}}}' \
  | env -u UBIQUITTY_HOST_BIN UBIQUITTY_HOME="$KDHOME" UBIQUITTY_AGENT_POLICY="$KILL_DENY_POLICY" "$AGENT" mcp 2>/dev/null || true)"
case "$KDOUT" in
  *"blocked by policy"*) : ;;
  *) fail "terminal_kill under deny policy was not blocked: $KDOUT" ;;
esac
KDAUDIT="$KDHOME/agent-audit.log"
grep -q 'kill deny:' "$KDAUDIT" 2>/dev/null || fail "policy-denied kill not audited: $(cat "$KDAUDIT" 2>/dev/null)"
rm -rf "$(dirname "$KDHOME")"

# 7c. run/send/interrupt create a session when `session` is omitted — their
#     documented contract — and that creation must face the same gate terminal_new
#     does. A policy that denies creation but allows the verb must stop them, or a
#     `deny new` rule stops only the verb with `new` in its name. Hermetic: the gate
#     short-circuits before any daemon contact.
echo "mcp-smoke: an implied session creation is gated as a creation"
ICHOME="$(mktemp -d /tmp/ubiquitty-implied.XXXXXX)/.ubiquitty"
mkdir -p "$ICHOME"
IC_POLICY="$WORK/deny-creation-allow-verbs.conf"
printf 'default allow\ndeny 80x24\n' > "$IC_POLICY"
for verb_args in \
  'terminal_run|{"command":"whoami"}' \
  'terminal_send|{"text":"hi"}' \
  'terminal_interrupt|{}'
do
  V="${verb_args%%|*}"; A="${verb_args##*|}"
  OUT="$(printf '%s\n' "{\"jsonrpc\":\"2.0\",\"id\":20,\"method\":\"tools/call\",\"params\":{\"name\":\"$V\",\"arguments\":$A}}" \
    | env -u UBIQUITTY_HOST_BIN UBIQUITTY_HOME="$ICHOME" UBIQUITTY_AGENT_POLICY="$IC_POLICY" "$AGENT" mcp 2>/dev/null || true)"
  case "$OUT" in
    *"blocked by policy"*) : ;;
    *) fail "$V without a session was not gated as a creation: $OUT" ;;
  esac
done
grep -q 'new deny:' "$ICHOME/agent-audit.log" 2>/dev/null || fail "implied creation not audited as new: $(cat "$ICHOME/agent-audit.log" 2>/dev/null)"

# Naming a session implies no creation, so the same policy must not block it there.
OUT="$(printf '%s\n' '{"jsonrpc":"2.0","id":21,"method":"tools/call","params":{"name":"terminal_run","arguments":{"command":"whoami","session":7}}}' \
  | env -u UBIQUITTY_HOST_BIN UBIQUITTY_HOME="$ICHOME" UBIQUITTY_AGENT_POLICY="$IC_POLICY" "$AGENT" mcp 2>/dev/null || true)"
case "$OUT" in
  *"blocked by policy"*) fail "naming a session must not trip the creation gate: $OUT" ;;
  *) : ;;
esac
rm -rf "$(dirname "$ICHOME")"

# 8. CLI negatives: new and an interrupt under a Deny policy.
#    The gate short-circuits before any daemon contact, so these are hermetic.
#    Verify-by-revert: removing the gate() call in new/an interrupt makes the
#    CLI proceed past the gate and fail for a different reason (no daemon) — the
#    exit code changes from 4 to 1, so the `-eq 4` assertions FAIL.
echo "mcp-smoke: CLI deny-policy negatives (new + interrupt — hermetic, no daemon needed)"
DENY_POLICY="$WORK/cli-deny-all.conf"
printf 'default deny\n' > "$DENY_POLICY"

# new under deny policy must refuse with exit 4 (blocked).
NEW_RC=0
UBIQUITTY_AGENT_POLICY="$DENY_POLICY" "$AGENT" new --columns 80 --rows 24 2>/dev/null || NEW_RC=$?
[ "$NEW_RC" -eq 4 ] || fail "CLI: new under deny policy exited $NEW_RC, expected 4 (blocked)"
echo "mcp-smoke: CLI new deny-policy blocked OK (exit=$NEW_RC)"

# an interrupt under deny policy must refuse with exit 4 (blocked).
INT_RC=0
UBIQUITTY_AGENT_POLICY="$DENY_POLICY" "$AGENT" send -s 1 "$(printf '\003')" 2>/dev/null || INT_RC=$?
[ "$INT_RC" -eq 4 ] || fail "CLI: an interrupt under deny policy exited $INT_RC, expected 4 (blocked)"
echo "mcp-smoke: CLI an interrupt deny-policy blocked OK (exit=$INT_RC)"

# kill under deny policy must refuse with exit 4 (blocked) — terminating a
# session is agent-origin; a Deny rule must block it, not silently proceed.
# Verify-by-revert: removing kill's gate() makes it proceed to attachControl
# (no daemon → exit 1), so the `-eq 4` assertion FAILS.
KILL_RC=0
UBIQUITTY_AGENT_POLICY="$DENY_POLICY" "$AGENT" kill -s 1 2>/dev/null || KILL_RC=$?
[ "$KILL_RC" -eq 4 ] || fail "CLI: kill under deny policy exited $KILL_RC, expected 4 (blocked)"
echo "mcp-smoke: CLI kill deny-policy blocked OK (exit=$KILL_RC)"

# 9. negative: stdin unbounded-line DoS cap.
#    Feed an INFINITE non-newline stream (cat /dev/zero piped through tr) so stdin
#    never closes. The capped agent must detect buffer overflow and exit promptly;
#    without the cap it would accumulate until OOM or the test timeout fires.
#    Bounded by a 10 s wall-clock limit so CI never blocks.
#    Verify-by-revert: raising maxMCPLineBytes past max_size() (e.g. SIZE_MAX) in
#    AgentMCP.cpp makes the appendMCPChunk check dead; the agent then accumulates
#    until libc++ throws bad_alloc or the OS OOM-kills it — either way the process
#    survives the first seconds, the cap check never fires early, and this test
#    observes that a capped error message was not emitted (see the grep below).
echo "mcp-smoke: DoS cap — infinite non-newline stream must trigger cap + exit, not hang"
OVERFLOW_OUT=""
OVERFLOW_RC=0
# Bounded here rather than by `timeout`, which a Mac does not have: the command
# was then not found, its message went to /dev/null with the agent's, and the
# stanza failed on an empty output with the agent never having run. The whole
# pipeline is one process group, ended together at the deadline, so an agent
# that does hang leaves no `cat` and `tr` feeding it.
OVERFLOW_OUT="$(python3 - "$AGENT" <<'EOF'
import os, signal, subprocess, sys
pipeline = subprocess.Popen(["bash", "-c", 'cat /dev/zero | tr "\\0" "x" | "$0" mcp', sys.argv[1]],
                            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, start_new_session=True)
try:
    output, _ = pipeline.communicate(timeout=10)
except subprocess.TimeoutExpired:
    os.killpg(pipeline.pid, signal.SIGKILL)
    pipeline.wait()
    sys.exit(124)
sys.stdout.buffer.write(output)
sys.exit(pipeline.returncode if pipeline.returncode >= 0 else 128 - pipeline.returncode)
EOF
)" || OVERFLOW_RC=$?
# The cap fires as soon as the 4 MiB limit is crossed (within the first few seconds)
# and the agent emits a JSON-RPC error then exits. Verify both:
#   1. Agent exited before the 10 s deadline (rc != 124).
#   2. The cap-overflow error was actually the reason (not some unrelated crash).
[ "$OVERFLOW_RC" -ne 124 ] || fail "agent did NOT exit within 10 s on infinite non-newline stream (timeout=124 → DoS cap missing or not triggered)"
grep -q 'line too long' <<<"$OVERFLOW_OUT" || fail "agent exited but did not emit 'line too long' cap error — wrong exit path ($OVERFLOW_OUT)"
echo "mcp-smoke: DoS cap OK (agent exited rc=$OVERFLOW_RC with cap error)"

echo "mcp-smoke: PASS"
exit 0
