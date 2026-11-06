<!-- Copyright (C) 2026 Stanislav Yaglo -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Capability origin and principal identity

A terminal capability decision is
`CapabilityDecision check(Capability, CapabilityOrigin, std::string_view detail)`.
The capability names the effect; origin describes its trust class. The host adds
a verified principal identity when consulting its grant store. These are distinct
from the token's scope and from permission to attach to a particular session.

## Origin classification

`Daemon::originOf` resolves clients in this order:

| Client | Origin |
|---|---|
| QUIC peer, or no verified local process identity | Remote |
| Verified local peer with an agent label | Agent |
| Unlabelled local peer that runs one of the daemon's own binaries | Local |
| Any other verified local peer | Agent |

A path or an absent label alone does not establish Local authority. A claimed
agent label is display information, not authentication. Local socket access is
uid-gated; process identity and trusted-binary checks remain separate checks.

The daemon's own binaries are its app and its attach bridge
(`ubiquitty-host --attach`), which is a host executable relaying a stream it
does not parse. On macOS the daemon reads the code signature of what the peer
runs, once for each binary, from the kernel's account of the running code. A
release build knows its own binaries by that signature alone: the release
team's Developer ID on the released app or on a host binary, and on nothing
else the team signed, as `ubq`. An unsigned or ad hoc binary is an agent
wherever it lies. A development build, which runs unsigned or under a
development certificate, also knows them by identity: the trusted binary set,
the daemon's own executable and the interface bundled beside it or named by
its install. A platform that signs no code, Linux, knows them by identity
alone ([SignaturePolicy.h](../Source/Platform/Host/SignaturePolicy.h)). Any process running as the same user can
start the bridge, so the Local tier is not a boundary against same-user code:
such a process can equally issue itself a token or edit the files under the
control directory. What the trusted-binary check establishes is that a program
the daemon does not recognise is never Local by omitting its label. The agent
tier therefore gates programs that identify themselves or that the daemon
cannot identify; the boundary against hostile same-user code is the
operating-system account, not this classification.

Human/agent classification for consent is related but not identical.
`Daemon::isAgentClient` also recognizes an unlabelled, authenticated
interactive-user QUIC bootstrap token as a person. That person's capability
origin remains Remote; the identity mark does not relax effect policy.

### A client that reached the host over SSH

A client on another machine attaches by having sshd run the daemon's own
bridge, so by binary identity alone it is a Local peer. SSH authenticated
that person, so `originOf` keeps them Local: they are the owner for tokens,
scopes, roles, consent and person-or-agent classification. But they are not
on this host, so `Daemon::capabilityOriginOf` answers Remote for them, and
that is the origin their input stamps on a session, the origin a session's
output is gated at, and the origin every check asking whether a person is on
this host is judged by: a call restricted to clients on this host, and the
daemon-local peer test that sharing an adopted window and reclaiming a session
read. Remote's per-capability defaults then apply: a file read is refused, and
a clipboard write or a notification is asked of the person.

