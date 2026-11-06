#!/usr/bin/env bash
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

#
# Tests/Agent/ubq_smoke.sh — `ubq` against a live, isolated daemon.
#
# Pins the command line's contract: every verb is an operation the method
# registry describes and validates (a bad parameter is refused with the
# schema's sentence before anything reaches the daemon, and every --json
# result satisfies the result schema `api describe` prints); `run` returns the
# exit status of the command it typed, not of a boundary that arrived late;
# `wait` is woken by the daemon's events, reads the state only after it is
# subscribed, and reports events the log lost; `events --since` resumes after
# the cursor; `status` shows an OSC 7501 record; targets resolve by label and
# by `here`, and `here` trusts only a socket in the control directory;
# `read --scrollback` reads history and `search` finds lines in it; the
# policy refuses `new`, `split`, `kill` and `label`, and only a person's
# terminal can answer a `confirm`; a refusal
# exits with the code the manual gives it; no token reaches a daemon or ssh it
# starts; `-H` reaches a daemon through the ssh bootstrap the app's remote
# windows use; and the manual's Getting started runs as written.
#
# Hermetic: short private homes under one temporary directory, and for -H a
# stand-in `ssh` on PATH that runs the remote command under a private $HOME, so
# nothing touches a real daemon or a real host.
#
# Env:
#   UBIQUITTY_HOST_BIN   path to the built ubiquitty-host (else under UBQ_BUILD_ROOT)
#   UBQ_BIN              path to the built ubq (else under UBQ_BUILD_ROOT)
#   UBQ_CONSENT_PERSON_BIN real trusted-bridge consent participant built by run-tests.sh

set -uo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
# shellcheck source=Scripts/build-root.sh
. "$ROOT/Scripts/build-root.sh"
HOST="${UBIQUITTY_HOST_BIN:-$UBQ_BUILD_ROOT/host/ubiquitty-host}"
UBQ="${UBQ_BIN:-$UBQ_BUILD_ROOT/agent/ubq}"
CONSENT_PERSON="${UBQ_CONSENT_PERSON_BIN:-}"

fail() { echo "ubq-smoke: FAIL: $*" >&2; exit 1; }
pass() { echo "ubq-smoke: ok: $*"; }
# json_field <json> <python expression over d> — reads one value out of a reply.
json_field() { python3 -c 'import json,sys; d=json.loads(sys.argv[1]); print(eval(sys.argv[2]))' "$1" "$2"; }

[ -x "$HOST" ] || fail "ubiquitty-host not built at $HOST (run Scripts/build-host.sh)"
[ -x "$UBQ" ] || fail "ubq not built at $UBQ (run Scripts/build-agent.sh)"
[ -x "$CONSENT_PERSON" ] || fail "consent_person not built (run Scripts/run-tests.sh --only ubq_smoke)"

# --help and -h answer on standard output and exit 0, naming every verb the
# registry gives the command line. No daemon is needed or started. The text is
# matched from a here-string: `grep -q` leaves a pipe once it matches, and under
# pipefail a writer still writing to it fails the check with SIGPIPE.
for flag in --help -h; do
    HELP="$("$UBQ" "$flag" 2>/dev/null)"; status=$?
    [ "$status" -eq 0 ] || fail "ubq $flag exited $status"
    for verb in ls tree new run send read search split kill label tag status wait events mcp completions "api list" "api describe" "api call"; do
        grep -qE "^  $verb( |\$)" <<<"$HELP" || fail "ubq $flag does not list '$verb'"
    done
    for word in '-H, --host' '--json' '--yes' UBIQUITTY_AGENT_TOKEN UBQ_REMOTE_TOKEN UBIQUITTY_HOME; do
        grep -qF -- "$word" <<<"$HELP" || fail "ubq $flag does not document $word"
    done
done
pass "--help and -h list the verbs and exit 0"

# `ubq <verb> --help` and `-h` print that verb's usage and options on standard
# output and exit 0, with no daemon to reach: the home is empty and nothing
# would start one.
HELP_HOME="$(mktemp -d "${UBQ_SMOKE_TMP:-/tmp}/ubq-help.XXXXXX")"
while IFS=$'\t' read -r verb sentence; do
    read -ra words <<<"$verb"
    for flag in --help -h; do
        OUT="$(env -u UBIQUITTY_HOST_BIN -u UBIQUITTY_SOCKET UBIQUITTY_HOME="$HELP_HOME" "$UBQ" "${words[@]}" "$flag" 2>"$HELP_HOME/err")"; status=$?
        [ "$status" -eq 0 ] || { rm -rf "$HELP_HOME"; fail "ubq $verb $flag exited $status: $(cat "$HELP_HOME/err")"; }
        grep -qF "usage: ubq [-H [user@]host] [--json] [--yes] $verb" <<<"$OUT" && grep -qF "$sentence" <<<"$OUT" \
            || { rm -rf "$HELP_HOME"; fail "ubq $verb $flag does not print its usage and sentence: $OUT"; }
    done
done < <("$UBQ" api list)
OUT="$(UBIQUITTY_HOME="$HELP_HOME" "$UBQ" search --help)"
grep -qE '^  -s, --session S ' <<<"$OUT" && grep -qE '^  --case-sensitive ' <<<"$OUT" || { rm -rf "$HELP_HOME"; fail "search --help does not list -s and --case-sensitive: $OUT"; }
rm -rf "$HELP_HOME"
pass "every verb's --help and -h print its usage and options without a daemon"

# A word the command line cannot read is refused by name, before anything is
# sent: an unknown option, an option missing its value, a word a verb takes
# none of.
refused_as() {
    local expected="$1" err status
    shift
    err="$("$UBQ" "$@" 2>&1 >/dev/null)"; status=$?
    [ "$status" -eq 2 ] || fail "ubq $* exited $status, not 2: $err"
    grep -qF -- "$expected" <<<"$err" || fail "ubq $* was not refused with \"$expected\": $err"
}
refused_as "'-x' is not an option of new" new -x
refused_as "'--bogus' is not an option of send" send -s nothing --bogus hi
refused_as "'extra' is not an argument ls takes" ls extra
refused_as "'-w' is not an option of read" read -s nothing -w 1
refused_as "-s needs a session ID" read -s
refused_as "--for needs exit, idle, blocked or text:<s>" wait -s nothing --for
refused_as "--timeout needs a value" run -s nothing --timeout
refused_as "-H needs a host" -H
refused_as "api takes list, describe, call or validate, not 'bogus'" api bogus
refused_as "'extra' is one argument more than split takes" split -s nothing down extra
refused_as "cannot read /nonexistent/params.json" api call ls @/nonexistent/params.json
refused_as "api validate needs a document" api validate ls
pass "an unknown option, a missing value and a stray word are refused by name"

# A failure names its verb once: "ubq: ls: no daemon answered", not "ubq: ls:
# ls: ...". The home is empty, so no daemon answers.
EMPTY_HOME="$(mktemp -d "${UBQ_SMOKE_TMP:-/tmp}/ubq-empty.XXXXXX")"
for verb in ls tree events; do
    ERR="$(env -u UBIQUITTY_HOST_BIN -u UBIQUITTY_SOCKET UBIQUITTY_HOME="$EMPTY_HOME" "$UBQ" "$verb" 2>&1 >/dev/null)"
    grep -q "^ubq: $verb: no daemon answered" <<<"$ERR" || { rm -rf "$EMPTY_HOME"; fail "ubq $verb with no daemon does not name its verb once: $ERR"; }
done
OUT="$(env -u UBIQUITTY_HOST_BIN -u UBIQUITTY_SOCKET UBIQUITTY_HOME="$EMPTY_HOME" "$UBQ" --json ls)"
rm -rf "$EMPTY_HOME"
[ "$(json_field "$OUT" 'd["error"]["message"]')" = "ls: no daemon answered (start one: ubiquitty-host --ensure-daemon)" ] || fail "the JSON error lost its verb: $OUT"
pass "a failure names its verb once"

WORK="$(mktemp -d "${UBQ_SMOKE_TMP:-/tmp}/ubq-smoke.XXXXXX")"
export UBIQUITTY_HOME="$WORK/h"
REMOTE_HOME="$WORK/r"
# The verbs run outside any pane, so `here` names nothing unless a case sets it.
unset UBIQUITTY_SESSION UBIQUITTY_PANE UBIQUITTY_WINDOW UBIQUITTY_TAB UBIQUITTY_SOCKET UBIQUITTY_AGENT_LABEL UBQ_REMOTE_TOKEN UBIQUITTY_HOST_BIN

stop_daemons() {
    # Every home a case made is under $WORK, whatever depth its control
    # directory lies at.
    local pidfile
    while IFS= read -r pidfile; do
        p="$(cat "$pidfile" 2>/dev/null || true)"
        if [ -n "${p:-}" ] && ps -p "$p" -o comm= 2>/dev/null | grep -q 'ubiquitty-host'; then
            kill "$p" 2>/dev/null || true
            sleep 0.2
            kill -9 "$p" 2>/dev/null || true
        fi
    done < <(find "$WORK" -name 'host*.pid' -type f 2>/dev/null)
}
cleanup() {
    [ -n "${WATCHDOG:-}" ] && kill "$WATCHDOG" 2>/dev/null
    stop_person
    [ -n "${WAITER:-}" ] && kill "$WAITER" 2>/dev/null
    stop_daemons
    rm -rf "$WORK"
}
trap cleanup EXIT
# A signal ends the run through EXIT, so the daemons it started, each in a
# session of its own that no process-group kill reaches, are stopped too.
trap 'exit 143' TERM
trap 'exit 129' HUP
trap 'exit 130' INT
# The smoke's own deadline, under the runner's limit: past it the run ends
# itself through the same traps. A foreground command holds bash's trap until
# it returns, so the watchdog stops the run's direct children first.
UBQ_SMOKE_DEADLINE_SECONDS="${UBQ_SMOKE_DEADLINE_SECONDS:-1200}"
SMOKE_PID=$$
(
    trap - EXIT HUP INT
    sleep "$UBQ_SMOKE_DEADLINE_SECONDS" &
    sleeper=$!
    # Stopped with the run, it takes its timer with it.
    trap 'kill "$sleeper" 2>/dev/null; exit 0' TERM
    wait "$sleeper" || exit 0
    echo "ubq-smoke: FAIL: the smoke ran past its own ${UBQ_SMOKE_DEADLINE_SECONDS} s deadline" >&2
    kill -TERM "$SMOKE_PID" 2>/dev/null
    for child in $(pgrep -P "$SMOKE_PID"); do
        [ "$child" = "$BASHPID" ] || kill -TERM "$child" 2>/dev/null
    done
) &
WATCHDOG=$!

