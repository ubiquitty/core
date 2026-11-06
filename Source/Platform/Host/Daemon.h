// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
// The ubiquitty-host session daemon: a terminal MULTIPLEXER owning N independent
// sessions (each a forkpty'd login shell driven into a Core Terminal) and the
// attached clients, each bound to one session. Survives client disconnects (tmux
// model) — a session stays live with zero clients; a later attach (HELLO echoing
// the session id) reattaches it and gets a fresh Sync checkpoint. A single poll()
// loop multiplexes the listening socket, every PTY master, and every client fd.
// The daemon exits when its last session's shell exits.
//
// Wire: each client speaks the Sync protocol framed over its socket. HELLO with
// sessionID 0 CREATES a session; non-zero reattaches (or creates if gone). The
// screen plane is Sync (Docs/sync.md), carried in Sync frames: one
// Protocol::Sync::HostConnection per client, one PaneSource per session.

#include "Core/DragDrop/Queue.h"
#include "Core/OSC/PublishedFaces.h"
#include "Core/Preferences/Preferences.h"
#include "Core/Protocol/Control/Consent.h"
#include "Core/Protocol/Control/EventStreamReplay.h"
#include "Core/Protocol/Control/MethodRegistry.h"
#include "Core/Protocol/Control/PasteUpload.h"
#include "Core/Protocol/Control/Reclaim.h"
#include "Core/Protocol/ControlRPC.h"
#include "Core/Protocol/Local/AdoptIngress.h"
#include "Core/Protocol/Local/Clipboard.h"
#include "Core/Protocol/Local/Handoff.h"
#include "Core/Protocol/Sync/Connection.h"
#include "Core/Protocol/Sync/Consent.h"
#include "Core/Protocol/Sync/HandoverBody.h"
#include "Core/Protocol/Sync/HandoverChannel.h"
#include "Core/Protocol/Sync/HostManagement.h"
#include "Core/Protocol/Sync/NativeAttachment.h"
#include "Core/Protocol/Sync/PublishCensus.h"
#include "Core/Protocol/Sync/StructureConsent.h"
#include "Core/Runtime/DrainPass.h"
#include "Core/Runtime/FrameCoherence.h"
#include "Core/Runtime/HandoverQuiesce.h"
#include "Core/Runtime/RemoteInput.h"
#include "Core/Session/AdoptTransaction.h"
#include "Core/Session/BackgroundEvents.h"
#include "Core/Session/EventLog.h"
#include "Core/Session/HostLayoutRecord.h"
#include "Core/Session/Identity.h"
#include "Core/Session/LaunchContext.h"
#include "Core/Session/LayoutTree.h"
#include "Core/Session/PaneActivity.h"
#include "Core/Session/SessionTree.h"
#include "Core/Session/WorkspaceIntent.h"
#include "Core/TerminalOperations.h"
#include "PAL/Process.h"
#include "PAL/UnixFileDescriptor.h"
#include "Platform/Host/AttachmentHandover.h"
#include "Platform/Host/CodeSignature.h"
#include "Platform/Host/GrantFileReader.h"
#include "Platform/Host/HandoverAdmission.h"
#include "Platform/Host/HandoverRequest.h"
#include "Platform/Host/HostCapabilityPolicy.h"
#include "Platform/Host/HostEffects.h"
#include "Platform/Host/LocalControl.h"
#include "Platform/Host/PollWake.h"
#include "Platform/Host/TrustedLocalBinaries.h"
#include "Platform/POSIX/HandoverLease.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Ubiquitty {
struct Terminal;
} // Core/TerminalOperations.h

namespace Ubiquitty {
enum class CapabilityOrigin : std::uint8_t; // PAL/Capability.h — the capability principal axis
namespace PAL {
class Stream; // PAL/Stream.h — the session's byte-stream transport (forkpty PTY)
}
}

namespace Ubiquitty {
namespace Host {

struct QUICConnection; // HostQUIC.h — an accepted ngtcp2 connection
class QUICListener; // HostQUIC.h — the daemon's QUIC data-plane listener
/// The shared file of Always capability grants, in CapabilityGrantFile.h.
class CapabilityGrantFile;
/// One intent against that file, in CapabilityGrantFile.h.
enum class CapabilityGrantOperation : std::uint8_t;

/// Why the listening socket could not be bound.
///
/// Each cause is known where it is detected and cannot be recovered there, so
/// the one that occurred travels out rather than collapsing into a bare false.
/// A caller that cannot name the cause reports a guess, and a guess about a
/// deterministic refusal reads to an operator as a transient fault.
enum class BindFailure : std::uint8_t {
    None,
    PathTooLong, // longer than sockaddr_un::sun_path holds — Darwin 104, Linux 108
    LockHeld, // another starter owns this socket path right now
    SocketCreateFailed,
    AddressInUse, // a live daemon answered on the existing socket
    BindFailed,
    ListenFailed,
};

/// A short operator-facing phrase naming `failure`.
[[nodiscard]] const char* describeBindFailure(BindFailure failure);

/// The longest socket path this platform can bind, in bytes excluding the
/// terminator. The limit is the platform's own: a literal would be wrong on one
/// of the two targets the host builds for.
[[nodiscard]] std::size_t maximumSocketPathLength();

/// Whether a daemon started with `UBIQUITTY_REQUIRE_TOKEN` set to `value`
/// requires a token from peers it cannot verify as its own binaries.
///
/// Only the exact value `0` turns the requirement off; an unset, empty or
/// other value leaves it on, so a typo never opens the host.
///
/// - Parameter value: The variable's value, or null when it is unset.
/// - Returns: False only for `0`.
[[nodiscard]] bool requiresTokenFor(const char* value);

/// Wakes the daemon's loop from a process tracker's watcher thread.
///
/// Shared with every registration, so a watcher that outlives the loop's last
/// look still writes to a pipe that exists. One byte stands for any number of
/// edges until the loop drains it.
struct ProcessTreeWake {
    PAL::UnixPipe pipe;
    std::atomic<bool> isPending { false };
};

class Daemon {
public:
    Daemon();
    ~Daemon();

    // Grants the QUIC bootstrap-token TTL test (Tests/Host/quic_token_ttl.cpp)
    // direct access to mint/consume/prune — the live QUIC wire path can't be
    // exercised without a QUIC build, so the expiry logic is unit-tested here.
    // Grants the fd-exhaustion negative (Tests/Host/daemon_start_arbitration.cpp)
    // the accept loop itself: with the descriptor table full there is no way to
    // reach that branch through the poll loop without exhausting the harness's
    // own descriptors first, so the loop is driven directly.
    friend struct AcceptUnderExhaustionAccess;

    friend struct QUICTokenTTLAccess;

    // Grants the mint-flood ownership negative (Tests/Host/quic_token_mint_flood.cpp)
    // direct access to mintQUICToken + m_pendingQUICTokens — the live QUIC wire path
    // can't drive a 64-mint flood from two distinct clients without a QUIC build, so
    // the cap-eviction-ownership invariant is unit-tested here.
    friend struct QUICMintFloodAccess;

    // Grants the uid-local mint-gate test (Tests/Host/quic_token_uid_local.cpp,
    // QUIC-ON only) direct access to m_clients, m_pendingQUICTokens, and
    // handleFrame, to prove only a uid-LOCAL client mints a QUIC bootstrap token
    // while a QUIC NETWORK peer (quicConnection set) mints nothing.
    friend struct QUICTokenUIDLocalAccess;
    // The real transport fixture supplies a loopback-bound listener while
    // retaining the production reactor and authenticated attach bridge.
    friend struct SyncTransportAcceptanceAccess;

    // Grants the deferred-drop contract test (Tests/Host/drop_defer.cpp) direct
    // access to m_clients + dropClient. The live-wire race (a peer's write failing
    // mid-handler so an inline erase shifts indices) can't be driven black-box on
    // Darwin — SHUT_RD doesn't EPIPE the peer, the first write to a closed peer
    // succeeds, and a closed peer's read-EOF is reaped by the index-safe service
    // loop first — so the invariant "dropClient never erases inline" is
    // unit-tested here.
    friend struct DropDeferAccess;
    // Drives authenticated clipboard assembly and observes its borrowed budget
    // across a deferred drop (Tests/Host/inbound_holds.cpp).
    friend struct InboundHoldsAccess;
    friend struct HelloWireVersionSkewAccess;
    friend struct HelloAttachOnlyAccess;
    // Grants Tests/Host/session_sized_text.cpp the session's terminal, to show
    // it holds a sized run as a run rather than as plain text.
    friend struct SessionSizedTextAccess;
    // Grants Tests/Host/slow_client_withhold.cpp a subscribed client parked
    // above the withhold line and the publish path, to show it is withheld
    // from rather than dropped and converges on a keyframe.
    friend struct SlowClientWithholdAccess;
    // Grants the Host tests that drive the Sync screen plane without the
    // poll loop (Tests/Host/ScreenPlaneAccess.h) sessions, subscribed clients,
    // the publish and the loop's screen step.
    friend struct ScreenPlaneAccess;
    // Names and classifies Session ownership without exposing it as a public API.
    friend struct SessionHandoverLedger;
    friend struct HandoverAccess;
    friend struct SessionHandoverFacts;
    friend struct SessionHandover;
    friend struct HostTableHandover;
    friend struct HostHandover;
    friend struct HandoverStartup;
    friend struct WorkspaceLayoutProjectionAccess;

    // Grants the output-origin staleness test (Tests/Host/origin_staleness.cpp)
    // direct access to m_clients, m_sessions, m_grantStore, originOf and
    // servicePTY's pre-read origin-recompute. The bug is a STALE Local stamp on
    // program OUTPUT while a Remote driver is attached — driving it black-box
    // needs a live remote attach + a program that emits DECUDK autonomously
    // mid-drain, which the consent/PTY harness can't synthesize deterministically;
    // so the recompute-then-stamp invariant is unit-tested here off a stub stream.
    friend struct OriginStalenessAccess;

    // Grants the QUIC slow-consumer output-cap test (Tests/Host/quic_output_capacity.cpp,
    // QUIC-ON only) direct access to m_clients, m_quic, and queueToClient — to prove
    // a stalled/desynced QUIC reader's queued-but-unsent bytes are counted toward the
    // per-client high-water cap (so they can't grow daemon memory without bound) and
    // that a saturating pendingBytes blocks an integer-wrap cap bypass.
    friend struct QUICOutputCapacityAccess;

    // Grants the pre-auth preamble-capacity test (Tests/Host/quic_preamble_capacity.cpp,
    // QUIC-ON only) direct access to m_quic, m_clients, and onQUICStreamData — to
    // prove an UNAUTHENTICATED QUIC peer's pre-auth preamble buffer is bounded at
    // 1 + maxQUICTokenLength, so a token-less byte flood is dropped before it is
    // accumulated (pre-auth remote-OOM). The path can't be driven black-box
    // without a live QUIC handshake, so the cap is unit-tested through a friend seam.
    friend struct QUICPreambleCapacityAccess;

    // Grants the pre-auth preamble fuzz harness (Tests/Host/fuzz_quic_preamble.cpp,
    // QUIC-ON only) direct access to m_quic, m_clients, m_pendingQUICTokens, and
    // onQUICStreamData — so libFuzzer drives the [u8 length][token] preamble state
    // machine of a fresh QUIC peer with one pending token seeded (successful auth
    // reachable), and asserts the pre-auth buffer stays within 1 + maxQUICTokenLength
    // while the peer is unauthenticated.
    friend struct QUICPreambleFuzzAccess;

    // Grants the aggregate-outbound-capacity test (Tests/Host/aggregate_outbound_capacity.cpp)
    // direct access to m_clients + enforceOutboundBudget — to prove the daemon-wide
    // total-queued-output cap (4x the per-client high-water = 64 MiB) drops newest-first,
    // so N clients can't each sit at the per-client limit for an N x 16 MiB unbounded
    // aggregate.
    friend struct AggregateOutboundCapacityAccess;

    // Grants the close-on-reap test (Tests/Host/quic_close_on_reap.cpp, QUIC-ON only)
    // direct access to m_quic, m_clients, and reapClient — to prove reapClient frees a
    // dropped QUIC client's connection (+ its toSend/toSendDatagrams, each bounded
    // but held) from the listener table immediately, rather than leaving it to the
    // 90 s idle timeout.
    friend struct QUICCloseOnReapAccess;
    friend struct MessageGateAccess;
    friend struct RevokeConsentAccess;
    friend struct HelloAuthAccess;
    friend struct GrantPersistenceAccess;
    friend struct ResizeFocusAccess;
    friend struct ServicePTYAccess;
    friend struct ScrollbackMaintenanceAccess;
    friend struct ScrollbackBackfillAccess;
    friend struct SyncMirrorAccess;
    friend struct PTYWritebackAccess;
    // PASTE_PUSH's landing + injection, unit-tested through a friend seam: a
    // black-box driver cannot hold the temp directory still or read the PTY
    // side of the injection.
    friend struct PastePushAccess;
    friend struct DaemonStartupAccess;
    // The session refused at each structural allocation in turn
    // (Tests/Host/create_session_refusal.cpp): m_shellPath, m_sessions, Session,
    // createSession.
    friend struct CreateSessionRefusalAccess;
    friend struct ConsentAnswerAccess;

    // Grants the origin fail-safe negative (Tests/Host/origin_unlabeled_agent.cpp) direct
    // access to Client + originOf + m_clients — proving an unlabeled verified-local peer
    // classifies as the GATED Agent tier (not Local/full-Allow) and that the
    // ubiquitty-host attach bridge stays Local.
    friend struct UnlabeledAgentAccess;

    // Grants the capability-consent grant test (Tests/Host/capability_consent.cpp)
    // direct access to m_pendingCapabilityGrants, m_grantStore, m_clients, and
    // canonical Call ingress — to prove consent.respond records a capability grant only from
    // the daemon's OWN pending record and only when the responder is a human on that
    // session (agent / wrong-session / deny all refused), without a live PTY session
    // that emits a host-escaping sequence at Agent origin.
    friend struct CapabilityConsentAccess;

    // Grants Tests/Host/consent_remote_clipboard.cpp the session, client and
    // grant-store seams to drive a remote principal's OSC 52 write through
    // servicePTY and the phone's answer through handleFrame.
    friend struct ConsentRemoteClipboardAccess;

    // Grants Tests/Host/consent_remote_effects.cpp the same seams, plus the
    // input-driver stamp and the grant store's records, to prove a remote
    // program's allowed clipboard writes and notifications reach the primary
    // human and no agent, and which grant lifetimes the daemon keeps.
    friend struct ConsentRemoteEffectsAccess;

    // Grants the capability-grant lifetime test
    // (Tests/Host/capability_grant_scope.cpp) direct access to m_grantStore,
    // m_consentQueue, m_pendingCapabilityGrants, m_clients, m_socketPath, and
    // handleFrame — to prove OneShot grants are consumed on first Allow, HostEscaping/
    // ReadExfil are forced to OneShot regardless of the human's requested scope,
    // LowRiskInfo + AllowAlways with a token identity persists and survives reload
    // while a non-token identity is degraded to Session, and dropSession clears
    // Session-scoped grants without disturbing others.
    friend struct CapabilityGrantScopeAccess;

    // Grants the agent-classifier negative
    // (Tests/Host/agent_classifier_origin_unify.cpp) direct access to Client +
    // m_clients + m_requireToken + isAgentClient + tokenScopeAllows + handleFrame —
    // proving a tokenless, capability-clean Remote (QUIC) peer classifies as
    // isAgentClient (not human), is denied every non-Local token scope, and is
    // rejected under require_token regardless of self-asserted HELLO caps.
    friend struct AgentClassifierOriginUnifyAccess;

    // Grants the QUIC user-identity test (Tests/Host/quic_user_identity.cpp) direct
    // access to Client, m_clients, m_requireToken, m_shellPath, m_sessions,
    // isAgentClient, createSession and handleFrame — to prove the bearer of an
    // unlabelled interactive-user token is a person while every other token shape
    // stays an agent, that a tokenless remote peer is still refused under
    // require_token, and that a SESSION_LIST entry carries the process cwd of an
    // OSC 7-silent shell and a workspace name cut to the wire's byte budget.
    friend struct QUICUserIdentityAccess;

    // Grants the revoke-disconnect negative (Tests/Host/revoke_disconnect.cpp) direct
    // access to m_clients, m_sessions, m_alwaysAllowedAgents, m_nextClientID,
    // m_nextSessionID, agentHasStandingGrant, findSession, and handleFrame — to prove
    // both StopAlways and SessionGrant revoke paths kick the live attached agent
    // (kickClient → `didOverflow`) and do not kick the human client.
    friend struct RevokeDCAccess;
    // Tests/Host/ssh_relay_origin.cpp: the bridge's SSH mark and the origin it sets.
    friend struct SSHRelayOriginAccess;

    // Grants the reentry-guard negative (Tests/Host/hello_reentry_guard.cpp)
    // direct access to m_tokenPath, m_clients, m_sessions, m_nextSessionID,
    // m_grantStore, and handleFrame — to prove a second HELLO on one connection
    // (the sticky-scope re-attach escalation) is rejected before it can bind the
    // client to a real, unattended session under a stale/absent token scope.
    friend struct HelloReentryGuardAccess;

    // Grants the consent-owner-gate negative (Tests/Host/consent_owner_gate.cpp)
    // direct access to m_shellPath, m_clients, m_sessions, m_nextSessionID,
    // m_alwaysAllowedAgents, findSession, and handleFrame — to prove a never-
    // attached, ungranted agent is DENIED (not silently let in) attaching to or
    // terminating a detached-but-alive session, while a standing-grant agent, the
    // session's own creator, and an already-attached agent are all unaffected.
    friend struct ConsentOwnerGateAccess;
    friend struct CapabilityPromptCapAccess;

    // Grants the consent-pending-identity TOCTOU regression negative
    // (Tests/Host/consent_pending_toctou.cpp) direct access to m_clients,
    // m_sessions, m_nextSessionID, and handleFrame — confirming a held (consent-
    // pending) agent cannot mutate its bound consentSessionID/consentRequestID
    // via a second HELLO (blocked by the reject-2nd-HELLO rule) or a second
    // TerminateSession (blocked by its own existing consentPending guard) while
    // the original decision is still outstanding.
    friend struct ConsentPendingTOCTOUAccess;
    // Drives the real, private serviceClient(index) over a socketpair fd
    // to prove a client kicked mid-batch (a duplicate HELLO) stops dispatching
    // the rest of that same already-decoded batch (trailing palette Facts).
    friend struct InboundKickGateAccess;
    friend struct DaemonClientIOAccess;

    // Grants the row-delta baseline corruption regression negative
    // (Tests/Host/rowdelta_baseline_corruption.cpp) direct access to Session,
    // m_nextSessionID, m_sessions, and broadcastSnapshot — to prove a trimmable
    // frame whose buildRowDelta bails on an unsafe row (after processing a
    // safe one) does not splice buildRowDelta's stale, pooled dirty-row cell
    // data into the baseline grid the session's encoder holds.
    friend struct RowDeltaBaselineAccess;

    // Grants the overflowed-client regression
    // (Tests/Host/overflowed_client_stays_unsynced.cpp) direct access to
    // Session, Client, m_nextSessionID, m_sessions, m_clients, and
    // broadcastSnapshot — to prove a client whose output hit the high-water
    // mark is never marked synced to the frame it never got.
    friend struct OverflowedClientStaysUnsyncedAccess;

    // Grants the foreground-probe-budget test (Tests/Host/foreground_probe_budget.cpp)
    // direct access to Session, Client, m_sessions, m_clients, m_nextSessionID, and
    // publishFrame — to prove probeForegroundIfDue probes the expensive
    // foregroundIsShell()/foregroundName() pair on the tick budget (immediately on a
    // foreground-group change, every frame while unsettled), never per frame, and
    // that a throttled session still updates the canonical facts after a change.
    friend struct ForegroundProbeBudgetAccess;

    // Grants the synchronised-output regression (Tests/Host/sync_output_withhold.cpp)
    // direct access to Session, m_sessions, m_nextSessionID and mayPublishSnapshot —
    // to prove the daemon withholds while an application composes under DEC mode
    // 2026, that the composed frame is deferred rather than dropped, and that the
    // block still expires on its own budget when the application goes quiet inside
    // it, and servicePTY — to prove a drain pass reads on past a short read to a
    // would-block or its capacity. The decision lives in the poll loop, which a
    // test cannot enter.
    friend struct SyncOutputWithholdAccess;

    // Grants the create-ownership-negative (Tests/Host/create_ownership_negative.cpp)
    // direct access to m_shellPath, m_clients, m_sessions, m_nextSessionID, m_grantStore,
    // findSession, and handleFrame — to prove (1) a same-identity agent reconnects to its
    // own created session; (2) a different identity is denied; (3) same identity, foreign
    // session is denied; (4) an empty-key (tokenless) agent never matches a grant and is
    // denied on reattach.
    friend struct CreateOwnershipNegativeAccess;

    // Grants the HELLO ssh-host test (Tests/Host/hello_ssh_host.cpp) direct access
    // to m_shellPath, Client, m_clients, Session, m_sessions, and handleFrame — to
    // prove a fresh HELLO's host string becomes the session's `ssh` argv, that an
    // argv-injecting host is refused with no session spawned, and that neither an
    // unclaimed capability nor a HELLO naming a session id launches anything. The
    // launched argv is only observable from the session's PTY, which the run loop
    // owns and a test cannot enter.
    friend struct HelloSSHHostAccess;
    friend struct HelloFieldBoundsAccess;
    friend struct AdoptInPartsAccess;

    // Grants the flushClient drain-cursor test (Tests/Host/flush_client_cursor.cpp)
    // direct access to m_clients, queueToClient, and flushClient — to prove a
    // backlogged client drains byte-exact through the consumed-bytes cursor over a
    // real small-SO_SNDBUF socketpair, that the cursor resets on a full drain, and
    // that the 16 MiB high-water keys on PENDING bytes (size - consumed), not the
    // raw buffer size.
    friend struct FlushClientCursorAccess;

    // Grants Tests/Host/backlog_drop_says_why.cpp m_clients, queueToClient and
    // isReapDue, to prove a client pushed past its high-water mark is sent a
    // Bye{OutputBacklog} after the frame it was partway through, and that the
    // reap waits a bounded time for that Bye to leave.
    friend struct BacklogDropAccess;

    // Grants Tests/Host/lost_drop_says_why.cpp isReapDue, to prove a
    // connection silent past the bound is dropped with a Bye{Silent} that a
    // client still reading receives, and is not reaped before it leaves.
    friend struct LostDropAccess;

    // Grants Tests/Host/quic_backlog_close.cpp the QUIC listener, the token
    // mint, queueToClient, isReapDue and reapClient, to prove a QUIC client
    // pushed past its high-water mark is closed with Bye{OutputBacklog} in
    // its application CONNECTION_CLOSE, read by the production client.
    friend struct QUICBacklogCloseAccess;

    // Grants Tests/Host/test_daemon_session_configuration.cpp createSession and
    // the Session it returns, to prove two sessions in one daemon run under
    // the configurations they were each seeded with — identity, scrollback,
    // the [security] OSC 52 policy and the sequence gate.
    friend struct SessionConfigurationAccess;
    friend struct DaemonCoreRulesAccess;

    // Grants Tests/Host/event_stream.cpp the event log, the delivery count and
    // the hooks a session's events are raised from, to prove a subscriber is
    // pushed each record once and resumes from a cursor without a gap.
    friend struct EventStreamAccess;

    Daemon(const Daemon&) = delete;
    Daemon& operator=(const Daemon&) = delete;

    // Bind the listening unix socket at `socketPath`, write `processIDFilePath`, and
    // record the shell + default geometry used to spawn sessions on demand.
    // No session is created until the first client HELLO. Returns false (and
    // tears down) if the socket could not be bound; -lastBindFailure names which
    // of the causes it was.
    bool start(const char* socketPath, const char* processIDFilePath, const char* shellPath, int columns, int rows);

    /// The preferences a session created from now on is seeded with.
    ///
    /// The daemon reads no process-wide preferences of its own: what it is
    /// handed here is copied into each session it creates, and that copy is
    /// the only configuration the session ever consults. A session already
    /// created keeps the copy it was seeded with. The `[capabilities.*]`
    /// verdicts are resolved here, once, into the table each new session's
    /// capability policy is constructed with.
    ///
    /// - Parameter configuration: The preferences to seed sessions from.
    void setConfiguration(const Preferences::Values& configuration);

    /// Names the directory every shell this daemon starts receives as
    /// `UBIQUITTY_INTEGRATION_DIR`.
    ///
    /// The documented rc line sources the integration only when that variable
    /// is set, so a shell started without it never reports its commands.
    ///
    /// - Parameter directory: The directory holding the scripts; empty leaves
    ///   the variable as the daemon's own environment has it.
    void setShellIntegrationDirectory(std::string directory);

