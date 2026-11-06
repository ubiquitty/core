// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
// The portable remote-session state machine. Drives a Sync session over a
// Transport::Channel: connect → HELLO and Greet → apply the Sync screen plane
// into a passive Terminal → reconnect on a drop → stop. The platform supplies thin hooks (Dispatcher,
// ChannelFactory, RemoteSessionObserver).
//
// Single-threaded by contract: every public method and handler runs on the session
// thread. The Channel fires callbacks on its own I/O thread; the core marshals each
// onto the session thread via the Dispatcher, so the Terminal is only touched there.

#include "Base/WeakPtr.h"
#include "Core/Clipboard/Scope.h"
#include "Core/Protocol/Control/PasteUpload.h"
#include "Core/Protocol/Control/Reclaim.h"
#include "Core/Protocol/Sync/Consent.h"
#include "Core/Protocol/Sync/HistoryPull.h"
#include "Core/Protocol/Sync/StructureConsent.h"
#include "Core/Protocol/Sync/SyncClientCore.h"
#include "Core/Protocol/Sync/SyncTrace.h"
#include "Core/Session/BootstrapResponse.h"
#include "Core/Session/EventLog.h"
#include "Core/Session/FileLink.h"
#include "Core/Session/LayoutTree.h"
#include "Core/Session/PaneLink.h"
#include "Core/Session/PaneLinkEvents.h"
#include "Core/Session/PostedWork.h"
#include "Core/Session/Presence.h"
#include "Core/Session/SessionRows.h"
#include "Core/Session/SharedWorkspace.h"
#include "Core/Session/WorkspaceIntent.h"
#include "Core/Session/WorkspaceWire.h"
#include "Core/Transport/Channel.h"
#include "Core/Transport/InboundBackpressure.h"
#include "PAL/NetworkPath.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Ubiquitty {
struct Terminal;
}

namespace Ubiquitty::Session {
class HostListing;
struct TreeWorkspace;

/// A host advertises `uname -sm` in Welcome. Only Darwin hosts have a
/// Command modifier on their native keyboard.
[[nodiscard]] inline bool isMacOSHostPlatform(std::string_view platform)
{
    return platform.starts_with("Darwin ") && platform.size() > sizeof("Darwin ") - 1;
}

struct RemoteSessionDatagramIngressAccess;
class WorkspaceConnection;
struct WorkspaceConnectionStorage;

// Connection phase, surfaced for status UI. (SESSION-level Reconnecting is
// here; the Channel's own Health is per-attempt and not exposed.) Frontends read
// it as it is, through the session (RuntimeDrive.remoteState on Apple).
enum class State : std::uint8_t {
    Idle = 0, // constructed; start() not called
    Connecting, // a transport is being stood up; awaiting HELLO_ACK
    Connected, // HELLO_ACK seen; frames flow
    Reconnecting, // dropped; backing off before the next attempt
    Failed, // stopped, or the host's shell exited — terminal
    Deploying, // the host signalled it has no daemon binary; the platform is
               // pushing one and will call deployFinished()
};

// Outcome the platform reports back from an onDeployRequested() deploy.
enum class DeployResult : std::uint8_t {
    Succeeded, // the binary is in place; reconnect and resume the handshake
    Failed, // the push/build failed; the session is terminal
};

/// The answer to one captured file-download question, exposed independently
/// of the transport-owning class so native frontends can import it.
enum class FileDownloadDecision : std::uint8_t {
    Cancel,
    Download
};

/// The answer to one captured host-window close question.
enum class RemoteWindowCloseDecision : std::uint8_t {
    Cancel,
    CloseWindow
};

/// Runs work on the session's single thread, immediately or after a delay.
///
/// An implementation owns each unit it is handed, and runs or destroys it
/// exactly once. A frontend's wraps its event loop; a test's queues the units
/// and runs them when the case says.
class Dispatcher {
public:
    virtual ~Dispatcher() = default;

    /// Takes one unit to run on the session thread once the current turn ends.
    ///
    /// - Parameter work: The unit; the dispatcher owns it from here, and runs
    ///   or destroys it exactly once.
    virtual void post(PostedWork work) = 0;

    /// Takes one unit to run on the session thread once `seconds` have passed.
    ///
    /// - Parameters:
    ///   - seconds: The delay.
    ///   - work: As ``post``.
    virtual void postDelayed(double seconds, PostedWork work) = 0;
};

/// Which carrier a remote session runs over.
enum class TransportMode : uint8_t {
    TCP,
    QUIC
};

/// What a HELLO naming a session asks the daemon to do when that session has
/// ended.
///
/// Mirrored by the Apple kit's `RemoteReattachPolicy`, which pins the same
/// values.
enum class ReattachPolicy : uint8_t {
    /// Reattach, or create a fresh session in place of one that ended. What a
    /// restored window wants: its session outliving a restart is the point,
    /// and a shell is better than none.
    AttachOrCreate = 0,
    /// Reattach, or be refused with `SessionGone`; never create. What a
    /// connect returning to a remembered session wants: a fresh shell there
    /// would be taken for the one the person came back to.
    AttachOnly = 1,
};

/// The notice a connection carries when the host refuses to hand back the
/// session the handshake asked to reattach.
///
/// Written once and read from both ends of the report — the state the core
/// fails into and the flag surface a frontend reads beside it — so the two
/// cannot word one refusal differently.
///
/// - Parameters:
///   - sessionID: The session the refused HELLO named.
///   - reason: The wire reason the host refused with.
/// - Returns: A phrase for a status line, naming the session.
[[nodiscard]] std::string reattachRefusedNotice(std::uint32_t sessionID, Protocol::Sync::ByeReason reason);

/// The SHA-256 of a host binary being uploaded, which the install on the host
/// verifies before it replaces anything (HostInstall.h).
///
/// Defined out of line, in the unit every client that installs a host links,
/// so the headers above it stay free of the crypto backend's: the Swift
/// importer reads them and cannot see those.
///
/// - Parameter bytes: The upload.
/// - Returns: The digest, lowercase hex.
[[nodiscard]] std::string hostUploadDigest(std::string_view bytes);

/// What a carrier presents to continue an attachment without the login that
/// began it (Docs/sync.md §6.9 "Resume without SSH"): a preamble only that
/// connection can compute, in place of a bootstrap token.
struct ResumeKey {
    /// The label the connection exports its keying material under.
    std::string exporterLabel;
    /// Builds the preamble from the 32 bytes the handshake exported. Called
    /// once, on the carrier's own thread: it holds copies and no session
    /// state.
    std::function<std::vector<std::uint8_t>(std::span<const std::uint8_t, 32> exporter)> buildPreamble;
};

/// Whether repeating a failed carrier setup can change its outcome.
enum class ChannelFailureRecovery : std::uint8_t {
    Retry,
    Stop,
    /// An authenticated bootstrap offered no QUIC carrier. Try the factory's
    /// same-owner stream once; never use for a trust or credential refusal.
    UseStream,
    /// The bootstrap was pinned to a daemon nothing listens for any longer.
    /// Neither a retry nor the stream can reach it; the session drops the
    /// pin and asks the host's current daemon instead.
    InstanceGone,
};

/// A factory's failure before it could construct a carrier. This is not a
/// host session refusal and never authorizes another session or daemon.
struct ChannelCreationFailure {
    ChannelFailureRecovery recovery = ChannelFailureRecovery::Stop;
    std::string detail;
};

// Builds a transport for one (re)connect attempt. ASYNC: a QUIC carrier needs
// a bootstrap subprocess, so the channel arrives via `done` (called on the
// session thread). `transport` is TransportMode::TCP once QUIC has failed this session.
// A null completion has no carrier; creationFailure may supply its cause and
// retry policy. Untyped factories keep the established reconnect policy.
/// Where a resumable QUIC carrier reached the daemon: the address the
/// bootstrap's own connection reached, the daemon's port and its certificate
/// pin. A frontend keeps it beside its resume record, so the next process can
/// dial the daemon again by a resume proof with no login.
struct ResumeReach {
    std::string address;
    std::uint16_t port = 0;
    std::array<std::uint8_t, 32> certificatePin {};

    friend bool operator==(const ResumeReach&, const ResumeReach&) = default;
};

class ChannelFactory {
public:
    virtual ~ChannelFactory() = default;
    /// The answer to one createChannel: the channel built, or null when none could be.
    ///
    /// Called exactly once, and may be called after createChannel returns — a factory that shells
    /// out answers when that finishes. Calling it after the session that asked has been released is
    /// allowed and does nothing: the completion holds a weak reference to that session. So a factory
    /// whose own owner vanished mid-build answers null rather than dropping the completion.
    using Done = std::function<void(std::unique_ptr<Ubiquitty::Transport::Channel>, bool isQUIC)>;
    virtual void createChannel(TransportMode transport, Done) = 0;
    /// The failure accompanying the most recent null completion, if typed by
    /// the embedder. Set before calling Done and cleared for a new attempt.
    [[nodiscard]] virtual std::optional<ChannelCreationFailure> creationFailure() const { return std::nullopt; }
    /// Whether the factory can dial again the carrier its last channel
    /// reached the host over without the login that minted it: a QUIC
    /// carrier whose address, port and certificate pin it kept.
    [[nodiscard]] virtual bool canResumeChannel() const { return false; }
    /// Dials that carrier, which presents the key in place of a bootstrap
    /// token. Nothing is spawned and no login runs. The answer is `Done`'s as
    /// for `createChannel`; null when the factory cannot.
    ///
    /// - Parameters:
    ///   - key: What the carrier presents.
    ///   - done: Receives the channel.
    virtual void resumeChannel(ResumeKey /*key*/, Done done) { done(nullptr, false); }
    /// The reach `resumeChannel` would dial, when the factory keeps one.
    ///
    /// - Returns: The reach, or `nullopt` when nothing can be dialled again.
    [[nodiscard]] virtual std::optional<ResumeReach> resumeReach() const { return std::nullopt; }
    /// Adopts a reach a previous process kept, before any carrier exists. A
    /// factory takes one only while it holds none of its own.
    ///
    /// - Parameter reach: A numeric address, a port and a nonzero pin.
    /// - Returns: Whether the factory will dial it on a resume.
    virtual bool seedResumeReach(const ResumeReach& /*reach*/) { return false; }
    /// Invalidates bootstrap failure state after installing a host executable.
    virtual void hostDeploymentFinished() { }
    /// Pins subsequent connection attempts to the daemon acknowledged by the host.
    ///
    /// - Returns: Whether the factory accepts the daemon identity.
    virtual bool pinDaemonSocket(std::string_view) { return true; }
    /// Drops predecessor socket and cached carrier reach after a welcomed host
    /// requests handover. Authentication and the expected issuer are retained.
    virtual void clearHandoverReach() { }
    /// Why a QUIC carrier was not used, once one was asked for and fell back.
    ///
    /// - Returns: The reason, verbatim for a person to read; empty when QUIC
    ///   has not fallen back.
    [[nodiscard]] virtual std::string quicFallbackReason() const { return {}; }

    /// A control errand has no QUIC-then-stream retry sequence. It asks once
    /// for the carrier this factory supplies, independently of session fallback.
    [[nodiscard]] virtual TransportMode controlTransport() const { return TransportMode::TCP; }