# A fixture person is a real client of the existing trusted host bridge. Its
# decision is explicit, and no test hook changes the daemon's authorization.
start_person() {
    local session="$1" mode="${2:-allow}"
    stop_person
    # Emptied here, before the fixture starts: the redirection below truncates
    # in the forked child, and a loop that ran first would read the previous
    # person's READY and return before this one is attached.
    : >"$WORK/person.out"
    : >"$WORK/person.err"
    "$CONSENT_PERSON" "$HOST" "${session#session:}" "$mode" >"$WORK/person.out" 2>"$WORK/person.err" &
    PERSON_PID=$!
    for ((attempt = 0; attempt < 600; ++attempt)); do
        grep -q '^READY$' "$WORK/person.out" && return
        kill -0 "$PERSON_PID" 2>/dev/null || fail "the person fixture exited: $(cat "$WORK/person.err")"
        sleep 0.05
    done
    stop_person
    fail "the person fixture did not attach: $(cat "$WORK/person.err")"
}
stop_person() {
    if [ -n "${PERSON_PID:-}" ]; then
        kill "$PERSON_PID" 2>/dev/null || true
        wait "$PERSON_PID" 2>/dev/null || true
        PERSON_PID=
    fi
}

# Creation and labelling are separate operations; the person attaches between
# them. The partial-result cases below cover new --label without an approver.
new_labelled() {
    local label="$1" made session
    made="$("$UBQ" --json new)" || return 1
    session="$(json_field "$made" 'd["session"]')"
    start_person "$session"
    if ! "$UBQ" label -s "$session" "$label" >/dev/null; then
        stop_person
        return 1
    fi
    stop_person
    printf '%s\n' "$made"
}

"$HOST" --ensure-daemon --columns 80 --rows 24 || fail "--ensure-daemon returned nonzero"
UBIQUITTY_AGENT_TOKEN="$("$HOST" token issue --scope all --label ubq-smoke 2>/dev/null | tail -1)"
[ -n "$UBIQUITTY_AGENT_TOKEN" ] || fail "could not mint the smoke's token"
export UBIQUITTY_AGENT_TOKEN
export UBIQUITTY_AGENT_POLICY="$WORK/policy.conf"
printf 'default allow\n' > "$UBIQUITTY_AGENT_POLICY"


# conforms <verb> <json> — the document satisfies the verb's result schema, as
# `api validate` checks it with the registry's own validator.
conforms() {
    local why
    why="$("$UBQ" api validate "$1" "$2" 2>&1)" || fail "the --json result of $1 does not satisfy its result schema: $why ($2)"
}

# as_person <command...> — runs a command on a pseudo-terminal, as a person at
# a prompt does, so its --yes answers a confirm; prints what it printed and
# exits with its status. A line that chains or substitutes commands needs that
# answer even under `default allow`, and a process with no terminal cannot
# give it.
as_person() {
    python3 "$ROOT/Tests/Agent/on_a_terminal.py" "$@" </dev/null | tr -d '\r'
    return "${PIPESTATUS[0]}"
}

# within <seconds> <command...> — runs the command until it succeeds, for a
# state the daemon reaches on its own schedule; fails when the ceiling passes.
within() {
    local ceiling="$1" started=$SECONDS
    shift
    until "$@"; do
        [ $((SECONDS - started)) -lt "$ceiling" ] || return 1
        sleep 0.1
    done
}
session_state() { "$UBQ" --json status -s "$1" | python3 -c 'import json,sys; print(json.load(sys.stdin)["state"])'; }
is_status_none() { [ "$(session_state "$1")" = none ]; }
is_listed_busy() { "$UBQ" ls | grep -q "^$1	$2	$3	80x24	busy	"; }

# --- every verb is an operation with a schema --------------------------------
LIST="$("$UBQ" api list --json)" || fail "api list exited nonzero"
conforms "api list" "$LIST"
for verb in ls tree new run send read search split kill label tag status wait events "api list" "api describe" "api call" "api validate" mcp completions; do
    python3 - "$LIST" "$verb" <<'EOF' || fail "api list has no schema for verb '$verb'"
import json, sys
methods = json.loads(sys.argv[1])["methods"]
entry = [m for m in methods if m["verb"] == sys.argv[2]]
sys.exit(0 if entry and entry[0]["params"].get("type") == "object" and entry[0]["description"] else 1)
EOF
done
pass "every verb appears in api list with a parameter schema"

# --- api list names every surface's operations, from the one registry --------
# The control socket's methods are the app's, and the MCP tools' schemas are
# the ones `ubq mcp` offers in tools/list: one table answers both.
CONTROL="$("$UBQ" --json api list --surface control)" || fail "api list --surface control exited nonzero"
TOOLS="$(printf '%s\n' '{"jsonrpc":"2.0","id":1,"method":"tools/list"}' | "$UBQ" mcp 2>/dev/null)"
MCP="$("$UBQ" --json api list --surface mcp)" || fail "api list --surface mcp exited nonzero"
python3 - "$CONTROL" "$MCP" "$TOOLS" <<'EOF' || fail "api list does not describe the control socket and the MCP tools as they are served"
import json, sys
control = json.loads(sys.argv[1])["methods"]
names = sorted(m["controlMethod"] for m in control)
want = sorted(["ping", "initialize", "list", "describe", "ls", "focus", "send-text", "get-text", "new-window", "new-tab", "split", "set-title"])
if names != want:
    sys.exit("control methods %s, want %s" % (names, want))
send = [m for m in control if m["controlMethod"] == "send-text"][0]
if "text" not in send["params"]["properties"] or "verb" in send:
    sys.exit("send-text listed without its schema, or with a verb it does not have")
listed = {m["mcpTool"]: m["params"] for m in json.loads(sys.argv[2])["methods"]}
served = {t["name"]: t["inputSchema"] for t in json.loads(sys.argv[3])["result"]["tools"]}
if listed != served:
    sys.exit("tools/list %s differs from api list --surface mcp %s" % (sorted(served), sorted(listed)))
EOF
"$UBQ" api list --surface control | grep -q '^send-text	' || fail "api list --surface control does not print send-text by its method name"
pass "api list --surface lists the control socket's methods and the MCP tools with the schemas they are served under"

# --- a Sync Call reaches the methods the registry names for it ------------
SYNC_METHODS="$("$UBQ" --json api list --surface sync)" || fail "api list --surface sync exited nonzero"
SEARCH="$("$UBQ" --json api describe pane.search)" || fail "api describe pane.search exited nonzero"
python3 - "$SYNC_METHODS" "$SEARCH" <<'EOF' || fail "api list and describe do not name the Sync methods"
import json, sys
names = sorted(m["syncMethod"] for m in json.loads(sys.argv[1])["methods"])
want = sorted(["client.clipboardServing", "client.label", "consent.respond", "consent.revoke", "divider.setRatio", "events.subscribe", "file.read", "host.client.disconnect", "host.describe", "host.grant.revoke", "host.management.list", "host.methods", "host.refresh", "host.session.terminate", "host.sessions.subscribe", "host.sessions.unsubscribe", "host.token.issue", "host.token.revoke", "label.set", "pane.adjustRatio", "pane.capture", "pane.clear", "pane.extractTab", "pane.extractWindow", "pane.focus", "pane.move", "pane.moveToNewTab", "pane.moveToTab", "pane.overrideGeometry", "pane.pinGeometry", "pane.processes", "pane.reclaim", "pane.search", "pane.swap", "pane.zoom", "paste.push", "session.terminate", "tab.activate", "tab.balance", "tab.extractWindow", "tab.move", "tab.moveToPane", "tab.rename", "tags.set", "window.activate", "window.close", "window.moveFocus", "workspace.addTab", "workspace.addWindow", "workspace.closePane", "workspace.closeTab", "workspace.rename", "workspace.share", "workspace.split"])
if names != want:
    sys.exit("Sync methods %s, want %s" % (names, want))
if json.loads(sys.argv[2]).get("syncMethod") != "pane.search":
    sys.exit("describe pane.search names no Sync method")
EOF
pass "api list --surface sync and describe name the methods a Sync Call reaches"

# --- a bad parameter is refused by the schema, with the registry's sentence --
ERR="$("$UBQ" wait -s nothing --for sleep 2>&1 >/dev/null)"
rc=$?
[ "$rc" -eq 2 ] || fail "a bad --for exited $rc, not the usage code 2"
grep -q "'for' must be one of" <<<"$ERR" || fail "the refusal did not quote the schema: $ERR"
grep -q "Waits until" <<<"$ERR" || fail "the refusal did not give the registry's sentence for wait: $ERR"
OUT="$("$UBQ" --json api call wait '{"for":"exit","fro":1}')"
[ $? -eq 2 ] || fail "api call with a misspelt parameter was not refused"
[ "$(json_field "$OUT" 'd["error"]["data"]["parameter"]')" = "/fro" ] || fail "the refusal did not name /fro: $OUT"
NEGATIVE="$("$UBQ" read -s nothing --scrollback -3 2>&1)"
grep -q "'lines' must be at least 1" <<<"$NEGATIVE" || fail "a negative scrollback count was not refused by the schema: $NEGATIVE"
pass "a bad parameter is refused by the registry's schema before any connection"

# --- every ubq command a refusal names runs as written ------------------------
# A refusal points at the command that says more: `ubq api describe <name>`,
# `ubq api list`. Each one printed is run here word for word and must succeed,
# including for a verb of two words and for an operation with no verb. One
# with a placeholder, such as `ubq <verb> --help`, is a pattern and not run.
hints_run() {
    local refusal="$1" hint words
    while IFS= read -r hint; do
        [ -n "$hint" ] || continue
        case "$hint" in *'<'*) continue ;; esac
        read -ra words <<<"${hint#ubq }"
        "$UBQ" "${words[@]}" >/dev/null 2>&1 || fail "the refusal names \`$hint\`, which exits $?: $refusal"
    done < <(grep -o '`ubq [^`]*`' <<<"$refusal" | tr -d '`')
}
HINTED=0
while IFS= read -r verb; do
    read -ra words <<<"$verb"
    REFUSAL="$("$UBQ" "${words[@]}" --no-such-option 2>&1 >/dev/null)"
    grep -q '`ubq ' <<<"$REFUSAL" || fail "ubq $verb --no-such-option names no command that says more: $REFUSAL"
    hints_run "$REFUSAL"
    HINTED=$((HINTED + 1))
