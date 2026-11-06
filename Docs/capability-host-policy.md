<!-- Copyright (C) 2026 Stanislav Yaglo -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Host capability policy

The daemon installs one `HostCapabilityPolicy` per session terminal. Core
asks it at an effect or reply boundary using the capability and current origin.
The policy object, shared grant store, and consent queue are daemon-owned;
Core does not wait for a human or own an approval queue.

This policy is one authorization layer. Token scopes, session attach consent,
operation-specific authority, and client-side command approval remain separate.

## Decisions and defaults

`check()` returns Allow, Deny, or Prompt synchronously. Only Allow permits an
effect to run. Prompt records a deduplicated question without running the effect.
The host retains one exact OSC 52 clipboard write for approval as described below;
other discarded one-shot effects, including notifications, are not replayed.
A grant may permit a subsequent attempt.

`Source/PAL/Capability.h` owns the capability tiers and defaults:

| Tier | Local | Agent | Remote |
|---|---|---|---|
| HostEscaping | Allow | Prompt | Deny |
| ClipboardWrite and DesktopNotification refinement | Allow | Prompt | Prompt |
| ReadExfil | Allow | Prompt | Deny |
| LowRiskInfo | Allow | Allow | Allow |
| Benign | Allow | Allow | Allow |
| Rendering | Allow | Allow | Allow |

HostEscaping includes execution, file access, key programming, window changes,
sound, and clipboard writes. ReadExfil includes clipboard, screen-content, and
window-title reads. Ordinary device/cursor/mode/geometry queries are Benign;
suppressing them would break programs that wait for replies. Rendering describes
grid content, not permission to access host files.

Local programs already run with the user's OS privileges. The default local
Allow decision is not a sandbox. Remote clipboard writes and notifications can
be consented because their destination is the attached person's device; other
remote host-escaping capabilities default to Deny.

### Presentation state and external progress

OSC 9 progress state and percentage are bounded terminal metadata, replicated
for pane and tab presentation. Recording them does not perform an external
effect or grant permission. The capability coverage check classifies this path
as pull-state, alongside window-title metadata.

`ProgressIndicator` remains HostEscaping and Remote-denied by default. Any
consumer that writes progress to the system Dock or taskbar must check that
capability at its own effect boundary; the pull-state classification is not
an exemption for such a writer. Notification, attention, and focus operations
keep their separate capability checks.

## Configuration

The daemon resolves configuration at startup into an immutable
`CapabilityDecisionTable`. Each session gets a copy; an adopted session uses
the receiving daemon's table. The configurable keys are:

```toml
[capabilities.remote]
informational = "allow"
host_escaping = "deny"
read_exfiltration = "deny"

[capabilities.agent]
informational = "allow"
host_escaping = "prompt"
read_exfiltration = "prompt"

[capabilities.local]
informational = "allow"
host_escaping = "allow"
read_exfiltration = "allow"
```

This is an explicit configuration, not a verbatim statement of all defaults:
setting `remote.host_escaping = "deny"` also denies clipboard writes and
notifications, overriding their default Prompt refinement. Unset keys preserve
the defaults. Invalid values are reported and skipped. Benign and Rendering
have no configuration key.

A configured Deny is final and cannot be lifted by an old grant. Allow needs
no grant. Prompt can be satisfied by a matching grant.

## Grants and consent

The daemon owns a store keyed by `(Capability, Origin, identity)`. Identity is
the verified token digest or nonpersistent peer-path fallback described in
[capability origin](capability-origin.md).

| Lifetime | Behavior |
|---|---|
| OneShot | Consumed by one Allow-returning check. |
| Session | Recorded with a granting session and removed with that session or its scoped revocation. |
| Always | Persisted only for token-bound identities. |

HostEscaping and ReadExfil grants are forced to OneShot, except the remote
owner's ClipboardWrite and DesktopNotification grants. A requested Always
grant for a tokenless identity is reduced to Session.

Always grants live in `capability_grants` in the control directory, which
several daemons can share. Each grant or withdrawal edits that file's current
contents under its lock, one grant or one identity at a time, so a daemon
never writes back a grant another one withdrew. Before a cached Always grant
authorizes, the daemon checks that the file is still the one it last read
whole, and rereads it if not, so a withdrawal takes effect at every daemon's
next check. A file that cannot be read whole grants nothing. An Always answer
that cannot be recorded holds for the session, and a withdrawal that cannot be
recorded is reported and changes nothing.