    /// Sets the file-creation mask every shell this daemon starts receives.
    ///
    /// A detached daemon runs under an owner-only mask of its own; its shells
    /// are the person's and create files as the person's own mask says.
    ///
    /// - Parameter mask: The mask the daemon's launcher ran under.
    void setSessionFileCreationMask(std::uint32_t mask);

    // Why the most recent -start failed to bind. None before the first attempt
    // and after a successful one.
    [[nodiscard]] BindFailure lastBindFailure() const { return m_bindFailure; }

    // Bring up the QUIC data-plane listener on `port` (0 = ephemeral) and
    // bind its stream relay to this daemon's Session/Client machinery, so a
    // QUIC client speaks the same framed Sync protocol as an AF_UNIX client.
    // Call after start(); the run() loop then services it. Returns false
    // (reason in the host log) if the listener fails to bind. A no-op
    // returning false when built without -DUBIQUITTY_QUIC=ON.
    bool startQUIC(std::uint16_t port);

    /// Whether a predecessor handed this daemon its bound QUIC socket, which
    /// `startQUIC` serves on in place of binding `port`.
    /// - Returns: True until `startQUIC` adopts the socket.
    [[nodiscard]] bool hasInheritedQUICSocket() const { return static_cast<bool>(m_inheritedQUICSocket); }

    // Run the poll() loop until the last session's shell exits (or a fatal
    // error). Returns the process exit code (0 on clean exit).
    int run();

    // Remove the socket + processIDFile. Safe to call after a partial start.
    void cleanup();

private:
    /// A session Terminal's clock: the instant the daemon last read the
    /// session's program output, which the daemon ticks before each read is
    /// parsed. A command's marks are timed by when its bytes arrived, on the
    /// daemon's own clock, and never by another session's.
    class SessionClock final : public TerminalClock {
    public:
        /// Moves the clock to the instant the next bytes were read.
        ///
        /// - Parameter instant: The read's instant on the 60 Hz clock.
        void tick(Ticks instant) { m_now = instant; }

        /// Returns the instant of the read being parsed.
        ///
        /// - Returns: The last tick, or zero before the first read.
        Ticks now() override { return m_now; }

    private:
        Ticks m_now {};
    };

    // One multiplexed session: a forkpty'd shell + its Core Terminal. Stored
    // behind a unique_ptr so its address (and its non-movable PTY) is stable
    // as the session list grows/shrinks.
    struct Session {
        uint32_t id = 0;
        std::uint64_t createdUnix = 0;
        /// Where the session's pane was minted: the root ids of the workspace a
        /// session creates by being created, or the pane a verb minted for it.
        ///
        /// Known before the PTY opens, so the program it starts is told where
        /// it runs. Invalid ids for a session no host path placed.
        Ubiquitty::Session::Layout::RootIdentity placement;
        /// The client whose input most recently drove this session, by its
        /// stable `clientID`; zero before anyone has typed.
        ///
        /// The origin stamped beside it says what kind of principal drove the
        /// session, which is what the capability gate needs. This says which
        /// one, which is what a per-client resource needs — two people sharing
        /// a session hold two different pasteboards, and a command one of them
        /// typed must not be answered from the other's.
        uint32_t lastInputClientID = 0;
        /// The human whose input most recently drove this session, by
        /// `clientID`; zero before one has typed. The primary of
        /// Docs/sync.md §6.11: the one client a program's clipboard write or
        /// notification is delivered to. An agent's input does not move it.
        uint32_t lastHumanInputClientID = 0;
        /// When this pane's program last wrote and when someone last typed
        /// into it, on the host's wall clock, each stamped once per batch: a
        /// drain pass of the PTY, a pass of client input. What a listing's
        /// "output 2 min ago" and "typed 1 h ago" are read from.
        Ubiquitty::Session::PaneActivity activity;
        /// The pane's count of typed writes when its input was last stamped
        /// (``stampTypedInput``).
        std::uint64_t stampedTypedInputWrites = 0;
        /// The pane's count of a person's focus gains when its unseen
        /// attention was last cleared (``clearAttentionAPersonSaw``).
        std::uint64_t seenPersonFocusGains = 0;
        /// When the pane's records were last read from its terminal
        /// (``publishRecords``), on the loop's clock; zero before the first.
        std::uint64_t lastRecordsMilliseconds = 0;
        /// When records a frame or a change asked for inside the budget are
        /// owed, or zero. The loop reads them once when the window closes.
        std::uint64_t recordsOwedAtMilliseconds = 0;
        /// When the activity stamps last moved the facts' revision by
        /// themselves, on the loop's clock.
        std::uint64_t lastActivityStampMilliseconds = 0;
        /// The pane's structural generation as its workspace's structure
        /// last had reason to say it.
        std::uint64_t describedStructuralGeneration = 0;
        /// The terminal's activity generation the status set was last read
        /// at, so a pane whose reports stand is not copied again.
        std::optional<std::uint64_t> recordedActivityGeneration;
        /// How the session ended, once it has: read at the reap, where the
        /// child's exit status is known.
        Protocol::Sync::SessionEndFacts end;
        // Cached output-side capability origin: the most-restrictive origin among
        // attached drivers, refreshed each output drain. The gate reads it so a
        // program's autonomous side effects are attributed to the least-trusted
        // attached principal, not the stale last-input origin.
        Ubiquitty::CapabilityOrigin outputOrigin = Ubiquitty::CapabilityOrigin::Local;
        // Sticky origin FLOOR: the high-water mark of every principal that has
        // been able to drive this session — any Drive-scoped attendee and every input-
        // injecting principal ratchet it up, and outputOrigin is never allowed BELOW it.
        // A higher-origin driver (Agent/Remote) can spawn background work whose output
        // lands after it detaches; without the floor, servicePTY recomputes the origin
        // from the remaining (Local) attendees and DOWNGRADES the gate, so that agent-
        // spawned side effect gates as Local (Allow) — a detach-race TOCTOU. Sticky for
        // the session lifetime: a background job outlives both its driver and the shell
        // prompt, so no clear point tied to the command/prompt boundary (OSC-133 C→D)
        // is sound — the backgrounded write lands at prompt time, after the boundary.
        // This over-taints a co-driven Local human's own autonomous effects to Prompt,
        // which is the fail-safe direction.
        Ubiquitty::CapabilityOrigin driverOriginFloor = Ubiquitty::CapabilityOrigin::Local;
        /// The first input principal, including an empty key for an unknown sender.
        std::optional<std::string> firstInputPrincipal;
        /// Whether output has more than one possible input principal.
        bool hasMultipleInputPrincipals = false;
        // Declared before `term`: the Ubiquitty::Terminal holds a CheckedPtr to this policy, so
        // the policy must outlive the Terminal (members destroy in reverse order).
        Ubiquitty::HostCapabilityPolicy capabilityPolicy;
        // The Terminal's clipboard and shell clients, and the effects they hold
        // for delivery. Declared before `term` for the reason the policy is.
        HostEffectQueue effects;
        HostClipboardClient clipboardClient { effects };
        HostShellClient shellClient { effects };
        DragDrop::Queue dragDropQueue;
        std::uint32_t dragDropOwner = 0;
        std::uint64_t dragDropEpoch = 0;
        // Where the Terminal rings its bells; see installTerminalClients.
        HostEffectRoute effectRoute;
        // What the Terminal reads as its present; servicePTY ticks it.
        SessionClock clock;
        // What the Terminal measures proportional runs with: the faces the
        // clients viewing it draw runs in, which servicePTY refreshes before
        // each read. Declared before `term` for the reason the policy is.
        Ubiquitty::OSC::TextSizing::PublishedFacesMeasurer runFaces;
        // This session's own preferences, owned here: the identity its child
        // is stamped with and the scrollback it keeps come from this copy and
        // never from the process-wide instance, so two sessions in one daemon
        // can run under different configurations.
        Ubiquitty::Preferences::Values configuration;
        std::unique_ptr<Ubiquitty::PAL::Stream> stream; // the forkpty session (openPTY)
        std::unique_ptr<Ubiquitty::Terminal> term;
        /// The live human driver whose fonts answer rich-text measurements.
        /// Zero uses the Core estimate; other viewers keep their local layout.
        std::uint32_t richTextMeasurerClientID = 0;
        /// Full candidate elections, including revalidation before owed work.
        /// A settled service pass with unchanged offers does not elect again.
        std::uint64_t richTextMeasurerElections = 0;
        /// No measurer has been chosen since the session began. The first PTY
        /// pass chooses even when no client has offered typography.
        bool isRichTextMeasurerStale = true;
        /// The terminal changed since the last frame was published, so a frame
        /// is owed and no connection may encode from the terminal until it is
        /// taken.
        bool isFrameDue = false;
        /// Whether some driver held the pane's focus when the event log was
        /// last told, so the next time one takes it is recorded once.
        bool isDriverFocusRecorded = false;
        /// The geometry the pane's arbiter holds due, applied once the read
        /// pass ends (or before input that follows it), so a storm of
        /// viewports costs one reflow, one SIGWINCH and one checkpoint.
        std::optional<std::pair<int, int>> pendingGeometry;
        /// The owner's presented pixels changed the pixel geometry the program reads.
        bool isWindowSizeDue = false;
        /// The geometry changes applied, for the reap's journal line.
        std::uint64_t geometryChangesApplied = 0;
        /// Whose presentation was applied and when. A live human may present
        /// colours while the geometry owner is absent, retaining its pixels.
        struct PresentedApplied {
            Protocol::Sync::ID128 owner;
            std::uint64_t revision = 0;
            std::uint64_t panePixelsRevision = 0;
            std::uint64_t appliedAt = 0;
        };
        std::optional<PresentedApplied> presentedApplied;
        /// When an ownership change waiting out its window applies the new
        /// owner's pixels and theme.
        std::optional<std::uint64_t> presentedDueAt;
        /// Who presents for the pane while no driver owns its size: the
        /// client the daemon verifies as a person that presented last.
        std::optional<Protocol::Sync::ID128> presentedStandIn;
        /// The first change no published generation holds yet: frames taken
        /// while no client shows the pane advance no generation, so the next
        /// published one answers for their changes too.
        std::optional<std::uint64_t> unpublishedSince;
        // When the next frame may be taken and in which class: the rule the
        // local frontends publish by, fed this session's drain passes.
        Runtime::FrameCoherence coherence;
        // Where a remote client's input is written, and its encoder, which
        // reads `term`'s modes at each write (Docs/sync.md §6.7). Declared
        // before `screenSource`, which borrows the encoder, so they outlive it.
        std::unique_ptr<Runtime::ByteSink> inputSink;
        std::unique_ptr<Runtime::RemoteInputEncoder> inputEncoder;
        // The screen plane every attached client's connection is cut from:
        // the frames the coherence rule admits, captured once and encoded once.
        // Declared after `term`, which it reads, so it is destroyed first.
        std::unique_ptr<Protocol::Sync::PaneSource> screenSource;
        // How long each published frame waited, by the class the rule took it
        // in; observes the rule and never decides it. Read at the drain-pass
        // boundary in servicePTY and at each admitted publish.
        Protocol::Sync::PublishCensus census;

        // Agent-event emission state, so command and foreground records
        // fire once per change: the last OSC-133 completion generation emitted,
        // and the last foreground state reported.
        std::uint32_t lastEmittedCommandGeneration = 0;
        // Last OSC 633;E command-line generation retained by a command effect.
        // The host attaches the live command line only
        // when it advanced since the prior completion, so a completion with no
        // explicit command line carries an empty field instead of a stale one.
        std::uint32_t lastEmittedCommandLineGeneration = 0;
        // Last OSC 133 C generation reported to the event stream as a command
        // that started, so each start is recorded once and before the
        // completion that followed it.
        std::uint32_t lastEmittedCommandStartedGeneration = 0;
        bool lastForegroundIsShell = true;
        std::string lastForegroundName;
        /// Moves each time the foreground program changes, for the echo
        /// context a client's predictor reads.
        std::uint32_t foregroundGeneration = 0;
        /// The echo facts the last published frame carried, absent before
        /// one; input written under other facts makes a frame due.
        std::optional<Protocol::Sync::PaneEchoFacts> publishedEchoFacts;
        /// The authoritative status snapshot last reported to both event streams.
        /// Keeping it independently of journal retention prevents an evicted,
        /// still-blocked question from being issued as a new question.
        std::vector<OSC::ProgramStatus> reportedProgramStatus;
        std::uint64_t reportedActivityGeneration = 0;
        // Foreground-probe budget (TerminalRuntime's foregroundProbeIntervalTicks
        // twin): the records reuse the cached foreground answer between
        // probes, re-probing when the cheap foregroundProcessGroup() sample moves,
        // when the budget elapses, or while the last probe was unsettled (no real
        // foreground resolved yet — the pre-exec window — or one moving around
        // its name), so an early empty or torn answer never pins for a full
        // budget window.
        std::uint64_t lastForegroundProbeMilliseconds = 0;
        int lastForegroundProbeGroup = 0; // PAL::ProcessID (Stream.h is not included here)
        bool foregroundProbeSettled = false;
        // What the budgeted probe last read, for every reader of the
        // foreground: the event log compares it with what it last reported, and
        // the pane facts take their name and stand-in directory from it. A
        // reader that probed for itself spent the budget once per reader.
        bool isProbedForegroundShell = true;
        std::string probedForegroundName;
        std::string probedForegroundDirectory;
        // When a probe the budget held back is owed, or zero. A frame that
        // asks inside the budget is answered from the last reading, and if no
        // later frame comes that reading would stand for good: a `cd` whose
        // prompt lands inside the window would never move the folder. The loop
        // probes once when the window closes.
        std::uint64_t foregroundProbeOwedAtMilliseconds = 0;
        // A program can take the terminal without printing anything, and then
        // no frame asks the probe: `sleep` in the foreground read as the shell
        // at its prompt for as long as it ran. Where the stream's tracker
        // raises fork, exec and exit edges, its watcher thread sets this flag
        // and wakes the loop; where it raises none, a pane someone reads is
        // probed on Title::walkFallbackMilliseconds instead. Null until the
        // loop registers it on the session's stream.
        std::shared_ptr<std::atomic<bool>> processTreeEdge;
        bool isTreeEdgeTracked = false;
        // The first and the latest edge no probe has answered, zero while none
        // has gone unanswered. The probe waits for the tree to hold still for
        // Title::edgeSettleMilliseconds, and at most
        // Title::edgeMaximumDeferMilliseconds while it keeps moving.
        std::uint64_t firstUnansweredTreeEdgeMilliseconds = 0;
        std::uint64_t latestTreeEdgeMilliseconds = 0;

        // The process-derived inputs for the source's typed record.
        // The source alone owns its wire revision and per-reader delivery.
        // Only the process strings are cached here; paneHostFactsOf derives
        // geometry, activity, identity and presentation from their owners.
        Protocol::Sync::PaneHostFacts paneFacts;
        // What the walk behind `paneFacts` read, which gates the next one: the
        // walk is the costly half, run when the foreground can have moved.
        std::uint64_t lastPaneFactsWalkMilliseconds = 0;
        std::uint32_t paneFactsChainProcessGroup = 0;
        int paneFactsChainForegroundCount = 0;

        bool shellExited = false;
        // Which condition set `shellExited`, for the line printed when the
        // session is reaped. Carried rather than printed where it is detected
        // because the child's exit status is not knowable there: EOF on the
        // master means the replica's last descriptor closed, which a child
        // reaches by exiting and, separately, by closing its tty, so waitpid at
        // that instant has nothing to report. Empty until a condition fires.
        std::string exitReason;
        /// The last reply write was refused. Its bytes stay in `term`; the
        /// stream's POLLOUT or the pane's next drain moves them, never an
        /// immediate pass, which would find the same refusal.
        bool isReplyWriteRefused = false;

        // Stale-mouse guard (TerminalRuntime::m_mouseTrackingProcessGroup's
        // twin for daemon-hosted sessions): the foreground process group that
        // held the tty when mouse tracking switched on. servicePTY clears the
        // tracking modes once the shell is back in the foreground under a
        // different group — the enabler died without DECRST. 0 = unrecorded.
        int mouseTrackingProcessGroup = 0;

        // Consent gate: the agent identities this session admits without
        // re-prompting until it closes. Each is a verified grant key — the
        // creating agent's, and one per agent a person approved for this
        // session. One per principal, so approving one agent neither widens
        // nor drops another's grant, and each is listed and revoked on its
        // own. An agent with no verified identity (remote/QUIC, empty key) is
        // never entered and always re-prompts. Bounded so a person's repeated
        // approvals on one session cannot grow it without limit; at the bound
        // a further approval is not recorded and that agent re-prompts.
        static constexpr std::size_t maxSessionGrantIdentities = 32;
        std::vector<std::string> agentSessionGrantIdentities;

        /// Whether this session admits `identity` without a fresh prompt.
        ///
        /// - Parameter identity: A verified grant key; an empty one is never a grant.
        /// - Returns: Whether the identity holds a session grant here.
        [[nodiscard]] bool hasSessionGrantFor(std::string_view identity) const
        {
            return !identity.empty() && std::ranges::find(agentSessionGrantIdentities, identity) != agentSessionGrantIdentities.end();
        }

        /// Records a session grant for `identity`, within the per-session bound.
        ///
        /// - Parameter identity: A nonempty verified grant key.
        /// - Returns: Whether the identity holds a grant after the call; false
        ///   only when it is new and the set is already full.
        [[nodiscard]] bool addSessionGrant(std::string identity)
        {
            if (identity.empty())
                return false;
            if (hasSessionGrantFor(identity))
                return true;
            if (agentSessionGrantIdentities.size() >= maxSessionGrantIdentities)
                return false;
            agentSessionGrantIdentities.push_back(std::move(identity));
            return true;
        }

        // Re-homed via ADOPT_SESSION: the app owns the shell child (our
        // adoptPTY set childProcessID=-1, so the daemon never reaps it). Only
        // an adopted session may be RECLAIMed — handing a daemon-spawned shell's
        // fd back would orphan a child the daemon is responsible for reaping.
        bool adopted = false;
        /// A process-local publication receipt. A replacement daemon requires a
        /// fresh pin; its authoritative layout still refuses moving grouped panes.
        std::string sharedWindowFingerprint;
        /// How many of the window's oldest history lines an adopt left
        /// behind; the scrollback's first line says so.
        std::uint32_t historyLinesNotCarried = 0;

        /// The name a HELLO reaches this session by, empty when unnamed.
        ///
        /// A named session is a workspace of one pane while no workspace
        /// container exists, so the workspace a SESSION_LIST entry names is the
        /// session itself and its workspaceID is this session's own id.
        std::string workspaceName;

        /// The session ends with the last person on it, instead of outliving
        /// its clients the way a session does by default.
        ///
        /// Taken from the HELLO that creates the session: a later HELLO
        /// observes what the session is rather than restating it.
        bool endsWhenDetached = false; // bool-ok: mirrors the single HELLO flag bit helloSessionEndWhenDetached; an enum here would be a second spelling of one wire bit

        /// The `[user@]host` the session was launched against as a remote
        /// session, empty for one running the host's shell.
        ///
        /// Kept as the host string the HELLO named rather than as the argv
        /// assembled from it, so a record of the session holds something
        /// `Transport::validateSSHHost` can judge again when it is restored.
        std::string sshHost;

        /// The session was started from a record of its workspace after the
        /// host restarted, so its shell is fresh and its earlier screen gone.
        bool wasRestoredAfterHostRestart = false;

        /// The session's program was killed by a signal rather than exiting,
        /// which is how a crash or a machine shutting down ends it. Set at the
        /// reap, where the exit status is known.
        bool wasKilledBySignal = false;

        /// When the reap stops waiting for the child's exit status after the
        /// terminal hung up; zero until the reap first finds it running.
        std::uint64_t exitStatusDeadlineMilliseconds = 0;

        /// The directory the session last reported by OSC 7 when the host
        /// looked, so a change is noticed once rather than on every frame.
        std::string observedDirectory;

        /// The deadline at which an ends-when-detached session nobody is on is
        /// destroyed; zero while none is armed.
        ///
        /// A clean detach says the client will not be back, so its deadline is
        /// the moment it left; a lost transport gets a reconnect's worth of
        /// window. An arrival within the window disarms the deadline.
        std::uint64_t graceExpiresMilliseconds = 0;

        /// Whether the armed window waits for a person to come back.
        ///
        /// A session a person left is held open by a person returning to it,
        /// and an agent arriving is not the return it waits for; a session only
        /// agents have ever been on is held by any client. Meaningful only
        /// while `graceExpiresMilliseconds` is armed.
        bool isGraceAwaitingHuman = false;

        /// Temp files this session's PASTE_PUSHes landed, to unlink on teardown.
        ///
        /// The path is typed to the program as the paste, so the file has to
        /// outlive the push — the program opens it whenever it gets round to it.
        /// It must not outlive the SESSION: nothing else ever deletes these, so
        /// without this every successful paste leaves a file behind for the life
        /// of the host, holding whatever was pasted.
        std::vector<std::string> landedPastePaths;
        /// A private successor has the paths but may not unlink the holder's files.
        bool ownsLandedPastePaths = true;

        Session() = default;
        // A session whose capability policy decides by `capabilityDecisions`
        // for its whole life — the table is fixed at construction.
        explicit Session(const Ubiquitty::CapabilityDecisionTable& capabilityDecisions);
        ~Session(); // RAII closes the Ubiquitty::Terminal and stream
        Session(const Session&) = delete;
        Session& operator=(const Session&) = delete;
    };

    /// Holds synchronization state for one pane on a physical workspace
    /// connection.
    struct PaneSubscription {
        std::uint32_t sessionID = 0;
        /// The pane's handle on the client's Sync connection, 0 until bound.
        std::uint32_t screenHandle = 0;
        /// A physical reader reports its first streamed screen once per pane.
        bool hasReportedScreenAccess = false;
        /// The pixels this client draws this pane in, from its Viewport.
        std::optional<Ubiquitty::PixelGeometry> pixels {};
        /// Moves when this pane's pixel geometry changes.
        std::uint64_t pixelsRevision = 0;
        /// The last eligible rich-text offer observed on this subscription.
        /// Null means no offer; immutable shared ownership keeps pointer
        /// identity valid until the next comparison, without copying a table.
        std::shared_ptr<const RichText::TypographyFacts> richTextOffer {};
        /// The binding under which richTextOffer was published, or zero.
        std::uint32_t richTextOfferHandle = 0;
    };

    /// The live recipient and exact pane session used to present its question.
    struct ConsentRecipient {
        /// Whether the recipient is still asked.
        enum class Standing : std::uint8_t {
            /// It may still show the question to a person.
            Asked,
            /// It declined to show it. It stays listed, so it is not asked
            /// the question again, but nobody waits on it.
            Declined,
            /// Its connection ended. Its person may return while the
            /// attachment it was asked through can be resumed.
            Left,
            /// The host moved the attachment it was asked through to another
            /// connection, which a resume asks again on attaching.
            Continued,
        };
        std::uint32_t clientID = 0;
        std::uint32_t sessionID = 0;
        /// The attachment the question was asked through, which says whether
        /// a person who left may return.
        Protocol::Sync::ID128 attachmentID {};
        Standing standing = Standing::Asked;
    };

    struct Client {
        PAL::UnixFileDescriptor fileDescriptor;
        HandoverRequestReader handoverRequest;
        LocalControlReader localControl;
        std::unique_ptr<Protocol::Local::AdoptIngress> localAdopt;
        /// Nonzero only after the descriptor-free operation-3 readiness reply.
        std::uint64_t localHandoffCorrelation = 0;
        /// Pinned before readiness; every adoption phase rederives these facts.
        std::optional<Ubiquitty::Session::AdoptAuthority> localAdoptAuthority;
        bool isHandoffComplete = false;
        Protocol::Local::LocalClipboardRequestReader localClipboard;
        /// Nonzero only on the permanent local clipboard route, never a HELLO.
        std::uint64_t localClipboardCorrelation = 0;
        Protocol::Local::LocalClipboardKind localClipboardKind = Protocol::Local::LocalClipboardKind::Types;
        /// Borrows the MIME owner retained by localClipboard through reply drain.
        std::string_view localClipboardMIME;
        /// Set only after the local accept verifies the kernel peer's owning uid.
        bool hasOwnerLocalCredentials = false;
        /// Set when this daemon's own bridge marked the connection as relaying
        /// a client that reached the host over SSH. The person is the owner,
        /// whom SSH authenticated, but is not on this host.
        bool isRelayedFromSSH = false;
        /// The sole descriptor accompanying Prepare's isolated first byte.
        PAL::UnixFileDescriptor pendingAdoptFileDescriptor;
        uint32_t clientID = 0; // stable internal connection handle, not a revocation selector
        /// Bounds delivery of queued results and EXIT after the session ends.
        std::uint64_t sessionExitDeadlineMilliseconds = 0;
        /// The output still owed when the session-exit deadline was last set;
        /// zero for a deadline that is never extended.
        std::size_t sessionExitPendingBytes = 0;
        /// When the session-exit deadline was first armed, which bounds how far
        /// its extensions may carry it.
        std::uint64_t sessionExitDrainStartMilliseconds = 0;
        bool isReadingFrames = false;
        /// A failed write leaves a bounded interval to read the peer's final frames.
        std::uint64_t writeFailureDeadlineMilliseconds = 0;
        std::size_t writeFailureReadBytes = 0;

