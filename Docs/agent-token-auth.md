<!-- Copyright (C) 2026 Stanislav Yaglo -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Agent tokens and session consent

Agent tokens identify an automation principal and set a ceiling on its authority.
They compose with session consent, message/method authorization, and terminal
capability policy; possessing a token does not bypass those checks.

A token is not protection against compromise of the same operating-system user,
who can read that user's credentials or control their processes. It provides
scoping, expiry, revocation, and distinct identities for otherwise similar clients.

## Issue and store a token

The administrative CLI is:

```sh
ubiquitty-host token issue --label <name> --scope <list> --expires <duration>
ubiquitty-host token list
ubiquitty-host token revoke <token-prefix>
```

`<duration>` is a whole number of seconds, or of minutes, hours or days written
`30m`, `12h` or `7d`; `0` or no `--expires` issues a token that does not expire.
Any other spelling is refused, and no token is issued.

Issuance prints the secret once. The store is `<control-dir>/tokens`, one line
per token with its SHA-256 digest, scope mask, expiry, and label, mode `0600`;
the plaintext is never written. Every edit takes the lock file
`<control-dir>/tokens.lock` first and rewrites the store whole. The control
directory is `~/.ubiquitty`, or `$UBIQUITTY_HOME` when it is set.
Public listings expose identifiers, not token plaintext. Host management
also has explicit administrative methods; ordinary Control scope cannot mint
Manage authority. See [host management](host-management.md).

For local connections, `AgentSession::resolveAuthToken` reads
`UBIQUITTY_AGENT_TOKEN`, then `<control-dir>/agent-token`. SSH connections use
only `UBQ_REMOTE_TOKEN`: a local host's token is not a credential for another host.
Keep plaintext token files private and avoid putting secrets in command histories.

## Scopes and admission

The stable scope bits are defined by `Scope` in
`Source/Core/Protocol/Sync/Scope.h`. `--scope` takes a comma-separated list of
the names in the second column; `token list` prints the first of them.

| Scope | `--scope` names | Permission ceiling |
|---|---|---|
| Observe | `observe`, `obs` | Attach and receive the screen plane. |
| Drive | `drive` | Input, focus, and viewport/geometry changes. |
| Control | `control`, `ctl` | Session listing and control operations, subject to their own gates. |
| RegionRead | `region`, `read` | Bounded region and last-command reads. |
| NewSession | `new`, `new-session` | Create sessions. |
| FileRead | `file` | Explicit file-transfer reads. |
| Manage | `manage` | Host administration, credentials, and other clients. |

`all` stands for `observe,drive,control,region,new`: it grants neither `file`,
which a token gets only by naming it, nor `manage`. Without `--scope`, a token
gets `observe,drive`. An unknown name, or a list that names no scope, is
refused, and no token is issued.