For Prompt, the host queues one question per capability/origin/identity.
`sendCapabilityGrantRequests` sends it only to live, greeted, non-agent
subscribers of the session. If nobody can answer, the queue entry is resolved
so a later attempt can ask again. A response must name the host's pending
record; wire fields cannot substitute a different capability or principal.
An explicit allow creates a grant; denial resolves the question without an
allow grant. This does not create a permanent Deny rule.

For an OSC 52 write requiring consent, the host retains the first complete decoded
payload and its selectors, not terminal escape sequences to parse again. The
bound is one pending operation per session, at most 1 MiB, with a 30-second
consent deadline. Later copies do not replace that operation. Oversize input or
allocation failure refuses it. A question already owned by another session
cannot acquire this session's payload.

The pending write binds the verified principal, source client, session, exact
request, and primary human recipient. Only that recipient can answer. Before
delivery the host rechecks the source's live subscription and Drive scope, both
endpoints' token expiry, the current primary recipient, and the session's OSC 52
policy. Approval consumes the held operation and any OneShot grant before
dispatching its original selectors and bytes; a duplicate answer delivers nothing.
Denial, timeout, endpoint disconnect or revocation, session teardown, and pending
request eviction discard the retained payload. No valid recipient means no
delivery and no payload saved for a later arrival.

A capability prompt identifies a program in the session and separately shows
the principal used for attribution. The sticky output-origin floor is not
causal proof that the named principal produced that output.

Clipboard writes and notifications are held in a bounded host effect queue,
then sent only to the session's primary human driver: the most recent typing
human still present, or the sole eligible human if nobody has typed. An agent
or Observe-only viewer is not the destination. No eligible recipient means
the effect is discarded, not saved for a later arrival.

## Revocation

Consent revocation withdraws the relevant standing grants and pending questions.
StopAlways removes that identity's capability grants and disconnects its live
agents on the selected session. SessionGrant withdraws the session's attach grant
and matching session-scoped capability grants. Management revocation can name a
specific grant. Token revocation reloads token validity and closes matching live
token clients; expiry is also serviced by the daemon.

These mechanisms must not be described as rollback of commands or effects already
executed. Complete cancellation of queued input/effects and retained resume
authority is not currently guaranteed.

## Conformance-test policy

Capability decisions and clipboard confirmation have no process-wide
environment bypass. The conformance runners use normal policy by default.
Their explicit `--allow-clipboard-read` option selects an isolated configuration
containing only `[security] clipboard_read = "allow"`; it does not change the
person's configuration or relax capability tiers, token scopes, or consent
for other effects. The run records the selected configuration with its results.

An in-process fixture may install a test policy on its own terminal. That
policy must name the allowed capability and origin; neighboring terminals keep
their own policy.

## Current security limitations

When no capability policy is installed, `effectAllowed` answers from
`defaultDecision` in `Source/PAL/Capability.h` for the terminal's origin and
passes only its Allow: Local is allowed every operation, and Agent and Remote
are refused host-escaping and exfiltrating operations while their benign,
informational and rendering ones still answer. A Prompt from those defaults is a
refusal, including an OSC 52 write, because nothing is installed to hold the
request for consent. `effectGranted`, used by authoritative file-read paths,
refuses every operation without a policy. Embedders must install a policy
explicitly. The adapters on native, web, and agent surfaces do not provide
identical policy/consent behavior.

Terminal policy is not OS process isolation. An authorized command can use the
user's ordinary filesystem and process privileges. Same-user compromise is
outside the protection afforded by a local token file.

## Implementation and tests

Primary sources are `Source/PAL/Capability.h`,
`Source/Platform/Host/HostCapabilityPolicy.h`, and the admission, consent,
effect-delivery, and revoke methods in `Source/Platform/Host/Daemon.cpp`.

Focused coverage includes `test_effect_gate`, `test_capability_environment`,
`test_high2_readexfil_gate`, `Scripts/test-esctest-security.py`,
`capability_consent`, `capability_grant_scope`, `capability_prompt_cap`,
`consent_remote_clipboard`, `consent_remote_effects`,
`revoke_disconnect`, and `revoke_consent_actions`. These tests cover specific
boundaries; they do not establish the stronger queued-work cancellation guarantee.