        // Pending outbound bytes are outputBuffer[outputConsumed..end): flushClient
        // advances the cursor instead of erasing the sent prefix, so draining a
        // multi-MiB backlog one ~8 KB socket-buffer write at a time stays O(n)
        // (front-erase per write is O(n^2) memmove). Every "how much is queued"
        // read goes through pendingOutputBytes(), never outputBuffer.size().
        std::vector<uint8_t> outputBuffer;
        std::size_t outputConsumed = 0; // bytes already written from outputBuffer's front
        /// The writes the output took: each `write` that moved bytes, and each
        /// hand-off to a QUIC stream. A loop pass queues every frame it has for
        /// the client before it flushes, so one publish costs one.
        std::uint64_t outputWrites = 0;
        /// Whether the descriptor no longer raises SIGPIPE for a write to a
        /// peer that hung up, where that is a socket option set once.
        bool isBrokenPipeSignalSuppressed = false; // bool-ok: names the one state it reports
        // Saturating: a desync (consumed > size) must read as 0 pending, never
        // underflow to ~SIZE_MAX and wreck the high-water cap.
        std::size_t pendingOutputBytes() const { return outputBuffer.size() >= outputConsumed ? outputBuffer.size() - outputConsumed : 0; }
        bool hasPendingOutput() const { return outputConsumed < outputBuffer.size(); }
        /// For a client whose output is Sync frames: the first byte of the frame
        /// `outputConsumed` stopped inside, or `outputConsumed` itself on a frame
        /// boundary. Frames are queued whole, so a walk from here finds where the
        /// frame a write stopped inside ends.
        std::size_t outputFrameStart = 0;
        /// When a client dropped for its output backlog is reaped even though its
        /// Bye has not left; zero for every other client.
        std::uint64_t backlogByeDeadlineMilliseconds = 0;
        void clearPendingOutput()
        {
            outputBuffer.clear();
            outputConsumed = 0;
            outputFrameStart = 0;
        }
        /// Advances `outputFrameStart` to the frame `outputConsumed` is inside.
        ///
        /// - Returns: Where that frame ends, `outputConsumed` on a boundary, or
        ///   nothing when the output does not walk as whole frames.
        [[nodiscard]] std::optional<std::size_t> advanceOutputFrameStart();
        // Reclaims the already-sent prefix once it dominates the buffer and is big
        // enough to amortize the memmove; a full drain resets via clearPendingOutput
        // instead. Keeps the dead prefix bounded (≤ max(floor, size/2)) without
        // paying a shift on every partial write. A framed client reclaims only up
        // to the frame a write stopped inside, so the frame boundaries stay known.
        static constexpr std::size_t outputCompactionFloorBytes = 256u * 1024u;
        void compactOutputBuffer(std::size_t reclaimable)
        {
            if (outputConsumed >= outputBuffer.size()) {
                clearPendingOutput();
                return;
            }
            reclaimable = std::min(reclaimable, outputConsumed);
            if (reclaimable < outputCompactionFloorBytes || reclaimable < outputBuffer.size() / 2)
                return;
            outputBuffer.erase(outputBuffer.begin(), outputBuffer.begin() + static_cast<long>(reclaimable));
            outputConsumed -= reclaimable;
            outputFrameStart = outputFrameStart > reclaimable ? outputFrameStart - reclaimable : 0;
        }
        bool helloSeen = false;
        bool didSeeClipboardToolRequest = false;
        std::uint32_t clipboardToolSessionID = 0;
        std::uint32_t clipboardToolSourceClientID = 0;
        std::uint32_t clipboardToolReplySourceClientID = 0;
        /// Pins native routing or an empty client answer until queued output drains.
        std::uint32_t clipboardToolSelectionClientID = 0;
        bool isClipboardToolHostSelected = false;
        std::uint32_t clipboardToolRequestID = 0;
        std::uint32_t clipboardToolOriginalRequestID = 0;
        std::uint64_t clipboardToolDeadlineMilliseconds = 0;
        bool didOverflow = false; // pending output hit the high-water cap → drop it
        // The client said it was leaving (EXIT) rather than losing its
        // transport. An ends-when-detached session waits out a reconnect window
        // for the second departure and none at all for the first, so the two
        // are told apart at the reap that erases the client.
        bool didDetachCleanly = false;
        // Partial-frame budget: the wall-clock deadline by which an
        // incomplete declared frame must complete, armed when this client's
        // inbound decoder first holds bytes and disarmed when it drains. A
        // peer that trickles or withholds a large body past this is dropped.
        std::uint64_t partialFrameDeadlineMilliseconds = 0; // 0 = disarmed
        uint32_t sessionID = 0; // attached session (0 = pre-HELLO)
        std::uint64_t attachedUnix = 0;
        std::uint64_t connectMilliseconds = 0; // accept() time; reaped if no HELLO within the grace window
        // file.read is handled synchronously to completion (no async round
        // trip), so there's never more than one "in flight" — this instead
        // bounds total requests over the connection's lifetime, the real
        // DoS-prevention lever, since cwd confinement already rules out a
        // filesystem sweep.
        uint32_t fileRequestCount = 0;
        // Served PASTE_PUSH count, capped like the file reads: each landed
        // push is a file this daemon wrote, and an unbounded count is a disk
        // a hostile client can fill.
        uint32_t pastePushCount = 0;
        /// One exact call owns one bounded upload, never a second disk staging file.
        struct PasteCall {
            std::uint32_t callIndex = 0;
            std::uint32_t sessionID = 0;
            std::uint32_t handle = 0;
            Protocol::Sync::ID128 paneID;
            Protocol::Sync::ID128 attachmentID;
            Protocol::Sync::Role role = Protocol::Sync::Role::Viewer;
            std::string principal;
            std::string requestID;
            std::uint64_t deadlineMilliseconds = 0;
            std::uint64_t lastCreditMilliseconds = 0;
            bool hasReceivedChunk = false;
            Protocol::Control::PasteUpload upload;
            std::optional<Protocol::Control::PastePushReply> reply;
            std::optional<Codec::JSON::Value> refusal;
        };
        // Covers bounded manifest/correlation/principal/result metadata,
        // including temporary serialized copies, separately from raw storage.
        static constexpr std::size_t pasteCallMetadataBytes = 4u * Protocol::Sync::maxFrameBodyBytes + sizeof(PasteCall);
        std::optional<PasteCall> pasteCall;
        std::uint32_t nextPasteTransfer = 1;
        /// Clipboard requests this client has been asked and not yet answered.
        ///
        /// Bounded because a program can ask faster than a person can approve a
        /// pasteboard read, and an unbounded map is a client-driven allocation.
        std::vector<std::uint32_t> pendingClipboardRequests;
        /// The immutable workspace authority established by HELLO.
        std::uint32_t workspaceID = 0;
        /// Per-pane synchronization state; transport and authentication remain on
        /// Client.
        std::vector<PaneSubscription> paneSubscriptions;
        /// Whether the client subscribed to the host's listing
        /// (`host.sessions.subscribe`): it is sent every workspace's
        /// structure and the records of every pane it has not bound.
        bool isListing = false; // bool-ok: names the one state it reports
        /// The panes listed on the client's connection, each session with
        /// the handle its pane is listed under.
        std::vector<std::pair<std::uint32_t, std::uint32_t>> listedSessions;
        /// Finds the independently synchronized state of a subscribed pane.
        ///
        /// - Parameter targetSessionID: The pane's backing session.
        /// - Returns: The subscription, or null when the pane is not admitted.
        PaneSubscription* subscriptionFor(std::uint32_t targetSessionID);
        /// Finds the independently synchronized state of a subscribed pane.
        ///
        /// - Parameter targetSessionID: The pane's backing session.
        /// - Returns: The subscription, or null when the pane is not admitted.
        const PaneSubscription*
        subscriptionFor(std::uint32_t targetSessionID) const;
        /// What the client's HELLO said this connection is for.
        Protocol::Sync::HelloIntents intents {};

        // Non-null when this client's transport is a QUIC connection rather
        // than the AF_UNIX socket (fileDescriptor stays -1). Its Sync byte
        // stream arrives via the listener's stream-data callback and is sent
        // with QUICListener::sendStream instead of write(fd). Owned by the
        // listener; the daemon only holds the pointer for routing.
        QUICConnection* quicConnection = nullptr;
        /// Why the reap closes this QUIC client's connection, carried in its
        /// application CONNECTION_CLOSE; nothing for a silent close.
        std::optional<Protocol::Sync::ByeReason> quicCloseReason;

        /// The host end of this client's Sync screen plane, created by the
        /// first Sync frame the client sends after its HELLO. Its panes are
        /// the client's subscriptions, bound once it has welcomed the client.
        std::unique_ptr<Protocol::Sync::HostConnection> syncConnection;
        /// Retains image snapshots and bounded carrier scratch under this
        /// client's existing output high water, even after its ordinary owner
        /// is removed. It is distinct from the daemon's aggregate ledger.
        std::shared_ptr<Protocol::Sync::BulkSourceBudget> imageSourceBudget;
        // QUIC auth gate: a QUIC connection leads with a preamble before any
        // Sync frame, a one-time token ([1][length][token]; the cert is
        // self-signed, so the SSH-delivered token is the auth) or a resume
        // proof for an attachment it holds ([2][proof]). quicPreamble holds
        // the preamble's bytes, and no more, until it validates; only then do
        // stream bytes reach the HostConnection. AF_UNIX clients are uid-gated
        // and never go through this path (quicAuthed stays unused).
        bool quicAuthed = false;
        Protocol::Sync::PreambleReader quicPreamble;

        // Capability-token auth. hasToken is set when HELLO presented a
        // VALID token; tokenScope is the granted ceiling enforced per op;
        // tokenID is the token digest — a non-spoofable principal that keys
        // always-allow/allow-session grants instead of the mutable exe path.
        bool hasToken = false;
        Protocol::Sync::ScopeSet tokenScope {};
        std::string tokenID;
        /// The client's window pixels and theme, from Sync Facts. Per-pane
        /// Viewport pixels belong to their PaneSubscription. A session takes
        /// its live geometry owner's presentation (Docs/sync.md §6.10,
        /// §6.11), or a live human's colours while that owner is absent.
        struct Presented {
            std::optional<Ubiquitty::PixelGeometry> pixels;
            std::optional<Protocol::Sync::ThemePalette> palette;
            /// Host-wide order of changed presentations, for the live fallback.
            std::uint64_t revision = 0;
        };
        Presented presented;
        // When the token in force lapses, as a Unix timestamp; 0 when it never
        // does. The gate that admitted the connection is the handshake, and a
        // lapse changes nothing in the store, so the connection carries its own
        // deadline and ends at it (dropExpiredTokenClients).
        std::uint64_t tokenExpiryUnix = 0;
        // Whether the token in force is the one-time QUIC bootstrap token the
        // preamble carried rather than a record in the tokens file. The store
        // holds no digest for it, so the revoke pass reads its absence as what it
        // is — never issued there — instead of as a revocation.
        bool hasBootstrapToken = false;
        // Whether the presented token was minted for the authenticated person
        // rather than for automation: the QUIC bootstrap token a uid-gated local
        // client asks for on that person's behalf, carried forward by the
        // preamble. A token read from the tokens file never sets it. It answers
        // who the client is (isAgentClient), never what it may do — the scope
        // ceiling stays exactly what the token carries.
        bool isInteractiveUser = false;

        /// Consent gate. agentLabel is the agent-claimed self-id from
        /// HELLO; peerProcessID/peerProcess are the daemon-verified local identity
        /// (0/"" for a QUIC/remote client) — peerProcess is the always-allow
        /// grant key. When a control agent's attach is held pending the
        /// human's consent, consentPending is set with the requested session,
        /// an assigned request id, and a deadline.
        /// What a held consent decides: Attach binds the agent + seeds its
        /// attach state on grant and DROPS the agent on deny/timeout; Terminate
        /// SIGHUPs the target session on grant and just clears the hold on
        /// deny/timeout (the control client lives on).
        enum class ConsentAction : uint8_t {
            Attach,
            Terminate
        };

        std::string agentLabel;
        uint32_t peerProcessID = 0;
        /// Process lifetime sampled at accept when the platform supplies it.
        std::optional<std::uint64_t> peerProcessInstance;
        /// A positive lifetime mismatch at accept cannot become an unknown witness.
        bool hasPeerProcessInstanceMismatch = false;
        std::string peerProcess;

        /// Which file the peer is running, taken once when the connection was
        /// accepted, from what the kernel resolved.
        ///
        /// `peerProcess` above is a label: it keys the grant store and prints in
        /// the interface. It is not identity, because a path names whatever
        /// stands there at the moment it is read, and the peer chooses that.
        BinaryIdentity peerBinary;

        /// Whether `peerBinary` was taken. False for a peer with no local
        /// identity to take — a QUIC connection, or a platform whose credential
        /// call reports no pid.
        bool hasPeerBinary = false;
        /// What the signature of the code the peer runs says, read when the
        /// connection was accepted (`isOwnBinary`).
        CodeSignature peerSignature {};
        bool consentPending = false;
        /// Its accepted event-stream cursor; backpressure retains no second log.
        Protocol::Control::EventStreamReplay eventReplay;
        Protocol::Control::EventStreamReplay screenAccessReplay;
        std::vector<std::uint32_t> screenAccessSessions;
        ConsentAction consentAction = ConsentAction::Attach;
        uint32_t consentRequestID = 0;
        uint32_t consentSessionID = 0;
        /// Only current clients whose exact requested Notify was admitted may
        /// answer or be sent this question's eventual retirement.
        std::vector<ConsentRecipient> consentRecipients;
        std::uint64_t consentDeadlineMilliseconds = 0;
        /// The immutable pane named when this attach question was created.
        Protocol::Sync::ID128 consentPaneID;
        /// A decision retains both authenticated owners while output is blocked.
        struct AttachConsentApproval {
            Protocol::Sync::ConsentBinding question;
            Protocol::Sync::ConsentChoice decision = Protocol::Sync::ConsentChoice::Deny;
            Protocol::Sync::ID128 requesterAttachment;
            std::string requesterPrincipal;
            Protocol::Sync::ScopeSet requesterScopes;
            std::uint32_t approverClientID = 0;
            Protocol::Sync::ID128 approverAttachment;
            std::string approverPrincipal;
            /// Who the management history names as having granted what the
            /// approval creates, kept for when the approver has gone.
            std::string approverIdentity;
            Protocol::Sync::ID128 targetPane;
        };
        std::optional<AttachConsentApproval> attachConsentApproval;
        /// Denial or expiry is delivered through the exact own-status path.
        std::optional<Protocol::Sync::ConsentStatusPhase> attachConsentTerminal;
        /// One exact destructive call held across its single-use human decision.
        struct TerminationCall {
            std::uint32_t callIndex = 0;
            std::uint32_t sessionID = 0;
            Protocol::Sync::ID128 paneID;
            Protocol::Sync::ID128 attachmentID;
            Protocol::Sync::Role role = Protocol::Sync::Role::Viewer;
            Protocol::Sync::Scope scope = Protocol::Sync::Scope::Control;
            std::string principal;
            Codec::JSON::Value requestID;
        };
        std::optional<TerminationCall> terminationCall;
        /// One attached-owner release waiting until no decoder borrows its pane.
        struct ReclaimCall {
            std::uint32_t callIndex = 0;
            Protocol::Control::ReclaimTarget target;
            Protocol::Sync::Role role = Protocol::Sync::Role::Viewer;
            Protocol::Sync::ScopeSet scopes;
            std::string principal;
            Codec::JSON::Value requestID;
        };
        std::optional<ReclaimCall> reclaimCall;
        /// Retirements that met output backpressure. No new structure question
        /// is offered until these drain; at most the sixteen pending questions
        /// can become retirements while the output is held.
        std::vector<Protocol::Sync::ConsentResolution> pendingConsentRetirements;
    };

    /// Claims the right to decide who owns this socket path.
    ///
    /// Held for the daemon's lifetime. Without it the stale-socket probe in
    /// `bindSocket` cannot distinguish a dead daemon's leftover socket from a
    /// live one that has bound and not yet listened, and two starters racing
    /// can leave the winner listening on an unlinked inode.
    ///
    /// - Parameter socketPath: The socket whose lock file sits beside it.
    /// - Returns: False only when another starter holds the lock right now.
    bool acquireStartupLock(const char* socketPath);

    /// Refuses one pending connection using the reserve descriptor.
    ///
    /// Called when `accept` reports the descriptor table is full, where the
    /// pending connection would otherwise stay in the backlog keeping the
    /// listener readable forever.
    /// - Parameter listener: The readable listening socket.
    void refuseClientWithReserveDescriptor(int listener);

    /// Binds and listens on the unix socket at `path`.
    ///
    /// - Parameter path: The socket path to own.
    /// - Returns: `BindFailure::None` on success, else the cause. The path is
    ///   validated before the startup lock is taken, so a refused path leaves no
    ///   lock file beside a socket that was never created.
    BindFailure bindSocket(const char* path);

    /// Listens on a bound unix socket as this process, with the client cap
    /// as its backlog.
    ///
    /// Where the kernel records a listener's credentials when it listens
    /// (Linux), a connection not yet accepted reports the process that last
    /// listened, so a socket this daemon inherited listens again here before
    /// a later successor can authenticate its owner through it.
    ///
    /// - Parameter descriptor: A bound unix stream socket.
    /// - Returns: Whether it listens.
    [[nodiscard]] static bool listenAsOwner(int descriptor);

    /// Pins and hashes the executables a local adopt may come from, before
    /// the daemon serves its first client. A daemon that never serves (an
    /// in-process one a test drives) holds none unless it asks for them, and
    /// hashing a whole program file is a cost it otherwise pays for nothing.
    void captureLocalAdoptExecutables();
    void writeProcessIDFile(const char* path);

    // Basename of m_socketPath (e.g. "host-<id>.sock") — sent in HELLO_ACK so
    // a client can pin a reattach to this daemon's socket.
    std::string socketBasename() const;

    // Session registry.
    /// Registers a session on a PTY of its own, forking a child or adopting one.
    ///
    /// The adopt path (`adoptFileDescriptor >= 0`) re-homes a PTY master an app
    /// passed over SCM_RIGHTS: that child is already running, so nothing here
    /// sizes it, execs into it, or reaps it — see `PAL::adoptPTY`'s ownership
    /// check for what corroborates the claim.
    ///
    /// The exec path takes `command` as given. Whatever assembled it owns
    /// validating what it was assembled from: a session command derived from a
    /// client's bytes is the caller's decision, made where the authority to make
    /// it is known, and re-deriving one here would put that decision in two
    /// places with only one of them gated.
    ///
    /// - Parameters:
    ///   - columns: Client grid width; the daemon's default when not positive.
    ///   - rows: Client grid height; the daemon's default when not positive.
    ///   - command: The argv to exec — the assembled `ssh [user@]host` of a
    ///     remote session. Empty runs `m_shellPath` as a login shell. Meaningless
    ///     alongside `adoptFileDescriptor`, whose child another process launched.
    ///   - adoptFileDescriptor: A PTY master to take over instead of forking one.
    ///   - adopterProcessID: The kernel-verified peer making the adopt claim
    ///     (`Client::peerProcessID`), meaningful only alongside the descriptor.
    ///   - placement: The pane a verb minted for this session; nothing for a
    ///     session that roots a workspace of its own, whose root is minted here.
    ///   - creatorGrantKey: The grant key of the agent creating the session,
    ///     which owns it from its first event on; empty for a person, or an
    ///     agent with no stable key, which own nothing they could be matched by.
    ///   - initialDirectory: Where the child starts; empty, or a directory
    ///     that cannot be entered, starts it in the home directory.
    ///   - authoritativeTerminal: A complete privately decoded local terminal,
    ///     installed before any source, encoder or callback borrows its model.
    ///     Only the descriptor-adopt path may supply it; it is never reseeded
    ///     or resized over the carried continuation.
    /// - Returns: The registered session, or nothing when the session cap is
    ///   reached, an ID kind is exhausted, or the PTY could not be opened or
    ///   adopted.
    Session* createSession(int columns, int rows, std::span<const std::string> command = {}, int adoptFileDescriptor = -1, uint32_t adopterProcessID = 0,
        std::optional<Ubiquitty::Session::Layout::RootIdentity> placement = std::nullopt, std::string_view creatorGrantKey = {}, std::string_view initialDirectory = {},
        std::unique_ptr<Ubiquitty::Terminal> authoritativeTerminal = {});
    /// Constructs the complete session without registering it or emitting events.
    /// A normal shell may start here; local adoption supplies an already-running
    /// master and an authoritative terminal, so it neither launches nor resizes.
    [[nodiscard]] std::unique_ptr<Session> constructSession(int columns, int rows, std::span<const std::string> command, int adoptFileDescriptor, uint32_t adopterProcessID,
        std::optional<Ubiquitty::Session::Layout::RootIdentity> placement, std::string_view creatorGrantKey, std::string_view initialDirectory,
        std::unique_ptr<Ubiquitty::Terminal> authoritativeTerminal);
    /// Publishes a completely constructed owner and its creation event.
    /// Callers staging adoption reserve registry capacity before this step.
    [[nodiscard]] Session* publishSession(std::unique_ptr<Session> session);
    Session* findSession(uint32_t id);
    /// The live session a HELLO's workspace name reaches.
    ///
    /// - Parameter workspaceName: The name to resolve; an empty one reaches
    ///   nothing, since an unnamed session is not addressable by name.
    /// - Returns: The session bearing the name, or nothing when the daemon
    ///   holds none — which is the caller's cue to create one.
    Session* findSessionByWorkspaceName(const std::string& workspaceName);
    /// Who is on a session: each client that has handshaken, is bound to it,
    /// and is not on its way out, with whether it is a person or an agent.
    ///
    /// The one roster predicate: the listing's tree states it, and an
    /// ends-when-detached session's departure clock reads it.
    ///
    /// - Parameter sessionID: The session whose roster is read.
    /// - Returns: Its attendees, by client; none for session 0.
    std::vector<Ubiquitty::Session::TreeAttendee> attendeesOf(uint32_t sessionID) const;
    /// Arms the departure deadline of an ends-when-detached session.
    ///
    /// Called as a client is erased, which is where a clean detach and a lost
    /// transport are still distinguishable. An already-armed deadline stands: a
    /// person's lost transport bought the window, and an agent leaving after
    /// does not shorten it.
    ///
    /// - Parameters:
    ///   - sessionID: The session the departing client was on.
    ///   - wasHuman: Whether the client that left was a person.
    ///   - cleanDetach: Whether it said so with EXIT.
    void noteClientDeparture(uint32_t sessionID, bool wasHuman, bool cleanDetach); // bool-ok: two independent facts about one departure — whether a person left, and whether the detach was clean; an enum would have to enumerate their product
    /// Ends the sessions whose departure deadline has passed.
    ///
    /// The arrival a window waits for disarms it, so a reconnect inside the
    /// window keeps the session it returns to.
    ///
    /// - Parameter now: The current monotonic milliseconds.
    void serviceDetachedSessions(std::uint64_t now);
    // The principal a client's authority is decided by: who it is, for tokens,
    // scopes, roles, consent and local control.
    Ubiquitty::CapabilityOrigin originOf(const Client&) const;
    /// Where the person a client represents is, for what its sessions may do
    /// beyond the grid.
    ///
    /// ``originOf`` answers who the client is; this answers whether the person
    /// is on this host. A client this daemon's own bridge relays from an SSH
    /// channel is the owner by authority, since SSH authenticated it, but its
    /// person is on another machine, so its capability origin is Remote. This
    /// is what an input driver stamps on its session, what a session's output
    /// is gated at, and what a caller restricted to clients on this host is
    /// judged by.
    ///
    /// - Parameter client: The client.
    /// - Returns: Remote for a client relayed from SSH, otherwise ``originOf``.
    [[nodiscard]] Ubiquitty::CapabilityOrigin capabilityOriginOf(const Client& client) const;
    /// Reads only kernel-established local authority and current connection state.
    [[nodiscard]] LocalControlPeer localControlPeer(const Client&) const;
    /// Revalidates the authenticated local process before a handoff record.
    [[nodiscard]] bool mayReceiveLocalHandoff(const Client&) const;
    /// Delivers complete transaction records only on the selected local lane.
    void receiveLocalAdopt(std::size_t index, std::span<const std::uint8_t> bytes);
    /// Applies one exact Prepare, Commit, Abort, or Status under fresh authority.
    void dispatchLocalAdopt(std::size_t index, const Protocol::Local::AdoptIngress::Frame&);
    /// Publishes an irrevocably committed owner after the ingress borrow ends,
    /// even when its requester cannot receive the retained result.
    void finishLocalAdoptPublication();
    /// Resolves the kernel helper process to exactly one live hosted process tree.
    [[nodiscard]] bool bindLocalClipboardHelper(std::size_t index);
    /// Reads only the permanently selected local operation, never remote frames.
    /// - Parameters:
    ///   - index: The exact selected helper connection.
    ///   - bytes: Newly received operation-body bytes, possibly empty at prefix completion.
    void feedLocalClipboard(std::size_t index, std::span<const std::uint8_t> bytes);
    /// Appends one fully measured reply directly to admitted output storage.
    [[nodiscard]] bool queueLocalClipboardReply(std::size_t index, const Protocol::Local::LocalClipboardReply& reply);
    /// Revalidates the exact provider and helper lifetime before delayed output.
    [[nodiscard]] bool isClipboardToolSelectionCurrent(const Client&) const;
    // Send EXIT to the session's clients, detach them, dispose the session.
    void destroySession(uint32_t id);
    /// Releases an authorized adopted stream after its exact Result was queued.
    /// The requester already owns its retained local master; no descriptor crosses.
    void reclaimSession(std::size_t requesterIndex, uint32_t sessionID);
    /// Accepts every pending connection on the daemon's own listener.
    void acceptClient();
    /// Accepts every pending connection on one listening socket.
    /// - Parameter listener: The daemon's own listener or one it inherited.
    void acceptClient(int listener);
    void dropClient(std::size_t index); // deferred: mark dead; reapClient does the close+erase
    /// Closes and erases a client already marked dead.
    ///
    /// Called only from the run loop's reap pass, walking from the end so an
    /// erase cannot move an index the same pass has yet to visit.
    ///
    /// - Parameters:
    ///   - index: The client slot to close and erase.
    void reapClient(std::size_t index);
    void kickClient(std::size_t index); // EXIT + deferred-reap a client (revoke/disconnect)
    /// Retires only the attachment this connection still serves and its retained
    /// resume grant, before a refusal can flush output or defer the client reap.
    /// Ordinary transport loss does not call this operation.
    ///
    /// - Parameter index: The client whose authority ended.
    void withdrawClientAttachment(std::size_t index);
    /// Releases every borrower before retiring one attachment's authority.
    ///
    /// - Parameter attachmentID: The exact server-issued attachment identity.
    void retireAttachmentGrant(Protocol::Sync::ID128 attachmentID);
    /// Finds agent attachments holding a session's pane, including lost streams.
    ///
    /// The returned identities survive retirement of any matching record.
    ///
    /// - Parameters:
    ///   - sessionID: The selected session, or zero for every session.
    ///   - identity: The verified principal, or empty for every agent.
    /// - Returns: Owned attachment identities whose authority must end.
    [[nodiscard]] std::vector<Protocol::Sync::ID128> agentAttachmentsForSession(std::uint32_t sessionID, std::string_view identity) const;
    /// Arms a client's session-exit deadline over the output it still owes.
    ///
    /// - Parameters:
    ///   - client: The client sent EXIT.
    ///   - now: The current monotonic milliseconds.
    void armSessionExitDeadline(Client& client, std::uint64_t now) const;
    /// Returns whether a client draining a session exit is due to be reaped.
    ///
    /// A deadline that passes while the client is still reading extends
    /// itself: the EXIT sits behind everything queued before it, and reaping a
    /// slow but live reader discards it and leaves a bare EOF, which a client
    /// takes for a dropped link and reattaches. Reading means a minimum amount
    /// per window, and the extensions stop at a ceiling measured from the
    /// first deadline, so a client that reads a trickle, or nothing, is reaped.
    ///
    /// - Parameters:
    ///   - client: The client to judge; its deadline moves when it is extended.
    ///   - now: The current monotonic milliseconds.
    /// - Returns: True when the deadline has passed and the client is not reading enough to extend it.
    [[nodiscard]] bool isSessionExitDrainOver(Client& client, std::uint64_t now) const;
    /// Returns whether the run loop's reap pass closes and erases a client now.
    ///
    /// - Parameters:
    ///   - client: The client to judge; a session-exit deadline moves when it
    ///     is extended.
    ///   - now: The current monotonic milliseconds.
    /// - Returns: True for a client marked dead, one that never greeted within
    ///   its grace, and one whose session-exit drain is over.
    [[nodiscard]] bool isReapDue(Client& client, std::uint64_t now) const;
    // Push the live attendee roster to every presence-aware human on `sessionID`
    // (no-op if none). Called on attach/detach/grant/revoke.
    void broadcastPresence(uint32_t sessionID);
    /// Returns presentation only for an exact current live row shared with a
    /// currently admitted human Observe recipient. No sends or reentry occur.
    /// - Parameters:
    ///   - recipientClientID: The captured stable physical recipient identity.
    ///   - attachmentID: The authority-derived base row's exact identity.
    ///   - panes: Only the base roster's shared bound pane intersection.
    ///   - isSelf: Whether that base row is the recipient itself.
    /// - Returns: Verified display facts and exact per-pane grants, or no metadata.
    [[nodiscard]] std::optional<Protocol::Sync::AttachmentPresence> attachmentPresenceFor(std::uint32_t recipientClientID,
        Protocol::Sync::ID128 attachmentID, std::span<const Protocol::Sync::ID128> panes, bool isSelf) const;