The bridge knows it was started by sshd from its own environment, since sshd
sets `SSH_CONNECTION` for every session it starts, a command channel
included. It then sends the `RemoteRelay` daemon-local operation
([Sync](sync.md#daemon-local-control)) before the client's first byte and
waits for the daemon's acknowledgement. The daemon applies the mark only from
its own bridge; any other same-user program's is acknowledged as not applied,
since Remote would give a program a remote owner's rememberable grants. The
bridge relays only on an applied mark. Anything else fails closed, with a
sentence for the person: a daemon older than the mark, which closes on it, is
not told apart from a current one that failed, so neither attaches the person
unmarked. An editor client run on the host from inside an SSH login is marked
the same way, since its person is just as remote.

The environment is the evidence, and it is inherited rather than proved. A
bridge started without the variables, from a terminal multiplexer begun at the
console or a service manager's helper, is not marked, so its person is gated
as Local although they reached the host over SSH. A bridge that inherits them
without SSH, from a session of a daemon the SSH bootstrap started, is marked,
which only lowers what it may do. Anything that can set the environment is
already the owner.

### Effects a host delivers to a client

A clipboard write or a notification a session's program makes is applied on
the device it reaches, so that device's person decides it, and is asked once.
A client whose host is on another machine says so in its `Greet`
(`AsksAboutHostEffects`, [Sync](sync.md#73-connection-and-control-channel-0)),
and `SyncClientCore` decides every effect such a host delivers under the
client's own configuration at the Remote origin, where a clipboard write or a
notification is asked of the person and a sound or an attention request is
refused. The verdicts are read from the client's settings when each effect
arrives, so a change reaches a connection that is already open.

The host asks no question of its own about a clipboard write or a notification
gated at the Remote origin for such a client: while the person a session's
effects go to asks about them, `HostCapabilityPolicy` forwards one it would
otherwise ask about, bound to that person, and the client judges it by its own
Remote verdict. A configured Deny on the host still refuses it there. An effect
gated at another origin keeps the host's question, since only the host knows
that origin's rule: an agent's grants are good for one use, and a person's "for
this connection" on their device must never cover an agent's later writes. The
client then judges what that answer released by its own verdict too. A
recipient on the host itself, or one that does not ask, gets the host's
question as before. So does a sound, which is emitted after its frame, when
someone else may be the recipient; a remote sound is refused by default on both
sides. The host's questions about what happens on the host, reading its
clipboard or its files or an agent's actions there, are its own and unchanged.

The client places its host from the carrier it built, as a
`Protocol::Sync::HostLocation`. Only `Session::isLocalTransportPlan` (this
machine's own bridge and bootstrap, which may name different binaries here,
its QUIC carrier on the loopback address) and a session shared into this
machine's daemon place a host on this machine, whose admission stands. An
embedder's carrier and every other command place it on another machine. The
web page keeps its host's admission, since the page is that host's code.

An effect asked about is held, one of each kind, and a newer one of a kind
replaces the one waiting; a host delivers a clipboard write that names several
buffers as one effect for each, with the same items, one right after another,
and only those join the write waiting, so one answer covers the buffers of one
write and no other. The frontend sees each as this
client's own capability question (`pendingConsentIsClientHeld`), one at a time,
the rest waiting their turn in the order they arrived (an effect that replaces
one keeps its place), and the answer releases or discards the held effect
without crossing to the host, which sets no time limit for it. An effect that
leaves unanswered, replaced or dropped with its connection, is reported
withdrawn and its question closes; none vanishes unreported. A held sound keeps
its own copy of a sample, since the sample's PCM is the connection's for one
call. Nothing is saved: the widest allow the prompt offers is "Allow for This
Connection", which covers later effects of the kind until the connection ends,
and a sound or an attention request, whose grants are good for one use, is
offered once only. A reconnect ends the connection, so it forgets those answers
and withdraws the held effects. A configured Deny outranks every answer.

A question the host asks waits its turn like any other: one that arrives
while another is shown is put in line, a few at most. A client answers only a
question it has shown, since an answer from any recipient decides the question
for all of them. One past the line, or one that arrives once the session can
present no more, is declined as unshown: it stays with the others asked, the
host does not ask that client it again, and it refuses the question at once
only when nobody else is still asked. The host also retires
every question it asked unanswered after thirty seconds and refuses for
itself.

An observer that presents no question leaves the effect unapplied. The
embedding `Session` in UbiquittyKit asks nothing, so a remote host's clipboard
writes and notifications do not apply there and its sounds are refused. The
GTK frontend runs only local sessions and attaches to no host, so no host
delivers it an effect; a frontend that attached without presenting the
question would apply none of the effects it holds.

## Input and program output

`Daemon::stampInputOrigin` records the input driver and its origin. An
interactive human admitted through the uid-gated SSH bridge, or through a
one-time QUIC token that bridge minted for the human, has the host account as
its input principal. A new bootstrap or resumed attachment for that account
keeps the same principal. Agent and control token identities remain distinct;
agent input alongside human input makes the session ambiguous and withholds
host-escaping grants. Two clients admitted as the same host account share that
account's authority, but the clipboard recipient is still the human client
that typed last. A local agent retains its grant key as its input principal.
The session's capability policy separately receives `grantKeyFor(client)` for
consent decisions. Input is encoded against the host's current terminal modes.

Before parsing PTY output, `Daemon::servicePTY` uses the most restrictive
origin of the session's attached, live clients with Drive scope. Observe-only
clients do not raise that origin. A session keeps a sticky
`driverOriginFloor`: once an Agent or Remote driver has raised it, later
disconnects cannot downgrade the origin of background output to Local.
A detached session retains its attribution.

This is conservative session attribution, not proof that a particular keystroke
caused a later escape sequence. Prompts must not claim stronger causation.
An output-side capability check reads `Terminal::currentCapabilityOrigin`;
wrapping bytes in passthrough sequences does not create a new trusted principal.
The tmux control-mode host carries the parent origin into its pane terminals.

Examples of execution boundaries include file access in the Kitty/POSIX paths,
clipboard and notification effects, DECUDK key programming, and replies through
`replyDisposition` / `respond`. A mode or screen update is not authority to
perform an external effect.

## Grant identity

`Daemon::grantKeyFor` uses `token:<digest>` for a token-bound client, otherwise
the verified peer executable path. An empty identity matches no standing grant.
The path fallback is mutable, so it cannot receive a persisted Always grant.

Capability grants use `(Capability, CapabilityOrigin, identity)`. A grant for
one principal, origin, or capability does not authorize another tuple. Session
grants retain their granting session for lifetime cleanup; OneShot grants are
consumed by an Allow-returning check. See
[host capability policy](capability-host-policy.md) for defaults and persistence.

Attach consent is a separate record. A capability grant does not itself admit an
agent to a session, and an attach grant does not remove the token's scope ceiling.

## Frontend and embedding contract

An embedder installs a `CapabilityPolicy` on each `Terminal` before processing
untrusted output, supplies the correct origin, and keeps the policy alive until
the terminal no longer references it. Reset preserves the policy and origin;
they are host-owned state, not checkpoint authority.

The host installs a policy per session and asks a person about what its tier
table prompts. The policies of the macOS, iOS, Linux, Web and Wscons frontends
hold no consent question for a terminal's own effects, so each answers
`defaultDecisionWithoutConsent` in `Source/PAL/Capability.h`: PAL's default for
the origin, with a Prompt refused. A Remote or Agent program there, such as one
in a tmux pane the macOS app dialled over SSH, keeps the feature, cursor and
colour replies it blocks on and the effects that stay on the grid, and is
refused every host-escaping and exfiltrating effect, the clipboard write and
notification a remote owner would be asked about included. The agent mirror
refuses every effect.

The embedding kits install no policy of their own. `UbiquittyKit::Session`
drives a terminal its embedder owns, and the embedder installs the policy on
it. A shell that builds `TerminalSurface`s, whose sessions no workspace model
makes, registers its clients once as the process default
(`WorkspaceSession::setDefaultTerminalClientInstaller`) and asks for them on
each surface (`RuntimeDrive.installDefaultClients`), as the iOS shell does in
`Source/Platform/iOS/SessionClients.cpp`.
With no policy, `effectAllowed` answers PAL's `defaultDecision` for the origin:
a Local terminal is allowed every effect, an Agent or Remote one is refused
host-escaping and exfiltrating effects, and a Prompt is a refusal. `effectGranted`
refuses every effect without a policy. A missing policy is therefore not a safe
sandbox for Local output.
The environment override described in the
[host policy reference](capability-host-policy.md#current-security-limitations)
also remains security-relevant.

Command approval in `Security::AgentPolicy` is separate from terminal capability
policy: its Allow/Confirm/Deny vocabulary is not a substitute for the host's
Allow/Prompt/Deny effect decision.

## Implementation and coverage

The classification and stamping live in `Source/Platform/Host/Daemon.cpp`;
the vocabulary and tier defaults live in `Source/PAL/Capability.h`.
Relevant tests include `origin_unlabeled_agent`, `origin_staleness`,
`agent_classifier_origin_unify`, `quic_user_identity`, `test_effect_gate`,
`test_high2_readexfil_gate`, for the frontends' own policies
`test_frontend_capability_policies`, for the client's own gate, `test_sync`,
`test_attach_over_carrier` and `test_remotebootstrap`, and, for the host
forwarding what its recipient asks about and the deadline on every question,
`consent_remote_effects`. Test names identify
coverage, not a claim that every platform or security property has been
validated.