done < <("$UBQ" api list | cut -f1)
[ "$HINTED" -ge 20 ] || fail "only $HINTED verbs were checked for their hints"
for refusal in "api describe" "api call" "api validate"; do
    read -ra words <<<"$refusal"
    hints_run "$("$UBQ" "${words[@]}" 2>&1 >/dev/null)"
done
REFUSAL="$("$UBQ" api call label.set '{"bogus":1}' 2>&1 >/dev/null)"
grep -q '`ubq api describe label.set`' <<<"$REFUSAL" || fail "a refusal of an operation with no verb does not name it by its registry name: $REFUSAL"
hints_run "$REFUSAL"
hints_run "$("$UBQ" bogus 2>&1 >/dev/null)"
[ "$(json_field "$("$UBQ" --json api describe api describe)" 'd["name"]')" = "ubq.api.describe" ] || fail "api describe does not take a verb of two words"
[ "$(json_field "$("$UBQ" --json api call api list '{"surface":"mcp"}')" 'len(d["methods"]) > 0')" = "True" ] || fail "api call does not take a verb of two words"
pass "every ubq command a refusal names runs as written"

# --- an operation that does not exist is a usage error, not a missing target -
OUT="$("$UBQ" --json api call nosuch '{}')"
rc=$?
[ "$rc" -eq 2 ] || fail "api call of an unknown operation exited $rc, not the usage code 2: $OUT"
[ "$(json_field "$OUT" 'd["error"]["data"]["kind"]')" = "unknown_method" ] || fail "the refusal is not unknown_method: $OUT"
"$UBQ" api describe nosuch >/dev/null 2>&1
rc=$?
[ "$rc" -eq 2 ] || fail "api describe of an unknown operation exited $rc, not the usage code 2"
pass "an unknown operation exits 2, as an unknown verb does"

# --- new, label, ls ----------------------------------------------------------
NEW="$(new_labelled build)" || fail "new and approved label exited nonzero: $NEW"
conforms new "$NEW"
SESSION="$(json_field "$NEW" 'd["session"]')"
PANE="$(json_field "$NEW" 'd["pane"]')"
case "$SESSION" in session:*) ;; *) fail "new returned no typed session ID: $NEW" ;; esac
"$UBQ" ls | grep -q "^$SESSION	build	$PANE	" || fail "ls does not list $SESSION labelled build in $PANE"
LISTED="$("$UBQ" --json ls)" || fail "ls --json exited nonzero"
conforms ls "$LISTED"
pass "new creates a session and a person's exact approval labels it for ls"

# --- a label the label book refuses is refused before a session is made ------
# The count reads ls's reply; an ls that fails says so with its status and its
# words, so a refused or timed-out ls is never read as a wrong count.
count_sessions() {
    local listed status
    listed="$("$UBQ" --json ls 2>"$WORK/count-sessions.err")"
    status=$?
    [ "$status" -eq 0 ] || fail "ls --json exited $status while counting sessions: $(cat "$WORK/count-sessions.err")"
    [ -n "$listed" ] || fail "ls --json printed nothing while counting sessions: $(cat "$WORK/count-sessions.err")"
    printf '%s' "$listed" | python3 -c 'import json,sys; print(len(json.load(sys.stdin)["sessions"]))'
}
BEFORE="$(count_sessions)"
OUT="$("$UBQ" --json new --label 123 --command 'echo orphan')"
rc=$?
[ "$rc" -eq 2 ] || fail "new --label 123 exited $rc, not the usage code 2: $OUT"
[ "$(count_sessions)" = "$BEFORE" ] || fail "new --label 123 was refused after it created a session"
"$UBQ" new -x >/dev/null 2>&1
[ "$(count_sessions)" = "$BEFORE" ] || fail "new -x created a session, which it would have typed -x into"
pass "new refuses a label that reads as an ID, and an option it does not take, before it creates anything"

# A valid label is a separate consented edit. A no-person refusal reports the
# retained session explicitly and never starts the optional command.
for with_command in no yes; do
    BEFORE="$(count_sessions)"
    COMMAND_ARGS=()
    [ "$with_command" = yes ] && COMMAND_ARGS=(--command "touch $WORK/partial-command-ran")
    OUT="$("$UBQ" --json new --label needs-approval ${COMMAND_ARGS[@]+"${COMMAND_ARGS[@]}"})"
    rc=$?
    [ "$rc" -eq 3 ] || fail "new --label without a person did not return refusal: $OUT"
    PARTIAL_SESSION="$(json_field "$OUT" 'd["error"]["data"]["createdSession"]')"
    PARTIAL_PANE="$(json_field "$OUT" 'd["error"]["data"]["createdPane"]')"
    [ -n "$PARTIAL_SESSION" ] && [ -n "$PARTIAL_PANE" ] || fail "partial creation omitted its identities: $OUT"
    [ "$(json_field "$OUT" 'str(d["error"]["data"]["labelApplied"])+" "+str(d["error"]["data"]["commandStarted"])')" = "False False" ] || fail "partial creation claimed an edit or command ran: $OUT"
    [ "$(count_sessions)" -eq "$((BEFORE + 1))" ] || fail "partial creation did not preserve its reported session"
    [ ! -e "$WORK/partial-command-ran" ] || fail "a command ran after its label was refused"
    if [ "$with_command" = no ]; then
        json_field "$OUT" 'd["error"]["message"]' | grep -q command && fail "partial wording invented a requested command: $OUT"
    fi
    "$UBQ" kill -s "$PARTIAL_SESSION" >/dev/null || fail "could not end the partial-created test session"
done
pass "new --label without a person reports its partial result and does not run a command"

# --- api reaches the command line's own operation by its verb ----------------
OUT="$("$UBQ" --json api call ls '{}')" || fail "api call ls exited nonzero: $OUT"
conforms ls "$OUT"
"$UBQ" --json api call ls '{}' | grep -q "\"$SESSION\"" || fail "api call ls did not list the sessions"
[ "$(json_field "$("$UBQ" --json api describe split)" 'd["name"]')" = "ubq.split" ] || fail "api describe split did not describe ubq split"
[ "$(json_field "$("$UBQ" --json api describe ls)" 'd["name"]')" = "ubq.ls" ] || fail "api describe ls did not describe ubq ls"
pass "api call and api describe reach ubq's own ls and split by their verbs"

# --- run returns the command's exit status ----------------------------------
# The marks are printed by the command itself, so the case does not depend on
# the login shell's own integration.
as_person "$UBQ" --yes run -s build -- 'printf "\033]133;C\007"; echo RUN_$((40+2)); printf "\033]133;D;5\007"' >"$WORK/run.out" 2>"$WORK/run.err"
rc=$?
[ "$rc" -eq 5 ] || fail "run returned $rc, not the command's 5 ($(cat "$WORK/run.out"))"
grep -q 'RUN_42' "$WORK/run.out" || fail "run did not print the command's output"
"$UBQ" read -s build --screen | grep -q 'RUN_42' || fail "read --screen does not show the output"
OUT="$(as_person "$UBQ" --yes --json run -s build -- 'printf "\033]133;C\007"; echo JSON_RUN; printf "\033]133;D;0\007"')" || fail "run --json exited nonzero: $OUT"
conforms run "$OUT"
pass "run returns the command's exit status and output"

# --- run ignores a boundary that arrives after it typed, before its command -
# The command before it reports its end a second after `run` has typed, which
# a run that took the first boundary would report as its own.
as_person "$UBQ" --yes send -s build --enter 'sleep 1; printf "\033]133;D;7\007"' >/dev/null || fail "send exited nonzero"
as_person "$UBQ" --yes run -s build --timeout 15 -- 'printf "\033]133;C\007"; echo LATE_$((6*7)); printf "\033]133;D;5\007"' >"$WORK/late.out" 2>"$WORK/late.err"
rc=$?
[ "$rc" -eq 5 ] || fail "run returned $rc, the status of a boundary from before its command, not its own 5 ($(cat "$WORK/late.out"))"
grep -q 'LATE_42' "$WORK/late.out" || fail "run did not print its own command's output: $(cat "$WORK/late.out")"
pass "run reports the command it typed, not a late boundary from the one before"

# --- a token short of a verb's scopes is refused, naming them -----------------
# Every verb that names a session lists sessions over a control connection,
# which a token without `control` cannot open; the refusal names the scopes
# the verb needs, gives the host's words, and the command that issues such a
# token. run, send and new ask the host what the token carries before they
# type or make anything, so a token short of one types nothing.
[ "$(json_field "$("$UBQ" --json api describe ls)" 'd["commandLineScopes"]')" = "['control']" ] || fail "api describe ls does not name the control scope"
[ "$(json_field "$("$UBQ" --json api describe run)" 'd["commandLineScopes"]')" = "['observe', 'drive', 'control', 'region']" ] || fail "api describe run does not name its scopes"
grep -qF '`ubiquitty-host token issue --scope observe,drive,control,region` carries' <<<"$("$UBQ" run --help)" || fail "run --help does not name its scopes"
SHORT_TOKEN="$("$HOST" token issue --scope observe,drive --label ubq-short 2>/dev/null | tail -1)"
for verb in ls tree; do
    ERR="$(UBIQUITTY_AGENT_TOKEN="$SHORT_TOKEN" "$UBQ" "$verb" 2>&1 >/dev/null)"
    rc=$?
    [ "$rc" -eq 7 ] || fail "$verb with an observe,drive token exited $rc, not 7: $ERR"
    grep -qF "$verb needs a token with the control scope; the host refused this one: This token carries observe,drive; a control connection" <<<"$ERR" \
        && grep -qF '`ubiquitty-host token issue --scope control`' <<<"$ERR" || fail "$verb's refusal does not name the scope it needs and the host's words: $ERR"
