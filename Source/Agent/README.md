<!-- Copyright (C) 2026 Stanislav Yaglo -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# ubq — the command line for people and agents

`ubq` lets a person, script, or agent run commands in a real terminal and read
the screen as UTF-8 text. Shell integration supplies command boundaries;
event subscriptions provide completion notifications without repeated reads.

It is a headless client of `ubiquitty-host`. The daemon owns the PTYs and
terminal state and keeps sessions alive across disconnects. `ubq` applies
checkpoints and updates to a passive terminal model; it does not parse the
shell's VT output again. Text queries read that model, and PNG screenshots use
the shared software renderer.

The manual — every verb with examples, targets, context, events, `wait`,
`api`, JSON shapes, remote use and exit codes — is
[Docs/ubq.md](../../Docs/ubq.md). This file is for working on the code.

```
  AI agent ──(MCP tools / CLI)──▶ ubq ──Sync proto──▶ ubiquitty-host ──▶ shell (PTY + Core)
                                  (this binary)   over daemon sock  (the daemon)
```

## Build

```sh
./Scripts/build-host.sh
./Scripts/build-agent.sh
export UBQ_BUILD_ROOT="$(python3 Scripts/build_root.py .)"
export UBIQUITTY_HOST_BIN="$UBQ_BUILD_ROOT/host/ubiquitty-host"
export UBQ_BIN="$UBQ_BUILD_ROOT/agent/ubq"
```

Both are portable POSIX C++ (no Cocoa/Metal), so they build on macOS and Linux.

## Run

`ubq` needs a running daemon. Start one detached from the invoking shell:

```sh
"$UBIQUITTY_HOST_BIN" --ensure-daemon --columns 80 --rows 24
```