    void flushClient(std::size_t index);
    /// Appends bytes to one client's pending output.
    ///
    /// A client the bytes would take over its high-water mark is not grown:
    /// `dropForOutputBacklog` sheds it, and the service loop reaps it.
    ///
    /// - Parameters:
    ///   - index: Client slot; out of range queues nothing.
    ///   - bytes: Copied into the buffer before the call returns.
    void queueToClient(std::size_t index, std::span<const std::uint8_t> bytes);
    /// Drops a client whose output passed its high-water mark, telling it why.
    ///
    /// What was queued is discarded, except the rest of the frame a write
    /// stopped inside, which a framed stream needs whole. A Sync client on a
    /// stream is then sent `Bye{OutputBacklog}` past the mark, one small frame,
    /// and the reap waits a bounded time for it to leave. A QUIC client is
    /// closed by the reap with the reason in its application
    /// CONNECTION_CLOSE. Any other client is dropped with nothing said.
    ///
    /// - Parameter index: The client, in range.
    void dropForOutputBacklog(std::size_t index);
    /// Drops a client with a `Bye` naming why, as `dropForOutputBacklog` does
    /// for its own reason.
    ///
    /// - Parameters:
    ///   - index: The client, in range.
    ///   - reason: The reason the `Bye`, or a QUIC client's close, names.
    ///   - detail: The `Bye`'s sentence.
    void dropWithFarewell(std::size_t index, Protocol::Sync::ByeReason reason, std::string_view detail);
    /// Reserves a complete local token answer before any bearer is minted.
    [[nodiscard]] LocalReplyAdmission reserveLocalQUICTokenReply(std::size_t index);
    /// Admits the complete local reply and its output capacity before mutation.
    /// Allocation uses the existing output-vector policy; this is not a
    /// recoverable-allocation guarantee under memory exhaustion.
    [[nodiscard]] LocalReplyAdmission reserveLocalReplyBytes(std::size_t index, std::size_t replyBytes);
    /// Moves the connection's interactive frames into its bounded output,
    /// holding every bulk frame for the window.
    ///
    /// - Parameter index: The client.
    void drainScreenInteractive(std::size_t index);

    void serviceClient(std::size_t index);
    /// Reports whether reading a client's socket waits for the client to read.
    ///
    /// A client is not read while its screen connection holds as many unsent
    /// interactive bytes as it allows: a client that writes calls and reads
    /// none of the answers is held to that bound (Docs/sync.md §6.5).
    ///
    /// A program that reads nothing holds no client. A client's paste is kept
    /// inside a window by its own receipts (Docs/sync.md §6.7), typed input
    /// past the pane's bound closes it, and what it sends behind a paste a
    /// program is not reading, an interrupt or a `PasteAbort`, has to be
    /// seen to be acted on.
    ///
    /// - Parameter client: The client whose pane subscriptions are considered.
    /// - Returns: Whether the client's input is held.
    [[nodiscard]] bool isInputHeld(const Client& client) const;

    /// Logs, for each client, whether its input is being read and each fact
    /// the answer turns on, and for each session whether its program has left
    /// input unread. Asked for with SIGUSR1: a client whose keys reach nothing
    /// looks the same from outside whichever hold is starving it.
    void logHoldState() const;

    /// Runs one call a client's screen connection admitted.
    ///
    /// `pane.capture {pane: handle}` answers `{"text": ...}`, the pane's visible
    /// screen as the region reader writes it, read at answer time from the one
    /// terminal the session holds, so it is one generation and never torn.
    /// `pane.search {pane: handle, pattern, from?, to?, caseSensitive?}`
    /// answers the matches in the pane's whole history (`Sync::searchPane`).
    /// A method the registry's Sync column does not name is not found
    /// (`Control::findSyncMethod`).
    ///
    /// - Parameters:
    ///   - client: The caller.
    ///   - request: The admitted request.
    /// - Returns: The JSON-RPC 2.0 response object.
    [[nodiscard]] Codec::JSON::Value serveScreenCall(Client& client, const Protocol::Control::DecodedRequest& request);

    /// Reads one admitted file under the foreground process's kernel directory.
    /// - Parameters:
    ///   - client: The physical caller whose lifetime request budget is charged.
    ///   - session: The exact bound session, resolved before opening a file.
    ///   - handle: Its connection-local pane handle.
    ///   - request: The validated file.read request.
    /// - Returns: A correlated bounded file result or explicit refusal.
    [[nodiscard]] Codec::JSON::Value serveFileRead(Client& client, Session& session, std::uint32_t handle, const Protocol::Control::DecodedRequest& request);

    /// Who a client is to a Sync call: the role its attachment was granted,
    /// the scopes its credential carries (every scope for a local client with
    /// no token), and where the daemon verified it to stand. What
    /// `Control::admitCall` is asked with.
    ///
    /// - Parameter client: The caller.
    /// - Returns: Its authority.
    [[nodiscard]] Protocol::Control::CallerAuthority callerAuthorityOf(const Client& client) const;

    /// The scopes a client's credential carries here: every scope for a
    /// local client with no token, as `tokenScopeAllows` has it.
    ///
    /// - Parameter client: The caller.
    /// - Returns: The scopes.
    [[nodiscard]] Protocol::Sync::ScopeSet scopesOf(const Client& client) const;

    /// Says why a client's token does not reach what it asked for: the scopes
    /// the token carries and the one it would need, so the person who issues
    /// tokens can issue the right one.
    ///
    /// - Parameters:
    ///   - client: The refused client.
    ///   - needed: The scope it lacks.
    ///   - asked: What it asked for, as "a control connection".
    /// - Returns: The sentence a refusal's `Bye` carries.
    [[nodiscard]] std::string scopeRefusal(const Client& client, Protocol::Sync::Scope needed, std::string_view asked) const;

    /// The most a client's credential lets its attachment be: an agent for a
    /// client the daemon verified as one, the owner for the host's own user,
    /// a driver for a token that may drive, a viewer otherwise.
    ///
    /// - Parameter client: The client greeting.
    /// - Returns: The ceiling its Welcome grants under.
    [[nodiscard]] Protocol::Sync::Role maxRoleOf(const Client& client) const;

    /// Serves a call addressed to the host: one whose registry entry names
    /// `CallTarget::Host`, admitted already.
    ///
    /// - Parameters:
    ///   - client: The caller.
    ///   - method: The call's registry entry.
    ///   - request: The admitted request.
    /// - Returns: The JSON-RPC 2.0 response object.
    [[nodiscard]] Codec::JSON::Value serveHostCall(const Client& client, const Protocol::Control::MethodDescriptor& method, const Protocol::Control::DecodedRequest& request);
    /// Places committed local adoptions in a complete window without restarting PTYs.
    [[nodiscard]] Codec::JSON::Value serveSharedWorkspace(const Client&, const Protocol::Control::DecodedRequest&);
    /// Reads passive catch-up state without binding panes or changing presentation ownership.
    [[nodiscard]] Codec::JSON::Value serveBackgroundRefresh(const Client&, const Protocol::Control::DecodedRequest&);
    /// Loads only a bounded regular journal owned by the host user, preserving refused files.
    void loadBackgroundEvents();
    /// Marks the journal for a coalesced durable write during host maintenance.
    void scheduleBackgroundEventWrite();
    /// Commits the complete journal before any response exposes its cursor.
    ///
    /// - Returns: Whether the current journal revision is durably stored.
    [[nodiscard]] bool writeBackgroundEvents();

    /// Serves a call that changes what a workspace's structure says: a label,
    /// a set of tags, a workspace's or a tab's name, a tab's place, a window
    /// closed (`label.set`, `tags.set`, `workspace.rename`, `tab.rename`,
    /// `tab.move`, `window.close`).
    ///
    /// The call was admitted for its role, origin, scope and parameters. Here
    /// the object it names is found, the caller is held to
    /// ``rearrangeRefusal`` for the workspace holding it, and the change is
    /// made or refused with the reason as the error's message. A change is
    /// recorded, sent to the workspace's attached clients and seen by every
    /// listing at its next structure. The sessions of a closed window end
    /// after the pass that served the call (``settleOwedClosures``), never
    /// inside it.
    ///
    /// - Parameters:
    ///   - index: The caller's place among the clients.
    ///   - method: The call's registry entry.
    ///   - request: The admitted request.
    ///   - approver: The person authorizing this exact operation, only after
    ///     its pending request was revalidated; absent for a person's own call.
    /// - Returns: The JSON-RPC 2.0 response object.
    [[nodiscard]] Codec::JSON::Value serveStructureCall(std::size_t index, const Protocol::Control::MethodDescriptor& method, const Protocol::Control::DecodedRequest& request, const Client* approver = nullptr);

    /// Holds an agent's exact tree call until an authorized person answers.
    ///
    /// - Parameters:
    ///   - index: The requesting client.
    ///   - callIndex: Its connection's outstanding call number.
    ///   - request: The original call.
    /// - Returns: An immediate refusal, or nothing while the call is held.
    [[nodiscard]] std::optional<Codec::JSON::Value> beginStructureConsent(std::size_t index, std::uint32_t callIndex, const Protocol::Control::DecodedRequest& request);

    /// Answers a pending exact operation, revalidating both clients and the
    /// workspace before any effect.
    ///
    /// - Parameters:
    ///   - approver: The client answering.
    ///   - request: Its `consent.respond` call.
    ///   - decision: The strict typed decision, limited to deny or once.
    /// - Returns: That answer call's result or refusal.
    [[nodiscard]] Codec::JSON::Value answerStructureConsent(const Client& approver, const Protocol::Control::DecodedRequest& request, Protocol::Sync::ConsentChoice decision);

    /// Decides only the exact retained question and its recorded operation kind.
    /// - Parameters:
    ///   - approver: A live subscribed person, never an agent.
    ///   - request: The bounded canonical consent response Call.
    /// - Returns: Its correlated result or refusal.
    [[nodiscard]] Codec::JSON::Value serveConsentResponse(const Client& approver, const Protocol::Control::DecodedRequest& request);
    /// Takes a recipient that could not show a question off it.
    ///
    /// The question stays with the others it was asked of, so one recipient
    /// with no room never refuses it for a person looking at it elsewhere.
    /// Once none is left, it is refused at once rather than at its deadline:
    /// a capability is asked about again on its next reach, a tree change's
    /// caller hears it was not allowed, and an attach or termination is
    /// denied.
    ///
    /// - Parameters:
    ///   - approver: The recipient.
    ///   - request: The canonical call, for its answer.
    ///   - response: The question's kind and request ID.
    /// - Returns: The call's answer.
    [[nodiscard]] Codec::JSON::Value declineUnshownConsent(const Client& approver, const Protocol::Control::DecodedRequest& request, const Protocol::Control::ConsentResponse& response);
    /// How a recipient leaves a question it was asked.
    enum class QuestionLeave : std::uint8_t {
        /// It declined to show the question, and stays connected.
        Declined,
        /// Its connection ended. Whether its person may return is read from
        /// the attachment table: a lost link leaves the attachment resumable.
        Departed,
        /// The host superseded its connection, having moved the attachment
        /// to another one, where the person is asked again.
        Continued,
    };
    /// What taking a recipient off a question found.
    enum class QuestionLeaving : std::uint8_t {
        /// The question is no longer pending, or past its deadline.
        Gone,
        /// The client was not one it was asked of.
        NotAsked,
        /// The client is off it; the question stays, or is refused when no
        /// one is left who could show it.
        Left,
    };
    /// Takes a recipient off a question's list in the way it leaves. It
    /// stays listed with how it left, so a decliner is not asked again and a
    /// person who left is known to have been asked through its attachment.
    ///
    /// - Parameters:
    ///   - recipients: The question's recipients.
    ///   - clientID: The recipient leaving it.
    ///   - how: How it leaves.
    /// - Returns: Whether it was a recipient.
    static bool takeRecipientOff(std::vector<ConsentRecipient>& recipients, std::uint32_t clientID, QuestionLeave how);
    /// Whether some recipient is still asked: listed, connected and not
    /// declined.
    ///
    /// - Parameter recipients: The question's recipients.
    /// - Returns: Whether the question waits on anyone listed.
    static bool isStillAsked(std::span<const ConsentRecipient> recipients);
    /// Whether the attachment a recipient was asked through can be resumed:
    /// it is lost, or its connection is being dropped, which leaves it lost.
    ///
    /// - Parameter attachmentID: The attachment.
    /// - Returns: Whether its person may resume it and be asked again.
    [[nodiscard]] bool mayAttachmentResume(Protocol::Sync::ID128 attachmentID) const;
    /// Whether one recipient that left a question may return to it.
    ///
    /// - Parameters:
    ///   - recipient: The recipient.
    ///   - recipients: Every recipient of the question, for a continuation
    ///     whose new connection declined.
    ///   - isAskable: Says whether a client can be asked this question.
    /// - Returns: Whether its person may return.
    template<typename IsAskable>
    [[nodiscard]] bool mayRecipientReturn(const ConsentRecipient& recipient, std::span<const ConsentRecipient> recipients, const IsAskable& isAskable) const;
    /// Whether a person who left a question may return to it: the attachment
    /// it was asked through can be resumed, or the host continued it on
    /// another connection that can be asked and has not declined. A resume
    /// asks the question again on attaching.
    ///
    /// - Parameters:
    ///   - recipients: The question's recipients.
    ///   - isAskable: Says whether a client can be asked this question.
    /// - Returns: Whether the question waits on someone who may return.
    template<typename IsAskable>
    [[nodiscard]] bool mayAskedPersonReturn(std::span<const ConsentRecipient> recipients, const IsAskable& isAskable) const;
    /// Whether a recipient stays listed when a question is asked again: one
    /// still asked whose client can be asked or whose attachment can be
    /// resumed, a decliner whose connection lasts, or one that left and may
    /// return.
    ///
    /// - Parameters:
    ///   - recipient: The listed recipient.
    ///   - recipients: Every recipient of the question, for a continuation.
    ///   - isAskable: Says whether a client can be asked this question.
    /// - Returns: Whether to keep it on the list.
    template<typename IsAskable>
    [[nodiscard]] bool isRecipientKept(const ConsentRecipient& recipient, std::span<const ConsentRecipient> recipients, const IsAskable& isAskable) const;
    /// Takes a recipient off a capability question, refusing it when no one
    /// is still asked, so it is asked about again on its capability's next
    /// reach. Nothing offers it to a person who resumes.
    ///
    /// - Parameters:
    ///   - requestID: The question.
    ///   - clientID: The recipient leaving it.
    ///   - how: How it leaves.
    /// - Returns: What leaving found.
    QuestionLeaving leaveCapabilityQuestion(std::uint32_t requestID, std::uint32_t clientID, QuestionLeave how);
    /// Takes a recipient off a tree change's question, refusing it to its
    /// caller when no one is still asked and no person asked of it may
    /// return.
    ///
    /// - Parameters:
    ///   - requestID: The question.
    ///   - clientID: The recipient leaving it.
    ///   - how: How it leaves.
    /// - Returns: What leaving found.
    QuestionLeaving leaveStructureQuestion(std::uint32_t requestID, std::uint32_t clientID, QuestionLeave how);
    /// Takes a recipient off an attach or termination question, denying it
    /// when no one is still asked, unless an answer already given stands or
    /// a person asked of it may return, since a person who resumes is asked
    /// again.
    ///
    /// - Parameters:
    ///   - requesterIndex: The client the question is about.
    ///   - clientID: The recipient leaving it.
    ///   - how: How it leaves.
    /// - Returns: What leaving found.
    QuestionLeaving leaveClientQuestion(std::size_t requesterIndex, std::uint32_t clientID, QuestionLeave how);
    /// Takes a departing client off every question it was asked.
    ///
    /// - Parameters:
    ///   - clientID: The departing client.
    ///   - how: Whether the host continued its attachment elsewhere.
    void leaveEveryQuestion(std::uint32_t clientID, QuestionLeave how);
    /// Whether a client shows a person the questions this host asks; one
    /// that says it shows none is never asked.
    ///
    /// - Parameter client: The client.
    /// - Returns: Whether it is shown questions.
    [[nodiscard]] bool showsQuestions(const Client& client) const;

    /// Revokes server-derived authority on the person's exact subscribed session.
    /// - Parameters:
    ///   - approver: A live subscribed person, never an agent.
    ///   - request: The bounded canonical consent revocation Call.
    /// - Returns: Its correlated result or refusal.
    [[nodiscard]] Codec::JSON::Value serveConsentRevocation(const Client& approver, const Protocol::Control::DecodedRequest& request);

    /// Refuses expired questions and forgets disconnected requesters.
    ///
    /// - Parameter now: The daemon clock, in milliseconds.
    void expireStructureConsents(std::uint64_t now);
    /// Admits one correlated termination, or retains it for human consent.
    /// - Parameters:
    ///   - index: The live requesting client.
    ///   - callIndex: Its outstanding typed call.
    ///   - request: The immutable operation parameters and correlation identity.
    /// - Returns: An immediate refusal, or no value when completion owns the call.
    [[nodiscard]] std::optional<Codec::JSON::Value> beginTerminationCall(std::size_t index, std::uint32_t callIndex, const Protocol::Control::DecodedRequest& request);
    /// Consumes one held operation, rechecking authority before committing it.
    /// - Parameters:
    ///   - index: The original requesting client.
    ///   - outcome: The terminal result word, including denial and timeout.
    void completeTerminationCall(std::size_t index, std::string_view outcome);
    /// Retains the exact attached owner and target, without releasing a stream.
    [[nodiscard]] std::optional<Codec::JSON::Value> beginReclaimCall(std::size_t index, std::uint32_t callIndex, const Protocol::Control::DecodedRequest& request);
    /// Revalidates and releases outside all decoder and service borrows.
    void completeReclaimCall(std::size_t index);
    /// Checks the same live attachment, principal, scope ceiling and adopted pane.
    [[nodiscard]] bool mayCompleteReclaim(const Client&) const;

    /// Effects applied only after a correlated administration answer has been
    /// queued and no dispatcher still borrows a client or connection.
    struct AdministrationEffects {
        std::vector<std::uint32_t> clientsToDisconnect;
        std::vector<Protocol::Sync::ID128> attachmentsToRetire;
        bool shouldReverifyTokens = false;
        [[nodiscard]] bool isPending() const { return shouldReverifyTokens || !clientsToDisconnect.empty() || !attachmentsToRetire.empty(); }
    };
    /// Admits, pins and answers an administration call before disconnecting.
    /// - Parameters:
    ///   - index: The requesting client's stable slot during this pass.
    ///   - callIndex: The connection's outstanding call number.
    ///   - request: The decoded immutable method, parameters and identity.
    ///   - effects: Pass-owned security cleanup, even if the transport fails.
    /// - Returns: An immediate refusal, or nothing once the call is answered.
    [[nodiscard]] std::optional<Codec::JSON::Value> beginHostAdministrationCall(std::size_t index, std::uint32_t callIndex, const Protocol::Control::DecodedRequest& request, AdministrationEffects& effects);
    /// Performs an already admitted operation with the same host-owned stores.
    /// - Parameters:
    ///   - actor: The original live and authorized connection.
    ///   - request: The exact bounded selectors and daemon incarnation.
    ///   - effects: Disconnects and token revalidation owed after the answer.
    /// - Returns: The owned snapshot or operation outcome, never logged.
    [[nodiscard]] Protocol::Sync::ManagementReply performHostAdministration(const Client& actor, const Protocol::Sync::ManagementRequest& request, AdministrationEffects& effects);