done
OUT="$(UBIQUITTY_AGENT_TOKEN="$SHORT_TOKEN" "$UBQ" --json read -s build)"
[ $? -eq 7 ] && [ "$(json_field "$OUT" 'd["error"]["data"]["scopes"]')" = "['observe', 'control', 'region']" ] || fail "read's refusal does not name its scopes: $OUT"
NO_REGION_TOKEN="$("$HOST" token issue --scope observe,drive,control,new --label ubq-no-region 2>/dev/null | tail -1)"
ERR="$(UBIQUITTY_AGENT_TOKEN="$NO_REGION_TOKEN" "$UBQ" run -s build --timeout 10 -- touch "$WORK/ran-without-region" 2>&1 >/dev/null)"
rc=$?
[ "$rc" -eq 7 ] && grep -qF 'without region, so nothing was sent' <<<"$ERR" || fail "run without the region scope was not refused before typing (exit $rc): $ERR"
sleep 1
[ ! -e "$WORK/ran-without-region" ] || fail "run without the region scope typed its command before it was refused"
NO_DRIVE_TOKEN="$("$HOST" token issue --scope observe,control,region,new --label ubq-no-drive 2>/dev/null | tail -1)"
for verb in "send -s build hello" "new -- echo typed"; do
    read -ra words <<<"$verb"
    ERR="$(UBIQUITTY_AGENT_TOKEN="$NO_DRIVE_TOKEN" "$UBQ" "${words[@]}" 2>&1 >/dev/null)"
    rc=$?
    [ "$rc" -eq 7 ] && grep -qF 'without drive, so nothing was sent' <<<"$ERR" || fail "$verb without the drive scope was not refused naming it (exit $rc): $ERR"
done
pass "a token short of a verb's scopes is refused naming them, before anything is typed"

# --- a verb that reads records waits for a person who answers late ----------
# A person answers later than the daemon replies. An attach the daemon holds
# for that answer waits for it whether the verb reads the screen or, as
# `read --last-command` does, only records, and the notice says it waits.
LATE_SESSION="$(json_field "$("$UBQ" --json new)" 'd["session"]')"
LATE_TOKEN="$("$HOST" token issue --scope all --label ubq-late 2>/dev/null | tail -1)"
start_person "$LATE_SESSION" late
OUT="$(UBIQUITTY_AGENT_TOKEN="$LATE_TOKEN" "$UBQ" --json read -s "$LATE_SESSION" --last-command 2>"$WORK/consent-late.err")"
rc=$?
stop_person
grep -q 'awaiting its person' "$WORK/consent-late.err" || fail "a held attach printed no notice that it waits: $(cat "$WORK/consent-late.err")"
[ "$rc" -eq 8 ] && [ "$(json_field "$OUT" 'd["error"]["data"]["kind"]')" = no_command ] \
    || fail "read --last-command gave up before a late answer instead of reading (exit $rc): $OUT $(cat "$WORK/consent-late.err")"
# The person granted the late token the session, so that token ends it.
UBIQUITTY_AGENT_TOKEN="$LATE_TOKEN" "$UBQ" kill -s "$LATE_SESSION" >/dev/null || fail "the token the person let in could not end the late-consent session"
pass "a verb that reads only records waits for a person's late answer"

# --- wait --for exit wakes on the event --------------------------------------
"$UBQ" --json wait -s build --for exit --timeout 20 >"$WORK/wait.out" 2>"$WORK/wait.err" &
WAITER=$!
sleep 1
START="$(python3 -c 'import time; print(time.time())')"
as_person "$UBQ" --yes send -s build --enter 'printf "\033]133;D;3\007"' >/dev/null || fail "send exited nonzero"
wait "$WAITER"
rc=$?
WAITER=
END="$(python3 -c 'import time; print(time.time())')"
[ "$rc" -eq 0 ] || fail "wait --for exit exited $rc ($(cat "$WORK/wait.err"))"
WAITED="$(cat "$WORK/wait.out")"
conforms wait "$WAITED"
[ "$(json_field "$WAITED" 'd["event"]["kind"]')" = "command-finished" ] || fail "wait woke on something else: $WAITED"
[ "$(json_field "$WAITED" 'd["event"]["exitStatus"]')" = "3" ] || fail "wait reported the wrong status: $WAITED"
python3 -c "import sys; sys.exit(0 if $END - $START < 3 else 1)" || fail "wait took $(python3 -c "print($END - $START)")s to hear the event"
pass "wait --for exit woke on the command-finished event with its status"

# --- events --since resumes without a gap ------------------------------------
# A subscriber is shown only the sessions it may watch, so the cursors it sees
# need not be consecutive; resuming yields the next one it may see.
"$UBQ" --json events --since 0 --limit 3 --timeout 5 >"$WORK/events.out" || fail "events --since 0 exited nonzero"
[ "$(wc -l <"$WORK/events.out" | tr -d ' ')" = 3 ] || fail "events --limit 3 printed $(wc -l <"$WORK/events.out") lines"
while IFS= read -r line; do conforms events "$line"; done <"$WORK/events.out"
SECOND="$(sed -n 2p "$WORK/events.out")"
CURSOR="$(json_field "$SECOND" 'd["cursor"]')"
RESUMED="$("$UBQ" --json events --since "$CURSOR" --limit 1 --timeout 5)" || fail "events --since $CURSOR exited nonzero"
[ "$(json_field "$RESUMED" 'd["kind"]')" != "gap" ] || fail "resuming reported a gap: $RESUMED"
[ "$(sed -n 3p "$WORK/events.out")" = "$RESUMED" ] || fail "the resumed record differs from the one first streamed"
"$UBQ" --json events --since 0 --timeout 2 | python3 -c '
import json, sys
created = [json.loads(line) for line in sys.stdin if "\"session-created\"" in line]
sys.exit(0 if created and not any("label" in event for event in created) else 1)' \
    || fail "a session-created event carries a label, which no session has when it is created"
pass "events --since resumes with the next record and no gap"

# --- status shows an OSC 7501 record -----------------------------------------
# A blocked record lasts while its program runs and is dropped at the next
# prompt, so the program here stays in the foreground until it is interrupted.
as_person "$UBQ" --yes send -s build --enter 'printf "\033]7501;id=job:state=blocked:kind=question:msg=Q29udGludWU/\033\\"; sleep 30' >/dev/null || fail "send exited nonzero"
"$UBQ" wait -s build --for blocked --timeout 30 >/dev/null || fail "wait --for blocked never saw the record"
STATUS="$("$UBQ" --json status -s build)" || fail "status exited nonzero"
conforms status "$STATUS"
[ "$(json_field "$STATUS" 'd["state"]')" = "blocked" ] || fail "status is not blocked: $STATUS"
[ "$(json_field "$STATUS" '[r["message"] for r in d["records"] if r["id"] == "job"][0]')" = "Continue?" ] || fail "status lost the message: $STATUS"
as_person "$UBQ" --yes send -s build "$(printf '\003')" >/dev/null || fail "the interrupt was not sent"
"$UBQ" wait -s build --for idle --timeout 30 >/dev/null || fail "the session did not return to its prompt"
# The shell has the terminal back when wait returns; a shell that marks its
# prompts drops the record at the prompt it draws next, which its prompt hooks
# can delay on a loaded machine.
within 30 is_status_none build || fail "the blocked record outlived its program: $("$UBQ" --json status -s build)"
pass "status shows the program's blocked record and its message"

# --- here, split, label a pane, kill -----------------------------------------
# A daemon tells each shell its socket beside its pane, and `here` is that
# daemon's pane. The app numbers the panes it runs itself apart from any
# daemon, so the same pane ID beside the app's control socket names none of the
# daemon's: acting on the daemon's pane:N there would type into a stranger.
DAEMON_SOCKET="$(ls "$UBIQUITTY_HOME"/host*.sock | head -1)"
HERE_SCREEN="$(UBIQUITTY_SOCKET="$DAEMON_SOCKET" UBIQUITTY_PANE="$PANE" "$UBQ" read --screen 2>&1)" || fail "inside $PANE, a verb with no target exited nonzero: $HERE_SCREEN"
grep -q 'RUN_42' <<<"$HERE_SCREEN" || fail "inside $PANE, a verb with no target did not act here: $HERE_SCREEN"
UBIQUITTY_SOCKET="$WORK/control.sock" UBIQUITTY_PANE="$PANE" "$UBQ" read --screen >"$WORK/here-app.out" 2>&1
rc=$?
[ "$rc" -eq 8 ] || fail "in a pane the app runs, here resolved against the daemon's $PANE (exit $rc): $(cat "$WORK/here-app.out")"
UBIQUITTY_PANE="$PANE" "$UBQ" read --screen >/dev/null 2>&1
[ $? -eq 8 ] || fail "a pane ID with no socket beside it was taken as the daemon's"
# A daemon socket's name with nothing listening: ubq reaches the daemon by
# probing, which did not number this pane, so here names nothing there.
OUT="$(UBIQUITTY_SOCKET="$UBIQUITTY_HOME/host-deadbeefdeadbeef.sock" UBIQUITTY_PANE="$PANE" "$UBQ" --json read --screen)"
rc=$?
[ "$rc" -eq 8 ] || fail "with a daemon socket that does not answer, here resolved on the daemon found by probing (exit $rc): $OUT"
[ "$(json_field "$OUT" 'd["error"]["data"]["kind"]')" = "no_target" ] || fail "the refusal is not no_target: $OUT"
start_person "$SESSION"
SPLIT="$("$UBQ" --json split -s build --down)" || fail "split exited nonzero: $SPLIT"
conforms split "$SPLIT"
NEWPANE="$(json_field "$SPLIT" 'd["pane"]')"
LABELLED="$("$UBQ" --json label -p "$NEWPANE" logs)" || fail "label -p exited nonzero"
conforms label "$LABELLED"
"$UBQ" --json ls | python3 -c 'import json,sys; s=json.load(sys.stdin)["sessions"]; sys.exit(0 if any(e["paneLabel"]=="logs" for e in s) else 1)' \
    || fail "the split pane is not listed with its label"
