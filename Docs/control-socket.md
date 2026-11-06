<!-- Copyright (C) 2026 Stanislav Yaglo -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Remote-Control Socket (JSON-RPC)

A local unix-domain socket that drives the running macOS app. Use it to
list/focus windows, tabs, and panes; type into a session; read the screen or
selection; open windows/tabs; and set a title.

Implementation: the server is portable Core,
`Source/Core/Protocol/Control/Server.{h,cpp}` — the socket path, the owner-only
bind, the peer-uid check, the token, the agent policy and its audit, and the
method dispatch. It reuses the agent's JSON value/parser
(`Source/Core/Codec/JSON`) and the JSON-RPC envelope
(`Source/Core/Protocol/ControlRPC`), and the daemon's `~/.ubiquitty` path and
permission helpers (`Source/Core/Session/ControlPaths`) — not a parallel stack.
What a verb does to a window is the frontend's: the server calls a C-ABI host
table (`HostOperations`), and on macOS that host is
`Source/Platform/macOS/App/ControlHost.swift`, which drives the Swift shell's
windows (`TerminalWindowController`).

## Enabling (off by default)

The socket is **disabled by default** for security. Enable it in
`~/.config/ubiquitty/config.toml` and (re)launch the app:

```toml
[control]
enable = true
# token = "s3cret"                 # optional shared secret (see Security)
# socket_path = "/tmp/ubiquitty.sock"  # optional path override
# approve = true                   # a confirm verdict counts as approved (see Security)
```

With `enable = true` the app binds `~/.ubiquitty/control.sock` (or the
`socket_path` override) at launch.

## Transport & framing

- Unix-domain stream socket, `SOCK_STREAM`.
- **Newline-delimited JSON**: one JSON-RPC 2.0 request object per line, one
  response object per line. A connection may carry many requests; it is served
  until the client closes it.
- Requests follow JSON-RPC 2.0: `{"jsonrpc":"2.0","id":1,"method":"...","params":{...}}`.
  A request with no `id` is a notification (no response). Responses carry either
  `result` or `error` ({`code`,`message`}).

## Methods

The method registry (`Source/Core/Protocol/Control/MethodRegistry.cpp`) is the
source of what this socket serves, and of `ubq`'s verbs and its MCP tools: one
table of what Ubiquitty exposes to people and agents. An entry carries a
one-sentence description, a JSON Schema for its parameters and one for its
result, an example satisfying both, who may call it, and its route: how the
socket serves it and what the agent policy is asked first. The dispatcher looks
a request's method up in the registry and follows the entry's route; it holds
no list of method names of its own, so a method the registry does not route is
not served, and a new method needs an entry and, when its route is the host's,
the host verb that performs it. An entry's Sync name is the host's list of
what a Sync `Call` may name (`Docs/sync.md` §7.3): the host serves a call
only when the registry gives its method that name.

Read the table from a shell:

```sh
ubq api list --surface control        # every method this socket serves, with its sentence
ubq api describe send-text            # one method whole: schemas, example, callers
ubq --json api list --surface control # the same, as JSON with every schema
ubq api list --surface sync           # every method a Sync Call reaches
```

The socket describes itself from the same table:

- `list` names every method the caller may invoke, with its description.
- `describe` with `{"method": "<name>"}` returns an entry whole — schemas,
  example, principals, and the name each surface reaches it by. Any of those
  names is accepted: `send-text`, `terminal_run` and `run` all describe.

Parameters are validated against the entry's schema before anything happens: a
misspelt or unknown parameter, a wrong type, or a value outside its enum or
range is refused with no host call, no policy verdict and no audit line. A
`token` rides beside the parameters and is not one of them.
`Scripts/check-method-registry.py` refuses a dispatcher that compares a method
name of its own, a host verb without an entry, and an entry routed to a host
verb that the macOS host does not perform.

The registry routes these control methods:

| Method | Route | Params | Result |
|---|---|---|---|
| `ping` / `initialize` | Handshake | — | `{name, methods[]}` handshake (no main-thread hop). |
| `list` | ListOperations | — | `{methods:[{name, description, controlMethod?, commandLineVerb?, mcpTool?}]}`. |
| `describe` | DescribeOperation | `method` | The entry: `{name, description, params, result, example, principals[], controlMethod?, commandLineVerb?, mcpTool?}`. |
| `ls` | Host | — | `{windows:[{id,title,focused,remote,daemonSessionID?,tabs:[{index,windowID,title,selected,panes:[{id,focused,columns,rows,title}]}]}]}` |
| `focus` | Host | `window?`, `pane?` | Raise the window (and its tab); focus `pane` if given. |
| `send-text` | HostTypesText | `window?`, `pane?`, `text` | Type `text` verbatim into the target session's PTY. Screened by the agent policy (see Security). |
| `get-text` | HostReadsScreen | `window?`, `pane?`, `source?` (`"screen"`\|`"selection"`) | `{source, text}` — the visible screen (default) or the current selection. Screened by the agent policy as a screen read. |
| `new-window` | HostOpens | — | Open a window; `{window}`. Screened by the agent policy as `<new window>`. |
| `new-tab` | HostOpens | — | Open a tab in the key window; `{window}`. Screened as `<new tab>`. |
| `split` | HostSplits | `window?`, `direction?` (`"down"`) | Split the focused pane; `{window}`. Screened as `<split right>` / `<split down>`. |
| `set-title` | Host | `window?`, `title` | Pin a user title (empty clears the override). |

Errors take one shape everywhere: `{"code": <int>, "message": <sentence>,
"data": {"kind": <word>, ...}}`, where `kind` is one of `parse_error`,
`invalid_request`, `method_not_found`, `bad_request`, `not_found`, `ambiguous`,
`unauthorized`, `forbidden` or `internal`, and a refused parameter is named in
`data.parameter` as a JSON Pointer (`/text`).

IDs are stable handles: a **window** is its AppKit `windowNumber`; a **tab** is
its index within the window's tab group (each tab is itself a window with its own
`windowID`); a **pane** is the id its window numbers it with — the layout's pane
id, or the id a tmux or remote session gives it. Omit `window`/`pane` to target
the key window (else the first window) and its focused pane. A `pane` given
without a `window` is found in whichever window holds it, and refused when two
windows number a pane alike.

## Context in every pane

Every program the app's window model or the `ubiquitty-host` daemon launches is
told where it runs (`Source/Core/Session/LaunchContext.h`):

| Variable | Value |
|---|---|
| `UBIQUITTY_SOCKET` | The socket serving the pane: this control socket for a local pane while one is listening, or the daemon's socket for a hosted one. |
| `UBIQUITTY_SESSION` | `session:<id>`, the daemon session backing the pane; absent for a local pane. |
| `UBIQUITTY_WINDOW` | `window:<id>`, the window the pane was opened in; set only by the daemon, whose IDs `ubq ls` lists. |
| `UBIQUITTY_TAB` | `tab:<id>`, the tab the pane was opened in; set only by the daemon. |
| `UBIQUITTY_PANE` | `pane:<id>`, the pane itself. |

A tool inside a pane addresses `here` with these instead of asking which pane
it is in. The IDs are the numbering of whoever launched the pane, which is why
`UBIQUITTY_SOCKET` travels with them, and `ubq` takes `here` from them only on
a connection to the socket they came with. For a local pane the app names only
the pane: this socket takes `{"pane": <id>}` from `UBIQUITTY_PANE` and finds
the window holding it, refusing a pane ID that a local window and a remote
window's daemon both use rather than guessing. It addresses a window by its
window number and has no tab IDs, so the app does not set `UBIQUITTY_WINDOW`
or `UBIQUITTY_TAB`, which would name nothing here. A pane keeps its ID when it
moves. The variables are identifiers, not credentials; the socket's own checks
still decide every request. A launcher sets every one it knows and the PTY
layer drops any it did not set, so a context inherited from an enclosing pane
never reaches a child.
`TERM_PROGRAM` is unchanged.

`send-text`, `get-text` and `split` first ask the host which window and pane the
request means, so the gate and the audit line carry the ids `ls` reports. Every
command that touches the UI runs on the main thread; the accept/read loop runs
on background threads and marshals each call across.

## Security model

The control surface is guarded at five layers; with the gate off there is no
socket at all. The first four decide who may speak to the socket; the fifth
decides what a speaker may do, and it is the same layer `ubq` is
held to.

1. **Default off.** The listener binds only when `[control] enable = true`
   (`Preferences::Effective::controlEnabled`, default `false`). Never synced — it is
   a security setting and stays local.
2. **Owner-only socket.** The socket file is created `0600` inside the `0700`
   `~/.ubiquitty` directory; the bind runs under `umask(0177)` so it is
   owner-only even before the explicit `chmod`.