The daemon listens on a socket under `~/.ubiquitty/`. Content-addressed builds
use `host-<id>.sock`, allowing daemons with different build identities to
coexist. Override the control
directory with `$UBIQUITTY_HOME` to keep sessions, tokens, and policy separate.
[Files in the control directory](../../Docs/quic-trust.md#files-in-the-control-directory)
lists everything the daemon and `ubq` keep there, and says when the daemon
exits.

If `$UBIQUITTY_HOST_BIN` points at the `ubiquitty-host` binary, `ubq`
**auto-starts** a daemon on first connect, so you can skip this step — the MCP
registration below sets it.

The daemon refuses an agent that presents no capability token (see
[Safety](#safety)), so issue one once and save it where the agent looks for it,
the `agent-token` file in the control directory:

```sh
dir="${UBIQUITTY_HOME:-$HOME/.ubiquitty}"; mkdir -p "$dir"
(umask 077 && "$UBIQUITTY_HOST_BIN" token issue --scope all --label my-agent > "$dir/agent-token.new") \
    && mv "$dir/agent-token.new" "$dir/agent-token"
```

The file reaches every agent process of this user, including an MCP server an
MCP client starts; `$UBIQUITTY_AGENT_TOKEN` overrides it for one process.

### Verbs

Every verb is an operation in the method registry
(`Source/Core/Protocol/Control/MethodRegistry.cpp`). The command line turns
its arguments into the operation's parameters and validates them against the
registry's schema before anything reaches the daemon, so a misspelt flag or a
value out of range is refused with the schema's sentence and exit code 2.
`--json` prints any verb's result as the JSON its result schema describes.
`ubq ls --json` includes `windowLabel` and `tabLabel` alongside each session
and pane label. An unlabelled parent has an empty string, not a derived title.

```sh
ubq ls                                    # sessions, their labels and panes
ubq tree [words]                          # workspaces, windows, tabs and panes as an outline
ubq new [--label L] [-- cmd]              # create a session, print session:N
ubq run [-s S | -p P] -- cmd              # output and reported status; 11 if merely settled
ubq send [-s S | -p P] [--enter] <text>   # type text
ubq read [-s S | -p P] [--screen | --last-command | --scrollback N]
ubq split [-s S | -p P] [right | down]    # split a pane, print pane:N
ubq kill [-s S | -p P]                    # end a session
ubq label [-w W | -W W | -t T | -p P | -s S] <label>   # label a workspace, window, tab or pane, else the session
ubq tag [-w W | -W W | -t T | -p P | -s S] [tags...]   # replace its tags; none clears them
ubq status [-s S | -p P]                  # OSC 7501 records: idle/working/done/blocked/error
ubq wait [-s S | -p P] --for exit|idle|blocked|text:<s> [--since C] [--timeout SECONDS]
ubq events [--since C] [--limit N] [--timeout SECONDS]   # one event per line
ubq api list [--surface command-line|control|mcp|sync|all] | api describe <name> | api call <verb> [json | @file | -]
ubq mcp                                   # the MCP server over standard input and output
ubq completions bash|zsh
```

Global options, anywhere on the line: `-H [user@]host` reaches that host's
daemon over ssh, through the same bootstrap the app's remote windows use;
`--json`; `--yes` approves a `confirm` policy decision when typed at a
terminal by a process that does not set `UBIQUITTY_AGENT_LABEL`, and approves
nothing otherwise.

Tree edits require an attached person's approval for each exact operation,
including when a person typed `ubq` in a shell. A standing attach grant does
not approve labels, tags, names, tab moves or window closure. Without an
eligible attached person they refuse; a question expires after thirty seconds.

`new --label` creates the session before requesting that separate label edit.
If the label is denied, expires or cannot be requested, the session remains:
the error reports `createdSession`, `createdPane`, `labelApplied: false` and
`commandStarted: false`. Any optional command is not started. The command
returns the refusal's nonzero status, not an unqualified success, and does not
silently kill the session it created.

**Targets.** `-s` takes a session ID (`session:3`, or `3`), a session label,
or `here`; `-p` takes a pane ID, a pane label, or `here`. A label that names
two objects is refused with both named rather than guessed. Inside a pane,
with neither given, a verb acts on the pane it runs in: the daemon tells every
program it launches `UBIQUITTY_SOCKET`, `UBIQUITTY_SESSION` and
`UBIQUITTY_PANE`, and `ubq` asks that socket first. Those IDs are that
daemon's numbering, so they count only on a connection to that socket: beside
the app's control socket (a pane the app runs itself), over `-H`, or when the
named daemon does not answer, `here` names nothing and exits 8.

**Events.** The daemon keeps a bounded stream of typed events: a session
created or ended, a pane focused, a program status change, a command finished
with its exit status, a bell, a title, a foreground change, and consent
questions and answers. Each carries a cursor that is never reused. `events`
prints them as they happen; `--since C` first prints every event after `C`
the daemon still holds, and a `gap` record says how many it no longer does.
`wait` is one subscriber of the same stream: it sends one subscription and
sleeps until the daemon pushes the event it waits for, so it never polls.
`send` reports the cursor from before its text went in, so
`ubq wait --since <cursor> --for exit` cannot miss a command that finished
quickly.

`run` needs the shell to mark commands with OSC 133 (shell integration); it
returns when the `D` mark arrives, exits with the command's status, and
prints the command's output. A shell without the marks never reports an end,
so give `--timeout`.

## MCP server

`ubq mcp` runs a Model Context Protocol server over stdio (JSON-RPC
2.0, one message per line), so an MCP client (e.g. Claude Code) can drive the
terminal through tool calls. Register it as a stdio MCP server pointing at the
binary; it needs a running daemon and a capability token (both as above: the
`agent-token` file, or `UBIQUITTY_AGENT_TOKEN` in the registration's
environment).

Tools: `terminal_new`, `terminal_list`, `terminal_screen`, `terminal_changes`,
`terminal_run`, `terminal_send`, `terminal_interrupt`, `terminal_resize`,
`terminal_wait`, `terminal_wait_idle`, `terminal_read_last`, `terminal_kill`,
`terminal_screenshot` (a software-rendered PNG of the received model;
`terminal_screen` is preferable for text). A screenshot does not reproduce a
native frontend's font shaping or establish graphics parity across clients.
`terminal_screen` includes visible proportional rich text. Pass
`format: "structured"` to receive the capture JSON with block identities,
Unicode scalar ranges, styles, native line metrics and grapheme caret edges.
`ubq read --json` exposes the same snapshot in its `screen` result member.
Geometry is tagged with the placement serial and typography epoch; it must be
read again after reflow or zoom. The agent waits within its capture deadline
when the host is still receiving native geometry from the font-owning frontend.
`tools/list` is generated from the method registry
(`Source/Core/Protocol/Control/MethodRegistry.cpp`), which also holds every CLI
verb and every control-socket method, so a tool's description and input schema
are the ones the control socket's `describe` and `ubq api list --surface mcp`
report. Arguments are validated against that schema before
any policy check or attach: an unknown or misspelt argument, a wrong type or an
out-of-range value is a JSON-RPC `-32602` error whose `data` names the `kind`
(`bad_request`) and the offending `parameter`, never a call run without it.
`terminal_run` returns the command's output plus a trailing `[exit=N]` (with
OSC-133 integration); `terminal_read_last` returns the last command's output;
`terminal_changes` returns a line-diff since the previous read (token-efficient
polling). `terminal_run` / `terminal_send` honor the same policy + audit log as
the CLI. MCP refuses both `confirm` and `deny` decisions with an `isError`
result; `confirm` reports "requires approval". MCP has no self-approval argument.
A human can allow the command in the policy file or run it through the CLI
with `--yes`; an MCP caller cannot grant itself that approval. Sessions persist in the daemon: pass the id
from `terminal_new` as `session` to reuse a shell across calls.

Quick check from a shell:

```sh
printf '%s\n%s\n' \
  '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"x","version":"0"}}}' \
  '{"jsonrpc":"2.0","id":2,"method":"tools/list"}' \
  | ubq mcp
```

## Using it from code

`Source/Agent/AgentSession.{h,cpp}` is the client library both surfaces sit on:

```cpp
Ubiquitty::Agent::AgentSession session;
session.connect(/*cols=*/80, /*rows=*/24);   // attach to the local daemon
session.waitForFirstScreen(3000);            // HELLO_ACK + first checkpoint
session.sendInput("ls -la\n");               // type into the shell
for (int i = 0; i < 20 && !session.exited(); ++i)
    session.pump(100);                       // drive the socket, apply snapshots
std::string visible = session.screenText();  // the rendered screen as UTF-8
session.detach();                            // leave; the daemon keeps the session
```

`sessionID()` returns the daemon's stable id for the session — pass it to a
later `connect(cols, rows, id)` to reattach the same live session after the
agent disconnects between turns.

## Safety

`run` and `send` reach a real shell, so each is screened by a **policy** and
recorded to an **audit log** before any byte hits the PTY — the agent-side gate
(`Source/Core/Security/InjectionGate.h`). The macOS app's control socket is held
to the same file and writes to the same log (`Docs/control-socket.md`).
Two host-side gates sit above it, both consumed by this client:

- **Consent** — an agent attaching to (or killing) a session a human is using is
  held until the user approves in the macOS app. A denied/timed-out attach exits
  5 / 6; a denied/timed-out kill is reported and the connection stays up.
  Attaching to agent-owned sessions (`new`, no human attached) needs no
  consent; tree edits still require the separate exact-operation approval.
  `split`, labels/tags and the host tree methods exposed by `api call` wait
  for an attached person's Allow Once/Deny decision, even for an agent-owned
  workspace. Standing attach grants and `--yes` cannot approve those edits.
  Add-tab/split also need the requester's `new` token scope; the approver's
  authority is never lent to the requester.
- **Token auth** — the daemon requires a capability token from every agent by
  default: `$UBIQUITTY_AGENT_TOKEN`, else `<control-dir>/agent-token`. Mint one
  with `ubiquitty-host token issue --scope
  observe,drive,control,region,new|all [--expires N|Nm|Nh|Nd] [--label NAME]` and
  save the printed value. A missing/invalid/expired/revoked/out-of-scope token
  exits 7. A daemon started with `UBIQUITTY_REQUIRE_TOKEN=0` admits a tokenless
  local agent as though it held an `all` token; that switch exists for
  development and tests, and a remote peer still needs a token.

Policy is an ordered ruleset (`$UBIQUITTY_AGENT_POLICY`, else
`<control-dir>/agent-policy.conf`). A rule matches when its words appear
together, in order and side by side, as whole words of the command line,
wherever they stand in it; the first rule that matches wins, else the default.
The default is `confirm` (fail-safe) — nothing runs unattended without an
`allow` rule, or a `--yes` a person types at a terminal:

```
# agent-policy.conf
default confirm        # allow | confirm | deny  (used when no rule matches)
deny  rm -rf           # never run, even with --yes
confirm sudo           # require --yes
allow git              # a line with the word "git" anywhere runs freely
allow ls
```

Deny rules come first. `allow git` also matches `rm -rf ~ git`, and only the
`deny rm -rf` above it refuses that line; it does not refuse `rm -fr ~ git` or
`rm -v -rf ~ git`, which `allow git` lets run. An allow rule is as narrow as its
words are wherever they appear. A line an `allow` rule or `default allow`
passes is still held for confirmation when it carries a shell metacharacter or
a control byte; the [policy section of the ubq manual](../../Docs/ubq.md#policy)
lists them.

Every gated action is appended to the audit log (`$UBIQUITTY_AGENT_AUDIT`, else
`<control-dir>/agent-audit.log`):

```
2026-06-04T13:20:00Z session=3 run allow: git status
2026-06-04T13:20:05Z session=3 run deny: rm -rf /
```

**This is a speed-bump and an audit trail, not a sandbox.** Matching is by
whole words (so `deny rm` catches neither `/bin/rm` nor `--form`; choose
patterns accordingly), and the gate only inspects the command/text string — an agent can still assemble a
denied command keystroke-by-keystroke via `send`, or hide it behind `eval` /
shell variables / base64. Use it to make the default safe (the `confirm`
default) and to keep a record; for real isolation run the daemon itself under OS
sandboxing (a restricted user, a container, a VM).

Agent sessions are separate from any macOS-app session by default.

## Environment and exit codes

Environment:

| Variable | Effect |
|---|---|
| `UBIQUITTY_HOST_BIN` | Path to `ubiquitty-host`; lets the agent auto-start a daemon. |
| `UBIQUITTY_HOME` | Control dir (default `~/.ubiquitty`): socket, token store, policy, audit. |
| `UBIQUITTY_AGENT_TOKEN` | Capability token presented in HELLO (else `<control-dir>/agent-token`). |
| `UBQ_REMOTE_TOKEN` | With `-H`, the token presented to the remote host's daemon, issued there (`ssh host ~/.ubiquitty/ubiquitty-host token issue`); a local token means nothing to another host's daemon and is not sent. |
| `UBIQUITTY_AGENT_LABEL` | Self-identification shown in the consent prompt (default `ubq`; the MCP server uses the `clientInfo.name` its client sent in `initialize`, or `mcp-client`). A spoofable display hint, not the grant key; a process that sets it is an agent, and its `--yes` approves nothing. |
| `UBIQUITTY_AGENT_POLICY` / `UBIQUITTY_AGENT_AUDIT` | Policy ruleset / audit-log paths. |

Exit codes:

| Code | Meaning |
|---|---|
| 0 | success |
| 1 | no daemon, a daemon of another build or a full one, no snapshot, a carrier that closed, or a send that failed |
| 2 | a verb or operation that does not exist, a parameter its schema refuses, or a label the label rules refuse |
| 3 | the daemon refused a tree edit: no eligible attached person, denial/expiry, lost authority, an invalid target, or a capacity limit |
| 4 | blocked by policy (`deny`, or `confirm` without `--yes` from a person's terminal) |
| 5 | the person using the session refused (consent) |
| 6 | timed out awaiting that person's consent |
| 7 | the daemon refused the token, its scopes do not reach the operation (the refusal names the ones the verb needs; [the manual](../../Docs/ubq.md#scopes) tabulates them), or it holds no grant for a session no person is attached to approve |
| 8 | a target named nothing live, named more than one object, or `here` outside a pane, or the session ended before the attach |
| 9 | a wait or a command ended by its timeout |
| 10 | a `wait --for exit --since C` from a cursor the daemon's log has dropped events after |
| 11 | `run` observed an unmarked shell settle; JSON reports `outcome: "settled"` without a command status |

`run` exits with the command's own status when the shell reports it; `--json`
tells that apart from the codes above. The codes are `ExitCode` in
`Source/Agent/main.cpp`.

## Test

```sh
./Tests/Agent/smoke.sh        # isolated daemon + `ubq selftest`, end to end
./Tests/Agent/mcp_smoke.sh    # the in-process MCP server over JSON-RPC
./Tests/Agent/auth_smoke.sh   # token auth: no→exit 7, valid→0, bogus→7 (daemon with enforcement on)
./Tests/Agent/ubq_smoke.sh    # every verb, events, wait, status, targets and -H against a live daemon
```

## See also

- [Docs/ubq.md](../../Docs/ubq.md), the manual.
- [Docs/control-socket.md](../../Docs/control-socket.md), the macOS app's
  socket, served from the same registry.
- [Docs/agent-token-auth.md](../../Docs/agent-token-auth.md), the tokens the
  daemon checks.