stop_person
KILLED="$("$UBQ" --json kill -p logs)" || fail "kill -p logs exited nonzero"
conforms kill "$KILLED"
if "$UBQ" read -p logs >/dev/null 2>&1; then fail "a killed pane still resolves"; fi
"$UBQ" read -p logs >/dev/null 2>&1
[ $? -eq 8 ] || fail "a target naming nothing did not exit 8"
pass "here, split, pane labels and kill resolve the objects they name"

# --- tree, and a label and tags at every level -------------------------------
start_person "$SESSION"
# What ls says of the session is what the daemon's tree says of its pane.
ENTRY="$("$UBQ" --json ls | python3 -c 'import json,sys; print(json.dumps([e for e in json.load(sys.stdin)["sessions"] if e["session"]==sys.argv[1]][0]))' "$SESSION")" \
    || fail "ls does not list $SESSION"
WINDOW="$(json_field "$ENTRY" 'd["window"]')"
TAB="$(json_field "$ENTRY" 'd["tab"]')"
WORKSPACE="workspace:${SESSION#session:}"
[ "$(json_field "$ENTRY" 'd["label"]')" = "build" ] && [ "$(json_field "$ENTRY" 'd["pane"]')" = "$PANE" ] && [ "$(json_field "$ENTRY" 'd["columns"]')" = "80" ] \
    || fail "ls does not say what the tree holds of $SESSION: $ENTRY"
for level in "-t $TAB tailing" "-W $WINDOW main" "-w $WORKSPACE client" "-p $PANE editor"; do
    # shellcheck disable=SC2086
    OUT="$("$UBQ" --json label $level)" || fail "label $level exited nonzero: $OUT"
    conforms label "$OUT"
    [ "$(json_field "$OUT" 'd["object"]+" "+d["label"]')" = "${level#-? }" ] || fail "label $level answered $OUT"
done
# A tab is named by the label it was just given, and a workspace by its label.
OUT="$("$UBQ" --json tag -t tailing production billing Production)" || fail "tag -t tailing exited nonzero: $OUT"
conforms tag "$OUT"
[ "$(json_field "$OUT" 'd["object"]+" "+",".join(d["tags"])')" = "$TAB production,billing" ] || fail "tag -t tailing answered $OUT"
"$UBQ" tag -w client billable >/dev/null || fail "tag -w client exited nonzero"
"$UBQ" tag -s build watch >/dev/null || fail "tag -s build exited nonzero"
TREE="$("$UBQ" --json tree)" || fail "tree exited nonzero: $TREE"
conforms tree "$TREE"
python3 - "$TREE" "$WORKSPACE" <<'EOF' || fail "tree --json does not show the labels and tags that were set: $TREE"
import json, sys
rows = json.loads(sys.argv[1])["rows"]
everything = " ".join(json.dumps(row) for row in rows)
workspace = [row for row in rows if row.get("id") == sys.argv[2]]
if not workspace:
    sys.exit("the workspace has no row")
if "client" not in json.dumps(workspace[0].get("labels", [])) or "billable" not in [tag["name"] for tag in workspace[0].get("tags", [])]:
    sys.exit("the workspace's row does not wear its label and its tag")
for word in ("tailing", "production", "billing", "watch", "editor"):
    if word not in everything:
        sys.exit("nothing in the tree says " + word)
EOF
TEXT="$("$UBQ" tree)" || fail "tree exited nonzero"
for word in client billable production watch; do
    grep -q "$word" <<<"$TEXT" || fail "tree does not print '$word': $TEXT"
done
[ "$(json_field "$("$UBQ" --json tree billable)" 'd["matchCount"]')" -ge 1 ] || fail "tree billable matched nothing"
[ "$(json_field "$("$UBQ" --json tree no-such-word-anywhere)" 'str(d["matchCount"])+" "+str(len(d["rows"]))')" = "0 0" ] || fail "tree kept rows that match no word"
# Tags are replaced whole, cleared by none, and refused with the daemon's
# sentence when it does not take them.
[ "$(json_field "$("$UBQ" --json tag -t tailing)" 'len(d["tags"])')" = "0" ] || fail "tag with no tags did not clear them"
OUT="$("$UBQ" --json tag -t tailing a b c d e f g h i j k l m n o p q)"
rc=$?
[ "$rc" -eq 2 ] || fail "seventeen tags exited $rc, not 2: $OUT"
json_field "$OUT" 'd["error"]["message"]' | grep -q "at most 16 tags" || fail "the refusal does not say how many tags an object carries: $OUT"
LONG_TAGS=()
for ((tag_index = 0; tag_index < 1100; ++tag_index)); do
    LONG_TAGS+=("xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx")
done
OUT="$("$UBQ" --json tag -t tailing ${LONG_TAGS[@]+"${LONG_TAGS[@]}"})"
rc=$?
[ "$rc" -eq 2 ] || fail "a tag call larger than a frame exited $rc, not 2: $OUT"
json_field "$OUT" 'd["error"]["message"]' | grep -q "A host call holds at most 65535 bytes" || fail "the oversized call was not refused with its byte bound: $OUT"
OUT="$("$UBQ" --json label -t "$TAB" -p "$PANE" both)"
[ $? -eq 2 ] || fail "a label aimed at two objects was not a usage error: $OUT"
OUT="$("$UBQ" --json label -t tab:99999 nothing)"
[ $? -eq 8 ] || fail "a label on a tab the daemon does not hold did not exit 8: $OUT"
for level in "-t $TAB" "-W $WINDOW" "-w $WORKSPACE" "-p $PANE"; do
    # shellcheck disable=SC2086
    "$UBQ" label $level '' >/dev/null || fail "clearing the label with $level exited nonzero"
done
"$UBQ" tag -w "$WORKSPACE" >/dev/null && "$UBQ" tag -s build >/dev/null || fail "clearing tags exited nonzero"
"$UBQ" --json tree | python3 -c 'import json,sys; rows=json.dumps(json.load(sys.stdin)["rows"]); sys.exit(1 if any(w in rows for w in ("tailing","billable","editor","watch")) else 0)' \
    || fail "a cleared label or tag is still in the tree"
pass "tree prints the daemon's outline, and a label and tags are set, found and cleared at every level"
stop_person