3. **Peer-uid check.** On every `accept`, the listener verifies the connecting
   peer's effective uid equals ours (`getpeereid`); a mismatch (the one case a
   `0600` file lets through is a root connector) is closed immediately. This
   keeps the surface uid-locked even if the socket is relocated to a shared path.
4. **Optional token.** When `[control] token` is set, every request must carry a
   matching `"token"` field (in `params` or at the top level) or it is rejected
   with `-32001 unauthorized` before any action runs.
5. **Agent policy + audit.** The peer at the socket is not the human at the
   keyboard, so `send-text`, `get-text`, `new-window`, `new-tab` and `split`
   go through the injection gate
   (`Source/Core/Security/InjectionGate.h`) — the rule file and audit log
   `ubq` uses (`$UBIQUITTY_AGENT_POLICY`, else
   `~/.ubiquitty/agent-policy.conf`; `$UBIQUITTY_AGENT_AUDIT`, else
   `~/.ubiquitty/agent-audit.log`; both read by the app at launch). A refusal
   is `-32002 forbidden`. The built-in default is `confirm`, and the socket has
   no human to ask, so with no rule file nothing is typed and nothing is read:
   `send-text` runs when a rule allows the text (`allow echo`, or
   `default allow`), `get-text` when a rule allows the screen read
   (`allow <read screen>`). `[control] approve = true` is the socket's twin of
   the CLI's `--yes`: a `confirm` verdict counts as approved, so a rule file
   with `default allow` also runs a chained or substituted line (an `allow`
   downgrades to `confirm` on shell indirection), and the audit line carries
   `[approved]`. It sits behind enable, the owner-only socket, the peer-uid
   check and the token, and does not override a Deny verdict. Every gated request
   is one line in the audit log, under
   the kinds `control-send(window=N pane=M)` and `control-screen(window=N
   pane=M)`, so the two doors read in one trail and the line names its target. The pane's `Terminal` keeps its `Local` capability origin; the
   screening happens before the bytes join the keyboard route.

   The CLI and control socket apply distinct input normalization rules. `send-text` is screened as
   the line it types — one trailing terminator removed, the way the agent's
   `run` verb appends its newline after screening — while the agent's `send`
   screens the raw bytes, so `ls\n` passes `allow ls` here and needs approval
   there. `get-text` has no agent-side twin: `terminal_screen` is not policy-
   gated, because an agent reaches a session only through the daemon's attach
   consent and token scopes, which the socket does not have; the socket's read
   is gated instead.

   AppleScript's `do script` uses macOS Automation authorization rather than
   this socket policy. That is a separate calling-application permission, not
   a grant to a same-uid socket peer.

## Example

```sh
SOCK=~/.ubiquitty/control.sock
# list windows
printf '{"jsonrpc":"2.0","id":1,"method":"ls"}\n' | nc -U "$SOCK"
# type a command into the key window (needs a rule that allows it — see Security)
printf '{"jsonrpc":"2.0","id":2,"method":"send-text","params":{"text":"echo hi\\n"}}\n' | nc -U "$SOCK"
```

A policy that lets a driver script read the screen and type lines that hold the
word `echo`:

```
# ~/.ubiquitty/agent-policy.conf
deny rm -rf
allow echo
allow <read screen>
```

`allow echo` passes any line with the word `echo` in it, wherever it stands, not
only an `echo` command. `rm -rf ~ echo` takes the deny rule above it, but
`rm -fr ~ echo` carries no shell metacharacter and is typed. An allow rule is as
narrow as its words are wherever they appear; the
[policy section of the ubq manual](ubq.md#policy) gives the matching rule.

A driver that types chained lines (`clear; ./bench; …`) needs the
allow to survive the indirection downgrade: `default allow` in the rule file
plus `approve = true` in `[control]`.

`Tests/Control/control_smoke.sh` drives the socket end-to-end (it SKIPs when no
socket is present, so it is safe to run unconditionally). Against an app with
no rule file it asserts the refusals; with `CONTROL_POLICY_ALLOWS=1` it asserts
the typed marker comes back on screen.

## Adding a method

A method is an entry in the registry with a route. A route the server answers
(`Handshake`, `ListOperations`, `DescribeOperation`) needs nothing more; a host
route needs the verb in `ControlHost.swift`'s `answer(verb:)`, under the
entry's control-method name. The screening a route performs is the route's, so
a method that types, reads or opens is held to the agent policy without a line
of its own in the dispatcher. A new route must state its authority checks,
parameter and result schemas, target resolution, refusal behavior, and tests
before it is exposed to callers.