A refusal of a token outside its scope says what the token carries and the
scope it would need, and `host.describe` answers a connection with the scopes
its token carries, so a client can refuse an operation before it starts one.
`ubq` names the scopes each verb needs ([ubq.md](ubq.md#scopes)).

The per-kind role rules are in `Source/Core/Protocol/Sync/FrameGates.h`;
the connection checks direction, phase and channel, and Host operations also
have scope- and method-specific authorization. Control does not grant Manage.

`Greet.credential` carries the opaque token. The daemon hashes and constant-time compares it,
checks expiry, and records its identity and scopes. Authentication failures produce
typed `Bye` responses such as `TokenMissing`, `TokenInvalid` and `TokenExpired`.
[The wire reference](sync-protocol.md) specifies the
current bodies and version negotiation.

Token enforcement is on by default for non-Local peers. Local exemption requires
the uid-gated, kernel-verified trusted-binary identity, not a claimed label or
arbitrary local path. That identity is the daemon's own executable, its attach
bridge included, and the bundled interface; a process running as the same user
can start either, so the exemption guards against unrecognised and mislabelled
programs, not against same-user code acting as the user. Starting the daemon
with `UBIQUITTY_REQUIRE_TOKEN=0` disables that requirement for agents the daemon
verifies as same-user local processes: a tokenless one is held to the ceiling an
`all` token carries (never FileRead or Manage). A Remote peer still needs a
token. Only the exact value `0` disables it, and a host started that way must not
be treated as an authenticated deployment. Interactive QUIC bootstrap
tokens are a separate one-use credential minted through the trusted local
bootstrap path; their human identity and scope are not claims supplied by an agent.

## Consent to attach

An agent requesting a session it did not create needs an applicable standing grant
or approval from an attached person. Without a live human to ask, the daemon refuses
immediately with NoConsent. With a human attached, an explicit awaiting-consent
Welcome and the exact `ConsentStatus` Pending notify hold the attach;
only a human subscribed to that session can answer. A second human arriving before
the 30-second deadline receives the original question.

Until approval, the agent has an unbound, unapproved attachment and receives no
Call result, topology or pane records. The canonical bounded decoder handles
the greeting immediately; it never buffers Calls to replay after approval.
Incoming operations are refused before handler admission. Denial, disconnect,
expiry or token revocation erases the unapproved identity rather than retaining
it as resumable Lost authority. An exact approved status must be admitted before
the same record is promoted and any pane is bound; backpressure revalidates all
authority, target and deadline conditions on every retry.

Each connection has one immutable request. There are at most 32 pending requests
globally and eight per session, with one per verified agent identity/session.
The daemon's client cap is 64. Timeout denies the request; disconnect or restart
discards it. Requests are not persisted, and a human's arrival does not extend
the deadline or itself grant access.

`token issue --allow-always` records token-bound standing attach consent for a
headless host. This is an explicit administrative grant, not permission inferred
from the absence of a person. A token issued without it remains subject to normal
consent. Tokenless executable-path identities cannot receive persistent Always
consent.

## Expiry and revocation

Expiry is checked at authentication and the daemon services live token expiry.

The `tokens` and `consent` files are the authority, not any notification of an
edit to them. Before a daemon dispatches an inbound batch that could exercise
token-bound or consent-bound authority, and once per run-loop pass while such
authority is held, it compares each file's device, inode, size and
modification and change times with what its last reload read. On any
difference it reloads both. A revocation committed before a batch is dispatched
therefore refuses that batch on every daemon that shares the store, whether or
not it was told: the live connection receives `Bye` `TokenRevoked` and its
input does not reach the session. A store that cannot be examined (a `stat`
failing other than for absence), opened or read to its end, or that holds a line
in no form a writer produces, grants nothing: every token-bound connection is
refused, every retained token grant is retired, standing consent is withheld,
and a greeting presenting a token is refused until the store reads whole again.

CLI revocation updates the store, removes standing attach consent and asks
daemon owners to revalidate live and retained token grants through authenticated
local sockets, so an idle connection ends without waiting for its next frame.
A daemon accepts that request only from its own executable or the bundled
interface, so another build or install sharing the control directory is left
unconfirmed and applies the edit at its next check instead. The notifier never
reads PID files or sends process signals. Its scan and exchanges have one
two-second aggregate deadline, with at most 64 socket candidates and 4096
directory entries. Only matching reload replies count as acknowledgements. The
CLI reports unconfirmed candidates and an incomplete scan separately. A daemon
built without the store check keeps the grants it already holds until it
restarts; it still verifies every new greeting against the store.
See [daemon-local control](sync.md#daemon-local-control).
Consent revocation can
disconnect a live agent and withdraw session or persistent grants. Pending
questions are retired so a stale answer cannot recreate the withdrawn grant.
Session revocation also retires the agent's retained attachments holding that
session's pane, including disconnected ones and connections whose greeting
selected another session. An old resume secret cannot restore that pane; an
independent attachment to a different session keeps its own authority.

A closed connection alone does not prove that already queued PTY input, queued
effects, or retained resume authority have been cancelled. Explicit revocation
retires the selected attachments; cancellation of queued effects is not complete.
Revocation also cannot undo commands already executed or bytes
already handed to the operating system.

## Command policy and terminal effects

The MCP client applies `Security::AgentPolicy` to operations such as run, send,
interrupt, create, resize, and terminate, and records audit outcomes. An absent
or unreadable command-policy file retains its built-in Confirm default.
This client policy is not a host security boundary against a custom client.

Terminal effects use the independent
[capability policy](capability-host-policy.md). Its current environment override
and missing-policy behavior are security limitations even when token admission
and consent are configured correctly.

## Tests

Relevant suites include `token_auth`, `token_scope_parse`, `hello_auth_gate`,
`message_gate_matrix`, `consent_owner_gate`, `consent_pending_toctou`,
`revoke_disconnect`, `revoke_consent_actions`, `attachment_grant_lifetime`,
and the QUIC bootstrap/resume tests. They cover distinct admission, identity,
scope, and lifetime boundaries; a passing disconnect test is not a queued-input
cancellation test.