# --- every exact tree method is usable through ubq api with a person's answer --
API_NEW="$("$UBQ" --json new)" || fail "could not create the API consent session"
API_SESSION="$(json_field "$API_NEW" 'd["session"]')"
API_ENTRY="$("$UBQ" --json ls | python3 -c 'import json,sys; session=sys.argv[1]; print(json.dumps(next(row for row in json.load(sys.stdin)["sessions"] if row["session"]==session)))' "$API_SESSION")"
API_WORKSPACE="$("$UBQ" --json tree | python3 -c 'import json,sys; session=sys.argv[1]; print(next(row["open"]["workspace"].split(":")[1] for row in json.load(sys.stdin)["rows"] if row.get("open",{}).get("session")==session))' "$API_SESSION")"
API_WINDOW="$(json_field "$API_ENTRY" 'd["window"].split(":")[1]')"
API_TAB="$(json_field "$API_ENTRY" 'd["tab"].split(":")[1]')"
API_PANE="$(json_field "$API_ENTRY" 'd["pane"].split(":")[1]')"
API_METHODS=(label.set tags.set workspace.rename tab.rename tab.move workspace.split workspace.addTab workspace.closePane workspace.closeTab window.close)
refresh_api_window_revision() {
    [ "${API_METHODS[$method_index]}" = window.close ] || return 0
    API_REVISION="$("$UBQ" --json tree | python3 -c 'import json,sys; wanted="workspace:"+sys.argv[1]; print(next(row["revision"] for row in json.load(sys.stdin)["workspaceRevisions"] if row["workspace"]==wanted))' "$API_WORKSPACE")" || fail "could not read exact close revision"
    API_PARAMS[$method_index]="{\"window\":$API_WINDOW,\"expectedRevision\":\"$API_REVISION\"}"
}
API_PARAMS=("{\"object\":\"workspace:$API_WORKSPACE\",\"label\":\"api-label\"}" "{\"object\":\"workspace:$API_WORKSPACE\",\"tags\":[\"api-tag\"]}" "{\"workspace\":$API_WORKSPACE,\"name\":\"api-workspace\"}" "{\"tab\":$API_TAB,\"name\":\"api-tab\"}" "{\"tab\":$API_TAB,\"window\":$API_WINDOW,\"index\":0}" "{\"pane\":$API_PANE,\"orientation\":\"vertical\",\"ratio\":0.375}" "{\"window\":$API_WINDOW}" "{\"pane\":$API_PANE}" "{\"tab\":$API_TAB}" "{\"window\":$API_WINDOW}")
for ((method_index = 0; method_index < ${#API_METHODS[@]}; ++method_index)); do
    refresh_api_window_revision
    OUT="$("$UBQ" --json api call "${API_METHODS[$method_index]}" "${API_PARAMS[$method_index]}")"
    [ $? -eq 3 ] || fail "unattended ${API_METHODS[$method_index]} did not refuse: $OUT"
done
start_person "$API_SESSION" viewer
OUT="$("$UBQ" --json api call label.set "${API_PARAMS[0]}")"
[ $? -eq 3 ] || fail "a Viewer could approve an agent's tree edit: $OUT"
stop_person
start_person "$API_SESSION" deny
for ((method_index = 0; method_index < ${#API_METHODS[@]}; ++method_index)); do
    refresh_api_window_revision
    OUT="$("$UBQ" --json api call "${API_METHODS[$method_index]}" "${API_PARAMS[$method_index]}")"
    [ $? -eq 3 ] || fail "denied ${API_METHODS[$method_index]} did not refuse: $OUT"
    json_field "$OUT" 'd["error"]["message"]' | grep -q 'denied this exact tree change' || fail "the denial was not the person's decision: $OUT"
done
stop_person
OBSERVE_TOKEN="$("$HOST" token issue --scope observe,control --label ubq-observer 2>/dev/null | tail -1)"
start_person "$API_SESSION"
OUT="$(UBIQUITTY_AGENT_TOKEN="$OBSERVE_TOKEN" "$UBQ" --json api call label.set "${API_PARAMS[0]}")"
[ $? -eq 3 ] || fail "a missing requester Drive scope was supplied by the approver: $OUT"
json_field "$OUT" 'd["error"]["message"]' | grep -q 'drive scope' || fail "missing Drive was not the refusal: $OUT"
NO_NEW_TOKEN="$("$HOST" token issue --scope observe,drive,control --label ubq-no-new 2>/dev/null | tail -1)"
for method_index in 5 6; do
    OUT="$(UBIQUITTY_AGENT_TOKEN="$NO_NEW_TOKEN" "$UBQ" --json api call "${API_METHODS[$method_index]}" "${API_PARAMS[$method_index]}")"
    [ $? -eq 3 ] || fail "the approver supplied the requester's missing NewSession scope: $OUT"
    json_field "$OUT" 'd["error"]["message"]' | grep -q 'does not permit creating a session' || fail "missing NewSession was not the refusal: $OUT"
done
for ((method_index = 0; method_index < ${#API_METHODS[@]}; ++method_index)); do
    # Close the newly created pane/tab, preserving the approver's original
    # session until the final window.close. Denial above used existing ids.
    if [ "${API_METHODS[$method_index]}" = workspace.closePane ]; then
        API_PARAMS[$method_index]="{\"pane\":$API_SPLIT_PANE}"
    elif [ "${API_METHODS[$method_index]}" = workspace.closeTab ]; then
        API_PARAMS[$method_index]="{\"tab\":$API_ADDED_TAB}"
    fi
    refresh_api_window_revision
    OUT="$("$UBQ" --json api call "${API_METHODS[$method_index]}" "${API_PARAMS[$method_index]}")" || fail "approved ${API_METHODS[$method_index]} did not run: $OUT"
    conforms "${API_METHODS[$method_index]}" "$OUT"
    if [ "${API_METHODS[$method_index]}" = workspace.split ]; then
        API_SPLIT_PANE="$(json_field "$OUT" 'd["pane"].split(":")[1]')"
    elif [ "${API_METHODS[$method_index]}" = workspace.addTab ]; then
        API_ADDED_TAB="$(json_field "$OUT" 'd["tab"].split(":")[1]')"
    fi
done
stop_person
"$UBQ" --json ls | python3 -c 'import json,sys; session=sys.argv[1]; sys.exit(any(row["session"] == session for row in json.load(sys.stdin)["sessions"]))' "$API_SESSION" || fail "approved window.close left its session running"
pass "all ten ubq api tree methods require exact person approval and preserve role and scope gates"

# --- here trusts only a socket in the control directory ----------------------
# Another daemon, listening outside this account's control directory, is what
# a planted UBIQUITTY_SOCKET would name. ubq must not hand it the token or take
# its pane numbers as here: it reaches its own daemon and asks for a target.
env UBIQUITTY_HOME="$WORK/o" "$HOST" --ensure-daemon --columns 80 --rows 24 || fail "the second daemon did not start"
FOREIGN_SOCKET="$(ls "$WORK"/o/host*.sock | head -1)"
OUT="$(UBIQUITTY_SOCKET="$FOREIGN_SOCKET" "$UBQ" --json ls)" || fail "ls with a foreign UBIQUITTY_SOCKET exited $?: $OUT"
grep -q "\"$SESSION\"" <<<"$OUT" || fail "ls with a foreign UBIQUITTY_SOCKET did not reach this account's daemon: $OUT"
OUT="$(UBIQUITTY_SOCKET="$FOREIGN_SOCKET" UBIQUITTY_PANE="$PANE" "$UBQ" --json read --screen)"
[ $? -eq 8 ] || fail "here resolved beside a socket outside the control directory: $OUT"
pass "UBIQUITTY_SOCKET outside the control directory is neither connected to nor trusted for here"

# A daemon leaf is its own socket, never an alias to another daemon, even
# when that target is also inside the private control directory.
for target in "$FOREIGN_SOCKET" "$DAEMON_SOCKET"; do
    alias_socket="$UBIQUITTY_HOME/host-eeeeeeeeeeeeeeee.sock"
    ln -s "$target" "$alias_socket" || fail "could not create the discovery alias"
    OUT="$(UBIQUITTY_SOCKET="$alias_socket" UBIQUITTY_PANE="$PANE" "$UBQ" --json read --screen)"
    [ $? -eq 8 ] || fail "a socket symlink established here: $OUT"
    OUT="$(UBIQUITTY_SOCKET="$alias_socket" "$UBQ" --json ls)" || fail "discovery through real sockets failed: $OUT"
    grep -q "\"$SESSION\"" <<<"$OUT" || fail "the socket walk followed a symlink: $OUT"
    rm "$alias_socket"
done
pass "socket aliases are excluded from explicit and fallback daemon discovery"

# --- read --scrollback reads history -----------------------------------------
as_person "$UBQ" --yes send -s build --enter 'i=1; while [ $i -le 60 ]; do echo SCROLL_$i; i=$((i+1)); done; echo SCROLL_DONE_$((2+2))' >/dev/null || fail "send exited nonzero"
"$UBQ" wait -s build --for text:SCROLL_DONE_4 --timeout 30 >/dev/null || fail "the lines never reached the screen"
SCROLLED="$("$UBQ" --json read -s build --scrollback 80)" || fail "read --scrollback exited nonzero: $SCROLLED"
conforms read "$SCROLLED"
python3 - "$SCROLLED" <<'EOF' || fail "read --scrollback 80 did not return the history that scrolled off: $SCROLLED"
import json, re, sys
text = json.loads(sys.argv[1])["text"]
sys.exit(0 if re.search(r"^SCROLL_1\s*$", text, re.M) and re.search(r"^SCROLL_60\s*$", text, re.M) and len(text.rstrip("\n").split("\n")) <= 80 else 1)
EOF
# The bottom is whatever the screen ends with, which is the shell's prompt and
# anything its hooks print before it, not a line this test chose: a login
# shell whose prompt hooks write two lines (a precmd that calls nice(1) under
# this suite's nice -n 19 does) pushes a fixed marker out of the last three.
SCREEN_NOW="$("$UBQ" --json read -s build --screen)" || fail "read --screen exited nonzero: $SCREEN_NOW"
LAST3="$("$UBQ" --json read -s build --scrollback 3)" || fail "read --scrollback 3 exited nonzero: $LAST3"
python3 - "$SCREEN_NOW" "$LAST3" <<'EOF' || fail "read --scrollback 3 does not end at the bottom of the screen: $LAST3 against $SCREEN_NOW"
import json, sys
# Each row ends in a newline. A bottom row the shell has not drawn its prompt
# on yet is blank, and the screen's text trims blank rows from its end, so
# both sides are compared with their trailing blank rows dropped.
def rows(text):
    return [line.rstrip() for line in (text[:-1] if text.endswith("\n") else text).split("\n")]
def trimmed(lines):
    while lines and not lines[-1]:
        lines = lines[:-1]
    return lines
screen = trimmed(rows(json.loads(sys.argv[1])["text"]))
read = rows(json.loads(sys.argv[2])["text"])
last = trimmed(read)
sys.exit(0 if len(read) == 3 and last and last == screen[-len(last):] else 1)
EOF
pass "read --scrollback returns the lines that scrolled off, ending at the bottom"

# --- search finds lines in the history the host keeps ------------------------
# SCROLL_10 to SCROLL_19 left the screen above; the host searches its own
# history, whatever this connection was sent.
"$UBQ" search -s nothing >/dev/null 2>&1
[ $? -eq 2 ] || fail "search without a pattern was not refused by the schema before connecting"
FOUND="$("$UBQ" --json search -s build 'SCROLL_1[0-9]')" || fail "search exited nonzero: $FOUND"
conforms search "$FOUND"
python3 - "$FOUND" <<'EOF' || fail "search did not find SCROLL_10 to SCROLL_19 as ten distinct lines: $FOUND"
import json, sys
d = json.loads(sys.argv[1])
lines = {m["line"] for m in d["matches"]}
sys.exit(0 if len(d["matches"]) == 10 and len(lines) == 10 and not d["truncated"]
         and all(m["startCell"] == 0 and m["endCell"] == 9 for m in d["matches"]) else 1)
EOF
TEXT="$("$UBQ" search -s build --case-sensitive 'SCROLL_1[0-9]')" || fail "search exited nonzero: $TEXT"
[ "$(printf '%s\n' "$TEXT" | awk -F'\t' 'NF == 3 && $2 == 0 && $3 == 9' | wc -l | tr -d ' ')" = 10 ] || fail "search did not print one line, start and end per match: $TEXT"
NONE="$("$UBQ" search -s build --case-sensitive 'scroll_1[0-9]')" || fail "a search with no match exited nonzero"
[ -z "$NONE" ] || fail "a case-sensitive search matched another case: $NONE"
pass "search finds lines that scrolled off on the host, by logical line"

# --- send's cursor, and a wait that the bounded log outran -------------------
SENT="$(as_person "$UBQ" --yes --json send -s build --enter 'printf "\033]133;D;4\007"')" || fail "send exited nonzero: $SENT"
conforms send "$SENT"
FROM="$(json_field "$SENT" 'd["cursor"]')"
[ "$FROM" -gt 0 ] || fail "send reported cursor $FROM, which would replay the whole log"
# More status changes than the log keeps, so the command-finished event after
# the sent cursor is gone by the time the wait asks for it. The host reports a
# change it saw between two of its reads, so a round whose two halves land in
# one read reports nothing; twice the rounds the log needs, with pauses a
# loaded host still reads between, leave it outrun.
as_person "$UBQ" --yes send -s build --enter 'r=0; while [ $r -lt 16 ]; do i=0; while [ $i -lt 256 ]; do printf "\033]7501;id=f%d:state=working\033\\" $i; i=$((i+1)); done; sleep 0.6; i=0; while [ $i -lt 256 ]; do printf "\033]7501;id=f%d:state=clear\033\\" $i; i=$((i+1)); done; sleep 0.6; r=$((r+1)); done; echo FLOOD_$((1+1))_DONE' \
    >/dev/null || fail "send exited nonzero"
"$UBQ" wait -s build --for text:FLOOD_2_DONE --timeout 120 >/dev/null || fail "the status flood did not finish"
OUT="$("$UBQ" --json wait -s build --for exit --since "$FROM" --timeout 10)"
rc=$?
[ "$rc" -eq 10 ] || fail "a wait whose events the log dropped exited $rc, not 10: $OUT"
[ "$(json_field "$OUT" 'd["error"]["data"]["kind"]')" = "gap" ] || fail "the lost events were not reported as a gap: $OUT"
pass "send reports a real cursor, and a wait the log outran says so instead of waiting on"

# --- the policy, and who may answer it ----------------------------------------
printf 'default confirm\nallow printf\n' >"$WORK/confirm.conf"
OUT="$(UBIQUITTY_AGENT_POLICY="$WORK/confirm.conf" "$UBQ" --json new)"
rc=$?
[ "$rc" -eq 4 ] || fail "new under a confirm policy exited $rc, not 4: $OUT"
[ "$(json_field "$OUT" 'd["error"]["data"]["kind"]')" = "policy" ] || fail "the refusal is not policy: $OUT"
UBIQUITTY_AGENT_POLICY="$WORK/confirm.conf" "$UBQ" split -s build --down </dev/null >/dev/null 2>&1
[ $? -eq 4 ] || fail "split under a confirm policy was not refused with 4"
UBIQUITTY_AGENT_POLICY="$WORK/confirm.conf" "$UBQ" --yes new </dev/null >/dev/null 2>&1
[ $? -eq 4 ] || fail "--yes from a process with no terminal approved a confirm"
APPROVED="$(as_person env UBIQUITTY_AGENT_POLICY="$WORK/confirm.conf" "$UBQ" --yes --json new)"
[ $? -eq 0 ] || fail "--yes typed at a terminal did not approve a confirm"
APPROVED_SESSION="$(json_field "$APPROVED" 'd["session"]')"
start_person "$APPROVED_SESSION"
"$UBQ" label -s "$APPROVED_SESSION" approved >/dev/null || fail "a person could not approve the label"
stop_person
"$UBQ" ls | grep -q '	approved	' || fail "the session a person approved was not created"
as_person env UBIQUITTY_AGENT_POLICY="$WORK/confirm.conf" UBIQUITTY_AGENT_LABEL=helper "$UBQ" --yes new >/dev/null
[ $? -eq 4 ] || fail "--yes from a process that names itself an agent approved a confirm"
printf 'default allow\ndeny <terminate session>\ndeny <label>\ndeny <tag>\ndeny <split down>\n' >"$WORK/deny.conf"
as_person env UBIQUITTY_AGENT_POLICY="$WORK/deny.conf" "$UBQ" --yes kill -s approved >/dev/null
[ $? -eq 4 ] || fail "a deny rule did not stop kill, even with --yes"
"$UBQ" ls | grep -q '	approved	' || fail "kill ran despite the deny rule"
UBIQUITTY_AGENT_POLICY="$WORK/deny.conf" "$UBQ" label -s approved renamed >/dev/null 2>&1
[ $? -eq 4 ] || fail "a deny rule did not stop label"
UBIQUITTY_AGENT_POLICY="$WORK/deny.conf" "$UBQ" tag -s approved marked >/dev/null 2>&1
[ $? -eq 4 ] || fail "a deny rule did not stop tag"
UBIQUITTY_AGENT_POLICY="$WORK/deny.conf" "$UBQ" api call workspace.split '{"pane":1,"orientation":"horizontal","ratio":0.5}' >/dev/null 2>&1
[ $? -eq 4 ] || fail "api call bypassed the ordinary split policy description"
for closing_method in window.close workspace.closeTab workspace.closePane; do
    case "$closing_method" in
        window.close) closing_params='{"window":1,"expectedRevision":"1"}' ;;
        workspace.closeTab) closing_params='{"tab":1}' ;;
        workspace.closePane) closing_params='{"pane":1}' ;;
    esac
    UBIQUITTY_AGENT_POLICY="$WORK/deny.conf" "$UBQ" api call "$closing_method" "$closing_params" >/dev/null 2>&1
    [ $? -eq 4 ] || fail "api call bypassed the ordinary termination policy description: $closing_method"
done
"$UBQ" --json tree marked | python3 -c 'import json,sys; sys.exit(0 if json.load(sys.stdin)["matchCount"] == 0 else 1)' || fail "a tag the policy denied was set"
pass "the policy screens new, split, kill, label and tag, and only a person's terminal answers a confirm"

# --- an ambiguous label, a refused split, a run that times out ---------------
DUP_ONE="$(new_labelled dup)" || fail "new and approved label dup exited nonzero"
DUP_TWO="$(new_labelled dup)" || fail "the second new and approved label dup exited nonzero"
OUT="$("$UBQ" --json read -s dup)"
rc=$?
[ "$rc" -eq 8 ] || fail "a label naming two sessions exited $rc, not 8: $OUT"
python3 - "$OUT" "$(json_field "$DUP_ONE" 'd["session"]')" "$(json_field "$DUP_TWO" 'd["session"]')" <<'EOF' || fail "the refusal does not list both sessions: $OUT"
import json, sys
candidates = json.loads(sys.argv[1])["error"]["data"]["candidates"]
sys.exit(0 if sorted(candidates) == sorted(sys.argv[2:]) else 1)
EOF
# A session created by another token still requires an attached person's
# exact-operation consent; neither token can edit it unattended.
OTHER_TOKEN="$("$HOST" token issue --scope all --label ubq-other 2>/dev/null | tail -1)"
UBIQUITTY_AGENT_TOKEN="$OTHER_TOKEN" new_labelled theirs >/dev/null || fail "the other token could not create and label a session with approval"
BEFORE="$(count_sessions)"
OUT="$("$UBQ" --json split -s theirs)"
rc=$?
[ "$rc" -eq 3 ] || fail "a split the daemon refused exited $rc, not 3: $OUT"
[ "$(json_field "$OUT" 'd["error"]["data"]["kind"]')" = "forbidden" ] || fail "the unattended split was not forbidden: $OUT"
json_field "$OUT" 'd["error"]["message"]' | grep -q "no such person is attached" || fail "the split refusal does not name missing consent: $OUT"
[ "$(count_sessions)" = "$BEFORE" ] || fail "the unattended split created a session"
# Nor its to label or tag, and the daemon's answer says what would allow it.
for verb in "label -s theirs mine" "tag -s theirs mine"; do
    # shellcheck disable=SC2086
    OUT="$("$UBQ" --json $verb)"
    rc=$?
    [ "$rc" -eq 3 ] || fail "$verb, which the daemon refused, exited $rc, not 3: $OUT"
    json_field "$OUT" 'd["error"]["message"]' | grep -q "no such person is attached" || fail "the refusal of $verb does not say what would allow it: $OUT"
done
"$UBQ" --json ls | python3 -c 'import json,sys; sys.exit(0 if [e["label"] for e in json.load(sys.stdin)["sessions"]].count("theirs") == 1 else 1)' || fail "a refused label replaced the session's own"
# Nor end it: with nobody attached to ask, the daemon refuses, and the refusal
# is the daemon's reason, not a token that was never missing.
OUT="$("$UBQ" --json kill -s theirs)"
rc=$?
[ "$rc" -eq 7 ] || fail "kill of another token's session with nobody attached exited $rc, not 7: $OUT"
json_field "$OUT" 'd["error"]["message"]' | grep -q 'no attached person can approve' || fail "the kill refusal is not the daemon's reason: $OUT"
if json_field "$OUT" 'd["error"]["message"]' | grep -q 'no agent token presented'; then fail "the kill refusal blames a token that was presented: $OUT"; fi
[ "$(count_sessions)" = "$BEFORE" ] || fail "a refused kill ended the session"
OUT="$("$UBQ" --json run -s build --timeout 1 -- sleep 8)"
rc=$?
[ "$rc" -eq 9 ] || fail "run --timeout 1 -- sleep 8 exited $rc, not 9: $OUT"
python3 -c 'import json,sys; error=json.loads(sys.argv[1])["error"]; sys.exit(0 if error["data"]["kind"] == "timeout" and "may still be running; ubq read shows its screen" in error["message"] else 1)' "$OUT" || fail "the timeout does not explain the running command: $OUT"
# ls says whether a session is at its prompt, from the tree: not while the
# command still runs, and again once it has ended.
# run has typed the line; the shell starts sleep when it reads it, which on a
# loaded machine can be after run gave up waiting.
within 30 is_listed_busy "$SESSION" build "$PANE" || fail "ls does not say $SESSION is busy while sleep runs: $("$UBQ" ls)"
[ "$(json_field "$("$UBQ" --json ls)" '[e["atPrompt"] for e in d["sessions"] if e["label"]=="build"]')" = "[False]" ] || fail "ls --json says $SESSION is at its prompt while sleep runs"
"$UBQ" wait -s build --for idle --timeout 60 >/dev/null || fail "the session did not return to its prompt after sleep"
"$UBQ" ls | grep -q "^$SESSION	build	$PANE	80x24	idle	" || fail "ls does not say $SESSION is idle at its prompt: $("$UBQ" ls)"
[ "$(json_field "$("$UBQ" --json ls)" '[e["atPrompt"] for e in d["sessions"] if e["label"]=="build"]')" = "[True]" ] || fail "ls --json does not say $SESSION is at its prompt"
pass "an ambiguous label lists its matches, a refused split, label or tag exits 3 with its reason, and a run that times out carries the screen"

# --- a daemon ubq starts inherits no token -----------------------------------
mkdir -p "$WORK/s"
chmod 700 "$WORK/s"
SPAWN_TOKEN="$(env UBIQUITTY_HOME="$WORK/s" "$HOST" token issue --scope all --label spawned 2>/dev/null | tail -1)"
as_person env UBIQUITTY_HOME="$WORK/s" UBIQUITTY_HOST_BIN="$HOST" UBIQUITTY_AGENT_TOKEN="$SPAWN_TOKEN" UBQ_REMOTE_TOKEN=remote-secret \
    "$UBQ" --yes new --command "env > '$WORK/spawned.env'" >/dev/null || fail "ubq could not start a daemon and a session in it"
# The session's shell writes the file when it runs the command, on the
# machine's schedule rather than a fixed allowance.
within 60 grep -qs '^UBIQUITTY_PANE=' "$WORK/spawned.env" || fail "the spawned session's environment was not captured"
if grep -q '^UBIQUITTY_AGENT_TOKEN=\|^UBQ_REMOTE_TOKEN=' "$WORK/spawned.env"; then
    fail "a shell of the daemon ubq started holds a token: $(grep TOKEN "$WORK/spawned.env")"
fi
pass "a daemon ubq starts, and every shell it runs, inherits no token"

# --- -H reaches a daemon through the ssh bootstrap ---------------------------
mkdir -p "$WORK/bin" "$REMOTE_HOME/.ubiquitty"
chmod 700 "$REMOTE_HOME/.ubiquitty"
ln -s "$HOST" "$REMOTE_HOME/.ubiquitty/ubiquitty-host"
# Stands in for ssh: records the environment ssh would forward, waits as a
# slow link does when the case asks, ignores the options and the host, and
# runs the remote command as the remote shell would, in the remote home.
cat >"$WORK/bin/ssh" <<EOF
#!/bin/sh
env > "$WORK/ssh.env"
[ -n "\${FAKE_SSH_DELAY:-}" ] && sleep "\$FAKE_SSH_DELAY"
for last; do :; done
exec env -u UBIQUITTY_HOME HOME="$REMOTE_HOME" /bin/sh -c "\$last"
EOF
chmod +x "$WORK/bin/ssh"
# Without UBQ_REMOTE_TOKEN the remote daemon is shown no token at all, and the
# refusal says which variable to set and that the remote host issues it: the
# local token in UBIQUITTY_AGENT_TOKEN is not one, and is not offered.
ERR="$(PATH="$WORK/bin:$PATH" "$UBQ" -H tester@remotebox ls 2>&1 >/dev/null)"
rc=$?
[ "$rc" -eq 7 ] || fail "-H without UBQ_REMOTE_TOKEN exited $rc, not 7: $ERR"
grep -q 'UBQ_REMOTE_TOKEN' <<<"$ERR" && grep -q "that host's own \`ubiquitty-host token issue\`" <<<"$ERR" \
    || fail "-H without a token does not say to set UBQ_REMOTE_TOKEN from the remote host's token issue: $ERR"
if grep -q 'UBIQUITTY_AGENT_TOKEN' <<<"$ERR"; then fail "-H without a token names the local token, which -H never sends: $ERR"; fi
ERR="$(PATH="$WORK/bin:$PATH" UBQ_REMOTE_TOKEN=deadbeefdeadbeefdeadbeef "$UBQ" -H tester@remotebox ls 2>&1 >/dev/null)"
rc=$?
[ "$rc" -eq 7 ] && grep -q 'does not recognize UBQ_REMOTE_TOKEN' <<<"$ERR" || fail "-H with a token the remote host never issued does not name UBQ_REMOTE_TOKEN (exit $rc): $ERR"
pass "-H names UBQ_REMOTE_TOKEN, and the host that issues it, when the remote host refuses the token"
# A token means something only to the daemon that issued it, so the remote
# account issues its own and the local one is not sent over ssh.
UBQ_REMOTE_TOKEN="$(env -u UBIQUITTY_HOME HOME="$REMOTE_HOME" "$HOST" token issue --scope all --label ubq-remote 2>/dev/null | tail -1)"
[ -n "$UBQ_REMOTE_TOKEN" ] || fail "the remote account could not issue a token"
export UBQ_REMOTE_TOKEN
REMOTE="$(PATH="$WORK/bin:$PATH" "$UBQ" -H tester@remotebox --json new)" || fail "-H new exited nonzero: $REMOTE"
REMOTE_SESSION="$(json_field "$REMOTE" 'd["session"]')"
PATH="$WORK/bin:$PATH" "$UBQ" -H tester@remotebox ls | grep -q "^$REMOTE_SESSION	" || fail "-H ls does not list $REMOTE_SESSION"
[ -n "$(find "$REMOTE_HOME/.ubiquitty" -maxdepth 1 -type s -name 'host*.sock' -print -quit)" ] || fail "-H did not reach the remote account's daemon"
if grep -q '^UBQ_REMOTE_TOKEN=\|^UBIQUITTY_AGENT_TOKEN=' "$WORK/ssh.env"; then
    fail "ssh was started with a token in its environment, which SendEnv would forward"
fi
pass "-H reaches the remote daemon through the ssh bootstrap, and ssh holds no token"

# MCP must select the same host for attached tools and for listing/termination.
# Both private daemons are live, so silently selecting the local one is visible.
PATH="$WORK/bin:$PATH" python3 - "$UBQ" "$REMOTE_SESSION" <<'EOF' || fail "-H MCP selected the wrong daemon"
import json
import os
import subprocess
import sys

agent, session = sys.argv[1:]
session = int(session.removeprefix("session:"))

def call(name, arguments, environment=None):
    messages = [
        {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {
            "protocolVersion": "2024-11-05", "capabilities": {},
            "clientInfo": {"name": "remote-screen-reader", "version": "1"}}},
        {"jsonrpc": "2.0", "id": 2, "method": "tools/call", "params": {
            "name": name, "arguments": arguments}},
    ]
    process = subprocess.run([agent, "-H", "tester@remotebox", "mcp"],
                             input="".join(json.dumps(message) + "\n" for message in messages),
                             text=True, capture_output=True, timeout=30, env=environment)
    assert process.returncode == 0, process.stderr
    replies = [json.loads(line) for line in process.stdout.splitlines()]
    result = next(reply["result"] for reply in replies if reply.get("id") == 2)
    return result, "\n".join(block["text"] for block in result["content"] if block["type"] == "text")

without_remote_token = os.environ.copy()
without_remote_token.pop("UBQ_REMOTE_TOKEN", None)
for name, arguments in (("terminal_screen", {"session": session}), ("terminal_list", {}),
                        ("terminal_kill", {"session": session})):
    result, text = call(name, arguments, without_remote_token)
    assert result.get("isError") and "UBQ_REMOTE_TOKEN" in text, (name, result, text)

result, text = call("terminal_list", {})
assert not result.get("isError") and any(line.startswith(f"{session}\t") for line in text.splitlines()), (result, text)
result, text = call("terminal_run", {"session": session, "command": "printf 'MCP_REMOTE_%s\\n' screen"})
assert not result.get("isError") and "MCP_REMOTE_screen" in text, (result, text)
result, text = call("terminal_screen", {"session": session})
assert not result.get("isError") and "MCP_REMOTE_screen" in text.splitlines(), (result, text)
result, text = call("terminal_screen", {"session": session, "format": "structured"})
assert not result.get("isError") and isinstance(json.loads(text), dict), (result, text)

result, text = call("terminal_new", {})
assert not result.get("isError") and text.startswith("session "), (result, text)
temporary_session = int(text.split()[1])
result, text = call("terminal_kill", {"session": temporary_session})
assert not result.get("isError"), (result, text)
result, text = call("terminal_list", {})
assert not result.get("isError"), (result, text)
assert not any(line.startswith(f"{temporary_session}\t") for line in text.splitlines()), text
assert any(line.startswith(f"{session}\t") for line in text.splitlines()), text
EOF
pass "-H MCP reads remote screen text and uses the remote token for every connection"

# --- wait --for idle reads the state after it is subscribed ------------------
# Each connection over the slow link takes two seconds, so the command below is
# busy when wait first reaches the daemon and back at its prompt before a wait
# that attached afterwards could hear it return.
PATH="$WORK/bin:$PATH" "$UBQ" -H tester@remotebox send -s "$REMOTE_SESSION" --enter 'sleep 3' || fail "-H send exited nonzero"
PATH="$WORK/bin:$PATH" FAKE_SSH_DELAY=2 "$UBQ" -H tester@remotebox wait -s "$REMOTE_SESSION" --for idle --timeout 15 >/dev/null
rc=$?
[ "$rc" -eq 0 ] || fail "wait --for idle missed a return to the prompt while it was connecting (exit $rc)"
pass "wait --for idle sees a return to the prompt that happened while it attached"

# --- the manual's Getting started runs as written ----------------------------
# The two blocks after the build, in a home of their own, with nothing of this
# smoke's environment but the build root, bounded so a manual whose `run`
# never ends fails instead of hanging. The build block is the one step not
# run: the binaries it makes are the ones this smoke drives, so the manual's
# paths to them name those.
python3 - "$ROOT/Docs/ubq.md" "$HOST" "$(dirname "$UBQ")" >"$WORK/getting-started.sh" <<'EOF' || fail "Docs/ubq.md has no Getting started blocks to run"
import re, shlex, sys
text = open(sys.argv[1], encoding="utf-8").read()
section = text.split("## Getting started", 1)[1].split("\n## ", 1)[0]
blocks = re.findall(r"```sh\n(.*?)```", section, re.S)
if len(blocks) < 3:
    sys.exit(1)
script = "\n".join(blocks[1:3]).replace("ubq run ", "ubq run --timeout 30 ", 1)
built = {
    'export UBIQUITTY_HOST_BIN="$UBQ_BUILD_ROOT/host/ubiquitty-host"': "export UBIQUITTY_HOST_BIN=" + shlex.quote(sys.argv[2]),
    'export PATH="$UBQ_BUILD_ROOT/agent:$PATH"': "export PATH=" + shlex.quote(sys.argv[3]) + ':"$PATH"',
}
for written, used in built.items():
    if written not in script:
        sys.exit(1)
    script = script.replace(written, used, 1)
print("set -e")
print(script)
EOF
# The manual asks for the shell integration before the examples run; the
# home here holds the startup files that source it, for either shell.
mkdir -p "$WORK/g"
printf 'source "%s/Resources/shell-integration/zsh.sh"\n' "$ROOT" >"$WORK/g/.zshrc"
printf 'source "%s/Resources/shell-integration/bash.sh"\n' "$ROOT" >"$WORK/g/.bashrc"
printf '. "$HOME/.bashrc"\n' >"$WORK/g/.bash_profile"
( cd "$ROOT" && env -i HOME="$WORK/g" PATH="/usr/bin:/bin:/usr/sbin:/sbin" UBIQUITTY_HOME="$WORK/g/.ubiquitty" UBQ_BUILD_ROOT="$UBQ_BUILD_ROOT" \
    TMPDIR="${TMPDIR:-/tmp}" bash "$WORK/getting-started.sh" ) >"$WORK/getting-started.out" 2>&1 \
    || fail "the Getting started commands in Docs/ubq.md fail as written: $(tail -5 "$WORK/getting-started.out")"
pass "the manual's Getting started runs as written"

echo "ubq-smoke: PASS"
exit 0