    /// Ends what a served call left to end: the sessions of a window it
    /// closed, or of a workspace whose only window it closed. Run by the
    /// loop between passes, where no connection is being served.
    void settleOwedClosures();
    /// Consumes a transport read, admitting each header against retained-byte
    /// budgets before allocation and stopping at the first deferred drop.
    ///
    /// - Parameters:
    ///   - index: The client owning the transport read.
    ///   - bytes: The borrowed bytes to consume during this call.
    void feedClientFrames(std::size_t index, std::span<const std::byte> bytes);
    /// Checks replacement of one charged ingress allocation by a new capacity.
    [[nodiscard]] bool admitsInboundCapacity(std::size_t index, std::size_t previous, std::size_t capacity) const;
    /// How much more input the host's panes take unwritten, all together.
    ///
    /// - Returns: What is left of the host's bound.
    [[nodiscard]] std::size_t heldInputRoom() const;
    /// Takes the pixels a client's driver draws its panes in from the
    /// `Viewport`s it sent since the last take. They are the client's own
    /// until it owns a pane's size: the program reads the owner's as
    /// ws_xpixel and ws_ypixel and in CSI 14/16 t. A client that has
    /// measured none sent zeros, which leave the last answer standing. The
    /// grid of each went to its pane's arbiter as the frame was read.
    ///
    /// - Parameter index: The client.
    void adoptViewportPixels(std::size_t index);

    /// Authenticates one canonical greeting and resolves its exact serving target.
    /// The connection invokes this before publishing Welcome or pane authority;
    /// client removal remains deferred until its decoder has returned.
    [[nodiscard]] std::expected<Protocol::Sync::HostIdentity, Protocol::Sync::Bye> admitGreeting(
        std::uint32_t clientID, const Protocol::Sync::Greet&);
    // After draining a client's inbound decoder: release retained
    // capacity when fully drained, and arm/disarm the partial-frame deadline.
    void noteInboundDecoder(std::size_t index);
    // Enforce the per-client + aggregate inbound-byte caps and the partial-frame
    // deadline; drop offenders. Runs once per loop tick.
    void enforceInboundBudget();

    /// The bytes a client has the daemon hold for what it has not finished
    /// sending: a selected reader's partial frame, an adopt assembly, typed
    /// clipboard owners and retained consent or control state.
    ///
    /// - Parameter client: The client.
    /// - Returns: The bytes, as the inbound budget counts them.
    [[nodiscard]] static std::size_t heldInboundBytes(const Client& client);
    /// Whether an additional assembly allocation fits the aggregate ledger.
    /// Unlike a frame allocation, an assembly has its own per-message cap.
    [[nodiscard]] bool admitsAssemblyCapacity(std::size_t index, std::size_t additional) const;

    // Enforce the aggregate OUTBOUND-byte cap across all clients (the per-client
    // high-water alone admits N x 16 MiB); drop newest-first. Runs once per tick.
    void enforceOutboundBudget();

    /// Drains at most the leased byte count, retaining ordinary pass limits.
    /// Marks changed frames due and records a departed shell.
    /// - Parameters:
    ///   - s: The session whose output is parsed.
    ///   - maxBytes: A smaller read budget during handover, or the default.
    /// - Returns: The actual read result, including would-block and byte count.
    Runtime::DrainPassResult servicePTY(Session& s, int maxBytes = std::numeric_limits<int>::max());

    /// Refreshes a pane's driving faces and releases any bounded measure wait.
    ///
    /// - Parameter session: The pane whose drivers and pending replies are read.
    void refreshRunFaces(Session& session);

    /// Starts a bounded attempt without accepting any new client input.
    /// - Parameter now: Monotonic microseconds at the start of the attempt.
    /// - Returns: Whether every live pane can participate in the attempt.
    [[nodiscard]] bool beginHandoverQuiesce(std::uint64_t now);

    /// Refuses physical-stream operations that cannot be rebound on resume.
    /// Accepted pane input and its owned PTY suffix are carried, not blockers.
    /// - Returns: Whether ordinary service must finish existing operations first.
    [[nodiscard]] bool hasHandoverBlockedOperations() const { return handoverBlocker().has_value(); }

    /// Names the first operation that keeps this host from handing over, as
    /// the refusal its successor and the person running the install are told.
    /// - Returns: The blocker, or `nullopt` when nothing blocks a handover.
    [[nodiscard]] std::optional<Protocol::Sync::HandoverRefuseReason> handoverBlocker() const;

    /// Names the operation that ended the last quiesce attempt, when one did:
    /// a pane that missed ground is not named here.
    /// - Returns: The blocker, or `nullopt`.
    [[nodiscard]] std::optional<Protocol::Sync::HandoverRefuseReason> handoverQuiesceBlocker() const { return m_handoverQuiesceBlocker; }

    /// Advances PTY output only, holding each pane at its accepted boundary.
    /// A deadline failure restores normal service for the whole daemon.
    /// - Parameter now: The monotonic observation time.
    void serviceHandoverQuiesce(std::uint64_t now);

    /// Validates the whole host's frozen publications.
    /// - Returns: Whether all participating panes are frozen at ground.
    [[nodiscard]] bool isQuiesced() const;

    /// Abandons the attempt without consuming held client input.
    void endHandoverQuiesce();
    void drainTermToPTY(Session& s);

    /// Returns the wait until a quiet session can fold.
    ///
    /// - Parameter nowMilliseconds: The current monotonic time.
    /// - Returns: Milliseconds until maintenance is due, or -1 without work.
    [[nodiscard]] int scrollbackMaintenanceTimeout(std::uint64_t nowMilliseconds) const;
    /// Folds one cold block across all sessions, rotating by session identity.
    ///
    /// - Parameter nowMilliseconds: The current monotonic time.
    void serviceScrollbackMaintenance(std::uint64_t nowMilliseconds);

    /// Subscribes a client to its session, or to every pane of its workspace,
    /// and sends what an attach owes it before its screen plane binds.
    ///
    /// - Parameters:
    ///   - clientIndex: The client.
    ///   - session: The session it attached to.
    void seedInitialAttachState(std::size_t clientIndex, Session& session);
    /// Publishes one pane's state after its workspace topology is queued.
    ///
    /// - Parameter session: The authoritative hosted pane.
    void seedPaneState(Session& session);
    /// The topology above a named session's leaves (Docs/session-model.md §6).
    ///
    /// A daemon Session is a pane's leaf backing; this is what sits above them.
    /// Identity and lifetime belong to this record, independently of any
    /// session backing a pane. SESSION_LIST and a workspace's structure read it. Every pane
    /// the model holds is bound to a Session here, minted when a verb splits or
    /// adds one, so a layout never names a pane nothing draws into.
    struct HostedWorkspace {
        /// Creates the workspace around root ids minted from the host's allocator.
        ///
        /// - Parameters:
        ///   - identities: The allocator every hosted workspace shares.
        ///   - root: The root's surface, tab, and pane.
        HostedWorkspace(std::shared_ptr<Ubiquitty::Session::Identity::Allocator> identities, Ubiquitty::Session::Layout::RootIdentity root)
            : layout(std::move(identities), root)
        {
        }

        /// Creates the workspace with a root minted from the host's allocator.
        ///
        /// - Parameter identities: The allocator every hosted workspace shares.
        explicit HostedWorkspace(std::shared_ptr<Ubiquitty::Session::Identity::Allocator> identities)
            : layout(std::move(identities))
        {
        }

        /// Remains stable when the initial session or pane ends.
        uint32_t workspaceID = 0;
        std::string name;
        bool shouldEndWhenDetached = false;
        bool isClosing = false;
        /// The workspace was recreated from its record after the host restarted.
        bool wasRestoredAfterHostRestart = false;
        /// The daemon instances that held the workspace before this one,
        /// newest first, at most `maxHostLayoutHolders`, each with the id it
        /// held the workspace under: what its record names besides this
        /// instance, so a client that saved any of them can still bring it
        /// back, or reach it while it runs here (`findWorkspaceReference`).
        std::vector<Ubiquitty::Session::HostLayoutHolder> formerHolders;
        /// When a host last held the workspace, in Unix seconds, to the
        /// record's resolution: what ranks its record for eviction.
        std::uint64_t heldUnixSeconds = 0;
        /// The workspace as the record file last held it.
        ///
        /// What becomes restorable when the workspace's last pane is lost
        /// rather than ended: by then the panes lost before it have already
        /// left the live tree, and the arrangement worth keeping is the one
        /// from before the loss began.
        std::optional<Ubiquitty::Session::HostLayoutRecord> recordedLayout;
        Ubiquitty::Session::Layout::Workspace layout;
        /// Every pane the model holds has a session here; a pane leaves the model
        /// and its session ends together.
        std::vector<std::pair<Ubiquitty::Session::Layout::PaneID, uint32_t>> paneSessions;
        [[nodiscard]] uint32_t sessionFor(Ubiquitty::Session::Layout::PaneID pane) const;
        [[nodiscard]] Ubiquitty::Session::Layout::PaneID paneFor(uint32_t sessionID) const;
    };
    /// Owns every unpublished local-adoption object through ingress delivery.
    /// No client or payload borrow survives here. Dropping this owner only
    /// releases private state and the duplicate master, never a live session.
    struct LocalAdoptCandidate final : Ubiquitty::Session::AdoptCandidate {
        LocalAdoptCandidate(Daemon&, std::unique_ptr<Session>, std::unique_ptr<HostedWorkspace>, PAL::PTYAdoptionIdentity);
        ~LocalAdoptCandidate() override;
        [[nodiscard]] Ubiquitty::Session::AdoptTarget target() const override { return m_target; }

        Daemon& daemon;
        const Ubiquitty::Session::AdoptTarget m_target;
        const PAL::PTYAdoptionIdentity endpoint;
        std::unique_ptr<Session> session;
        std::unique_ptr<HostedWorkspace> workspace;
        /// The decoded terminal's charge to the daemon's decode owner, held
        /// until this unpublished candidate is dropped.
        Protocol::Sync::MemoryCharge decodeCharge;
    };
    /// The exact output capacity admitted before the table records Committed.
    struct LocalAdoptCommitReply final : Ubiquitty::Session::AdoptCommitReservation {
        std::array<std::uint8_t, Protocol::Local::adoptWireOutcomeBytes> bytes;
        std::uint32_t clientID = 0;
    };
    class LocalAdoptDriver;
    [[nodiscard]] std::expected<Ubiquitty::Session::AdoptAuthorityLease, Ubiquitty::Session::AdoptTransactionError> admitLocalAdoptAuthority(std::size_t, LocalAdoptDriver&);
    [[nodiscard]] Ubiquitty::Session::AdoptRetirementEvidence localAdoptRetirementEvidence(const Ubiquitty::Session::AdoptAuthority&);
    void serviceLocalAdoptRetirement();
    /// Whether a daemon-verified person still has Drive authority and an
    /// attached pane in the workspace this exact operation affects.
    ///
    /// - Parameters:
    ///   - client: The proposed approver.
    ///   - workspace: The affected workspace.
    /// - Returns: Whether this person may approve its operation.
    [[nodiscard]] bool mayApproveStructure(const Client& client, const HostedWorkspace& workspace) const;
    /// Whether a person who may approve a workspace's operation can also be
    /// shown the question: one whose client shows no questions holds the
    /// authority and is present, but is never asked.
    ///
    /// - Parameters:
    ///   - client: The proposed approver.
    ///   - workspace: The affected workspace.
    /// - Returns: Whether this person may be asked to approve its operation.
    [[nodiscard]] bool mayBeAskedToApproveStructure(const Client& client, const HostedWorkspace& workspace) const;

    /// Whether the client still owns the live attachment making or approving
    /// a tree change; a resumed attachment no longer belongs to its old stream.
    ///
    /// - Parameter client: The connection to check.
    /// - Returns: Whether this connection still holds the attachment.
    [[nodiscard]] bool hasLiveStructureAttachment(const Client& client) const;
    std::vector<std::unique_ptr<HostedWorkspace>> m_workspaces;
    /// Finds the workspace holding `session` as a pane, making one on first need.
    ///
    /// A session no workspace holds roots a fresh one, itself its only pane.
    ///
    /// - Parameter session: A live session.
    /// - Returns: The workspace holding it.
    HostedWorkspace& hostedWorkspaceFor(Session& session);
    /// Finds the workspace holding a session as a pane, the root's included.
    ///
    /// - Parameter sessionID: The session's id.
    /// - Returns: The workspace, or null when no workspace holds the session.
    [[nodiscard]] HostedWorkspace* workspaceHolding(uint32_t sessionID);
    /// Finds where a session's pane stands in its tab: beside other panes,
    /// its size is its place among them and no window's size is its own.
    ///
    /// - Parameter sessionID: The session's id.
    /// - Returns: `BesideOthers` when the tab holding the pane holds more
    ///   than one, and `WholeWindow` otherwise, as for a session no
    ///   workspace holds.
    [[nodiscard]] Protocol::Sync::PanePlacement placementOf(uint32_t sessionID) const;
    /// Why a client may not change a workspace it names, in words that say what
    /// would let it. A person on another machine is told which workspace
    /// its connection is attached to. An agent is told of the session a
    /// person is attached to, which no grant or consent answers, or of the
    /// session it holds no grant for and how a grant is given.
    ///
    /// - Parameters:
    ///   - client: The asking client.
    ///   - workspace: The workspace it names.
    /// - Returns: Nothing when the client may change it, or the reason.
    [[nodiscard]] std::optional<std::string> rearrangeRefusal(const Client& client, const HostedWorkspace& workspace);

    /// Where an identity stands in the host's model.
    struct LocatedObject {
        /// The live workspace holding it; never null.
        HostedWorkspace* workspace = nullptr;
        /// The window it is or stands in; invalid for a workspace.
        Ubiquitty::Session::Layout::SurfaceID surface = Ubiquitty::Session::Layout::invalidSurface;
        /// The tab it is or stands in; invalid for a workspace or a window.
        Ubiquitty::Session::Layout::TabID tab = Ubiquitty::Session::Layout::invalidTab;
    };

    /// Finds the live workspace, window, tab, pane or session an identity
    /// names. A session no workspace holds yet is given the workspace every
    /// reader of the host's model sees it in.
    ///
    /// - Parameter id: The identity.
    /// - Returns: Where it stands, or nothing for an identity that names
    ///   nothing live, a client, or a workspace that is closing.
    [[nodiscard]] std::optional<LocatedObject> locateObject(Ubiquitty::Session::Identity::ObjectID id);

    /// Gives a workspace its name, or takes it away with an empty one.
    ///
    /// - Parameters:
    ///   - workspace: The workspace.
    ///   - name: The name.
    /// - Returns: Nothing when the workspace now has the name, or the reason
    ///   it does not: a name longer than a structure carries, or one another
    ///   workspace, live or restorable, is reached by.
    [[nodiscard]] std::optional<std::string> renameWorkspace(HostedWorkspace& workspace, std::string_view name);

    /// Gives a tab its name, or takes it away with an empty one.
    ///
    /// - Parameters:
    ///   - workspace: The workspace holding the tab.
    ///   - surface: The window the tab stands in.
    ///   - tab: The tab.
    ///   - name: The name.
    /// - Returns: Nothing when the tab now has the name, or the reason it
    ///   does not.
    [[nodiscard]] std::optional<std::string> renameTab(HostedWorkspace& workspace, Ubiquitty::Session::Layout::SurfaceID surface, Ubiquitty::Session::Layout::TabID tab, std::string_view name);
    /// Applies the model side of one verb.
    ///
    /// - Parameters:
    ///   - clientIndex: The asking client, whose scope a minted pane's session is charged to.
    ///   - workspace: The workspace the verb acts on.
    ///   - attached: The asking client's session; a pane the verb mints takes its geometry.
    ///   - verb: The validated local intent for the canonical operation.
    ///   - result: Filled with what the verb minted.
    /// - Returns: Whether the verb was performed.
    bool performWorkspaceVerb(std::size_t clientIndex, HostedWorkspace& workspace, Session& attached, const Ubiquitty::Session::WorkspaceIntent& verb, Ubiquitty::Session::WorkspaceIntentResult& result);
    /// Ends the session behind every pane the model no longer holds.
    ///
    /// - Parameter workspace: The workspace whose bindings are reconciled.
    void reapVanishedPanes(HostedWorkspace& workspace);
    /// How a session came to end, as far as its workspace's record is concerned.
    enum class SessionEnding : std::uint8_t {
        /// It ended or was ended: its program exited, a person closed it, or a
        /// client terminated it. The record follows the live workspace.
        Concluded,
        /// Its program was killed by a signal, which is how a crash or a
        /// machine shutting down ends it. The record keeps what it held.
        Lost,
    };
    /// Takes an ended session's pane out of its workspace.
    ///
    /// Every session is a leaf, so ending one never ends its siblings. The
    /// workspace goes with its last pane; a recorded one whose last pane is
    /// lost stays restorable under the arrangement last written for it.
    ///
    /// - Parameters:
    ///   - sessionID: The session that ended.
    ///   - ending: How it ended.
    void releaseWorkspaceSession(uint32_t sessionID, SessionEnding ending);

    /// A workspace the host recorded before it restarted, named or not,
    /// which nothing runs in until a client attaches to it.
    struct RestorableWorkspace {
        /// The id it is listed and attached by, from the session id space so
        /// that no session or workspace of this host ever takes it.
        uint32_t workspaceID = 0;
        Ubiquitty::Session::HostLayoutRecord record;
    };
    /// The workspaces this host could restore, the most recently held first;
    /// with the recorded live workspaces, at most `Session::maxHostLayoutRecords`.
    std::vector<RestorableWorkspace> m_restorableWorkspaces;
    /// The file this host's records live in: its socket's path and `.layouts`.
    std::string m_layoutRecordPath;
    /// When the records are next written.
    TrailingWrite m_layoutRecordWrite;
    /// The bytes the record file holds, so a write that would change nothing
    /// is skipped; empty when there is no file.
    std::vector<std::uint8_t> m_writtenLayoutRecords;

    /// Reports whether a workspace is one the host keeps a record of.
    ///
    /// Every workspace that outlives its clients, named or not: a client
    /// returns to an unnamed one by the reference it saved. A workspace that
    /// ends with its last person is not recorded: nobody returns to it.
    [[nodiscard]] static bool isRecordedWorkspace(const HostedWorkspace& workspace);
    /// Names a workspace in a log line: by its name, or by its id when it
    /// has none.
    ///
    /// - Parameters:
    ///   - name: The workspace's name, empty when it has none.
    ///   - workspaceID: Its id.
    /// - Returns: The words for it.
    [[nodiscard]] static std::string describeWorkspace(std::string_view name, uint32_t workspaceID);
    /// Keeps a workspace restorable, letting the least recently held go
    /// when the host holds more records than it keeps.
    ///
    /// - Parameter restorable: The workspace and its record.
    void keepRestorableWorkspace(RestorableWorkspace&& restorable);
    /// Orders the restorable workspaces the most recently held first and
    /// drops those past what a record file keeps beside the live ones.
    void trimRestorableWorkspaces();
    /// False once this host found a record file it could neither read nor
    /// set aside: it then writes none, so the file is not replaced.
    bool m_isLayoutRecordKept = true; // bool-ok: names the one state it reports
    /// Reads this host's record file, and the files of hosts on sibling
    /// sockets that are no longer running, into the restorable workspaces.
    void loadLayoutRecords();
    /// Moves this host's record file, which it could not read, to the same
    /// name with `.unread` after it, and logs the file, the reason and where
    /// it is kept. Files set aside before each move a place older
    /// (`Session::unreadHostLayoutFilePath`), and the oldest of
    /// `Session::maxUnreadHostLayoutFiles` is removed with a log line. A
    /// file that cannot be moved stops this host writing one.
    ///
    /// - Parameter reason: Why the file was not read, as a clause.
    void setAsideUnreadLayoutRecords(std::string_view reason);
    /// Takes in the records of a host on another socket of this directory that
    /// is not running, as a host replaced by another binary leaves them.
    ///
    /// - Parameter path: The other host's record file.
    /// - Returns: Whether the file was read and may be removed.
    bool adoptSiblingLayoutRecords(const std::string& path);
    /// Takes in the record files of every host on a sibling socket that is
    /// not running, writes this host's file, and removes theirs. A host does
    /// this as it starts, and again when a saved reference to an instance
    /// that stopped after it started finds no record here; each pass reads at
    /// most `maxSiblingLayoutRecordScan` entries of the directory, and each
    /// file within the bounds a record file is read with.
    ///
    /// - Returns: Whether any file was taken in.
    bool takeInStoppedSiblingRecords();
    /// Assigns restorable workspaces ids and keeps those whose names are new.
    ///
    /// - Parameter records: Decoded records, in file order.
    void admitRestorableRecords(std::vector<Ubiquitty::Session::HostLayoutRecord>&& records);
    /// Removes the record file this host wrote at startup once a takeover
    /// has moved its records to the file the chain keeps beside its first
    /// socket, which this host writes from then on.
    ///
    /// The chain's file is written first, so a record is always in one of
    /// the two; a write that fails leaves the startup file standing.
    ///
    /// - Parameter startupPath: The record file this host had before the takeover.
    void retireStartupLayoutRecords(const std::string& startupPath);
    /// Describes a live workspace as its record.
    ///
    /// - Parameter workspace: A recorded workspace.
    /// - Returns: The record, or nothing when it holds more panes than a
    ///   record may.
    [[nodiscard]] std::optional<Ubiquitty::Session::HostLayoutRecord> captureLayoutRecord(HostedWorkspace& workspace);
    /// Owes a write of the records, after the debounce interval.
    void scheduleLayoutRecordWrite();
    /// Owes a write when `workspace` is recorded.
    ///
    /// - Parameter workspace: The workspace whose topology changed.
    void scheduleLayoutRecordWrite(const HostedWorkspace& workspace);
    /// Writes every recorded workspace and every restorable one to the record
    /// file, atomically and readable by its owner alone, or removes the file
    /// when there is nothing to record.
    ///
    /// - Returns: Whether the file now holds what this host records.
    bool writeLayoutRecords();
    /// Finds a restorable workspace by the id it is listed under.
    ///
    /// - Parameter workspaceID: The listed id.
    /// - Returns: The workspace, or null.
    [[nodiscard]] RestorableWorkspace* findRestorableWorkspace(uint32_t workspaceID);
    /// Finds a restorable workspace by name.
    ///
    /// - Parameter workspaceName: The name; empty reaches nothing.
    /// - Returns: The workspace, or null.
    [[nodiscard]] RestorableWorkspace* findRestorableWorkspaceByName(std::string_view workspaceName);
    /// Whether a reference lookup searches a kind of workspace.
    enum class ReferenceSearch : std::uint8_t {
        Included,
        Excluded,
    };
    /// The one workspace a client's saved reference names here, if any.
    struct WorkspaceReference {
        /// The id a record that answers it is listed under; zero for none.
        uint32_t recordedWorkspaceID = 0;
        /// The live workspace that answers it; null for none.
        HostedWorkspace* live = nullptr;

