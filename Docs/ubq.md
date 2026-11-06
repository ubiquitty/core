<!-- Copyright (C) 2026 Stanislav Yaglo -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# ubq

`ubq` drives Ubiquitty's terminals from a prompt, a script or an AI agent. It
creates sessions, types into them, runs commands and returns their exit
status, reads the rendered screen as text, splits and labels panes, waits for
something to happen without polling, and streams what happens as it happens.

It talks to the `ubiquitty-host` daemon, which owns the shells and keeps them
alive between connections. `ubq` keeps nothing between invocations: every
command connects, does one thing and leaves.

Every verb is an operation in the method registry
(`Source/Core/Protocol/Control/MethodRegistry.cpp`), the same table that
describes the MCP tools and the macOS app's control socket. The registry gives
each operation a sentence, a parameter schema, a result schema and an example,
and `ubq` checks the arguments against the schema before anything reaches the
daemon. `ubq api describe <verb>` prints any of it.

- [Getting started](#getting-started)
- [Synopsis and global options](#synopsis-and-global-options)
- [Targets](#targets)
- [Verbs](#verbs)
- [Waiting and events](#waiting-and-events)
- [The registry: api](#the-registry-api)
- [JSON output](#json-output)
- [Remote hosts](#remote-hosts)
- [MCP](#mcp)
- [Policy](#policy)
- [Scopes](#scopes)
- [Exit codes](#exit-codes)
- [Environment](#environment)
- [Completion](#completion)

## Install

On macOS, choose **Ubiquitty → Install ubq CLI program…**. The installer links
`/usr/local/bin/ubq` to the app’s bundled program and asks for administrator
authorization if needed. It leaves an existing command from another location
untouched. Keep the app in Applications so the link continues to work after
updates. Remove the link to uninstall the command.

Linux downloads are available for [x86_64](https://ubiquitty.org/download/ubq/linux-x86_64)
and [ARM64](https://ubiquitty.org/download/ubq/linux-arm64). These static binaries
need no desktop libraries. Install the matching host separately and use the
same version of both. For example, after downloading the x86_64 binary:

```sh
mkdir -p ~/.local/bin
install -m 755 ubq-linux-x86_64 ~/.local/bin/ubq
```

Put `~/.local/bin` on your shell’s `PATH` if needed.

## Getting started

Build the daemon and `ubq`:

```sh
./Scripts/build-host.sh
./Scripts/build-agent.sh
```

Start a daemon, give `ubq` a token, and write a policy that lets it create a
session and run `uname` without asking:

```sh
export UBQ_BUILD_ROOT="$(python3 Scripts/build_root.py .)"
export UBIQUITTY_HOST_BIN="$UBQ_BUILD_ROOT/host/ubiquitty-host"
export PATH="$UBQ_BUILD_ROOT/agent:$PATH"

"$UBIQUITTY_HOST_BIN" --ensure-daemon
dir="${UBIQUITTY_HOME:-$HOME/.ubiquitty}"; mkdir -p "$dir"
(umask 077 && "$UBIQUITTY_HOST_BIN" token issue --scope all --label me > "$dir/agent-token")
printf 'default confirm\nallow 80x24\nallow uname\n' > "$dir/agent-policy.conf"
```

With `UBIQUITTY_HOST_BIN` set, `ubq` starts a daemon itself when none answers.
The daemon refuses a client without a token; `ubq` reads
`$UBIQUITTY_AGENT_TOKEN`, else the `agent-token` file in the control directory.
Every verb that types or creates something is screened by the
[policy](#policy) first, and under `default confirm` anything no rule allows
needs `--yes` typed at a terminal.

`run` learns where a command starts and ends from the shell's OSC 133 `C` and
`D` marks, which Ubiquitty's shell integration emits. Source it from the
startup file of the shell the daemon runs, `Resources/shell-integration/zsh.sh`
from `~/.zshrc` or `bash.sh` from `~/.bashrc`, before the daemon starts its
shells. A shell without it never reports an end; give such a `run` a
`--timeout`.

Then:

```sh
session="$(ubq --json new | python3 -c 'import json,sys; print(json.load(sys.stdin)["session"])')"
ubq run -s "$session" -- uname -a     # prints output; 11 if an unmarked shell settles
ubq read -s "$session" --last-command # reads the last command's output
ubq ls
```

Labelling, splitting, and other tree edits require an attached person with
Drive permission to approve the exact operation. Attach to the session in a
graphical client before issuing those commands. A token or `--yes` does not
replace this host-side approval. The commands above use the returned session
ID and do not require a label.

## Synopsis and global options

```
ubq [-H [user@]host] [--json] [--yes] <verb> [-s session] [-p pane] [options] [-- arguments]
```

| Option | Meaning |
|---|---|
| `-H`, `--host [user@]host` | Reach that host's daemon over ssh instead of this machine's. See [Remote hosts](#remote-hosts). |
| `--json` | Print the result as the JSON its result schema describes, and a failure as a JSON error on standard output. |
| `--yes` | Approve a `confirm` decision of the agent policy for this invocation. It counts only when standard input is a terminal and `UBIQUITTY_AGENT_LABEL` is unset: a confirmation is a person's answer, and an agent passing the flag would be answering for itself. A `deny` stays denied. |
| `-s`, `--session` | The session a verb acts on: an ID, a label, or `here`. |
| `-p`, `--pane` | The pane a verb acts on: an ID, a label, or `here`. |

The global options may stand anywhere on the line. A verb's other options are
its parameters, spelt as flags: `--timeout 5` sets `timeout`,
`--last-command` sets `source`. `ubq <verb> --help` (or `-h`) prints the verb's
usage and each option with the schema's words for it. Before any connection is
made, an option the verb does not take, an option missing its value, and a word
a verb takes none of are refused by name with exit code 2, and a value the
schema does not allow is refused with the schema's sentence:

```
$ ubq new -x
ubq: '-x' is not an option of new; `ubq new --help` lists them, and a word that begins with '-' goes after --
$ ubq wait -s build --for sleep
ubq: wait: 'for' must be one of "exit", "idle", "blocked", "text".
  wait — Waits until a session's command finishes, ...
  `ubq api describe wait` gives its parameters.
```

Words after `--` are never options, so text or a command that begins with `-`
goes there: `ubq send -s build -- -n`, `ubq run -s build -- ls -la`.

## Targets

A verb that acts on one session takes `-s` or `-p`:

| Written | Names |
|---|---|
| `session:3`, or `3` with `-s` | The session with ID 3. |
| `pane:5`, or `5` with `-p` | The pane with ID 5, and the session that fills it. |
| `build` | The session (with `-s`) or the pane (with `-p`) labelled `build`, compared without regard to case. |
| `here` | The pane this process runs in. |

IDs are typed (`session:`, `pane:`, `window:`, `tab:`) and never reused while
the daemon runs, so a stale ID names nothing rather than something else. A
label that names two objects is refused with every match named (in
`data.candidates` under `--json`) rather than guessed, and a target that names
nothing is refused; both exit 8:

```
$ ubq read -s web
ubq: read: No session is labelled 'web'.
```

Label an object with `ubq label` or `ubq new --label`. A label is at most 64
bytes, has no control characters, and cannot read as an ID or a number or be
`here`, because a target would resolve it as one.

### Context: here

The daemon tells every program it starts where it runs:

| Variable | Value |
|---|---|
| `UBIQUITTY_SOCKET` | The socket of the daemon that runs the pane. |
| `UBIQUITTY_SESSION` | `session:<id>` |
| `UBIQUITTY_WINDOW` | `window:<id>`, where the pane was opened. |
| `UBIQUITTY_TAB` | `tab:<id>`, where the pane was opened. |
| `UBIQUITTY_PANE` | `pane:<id>` |

The variables are set in the environment of the shell and inherited by
everything it runs, so no shell setup is needed. Inside such a pane, a verb
given neither `-s` nor `-p` acts on the pane it runs in, and `ubq` asks the
daemon at `UBIQUITTY_SOCKET` first:

```sh
ubq label tests          # label the session this shell runs in
ubq split down           # split this pane
ubq status               # this session's program status
```

The IDs are that daemon's own numbering. `here` names nothing on a connection
to any other: with `-H`, when `UBIQUITTY_SOCKET` names the app's control socket
(a pane the app runs itself, not one a daemon hosts), or when the daemon it
names does not answer and `ubq` reaches another one. Those cases exit 8 and ask
for `-s` or `-p` instead. The variables are identifiers, not credentials; the
daemon still checks the token on every request.

`ubq` connects to `UBIQUITTY_SOCKET` only when it resolves to a daemon socket
inside the control directory, and to any socket only when its listener runs as
the same user. Anything able to set the environment can name a socket, and
`ubq` sends its token in the first frame; a socket named elsewhere is ignored,
`ubq` finds this account's daemon by looking, and `here` names nothing.

## Verbs

Every verb prints text a person reads, or with `--json` the JSON its result
schema describes. The examples show both.

### ls

Lists the daemon's sessions: ID, label, pane, size, whether the shell is at
its prompt, the foreground process and the directory.

```
$ ubq ls
session:1	build	pane:1	80x24	idle	zsh	/src/app
session:2	-	pane:2	120x40	busy	vim	/src/app
```

```json
{"sessions":[{"session":"session:1","label":"build","window":"window:1","tab":"tab:1","pane":"pane:1","paneLabel":"","columns":80,"rows":24,"atPrompt":true,"foreground":"zsh","directory":"/src/app","title":"zsh"}]}
```

### tree

Prints the daemon's workspaces, windows, tabs and panes as an outline, one row
a line and two spaces a level, from the listing the app's own windows draw.
A row says what it is, its name, its labels and tags, its status, and how
long since it wrote. Words after the verb keep the rows that contain every
one, with the rows above them. With `--json` each row is an object with every
field named.

```sh
ubq tree
ubq tree production logs
```

A workspace the daemon could restore is listed with the rows its record
holds. `ls` lists only what runs.

`ubq` shows no theme. A `--json` row's `statusColor` is the colour the
palette a terminal starts with derives for its status; a window of the app
draws the same status in its own theme's colour. `statusSlot` names the
status in both.

### new

Creates a session running the login shell. A command after `--` is typed into
it, and `--columns` and `--rows` set its size (80 by 24 otherwise). Prints the
session's ID.

```sh
ubq new                         # session:4
ubq new -- npm start
ubq --json new                  # {"session":"session:5","pane":"pane:5"}
```

`--label` requests a separate, consent-gated edit after creation. If that edit
is refused, the session remains available and the optional command is not
started. Under `--json`, the error includes `data.createdSession`,
`data.createdPane`, `data.labelApplied`, and `data.commandStarted`; exit status
is 3. To label a new session, attach a person to it and issue `ubq label` with
the returned ID.

### run

Types a command line into a session, waits until its shell marks the command
finished, prints the command's output and exits with the command's status.
`--timeout SECONDS` bounds startup, input delivery, completion and capture
with one deadline. A timeout exits 9 and says the command may still be
running; `ubq read` reads its screen in a separate request.

Without shell integration, `run` waits for the screen to change and remain
still for half a second with the shell back in the foreground. It prints
that screen followed by `[settled; command status unknown]` and exits 11.
This is a settled observation, not a successful command status. The JSON
result contains `"outcome":"settled"` and no `exitStatus`; its `cursor` is
the last observed event position, not a fabricated command-finished event.

```sh
ubq run -s build -- make test && echo passed
ubq run -s build --timeout 60 -- ./slow-job.sh
ubq --json run -s build -- git status --short
```

```json
{"session":"session:1","exitStatus":0,"output":" M readme.md\n","cursor":41}
```

`exitStatus` is absent when the shell reported no status. For a marked
command, `cursor` is the event that ended it; see
[Waiting and events](#waiting-and-events).
The command's end is the first one the shell reports after it reports a
command starting (`C`) once the line is typed, so an end the shell writes for
something earlier, such as a prompt drawn late or a command still finishing,
is not taken for this command's.

### send

Types text into a session verbatim. `--enter` presses Return after it. Prints
nothing; the JSON result gives the bytes sent and the event cursor from before
the text went in. Nothing is typed until the daemon has said where its event
stream stands, so the cursor is always that position and never zero, which
would mean the whole log. `send` returns once the daemon says it has written
the text to the session; when it has not said so within 10 seconds, `send`
exits 9 and says the text may be written later or not at all.

```sh
ubq send -s server $'\003'               # Control-C
ubq send -p logs --enter 'tail -f app.log'
ubq --json send -s build --enter make    # {"sent":5,"cursor":40}
```

### read

Prints a session's text: the visible screen (`--screen`, the default), the
output of its last command as its shell marked it (`--last-command`), or the
last N lines of scrollback ending at the bottom of the screen
(`--scrollback N`, 100 when N is omitted).

```sh
ubq read -s build
ubq read -s build --last-command
ubq read -s build --scrollback 500 | grep -n error
```

```json
{"source":"last-command","text":"ok\n"}
```

`--last-command` exits 8 when the shell has marked no finished command.

### search

Finds a regular expression in all the history the host keeps of a pane, not
only what has been sent to a client: the registry's `pane.search`, the call
the app's remote windows make. History is the lines that have scrolled off the
screen; a line still on the screen is found once it scrolls off, and `ubq
read` prints the screen. Prints one match a line: the logical line's
number, then the first cell and the cell past the last, tab-separated. A line
soft-wrapped across rows is one line, and its number survives a reflow. The
search is case-insensitive unless `--case-sensitive` is given; `--from` and
`--to` bound it to lines `[from, to)`. No match prints nothing and exits 0.

```sh
ubq search -s build 'error: .*'
ubq search -p logs --case-sensitive --from 1000 -- 'FATAL'
```

```json
{"epoch":1,"matches":[{"line":1204,"startCell":0,"endCell":17}],"truncated":false}
```

At most 256 matches are returned; past that `truncated` is true, and a
person is told on standard error to narrow the range. The pattern is at most
127 bytes.

### split

Splits the target's pane to the right (the default) or downward and starts a
shell in the new pane. Prints the new pane's ID. The new pane's session belongs
to the token that split it, as one `new` creates does. An attached person with
Drive permission must approve the exact split. The daemon also checks the
requester's session access, token scopes, and tab capacity. No eligible person,
denial, missing authority, or a full tab refuses the split with exit 3 and
creates no session.

```sh
ubq split -s build down          # pane:6
ubq --json split -p pane:2 right # {"pane":"pane:7","session":"session:7"}
```

### kill

Ends a session by hanging up its shell. A session a person is attached to asks
that person first; a refusal exits 5, no answer in time exits 6. A session the
token did not create and holds no grant for, with no person attached to ask, is
refused with exit 7 and the daemon's reason.

```sh
ubq kill -s build                # {"session":"session:1","terminated":true} with --json
```

### label

Labels a workspace named with `-w`, a window with `-W`, a tab with `-t`, a
pane with `-p`, or otherwise a session, so later verbs can name it. An empty
label clears it. A label is checked before anything is sent, and the policy
screens the verb as `<label>`.

```sh
ubq label -s session:3 build
ubq label -p pane:6 logs
ubq label -t tab:7 tailing
ubq label -w workspace:1 client
ubq label -s build ''
```

```json
{"object":"pane:6","label":"logs"}
```

The daemon treats `ubq` as an agent regardless of who typed the command.
Labelling requires an attached person with Drive permission to approve that
exact operation. A standing attachment grant or ownership of the session does
not replace approval. No eligible person, denial, or missing requester scope
refuses the edit with exit 3 and leaves the label unchanged. Approval does not
grant the requester additional token scopes.

### tag

Replaces the tags on a workspace, a window, a tab, a pane, or otherwise a
session, named as `label` names them. Each word is a tag; no words clears
them. A tag is a marker a listing draws and searches by, not a handle: no
target resolves through one. An object carries at most 16, each at most 64
bytes. The policy screens the verb as `<tag>`, and the daemon decides who may
as it does for `label`.

```sh
ubq tag -W window:2 production billing
ubq tag -t logs noisy
ubq tag -t logs
```

```json
{"object":"window:2","tags":["production","billing"]}
```

### status

Prints the status records the session's programs reported with OSC 7501: one
line per record with its state (`idle`, `working`, `done`, `blocked` or
`error`), its ID, the program and its message, or `none`.

```
$ ubq status -s build
blocked	job	make	Continue?
```

```json
{"session":"session:1","state":"blocked","records":[{"id":"job","app":"make","state":"blocked","message":"Continue?","title":""}]}
```

### wait

Sleeps until something happens to a session, then prints it:

| `--for` | Returns when |
|---|---|
| `exit` | A command finishes (its shell's OSC 133 `D` mark). |
| `idle` | The foreground returns to the shell, or a program reports `idle`. Returns at once when the shell is already at its prompt. |
| `blocked` | A program reports `blocked`, for example waiting on a question. Returns at once when a record already says so. |
| `text:<s>` | The screen shows `<s>`. Returns at once when it already does. |

`--timeout SECONDS` bounds the wait (exit 9); a session that ends first exits 1.
`--since C` counts events after cursor `C`, so nothing that happened between an
earlier verb and the wait is missed. When the daemon's log has already dropped
events after `C`, `--for exit` cannot know whether the command finished among
them, and exits 10 with `data.kind` `gap` instead of waiting; the other
conditions are states, read again after the wait subscribes, and carry on.
The text output is the matching event as one line of JSON, or the condition's
name when the state already held.

```sh
ubq wait -s build --for exit --timeout 600
ubq wait -s server --for 'text:Listening on' --timeout 30
ubq wait -s build --for blocked && ubq status -s build
```

```json
{"matched":true,"for":"exit","cursor":42,"event":{"cursor":42,"kind":"command-finished","session":"session:1","exitStatus":0,"commandLine":"make","directory":"/src"}}
```

### events

Prints the daemon's events as they happen, one per line, until interrupted,
`--limit N` events have printed, or `--timeout SECONDS` passes. `--since C`
first prints every event after cursor `C` the daemon still holds (`--since 0`
prints all of them). `-s` or `-p` keeps one session's events.

```
$ ubq events --since 0 --limit 3
1	session-created	session:1	pane:1
2	foreground-changed	session:1	pane:1
3	command-finished	session:1	pane:1	0
```

A line is the cursor, the kind, the session and the pane, then what the kind
carries: a finished command's status and command line, a program status's
state and message, or the number of events a `gap` dropped.

With `--json`, each line is one event object; see the next section.

### api, mcp, completions

`api` is [the registry](#the-registry-api), `mcp` is [the MCP server](#mcp),
and `completions` prints [the completion scripts](#completion).

## Waiting and events

The daemon keeps a bounded log of what happens to its sessions. Each event has
a cursor, a number that only grows and is never reused, and a kind:

| `kind` | Fields beyond `cursor`, `kind`, `session`, `pane` |
|---|---|
| `session-created` | — (a session is labelled after it exists) |
| `session-ended` | `reason` |
| `pane-focused` | — |
| `program-status` | `state`, `message`, `id` |
| `command-started` | — (the shell's OSC 133 `C` mark) |
| `command-finished` | `exitStatus` (null when the shell gave none), `commandLine`, `directory` |
| `bell` | — |
| `title-changed` | `title` |
| `foreground-changed` | `atPrompt`, `process` |
| `consent-requested`, `consent-granted`, `consent-denied` | `agent`, `detail` |
| `gap` | `dropped`: how many events after the cursor the log no longer holds |

`wait` and `events` subscribe once, and the daemon pushes each event as it
happens; neither polls. To act and then wait without a race, take the cursor
from before the action:

```sh
cursor=$(ubq --json send -s build --enter 'make' | python3 -c 'import json,sys; print(json.load(sys.stdin)["cursor"])')
ubq wait -s build --since "$cursor" --for exit
```

A consumer that keeps the last cursor it saw resumes where it left off:

```sh
ubq --json events --since "$last" | while read -r line; do ...; done
```

A `gap` record says the daemon dropped events the consumer had not read; a
consumer that cares re-reads the state with `ls`, `status` or `read`.

The log is the daemon's, but each subscriber is shown only the sessions it
could watch by attaching: the ones it is attached to, created, or was granted
by a person. The daemon asks that of each event as it delivers it, so a grant
revoked after the subscription began stops that session's events at once. A finished command's line and directory reach it only when it
asks for command lines, as `ubq` does. Events it may not see are skipped, so
the cursors it sees need not be consecutive; a `gap` still means only that the
log dropped records.

## Rich text

A program in a session places styled text — families, sizes, weight, slant,
colour — into a rectangle of cells with OSC 7404, and asks first what fits
(Docs/rich-text.md). An agent drives it the way it drives any program: it
writes the program and runs it with `run`. This one asks how many rows a
heading and a paragraph need in a 40x6 block, reads the terminal's reply, and
places the block at that height:

```sh
cat > /tmp/notes.sh <<'SCRIPT'
#!/bin/bash
body='{\rtf1{\fonttbl{\f0\fswiss Helvetica;}}{\colortbl;\red0\green102\blue204;}\f0\fs36\b\cf1 Release notes\par\fs24\b0\cf0 The build now signs every artefact and verifies the chain before upload.}'
saved=$(stty -g)
stty raw -echo
printf '\033]7404;a=q:i=1:w=40:h=6;%s\033\\' "$body"
IFS= read -r -s -d '\' -t 2 reply   # ESC ] 7404 ; a=r:i=1:f=1:w=39:h=5:... ESC \
stty "$saved"
rows=${reply#*:h=}
rows=${rows%%:*}
printf '\033]7404;a=p:i=1:w=40:h=%s;%s\033\\' "${rows:-6}" "$body"
SCRIPT
ubq run -s notes -- bash /tmp/notes.sh
```

The reply's numbers are the viewer's: a session viewed from a phone is
measured in the phone's fonts. The body goes through `printf`'s `%s`, which
processes no escapes, so the RTF's backslashes arrive as written. Placing
`i=1` again replaces the block; `printf '\033]7404;a=d:i=1\033\\'` removes it.

## The registry: api

`api` reaches the registry's operations by name, with parameters given as
JSON, so a script needs no flag syntax:

```sh
ubq api list                         # the command line's verbs, one per line, with their sentences
ubq api list --surface control       # the macOS app's control-socket methods
ubq api list --surface mcp           # the MCP tools
ubq api list --surface all           # every operation, by its registry name
ubq api describe wait                # one operation whole: schemas, example, callers, names
ubq api call read '{"session":"build","source":"last-command"}'
ubq api call new @params.json        # parameters from a file
echo '{"for":"exit","session":"build"}' | ubq api call wait -
ubq --json ls | ubq api validate ls -    # does the output match ls's result schema
```

`describe`, `call` and `validate` accept any name an operation is reached by:
`wait`, `ubq.wait`, `send-text` or `terminal_run`, and a verb of two words by
both of them, as in `ubq api describe api list`. A name two surfaces share
is read as the command line's own first, so `ubq api describe split` describes
`ubq split` and not the control socket's `split`. `validate` checks a document
against the operation's result schema and exits 2 naming where it departs
(`data.path`), or prints `valid`. `api call` prints its result as JSON, and
refuses parameters the schema does not allow, naming the parameter:

```
$ ubq --json api call wait '{"for":"exit","fro":1}'
{"error":{"code":-32602,"message":"'fro' is not a parameter this method takes.","data":{"kind":"bad_request","parameter":"/fro","method":"ubq.wait"}}}
```

`ubq --json api list --surface all` is the whole table as JSON: every
operation's name on each surface, its description, and its parameter and
result schemas.

## JSON output

With `--json`, a result is one JSON object on standard output, shaped as the
operation's result schema says (`ubq api describe <verb>` shows it). `events`
prints one object per line instead.

A failure is one object on standard output in the shape the control socket and
the MCP server also use:

```json
{"error":{"code":-32000,"message":"run: the command did not finish before its timeout and may still be running; ubq read shows its screen","data":{"kind":"timeout"}}}
```

`code` is `-32602` for a refused parameter and `-32000` otherwise; the exit
status says which failure it was. `data.kind` is one word a program can branch
on: `bad_request`, `unknown_method`, `unsupported`, `invalid`, `no_target`,
`target`, `policy`, `consent`, `auth_denied`, `unavailable`, `timeout`, `gap`,
`ended`, `no_command`, `forbidden`, `refused` or `failed`. A refused parameter is named in
`data.parameter` as a JSON Pointer, and an ambiguous target lists its matches
in `data.candidates`.

`run` exits with the command's own status when the shell reports it. A script that
must tell a command's exit 4 from the policy's exit 4 reads `--json`: a result
with `exitStatus` is the command's, an `error` is `ubq`'s. A result with
`outcome` set to `settled` has no command status and exits 11.

## Remote hosts

`-H [user@]host` reaches the daemon on another machine through the same ssh
bootstrap the app's remote windows use. ssh's own configuration, keys and
agent apply; the remote host needs Ubiquitty installed, and the bootstrap starts
its daemon when none runs.

```sh
ubq -H build-box ls
ubq -H me@build-box new -- ./ci.sh
ubq -H build-box wait -s ci --for exit --timeout 3600
```

A token is issued by the daemon that checks it, so a local token is never sent
to another host, and `UBIQUITTY_AGENT_TOKEN` and the `agent-token` file count
only for this machine's daemon. Issue one there and give it to `ubq` in
`UBQ_REMOTE_TOKEN`; without it, or with one that host did not issue, `-H` exits 7
and says so:

```sh
export UBQ_REMOTE_TOKEN="$(ssh build-box '~/.ubiquitty/ubiquitty-host token issue --scope all --label laptop')"
```

Every verb works over `-H`. `here` does not, because the remote daemon numbers
its own panes; name the target with `-s` or `-p`.

## MCP

`ubq mcp` serves the Model Context Protocol over standard input and output, so
an MCP client can drive sessions through tool calls. Register it as a stdio
server whose command is the `ubq` binary with the argument `mcp`, with the
daemon running and a token in the `agent-token` file or in
`UBIQUITTY_AGENT_TOKEN` in the registration's environment.

`tools/list` is generated from the registry: each tool's name, description and
input schema are its entry's, the same ones `ubq api list --surface mcp`
prints, and a call's arguments are checked against that schema before
anything else. The tools are `terminal_new`, `terminal_list`,
`terminal_screen`, `terminal_changes`, `terminal_run`, `terminal_send`,
`terminal_interrupt`, `terminal_resize`, `terminal_wait`, `terminal_wait_idle`,
`terminal_read_last`, `terminal_kill` and `terminal_screenshot`.

```sh
printf '%s\n' '{"jsonrpc":"2.0","id":1,"method":"tools/list"}' | ubq mcp
```

The MCP server is held to the same policy as the command line, and an MCP
caller cannot approve its own `confirm`: a person allows the line in the
policy file instead.

For `terminal_run`, `timeout_ms` bounds command startup, input delivery,
completion, and capture after attachment. It does not bound the entire tool
call. Attachment has a separate first-screen wait: 10 seconds for a local host
or 30 seconds over SSH. A pending consent question can extend that wait.

## Policy

`run`, `send`, `new`, `split`, `kill`, `label`, `tag`, `selftest` and `imagetest`
are screened by the agent policy before anything is sent, and every decision is
a line in the audit log. The
policy is an ordered rule file (`$UBIQUITTY_AGENT_POLICY`, else
`agent-policy.conf` in the control directory). A rule matches when its words
appear together, in order and side by side, as whole words of the line, wherever
they stand in it, and the first rule that matches wins:

```
default confirm
deny rm -rf
allow git
allow make
allow <split right>
```

Deny rules come first. A rule's words can stand anywhere in the line, so
`allow git` also matches `rm -rf ~ git`, and only a `deny rm -rf` above it
refuses that line. Whole words means `deny rm` does not catch `/bin/rm`, and
side by side means `deny rm -rf` does not catch `rm -v -rf /` or `rm -fr /`.

A `#` that begins a word outside quotes starts a comment, as in the shell, so
`deny rm -rf   # never` names `rm -rf`.

The default is `confirm`: a line no rule allows needs `--yes` typed at a
terminal, and is refused with exit 4 without it. `--yes` from a process with
no terminal on its standard input, or one that sets `UBIQUITTY_AGENT_LABEL`,
answers nothing: a confirmation is a person's, so an agent's approval lives in
the policy file a person writes. `run`, `send` and the command given to `new`
are screened as the line typed; `new` itself as its size (`80x24`), and
`split`, `kill`, `label` and `tag` as a description in angle brackets
(`<split down>`, `<terminate session>`, `<label>`, `<tag>`), so a rule can name them.
`selftest` and `imagetest` make a session and type a line ubq writes, and are
screened as `new` is and as `<selftest>` or `<imagetest>`.

A line an `allow` rule or `default allow` passes still needs approval when it
holds any of `|`, `&`, `;`, `<`, `>`, `` ` ``, `$`, `(` or `)`, or a control
byte other than tab, DEL included. Each chains, pipes, redirects, substitutes or
expands, or makes the line discipline act, so the line runs more than the words
a rule matched: `git log; curl x | sh` matches `allow git` and is held. The hold
applies to the lines `run` and `send` carry and the command given to `new`; the
size and the bracketed descriptions are text `ubq` writes and are not held. A `deny` or `confirm`
decision stands as it is.

The audit log is `$UBIQUITTY_AGENT_AUDIT`, else `agent-audit.log` in the
control directory. For MCP actions, `allow` records the policy check. If the
attachment cannot proceed because the host or its first screen is unavailable,
an `incomplete` record names the same reason returned to the caller. A consent
or authentication refusal instead records `deny` with `attach-denied`.

An allow rule or `--yes` does not approve a tree edit. Splitting, labelling,
tagging, and other tree mutations require the attached person's separate
exact-operation consent and the requester's existing scopes.

The policy is a speed bump and a record, not a sandbox: it reads the line, not
what the shell will do with it. Run the daemon under an account or a container
that bounds what its shells can reach when that matters. The app's control
socket is held to the same file and writes to the same log
([control-socket.md](control-socket.md)).

## Scopes

The daemon admits a token only to what its scopes reach (see
[agent-token-auth.md](agent-token-auth.md#scopes-and-admission)). Every verb
that names a session or pane finds it over a control connection, so each of
them needs `control`:

| Verb | Scopes |
|---|---|
| `ls`, `tree`, `events`, `kill` | `control` |
| `status`, `search`, `wait` | `observe`, `control` |
| `read` | `observe`, `control`, `region` |
| `send` | `observe`, `drive`, `control` |
| `run` | `observe`, `drive`, `control`, `region` |
| `new` | `observe`, `control`, `new` |
| `label`, `tag` | `drive`, `control` |
| `split` | `drive`, `control`, `new` |
| `selftest`, `imagetest` | `observe`, `drive`, `new` |
| `api list`, `api describe`, `api validate`, `completions`, `mcp` | none of their own |

Some options need a scope more: `new --command` and `new --label` need
`drive`, and `wait --for text:<s>` reads the screen and needs `region`. `api
call` needs what the operation it calls needs. `ubq <verb> --help` and the
`commandLineScopes` of `ubq api describe <verb>` list a verb's scopes; `all`
carries every one of them.

A token short of one is refused with exit 7, and the refusal names the scopes
the verb needs, what the host said of the token, and the `ubiquitty-host token
issue --scope ...` that issues one carrying them:

```
$ ubq ls
ubq: ls: ls needs a token with the control scope; the host refused this one: This token carries observe,drive; a control connection, which lists and ends sessions, needs control. Issue one with `ubiquitty-host token issue --scope control` and give it to ubq in UBIQUITTY_AGENT_TOKEN or the agent-token file.
```

`run`, `send` and `new` ask the host what the token carries before they type
or create anything, so a token without `drive` or `region` is refused with
nothing typed, rather than after the command ran.

## Exit codes

| Code | Meaning |
|---|---|
| 0 | Success. |
| 1 | No daemon answered, the daemon is of another build or runs as many sessions as it allows, the session ended first, or the connection closed. |
| 2 | A verb or operation that does not exist, a parameter its schema refuses, or a label the label rules refuse. |
| 3 | The daemon refused a tree edit: no eligible person approved it, requester authority is insufficient, or a structural limit prevents it. A failed label after `new` also returns 3 with the created session identified in the result. |
| 4 | The agent policy refused: a `deny` rule, or a `confirm` without `--yes` from a person's terminal. |
| 5 | The person using the session refused. |
| 6 | The person using the session did not answer in time. |
| 7 | The daemon refused the token, its [scopes](#scopes) do not reach the operation, or it holds no grant for a session no person is attached to approve. |
| 8 | A target named nothing, named more than one object, or `here` named nothing on this connection; or the session it named ended before ubq attached. |
| 9 | A `wait` or a `run` reached its timeout, or the daemon did not confirm writing what `send`, `run` or `new --command` typed. |
| 10 | A `wait --for exit --since C` from a cursor the daemon's log has dropped events after. |
| 11 | An unmarked shell regained the foreground and its changed screen settled; the command's status is unknown. `run --json` reports `outcome` as `settled` without `exitStatus`. |
| other | `run` exits with the command's own status. |

## Environment

| Variable | Effect |
|---|---|
| `UBIQUITTY_HOME` | The control directory: daemon socket, token, policy, audit log. Default `~/.ubiquitty`. |
| `UBIQUITTY_HOST_BIN` | The `ubiquitty-host` binary, which `ubq` starts when no daemon answers. |
| `UBIQUITTY_AGENT_TOKEN` | The token presented to the local daemon, else the `agent-token` file. |
| `UBQ_REMOTE_TOKEN` | The token presented with `-H`, issued by the remote daemon. |

Neither token reaches anything `ubq` starts: a daemon it brings up, the shells
that daemon runs, and the ssh `-H` runs are all given the environment without
them.

| Variable | Effect |
|---|---|
| `UBIQUITTY_AGENT_POLICY`, `UBIQUITTY_AGENT_AUDIT` | The policy file and the audit log. |
| `UBIQUITTY_AGENT_LABEL` | The name shown to a person asked for consent. Default `ubq`. A process that sets it is an agent, and its `--yes` approves nothing. |
| `UBIQUITTY_SOCKET`, `UBIQUITTY_SESSION`, `UBIQUITTY_WINDOW`, `UBIQUITTY_TAB`, `UBIQUITTY_PANE` | Where this process runs; set by the daemon. See [Context](#context-here). |

## Completion

`ubq completions bash` and `ubq completions zsh` print completion scripts for
verbs and flags, and, where a daemon answers, live session IDs and labels. The
same scripts ship in `Resources/shell-integration` as `ubq.bash` and
`ubq.zsh`.

```sh
eval "$(ubq completions bash)"              # bash, in ~/.bashrc
ubq completions zsh > ~/.zfunc/_ubq          # zsh, with ~/.zfunc on fpath before compinit
```

`source <(ubq completions bash)` reads nothing in the bash 3.2 macOS ships,
which cannot source a process substitution; `eval` works in every bash.
