<!-- Copyright (C) 2026 Stanislav Yaglo -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# How a client names and trusts a QUIC host

The QUIC carrier is a transport, not an authority. The SSH bootstrap
authenticates the host and issues the credential for a fresh QUIC connection.
The daemon still validates that credential or a retained
attachment's resume proof before accepting protocol traffic, and authorizes
each operation separately. The relevant code is
`Source/Platform/Host/HostQUIC.cpp`, `Source/Core/Protocol/Sync/QUICBootstrap.h`
and `Source/Core/Session/RemoteBootstrap.h`.

## What the daemon listens on

The daemon creates an owner-only Unix socket, `~/.ubiquitty/host-<id>.sock`
(mode `0600`, inside a `0700` directory; `UBIQUITTY_HOME` overrides the directory).
With `UBIQUITTY_QUIC=ON`, the default host build, it also opens a UDP listener
on all interfaces at an OS-assigned ephemeral port. It binds `[::]` with IPv4
mapped peers enabled, or `0.0.0.0` when IPv6 is unavailable. Reachability from
other networks depends on the machine's firewall and network routing.

For a new daemon, pass `--quic-port -1` to `--daemon` or `--ensure-daemon` to
disable UDP, or build with `-DUBIQUITTY_QUIC=OFF`. `--quic-port N` chooses a
fixed port; `0` requests an ephemeral port. Ensuring an already running daemon
does not reconfigure its listener. The SSH QUIC bootstrap starts a daemon with
the default listener when none is running.

The listener validates addresses with Retry tokens, caps connections at 64,
and expires unauthenticated connections ten seconds after their handshake.
Over every carrier together, one daemon holds at most 64 sessions and serves
at most 64 clients; a QUIC connection counts as a client once it is accepted. A
request for a session past the cap is refused, an attach with the `HostFull`
reason and a shared window's adopt with the `HostFull` rejection, and nothing
is evicted to make room. A connection past the client cap is not admitted.
A bounded authentication preamble must supply a one-time token, or a resume
proof for an attachment the client already holds
([Sync](sync.md), §6.9), before any Sync traffic is accepted. These protections
do not restrict the listener to loopback.

## Files in the control directory

The control directory is `~/.ubiquitty`, or `$UBIQUITTY_HOME` when it is set,
created with mode `0700`. `<id>` is the first 16 hex digits of the SHA-256 of
the host binary, so daemons of different builds keep separate files.