        /// Whether a workspace answers the reference.
        [[nodiscard]] bool isFound() const { return recordedWorkspaceID || live; }
    };
    /// Finds the workspace a client's saved reference names: a record of it,
    /// or the workspace live here, which a handover brought or an earlier
    /// reference recreated from its record.
    ///
    /// Each names the reference by the instances that held the workspace and
    /// the ids they held it under, the record by its holders and the live
    /// workspace by its former holders, both at most `maxHostLayoutHolders`,
    /// rather than by the id it is listed under here. A reference that two of
    /// them claim cannot be both, so neither answers it.
    ///
    /// - Parameters:
    ///   - daemonInstanceID: The instance the client saved.
    ///   - workspaceID: The workspace id the client saved.
    ///   - records: Whether records are searched.
    ///   - live: Whether live workspaces are searched.
    /// - Returns: The one workspace the reference names, or none when none
    ///   does or more than one does.
    [[nodiscard]] WorkspaceReference findWorkspaceReference(std::uint64_t daemonInstanceID, uint32_t workspaceID, ReferenceSearch records, ReferenceSearch live);
    /// The session a reference to a live workspace is answered with: the one
    /// the reference names while it runs there, else the focused pane's,
    /// else the first pane's.
    ///
    /// - Parameters:
    ///   - workspace: The live workspace.
    ///   - requestedSessionID: The session the reference names, or zero when
    ///     its number means nothing here.
    /// - Returns: The session, or null when the workspace runs none.
    [[nodiscard]] Session* answeringSession(const HostedWorkspace& workspace, uint32_t requestedSessionID);
    /// Recreates a restorable workspace: its arrangement, and a fresh session
    /// behind every pane in the pane's recorded directory at its recorded grid.
    ///
    /// The record is consumed either way. A pane whose session cannot be
    /// started leaves the arrangement; a record nothing can be started from
    /// is logged and dropped.
    ///
    /// - Parameter workspaceID: The restorable workspace's listed id.
    /// - Returns: The session behind the focused pane of the active window, or
    ///   null when nothing could be restored.
    Session* restoreWorkspace(uint32_t workspaceID);
    /// Owes a write of the records when a person may have changed directory.
    ///
    /// A shell that reports no directory by OSC 7 changes it silently, so a
    /// line of input to a recorded workspace is the one cheap sign that the
    /// directory the record holds may be stale. The write reads it afresh,
    /// and comes a whole debounce interval after the line: a write already
    /// owed for something else may come before the shell has read it.
    ///
    /// - Parameters:
    ///   - session: The session the input went to.
    ///   - bytes: The input.
    void noteLayoutInput(const Session& session, std::span<const std::uint8_t> bytes);
    /// Notes that a workspace's arrangement changed, and has every carrier
    /// of the workspace take up the panes it gained: each is subscribed,
    /// seeded and, once the structure naming it has left, bound.
    ///
    /// - Parameter workspace: The workspace that changed.
    void admitWorkspacePanes(HostedWorkspace& workspace);
    /// How far behind a client's outbound stream is, across both transports.
    ///
    /// The staging buffer is the whole of it for an AF_UNIX client. For a QUIC
    /// one `flushClient` empties that buffer into the connection on every flush,
    /// so the bytes still waiting are the transport's, and a measure that reads
    /// only the buffer reports a stalled QUIC reader as keeping up.
    ///
    /// - Parameter c: The client to measure.
    /// - Returns: Bytes queued for `c` and not yet sent.
    /// Whether `c` may be asked to rule on a consent request for `sessionID`,
    /// and whether its answer counts.
    ///
    /// One rule rather than one per site: the ask and the answer have to agree,
    /// and an ask narrower than its answer leaves a client entitled to rule that
    /// is never asked. A capability grant is recorded as pending when it is
    /// raised and is not offered twice, so a prompt nobody receives is a grant
    /// nobody can ever give.
    ///
    /// - Parameters:
    ///   - c: The client to test.
    ///   - sessionID: The session the request belongs to.
    /// - Returns: Whether `c` is a non-agent client attached to that session.
    /// Asks this session's rulers to rule on every capability grant newly raised.
    ///
    /// A grant is recorded as pending when the policy raises it and is not
    /// offered twice, so a request nobody receives is a grant nobody can give.
    ///
    /// - Parameter s: The session whose queue to drain.
    void sendCapabilityGrantRequests(Session& s);
    /// Retires every capability question past its deadline as unanswered.
    ///
    /// The host keeps no grant for it, so the capability stays refused, and a
    /// later reach for it is asked about again. A clipboard write held for
    /// the question is discarded with it.
    ///
    /// - Parameter now: The monotonic expiry clock, in milliseconds.
    void expireCapabilityQuestions(std::uint64_t now);
    /// Cancels exact clipboard operations bound to a departing endpoint.
    ///
    /// - Parameter clientID: The endpoint's physical connection identity; zero is ignored.
    void cancelPendingClipboardWrites(std::uint32_t clientID);
    /// Releases a session's held clipboard payload when its request matches.
    ///
    /// - Parameters:
    ///   - requestID: The consent request identifying the exact operation.
    ///   - sessionID: The session holding that operation, if it still exists.
    void discardPendingClipboardWrite(std::uint32_t requestID, std::uint32_t sessionID);
    /// The record a capability question is kept by; declared with the map.
    struct PendingCapabilityGrant;
    /// Retires one capability question unanswered, the one way every path
    /// does: its recipients dismiss it, a clipboard write held for it is
    /// discarded, and its capability is asked about again on its next reach.
    /// The caller erases the record.
    ///
    /// - Parameters:
    ///   - requestID: The question.
    ///   - pending: Its record.
    void retireCapabilityQuestion(std::uint32_t requestID, const PendingCapabilityGrant& pending);
    [[nodiscard]] bool mayCompleteClipboardWrite(const Session&, std::uint32_t sourceClientID,
        std::uint32_t recipientClientID, std::string_view identity) const;
    /// Delivers the clipboard writes and notifications the session's program
    /// made since the last call to its primary human, and drops them when
    /// there is none (Docs/sync.md §6.11).
    ///
    /// - Parameter s: The session whose effects to deliver.
    void deliverSessionEffects(Session& s);
    /// Elects the native owner and drains commands within its transport budget.
    void deliverSessionDragDrop(Session&);
    [[nodiscard]] bool mayReceiveNativeDrag(std::uint32_t clientID, std::uint32_t handle, std::uint64_t epoch) const;
    /// Installs bounded journal retention before the live effect queue can evict a notification.
    void wireBackgroundNotifications(Session&);
    /// Installs on a session's Terminal the clients the session owns: its
    /// clipboard, its shell, the route its bells ring and its clock; and
    /// makes it the terminal that answers its program's queries.
    ///
    /// Every path that gives a session a Terminal calls this, so a pane never
    /// runs with a process-wide effect sink or none.
    ///
    /// - Parameter s: The session, whose Terminal is set.
    static void installTerminalClients(Session& s);
    /// Finds the client the session's effects are delivered to: the human
    /// who typed last, or the only human driver attached.
    ///
    /// - Parameter s: The session.
    /// - Returns: The client's index, or `m_clients.size()` when there is none.
    [[nodiscard]] std::size_t primaryHumanForSession(const Session& s) const;
    /// Whether a client asks its own person about the clipboard writes and
    /// notifications a session delivers to it.
    ///
    /// Its word is enough: it is the recipient, and this host's question would
    /// have gone to it. What it is forwarded unasked is only what was gated
    /// at the Remote origin.
    ///
    /// - Parameter client: The recipient.
    /// - Returns: Whether its Greet said so.
    [[nodiscard]] bool asksAboutHostEffects(const Client& client) const;

    /// Resolves the immutable posting identity for a typed effect producer.
    ///
    /// - Parameter s: The session with an admitted effect reader.
    /// - Returns: No value before typed delivery is selected; zero when there
    ///   is no human primary, so readable command records grant no posting.
    [[nodiscard]] std::optional<Protocol::Sync::ID128> typedEffectPrimaryForSession(const Session& s) const;
    /// Appends a pending bell after its picture boundary has been assigned.
    /// Refusal leaves the terminal's pending bit available for a later retry.
    ///
    /// - Parameter s: The session whose current output has been published.
    void emitPendingBell(Session& s);
    /// Appends captured sounds in order, retaining the front on log refusal.
    ///
    /// - Parameter s: The session whose current output has been published.
    void emitPendingSounds(Session& s);

    /// Drops the oldest outstanding capability prompt when the table is full.
    ///
    /// The dropped triple is resolved in the consent queue as well as erased.
    /// The queue dedupes on that triple, so a record removed while it still
    /// counts as pending is a capability nothing asks about again — the next
    /// reach reads as a repeat and the policy answers Prompt with no prompt
    /// behind it.
    void evictOldestCapabilityGrantIfFull();

    /// Whether a client is a live person attached to a session, with the
    /// authority to rule on its questions. Being present and being shown
    /// questions are different facts: a person on a client that shows none
    /// still holds the session.
    ///
    /// - Parameters:
    ///   - c: The client.
    ///   - sessionID: The session.
    /// - Returns: Whether it may rule on the session's questions.
    [[nodiscard]] bool mayRuleOnConsentFor(const Client& c, std::uint32_t sessionID) const;
    /// Whether a person who may rule on a session's questions can be shown
    /// one, and so asked.
    ///
    /// - Parameters:
    ///   - c: The client.
    ///   - sessionID: The session.
    /// - Returns: Whether it may be asked the session's questions.
    [[nodiscard]] bool mayBeAskedFor(const Client& c, std::uint32_t sessionID) const;

    /// Bytes queued for a client and not yet handed to its transport.
    ///
    /// How far behind the client is: the screen plane's low water and the
    /// per-client drop cap read this. A QUIC client's bytes in flight are not in
    /// it; the congestion controller paces those, and the peer's flow-control
    /// window bounds them.
    ///
    /// - Parameter c: The client.
    /// - Returns: The queued byte count.
    [[nodiscard]] std::size_t outboundPendingBytes(const Client& c) const;

    /// Bytes held for a client until its peer has them: queued, plus a QUIC
    /// client's bytes in flight.
    ///
    /// A session that exits drains to this, since a QUIC connection closed with
    /// bytes in flight loses them.
    ///
    /// - Parameter c: The client.
    /// - Returns: The held byte count.
    [[nodiscard]] std::size_t outboundRetainedBytes(const Client& c) const;

    /// Allocated outbound payload capacity plus room for two bounded carrier
    /// frames, including sent prefixes, spare vectors and queued screen payloads.
    /// Source/handoff reservations are separate; empty retained buffers count.
    [[nodiscard]] std::size_t outboundPayloadCapacity(const Client&) const;
    /// Looks up a stable client identity and reads both source ledgers plus
    /// every other retained payload owner without mutating the connection.
    [[nodiscard]] Protocol::Sync::BulkSourceAdmission imageSourceAdmission(std::uint32_t clientID) const;
    /// Gives up lowest-priority pixel snapshots before higher-priority output
    /// grows into their reserved room. Existing nonimage queue policy is kept.
    void preemptOutboundImages();
    /// Keeps carrier storage charged across final-chunk source destruction
    /// and output handoff. A caller destroys its vectors before this owner.
    [[nodiscard]] std::optional<Protocol::Sync::BulkSourceReservation> reserveImageCarrierScratch(std::uint32_t clientID, std::size_t bytes);
    /// Frees drained large staging buffers before admitting image sources;
    /// an allocation that still exists never counts as available room.
    void releaseIdleImageCarrierCapacity();

    /// Asks the coherence rule whether a session's frame may be taken now.
    ///
    /// - Parameters:
    ///   - s: The session.
    ///   - nowMicroseconds: The daemon's monotonic clock.
    /// - Returns: The class to take the frame in, or nothing while it is held.
    [[nodiscard]] std::optional<Runtime::FrameClass> mayPublishFrame(Session& s, std::uint64_t nowMicroseconds);
    /// Takes a session's frame when the coherence rule admits one now, as the
    /// loop does for every session after its PTYs are serviced.
    ///
    /// - Parameter s: The session.
    /// - Returns: The census's account of the frame taken, or nothing while
    ///   the rule holds it or none is due.
    std::optional<Protocol::Sync::CensusVerdict> publishFrameIfAdmitted(Session& s);

    /// Applies the size the session's geometry arbiter has due (Docs/sync.md
    /// §6.10): one reflow, one SIGWINCH and one frame owed, at most once per
    /// coalesce window however many clients move the size. A pixel-only
    /// change is one SIGWINCH.
    ///
    /// - Parameters:
    ///   - s: The session.
    ///   - typist: The client whose input is about to be written, whose own
    ///     new size goes first; `nullopt` at a pass's end.
    void applyPendingGeometry(Session& s, std::optional<Protocol::Sync::ID128> typist = std::nullopt);

    /// The key a client's geometry events are filed under: its screen
    /// attachment once it has one, and a key of its own before.
    ///
    /// - Parameter client: The client.
    /// - Returns: The key.
    [[nodiscard]] Protocol::Sync::ID128 geometryKey(const Client& client) const;

    /// Gives a session's terminal its geometry owner's pixels and theme
    /// (Docs/sync.md §6.10, §6.11): the program's pixel-size and colour
    /// replies are the owner's, never another client's. An ownership change
    /// applies at most once per coalesce window, as the size does, so typists
    /// in turn never storm the program with colour-scheme reports; the
    /// owner's own new facts apply at once.
    ///
    /// While no driver owns the size, the session's stand-in presents for it:
    /// the person who presented most recently. A change that waits out the
    /// window applies at its end whoever it is for, and nothing stays due for
    /// a client that is gone.
    ///
    /// - Parameter s: The session.
    void applyOwnerPresentation(Session& s);

    /// Records what a client presents, and applies it to each session it
    /// owns the geometry of.
    ///
    /// - Parameters:
    ///   - index: The client.
    ///   - presented: Its pixels and theme.
    void notePresented(std::size_t index, Client::Presented presented);

    /// Binds the live attachment table's retirement to this daemon's panes.
    ///
    /// Replacing the table at handover installs the same owner before the
    /// successor serves clients.
    void wireAttachmentRetirement();

    /// Frees the attachments past their grace, and forgets every freed one:
    /// its hold on a pane's size, and a paste its client will never finish,
    /// whose bracket is closed.
    ///
    /// - Parameter now: The daemon clock.
    void reapAttachments(std::uint64_t now);

    /// Answers `pane.pinGeometry`: freezes a pane's size, or with no size lets
    /// its owner's apply again. The caller needs the Drive scope.
    ///
    /// - Parameters:
    ///   - client: The caller.
    ///   - session: The pane.
    ///   - request: The call.
    /// - Returns: The answer.
    [[nodiscard]] Codec::JSON::Value pinPaneGeometry(const Client& client, Session& session, const Protocol::Control::DecodedRequest& request);

    /// Answers `pane.overrideGeometry`: sets a pane's size once, until a
    /// driver types or reports a different size. The caller needs the Drive
    /// scope.
    ///
    /// - Parameters:
    ///   - client: The caller.
    ///   - session: The pane.
    ///   - request: The call.
    /// - Returns: The size set, clamped to what a terminal holds, or the
    ///   refusal.
    [[nodiscard]] Codec::JSON::Value overridePaneGeometry(const Client& client, Session& session, const Protocol::Control::DecodedRequest& request);

    /// Records in the event log that a pane was focused, when a driver takes
    /// its focus and none held it: a client's `Focus` and `Facts`, and a
    /// client leaving, all move it.
    ///
    /// - Parameter session: The session whose pane it is.
    void notePaneFocus(Session& session);

    /// Files the geometry events a client sent before its screen attachment was
    /// known under the attachment.
    ///
    /// - Parameter client: The client.
    void rekeyGeometry(const Client& client);

    /// Publishes a session's frame to its screen source and the frames
    /// that follow a screen frame — title, bell, sound, images and the
    /// agent events — to its clients. The screen itself leaves through each
    /// client's connection when it is next serviced.
    ///
    /// - Parameters:
    ///   - s: The session.
    ///   - frameClass: Why the frame was taken.
    void publishFrame(Session& s, Runtime::FrameClass frameClass);

    /// Reads what a client's predictor is told of a session: the PTY's echo
    /// and signal characters, the shell's prompt and the foreground program.
    ///
    /// - Parameter s: The session.
    /// - Returns: The facts, now.
    [[nodiscard]] Protocol::Sync::PaneEchoFacts echoFactsOf(const Session& s) const;

    /// Reads a run of a client's Sync stream, creating its connection on the
    /// first, and binds its panes once the connection has welcomed it.
    ///
    /// - Parameters:
    ///   - index: The client.
    ///   - bytes: The run, as one Sync frame carried it.
    void receiveScreenBytes(std::size_t index, std::span<const std::uint8_t> bytes);

    /// Binds every subscription of a client that its connection has not yet
    /// bound.
    ///
    /// - Parameter index: The client.
    void bindScreenPanes(std::size_t index);
    /// Refreshes measurement authority when a subscribed pane's offer changes.
    ///
    /// - Parameter index: The client's index.
    void adoptScreenTypography(std::size_t index);

    /// Keeps the current eligible rich-text measurer, or selects a live driver.
    /// - Parameter session: The pane whose measurements need an authority.
    void refreshRichTextMeasurer(Session& session);
    /// Advances measurement deadlines and drains outcomes even without a viewer.
    /// - Parameter now: The monotonic host clock, in microseconds.
    void serviceRichTextMeasurements(std::uint64_t now);

    /// Unbinds a subscription from its client's connection.
    ///
    /// - Parameters:
    ///   - client: The client.
    ///   - subscription: The subscription leaving.
    void unbindScreenPane(Client& client, const PaneSubscription& subscription);

    /// Services a client's connection and moves what it may send into the
    /// client's output: stream frames while the output holds less than one
    /// frame, so the lanes keep choosing what goes next, and datagrams whole.
    ///
    /// - Parameters:
    ///   - index: The client.
    ///   - nowMicroseconds: The daemon's monotonic clock.
    void serviceSyncConnection(std::size_t index, std::uint64_t nowMicroseconds);
    /// The borrowing pass; its caller settles administration effects afterward.
    /// - Parameters:
    ///   - index: The requesting client's slot during this pass.
    ///   - nowMicroseconds: The daemon's monotonic clock.
    ///   - effects: Pass-owned disconnects and token revalidation.
    void serviceSyncConnection(std::size_t index, std::uint64_t nowMicroseconds, AdministrationEffects& effects);
    /// Drains admitted transport frames without serving any operation.
    /// - Parameters:
    ///   - index: The carrier owner.
    ///   - nowMicroseconds: The current monotonic clock.
    void drainSyncConnection(std::size_t index, std::uint64_t nowMicroseconds);

    /// The soonest instant any client's connection has something to send that
    /// no frame from the client and no publish will prompt.
    ///
    /// - Parameter nowMicroseconds: The daemon's monotonic clock.
    /// - Returns: The instant, or `nullopt` when there is none.
    [[nodiscard]] std::optional<std::uint64_t> nextScreenServiceMicroseconds(std::uint64_t nowMicroseconds) const;
    // Harvest command completion and foreground records once; PaneSource
    // captures each retained effect's generation.
    void emitAgentEvents(Session& s);
    /// Records a command-started event for each OSC 133 C the session's
    /// shell wrote since the last one recorded, up to a start count.
    ///
    /// - Parameters:
    ///   - session: The session whose starts to record.
    ///   - through: The start count to record up to; at most the shell's.
    void recordCommandStarts(Session& session, std::uint32_t through);
    /// Reads the session's foreground program when the probe budget allows.
    ///
    /// The one place the costly reads are made: whether the foreground is the
    /// shell, its name, and its directory while the shell reports none. The
    /// foreground group, one ioctl, is the change detector: a group that moved
    /// probes at once, an unsettled answer probes again, and otherwise the
    /// last answer stands until the budget elapses.
    ///
    /// - Parameter s: The session.
    void probeForegroundIfDue(Session& s);
    /// Makes the probe a frame asked for inside the budget, once the window
    /// has closed, and sends what moved: the Status, the pane facts and the
    /// records. A session whose facts nobody reads is not probed.
    ///
    /// - Parameters:
    ///   - s: The session.
    ///   - now: The loop's clock, in milliseconds.
    void serveOwedForegroundProbe(Session& s, std::uint64_t now);
    /// Registers the session for its stream's process-tree edges, once.
    ///
    /// - Parameter s: The session.
    void watchProcessTree(Session& s);
    /// Takes the edges the watcher threads raised since the last pass.
    ///
    /// - Parameter now: The loop's clock, in milliseconds.
    void takeProcessTreeEdges(std::uint64_t now);
    /// When the session's foreground is next probed without a frame asking:
    /// once its process tree has settled after an edge, or on the fallback
    /// cadence while someone reads its facts where no edges are raised.
    ///
    /// - Parameter s: The session.
    /// - Returns: The time in milliseconds, or zero when none is owed.
    [[nodiscard]] std::uint64_t processTreeProbeDueAt(const Session& s) const;
    /// Makes that probe when it is due, and sends what moved.
    ///
    /// - Parameters:
    ///   - s: The session.
    ///   - now: The loop's clock, in milliseconds.
    void serveProcessTreeProbe(Session& s, std::uint64_t now);
    /// A workspace's structure as every reader was last offered it.
    struct StructureRecord {
        std::uint64_t revision = 0;
        /// The record under revision zero, which a rebuild is compared with.
        std::vector<std::uint8_t> unrevised;
        /// The record under `revision`, shared with every connection offered it.
        std::shared_ptr<const std::vector<std::uint8_t>> record;
        /// The arrangement `record` says, ``Session::Layout::arrangementSignature``;
        /// empty until the next revision is made from a model.
        std::string arrangement {};
        /// The revision at which `arrangement` last changed, which a layout
        /// operation's revision is held to rather than `revision`
        /// (``Protocol::Sync::isStructureRevisionCurrent``).
        std::uint64_t arrangedAt = 0;
    };
    /// The structures by workspace id, live and restorable.
    std::map<std::uint32_t, StructureRecord> m_structureRecords;
    /// An immutable original call, kept once per requester and at most sixteen
    /// in the daemon. The parameters are bounded before this is retained.
    struct PendingStructureCall {
        std::uint64_t clientID = 0;
        Protocol::Sync::ID128 attachmentID;
        Protocol::Sync::Role role = Protocol::Sync::Role::Viewer;
        std::string principal;
        std::uint32_t callIndex = 0;
        std::uint32_t workspaceID = 0;
        std::uint64_t revision = 0;
        std::uint64_t deadlineMilliseconds = 0;
        /// Only clients actually offered the question receive its retirement.
        /// Each keeps the attachment it was asked through: one whose link is
        /// lost may resume within its grace and answer, so the question
        /// waits for it.
        std::vector<ConsentRecipient> recipients;
        Protocol::Control::DecodedRequest request;
        /// The question as first asked, without a recipient's session, for a
        /// person who attaches or resumes while it waits.
        Protocol::Sync::StructureConsent question;
    };
    std::map<std::uint32_t, PendingStructureCall> m_structureConsents;
    /// Asks a person who has just attached, resumed or bound a workspace every
    /// waiting tree question they may approve and were not yet asked.
    ///
    /// - Parameter index: The person's client.
    void offerStructureConsents(std::size_t index);
    /// Asks one person a waiting tree question if they may approve it and were
    /// not yet asked on this connection, and records them as its recipient.
    ///
    /// - Parameters:
    ///   - pending: The held operation.
    ///   - candidate: The proposed approver.
    ///   - workspace: The workspace the operation affects.
    void askStructureConsent(PendingStructureCall& pending, Client& candidate, const HostedWorkspace& workspace);
    /// Never reused during this daemon's lifetime; zero means exhausted.
    std::uint32_t m_nextStructureConsentID = 1;
    /// Dismisses a question on each surviving connection that received it.
    ///
    /// - Parameters:
    ///   - requestID: The host-lifetime question identifier.
    ///   - pending: The held operation and its bounded recipient list.
    void retireStructureConsent(std::uint32_t requestID, const PendingStructureCall& pending);

    /// Retires only actual current recipients, retaining bounded owed delivery.
    /// - Parameters:
    ///   - kind: The exact kind originally presented.
    ///   - requestID: The resolved host-owned question.
    ///   - recipients: The current-client and session pairs that received it.
    void retireConsentQuestion(Protocol::Sync::ConsentKind kind, std::uint32_t requestID, std::span<const ConsentRecipient> recipients);