    /// A host-only errand, distinct from a kept session's transport fallback.
    /// Native factories may enforce a control-only first HELLO on this door.
    virtual void createControlChannel(Done done) { createChannel(controlTransport(), std::move(done)); }
};

/// The carrier a connection is riding.
enum class Carrier : std::uint8_t {
    /// No carrier is up.
    None UBIQUITTY_SWIFT_NAME(none) = 0,
    /// The ssh or local stream.
    Stream UBIQUITTY_SWIFT_NAME(stream) = 1,
    /// QUIC.
    QUIC UBIQUITTY_SWIFT_NAME(quic) = 2,
};

/// Which carrier a connection rides, and why QUIC is not it when it was
/// offered.
struct CarrierFact {
    Carrier carrier { Carrier::None };
    /// Why QUIC fell back, verbatim; empty when it did not.
    std::string quicFallbackReason;
};

/// The last refusal a host answered a handshake with.
struct HostRefusal {
    /// Whether the host refused at all; the other fields are read only then.
    bool hasRefusal { false };
    Protocol::Sync::ByeReason reason { Protocol::Sync::ByeReason::TokenMissing };
    /// The notice the connection carried, verbatim.
    std::string detail;
    /// Which end of a version refusal is older.
    Protocol::Sync::OlderSide olderSide { Protocol::Sync::OlderSide::Unknown };
    /// What the refusing host said about its build.
    Protocol::Sync::PeerBuild hostBuild;
    /// The refusing host's `uname -sm`, empty when it did not say.
    std::string hostPlatform;
};

// Frontend events, in portable types. The platform adapter implements this and
// translates to its native delegate (NS types on macOS). Pure pass-throughs
// for the Sync frames the core does not act on itself.
class RemoteSessionObserver {
public:
    virtual ~RemoteSessionObserver() = default;
    virtual void onStateChanged(State /*state*/, const std::string& /*detail*/) { }
    /// Reports that the session's screen took a checkpoint or an update from
    /// the host, once per batch of bytes however many applied.
    virtual void onScreenChanged() { }
    /// Reports the window title the remote host most recently set.
    ///
    /// - Parameter utf8: Valid only for the duration of the call.
    virtual void onTitle(std::string_view /*utf8*/) { }
    virtual void onPaneFacts(const Protocol::Sync::PaneFacts& /*facts*/) { }
    virtual void onBell() { }
    virtual void onExit() { } // host shell exited — close the window
    /// Reports a typed command completion. The passive client does not parse
    /// the host's OSC 133 marks; presentation is the frontend's disposition.
    ///
    /// - Parameters:
    ///   - effect: The borrowed pane identity and ordered effect metadata.
    ///   - command: The borrowed command context, valid only for this call.
    ///   - disposition: Whether this client posts, badges or ignores presentation.
    virtual void onCommandDone(const Protocol::Sync::EffectView& /*effect*/, const Protocol::Sync::CommandFinishedEffect& /*command*/, Protocol::Sync::EffectDisposition /*disposition*/) { }
    // HELLO_ACK pinned a (possibly fresh) session id: persist + claim it.
    virtual void onSessionEstablished(uint32_t /*sessionID*/) { }
    virtual void onPresence(const PanePresenceSnapshot& /*presence*/) { }
    /// Reports a host-observed agent read for one bound pane, without screen contents.
    virtual void onScreenAccess(const Event& /*event*/) { }
    virtual void onConsentRequest(const Protocol::Sync::ConsentQuestion& /*request*/) { }
    /// Asks about one exact tree operation, routed to this attached pane.
    ///
    /// - Parameter request: The host's immutable operation description.
    virtual void onStructureConsent(const Protocol::Sync::StructureConsent& /*request*/) { }

    /// A question was answered, expired, or lost its authority.
    virtual void onConsentResolved(const Protocol::Sync::ConsentResolution& /*resolution*/) { }
    virtual void onReclaimed() { } // the daemon handed an adopted session back
    /// The connection this pane rides retires into another of its workspace
    /// (`WorkspaceConnection::retireInto`); the pane moves when it can.
    virtual void onConnectionRetiring() { }
    // The host has no usable daemon binary (the bootstrap printed the deploy
    // sentinel before any handshake). `platformArch` is the remote's
    // `uname -sm` (e.g. "Linux x86_64"). The platform pushes a binary/source
    // and then calls deployFinished(). Fired at most once per core lifetime.
    virtual void onDeployRequested(const std::string& /*platformArch*/) { }
    /// Delivers one complete correlated file result, transferring its byte owner.
    /// The endpoint incarnation and original pane/path have already been checked.
    virtual void onFileData(Protocol::Control::FileReadReply&& /*file*/) { }
    /// Reports where a pushed paste landed on the host, or why it did not.
    ///
    /// - Parameter ack: The push it answers, the verdict, and the landed
    ///   path when there is one.
    virtual void onPastePushAck(const Protocol::Control::PastePushReply& /*ack*/) { }

    /// A program on the host asked for a clipboard the host does not have.
    ///
    /// The platform answers with ``RemoteSessionCore/serveClipboard``, which it
    /// may do later: reading a pasteboard can need a user gesture, and the host
    /// waits on the reply rather than on this call returning.
    ///
    /// An observer that does not override this serves nothing and the host's
    /// tool reports an empty clipboard, which is what a client that never
    /// offered to serve should look like.
    ///
    /// - Parameter request: What is wanted, and the id to answer with.
    virtual void onClipboardRequest(const Protocol::Sync::ClipboardRequestMessage& /*request*/) { }
    [[nodiscard]] virtual bool supportsNativeDrag() const { return false; }
    /// Delivers a native command for this endpoint's attached pane.
    virtual void onNativeDragCommand(const Protocol::Sync::NativeDragCommand&) { }
    /// Delivers the primary human's complete typed clipboard data, borrowed
    /// through this call. A queued consumer must copy what it keeps.
    ///
    /// - Parameters:
    ///   - buffer: The original destination selector.
    ///   - items: All validated representations, including an empty clear.
    virtual void onClipboardWriteItems(ClipboardBuffer /*buffer*/, std::span<const ClipboardItem> /*items*/) { }
    /// Posts the primary human's full admitted notification, including
    /// replacement and timing. A queued consumer must copy what it keeps.
    ///
    /// - Parameter request: Fields borrowed for this call.
    virtual void onNotificationRequest(const NotificationRequest& /*request*/) { }
    /// Requests attention on the elected human's frontend.
    virtual void onAttention() { }
    /// Reports an effect the host delivered that this client's own policy
    /// holds for the person: a clipboard write, a notification, a sound or an
    /// attention request from a host on another machine. The frontend asks, then answers with
    /// ``RemoteSessionCore/answerHeldHostEffect``; an observer that does not
    /// override this leaves every such effect unapplied.
    ///
    /// - Parameters:
    ///   - token: The held effect, for its answer.
    ///   - capability: What the effect would do.
    virtual void onHostEffectHeld(std::uint64_t /*token*/, Capability /*capability*/) { }
    /// Reports that a held effect is gone without an answer, so a question
    /// shown for it should close; see ``Protocol::Sync::SyncClientObserver/onHostEffectWithdrawn``.
    ///
    /// - Parameters:
    ///   - token: The token ``onHostEffectHeld`` reported for it.
    ///   - reason: Whether a newer one replaced it or its connection ended.
    virtual void onHostEffectWithdrawn(std::uint64_t /*token*/, Protocol::Sync::HostEffectWithdrawal /*reason*/) { }
    /// Reports a change to the requested or acknowledged clipboard source.
    virtual void onClipboardRoutingChanged() { }

    /// Answers a ``RemoteSessionCore/requestSessionList`` with what the host
    /// holds.
    ///
    /// - Parameters:
    ///   - requestID: The ask this answers, as `requestSessionList` returned
    ///     it. A reply carrying anything else never reaches here.
    ///   - sessions: The host's tree read one row a session, in its order,
    ///     whole. Valid only for the duration of the call.
    virtual void onSessionList(uint32_t /*requestID*/, const std::vector<SessionRow>& /*sessions*/) { }
    /// Reports a refused, incomplete or timed-out one-shot listing.
    /// - Parameter requestID: The retired request identity.
    virtual void onSessionListFailed(uint32_t /*requestID*/) { }

    /// The mirror was rebuilt from a structure the host sent: on
    /// attach, and after every verb the host performed, from any client.
    /// Read it through ``RemoteSessionCore/workspace`` and
    /// ``RemoteSessionCore/sessionForPane``.
    virtual void onWorkspaceChanged() { }
    /// Reports one canonical pane ending without closing the workspace carrier.
    /// - Parameter sessionID: The exact bound session that ended.
    virtual void onPaneEnded(std::uint32_t /*sessionID*/) { }
    /// Reports a workspace pane that cannot be shown, with the reason.
    /// - Parameters:
    ///   - sessionID: The session the pane shows.
    ///   - reason: The sentence to show, borrowed for this call.
    virtual void onPaneFailed(std::uint32_t /*sessionID*/, std::string_view /*reason*/) { }
    /// Routes an effect through the physical workspace connection.
    ///
    /// - Parameters:
    ///   - sessionID: The authenticated bound session.
    ///   - effect: Stable identity and count, borrowed for this call.
    ///   - body: Validated fields, borrowed for this call.
    ///   - disposition: The connection's role and primary decision.
    virtual void onPaneEffect(uint32_t /*sessionID*/, const Protocol::Sync::EffectView& /*effect*/, const Protocol::Sync::EffectBody& /*body*/, Protocol::Sync::EffectDisposition /*disposition*/) { }
    /// Routes an exact clipboard question to a bound workspace endpoint.
    ///
    /// - Parameters:
    ///   - sessionID: The authenticated source pane.
    ///   - request: The operation, still subject to that endpoint's local policy.
    /// - Returns: Whether an endpoint accepted responsibility for answering.
    virtual bool onPaneClipboardRequest(uint32_t /*sessionID*/, const Protocol::Sync::ClipboardRequestMessage& /*request*/) { return false; }
    /// Delivers a workspace command to the endpoint for its named pane.
    virtual void onPaneNativeDragCommand(std::uint32_t /*sessionID*/, const Protocol::Sync::NativeDragCommand&) { }

    /// The terminal a workspace pane's screen replicates into.
    ///
    /// Asked by a workspace carrier when the host binds a pane, and answered by
    /// the pane's own core while one is attached. This is a non-reentrant
    /// lookup: it must not stop or reconnect the carrier, and the returned
    /// terminal must outlive the binding.
    ///
    /// - Parameter sessionID: The session the pane shows.
    /// - Returns: The terminal, or null while no pane core holds the session.
    virtual Ubiquitty::Terminal* paneTerminal(std::uint32_t /*sessionID*/) { return nullptr; }

    /// Reports that a workspace pane's screen changed.
    ///
    /// - Parameter sessionID: The session the pane shows.
    virtual void onPaneScreenChanged(std::uint32_t /*sessionID*/) { }

    /// Delivers the whole facts record for a workspace pane.
    ///
    /// - Parameters:
    ///   - sessionID: The session the record describes.
    ///   - facts: The checked record, borrowed for this call.
    virtual void onWorkspacePaneFacts(std::uint32_t /*sessionID*/, const Protocol::Sync::PaneFacts& /*facts*/) { }

    /// Reports that the host bound a workspace pane on the carrier, so frames
    /// queued for it can leave.
    ///
    /// - Parameter sessionID: The session the pane shows.
    virtual void onPaneBound(std::uint32_t /*sessionID*/) { }

    /// Reports that a workspace carrier read bytes that may have settled its
    /// panes' input, so each pane's own core tells its observer what moved.
    virtual void onPaneInputSettled() { }

    /// Answers a ``RemoteSessionCore/sendWorkspaceVerb``.
    ///
    /// - Parameter result: The verb's outcome and what it minted; its
    ///   `requestID` is what `sendWorkspaceVerb` returned.
    virtual void onWorkspaceVerbResult(const Ubiquitty::Session::WorkspaceIntentResult& /*result*/) { }

    /// The publication of a locally shared window settled or lost its connection.
    virtual void onSharedWorkspaceChanged() { }

    /// The host would not hand this connection back the session it named.
    ///
    /// Terminal for that session: the core stops reconnecting rather than
    /// presenting the same id again. A frontend answers by listing what the
    /// host does hold or by attaching fresh, which are the two things this
    /// connection cannot decide for it.
    ///
    /// - Parameters:
    ///   - sessionID: The session the refused HELLO asked to reattach.
    ///   - reason: The wire reason the host refused with.
    virtual void onReattachRefused(uint32_t /*sessionID*/, Protocol::Sync::ByeReason /*reason*/) { }