| File | Mode | Written by | Purpose |
|---|---|---|---|
| `host-<id>.sock` | `0600` | daemon | The Unix socket clients connect to. Removed when the daemon exits. |
| `host-<id>.sock.lock` | `0600` | daemon | Held with `flock` for the daemon's lifetime, so two starters cannot both take one socket path. Left in place. |
| `host-<id>.pid` | `0600` | daemon | The daemon's process ID. Removed when the daemon exits. |
| `host-<id>.log` | `0600` | `--ensure-daemon` | The detached daemon's standard output and error, appended across starts. |
| `host-<id>.sock.layouts` | `0600` | daemon | The workspace layouts the host keeps restorable. |
| `host-<id>.sock.background-events` | `0600` | daemon | The background event inbox a client's passive refresh reads. |
| `quic-key.pem`, `quic-cert.pem` | `0600` | daemon | The QUIC listener's self-signed RSA-2048 key and certificate, valid for ten years. |
| `tokens`, `tokens.lock` | `0600` | `token issue`, `token revoke`, host management | The [agent token](agent-token-auth.md) store and the lock taken before each edit. |
| `tokens.metadata` | `0600` | `token issue`, daemon | When each token and standing grant was issued and by whom, and when a token last authenticated a host-management request and from which program. Host management lists it; no authorization reads it. |
| `consent` | `0600` | daemon, `token issue --allow-always` | Standing attach consent, keyed by token. |
| `capability_grants`, `capability_grants.lock` | `0600` | daemon | Capability grants answered with Always, shared by every daemon using the directory, and the lock taken before each edit. |
| `clipboard-tools/host-<id>.sock/` | `0700` | daemon | The [clipboard commands](guides/selection-clipboard-and-search.md#clipboard-tools-in-hosted-sessions) its shells find first on `PATH`. |
| `shell-integration/` | `0700` | daemon | The shell integration scripts, when the daemon starts without `UBIQUITTY_INTEGRATION_DIR`. |
| `agent-token` | yours | you | The token `ubq` presents to the local daemon. |
| `agent-policy.conf` | yours | you | `ubq`'s command policy. |
| `agent-audit.log` | `0600` | `ubq` | `ubq`'s audit log. |
| `screenshots/` | `0700` | `ubq mcp` | PNGs the MCP screenshot tool saves under a caller-named leaf. |
| `control.sock` | `0600` | the app | The app's [control socket](control-socket.md), bound when `[control] enable = true` and no `socket_path` moves it. Left in place when the app quits; the next launch replaces it once nothing answers there. |

On a remote host, Connect also installs `ubiquitty-host` (mode `0700`) in
`~/.ubiquitty`, the executable the SSH bootstrap runs; see
[remote sessions](guides/remote-sessions-and-agents.md). It stays in
`~/.ubiquitty` when `UBIQUITTY_HOME` names another control directory.

A daemon loads its key pair once, when its QUIC listener starts, and generates
a new pair then if either PEM file is missing. Deleting the files therefore
replaces the certificate at the next daemon start, not before. The pin a client
holds is fetched over SSH with every bootstrap, which reads it from
`quic-cert.pem` at that moment. While the daemon that loaded the old pair keeps
running, a bootstrap after the files are deleted gets no QUIC credential, and
one after they are replaced gets a pin for a certificate the listener does not
serve, so the dial fails its pin check. Once that daemon has exited, the next
bootstrap starts one with the new certificate and carries its digest.

The daemon exits once it has had a session and its last session has ended:
the shell exited, or the session was ended with [`ubq kill`](ubq.md#kill) or
terminated from [host management](host-management.md). A daemon that has never
had a session keeps running. A daemon stopped by a signal leaves its socket
and PID file; the next start finds that nothing answers on the socket and
replaces it.

## The shape

1. The client connects over **SSH**, and SSH decides identity. The host key is
   judged by `Ubiquitty::Transport::verifyHostKeyFingerprint` — trust on first
   use, and a mismatch is refused and never re-pinned from.
2. Over that authenticated channel the client runs the host's own bootstrap
   command (`quicBootstrapRemoteCommand()`), which ensures a daemon and prints
   one line:

   ```
   quic_port=<decimal> quic_token=<hex> quic_cert_sha256=<hex>
   ```

3. The client dials `quic_port` with the **one-time token** as its credential
   and the **certificate pin** as its expectation of the peer.

So: SSH authenticates, QUIC carries. The token is a bearer credential minted for
this one dial; the pin is how the client knows the QUIC endpoint is the same
machine SSH just authenticated.

## Why the certificate is self-signed

There is no CA and no name to validate. The daemon generates its own key pair
because a certificate chain would answer a question nobody asked here — the
client is not trying to learn "is this really example.com", it already knows
which machine it is talking to, because SSH told it a moment ago.

What the certificate does carry is a **public key the client can pin**. The
SHA-256 of that certificate travels back over the authenticated SSH channel, so
an attacker who can see or modify QUIC traffic cannot substitute their own
endpoint without also having broken SSH.

## Why an unpinned token is refused, not tolerated

`parseQUICBootstrapLine` returns false unless **all three** fields are present
and well-formed, and `QUICBootstrapReader` turns a line that names a field but
does not parse into a terminal `Refused` rather than waiting for a better one.
The iOS bridge refuses again at `ubiquittyIOSQUICFactoryCreate`: a pin that is
not exactly `certificateSHA256Length`, or a token outside `maxQUICTokenLength`,
produces no factory at all.

The streaming reader holds at most 8192 bytes per output line, excluding the
newline. It refuses an oversized line before copying beyond that bound,
whether the newline arrives in the same delivery or a later one. Short login
banner lines are discarded independently before the bootstrap answer.

The reason is that a bearer token with no pin is stealable in the middle. A
client that dialled anyway would hand its one-time credential to whoever
answered, and the session would look normal. Refusing costs a fallback to SSH,
which works; dialling unpinned costs the token.

A truncated or padded pin is worse than no pin, because it *reads* as protected.
That is why the length is checked against Core's constant rather than being
adjusted to fit.

## Consent and what a session may do

Transport identity, person/agent identity, and capability origin are separate.
A QUIC peer has `CapabilityOrigin::Remote`, including a person using a phone.
An authenticated interactive-user bootstrap can establish that the attachment
is a person for presence and consent decisions; it does not promote its
output-side capability origin to Local.

The host must enforce the token scopes, attachment standing, and consent rules
for each operation. A frontend's name or a self-declared human kind grants no
authority. See [Capability origin](capability-origin.md),
[Host capability policy](capability-host-policy.md), and
[Agent token authentication](agent-token-auth.md).

## What the phone does differently, and why

Nothing about the trust model. The difference is mechanical: the desktop clients
run the bootstrap as a local subprocess (`ssh <host> '<command>'` through
`PAL::spawnPipedCommand`), and a sandboxed iOS app has no fork and no ssh
binary. It runs the **same command** over an SSH exec session on its own
authenticated channel, and parses the reply with the **same** Core grammar.

The command is named once, in `quicBootstrapRemoteCommand()`, so the two callers
cannot drift into asking different things.

## What is not here, and why

- **No pairing step.** There is nothing to pair: SSH access to the host is
  itself the pairing, and a person who can log in can already run a shell
  there.
- **No separate consent surface for QUIC.** The transport does not change what a
  session may do, so a prompt about it would be asking the reader to approve
  something that carries no new authority.
- **No certificate rotation protocol.** The pin is fetched fresh on every
  bootstrap over the authenticated channel, so a rotated certificate is picked
  up on the next connection with nothing to invalidate.

## Transport reporting

A failed QUIC dial may fall back to SSH. The frontend must report the transport
actually in use and, when a requested QUIC connection falls back, retain a
diagnostic reason. A functioning terminal grid alone does not establish that
QUIC was attempted or that its authenticated path succeeded.

Validation must cover malformed bootstrap output, missing or wrong-length
pins, host-key mismatch, refused or consumed tokens, bounded unauthenticated
connections, resume proof failure, and the reported fallback path.