    /// Retires an attach or termination question before its pending owner clears.
    /// - Parameter client: The requester holding its admitted recipient pairs.
    void retireClientConsent(Client& client);
    /// Whether something a structure says may have changed since the
    /// structures were last built: a client's frame, a call, a client or a
    /// session coming or going, or a pane's structural frame.
    bool m_isStructureDirty = true; // bool-ok: names the one state it reports
    /// Whether a client is sent workspace structures: one that lists the
    /// host, or a person attached to a session.
    ///
    /// - Parameter client: The client.
    /// - Returns: True when the client reads structures.
    [[nodiscard]] bool readsStructures(const Client& client) const;
    /// Builds every workspace's structure when something it says may have
    /// changed, gives a changed one its next revision, and offers each
    /// reader the ones it reads: a listing every workspace, a person the
    /// workspaces holding the sessions it is attached to. Lists every pane a
    /// listing has not bound. Does nothing while nobody reads a structure.
    void syncStructures();
    /// Lists on a listing client's connection every pane it has not bound,
    /// and ends the listing of one it has since bound.
    ///
    /// - Parameter index: The client.
    void syncListedPanes(std::size_t index);
    /// How much of a listed pane's facts a client is sent: whole when its
    /// credential would let it read the pane's screen (the Observe scope, or
    /// a trusted local client), and without command-line arguments when not.
    ///
    /// - Parameter client: The listing client.
    /// - Returns: The detail its listed panes are sent at.
    [[nodiscard]] Protocol::Sync::ListedDetail listedDetailFor(const Client& client) const;
    /// Ends every listing of a session's pane, before the session's source
    /// goes: a connection lists a pane by reference to it.
    ///
    /// - Parameter sessionID: The session.
    void unlistSession(uint32_t sessionID);
    /// Whether a client begins listing the host or ends it.
    enum class ListingChange : std::uint8_t {
        Begin,
        End,
    };
    /// Serves `host.sessions.subscribe` and `host.sessions.unsubscribe`.
    ///
    /// A client that begins is owed every workspace's structure and the
    /// records of every pane it has not bound, and is told which workspaces
    /// to expect and the host's wall clock, which ages are read against. One
    /// that ends has its listed panes unlisted at the next sync and is
    /// offered no workspace's structure but those it is attached to.
    ///
    /// - Parameters:
    ///   - index: The calling client.
    ///   - change: Which.
    /// - Returns: The call's result object.
    [[nodiscard]] Codec::JSON::Value serveListing(std::size_t index, ListingChange change);
    /// Whether some connection reads a session's facts: one the session's
    /// pane is bound on, or one that lists the host. Nothing is computed for
    /// a pane nobody reads: no process walk, no owed probe and no records.
    ///
    /// - Parameter s: The session.
    /// - Returns: True when the pane's facts have a reader.
    [[nodiscard]] bool isFactsRead(const Session& s) const;
    /// Whether a connection is attached to a pane, as a client drawing it or
    /// an agent reading its screen. A pane none is attached to has its frames
    /// taken in the pass that reads its output, unpaced.
    ///
    /// - Parameter s: The session.
    /// - Returns: True when a connection subscribes to the pane.
    [[nodiscard]] bool isAttachedTo(const Session& s) const;
    /// Asks for a session's records to be read from its terminal: now when
    /// the budget allows, and otherwise once when its window closes. A
    /// session whose facts nobody reads is not read and owes nothing.
    ///
    /// A frame, a probe, typed input, a pin, a new reader and a change in
    /// who shows the pane each ask. The read copies a title's worth of
    /// strings, so a pane that floods asks once per frame and is read on the
    /// probe's budget.
    ///
    /// - Parameters:
    ///   - s: The session.
    ///   - now: The loop's clock, in milliseconds.
    void noteRecordsDue(Session& s, std::uint64_t now);
    /// Reads the records a session was owed inside the budget, once its
    /// window has closed.
    ///
    /// - Parameters:
    ///   - s: The session.
    ///   - now: The loop's clock, in milliseconds.
    void serveOwedRecords(Session& s, std::uint64_t now);
    /// Reads a session's `PaneFacts` and `StatusRecords` from its terminal
    /// and what the host knows beside it, into the pane's source
    /// (Docs/sync.md §7.7).
    ///
    /// It reads what the probe and the walk last found and makes neither,
    /// and it queues no frame: a caller in the middle of a client's bytes
    /// may ask.
    ///
    /// Attention a person is looking at is marked seen first: a person's
    /// attachment that holds the pane `Live` in a focused window. The
    /// activity stamps move the revision by themselves at most once a second.
    ///
    /// - Parameters:
    ///   - s: The session.
    ///   - now: The loop's clock, in milliseconds.
    void publishRecords(Session& s, std::uint64_t now);
    /// What the host knows about a session's pane that its terminal does not
    /// hold: the process facts the probe and the walk last read, how the
    /// session ended, the activity stamps and where it came from.
    ///
    /// - Parameter s: The session.
    /// - Returns: The facts.
    [[nodiscard]] Protocol::Sync::PaneHostFacts paneHostFactsOf(const Session& s) const;
    /// Stamps a pane's last input when typed input reached its program since
    /// the pane was last asked.
    ///
    /// Asked once at the end of a read pass, so a pass that carried a hundred
    /// keys reads the clock once. Typed means raw bytes, keys and pastes; a
    /// pointer event or a focus report the same queue wrote is not counted.
    ///
    /// - Parameter s: The session.
    void stampTypedInput(Session& s);
    /// Clears a pane's unseen bell and notification when a person's driver
    /// holds the focus on it, or gained it since the pane was last asked.
    ///
    /// Asked once at the end of a read pass. A person is a driver the host
    /// verified as one: an agent's focus and a viewer's leave the latch for
    /// the person who looks later.
    ///
    /// - Parameter s: The session.
    void clearAttentionAPersonSaw(Session& s);
    /// Reads the host's wall clock.
    ///
    /// - Returns: Milliseconds since the Unix epoch.
    static std::uint64_t systemWallClockMilliseconds();
    /// Reads the host's monotonic clock.
    ///
    /// - Returns: Microseconds since an arbitrary fixed instant.
    static std::uint64_t systemMonotonicMicroseconds();
    /// The clock the coherence rule and the sessions' terminal clocks read; a
    /// test puts its own here.
    std::uint64_t (*m_monotonicMicroseconds)() = &Daemon::systemMonotonicMicroseconds;
    /// The clock the activity stamps read; a test puts its own here.
    std::uint64_t (*m_wallClockMilliseconds)() = &Daemon::systemWallClockMilliseconds;
    /// Rebuilds a session's pane facts from what the foreground probe last
    /// read, and reports whether they moved. Makes no probe of its own.
    ///
    /// - Parameter s: The session.
    /// - Returns: Whether the process-derived cache changed.
    bool refreshPaneFacts(Session& s);

    void teardownAll();

    // QUIC data-plane (built only with -DUBIQUITTY_QUIC=ON).
    // Each accepted QUIC connection is bound to a Client (fileDescriptor -1,
    // quicConnection set), so handleFrame drives it identically to a socket
    // client. The listener fires onQUICStreamData with inbound stream bytes
    // and onQUICConnectionClosed when a connection goes away.
    /// Takes inbound stream bytes from a QUIC connection.
    ///
    /// Before the auth preamble completes these accumulate in a per-connection
    /// buffer bounded by the token length, since an unauthenticated peer must
    /// not be able to grow daemon memory. After it, they feed the client's
    /// decoder directly.
    ///
    /// - Parameter data: Valid only for the duration of the call.
    void onQUICStreamData(QUICConnection*, std::span<const std::uint8_t> data);
    void onQUICConnectionClosed(QUICConnection*);
    /// Hands a datagram to the screen connection of the client it came from.
    ///
    /// - Parameters:
    ///   - connection: The QUIC connection it arrived on.
    ///   - datagram: Its payload, exactly one Sync envelope.
    void onQUICDatagram(QUICConnection* connection, std::span<const std::uint8_t> datagram);
    // The Client index bound to `connection`, creating a pre-HELLO one on
    // first sight. Returns m_clients.size() if the client cap is reached.
    std::size_t findOrCreateQUICClient(QUICConnection*);
    /// Feeds post-auth stream bytes to a QUIC client's decoder.
    ///
    /// Re-resolves the client by connection between frames, because handling
    /// one frame can drop the client and invalidate the index.
    ///
    /// - Parameters:
    ///   - index: Client slot as of entry.
    ///   - data: Valid only for the duration of the call.
    void feedQUICClientFrames(QUICConnection*, std::size_t index, std::span<const std::uint8_t> data);

    // One-time QUIC bootstrap tokens: mint one for local control operation 2 (over
    // the uid-gated local socket), consume it when a QUIC connection presents
    // it. mint stores; consume returns true + removes a match (one-time).
    // isInteractiveUser marks a token minted on the authenticated person's behalf;
    // it defaults off so a mint that does not state one authorizes automation.
    std::string mintQUICToken(std::uint32_t mintingClientID, Protocol::Sync::ScopeSet scope, bool isInteractiveUser = false); // bool-ok: carries the token record's own interactive-user mark, set only by a Local-origin minter; the flag is the mark, and a second spelling of it could disagree
    // On a valid one-shot match, hands back the token's carried scope and
    // principal — the scope the mint carried, never a widened one — and its
    // interactive-user mark to a caller that passes somewhere to put it, and
    // consumes it; false (outputs untouched) on no/expired match.
    bool consumeQUICToken(const std::string& token, Protocol::Sync::ScopeSet& outScope, std::string& outTokenID, bool* outInteractiveUser = nullptr);

    // Consent gate.
    // Human-vs-agent classification. The trusted local GUI (the ubiquitty-host
    // attach bridge, origin Local) is a person, and so is the bearer of an
    // unlabelled interactive-user bootstrap token — the person's own phone,
    // dialling in over QUIC with the token their uid-gated local client minted.
    // Every other client is an agent: a tokenless remote peer, a labelled token
    // bearer, a persisted-token client, a non-bridge local binary.
    //
    // This answers who a client is, for the consent, presence and terminate
    // gates. What its input may cause is a different question, answered by
    // originOf, which keeps a QUIC peer Remote. Not static — it consults
    // originOf (a const member).
    /// The binaries this daemon accepts as its own, by device and inode.
    ///
    /// Resolved once at construction, because the answer cannot change while the
    /// daemon runs: it is the file it was executed from and the interface beside
    /// it. Empty when the daemon cannot resolve its own path, which costs a
    /// consent prompt and never a wrong grant.
    std::vector<BinaryIdentity> m_trustedLocalBinaries;
    /// Descriptor-transfer trust is a bounded pinned-content owner, not cached inodes.
    std::optional<LocalAdoptExecutables> m_trustedAdoptExecutables;

    /// Drops the pending capability-grant records naming this principal.
    ///
    /// - Parameters:
    ///   - identity: The grant key whose outstanding questions are withdrawn.
    void withdrawPendingGrantsFor(std::string_view identity);

    /// Drops the pending capability-grant records naming this session.
    ///
    /// - Parameters:
    ///   - sessionID: The session whose outstanding questions are withdrawn.
    void withdrawPendingGrantsForSession(std::uint32_t sessionID);

    /// Drops the pending capability-grant records naming both.
    ///
    /// - Parameters:
    ///   - identity: The grant key the revoke named.
    ///   - sessionID: The session it named, leaving that principal's questions
    ///     about other sessions standing.
    void withdrawPendingGrantsForSessionOf(std::string_view identity, std::uint32_t sessionID);

    /// Whether this daemon has had a session, has none now, and has nothing
    /// left to deliver about the one that went.
    ///
    /// - Returns: True when the run loop should end.
    bool hasOutlivedItsLastSession() const;

    bool isAgentClient(const Client& c) const;
    // An agent attaching a HUMAN-occupied session with no standing grant is held
    // pending the human's consent.
    bool sessionHasHumanClient(uint32_t sessionID) const; // a human (non-agent) client on S
    /// Whether some person attached to a session can be asked its questions.
    ///
    /// - Parameter sessionID: The session.
    /// - Returns: Whether a question about it has anyone to ask.
    bool sessionHasAskableHuman(std::uint32_t sessionID) const;
    // True if `agent`'s verified identity already covers session `s` (an
    // identity-bound allow-session grant, or a persisted always-allow) — so
    // attach/terminate skips the prompt. An agent with no verified identity
    // (empty key) never matches; one human's grant never covers another agent.
    bool agentHasStandingGrant(const Client& agent, const Session& s) const;
    void capturePeerIdentity(Client& c); // verified processID + exe path off the socket
    // Persist an "always-allow" grant — but only for a token-bound principal; a
    // tokenless agent's key is a mutable exe path (swappable → grant inheritance)
    // so it is refused. Returns true iff the grant was persisted.
    bool persistAlwaysAllowedAgent(std::size_t agentIndex);
    // The grant key for an agent: its token digest when token-authed (a precise,
    // non-spoofable principal), else the verified exe path. Empty = ungrantable.
    std::string grantKeyFor(const Client& c) const;
    // Capability-token auth: scope enforcement + refusal + drop.
    bool tokenScopeAllows(const Client& c, Protocol::Sync::Scope scopeBit) const; // a tokenless Remote peer is allowed nothing
    /// Refuses a client: sends the ordered Bye, withdraws what it held, and
    /// leaves the reap to the run loop.
    ///
    /// - Parameters:
    ///   - index: The client.
    ///   - reason: Why, as the Bye carries it.
    void refuseClient(std::size_t index, Protocol::Sync::ByeReason reason);
    /// How a reload reads the credential stores.
    enum class CredentialStoreReading : std::uint8_t {
        /// Each store as it is on disk; one that cannot be read whole grants nothing.
        AsFound,
        /// Every store as empty, because one of them could not be examined.
        Withheld,
    };
    /// Re-reads the token store and refuses every live token-bound client, and
    /// retires every retained grant, whose token no longer validates.
    ///
    /// - Parameter reading: Whether the store on disk is consulted at all.
    void reverifyTokenClients(CredentialStoreReading reading = CredentialStoreReading::AsFound);
    /// Reloads persisted consent and revalidates every live and retained token grant.
    ///
    /// - Parameter reading: Whether the stores on disk are consulted at all.
    void reloadCredentialStores(CredentialStoreReading reading = CredentialStoreReading::AsFound);
    /// Whether any live client, pending request or retained grant draws its
    /// authority from the token or consent store.
    [[nodiscard]] bool holdsStoreBoundAuthority() const;
    /// Reloads the credential stores when either file differs from what the
    /// last reload found.
    ///
    /// Any owner may edit the shared stores: another build's command line,
    /// another daemon's host management, a program this daemon refuses to take
    /// a reload request from. The files, not a notification, are the
    /// authority, so this runs before an inbound batch that could exercise
    /// store-bound authority is dispatched and once per run-loop pass while
    /// such authority is held. A store that cannot be examined withholds
    /// every grant until it can.
    void refreshCredentialStoresIfChanged();
    /// Refreshes the stores before a client's inbound batch is dispatched.
    ///
    /// - Parameter index: The client; updated if the refresh moved it.
    /// - Returns: Whether the client is still there to be served.
    [[nodiscard]] bool refreshCredentialStoresBeforeDispatch(std::size_t& index);
    /// Drops every live client whose token has reached its expiry.
    ///
    /// A token's deadline binds the connection it authenticated, not only the
    /// handshake that presented it. A lapse leaves the store unchanged, so a
    /// record that lapses under an established connection is caught from the
    /// deadline the client carries; the refusal is `ByeReason::TokenExpired`
    /// and the reap that follows is the deferred one.
    void dropExpiredTokenClients();
    /// The soonest expiry among live clients and retained attachment grants.
    ///
    /// The run loop wakes for it, so a connection whose token lapses while the
    /// link is idle ends at its deadline rather than at whatever traffic
    /// arrives next.
    ///
    /// - Returns: The expiry as a Unix timestamp, or `0` when none expires.
    [[nodiscard]] std::uint64_t earliestTokenExpiryUnix() const;
    /// Sends a held agent's consent prompt to every human who may rule on it.
    ///
    /// A hold is taken only when one of them is attached, so the prompt has a
    /// reader by construction; what decides the outcome if none answers is the
    /// deadline the hold carries.
    ///
    /// - Parameter agent: The held client, whose consent fields name the request.
    void sendConsentRequestToHumans(Client& agent, std::optional<std::size_t> recipient = std::nullopt);

    /// The client whose clipboard answers for a session, by index.
    ///
    /// The one whose input most recently drove it: a program asking for "the
    /// clipboard" is asking on behalf of whoever typed the command that started
    /// it, and on a shared session that is one person out of several.
    ///
    /// Never falls back to a different client when a driver is known. Handing
    /// one person's pasteboard to a command another person typed is a
    /// disclosure, and it is the failure a fallback would cause rather than
    /// prevent.
    ///
    /// - Parameter sessionID: The session the asking program runs in.
    /// - Returns: The client's index, or `m_clients.size()` when nobody can
    ///   answer for it.
    [[nodiscard]] std::size_t clipboardClientForSession(std::uint32_t sessionID) const;

    /// Asks a session's client for its clipboard, on behalf of a host program.
    ///
    /// The host has no pasteboard of its own — a headless box runs no display
    /// server — so a tool that wants one is served from the client's, over the
    /// session it is attached to.
    ///
    /// Asked of ONE client, not broadcast: two clients on a shared session hold
    /// two different pasteboards, and a program asking for "the clipboard"
    /// wants one answer rather than a race between them. The client whose
    /// session the program runs in is the one whose user typed the command.
    ///
    /// - Parameters:
    ///   - sessionID: The session the asking program runs in.
    ///   - kind: Which types are held, or the bytes of one.
    ///   - mime: The type wanted, empty for a type list.
    /// - Returns: The request id to match the answer against, or zero when no
    ///   client on that session offered to serve.
    std::uint32_t requestClipboardFromClient(
        std::uint32_t sessionID, Protocol::Sync::ClipboardRequestKind kind, const std::string& mime);

    /// Distinguishes a clipboard read from a source-selection query.
    enum class ClipboardToolOperation : std::uint8_t {
        Read,
        InspectSource
    };
    /// Verifies a local helper's process ownership and requests its selected clipboard.
    ///
    /// - Parameters:
    ///   - index: The local helper client.
    ///   - request: The requested clipboard item or type list.
    ///   - operation: Whether to read clipboard data or inspect its selected source.
    void handleClipboardToolRequest(std::size_t index, const Protocol::Sync::ClipboardRequestMessage& request, ClipboardToolOperation operation);
    /// Queues a helper response and ends its pending clipboard request.
    ///
    /// - Parameters:
    ///   - index: The local helper client.
    ///   - response: The bounded clipboard reply or refusal.
    void finishClipboardTool(std::size_t index, const Protocol::Sync::ClipboardDataMessage& response);
    /// Frames a borrowed helper reply without copying its MIME or data owners.
    ///
    /// - Parameters:
    ///   - index: The local helper client.
    ///   - status: The validated answer status.
    ///   - items: Items borrowed until synchronous framing completes.
    void finishClipboardTool(std::size_t index, Protocol::Sync::ClipboardServeStatus status,
        std::span<const Protocol::Sync::ClipboardServeItemView> items);
    /// Changes the input driver and cancels clipboard requests owned by other clients.
    ///
    /// - Parameters:
    ///   - session: The session receiving input.
    ///   - clientID: The physical client that supplies the input.
    void setSessionInputDriver(Session& session, std::uint32_t clientID);

    /// A session's remote input sink: what its encoder writes reaches
    /// `writeRemoteInput`.
    class SessionInputSink;

    /// Settles what each session's program has had `E` to echo, when its
    /// output is quiet at the settle deadline (Docs/sync.md §6.7).
    ///
    /// - Parameter now: The clock, read before the readiness check.
    void settleDueInput(std::uint64_t now);

    /// The principal a client was authenticated as: a token's, or the
    /// daemon's own user for a local client the socket's uid check admitted;
    /// a remote client with neither is its connection alone, so nothing it
    /// leaves behind can be resumed by another.
    ///
    /// - Parameter client: The client.
    /// - Returns: The principal.
    [[nodiscard]] std::string principalOf(const Client& client) const;

    /// Binds the subscriptions a resume brought: each pane the connection
    /// bound itself is the client's, as an attach would have made it.
    ///
    /// - Parameter index: The client.
    void adoptResumedPanes(std::size_t index);

    /// Closes the connections resumes moved attachments off: each sends its
    /// `Bye{Superseded}` and is dropped once that has left.
    void closeSupersededConnections();

    /// Lends a session's screen source the encoder its remote input is
    /// written through.
    ///
    /// - Parameter session: The session, its terminal and screen source made.
    /// - Returns: False when an allocation was refused.
    [[nodiscard]] bool wireRemoteInput(Session& session);

    /// Records who drives a session before their input reaches its program:
    /// the program's resulting output is gated by that principal, and the
    /// floor below which a later detach cannot lower it rises to it.
    ///
    /// - Parameters:
    ///   - session: The session written to.
    ///   - client: The driving client.
    void stampInputOrigin(Session& session, const Client& client);

    /// Independent output ownership for ordinary input and paste delimiters.
    enum class RemoteInputLane : std::uint8_t {
        Ordinary,
        Paste
    };

    /// Writes a remote client's encoded input to a session's program: origin
    /// stamped, a pending resize applied first, and the program's exit noted.
    ///
    /// - Parameters:
    ///   - session: The session.
    ///   - authority: The server attachment that admitted this event.
    ///   - clientInstance: The client whose input it is.
    ///   - bytes: The encoded input.
    ///   - lane: Paste bytes remain distinct from signal bypass writes.
    /// - Returns: Whether it was written, refused while the program does not
    ///   read, or the program is gone.
    [[nodiscard]] Protocol::Sync::WriteStatus writeRemoteInput(Session& session, Protocol::Sync::ID128 authority, Protocol::Sync::ID128 clientInstance, std::span<const std::uint8_t> bytes, RemoteInputLane lane = RemoteInputLane::Ordinary);

    /// Revalidates authority after a wait, then drains the ready PTY and pane queue.
    /// - Parameter session: The session whose stream became writable.
    void drainRemoteInput(Session& session);

    /// Rechecks retained input authority at the sink, including wall-clock expiry.
    /// - Parameters:
    ///   - authority: The attachment that admitted the event.
    ///   - clientInstance: Its replay identity.
    /// - Returns: Whether its admitted input may still reach a PTY.
    [[nodiscard]] bool mayWriteRemoteInput(Protocol::Sync::ID128 authority, Protocol::Sync::ID128 clientInstance) const;
    /// Refuses pending helper requests and discards queued replies from a source.
    ///
    /// - Parameters:
    ///   - sourceClientID: The physical client whose clipboard became unavailable.
    ///   - sessionID: The affected pane, or zero for every pane on the connection.
    void cancelClipboardTools(std::uint32_t sourceClientID,
        std::uint32_t sessionID = 0);
    /// Refuses helper requests whose response deadline has expired.
    void expireClipboardTools();
    /// Rechecks the exact typed read's source, helper, deadline and live binding.
    ///
    /// - Parameters:
    ///   - index: The source client's current index.
    ///   - handle: Its authenticated pane handle.
    ///   - requestID: The outstanding host-issued operation ID.
    /// - Returns: Whether reply allocation, consumption or delivery is permitted.
    [[nodiscard]] bool mayReceiveClipboardData(std::size_t index, std::uint32_t handle, std::uint32_t requestID) const;
    /// Retains one decision bound to the exact pending requester and approver.
    /// - Parameters:
    ///   - index: The pending attach requester.
    ///   - approver: The still-admitted human recipient.
    ///   - decision: The explicit decision for this immutable question.
    /// - Returns: Whether the question still belongs to both authenticated owners.
    [[nodiscard]] bool retainAttachConsentDecision(std::size_t index, const Client& approver, Protocol::Sync::ConsentChoice decision);
    /// Revalidates a held decision on every bounded output-delivery attempt.
    /// - Parameter index: The pending requester.
    void servicePendingAttachConsent(std::size_t index);
    /// Seeds only an exact attachment whose approved status was already admitted.
    /// - Parameter index: The promoted requester.
    void grantAgentAttach(std::size_t index);
    void denyAgentAttach(std::size_t index, Protocol::Sync::ConsentStatusPhase reason);
    /// Names the standing-consent file beside the configured credential store.
    ///
    /// - Returns: The consent file's path.
    [[nodiscard]] std::string consentStorePath() const;
    /// Loads the complete standing-consent list or withholds every entry.
    ///
    /// - Returns: Whether the consent file was read whole, or was absent.
    [[nodiscard]] bool loadAlwaysAllowedAgents();
    /// Withdraws one key from the current consent file and adopts its snapshot.
    ///
    /// - Parameter identity: The exact standing-consent key.
    /// - Returns: Whether the locked mutation completed, including absence.
    [[nodiscard]] bool revokeAlwaysAllowedAgent(const std::string& identity);
    /// Adopts committed token entries and schedules lost authority for retirement.
    ///
    /// - Parameter entries: The committed consent snapshot, owned by the caller.
    void adoptStandingConsentEntries(std::vector<std::string> entries);
    /// Selects retained records that lost standing consent and lack a session grant.
    ///
    /// - Parameter identities: The withdrawn principals.
    void queueStandingConsentWithdrawals(std::span<const std::string> identities);
    /// Retires selected authority after connection and dispatcher borrows end.
    void retireStandingConsentWithdrawals();
    // Capability-level Always grants: persisted under the control directory in a
    // separate file from the agent-attach list (capability_grants). Token-bound
    // only — the same token-only-persist invariant as persistAlwaysAllowedAgent.
    /// Names the capability grant file beside the host socket.
    ///
    /// - Returns: The capability grant file's path.
    [[nodiscard]] std::string capabilityGrantsPath() const;
    /// Binds the capability grant file and adopts its grants, or none when it
    /// cannot be read whole.
    void loadAlwaysAllowedCapabilityGrants();
    /// Applies one intent to the current capability grant file and adopts the
    /// grants it committed; a refusal leaves the store as it was.
    ///
    /// - Parameters:
    ///   - operation: The single mutation intended.
    ///   - key: The grant, or the identity whose grants are withdrawn.
    /// - Returns: Whether the intent committed, including one the file
    ///   already reflected.
    [[nodiscard]] bool editAlwaysAllowedCapabilityGrants(CapabilityGrantOperation operation, const Ubiquitty::CapabilityGrantKey& key);
    /// Installs handed-over grants while keeping this daemon's grant file.
    ///
    /// The Always records arrive unconfirmed, so the next one to authorize
    /// rereads the file first.
    ///
    /// - Parameter grants: The predecessor's grant store.
    void replaceCapabilityGrants(Ubiquitty::CapabilityGrantStore&& grants);