    /// Reports that the link's indicators changed: the host went silent past
    /// the silence it may keep, input has waited past its bound for a
    /// receipt, or either cleared (Docs/sync.md §6.9 "Stalled is not lost").
    /// A stalled link is still connected; a frontend marks the pane and
    /// keeps its picture interactive.
    ///
    /// - Parameter link: The link as it stands.
    virtual void onLinkState(const Protocol::Sync::LinkState& /*link*/) { }

    /// Reports that the count of the pane's input events in doubt, or of
    /// those discarded, changed. Events are in doubt when a reconnect could
    /// not learn whether the host wrote them: the session was attached anew,
    /// or the host kept no count. They are never sent again unasked, since
    /// one may have been written already; a frontend shows them and offers
    /// `resendInDoubt` and `discardInDoubt`. New input waits behind them
    /// until the person decides.
    ///
    /// - Parameter status: The pane's input as it stands.
    virtual void onInDoubt(const Protocol::Sync::PaneInputStatus& /*status*/) { }
};

class RemoteSessionCore final : public Ubiquitty::CanMakeWeakPtr,
                                public PaneLink,
                                private Ubiquitty::Transport::ChannelObserver,
                                private Protocol::Sync::SyncClientObserver {
public:
    // `term`, `dispatcher`, `factory`, `observer` are caller-owned and outlive
    // the core. `term` is an initialized passive Terminal, written only here.
    RemoteSessionCore(Ubiquitty::Terminal*, Dispatcher*, ChannelFactory*, RemoteSessionObserver*);
    /// Creates a pane decoder borrowing the workspace's physical connection owner.
    RemoteSessionCore(Ubiquitty::Terminal*, WorkspaceConnection&, RemoteSessionObserver*);
    ~RemoteSessionCore() override;

    RemoteSessionCore(const RemoteSessionCore&) = delete;
    RemoteSessionCore& operator=(const RemoteSessionCore&) = delete;

    /// Takes the prepared cache/store before this physical owner starts.
    ///
    /// - Parameter prepared: The same core that restored before any network I/O.
    /// - Returns: Whether ownership transferred without opening a carrier.
    [[nodiscard]] bool adoptPreparedResume(std::unique_ptr<Protocol::Sync::SyncClientCore> prepared);

    /// The reach this connection's factory would dial again by a resume
    /// proof: what a frontend keeps beside its resume record.
    ///
    /// - Returns: The reach, or `nullopt` when the carrier cannot be resumed.
    [[nodiscard]] std::optional<ResumeReach> resumeReach() const { return m_factory ? m_factory->resumeReach() : std::nullopt; }

    /// How many of the panes the connection's last Welcome resumed the host
    /// continued as `outcome` says: a diff, a checkpoint, or nothing left.
    ///
    /// - Parameter outcome: The outcome counted.
    /// - Returns: The count; 0 with no live connection or no resume.
    [[nodiscard]] std::size_t resumedPaneCount(Protocol::Sync::ResumeOutcome outcome) const;

    /// Changes clipboard serving for this connection and subsequent reconnects.
    ///
    /// Enabling an established connection requires host routing support.
    /// Revoking access always takes effect locally, including with older hosts.
    bool setClipboardServing(Clipboard::Serving);

    /// Whether clipboard routing can be changed without reconnecting.
    [[nodiscard]] bool canChangeClipboardServing() const;

    /// Whether a requested source is awaiting the host acknowledgement.
    [[nodiscard]] bool isClipboardRoutingPending() const;

    /// Whether this connection currently permits serving its clipboard.
    [[nodiscard]] bool didClaimClipboardServe() const;

    /// Returns the authoritative workspace name after its first layout.
    [[nodiscard]] const std::string& workspaceName() const;

    /// Reports whether the host recreated this workspace from its record after
    /// it restarted, so every pane's shell is fresh and its earlier screen gone.
    ///
    /// - Returns: The bit the last layout carried; false before the first.
    [[nodiscard]] bool isWorkspaceRestoredAfterHostRestart() const;

    /// Names the workspace this connection's HELLO attaches to or creates.
    ///
    /// Read when a HELLO is built, so it must be set before ``start`` to reach
    /// the first one; a reconnect rebuilds the HELLO and re-reads it. An empty
    /// name leaves the connection unnamed, which is the create-or-reattach by
    /// session id the handshake has without it.
    ///
    /// A reattach carries the name too. The daemon ignores it by contract —
    /// a reattach observes what its session already is — so the name is sent
    /// from one place rather than from the subset of connects that create.
    ///
    /// - Parameters:
    ///   - workspaceName: The name a creating HELLO attaches under if the
    ///     daemon holds a session bearing it, and creates under otherwise.
    ///   - endsWhenDetached: Whether a session created under the name ends when
    ///     its last human detaches cleanly.
    void setWorkspace(std::string workspaceName, bool endsWhenDetached) // bool-ok: mirrors the single HELLO flag bit helloSessionEndWhenDetached; an enum here would be a second spelling of one wire bit
    {
        m_workspaceName = std::move(workspaceName);
        m_endsWhenDetached = endsWhenDetached;
    }

    /// Sets what every HELLO naming a session on this connection asks when
    /// that session has ended.
    ///
    /// Read when a HELLO is built, like ``setWorkspace``, so it must be set
    /// before ``start``; a reconnect re-reads it. A HELLO naming no session
    /// creates one whatever this says.
    ///
    /// - Parameter policy: Attach-or-create, or attach-only.
    void setReattachPolicy(ReattachPolicy policy) { m_reattachPolicy = policy; }

    // Begin the handshake. attachSessionID 0 = a fresh session; nonzero =
    // reattach an existing daemon session. columns/rows seed the HELLO geometry.
    void start(uint32_t attachSessionID, int columns, int rows);

    // Detach cleanly: send EXIT (the daemon keeps the session), tear the
    // transport down, and stop reconnecting. Idempotent.
    void stop();

    /// Sends one input event to the session's pane as the person meant it:
    /// keys, pointer events and a paste are encoded on the host under the
    /// modes live when it writes them (Docs/sync.md §6.7). The event is
    /// numbered in the pane's order and the host writes it once, whichever
    /// connection carries it: typed while the session reconnects, it waits
    /// within the attachment's bound and goes on the connection that
    /// continues the session.
    ///
    /// - Parameter intent: The event.
    /// - Returns: Whether it was taken. Not before the host has named the
    ///   session, for a pane that ended, for an event that is not one frame,
    ///   or once what waits has reached its bound.
    bool send(Protocol::Sync::InputIntent intent); // bool-ok: whether the event was taken

    /// Copies borrowed pane input into the attachment's bounded event queue.
    /// - Parameter input: The event; only raw input uses the local encoding.
    /// - Returns: Whether every event or raw chunk was admitted; false for
    ///   raw input with no bytes.
    bool send(const PaneInput& input) override;

    [[nodiscard]] PaneView view() override;
    [[nodiscard]] std::optional<std::uint8_t> lineEraseCharacter() const override;
    [[nodiscard]] bool wantHistory(Protocol::Sync::HistoryPriority, std::span<const Protocol::Sync::LineRange>) override;
    void setViewOffset(int rowsAboveTail) override;
    [[nodiscard]] LinkState linkState(std::uint64_t now) const override;
    [[nodiscard]] std::uint64_t clockMicroseconds() const override;
    void setFocus(Ubiquitty::Modes::FocusState focus) override { setSurfaceFocus(focus); }
    // The host reflows and sends the verified replacement picture. Reporting
    // the drawable must never resize the mirror ahead of that picture.
    void setViewport(const PaneViewport& viewport) override { setSize(viewport.columns, viewport.rows); }
    // Pixel geometry comes from the frontend's complete staged metrics,
    // not a partial copy in the resize request or the host's verified model.
    void setWindowSize(int, int, int, int) override { stateViewport(); }
    void observe(PaneLinkObserver& observer) override
    {
        if (m_linkEvents.observe(observer))
            m_linkFactsRevision.reset();
    }
    void pollEvents(std::uint64_t now) override;

    /// Reports what the device's network path costs, as the platform sees
    /// it. Core states the cost to the host as `Facts.link`: an expensive
    /// path is `Metered` and a constrained one `Constrained`, and the host
    /// then pushes no history beyond one viewport (Docs/sync.md §6.6). Kept
    /// for every later greeting; a workspace pane reports it for the shared
    /// connection.
    ///
    /// - Parameter cost: The path's cost.
    void setNetworkCost(PAL::NetworkCost cost);

    /// Where to lay this pane out, and whether the picture held fills that
    /// room (Docs/sync.md §6.10 "Topology never overtakes content"). A
    /// structure that names a size the pane's checkpoint has not yet brought
    /// leaves the old picture at its own size, letterboxed.
    ///
    /// - Returns: The rectangle, or `nullopt` before the pane has a picture
    ///   and a structure naming it.
    [[nodiscard]] std::optional<Protocol::Sync::PaneRectangle> paneRectangle() const;

    /// Keeps the surface's authenticated cold picture until a live frame replaces it.
    void adoptRestoredPreview(const FrameReady& frame) { m_restoredPreview = frame; }

    /// Sends bytes to the session's program as they are: `Raw` events, in
    /// order. For input that is already bytes, an agent's or a reply a
    /// frontend composed; what a person typed goes as keys through `send`.
    ///
    /// - Parameter bytes: The bytes. A sequence carrying an embedded NUL —
    ///   Ctrl-Space is one — has to be built from a pointer and a length,
    ///   since the one-argument form stops at the first NUL.
    void sendInput(std::string_view bytes);
    [[nodiscard]] bool sendNativeDragEvent(const DragDrop::NativeEvent&, std::span<const char>);

    /// What the person is told of the pane's input: events whose fate the
    /// host could not report after a reconnect, events waiting behind them or
    /// for a connection, and events discarded.
    [[nodiscard]] Protocol::Sync::PaneInputStatus inputStatus() const;

    /// Sends the pane's events in doubt again, as the person asked: the host
    /// writes each it had not written, once.
    void resendInDoubt();

    /// Discards the pane's events in doubt, as the person asked, and lets the
    /// input waiting behind them go.
    void discardInDoubt();

    /// Tells the host this client's typography table, so it measures and lays
    /// out the session's rich text in the fonts this client draws
    /// (Docs/rich-text.md).
    ///
    /// - Parameter facts: The table.
    void publishTypography(const RichText::TypographyFacts& facts);

    /// Tells the host the faces this client draws proportional runs in, so
    /// it answers the session's measure queries in them
    /// (Docs/proportional-text.md).
    ///
    /// - Parameter faces: The faces, from the client's measurer.
    void publishProportionalFaces(const OSC::TextSizing::ProportionalFaceFacts& faces);

    /// Whether this pane is attached as a human who may drive it, so its
    /// drawn faces can affect the host's proportional measure answers.
    [[nodiscard]] bool mayPublishProportionalFaces() const;

    /// Tells the host the colours a theme apply produced.
    ///
    /// Sent only when the host acknowledged ThemeControl: against one that did
    /// not, the frame would be dropped and the client would go on believing the
    /// program had been told the scheme changed.
    ///
    /// - Parameters:
    ///   - palette: What renders, and what OSC 4/10/11 mutate.
    ///   - baseline: What OSC 110/111/112 revert to.
    /// - Returns: Whether the request was sent to a connected, capable host.
    bool sendPalette(const TermPalette& palette, const TermPalette& baseline);
    /// Asks the host to run a clear command on its Terminal.
    ///
    /// The host owns the session's screen and history, so the command runs
    /// there exactly as it runs on a local session, redraw request included,
    /// and every attached client sees the result in its next frame.
    ///
    /// - Parameter command: Which clear the user asked for.
    /// - Returns: True when the frame went out; false before HELLO_ACK.
    [[nodiscard]] bool sendClearBuffer(Ubiquitty::Screen::ClearCommand command);
    /// Reports the grid this client's drawable holds for the session's pane.
    ///
    /// The fact is the client's and stays its own: the host decides what the
    /// pane is sized to, among every client showing it, and its frames carry
    /// that grid into the mirror. The two differ whenever another client's
    /// size applies, and neither replaces the other here. The host is told
    /// when the grid or the published pixels differ from what it was last
    /// told, and at the acknowledgement of each connection. A pane whose
    /// frontend has reported nothing yet states nothing: it is not laid out,
    /// and the grid it was launched with is nobody's drawable.
    ///
    /// - Parameters:
    ///   - columns: The columns the drawable holds; below one is ignored.
    ///   - rows: The rows, likewise.
    void setSize(int columns, int rows);

    /// Stages the pixels this client draws its grid in without changing the
    /// host's model. The following viewport or window-size report carries
    /// the complete set together, so a grid resize never sends old pixels.
    /// Metrics staged before attachment accompany its first viewport.
    void setPixelGeometry(const PixelGeometry&) override;

    /// Reports whether nothing this pane started on its connection is still
    /// open there, so the pane may move to another connection of its
    /// workspace without losing or repeating any of it.
    ///
    /// Settled means no input the host has not reported written, no reclaim,
    /// file, paste, window close or workspace request awaiting its answer,
    /// and no held host effect awaiting the person. Each of those retires on
    /// the connection that took it.
    ///
    /// - Returns: Whether the pane is attached and settled.
    [[nodiscard]] bool isSettledForMove() const;
    /// Takes on the presentation another endpoint gave the same pane: the
    /// size and pixels its frontend reported, its focus and view, and the
    /// newest effect it received, so an effect both connections carry is
    /// shown once. Called before ``start``.
    ///
    /// - Parameter predecessor: The endpoint the pane is leaving.
    void continueEndpoint(const RemoteSessionCore& predecessor);
    /// States this pane's view to its connection again, once it is attached.
    void restateView();

    // "Stop Sharing": ask the daemon to hand an adopted session back. The
    // host settles one reclaim per attachment at a time, so a pane on a
    // shared window's carrier waits behind another pane's, and is asked
    // for once that one is answered. Returns whether the ask was sent or
    // is waiting its turn.
    [[nodiscard]] bool requestReclaim();
    void revokeConsent(uint8_t action, const std::string& attachmentID, const std::string& identity);
    /// Answers the exact operation kind retained with the host's question.
    /// - Parameters:
    ///   - requestID: The nonzero host-owned question.
    ///   - kind: The original question's operation, never inferred from the answer.
    ///   - decision: The validated ConsentChoice tag supplied by presentation.
    void respondToConsent(uint32_t requestID, Protocol::Sync::ConsentKind kind, uint8_t decision);
    /// Places this connection's host. Panes of the workspace inherit it.
    ///
    /// - Parameter location: Where the client's own carrier says the host runs.
    void setHostLocation(Protocol::Sync::HostLocation location);
    /// Where this connection's host runs.
    ///
    /// - Returns: The location ``setHostLocation`` gave, or another machine.
    [[nodiscard]] Protocol::Sync::HostLocation hostLocation() const { return m_hostLocation; }
    /// Sets where this endpoint reads the verdicts its own configuration
    /// gives the Remote origin, at each effect a host on another machine
    /// delivers, so a settings change reaches it without a reconnect.
    ///
    /// - Parameter source: Reads the client's current capability table.
    void setHostEffectDecisions(Protocol::Sync::SyncClientCore::HostEffectDecisionSource source);
    /// Applies or discards the held host effect on the person's answer.
    ///
    /// - Parameters:
    ///   - token: The token ``RemoteSessionObserver/onHostEffectHeld`` reported.
    ///   - choice: The person's answer.
    void answerHeldHostEffect(std::uint64_t token, Protocol::Sync::ConsentChoice choice) { m_core->answerHeldHostEffect(token, choice); }
    /// The held effect that has waited longest; see
    /// ``Protocol::Sync::SyncClientCore/oldestHeldHostEffect``.
    ///
    /// - Returns: The effect, or nothing when none waits.
    [[nodiscard]] std::optional<Protocol::Sync::SyncClientCore::HeldHostEffectQuestion> oldestHeldHostEffect() const { return m_core->oldestHeldHostEffect(); }

    /// Answers one exact tree operation without granting a role or standing
    /// permission. Uses the physical workspace connection for a pane endpoint.
    ///
    /// - Parameters:
    ///   - requestID: The host's question.
    ///   - allow: Whether the person approved this one operation.
    /// - Returns: Whether the answer was queued on an attached connection.
    [[nodiscard]] bool respondToStructureConsent(std::uint32_t requestID, bool allow); // bool-ok: maps the local presentation choice to canonical AllowOnce or Deny

    // Fetch a file from the host for a terminal click-to-open. Returns the
    // requestID to correlate with observer->onFileData() (multiple may be
    // in-flight); 0 if not sent (before HELLO_ACK).
    uint32_t requestFile(const std::string& path);

    /// One user-confirmed download, bound to the pane and transport incarnation.
    struct FileDownload {
        std::uint64_t token { 0 };
        std::uint64_t generation { 0 };
        std::uint64_t daemonInstanceID { 0 };
        std::uint32_t sessionID { 0 };
        std::string host;
        std::string path;
        /// The line the clicked link named, which the file opens at once it
        /// lands; 0 when it named none.
        FileLinkLine line;
    };
    /// Prepares a question without sending a frame. Tokens belong to the owning runtime.
    ///
    /// - Parameters:
    ///   - token: The runtime's id for the question.
    ///   - host: The connected host, as the question names it.
    ///   - path: The path on the host.
    ///   - line: The line the clicked link named, if any.
    /// - Returns: Whether the question is pending.
    [[nodiscard]] bool prepareFileDownload(std::uint64_t token, const std::string& host, const std::string& path, FileLinkLine line = {});
    /// Returns the current question only while its original attachment is live.
    [[nodiscard]] const FileDownload* pendingFileDownload() const;
    /// Consumes a matching answer once; cancellation and stale answers send nothing.
    [[nodiscard]] std::uint32_t answerFileDownload(std::uint64_t token, FileDownloadDecision);

    /// Asks the host what sessions it is holding, over this connection.
    ///
    /// The HELLO claims `Control`, which is the bit the daemon reads
    /// LIST_SESSIONS on, so an attached session asks over the carrier it
    /// already has rather than standing a second connection up beside it.
    ///
    /// One ask is outstanding at a time: a second supersedes the first, and the
    /// answer to a superseded one is dropped. The reply arrives as
    /// ``RemoteSessionObserver/onSessionList`` carrying the id returned here.
    ///
    /// - Returns: The ask's id, or 0 when nothing was sent — before HELLO_ACK
    ///   the frame would reach a daemon that has not agreed which session this
    ///   connection is.
    uint32_t requestSessionList();

    /// The mirror of the host's workspace: every surface, tab and pane, with
    /// the host's ids, as of the last structure adopted.
    ///
    /// - Returns: The mirror, or null before the first layout arrived.
    [[nodiscard]] const Layout::Workspace* workspace() const;

    /// Identifies the exact adopted topology on the current physical owner.
    /// - Returns: A monotonic generation, or zero without a current mirror.
    [[nodiscard]] std::uint64_t workspaceMirrorGeneration() const;

    /// Takes the pairing of the mirror's panes with those of the workspace a
    /// host restored in their place, for a frontend that keeps views by the
    /// ids the mirror held.
    ///
    /// A host that took in the record of a daemon that stopped restores the
    /// workspace under ids of its own, so a window, a tab and a pane that are
    /// the same ones to a person are named afresh. The connection carries
    /// each pane's endpoint to its restored session itself; a frontend moves
    /// what it keeps by window, tab or pane from the former ids to these, and
    /// lets go of whatever a pair does not name. Once taken the restoration
    /// is gone, so a frontend moves its keys once.
    ///
    /// - Returns: The restoration since the last take, from the ids a
    ///   frontend held then to the mirror's current ones.
    [[nodiscard]] Layout::Restoration takeRestoration();

    /// The complete validated structure behind the current physical mirror.
    /// Borrowed on the owner thread until its next structure or connection
    /// revision; null before live admission, including while reconnecting.
    [[nodiscard]] const TreeWorkspace* workspaceStructureSnapshot() const;

    /// The host session drawing into a pane of the mirror.
    ///
    /// - Parameter pane: A pane the mirror holds.
    /// - Returns: Its session's id, or 0 for a pane the mirror does not hold.
    [[nodiscard]] uint32_t sessionForPane(Layout::PaneID pane) const;

    /// The pane of the mirror a host session draws into.
    ///
    /// - Parameter sessionID: A host session's id.
    /// - Returns: Its pane, or `invalidPane` when no pane of the mirror is bound to it.
    [[nodiscard]] Layout::PaneID paneForSession(uint32_t sessionID) const;

    /// The host's id for the mirrored workspace, which is its root session's; 0 before the first layout.
    [[nodiscard]] uint32_t workspaceID() const;

    /// Sends a verb against the workspace this session belongs to.
    ///
    /// The host answers with ``RemoteSessionObserver/onWorkspaceVerbResult``
    /// carrying the id returned here and, when it performed the verb, with
    /// the new topology through ``RemoteSessionObserver/onWorkspaceChanged``.
    /// Ids in the verb are the mirror's, which are the host's.
    ///
    /// - Parameter verb: The verb; its `requestID` and `workspaceID` are set here.
    /// - Returns: The request's id, or 0 when nothing was sent — before
    ///   HELLO_ACK the host has not agreed which session this connection is.
    uint32_t sendWorkspaceVerb(Ubiquitty::Session::WorkspaceIntent verb);

    /// Publishes a complete local window on its already attached daemon.
    /// A retry uses the first adopted session as its stable correlation.
    [[nodiscard]] bool publishSharedWorkspace(const SharedWorkspace& workspace);
    /// The publication outcome; a pending or lost answer is never success.
    [[nodiscard]] const SharedWorkspaceResult& sharedWorkspaceResult() const { return screenOwner().m_sharedWorkspaceResult; }

    /// Captures this pane's whole host surface without sending a request.
    [[nodiscard]] bool prepareWindowClose(std::uint64_t token);
    /// Returns zero after the attachment or observed workspace topology changes.
    [[nodiscard]] std::uint64_t pendingWindowCloseToken() const;
    /// Counts the exact sessions in the captured surface, including hidden tabs.
    [[nodiscard]] std::uint32_t pendingWindowCloseSessionCount() const;
    /// Consumes the matching answer once; a current approval calls window.close
    /// against the exact structure revision shown by the question.
    [[nodiscard]] std::uint32_t answerWindowClose(std::uint64_t token, RemoteWindowCloseDecision);

    /// Asks the host to bring a tab to the front of its window, for a person
    /// who chose it here.
    ///
    /// Which tab a window shows is the host's to say, and every client of the
    /// workspace shows the one it names. The ask is answered by a structure
    /// naming the tab active; until that arrives ``frontTab`` answers the tab
    /// asked for, so a structure sent before the host read the ask does not
    /// put the earlier tab back in front for a round trip.
    ///
    /// - Parameters:
    ///   - surface: The window of the mirror the tab stands in.
    ///   - tab: The tab the person chose.
    /// - Returns: Whether an ask went out: false when the tab is already the
    ///   one in front or already asked for, before the first structure, and
    ///   when nothing can be sent.
    bool bringTabToFront(Layout::SurfaceID surface, Layout::TabID tab);

    /// The tab in front on a window of the mirror.
    ///
    /// - Parameter surface: A window of the mirror.
    /// - Returns: The tab last asked for there while the host has yet to
    ///   answer, else the mirror's active tab; `invalidTab` before the first
    ///   structure and for a window the mirror does not hold.
    [[nodiscard]] Layout::TabID frontTab(Layout::SurfaceID surface) const;

    /// Pushes a pasted payload for the host to land as a file and type its
    /// path into the foreground program.
    ///
    /// - Parameters:
    ///   - mime: The payload's MIME name, e.g. "image/png".
    ///   - bytes: The payload, copied into the frame before returning.
    /// - Returns: The push id to correlate with the observer's ack, or 0
    ///   when nothing was sent — before HELLO_ACK, or past the frame cap.
    uint32_t pushPaste(const std::string& mime, std::span<const std::uint8_t> bytes);

    /// Answers a clipboard request from the client's own pasteboard.
    ///
    /// - Parameters:
    ///   - requestID: The request being answered, as ``onClipboardRequest``
    ///     reported it. An id nothing asked for is still sent: the host drops a
    ///     reply it has no request for, and a second rule here would let the
    ///     two ends disagree about which replies exist.
    ///   - status: Why the answer carries what it does.
    ///   - items: The types, or the one item's bytes; ignored unless `status`
    ///     is OK. Taken by value and moved onward — a pasteboard image runs to
    ///     megabytes and the caller has no use for it afterwards.
    void serveClipboard(std::uint32_t requestID, Protocol::Sync::ClipboardServeStatus status,
        std::vector<Protocol::Sync::ClipboardServeItem> items);

    /// How much of the last pushed paste has left this machine.
    ///
    /// Read from the channel's outbound queue: what is still queued has not
    /// gone. Progress of the transfer, not of the host's work — the ack is
    /// what says the landing finished.
    ///
    /// - Returns: 0 to 1, or -1 when no push is in flight or the channel
    ///   cannot say what it still holds.
    [[nodiscard]] double pastePushProgress() const;

    /// The last refusal the host answered a handshake with.
    ///
    /// - Returns: The refusal, with ``HostRefusal::hasRefusal`` clear when the
    ///   host has refused nothing.
    [[nodiscard]] const HostRefusal& lastRefusal() const { return m_lastRefusal; }

    /// Installs this app's current host before anything is dialled.
    ///
    /// Called before ``start``: the start that follows goes to Deploying
    /// instead of connecting, and the observer hears
    /// ``RemoteSessionObserver::onDeployRequested`` on the next dispatcher turn,
    /// once the embedder has armed its install. Nothing reaches the host until
    /// ``deployFinished`` answers — no bootstrap, so no daemon start by the host
    /// being replaced — and a failed install leaves the core Failed with
    /// nothing run there at all. A successful one dials the new host.
    ///
    /// Whether the update is offered at all is ``decideUpdateAction``'s; this
    /// carries it out.
    ///
    /// - Parameter platformArch: The host's `uname -sm`, empty when the refusal
    ///   that prompted the update did not name it (the install probes it).
    /// - Returns: Whether the update will run; false for a core that has
    ///   already started.
    bool updateHostBeforeConnect(std::string platformArch);

    /// Which carrier this connection rides, and why QUIC is not it.
    ///
    /// - Returns: The fact.
    [[nodiscard]] CarrierFact carrierFact() const;

    /// Sets the access token every later HELLO presents.
    ///
    /// The token decides whether the HELLO claims `Auth` at all, as
    /// ``SessionDirectoryCore::setAuthToken`` does: a claim with no token
    /// behind it is one the host cannot verify.
    ///
    /// - Parameter token: The token, or empty to present none.
    void setAuthToken(std::string token) { m_authToken = std::move(token); }

    // The platform finished an onDeployRequested() deploy. Succeeded →
    // reconnect (the bootstrap now finds the freshly-installed binary);
    // Failed → State::Failed with `error` as the detail. No-op unless a deploy
    // is pending. Session thread only, like every public method.
    void deployFinished(DeployResult result, const std::string& error);

    /// The device moved to a different network.
    ///
    /// The platform calls this when the OS reports a path change — a phone leaving WiFi for
    /// cellular, a laptop waking on a different network. What it fixes is a session waiting out a
    /// backoff earned on an old path. One retry may run immediately, but failures stay counted
    /// until a Welcome: repeated notifications cannot renew the fast retry window.
    ///
    /// While CONNECTED the answer depends on whether the carrier survives the move. A QUIC
    /// connection migrates — `ChannelObserver::onConnect` fires once per logical connection, not per
    /// network change — so it is kept, and the host is asked for an answer on the new path; one that
    /// stays silent past a short deadline is replaced rather than left to its idle timer. A stream
    /// connection survives a move that leaves its own route alone, so it is asked the same way. An
    /// attempt still connecting may be bound to an address this device no longer has. An
    /// interrupted resume proof falls back to the bootstrap; a bootstrap gets one replacement,
    /// after which its completion and the retry schedule own progress until a Welcome.
    ///
    /// Idempotent and cheap, so a platform that cannot tell a real move from a spurious
    /// notification may call it on every one. Session thread only, like every public method.
    void networkPathChanged();

    /// Sets the view this core's frontend holds of its pane: `Live` while it
    /// is on screen, `Dormant` while it is not, `RecordsOnly` for an owner
    /// that reads no screen (Docs/sync.md §6.8). The host then sends a
    /// `Dormant` pane no screen, one `PaneDirty` when it changes, and waking it
    /// costs only its changed rows. A pane on a shared workspace connection
    /// sets its view there. Kept for a pane not yet bound. Session thread
    /// only, like every public method.
    ///
    /// - Parameter state: The view.
    void setView(Protocol::Sync::ViewState state) override;

    /// Tells the host whether this core's surface has the keyboard. The host
    /// probes an idle connection only while its client is focused
    /// (Docs/sync.md §6.4); a workspace connection is focused while any of
    /// its panes is. Session thread only, like every public method.
    ///
    /// - Parameter focus: Whether the surface is focused.
    void setSurfaceFocus(Ubiquitty::Modes::FocusState focus);

    /// The stream bytes the host sent this core's pane, on its own connection
    /// or the workspace's: what a test reads to see a hidden pane cost
    /// nothing.
    ///
    /// - Returns: The bytes.
    [[nodiscard]] std::uint64_t paneBytesReceived() const;

    /// Reattaches after the platform suspended transport processing.
    ///
    /// A connected carrier may have expired at the peer while the process was
    /// suspended. One that may have survived — a stream connection, or a QUIC
    /// one inside the host's idle window — is asked to prove it carries frames
    /// and kept if it answers. Otherwise recovery follows the same bounded
    /// lifecycle retry as ``networkPathChanged``, preserving session and workspace
    /// identities. Stopped and failed sessions stay closed.
    ///
    /// - Parameter secondsSuspended: How long the platform held the process, as
    ///   the frontend measured it; negative when it cannot say.
    void resumeAfterSuspension(double secondsSuspended);

    /// Dials a reconnecting session at once instead of waiting out its backoff.
    ///
    /// - Returns: Whether a dial began; false for any state but Reconnecting.
    bool reconnectNow();

    /// Tells the host this client is about to be suspended, and lets the
    /// carrier go: an app at its background deadline, a machine about to
    /// sleep. Each pane goes `Dormant` at the host, which holds the
    /// attachment `Lost` at once and stops sending to a client that cannot
    /// read; the size this client owned stays its own. The session waits in
    /// `Reconnecting` with no timer running, and `resumeAfterSuspension`
    /// continues it. Nothing for a session that is not connected.
    void suspend();

    /// Sets the instance this client greets the host as, before `start`. A
    /// frontend keeps it with its window's restoration identity, so a
    /// relaunched window is the same client to the host.
    ///
    /// - Parameter clientInstance: The instance.
    void setClientInstance(Protocol::Sync::ID128 clientInstance) { m_core->setClientInstance(clientInstance); }

    /// The instance this client greets the host as: the one set, or one
    /// drawn at random when the core was made.
    [[nodiscard]] Protocol::Sync::ID128 clientInstance() const { return m_core->clientInstance(); }

    /// Whether the host's Welcome names a macOS keyboard.
    [[nodiscard]] bool hasMacOSHost() const { return isMacOSHostPlatform(m_core->hostPlatform()); }

    /// Whether the current authenticated host assigns this pane's geometry to
    /// another attachment. Unknown or disconnected ownership answers false.
    /// - Returns: Whether local non-owner presentation may be used.
    [[nodiscard]] bool isGeometryOwnedElsewhere() const;

    /// The link's indicators as they stand; neither shows while no
    /// connection is attached.
    [[nodiscard]] Protocol::Sync::LinkState linkState() const;

    /// Replaces the clock the screen plane reads: its stall bound, its
    /// probes and its link indicators are timed by it.
    ///
    /// - Parameter clock: Returns microseconds on a monotonic clock.
    void setClock(Protocol::Sync::SyncClientCore::Clock clock) { m_core->setClock(std::move(clock)); }

    /// Enables workspace multiplexing before the physical connection starts.
    void enableWorkspaceMultiplexing() { m_isMultiplexedWorkspace = true; }
    /// Keeps an adopted terminal's session as the attach target when sharing
    /// moves it between workspaces on its pinned daemon.
    void followAdoptedSessionWorkspace() { m_shouldFollowAdoptedSessionWorkspace = true; }
    /// Selects a stable workspace for the initial multiplexed handshake.
    void setAttachWorkspaceID(uint32_t identity) { m_requestedWorkspaceID = identity; }
    /// Pins a saved reference to one daemon process before its first handshake.
    void setExpectedDaemonInstanceID(std::uint64_t identity) { m_daemonInstanceID = identity; }
    /// Returns the daemon process identity pinned by this connection.
    [[nodiscard]] std::uint64_t daemonInstanceID() const { return m_daemonInstanceID; }
    /// The predecessor named by an admitted Workspace restore, or zero otherwise.
    [[nodiscard]] std::uint64_t restoredFromDaemonInstanceID() const { return m_core->restoredFromDaemonInstanceID(); }
    /// Returns bytes waiting in the physical transport's bounded queue.
    std::int64_t pendingOutboundBytes() const { return m_channel ? m_channel->pendingOutboundBytes() : 0; }
    /// Returns the physical transport's queue capacity, or -1 when unavailable.
    std::int64_t outboundCapacityBytes() const { return m_channel ? m_channel->outboundCapacityBytes() : -1; }
    /// Returns the pane/session bindings owned by the authoritative workspace mirror.
    const std::vector<std::pair<uint32_t, uint32_t>>& workspacePaneSessions() const { return m_paneSessions; }
    State state() const { return m_state; }
    uint32_t sessionID() const { return m_sessionID; }
    bool isUsingQUIC() const;
    bool acknowledged() const { return m_ackSeen; }
    /// Whether the host has welcomed this pane's screen plane.
    [[nodiscard]] bool hasHostScreen() const { return m_ackSeen && screenOwner().m_core->isScreenAttached(); }
    // Last foregroundIsShell the host reported via Status (defaults to shell). The
    // resize path gates the client-side scrollback reflow on this so the mirror
    // makes the same reflow decision the host does (geometry-only under less -X).
    bool foregroundIsShell() const { return m_foregroundIsShell; }
    // Basename of the daemon's listening socket from HELLO_ACK (empty until
    // then / against a pre-coexistence host). The frontend pins it to reattach
    // this daemon after a newer binary's daemon binds the default socket.
    const std::string& socketName() const;

    // Live traffic / protocol / performance instrument for this session, fed
    // from the inbound decode path and the outbound writes. Read on the session
    // thread (a frontend polls it for the Sync Debug window). resetTrace()
    // zeroes the counters + rings.
    const Protocol::Sync::SyncTrace& trace() const { return m_trace; }
    void resetTrace() { m_trace.reset(); }

    // Live transport statistics from the current carrier (only QUIC reports
    // any). Returns false when there's no channel or it has none. Session
    // thread only (same thread that creates/tears down the channel).
    bool queryTransportStats(Ubiquitty::Transport::TransportStats& out) const;

private:
    friend struct RemoteSessionDatagramIngressAccess;
    friend struct RemoteSessionWindowCloseAccess;
    friend struct RemoteSessionCarrierPictureAccess;
    friend struct RemoteSessionWorkspacePaneAccess;
    friend class WorkspaceConnection;
    friend struct WorkspaceConnectionStorage;
    // The workspace retains the physical client through callbacks that release its final frontend.
    std::weak_ptr<WorkspaceConnectionStorage> m_dispatchLifetime;
    void establishWorkspacePane(uint32_t);
    /// Takes a checked record from the shared workspace carrier.
    void receiveWorkspacePaneFacts(const Protocol::Sync::PaneFacts& facts);
    /// Delivers an admitted effect from the retained physical workspace reader.
    void receiveWorkspaceEffect(const Protocol::Sync::EffectView&, const Protocol::Sync::EffectBody&, Protocol::Sync::EffectDisposition);
    /// Offers the pane's typed read question to its existing local-policy adapter.
    [[nodiscard]] bool receiveWorkspaceClipboardRequest(const Protocol::Sync::ClipboardRequestMessage&);
    void workspaceConnectionChanged(State, const std::string&);
    WorkspaceConnection* m_workspaceConnection { nullptr }; // Borrowed; the runtime retains its shared owner.

    // A datagram is a small, latency-first transport. If the
    // session thread cannot keep up, retaining more seconds of obsolete screen
    // state only turns scheduler delay into process growth. Each of the pending
    // and draining batches has these payload and whole-frame limits; with at
    // most two batches, the aggregate retained limits are twice these values.
    static constexpr std::size_t datagramIngressBatchMaximumBytes = 128u * 1024u;
    static constexpr std::size_t datagramIngressBatchMaximumFrames = 256;
    static constexpr std::size_t datagramIngressMaximumRetainedBytes = datagramIngressBatchMaximumBytes * 2;
    static constexpr std::size_t datagramIngressMaximumRetainedFrames = datagramIngressBatchMaximumFrames * 2;

    struct DatagramIngressBatch {
        std::array<std::uint8_t, datagramIngressBatchMaximumBytes> bytes;
        std::array<std::size_t, datagramIngressBatchMaximumFrames> lengths;
        std::size_t byteCount = 0;
        std::size_t frameCount = 0;

        void clear()
        {
            byteCount = 0;
            frameCount = 0;
        }
    };

    // Ubiquitty::Transport::ChannelObserver — fired on the Channel's I/O thread; each
    // marshals onto the session thread via the Dispatcher (generation-guarded
    // so a superseded attempt's late callback is dropped).
    void onConnect() override;
    /// Copies the inbound bytes and posts them to the session thread.
    ///
    /// The post carries the generation current when the bytes arrived, so a
    /// slice that was already in flight when the connection was superseded is
    /// dropped on arrival rather than fed to the successor's decoder.
    ///
    /// - Parameter data: Valid only for the duration of the call.
    void onData(std::span<const std::uint8_t> data) override;
    /// Copies one datagram-borne frame and posts it to the session thread.
    ///
    /// Generation-stamped like `onData`, and separate from it because the
    /// datagram path skips the stream decoder: a whole frame arrives, so
    /// nothing has to be reassembled across calls.
    ///
    /// - Parameter data: Valid only for the duration of the call.
    void onDatagram(std::span<const std::uint8_t> data) override;
    void onClose(const std::string& reason) override;

    // Protocol::Sync::SyncClientObserver — fired synchronously inside feed(), already on
    // the session thread.
    /// Records a refused handshake, and whether retrying it is futile.
    ///
    /// Runs inside `feed()`, with the channel still up: the daemon drops the
    /// connection next, and this is what tells that drop it is terminal rather
    /// than a transport blip.
    ///
    /// - Parameter reason: The wire reason the host refused with.
    void onRefused(Protocol::Sync::ByeReason reason) override;
    void onFrameDecoded(uint8_t type, std::size_t wireBytes, std::size_t logicalBytes) override;
    void onHelloAck(uint32_t sessionID, const std::string& socketName) override;
    Ubiquitty::Terminal* screenTerminal(uint32_t sessionID) override;
    /// Settles an outstanding resize ask against the grid the host sent, and
    /// marks the screen for the coalesced repaint.
    ///
    /// - Parameter sessionID: The session whose screen changed.
    void onScreenChanged(uint32_t sessionID) override;
    void onPaneFacts(uint32_t sessionID, const Protocol::Sync::PaneFacts& facts) override;

    /// Marks the next frame as carrying inline images.
    ///
    /// Raised through the same coalescing flag a screen change uses, so a feed
    /// carrying several image frames repaints once. An image arrives with no
    /// cell change behind it, and a client repainting on grid movement alone
    /// holds the frame it has until something unrelated moves.
    void onImagesChanged() override;
    void onCommandDone(const Protocol::Sync::EffectView&, const Protocol::Sync::CommandFinishedEffect&, Protocol::Sync::EffectDisposition) override;
    /// Starts one bounded typed directory request on the physical connection.
    /// - Parameter requestID: The reserved local request identity.
    /// - Returns: Whether the request was admitted on the surviving connection.
    bool beginSessionList(std::uint32_t requestID);
    /// Refreshes a pending directory and finishes only a complete snapshot.
    void serviceSessionList();
    /// Whether the complete promised directory arrived before its deadline.
    enum class DirectoryCompletion : std::uint8_t {
        Complete,
        Failed
    };
    /// Retires the subscription before notifying its requester.
    /// - Parameter completion: Whether all promised records arrived.
    void finishSessionList(DirectoryCompletion completion);
    /// Rebuilds the workspace mirror from the structure the host sent of the
    /// workspace this session is attached to, and tells the observer when
    /// what the mirror shows changed.
    ///
    /// - Parameter workspaceID: The workspace whose structure changed.
    void onWorkspaceStructure(std::uint32_t workspaceID) override;
    void onPaneEnded(std::uint32_t sessionID) override;
    /// A workspace carrier hands the failure to the pane's own core; a
    /// connection of one session fails with the reason, which its frontend
    /// shows as the session's failure, until the host binds the pane again.
    void onPaneFailed(std::uint32_t sessionID, std::string_view reason) override;
    void onPaneEffect(uint32_t, const Protocol::Sync::EffectView&, const Protocol::Sync::EffectBody&, Protocol::Sync::EffectDisposition) override;
    bool onPaneClipboardRequest(uint32_t, const Protocol::Sync::ClipboardRequestMessage&) override;
    void onPaneNativeDragCommand(std::uint32_t, const Protocol::Sync::NativeDragCommand&) override;
    void onPaneBound(uint32_t sessionID) override;
    /// Applies the matching answer to this connection's clipboard routing request.
    void receiveClipboardServingAnswer(const std::string&);
    void onClipboardRequest(const Protocol::Sync::ClipboardRequestMessage&) override;
    [[nodiscard]] bool supportsNativeDrag() const override { return m_observer && m_observer->supportsNativeDrag(); }
    void onNativeDragCommand(const Protocol::Sync::NativeDragCommand&) override;
    void onClipboardWriteItems(ClipboardBuffer, std::span<const ClipboardItem>) override;
    void onNotificationRequest(const NotificationRequest&) override;
    void onAttention() override;
    void onHostEffectHeld(std::uint64_t token, Capability capability) override;
    void onHostEffectWithdrawn(std::uint64_t token, Protocol::Sync::HostEffectWithdrawal reason) override;
    /// Where the host runs and what the client's own policy says of it,
    /// applied to every core this endpoint builds or adopts.
    Protocol::Sync::HostLocation m_hostLocation = Protocol::Sync::HostLocation::AnotherMachine;
    Protocol::Sync::SyncClientCore::HostEffectDecisionSource m_hostEffectDecisions;

    // What the frontend asked for, and what the wire actually agreed. Two
    // fields because they answer different questions: a setting turned on
    // mid-session does not reach a HELLO already sent, and a badge that read
    // the first would describe a session that serves nothing.
    Clipboard::Serving m_clipboardServing = Clipboard::Serving::Disabled;
    bool m_didClaimClipboardServe = false;
    std::optional<std::uint64_t> m_clipboardServingCallID;

    // The workspace every HELLO on this connection names, and how a session
    // created under it ends; see setWorkspace. Held rather than passed through
    // start, because a reconnect builds its own HELLO with no caller present to
    // supply them again.
    std::string m_daemonSocketName;
    std::string m_workspaceName;
    bool m_endsWhenDetached = false; // bool-ok: mirrors the single HELLO flag bit helloSessionEndWhenDetached; an enum here would be a second spelling of one wire bit
    ReattachPolicy m_reattachPolicy = ReattachPolicy::AttachOrCreate;
    /// Whether the workspace's structure says the host restored it after a
    /// restart.
    bool m_isWorkspaceRestoredAfterHostRestart = false; // bool-ok: names the one state it reports
    /// What the mirror shows, as the structure it was last adopted from says
    /// it (`Layout::mirrorSignature`); empty until a structure is adopted on
    /// the connection in hand.
    std::string m_mirrorSignature;
    /// Monotonic identity of an adopted mirror; saturates rather than reusing an identity.
    std::uint64_t m_mirrorGeneration { 0 };
    /// Whether a host acknowledged this connection with the workspace it
    /// restored in place of the one the mirror holds, and the mirror has yet
    /// to adopt its structure.
    bool m_isAwaitingRestoredMirror = false; // bool-ok: names the one state it reports
    /// The pairs of the last restored structure adopted, from the session the
    /// mirror held to the one restored in its place, until the workspace
    /// connection carries its endpoints to them; nothing for an adoption that
    /// restored nothing.
    std::optional<std::vector<std::pair<std::uint32_t, std::uint32_t>>> m_carriedSessions;
    /// The restoration no frontend has taken; see ``takeRestoration``.
    Layout::Restoration m_restoration;

    void onBell() override;
    void onExit() override;

    // Session-thread handlers + helpers.
    void connect();
    void handleChannelReady(std::unique_ptr<Ubiquitty::Transport::Channel>, bool isQUIC, std::optional<ChannelCreationFailure>);
    /// What a carrier presents to continue this core's attachment, or
    /// `nullopt` when it holds none.
    [[nodiscard]] std::optional<ResumeKey> resumeKey() const;
    /// Whether the host answered a resume dial before it was abandoned.
    enum class ResumeDialFailure : std::uint8_t {
        Unanswered,
        HostRefused,
    };
    /// Gives up a proof dial and bootstraps. A host refusal leaves QUIC
    /// available; a carrier that never answered keeps this session on SSH.
    void abandonResumeDial(ResumeDialFailure = ResumeDialFailure::Unanswered);
    /// Latches the stream carrier after an unanswered QUIC handshake, and
    /// keeps why for ``carrierFact``.
    void latchQUICHandshakeFailure();
    /// Ends a proof of life that was answered, and sends the input held
    /// through it.
    void noteProofOfLife();
    /// Tells the observer the link's indicators when they changed.
    void reportLinkState();
    void handleConnect(std::uint64_t generation);
    /// Feeds one drain's worth of stream bytes to the decoder.
    ///
    /// Takes the staging buffer by reference rather than by value: the caller
    /// owns it across drains so a steady stream reuses one allocation.
    ///
    /// - Parameters:
    ///   - generation: The attempt the bytes arrived on; bytes from a
    ///     superseded attempt are dropped rather than fed.
    ///   - bytes: The accumulated bytes, in arrival order.
    void handleData(std::uint64_t generation, std::vector<std::uint8_t>& bytes);
    /// Feeds one datagram to the screen plane.
    ///
    /// A datagram is one Sync envelope, a superset update or a probe. The
    /// caller owns the observer notification, so a burst reports once.
    ///
    /// - Parameters:
    ///   - generation: The attempt the datagram arrived on.
    ///   - bytes: One envelope, as it came off the wire.
    void handleDatagram(std::uint64_t generation, std::span<const std::uint8_t> bytes);
    /// Stages one datagram while `m_inboundMutex` is held.
    ///
    /// Every datagram is replaceable: a superset update applies on any
    /// generation since its base, and the host resends what no `Ack` names,
    /// so one that does not fit the budget is dropped rather than retained.
    ///
    /// - Parameter bytes: One datagram.
    /// - Returns: Whether it was staged.
    [[nodiscard]] bool stageDatagramLocked(std::span<const std::uint8_t> bytes);
    /// Points the staging buffers at an attempt, discarding another's bytes.
    ///
    /// Called by the I/O thread with `m_inboundMutex` held. Bytes staged for a
    /// superseded attempt are the middle of a stream the next attempt's decoder
    /// never saw the start of, so they are dropped rather than prepended to it.
    ///
    /// - Parameter generation: The attempt the arriving bytes belong to.
    void stageGenerationLocked(std::uint64_t generation);

    /// Feeds everything the stream staged since the last drain.
    ///
    /// One of these is posted per burst rather than per chunk: the I/O thread
    /// appends and posts only when no drain is already queued, so a flood costs
    /// one cross-thread hop however many chunks it arrives in.
    ///
    /// - Parameter generation: The attempt this drain was posted for.
    void drainInbound(std::uint64_t generation);

    /// Feeds every datagram staged since the last drain, reporting once.
    ///
    /// Each datagram reaches the decoder whole, since a datagram is a frame;
    /// only the cross-thread hop and the observer notification are shared
    /// across the burst.
    ///
    /// - Parameter generation: The attempt this drain was posted for.
    void drainDatagrams(std::uint64_t generation);
    /// Tears the channel down after it closed, and decides what follows.
    ///
    /// - Parameters:
    ///   - generation: The connection attempt that closed.
    ///   - reason: What the person is shown.
    ///   - reconnectDelaySeconds: The wait before redialling, when the close
    ///     itself sets one; otherwise the ordinary backoff.
    void handleClose(std::uint64_t generation, std::string reason, std::optional<double> reconnectDelaySeconds = std::nullopt);
    /// Returns the wait before redialling after the host closed for a reason.
    ///
    /// A drop for falling behind the host's output counts one more in a row and
    /// waits past the ordinary fast schedule; any other reason sets no wait.
    ///
    /// - Parameter reason: Why the host closed.
    /// - Returns: The wait, or nothing for the ordinary backoff.
    std::optional<double> reconnectDelayAfterHostClose(Protocol::Sync::ByeReason reason);
    void scheduleReconnect(const std::string& reason, std::optional<double> delaySeconds = std::nullopt);
    void transition(State, const std::string& detail);
    void teardownChannel();
    /// Asks a connection kept across a suspension or a path change to prove it
    /// carries frames, with a screen-plane `Probe`, and arms the deadline that
    /// replaces it if the probe's own `Echo` does not arrive.
    ///
    /// - Parameter reason: What the replacement reports, should one happen.
    /// - Returns: False when the connection cannot be asked.
    bool askForProofOfLife(std::string reason);
    /// The core whose connection carries this pane's screen plane and input:
    /// the workspace's carrier for a pane that rides one, this core otherwise.
    [[nodiscard]] RemoteSessionCore& screenOwner();
    /// The same, for a reader.
    [[nodiscard]] const RemoteSessionCore& screenOwner() const;
    /// Tells the host the focus the frontend last reported for this pane,
    /// once the host has named the session and if the frontend has said.
    void reportPaneFocus();
    /// Tells the observer how many of the pane's events are in doubt or
    /// discarded when either count moved.
    void reportInputStatus();
    /// Claims one platform report for this dispatcher turn.
    ///
    /// A frontend tells every window about a wake or a path move, and the
    /// windows of one workspace reach this one connection through their
    /// panes, so the same fact arrives once per window in a single turn.
    /// Acting on each repeat replaced the carrier the first report had just
    /// started dialling, once per window, each an SSH login on port 22.
    ///
    /// - Parameter isReported: The flag for this kind of report, cleared
    ///   when the turn ends.
    /// - Returns: Whether this is the turn's first report of its kind.
    bool takeLifecycleReport(bool& isReported);
    /// Retires an old-path attempt once without renewing its retry budget.
    ///
    /// An interrupted proof falls back to the bootstrap. A successful Welcome
    /// permits the next lifecycle retry.
    /// - Parameter reason: The state detail reported when a carrier is replaced.
    void retryAfterLifecycleChange(const std::string& reason);
    /// Replaces the carrier at once, preserving the count of failed attempts.
    void replaceChannel(const std::string& reason);
    /// States the grid this client holds for its pane, with the pixels its
    /// frontend published, once the host has acknowledged the session and the
    /// frontend has laid the pane out.
    void stateViewport();

    /// Tells the host the size this session's pane is shown at, with the
    /// pixels the frontend last published: the pane's `Viewport`, and for a
    /// session alone on its carrier the window's size in `Facts` as well.
    ///
    /// - Parameters:
    ///   - columns: The grid's columns.
    ///   - rows: The grid's rows.
    void sendViewport(int columns, int rows);
    /// Ends the session with `reason` as its one reported failure.
    ///
    /// - Parameter reason: What the observer is told ended the session.
    void stopWithReason(const std::string& reason);
    /// Refuses the host that just acknowledged and ends the session.
    ///
    /// Called from `onHelloAck`, inside the feed that delivered it: the rest of
    /// that read is not decoded, since it comes from the host being refused.
    ///
    /// - Parameter reason: What the observer is told ended the session.
    void refuseAcknowledgedHost(const std::string& reason);
    void sendHello();
    bool writeFrame(const std::vector<uint8_t>& frame);

    /// Records each frame of a write in the trace, by its own type.
    ///
    /// - Parameter frames: Whole Sync frames, back to back.
    void traceOutbound(std::span<const uint8_t> frames);
    /// Writes what the screen plane has to say, and arms the timer for what
    /// it will owe next.
    void flushScreen();
    /// Projects a strictly decoded canonical roster only onto currently bound
    /// exact pane identities. False means a callback replaced this carrier.
    [[nodiscard]] bool receiveAttachments(const Protocol::Sync::AttachmentsList&);

    /// Completes only the exact pending file call, retiring its owner before callbacks.
    /// - Parameter answer: One complete JSON-RPC result from this physical connection.
    void receiveFileAnswer(std::string_view answer);
    /// Matches only this physical connection's exact paste credit/final answer.
    void receivePasteReady(std::span<const std::uint8_t> record);
    void receivePasteAnswer(std::string_view answer);
    /// Retires a pending reclaim before its exact endpoint may resume locally.
    bool consumeControlResult(std::string_view answer) override;
    /// Sends the oldest waiting reclaim once none is out, dropping any whose
    /// endpoint or connection changed since it asked.
    void serviceReclaims();
    /// Queues at most one raw chunk after interactive output and arms its next pass.
    void servicePasteSource();

    /// Sets the focus this core's screen plane reports, and writes the change.
    ///
    /// - Parameter focus: Whether the connection's client is focused.
    void applyScreenFocus(Ubiquitty::Modes::FocusState focus);
    void finishReclaim();
    // Pre-HELLO_ACK, sniff the inbound bytes for the deploy sentinel the
    // bootstrap prints when the host has no daemon binary, and for the line a
    // pinned bootstrap prints when its daemon is gone. Returns true if the
    // bytes were consumed by the sniffer (caller must not feed() them):
    // either still buffering a partial sentinel line, or the deploy or the
    // re-resolution was started. False once the stream is known not to be a
    // sentinel.
    bool sniffDeploySentinel(std::vector<std::uint8_t>& bytes);
    /// Answers a bootstrap that reported its pinned daemon gone.
    ///
    /// The pin goes and the host's current daemon is dialled at once, asked
    /// for the workspace by the identity this client holds: the daemon
    /// instance and workspace it last held. A host that took in the stopped
    /// daemon's records restores the workspace from them, and one that did
    /// not refuses it. Once per pin: a second report before a daemon has
    /// answered means the plan names a daemon this core cannot unpin, and the
    /// session fails rather than dialling it again.
    void reresolveGoneInstance();

    Ubiquitty::Terminal* m_term;
    PaneLinkEvents m_linkEvents;
    std::optional<FrameReady> m_restoredPreview;
    std::optional<std::uint32_t> m_linkFactsRevision;
    Dispatcher* m_dispatcher;
    ChannelFactory* m_factory;
    RemoteSessionObserver* m_observer;
    std::unique_ptr<Protocol::Sync::SyncClientCore> m_core;
    std::unique_ptr<Ubiquitty::Transport::Channel> m_channel;
    Protocol::Sync::SyncTrace m_trace; // live traffic/protocol instrument (session-thread)

    State m_state = State::Idle;
    uint32_t m_sessionID = 0;
    // The grid this client's drawable holds for the pane, as its frontend
    // last reported it; the launch grid until it reports one. The host's own
    // grid for the pane is the mirror terminal's.
    int m_columns = 80;
    int m_rows = 24;
    // Whether the frontend has reported a grid: until it has, or has
    // published its pixel metrics, the grid above is the launch grid,
    // which no drawable holds.
    bool m_isSizeReported = false;
    std::optional<std::array<std::uint16_t, 6>> m_pixelGeometry;
    bool m_ackSeen = false;
    std::optional<FileDownload> m_pendingFileDownload;
    struct PendingWindowClose {
        std::uint64_t token { 0 };
        std::uint64_t generation { 0 };
        std::uint64_t mirrorGeneration { 0 };
        std::uint64_t structureRevision { 0 };
        std::uint64_t daemonInstanceID { 0 };
        std::uint32_t workspaceID { 0 };
        std::uint32_t sessionID { 0 };
        Layout::SurfaceID surface { Layout::invalidSurface };
        std::vector<std::pair<std::uint32_t, std::uint32_t>> paneSessions;
    };
    /// Validates the physical attachment and the complete captured affected set.
    [[nodiscard]] const PendingWindowClose* pendingWindowClose() const;
    std::optional<PendingWindowClose> m_pendingWindowClose;
    /// One physical connection's typed tree calls; no endpoint owns another's reply.
    struct StructureCall {
        std::uint32_t requestID = 0;
        Ubiquitty::Session::WorkspaceIntentKind verb = Ubiquitty::Session::WorkspaceIntentKind::CloseSurface;
        std::uint32_t targetID = 0;
        std::string expectedText;
        std::uint32_t destinationID = 0;
        std::uint32_t destinationIndex = 0;
        std::uint32_t sourceWindowID = 0;
        std::uint32_t sourceTabID = 0;
        std::uint32_t destinationPaneID = 0;
        std::uint32_t destinationTabID = 0;
        double requestedRatio = 0;
    };
    std::vector<StructureCall> m_structureCalls;
    std::uint64_t m_workspaceStructureRevision = 0;
    std::uint64_t m_nextConsentRevocationID = 0;
    /// Queues a revision-bound close on the physical screen plane.
    /// - Parameters:
    ///   - requestID: A fresh physical request identity, already routed to its endpoint.
    ///   - surface: The captured host window.
    ///   - revision: The captured workspace structure revision.
    /// - Returns: Whether the call was admitted on the current carrier.
    [[nodiscard]] bool sendWindowCloseCall(std::uint32_t requestID, Layout::SurfaceID surface, std::uint64_t revision);
    /// Queues a typed tree operation as a JSON-RPC call on the physical carrier.
    /// - Parameters:
    ///   - verb: The operation and a fresh, physically routed request identity.
    ///   - revision: The confirmed revision for window close, zero otherwise.
    /// - Returns: Whether the call was admitted on the current carrier.
    [[nodiscard]] bool sendStructureCall(const Ubiquitty::Session::WorkspaceIntent& verb, std::uint64_t revision = 0);
    /// Consumes only matching, fully validated typed tree answers before notifying.
    /// - Parameter response: One bounded JSON-RPC answer from the physical carrier.
    void receiveStructureAnswer(const std::string& response);
    uint32_t m_requestedWorkspaceID { 0 };
    std::uint64_t m_daemonInstanceID { 0 };
    bool m_isMultiplexedWorkspace = false;
    bool m_shouldFollowAdoptedSessionWorkspace = false;
    // The view the frontend set, applied once the host names the session.
    std::optional<Protocol::Sync::ViewState> m_view;
    /// What the reader's scrolling asked of the pane's history; a reconnect
    /// forgets it, since the host's wants are a connection's.
    Protocol::Sync::HistoryPull m_historyPull;
    // Whether the frontend's surface has the keyboard, once it has said; a
    // workspace reads it when the pane attaches.
    std::optional<Ubiquitty::Modes::FocusState> m_surfaceFocus;
    /// The newest effect a workspace pane received, by the pane's own
    /// sequence, which every connection to the pane shares.
    struct ReceivedEffect {
        Protocol::Sync::ID128 paneID {};
        std::uint64_t sequence = 0;
    };
    std::optional<ReceivedEffect> m_lastReceivedEffect;
    /// What a pane received on the connection it moved from: an effect up to
    /// it, arriving again here, is not shown twice.
    std::optional<ReceivedEffect> m_effectsReceivedBeforeMove;
    bool m_stopped = false;
    bool m_hostExited = false;
    bool m_isScreenDirty = false;
    // The screen plane's resyncs already recorded in the trace; the plane
    // starts its count anew with each HELLO.
    std::uint64_t m_tracedResyncs = 0;
    // Whether a screen-plane tick is posted for this attempt, and when it is
    // due on the plane's clock. One counts at a time: a deadline that moves
    // earlier posts another under a new serial, and the later one does
    // nothing when it fires.
    bool m_isScreenTickPosted = false;
    std::uint64_t m_screenTickDue = 0;
    std::uint64_t m_screenTickSerial = 0;
    bool m_foregroundIsShell = true; // host's latest Status flag; gates resize reflow
    bool m_reconnectScheduled = false;
    // A timer whose epoch was superseded cannot open a second channel beside
    // the replacement attempt.
    unsigned m_reconnectEpoch = 0;
    // Consecutive failed attempts since the last HELLO_ACK; the backoff delay
    // is derived from it, so a host that is down is asked less and less often.
    unsigned m_reconnectAttempts = 0;
    /// Consecutive drops by the host for falling behind its output. A handshake
    /// does not reset it: reconnecting re-enters the condition that caused the
    /// drop, so each one in a row waits longer. Any other close resets it.
    unsigned m_backlogDrops = 0;
    /// Consecutive typed factory failures; setup errors have a bounded
    /// attempt budget rather than the established link's indefinite backoff.
    unsigned m_channelCreationFailures = 0;
    unsigned m_initialSetupFailures = 0;
    bool m_hasEstablishedConnection = false;
    bool m_initialSetupBudgetExhausted = false;
    // A denial no retry can satisfy. Held because the denial arrives inside
    // feed() while the channel is still up: the daemon's close follows, and
    // this is what tells that close it is terminal rather than a drop.
    bool m_hasPermanentAuthDenial = false;
    std::string m_authDenialNotice;
    /// The session whose pane failed and moved this core to `Failed`; a
    /// rebind of the same pane returns it to `Connected`.
    std::uint32_t m_failedPaneSession = 0;
    HostRefusal m_lastRefusal;
    std::string m_authToken;
    // A QUIC attempt that never reached HELLO_ACK; later dials in this session
    // ask for the stream carrier, including after lifecycle notifications.
    bool m_quicFailed = false;
    // Why the latch above was set by a carrier rather than by the factory:
    // the QUIC carrier did not reach HELLO_ACK. Empty while the latch is clear.
    std::string m_quicHandshakeFailure;
    // Whether a kept connection has yet to prove it still carries frames:
    // the screen plane's probe is out and its Echo has not come. Only that
    // Echo proves the path: frames the host sent before it read the probe,
    // queued while the process was suspended or while the old path was up,
    // arrive ahead of it and prove nothing about the path the connection is
    // on now.
    bool m_isProvingLife = false;
    // Counts the proofs asked for, so a deadline armed for an earlier one
    // judges nothing.
    std::uint64_t m_proofSerial = 0;
    // Whether the attempt in hand presented a resume proof instead of going
    // through the bootstrap.
    bool m_isResumeDial = false;
    // Whether a resume proof was refused or went unanswered since the last
    // HELLO_ACK, so the next attempts go through the bootstrap.
    bool m_isResumeDialRefused = false;
    /// Whether lifecycle recovery has used its immediate retry since the
    /// last Welcome. Bootstrap fallback consumes it too, so the bootstrap
    /// cannot be cancelled repeatedly by the notifications that led to it.
    bool m_hasRetriedAfterLifecycleChange = false;
    // Whether the pin was dropped for a daemon reported gone since the last
    // HELLO_ACK; see reresolveGoneInstance.
    bool m_hasDroppedGonePin = false;
    // Whether the last handshake that landed rode a QUIC carrier: only then
    // is the carrier the factory kept the one this session was on, and a
    // proof worth dialling it with. A session that fell back to the stream
    // goes through the bootstrap, which is where QUIC is offered again.
    bool m_isAcknowledgedCarrierQUIC = false;
    // The link indicators last reported to the observer.
    bool m_isLinkSilentReported = false;
    bool m_isLinkInputUndeliveredReported = false;
    // The counts of the pane's input last reported to the observer.
    std::size_t m_reportedInDoubt = 0;
    std::size_t m_reportedDiscarded = 0;
    // A resume or a path change already acted on in this dispatcher turn; see
    // ``takeLifecycleReport``.
    bool m_isResumeReported = false;
    bool m_isPathChangeReported = false;
    bool m_transportIsQUIC = false;
    bool m_reclaimRequested = false;
    struct ReclaimCall {
        WeakPtr<RemoteSessionCore> endpoint;
        std::uint64_t generation = 0;
        std::uint64_t endpointGeneration = 0;
        std::string correlation;
        Protocol::Control::ReclaimTarget target;
        /// The call's text, kept until it is sent.
        std::string request;
        bool isSent = false;
    };
    /// The most reclaims one physical owner holds unanswered: a shared
    /// window's panes ride one owner, and stopping the share asks for each.
    static constexpr std::size_t maxPendingReclaimCalls = 256;
    /// Unanswered reclaims, one per asking endpoint, oldest first. The host
    /// refuses a second reclaim from an attachment that has one awaiting
    /// release, so only the oldest is sent; each answer lets the next go.
    std::vector<ReclaimCall> m_reclaimCalls;
    std::string m_sharedWorkspaceCorrelation;
    SharedWorkspaceResult m_sharedWorkspaceResult;
    std::uint64_t m_nextReclaimCallID = 0;
    uint32_t m_nextFileRequestID = 0; // pre-incremented; 0 is never assigned
    /// A file callback belongs to one endpoint incarnation, even on a shared carrier.
    std::uint64_t m_fileReadGeneration = 0;
    /// Physical-call correlations never reuse a pane-local request id.
    std::uint64_t m_nextFileCallID = 0;
    /// One pending physical call and the endpoint incarnation allowed to consume it.
    struct FileCall {
        WeakPtr<RemoteSessionCore> endpoint;
        std::uint64_t generation = 0;
        std::uint32_t sessionID = 0;
        std::uint32_t pane = 0;
        std::uint32_t requestID = 0;
        std::string correlation;
        std::string path;
    };
    /// Bounded correlations only; file bytes are owned by the completed result.
    std::array<std::optional<FileCall>, 16> m_fileCalls;
    struct PasteCall {
        WeakPtr<RemoteSessionCore> endpoint;
        std::uint64_t generation = 0;
        std::uint32_t sessionID = 0;
        std::uint32_t pane = 0;
        std::uint32_t pushID = 0;
        Protocol::Control::PasteUploadSource source;
    };
    /// A shared workspace has exactly one metadata-inclusive source owner,
    /// rather than one 4 MiB copy for every logical pane facade.
    std::optional<PasteCall> m_pasteCall;
    std::uint64_t m_nextPasteCallID = 0;
    bool m_isPasteTickPosted = false;
    // The session-list ask space, separate from the file one because they are
    // separate wire spaces and one counter serving both would say two frames
    // are the same request.
    uint32_t m_nextSessionListRequestID = 0; // pre-incremented; 0 is never assigned
    uint32_t m_sessionListRequestID = 0; // the pending local directory request
    std::unique_ptr<HostListing> m_directoryListing;
    uint32_t m_nextWorkspaceVerbRequestID = 0; // pre-incremented; 0 is never assigned
    // The mirror and its pane bindings, rebuilt together from each layout;
    // a layout the model refuses leaves both as they were.
    std::unique_ptr<Layout::Workspace> m_workspace;
    /// The admitted source of the mirror, retained once per structure revision.
    /// Frontends borrow its labels; none keep a second mutable label book.
    std::unique_ptr<TreeWorkspace> m_workspaceStructureSnapshot;
    /// A tab a person brought to the front that the host has not shown yet.
    struct FrontTabAsk {
        /// The verb's request id, which its result echoes.
        uint32_t requestID = 0;
        Layout::SurfaceID surface = Layout::invalidSurface;
        Layout::TabID tab = Layout::invalidTab;
        /// Whether the host said it performed the verb: the structure that
        /// shows the tab is then the next one, and whatever that one names is
        /// the host's word.
        bool isAnswered = false; // bool-ok: names the one state it reports
    };
    /// The asks the mirror has yet to show, one per window at most; held by
    /// the core that holds the mirror.
    std::vector<FrontTabAsk> m_frontTabAsks;
    /// The core holding the mirror this session reads: the workspace
    /// carrier's when the pane rides one, else this one.
    [[nodiscard]] RemoteSessionCore& mirrorOwner();
    [[nodiscard]] const RemoteSessionCore& mirrorOwner() const;
    /// Settles the ask a verb result answers, if it answers one.
    void noteFrontTabAnswer(const Ubiquitty::Session::WorkspaceIntentResult& result);
    std::vector<std::pair<uint32_t, uint32_t>> m_paneSessions;
    uint32_t m_workspaceID = 0;
    // The attach id the HELLO on this attempt carried, held because m_sessionID
    // becomes the host's answer at HELLO_ACK: a denial has to say which session
    // was asked for, which is the value that went out rather than the one that
    // came back.
    uint32_t m_helloAttachSessionID = 0;
    // Deploy handshake (auto-install of ubiquitty-host on a fresh host).
    bool m_deployPending = false; // a deploy is in flight; suppress reconnect
    bool m_deployedOnce = false; // a deploy already ran this lifetime — a
                                 // second sentinel means the pushed binary is
                                 // broken, so fail rather than loop
    // An update asked for before start(); see updateHostBeforeConnect.
    std::optional<std::string> m_updateBeforeConnectPlatform;
    BootstrapResponse m_bootstrapResponse;
    // Bumped each connect()/teardown; a marshaled channel callback carrying an
    // older value belongs to a superseded attempt and is dropped. Atomic: the
    // I/O thread reads it to stamp a callback while the session thread bumps it.
    std::atomic<std::uint64_t> m_generation { 0 };
    // Inbound staging, written by the I/O thread and drained by the session
    // thread. The generation stamps the buffer rather than each post, so bytes
    // from a superseded attempt are dropped instead of fed into its successor.
    std::mutex m_inboundMutex;
    std::vector<std::uint8_t> m_inboundPending;
    // Paces the carrier against what this holds and has not drained. The two
    // marks live in Core so this consumer and the Kit's cannot answer
    // differently how much a slow session may buffer.
    Transport::InboundBackpressure m_inboundBackpressure;
    std::vector<std::uint8_t> m_inboundDraining; // swapped out under the lock; kept
                                                 // as a member so a steady stream
                                                 // reuses one allocation
    // Two lazily allocated fixed-capacity batches: one filled by the I/O thread
    // and one consumed by the session thread. No vector growth can carry the
    // resident queue beyond the named limits under a hostile flood.
    std::unique_ptr<DatagramIngressBatch> m_datagramPending;
    std::unique_ptr<DatagramIngressBatch> m_datagramDraining;
    std::uint64_t m_inboundGeneration = 0;
    bool m_isInboundDrainPosted = false;
    bool m_isReliableDisconnectPosted = false;
    std::uint64_t m_inboundDrainGeneration = 0;
    bool m_isDatagramDrainPosted = false;
    // Observer callbacks can run a nested session loop. A live drain retains
    // its batch until decoding returns and schedules any refill afterward.
    bool m_isDatagramDrainActive = false;
    std::uint64_t m_datagramDrainGeneration = 0;
};

} // namespace Ubiquitty::Session
