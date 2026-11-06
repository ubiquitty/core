<!-- Copyright (C) 2026 Stanislav Yaglo -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Security model

This page names who can reach a machine running `ubiquitty-host`, a client
attached to one, and the terminal itself, and the check that stands at each
door. The pages it links specify each check in full; this one is the map of
which check answers which party. An operation is authorized where it takes
effect, not where a client offers it: a menu item, a client-side policy or a
frontend's refusal is a convenience, never the boundary.

## The parties

| Party | What it starts with |
|---|---|
| The owner | The operating-system account the host and the app run as. |
| Another local user | An account on the same machine. |
| A same-user process | Any program running as the owner, including one inside a session. |
| An agent | A same-user program holding a [token](agent-token-auth.md) of some scope, or a remote program holding one. |
| A network peer | Anything that can send UDP to the host's QUIC port, or reach a client over the network. |
| An SSH peer | A person or program that can log in to the host as the owner. |
| A session program | Whatever runs in a pane, local or hosted, and the bytes it writes. |
| A host | The daemon a client attaches to, which may be compromised. |
| A web page | A page the owner's browser loads. |

## What each can reach

**Another local user reaches nothing.** The daemon's socket is mode `0600`
inside a `0700` control directory the daemon creates or repairs, and every
accepted connection is closed unless its kernel-reported effective uid is
the daemon's. A platform that cannot report one refuses every peer. The
app's control socket is off by default and, when enabled, is checked the
same way ([control socket](control-socket.md)). The store files in the
control directory are created `0600` ([QUIC trust](quic-trust.md#files-in-the-control-directory)).

**A same-user process is the owner.** It can start the attach bridge, which
is the daemon's own executable and therefore Local, edit the control
directory, or trace the owner's processes. Classification by binary
identity ([capability origin](capability-origin.md)) tells recognised
programs from unrecognised ones; it is not a boundary against code running
as the owner, and nothing here claims one.

**A host binary is admitted as a successor by who signed it.** A daemon hands
its sessions to a successor only over the uid-checked local socket, to a
process it identifies by the kernel's account of it, never by a PID or a path
the peer names ([host handover](sync.md#613-host-handover-live-sessions-survive-a-host-upgrade)).
On macOS a release's host admits a successor, and treats a peer as its app or
its bridge, only when the release team's Developer ID signs the code the peer
runs, as a host binary or as the released app; it reads the signature once per
candidate, and once for itself as it starts. An unsigned, ad hoc or other
signer's binary is refused as a successor and classified Agent, wherever it
lies, and a release host that cannot read its own team signature admits no
successor. A development build also accepts the file at its install path and
the files it resolved, which is what a same-user process could replace; Linux,
which signs no code, decides by those files alone. What the signature adds
against a same-user process is that a release's sessions do not move to a
binary the team did not sign; it is not a boundary against a process that can
already drive the owner's sessions.

**An agent reaches what its token's scope and session consent allow.** The
daemon decides at the operation boundary:

- the greeting names a target, and the token must carry Observe for a
  session, Control for the unbound control target and Manage for host
  management; a second greeting on one connection is refused;
- every frame kind has the roles that may send it, and input needs Drive;
- every `Call` names a registry entry whose roles, origin and scope the
  caller's authority must satisfy before its parameters are read;
- an agent attaching to a session it did not create needs an applicable
  standing grant or a person's answer, and is admitted to nothing while it
  waits.

The token store and the consent store are the authority for every
token-bound grant. A daemon checks them before it serves an operation that
such a grant admits, so an issue, revocation or withdrawn consent applies
on every daemon sharing the store; a store that cannot be read whole grants
nothing ([expiry and revocation](agent-token-auth.md#expiry-and-revocation)).
`all` never includes FileRead or Manage. Revocation cannot undo what an
operation already did.

**A network peer reaches the QUIC listener and nothing behind it.** The
listener binds every interface unless disabled. Before any state is kept it
validates the peer's address with a Retry; a connection then holds only its
authentication preamble, bounded in size and time, until it presents a
one-time bootstrap token or a resume proof bound to that connection's TLS
keys. Only the daemon's own executable or the bundled interface, over the
uid-checked local socket, can have a bootstrap token minted. The client
dials only with the certificate pin it fetched over SSH
([QUIC trust](quic-trust.md)). A network peer never reaches a client: no
shipped client listens.

**An SSH peer is the owner on that host.** SSH authenticates it, and what it
runs there runs as the owner. Ubiquitty adds no authority to an SSH login
and takes none from it. A client that attaches over SSH keeps the owner's
authority but is gated as a remote client for what its sessions may do
beyond the grid: the bridge sshd starts marks the connection, and the
sessions that client drives run at the Remote capability origin
([capability origin](capability-origin.md#a-client-that-reached-the-host-over-ssh)).
The client side of SSH is a boundary: a host name a
client turns into an ssh invocation must pass `Transport::validateSSHHost`,
which refuses a leading `-`, whitespace, quoting and shell metacharacters,
before it reaches ssh, and the command the far side runs is a fixed script
whose values are single-quoted.

**A session program reaches its own pane.** Core parses its output, and an
effect that leaves the pane, such as a clipboard read or write, a
notification, a window operation or a file read, is decided at the effect
by the [capability policy](capability-host-policy.md) for the session's
driver origin. A query's reply is sanitised or encoded so that it cannot
type a command. A program cannot raise its own origin: a session a remote
or agent driver has driven keeps that origin for later output.
An image's file or shared-memory transfer (kitty's `t=f`, `t=t` and `t=s`)
reads a file under the FileReadAccess capability. A Local-origin pane may
make one, an agent's pane asks the person first, and a remote pane is
refused. The reply tells the pane whether the temporary file or
shared-memory object exists, and its size. On a desktop where GLib falls
back to the freedesktop notification server and that server renders body
markup, a program's notification body can carry the formatting that server
allows.

**A host reaches what a client applies for it.** A client draws what the
host sends, but it does not take a host on another machine at its word for
the effects the host admitted. Each clipboard write, notification, sound and
attention request such a host delivers meets the client's own Remote-origin
verdict first: by default the client asks the person about a clipboard write
or a notification, as its own question that never crosses to the host and
whose widest allow lasts only for the connection, and refuses the rest. The
host asks nothing of its own about a clipboard write or a notification it
delivers to such a client, so the person answers one question, on the device
the effect reaches. The client
places its host
from the carrier it built, never from the host's word: only its own bridge
to this machine's daemon is this machine, and the web page, which its host
serves, keeps that host's decision. A client never runs a command or opens a
URL because a host said so; links open only from the person's click, under
the client's own link rules.

**A web page reaches nothing in a shipped build.** No shipped binary
listens for HTTP or WebSocket. The development bridge,
`Scripts/web-proxy.py`, admits an upgrade only when it names the bridge's
per-run secret path, its Host is the loopback listener and its Origin is a
page origin the bridge was given; the daemon's token check follows. The
page keeps its token in memory and renders terminal text to a canvas.

## What every door shares

- Untrusted input is bounded before it is buffered: frame bodies, queued
  input, pending consent requests, images, transfers and per-connection and
  global client counts each have a cap, and a peer past one is refused
  rather than accommodated.
- A refusal is fail-closed: a check that cannot be answered, a store that
  cannot be read, or a peer whose identity cannot be established is denied.
- A label, a client kind or a requested role can lower what a peer is
  granted and never raise it.