    PAL::UnixFileDescriptor m_listenFileDescriptor;
    // Created with the first session whose tracker raises edges.
    std::shared_ptr<ProcessTreeWake> m_processTreeWake;
    // Held from startup purely to be spent: with the descriptor table full,
    // closing this is what buys the one accept needed to refuse a connection
    // and clear it from the backlog.
    PAL::UnixFileDescriptor m_reserveFileDescriptor;
    // Held for the process's lifetime; releasing it is what lets the next
    // daemon start.
    PAL::UnixFileDescriptor m_startupLockFileDescriptor;
    // Why the last -start could not bind, for the caller's diagnostic.
    BindFailure m_bindFailure = BindFailure::None;
    // The capability grant file the store's Always records mirror. Declared
    // before the store, whose standing-grant source points into it.
    const std::unique_ptr<CapabilityGrantFile> m_capabilityGrantFile;
    // Host-wide capability grants, keyed (Capability x Origin x principal).
    // Declared before m_sessions: each session's policy points into this store
    // (setGrantStore), so the store must outlive the policies (reverse destroy).
    Ubiquitty::CapabilityGrantStore m_grantStore;
    // Host-wide sink for capabilities awaiting a human ruling. A session
    // policy's check() records a Prompt decision here; after that session's output
    // is parsed the daemon drains new entries and emits exact requested consent Notifies.
    // Declared before m_sessions for the same outlive-the-policies reason.
    Ubiquitty::CapabilityConsentQueue m_consentQueue;
    // In-flight capability-consent prompts: requestID -> the (capability, origin,
    // identity) the human is ruling on + the session it arose in. The response
    // grants from this record, never from the untrusted response, so a response
    // can only approve a request the daemon actually issued.
    struct PendingCapabilityGrant {
        Ubiquitty::Capability capability;
        Ubiquitty::CapabilityOrigin origin;
        std::string identity;
        uint32_t sessionID;
        // Nonzero only for a held exact OSC 52 write. Client IDs are never
        // reused, so a reconnect cannot inherit either endpoint's consent.
        std::uint32_t clipboardSourceClientID = 0;
        std::uint32_t clipboardRecipientClientID = 0;
        // When the host retires the question unanswered, for every kind: a
        // question nobody answers never keeps the capability's later reaches
        // from being asked about.
        std::uint64_t deadlineMilliseconds = 0;
        std::vector<ConsentRecipient> recipients;
    };
    std::unordered_map<uint32_t, PendingCapabilityGrant> m_pendingCapabilityGrants;
    std::vector<std::unique_ptr<Session>> m_sessions;
    /// Counts table-owned Prepared candidates and the post-consume activation.
    std::size_t m_preparedLocalAdopts = 0;
    /// The one decode owner every unpublished adopt candidate charges
    /// (Docs/sync.md §7.9), shared by the handover and reclaim paths.
    std::shared_ptr<Protocol::Sync::MemoryBudget> m_decodeMemory = std::make_shared<Protocol::Sync::MemoryBudget>(Protocol::Sync::defaultMaxHostDecodeBytes);
    std::unique_ptr<Ubiquitty::Session::AdoptTransaction> m_localAdoptTransactions;
    struct LocalAdoptAuthorityRecord {
        std::optional<LocalAdoptLifetime> lifetime;
        std::optional<Ubiquitty::Session::AdoptAuthorityLease> lease;
    };
    static constexpr std::size_t localAdoptAuthorityCapacity = 64;
    std::array<LocalAdoptAuthorityRecord, localAdoptAuthorityCapacity> m_localAdoptAuthorities;
    /// Drained synchronously after local ingress, before any other reactor work.
    std::unique_ptr<LocalAdoptCandidate> m_pendingLocalAdopt;
    std::unique_ptr<LocalAdoptCommitReply> m_pendingLocalAdoptReply;
    /// Holds one pane at the exact publication accepted for handover.
    struct QuiescingPane {
        std::uint32_t sessionID;
        Runtime::HandoverQuiesce controller;
        bool isReadQuietRecorded = false;
        std::uint64_t heldGeneration = 0;
    };
    /// Present only while the reactor is restricted to the handover drain.
    std::optional<std::vector<QuiescingPane>> m_quiescingPanes;
    /// An admitted successor's request whose transaction waits for this host
    /// to owe its clients nothing.
    ///
    /// While it waits no pane is read, so no new output is made for a client,
    /// and the reactor goes on writing to clients and answering their
    /// transports. The transaction starts once nothing blocks it, or at
    /// `deadlineMilliseconds` with what still does as its refusal.
    struct PendingHandover {
        PAL::UnixFileDescriptor channel;
        AdmittedHandoverRequest admitted;
        std::uint64_t startedMilliseconds = 0;
        std::uint64_t deadlineMilliseconds = 0;
    };
    std::optional<PendingHandover> m_pendingHandover;
    bool m_didHandoverQuiesceFail = false;
    /// What ended the last quiesce attempt, when an operation that cannot be
    /// handed over began during it rather than a pane missing ground.
    std::optional<Protocol::Sync::HandoverRefuseReason> m_handoverQuiesceBlocker;
    std::uint64_t m_nextScrollbackFoldMilliseconds = 0;
    std::uint32_t m_lastScrollbackFoldSessionID = 0;
    std::vector<Client> m_clients;
    std::uint64_t m_presentationRevision = 0;
    std::shared_ptr<Protocol::Sync::BulkSourceBudget> m_imageSourceBudget;
    uint32_t m_nextSessionID = 1;
    const std::uint64_t m_daemonInstanceID;
    /// Draws attachment secrets from the system's entropy.
    class EntropySecrets final : public Protocol::Sync::SecretSource {
    public:
        [[nodiscard]] Protocol::Sync::ID128 nextSecret() override;
    };
    EntropySecrets m_attachmentSecrets;
    /// This host's instance, time-ordered: the issue time in the high half,
    /// entropy in the low, which every attachment ID carries.
    Protocol::Sync::ID128 m_hostInstanceID;
    /// This machine's name and the daemon user's home directory, as every
    /// pane's facts carry them; read once.
    std::string m_paneHostName = PAL::systemHostName();
    std::string m_paneHomeDirectory = homeDirectoryOfProcess();
    /// The home directory this process was started with.
    ///
    /// - Returns: `HOME`, or empty when the environment has none.
    [[nodiscard]] static std::string homeDirectoryOfProcess();
    /// The attachments that outlive their connections (Docs/sync.md §6.9).
    std::unique_ptr<Protocol::Sync::AttachmentTable> m_attachments;

    using AttachmentGrant = Host::AttachmentGrant;
    /// One grant per token-bound attachment, dropped with the attachment.
    std::vector<AttachmentGrant> m_attachmentGrants;
    /// Authenticated outer/full identity pairs for the installed ancestry only.
    std::vector<AttachmentHostIdentity> m_inheritedHostIdentities;

    /// Records what a client's attachment was granted, once its `Greet` has
    /// attached it, for a later connection a resume proof admits.
    ///
    /// - Parameter client: The verified token client holding the attachment.
    void noteAttachmentGrant(const Client& client);
    /// Copies retained attachment authority without changing the predecessor.
    /// - Parameter now: The shared monotonic freeze time in microseconds.
    /// - Returns: Owned table and exact grants; physical client state is absent.
    [[nodiscard]] AttachmentHandover exportAttachmentAuthority(std::uint64_t now) const;
    /// Installs authenticated authority before any successor client is admitted.
    /// Refusal preserves the destination and every supplied owner. Persistent
    /// token grants are rechecked against this host's current store before commit.
    /// - Parameters:
    ///   - carried: The predecessor's owned table and grants.
    ///   - predecessor: The full identity verified by the handover transport.
    ///   - now: The shared monotonic time in microseconds.
    ///   - nowUnix: The current Unix time for token expiry.
    /// - Returns: Success, or the refused authority boundary.
    [[nodiscard]] std::expected<void, AttachmentHandoverRefusal> inheritAttachmentAuthority(AttachmentHandover&& carried,
        Protocol::Sync::ID128 predecessor, std::uint64_t now, std::uint64_t nowUnix);
    /// Prepares the same authority without publishing any part of it.
    /// A whole-host candidate can be abandoned after this succeeds; the live
    /// destination's attachment table and grants remain untouched.
    [[nodiscard]] std::expected<PreparedAttachmentAuthority, AttachmentHandoverRefusal> prepareAttachmentAuthority(AttachmentHandover&& carried,
        Protocol::Sync::ID128 predecessor, std::uint64_t now, std::uint64_t nowUnix);
    /// Checks retained authority before consuming a resume proof and installs
    /// exactly that authority on the new connection, never a bootstrap upgrade.
    ///
    /// - Parameters:
    ///   - index: The unauthenticated connection presenting the proof.
    ///   - proof: The attachment and MAC received on that connection.
    ///   - exporter: Keying material verified by that connection's transport.
    /// - Returns: Success only after installing the retained authority.
    [[nodiscard]] std::expected<void, Protocol::Sync::ResumeRefusal> admitAttachmentProof(std::size_t index,
        const Protocol::Sync::ResumeProof& proof, std::span<const std::uint8_t, 32> exporter);
    /// The wire version this host speaks; this build's, unless a test stands
    /// up a host at another.
    std::uint16_t m_wireVersion = Protocol::Sync::wireVersion;
    uint32_t m_nextClientID = 1; // stable internal routing handle; roster client numbers are display-only
    /// The one allocator every hosted workspace mints windows, tabs, and panes
    /// from, so each of those IDs is unique on this host and never reused.
    std::shared_ptr<Ubiquitty::Session::Identity::Allocator> m_layoutIdentities = std::make_shared<Ubiquitty::Session::Identity::Allocator>();
    /// The labels SESSION_LIST reports, keyed by typed identity.
    Ubiquitty::Session::Identity::LabelBook m_labels;
    /// The tags people put on this host's objects, keyed by typed identity.
    Ubiquitty::Session::Identity::TagBook m_tags;
    /// Replaces an object's tags, pruning the tags of ended objects when the
    /// book is full.
    ///
    /// - Parameters:
    ///   - id: The object to tag.
    ///   - names: Its tags; none clears them.
    /// - Returns: Whether the tags were stored.
    bool setTags(Ubiquitty::Session::Identity::ObjectID id, std::span<const std::string> names);
    /// Replaces an object's tags, making room in a full book as ``setTags``
    /// does, and says why when they were not stored.
    ///
    /// - Parameters:
    ///   - id: The object to tag.
    ///   - names: Its tags; none clears them.
    /// - Returns: The book's verdict.
    Ubiquitty::Session::Identity::TagVerdict putTags(Ubiquitty::Session::Identity::ObjectID id, std::span<const std::string> names);
    /// Sets or clears an object's label, making room in a full book as
    /// ``setLabel`` does, and says why when it was not stored.
    ///
    /// - Parameters:
    ///   - id: The object to label.
    ///   - label: The label; empty clears it.
    /// - Returns: The book's verdict.
    Ubiquitty::Session::Identity::LabelVerdict putLabel(Ubiquitty::Session::Identity::ObjectID id, std::string_view label);
    /// The workspaces a served call closed a window of, whose sessions
    /// ``settleOwedClosures`` ends.
    std::vector<std::uint32_t> m_closuresOwed;
    /// At most one entry per live session; identity is checked again before release.
    std::vector<std::pair<std::uint32_t, Protocol::Sync::ID128>> m_terminationsOwed;
    /// Sets a label, pruning the labels of ended objects when the book is full.
    ///
    /// - Parameters:
    ///   - id: The object to label.
    ///   - label: The label; empty clears it.
    /// - Returns: Whether the label was stored.
    bool setLabel(Ubiquitty::Session::Identity::ObjectID id, std::string_view label);
    /// Returns where a session's child runs: this daemon's socket, the session,
    /// and the window, tab, and pane it was placed in.
    ///
    /// - Parameter session: The session about to launch its child.
    /// - Returns: The context its child's environment is stamped with.
    [[nodiscard]] Ubiquitty::Session::LaunchContext launchContextFor(const Session& session) const;
    /// Reports whether an identity still names a live object on this host.
    ///
    /// - Parameter id: The identity to look up.
    /// - Returns: Whether the object exists.
    [[nodiscard]] bool isLiveObject(Ubiquitty::Session::Identity::ObjectID id) const;
    bool m_everHadSession = false;
    /// The host's event stream, which subscribers resume from by cursor.
    Ubiquitty::Session::EventLog m_events;
    Ubiquitty::Session::BackgroundEvents m_backgroundEvents { m_daemonInstanceID };
    /// Fences a passive inventory continuation against session insertion/removal.
    std::uint64_t m_backgroundInventoryRevision = 1;
    std::string m_backgroundEventPath;
    std::uint64_t m_backgroundEventWriteDue = 0;
    std::uint64_t m_writtenBackgroundEventRevision = 0;
    bool m_isBackgroundEventStorageAvailable = true;
    /// How many EVENT frames have been queued to subscribers, counted per
    /// recipient: one record reaching two subscribers counts twice. A waiter
    /// that polled would show as deliveries with no events behind them.
    std::uint64_t m_eventDeliveries = 0;
    /// Appends an event and pushes it to every subscriber that may see it.
    ///
    /// - Parameter event: The record; its cursor is assigned here.
    void recordEvent(Ubiquitty::Session::Event event);
    /// Records agent disclosure without screen contents or authority derived from its label.
    /// - Parameters:
    ///   - client: The admitted reader of the capture or stream.
    ///   - session: The exact session whose content was disclosed.
    void recordScreenAccess(const Client& client, const Session& session);
    /// How much of a host event one subscriber is shown.
    enum class EventDisclosure : std::uint8_t {
        /// The record is not sent; the subscriber's cursors skip it.
        Withheld,
        /// The record is sent as logged.
        Whole,
    };
    /// Decides what of an event a subscriber is shown.
    ///
    /// The log is the host's, so this is where a subscriber's reach is
    /// applied. A person at this machine sees every record. Any other client
    /// sees the records of sessions it is attached to or holds a standing
    /// grant for, which is what it could watch by attaching, the command
    /// lines a finished command carries included, as CommandFinished carries them
    /// to every client attached to the session.
    ///
    /// - Parameters:
    ///   - subscriber: The receiving client.
    ///   - event: The logged record.
    /// - Returns: What to send.
    [[nodiscard]] EventDisclosure eventDisclosureFor(const Client& subscriber, const Ubiquitty::Session::Event& event);
    /// Returns the pane a session fills, or zero when the host has not placed it.
    ///
    /// - Parameter session: A live session.
    /// - Returns: The pane's number.
    [[nodiscard]] std::uint32_t paneOfSession(const Session& session);
    /// Records a status event for each OSC 7501 record that changed since the
    /// last report, the cleared ones included.
    ///
    /// - Parameter session: The session whose records to compare.
    void recordProgramStatusEvents(Session& session);
    /// Offers one bounded pass from the log under the subscriber's live authority.
    /// - Parameter index: The connection whose admitted cursor is serviced.
    /// - Returns: False for an irrecoverable representation/admission failure;
    ///   the connection's owner then closes it instead of skipping a cursor.
    [[nodiscard]] bool serviceEventSubscriber(std::size_t index);
    /// Replays bounded activity to a currently authorized human on its own panes.
    /// - Parameter index: The client being serviced on the owner thread.
    /// - Returns: False if a permanent output failure requires dropping the client.
    [[nodiscard]] bool serviceScreenAccessSubscriber(std::size_t index);
    /// Holds a Drive-gated paste call before granting any raw upload credit.
    /// - Parameters:
    ///   - index: The exact calling physical connection.
    ///   - callIndex: Its still-outstanding call.
    ///   - request: The small manifest, not payload bytes.
    /// - Returns: An immediate refusal, or nothing while the upload is held.
    [[nodiscard]] std::optional<Codec::JSON::Value> beginPasteCall(std::size_t index, std::uint32_t callIndex, const Protocol::Control::DecodedRequest& request);
    /// Queries the original pane, attachment and Drive authority without effects.
    /// - Parameter index: The exact physical connection holding the upload.
    /// - Returns: Whether its captured authority and bound pane are still live.
    [[nodiscard]] bool mayReceivePasteUpload(std::size_t index) const;
    /// Receives only the credited transfer and copies verified bounded memory.
    /// - Parameters:
    ///   - clientID: The stable physical connection identity.
    ///   - chunk: One borrowed raw chunk; nothing here lands a file.
    /// - Returns: Whether paste claimed, accepted or refused the transfer.
    [[nodiscard]] Protocol::Sync::ClientBulkDisposition receivePasteUpload(std::uint32_t clientID, const Protocol::Sync::BulkChunk& chunk);
    /// Offers credit or one final answer; file landing occurs at most once.
    /// - Parameter index: The exact physical connection holding the upload.
    void servicePasteUpload(std::size_t index);
    /// Preserves the existing private-file landing and exact path injection.
    /// - Parameters:
    ///   - index: The authenticated physical connection being charged.
    ///   - session: The currently authorized target with its live input stream.
    ///   - mime: The verified manifest's bounded MIME name.
    ///   - bytes: The complete hash-verified immutable payload.
    /// - Returns: The exact final status and any landed path, including Busy.
    [[nodiscard]] Protocol::Control::PastePushReply performPastePush(std::size_t index, Session& session, std::string_view mime, std::span<const std::uint8_t> bytes);

    // Copied into each session at creation; see setConfiguration.
    Preferences::Values m_configuration;
    // Resolved from m_configuration once per setConfiguration and copied into
    // each session's capability policy at creation.
    CapabilityDecisionTable m_capabilityDecisions;
    // Recorded at start(); used to spawn sessions on demand.
    std::string m_shellPath;
    std::string m_shellIntegrationDirectory;
    std::optional<std::uint32_t> m_sessionFileCreationMask;
    // The records one pane-facts walk fills, kept so a walk on the probe's
    // cadence does not allocate a table's worth of records each time.
    std::vector<PAL::Process::Record> m_processWalkRecords;
    int m_columns = 80;
    int m_rows = 24;

    std::string m_socketPath;
    std::string m_handoverInstallPath;
    /// This daemon's own signature, read once as it started: the successors
    /// it admits by signature are judged against it.
    CodeSignature m_handoverSignature {};
    /// The signatures of the binaries local peers ran, each read once.
    SignatureMemo m_peerSignatures;
    /// Set once a successor owns every pane: the run loop ends without writing
    /// the records the successor now keeps or closing a child.
    bool m_hasRelinquishedService = false;
    /// The service leases this daemon claimed from its predecessors.
    std::vector<POSIX::HandoverLease> m_claimedHandoverLeases;
    /// A predecessor's listening socket this daemon serves, at the path a
    /// client may have pinned. The path is this daemon's to remove at exit.
    struct InheritedListener {
        PAL::UnixFileDescriptor descriptor;
        std::string path;
    };
    /// In the order Release carried them: the predecessor's own, the one its
    /// records are kept beside, then the rest newest first.
    std::vector<InheritedListener> m_inheritedListeners;
    /// The predecessor's bound QUIC socket, until `startQUIC` adopts it.
    PAL::UnixFileDescriptor m_inheritedQUICSocket;
    std::string m_processIDFilePath;

    // Unconsumed one-time QUIC tokens (bounded; the CAP is enforced by self-
    // eviction only — see mintQUICToken — never by evicting another client's
    // token). Each carries a monotonic expiry deadline so a leaked-but-
    // unused token can't authorize a connection forever — it's rejected + GC'd
    // past its TTL, even with no traffic.
    struct PendingQUICToken {
        std::string token;
        std::uint64_t deadlineMilliseconds; // nowMilliseconds() + TTL at mint
        std::uint32_t mintingClientID; // Client::clientID that minted this — self-eviction only
        // Identity the bootstrap token carries forward to the QUIC client that
        // presents it: the scope baked in at mint (from the uid-gated
        // Local minter's authority) + a non-spoofable per-token principal (the
        // token digest). consumeQUICToken stamps these onto the connecting client
        // so its tokenless HELLO is already authenticated at the require_token
        // gate with exactly this scope — never a blanket grant at the gate.
        Protocol::Sync::ScopeSet scope {};
        std::string tokenID;
        // Whether the connection this token authorizes stands in for the person
        // at the minting client rather than for automation. Only the mint a
        // uid-gated local client drives sets it, and a QUIC peer cannot reach
        // that mint, so a bearer can never promote itself by presenting one.
        bool isInteractiveUser = false;
    };
    std::vector<PendingQUICToken> m_pendingQUICTokens;
    // Soonest pending-token deadline, or 0 when none; lets the run loop wake
    // poll() to GC an expired token without inbound traffic.
    std::uint64_t earliestQUICTokenDeadline() const;
    void pruneExpiredQUICTokens(std::uint64_t nowMilliseconds);

    // Consent gate: verified peer exe paths granted "always allow"
    // (persisted under the control directory), and the next consent request id.
    std::vector<std::string> m_alwaysAllowedAgents;
    uint32_t m_nextConsentRequestID = 1;
    /// Ids for clipboard requests this host makes of its clients.
    ///
    /// One space across every session: an answer is matched against the client
    /// that was asked, and a daemon-wide id cannot be confused between two
    /// sessions the way a per-session counter could.
    uint32_t m_nextClipboardRequestID = 0;

    // Capability-token auth. m_tokenPath is the hashed-token store
    // (<controlDir>/tokens); m_requireToken (env UBIQUITTY_REQUIRE_TOKEN) makes
    // a tokenless non-Local peer fail closed. On unless the variable is exactly
    // `0`, which admits a tokenless same-user local agent at the `all` ceiling;
    // a Remote peer needs a token either way, and the uid-gated Local attach
    // bridge needs none.
    std::string m_tokenPath;
    /// One credential store file as `stat` described it at the last reload.
    using CredentialFileStamp = GrantFileStamp;
    /// Describes one store file as `stat` finds it now.
    ///
    /// - Parameter path: The store file.
    /// - Returns: Its identity, size and times, or the `errno` of a failed `stat`.
    static CredentialFileStamp credentialFileStamp(const std::string& path);
    /// The token and consent stores as the last reload found them; empty
    /// until a reload has read both, or while either cannot be examined.
    std::optional<std::array<CredentialFileStamp, 2>> m_credentialStoreStamp;
    /// Whether the last consent read permits store-bound authority.
    bool m_isConsentStoreWhole = true;
    /// Whether both credential stores were last read completely.
    bool m_areCredentialStoresWhole = true;
    /// Owned identities selected while a callback may still borrow a connection.
    std::vector<Protocol::Sync::ID128> m_standingConsentRetirements;
    /// Withdrawn principals whose pending attach approvals cannot survive.
    std::vector<std::string> m_withdrawnStandingConsentPrincipals;
    bool m_didFailManagementHistory = false;
    /// Records the actor and time of an explicit grant.
    ///
    /// - Parameters:
    ///   - identifier: The exact authorization record being granted.
    ///   - actor: The authenticated client approving it.
    void recordManagementCreation(const std::string& identifier, const Client& actor);
    /// Records the actor and time of an explicit grant, by the identity the
    /// management history names its actor with.
    ///
    /// - Parameters:
    ///   - identifier: The exact authorization record being granted.
    ///   - identity: ``managementIdentityOf(_:)`` of the approver.
    void recordManagementCreation(const std::string& identifier, const std::string& identity);
    /// The name the management history gives an actor.
    ///
    /// - Parameter actor: The authenticated client.
    /// - Returns: The local owner by uid, or the actor's grant key.
    [[nodiscard]] std::string managementIdentityOf(const Client& actor) const;
    /// Voids every approval this client gave that has not been delivered: its
    /// authority was withdrawn by the owner, or its token revoked or expired,
    /// before the agent it allowed took the approval.
    ///
    /// - Parameter approverClientID: The client whose approvals end.
    void voidApprovalsGivenBy(std::uint32_t approverClientID);
    /// Voids undelivered approvals authorized by one stored token digest.
    ///
    /// The principal survives reaping of the connection that approved.
    /// - Parameter tokenID: The revoked or expired token's digest.
    void voidApprovalsGivenUnderToken(std::string_view tokenID);
    /// Records a successful token authentication without affecting authorization.
    ///
    /// - Parameter actor: The authenticated client using the token.
    void recordManagementTokenUse(const Client& actor);
    bool m_requireToken = true;

#ifdef UBIQUITTY_QUIC
    std::unique_ptr<QUICListener> m_quic; // the QUIC data-plane listener, or null
#endif
};

/// Test-only accessor that runs one pass of the accept loop.
struct AcceptUnderExhaustionAccess {
    /// Runs `acceptClient` once against whatever is pending.
    ///
    /// - Parameter daemon: The daemon whose listener to service.
    static void acceptOnce(Daemon& daemon) { daemon.acceptClient(); }

    /// The listening descriptor, for asking whether anything is still pending.
    ///
    /// - Parameter daemon: The daemon to inspect.
    /// - Returns: The descriptor, or -1 when the daemon is not listening.
    static int listenDescriptor(Daemon& daemon) { return daemon.m_listenFileDescriptor.get(); }
};

} // namespace Host
} // namespace Ubiquitty
