<!-- Copyright (C) 2026 Stanislav Yaglo -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Host management

The management surface is a separate window reached from Connect for a selected
host. Settings retains preferences; the management window displays live facts
owned by that daemon. macOS, iOS, and web frontends consume the same protocol.

## Authority

Host management has its own token scope. Control alone cannot enumerate grants
or tokens, disconnect other clients, or mint credentials. The uid-verified,
trusted local human bridge has owner authority; token clients must carry an
explicit management grant. An omitted agent label alone does not establish
that authority for a program the daemon does not recognise; the bridge is the
daemon's own executable, which any same-user process can start, so the
authority is the account's (see [origin classification](capability-origin.md#origin-classification)).
Every typed management Call is admitted by the shared
method registry at the operation boundary, with the existing Manage gate and
caller-role/principal rules. Termination retains its separate human-only
`host.session.terminate` authority; the five other administration methods do not
silently change their existing principal admission.

The host bounds requests and replies and returns a correlated error on refusal.
A refresh failure retains the last snapshot with a visible stale-state message.
No action runs against a different daemon incarnation after reconnect.

## Surface and operations

| Section | Facts | Actions |
| --- | --- | --- |
| Sessions | Command, folder, age, topology, adoption, sharing, retention | Terminate with the host and session named |
| Clients | Principal, label, transport, attachment time, session | Disconnect one attachment |
| Permissions | Capability, principal, origin, session, lifetime, granting actor and time | Revoke the selected grant |
| Tokens | Label, fingerprint, scopes, expiry, creation and last use | Issue with explicit scopes and expiry; revoke |
| Handoff | Session and host identity | Copy an authenticated-session locator or show its QR code |

Sessions come from the host listing tree. `host.management.list` supplies the
clients, grants, and token metadata; its schema describes those three collections.

Token listings contain fingerprints only. A freshly minted secret is displayed
once after an explicit Issue action and is never placed in a handoff URL, log,
or saved arrangement. Mutating actions require a current host identity and a
confirmation naming their target. Revocation must withdraw the selected live
authority and invalidate delayed work that depended on it; it must not revoke
an unrelated principal or silently preserve a revoked grant through reconnect.
See the scope and lifetime rules in [Host capability policy](capability-host-policy.md).

## Handoff

A handoff link is a locator, not a credential. It names the SSH authority,
daemon incarnation, and session. The receiver reviews the destination and uses
its own SSH credentials. A restarted daemon cannot satisfy a locator for the
old incarnation. Links, QR codes, and incoming-link review obey the same
[session handoff contract](session-handoff.md), independently of the device
or discovery mechanism carrying the locator.

## Validation

The acceptance matrix includes missing management scope, a Control-only token,
a forged or multiplexed request, malformed and oversized messages, expired
credentials, unknown targets, daemon replacement, and reply correlation.
Positive cases cover listing and each mutation against a private live daemon,
multiple clients, grant/token revocation, and handoff on an independently
authenticated receiver. UI tests verify empty, loading, stale, denied, and
successful states and confirmation targets.
