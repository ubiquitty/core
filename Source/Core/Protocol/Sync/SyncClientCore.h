// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
//
// SyncClientCore.h — the transport-agnostic Sync client.
//
// Decodes host->client frames, applies the Sync screen plane into a passive
// Terminal through a `Sync::ClientConnection`, and builds the client->host
// frames. It owns no transport: the caller reads bytes and calls feed(), and
// writes whatever the build*() helpers and `takeScreenOutput` return.
//
// The stream is the canonical envelope from byte one (Docs/sync.md §7.2):
// `buildHello` builds one authenticated `Greet`, and the host binds the
// session's pane once it has welcomed the client, and the checkpoints and
// updates that follow apply to the terminal this core was given. The core
// answers with acknowledgements and resyncs of its own, which the caller
// collects with `takeScreenOutput` after each feed and each `tickScreen`.
//
// Single-threaded: feed() and the build*() helpers run on one thread. An
// observer notification may stop/reset and greet a new connection. Active
// readers retain their old connection/attachment until they unwind and consume
// no old suffix.
// screenTerminal is a non-reentrant lookup, not a notification.
// A nested feed into the same connection is not supported. The core itself
// must outlive each call and its callbacks.
//

#include "Base/ActiveObjectUse.h"
#include "Core/Clipboard/Scope.h"
#include "Core/Protocol/Sync/ClientAttachment.h"
#include "Core/Protocol/Sync/Connection.h"
#include "Core/Protocol/Sync/Consent.h"
#include "Core/Protocol/Sync/EffectCodec.h"
#include "Core/Protocol/Sync/HandshakeRequest.h"
#include "Core/Protocol/Sync/HostManagement.h"
#include "Core/Protocol/Sync/NativeAttachment.h"
#include "Core/Screen/ClearBuffer.h"
#include "PAL/Capability.h"
#include "PAL/PAL.h"
#include "PAL/TerminalClients.h"

#include <array>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace Ubiquitty {
struct Terminal;
}

namespace Ubiquitty::Protocol::Sync {

/// Where the host a connection reaches runs, as the client that dialled it
/// knows from its own carrier. The host's word is never asked: a compromised
/// host would answer that it is this machine.
enum class HostLocation : std::uint8_t {
    /// A host on another machine, or one the client cannot place: the client's
    /// own Remote-origin policy decides each effect the host delivers.
    AnotherMachine,
    /// This machine's own daemon, which already gated the effect as this
    /// client's owner would.
    ThisMachine,
    /// The host that served this client's own code, as a web bridge serves its
    /// page. A gate in that code would be the host's, so the host's admission
    /// stands.
    ServedThisClient,
};

/// Why a held effect left without an answer.
enum class HostEffectWithdrawal : std::uint8_t {
    /// A newer effect of its kind took its place, announced first.
    Replaced,
    /// Its connection ended, with every other held effect, all at once: every
    /// slot is empty before the first of these is reported.
    ConnectionEnded,
};

// Per-frame callbacks, all defaulting to no-operations. feed() invokes them
// synchronously on the caller's thread.
class SyncClientObserver {
public:
    virtual ~SyncClientObserver() = default;
    // Fired for every decoded inbound frame (before the type-specific callback),
    // including unknown tags. wireBytes is the framed transport size (header +
    // payload); logicalBytes is the decompressed size (== wireBytes unless compressed).
    virtual void onFrameDecoded(uint8_t /*type*/, std::size_t /*wireBytes*/, std::size_t /*logicalBytes*/) { }
    virtual void onHelloAck(uint32_t /*sessionID*/, const std::string& /*socketName*/) { }

    /// The terminal a session's pane replicates into when the host binds it.
    ///
    /// A connection carrying one session replicates it into the terminal the
    /// core was given, and asks this only for another session; a workspace
    /// carrier, given none, asks for each of its panes.
    ///
    /// - Parameter sessionID: The session the pane shows.
    /// - Returns: The terminal, which must outlive the binding, or null for a
    ///   replica the connection keeps itself.
    /// This lookup must not stop, reset, greet, or otherwise re-enter the
    /// core. Notifications below may replace the connection; lookups may not.
    virtual Ubiquitty::Terminal* screenTerminal(uint32_t /*sessionID*/) { return nullptr; }

    /// Reports that a session's screen took a checkpoint or an update.
    ///
    /// - Parameter sessionID: The session whose terminal changed.
    virtual void onScreenChanged(uint32_t /*sessionID*/) { }

    /// Reports a whole pane record after the connection has finished parsing.
    ///
    /// - Parameters:
    ///   - sessionID: The session whose facts changed.
    ///   - facts: The host's record, borrowed for this call.
    virtual void onPaneFacts(uint32_t /*sessionID*/, const Sync::PaneFacts& /*facts*/) { }

    /// Reports that the host bound a session's pane on this connection, so a
    /// frame addressed to it can leave.
    ///
    /// - Parameter sessionID: The session the pane shows.
    virtual void onPaneBound(uint32_t /*sessionID*/) { }
    /// Reports that one bound pane ended without ending its workspace carrier.
    /// - Parameter sessionID: The exact session whose binding ended.
    virtual void onPaneEnded(uint32_t /*sessionID*/) { }
    /// Reports that a pane cannot be shown, with the reason the person is
    /// told: the host unbound it for a checkpoint past its cap, or the client
    /// refused its checkpoints to the retry policy's end. It is reported in
    /// place of `onPaneEnded` or `onExit` for the same pane.
    /// - Parameters:
    ///   - sessionID: The session the pane shows.
    ///   - reason: The sentence to show, borrowed for this call.
    virtual void onPaneFailed(uint32_t /*sessionID*/, std::string_view /*reason*/) { }

    virtual void onImagesChanged() { } // inline-image store (pixels/placements) updated → repaint
    virtual void onBell() { }
    virtual void onExit() { } // EXIT frame; transport drop is the host's to detect
    /// Reports readable command state, including for an agent whose posting
    /// disposition is Ignore. Both views are valid only for this synchronous call.
    ///
    /// - Parameters:
    ///   - effect: The pane, generation, sequence and immutable primary.
    ///   - command: The completed command and its borrowed shell context.
    ///   - disposition: Whether this client posts, badges or ignores presentation.
    virtual void onCommandDone(const Sync::EffectView& /*effect*/, const Sync::CommandFinishedEffect& /*command*/, Sync::EffectDisposition /*disposition*/) { }
    /// A program on the host asked for a clipboard the host does not have.
    ///
    /// Answered later with ``SyncClientCore/serveClipboard``: reading a
    /// pasteboard can need a user gesture, and the host waits on the reply
    /// rather than on this returning.
    virtual void onClipboardRequest(const ClipboardRequestMessage& /*request*/) { }
    [[nodiscard]] virtual bool supportsNativeDrag() const { return false; }
    /// Delivers a command for the single attached pane.
    virtual void onNativeDragCommand(const NativeDragCommand&) { }
    /// Delivers the primary human's complete typed write on Post only, without
    /// duplicating its payload owner. A queued consumer must copy what it keeps.
    ///
    /// - Parameters:
    ///   - buffer: The exact destination selector.
    ///   - items: Validated items borrowed only for this synchronous callback.
    virtual void onClipboardWriteItems(ClipboardBuffer /*buffer*/, std::span<const ClipboardItem> /*items*/) { }
    /// Posts the primary human's notification while preserving its replacement
    /// identity and condition. A queued consumer must copy what it keeps.
    ///
    /// - Parameter request: Validated fields borrowed for this call.
    virtual void onNotificationRequest(const NotificationRequest& /*request*/) { }
    /// Requests attention on the elected human's frontend.
    virtual void onAttention() { }
    /// Reports an effect the host delivered that this client's own policy
    /// holds for the person: a clipboard write, a notification, a sound or an
    /// attention request from a host on another machine. Nothing happens until
    /// the frontend answers with ``SyncClientCore::answerHeldHostEffect``; an
    /// unanswered effect is never applied. One of each kind is held, a newer
    /// one of a kind replaces the one waiting, a clipboard write to another
    /// buffer with the same items joins it, and every effect that leaves
    /// unanswered is reported to ``onHostEffectWithdrawn``.
    ///
    /// - Parameters:
    ///   - token: The held effect, for its answer.
    ///   - capability: What the effect would do.
    virtual void onHostEffectHeld(std::uint64_t /*token*/, Capability /*capability*/) { }
    /// Reports that a held effect is gone without an answer. A question shown
    /// for it answers nothing now and should close.
    ///
    /// - Parameters:
    ///   - token: The token ``onHostEffectHeld`` reported for it.
    ///   - reason: Whether a newer one replaced it or its connection ended.
    virtual void onHostEffectWithdrawn(std::uint64_t /*token*/, HostEffectWithdrawal /*reason*/) { }
    virtual void onConsentPending(uint32_t /*sessionID*/) { }
    virtual void onConsentDenied(uint32_t /*sessionID*/, Sync::ConsentStatusPhase /*phase*/) { }
    /// The host refused the handshake or an operation and is closing the
    /// connection: an AUTH_DENIED frame, or a Sync connection closed on a
    /// refusal (`Sync::isRefusal`), or Oversize/TransferRefused before Welcome.
    /// Told once a connection.
    ///
    /// - Parameter reason: Why; `Sync::isPermanentRefusal` says whether
    ///   connecting again can change it.
    virtual void onRefused(Sync::ByeReason /*reason*/) { }
    /// Offers one complete canonical Result before any later frame is delivered.
    /// The receiver may consume only its own exact pending correlation.
    /// - Parameter text: Borrowed for this callback; never retained by reference.
    /// - Returns: Whether it was consumed rather than queued for takeCallResults.
    virtual bool consumeControlResult(std::string_view /*text*/) { return false; }
    /// A workspace's structure arrived, or was withdrawn. The host sends the
    /// structure that first names a pane before any frame of the pane, so a
    /// reader that mirrors the workspace knows every pane it is sent frames
    /// of. ``SyncClientCore::workspaceStructure`` holds what stands.
    ///
    /// - Parameter workspaceID: The workspace.
    virtual void onWorkspaceStructure(std::uint32_t /*workspaceID*/) { }
    /// Routes an admitted effect to its workspace endpoint without another decode.
    ///
    /// - Parameters:
    ///   - sessionID: The stable session bound to the effect's pane.
    ///   - effect: Identity and count, borrowed for this call.
    ///   - body: Validated typed fields, borrowed for this call.
    ///   - disposition: The central role and primary decision.
    virtual void onPaneEffect(uint32_t /*sessionID*/, const Sync::EffectView& /*effect*/, const Sync::EffectBody& /*body*/, Sync::EffectDisposition /*disposition*/) { }
    /// Routes a clipboard question to its exact workspace pane endpoint.
    ///
    /// - Parameters:
    ///   - sessionID: The authenticated pane's session.
    ///   - request: A local adapter of the typed request, valid through this call.
    /// - Returns: Whether that endpoint accepted responsibility for answering.
    virtual bool onPaneClipboardRequest(uint32_t /*sessionID*/, const ClipboardRequestMessage& /*request*/) { return false; }
    /// Routes a native command to its workspace pane.
    virtual void onPaneNativeDragCommand(std::uint32_t /*sessionID*/, const NativeDragCommand&) { }
};

/// Decodes host-to-client Sync frames into a passive Terminal and builds the
/// client-to-host frames.
///
/// Owns no transport: the caller reads bytes and passes them to `feed`, then
/// writes whatever the `build*` helpers and `takeScreenOutput` produce. It is
/// the Terminal's `MirrorClient` because the screen plane checks every update
/// against the rows it applied, so Core tells it when something else has
/// rewritten the grid.
class SyncClientCore : public Ubiquitty::MirrorClient, private Sync::ClientPaneDelegate, private Sync::ClientWelcomeSink {
public:
    /// The clock the screen plane reads, in microseconds.
    using Clock = std::function<std::uint64_t()>;

    // `term` and `observer` are caller-owned and outlive the core; `term` is
    // written only from feed() and tickScreen(). A null observer decodes and
    // updates the Terminal with no callbacks.
    SyncClientCore(Ubiquitty::Terminal*, SyncClientObserver*);
    ~SyncClientCore() override;

    SyncClientCore(const SyncClientCore&) = delete;
    SyncClientCore& operator=(const SyncClientCore&) = delete;

    /// Installs the sole store owner and restores before any greeting or I/O.
    /// A prepared workspace core owns its cached terminals; drawing a preview
    /// never lends a frontend terminal across the asynchronous dial.
    ///
    /// - Parameters:
    ///   - store: A protected store already bound to the saved target.
    ///   - clientInstance: The protected stable client identity.
    ///   - certificatePin: The protected pin used only for cold reads; the
    ///     authenticated carrier must replace it before accepting Welcome.
    /// - Returns: Whether preparation succeeded, with or without a cached record.
    [[nodiscard]] bool prepareResumeStore(std::shared_ptr<Sync::ResumeRecordStore> store, Sync::ID128 clientInstance, const std::array<std::uint8_t, 32>& certificatePin);

    /// Installs the window's store on a core that draws into its own terminal,
    /// and restores before any greeting or I/O: the stored picture of the
    /// window's pane is drawn into that terminal, and the next `buildHello`
    /// resumes the stored attachment. A stored record of another client
    /// instance is purged before anything is drawn, and the core starts fresh
    /// with the store.
    ///
    /// - Parameters:
    ///   - store: The window's store, which this core keeps.
    ///   - clientInstance: The window's stable client identity.
    /// - Returns: Whether the store was installed, with or without a picture.
    [[nodiscard]] bool restoreResumeStore(std::shared_ptr<Sync::ResumeRecordStore> store, Sync::ID128 clientInstance);

    /// Writes the window's record now, as a frontend does when it is about
    /// to go away: the newest picture the client holds is the one stored.
    void persistResume()
    {
        if (m_resumeStore && m_attachment)
            m_attachment->persist();
    }

    /// Whether a restored picture of the window's pane is drawn and no
    /// connection has replaced it yet.
    [[nodiscard]] bool hasRestoredPicture() const
    {
        return m_sessionID && m_attachment && m_attachment->restoredPictureGeneration(paneIDForSession(m_sessionID)).has_value();
    }

    /// Copies one authenticated cached picture without starting a connection.
    ///
    /// - Parameters:
    ///   - paneID: The protected selected pane identity.
    ///   - destination: The frontend's own terminal, retained by that frontend.
    /// - Returns: Whether a complete picture was committed to the destination.
    [[nodiscard]] bool drawPreparedPane(Sync::ID128 paneID, Ubiquitty::Terminal& destination) const;

    /// The generation authenticated by the prepared picture's checkpoint.
    [[nodiscard]] std::optional<std::uint64_t> preparedPaneGeneration(Sync::ID128 paneID) const
    {
        return m_attachment ? m_attachment->restoredPictureGeneration(paneID) : std::nullopt;
    }

    /// Transfers notification ownership before the first greeting only.
    ///
    /// - Parameter observer: The physical connection owner, which outlives this core.
    /// - Returns: Whether this was an unused prepared core.
    [[nodiscard]] bool attachPreparedObserver(SyncClientObserver& observer);

    /// Records the pin actually used by the authenticated transport.
    ///
    /// - Parameter certificatePin: The current carrier's verified pin.
    void setResumeCertificatePin(const std::array<std::uint8_t, 32>& certificatePin);

    /// Decodes whole frames from the buffer and dispatches them.
    ///
    /// A partial read is fine: the tail is buffered until the rest arrives, so
    /// the caller may hand over whatever the transport delivered.
    ///
    /// - Parameter bytes: Transport bytes, at any frame boundary or none.
    /// - Returns: False once this connection cannot continue — the decoder
    ///   faulted, the screen plane closed, or the handshake refused the host —
    ///   which is when the caller drops it.
    [[nodiscard]] bool feed(std::span<const std::byte> bytes);

    /// Delivers a workspace carrier's record to this pane's client.
    ///
    /// - Parameter facts: The whole record already checked by the carrier.
    void receivePaneFacts(const Sync::PaneFacts& facts);

    /// The host's latest record for a bound session, including one whose
    /// frontend attached after the record arrived.
    ///
    /// - Parameter sessionID: The bound session.
    /// - Returns: The borrowed record, or null until one has arrived.
    [[nodiscard]] const Sync::PaneFacts* paneFacts(uint32_t sessionID) const;
    /// Whether this admitted attachment must present another owner's grid.
    /// Retained facts cannot confer authority while disconnected or reconnecting.
    [[nodiscard]] bool isGeometryOwnedElsewhere(uint32_t sessionID) const;

    /// Feeds one QUIC datagram: exactly one Sync envelope.
    ///
    /// A datagram carries a superset update or a probe, which applies on any
    /// generation the pane holds since its base, so a lost or reordered one
    /// costs only the next.
    ///
    /// - Parameter bytes: The datagram.
    /// - Returns: False once the screen plane closed.
    [[nodiscard]] bool feedDatagram(std::span<const std::byte> bytes);

    /// Sends a screen-plane acknowledgement when one is due.
    void tickScreen();

    /// Whether the screen plane found the path silent: the owner drops the
    /// transport and dials again.
    [[nodiscard]] bool isScreenLost() const;

    /// Whether a welcomed host requested an immediate, unpinned successor dial.
    [[nodiscard]] bool wantsHandoverRedial() const;

    /// When `tickScreen` next has something to send, or the link's
    /// indicators next turn.
    ///
    /// - Returns: Microseconds from now, or `nullopt` when nothing is owed.
    [[nodiscard]] std::optional<std::uint64_t> screenTickDelayMicroseconds() const;

    /// The same moment on the clock the screen plane reads, so an owner can
    /// tell a deadline that moved earlier from the one it already waits for.
    ///
    /// - Returns: The time, or `nullopt` when nothing is owed.
    [[nodiscard]] std::optional<std::uint64_t> screenTickDueMicroseconds() const;

    /// Takes the screen plane's stream output as Sync frames.
    ///
    /// - Parameter out: Receives the frames, appended; untouched when the
    ///   plane has nothing to say.
    void takeScreenOutput(std::vector<uint8_t>& out);

    /// Takes the datagrams the screen plane queued, each one whole envelope.
    [[nodiscard]] std::vector<std::vector<std::uint8_t>> takeScreenDatagrams();

    /// Moves a session's screen onto a terminal, and asks for the checkpoint
    /// that fills it.
    ///
    /// A workspace carrier binds a pane before a pane core may be attached to
    /// it, and outlives the cores that attach and detach.
    ///
    /// - Parameters:
    ///   - sessionID: The session.
    ///   - terminal: The terminal of the core that holds it.
    void setSessionTerminal(uint32_t sessionID, Ubiquitty::Terminal& terminal);

    /// Moves a session's screen off the terminal of a core that let go of it,
    /// onto one the connection keeps, so the binding outlives the core.
    ///
    /// - Parameter sessionID: The session.
    void releaseSessionTerminal(uint32_t sessionID);

    /// Takes back a terminal a core lent and is about to free. A bound pane
    /// moves off it through ``releaseSessionTerminal(_:)`` first; a pane the
    /// host unbound, or one held across a reconnect, has no handle to move
    /// and keeps its picture there until this drops it.
    ///
    /// - Parameter terminal: The core's terminal.
    void withdrawTerminal(const Ubiquitty::Terminal& terminal);

    /// Sets the view this connection holds of a session's pane: whether the
    /// host sends its screen (Docs/sync.md §6.8). Kept for a pane not yet
    /// bound, which takes it when the host binds it.
    ///
    /// - Parameters:
    ///   - sessionID: The session.
    ///   - state: The view.
    void setPaneView(uint32_t sessionID, Sync::ViewState state);

    /// The stream bytes the host sent a session's pane on this connection.
    ///
    /// - Parameter sessionID: The session.
    /// - Returns: The bytes, 0 for a pane not bound.
    [[nodiscard]] std::uint64_t paneBytesReceived(uint32_t sessionID) const;

    /// Whether a session's pane is bound on this connection, which is when a
    /// frame can be addressed to it.
    ///
    /// - Parameter sessionID: The session.
    /// - Returns: Whether the host bound it.
    [[nodiscard]] bool isPaneBound(uint32_t sessionID) const;

    /// The current connection-local handle of a stable session's pane.
    /// - Parameter sessionID: The session, including panes inherited by a successor.
    /// - Returns: Its bound handle, or zero if it is not bound.
    [[nodiscard]] std::uint32_t paneHandle(std::uint32_t sessionID) const;

    /// The current exact bound identity, never reconstructed from a successor.
    /// - Parameter sessionID: The native session label.
    /// - Returns: Its admitted bound pane, or no identity before attachment/bind.
    [[nodiscard]] std::optional<Sync::ID128> boundPaneIdentity(std::uint32_t sessionID) const;

    /// Requests history of this exact retained session identity, never a pane
    /// inferred from the latest daemon instance after a handover.
    [[nodiscard]] bool wantPaneHistory(std::uint32_t sessionID, Sync::HistoryPriority, std::span<const Sync::LineRange>);

    /// Publishes a pane's native typography now or once its binding arrives.
    ///
    /// - Parameters:
    ///   - sessionID: The pane's session; zero names the pending initial session.
    ///   - facts: Its renderer revision, capabilities and font metrics.
    void publishTypography(std::uint32_t sessionID, const RichText::TypographyFacts& facts);

    /// Tells the host the faces this client draws proportional runs in
    /// (Docs/proportional-text.md), now or once `buildHello` has greeted a
    /// screen plane; `takeScreenOutput` carries them.
    ///
    /// - Parameter faces: The faces, from the client's measurer.
    void publishProportionalFaces(const OSC::TextSizing::ProportionalFaceFacts& faces);

    /// The connection's screen plane, or null before `buildHello` greets one
    /// and once the connection it was greeted on is gone.
    [[nodiscard]] const Sync::ClientConnection* screen() const { return liveScreen(); }

    /// The attachment the screen planes of this core's connections continue
    /// (Docs/sync.md §6.9), or null before the first `buildHello` greets
    /// one. It outlives each connection: the next `buildHello` resumes it.
    [[nodiscard]] const Sync::ClientAttachment* attachment() const { return m_attachment.get(); }

    /// A workspace's structure as the host last sent it on this connection.
    ///
    /// - Parameter workspaceID: The workspace.
    /// - Returns: The record's bytes, for `Session::decodeWorkspaceStructure`;
    ///   empty when the host sent none or withdrew it.
    [[nodiscard]] std::span<const std::uint8_t> workspaceStructure(std::uint32_t workspaceID) const;

    /// Sets the instance this client greets as, which a frontend keeps with
    /// its window's restoration identity so a relaunched window is the same
    /// client to the host. It applies to the attachment the first
    /// `buildHello` makes; a core given none draws one at random.
    ///
    /// - Parameter clientInstance: The instance.
    void setClientInstance(Sync::ID128 clientInstance) { m_clientInstance = clientInstance; }

    /// The instance this client greets as.
    [[nodiscard]] Sync::ID128 clientInstance() const { return m_clientInstance; }

    /// Sets how the next `buildHello` proves the attachment it resumes: the
    /// secret inside a stream the login authenticated, or nothing more on a
    /// connection a resume proof admitted (`resumeProofPreamble`).
    ///
    /// - Parameter carrier: The carrier's way.
    void setScreenResumeCarrier(Sync::ResumeCarrier carrier) { m_screenResumeCarrier = carrier; }

    /// What a carrier needs to continue the attachment without a login: the
    /// values a resume proof is computed from, copied so the proof can be
    /// built on the carrier's own thread once its handshake has exported
    /// keying material.
    ///
    /// - Returns: The values, or `nullopt` when no attachment is held.
    [[nodiscard]] std::optional<Sync::HeldAttachment> heldAttachment() const { return m_attachment ? m_attachment->held() : std::nullopt; }

    /// Sends one input event to a session's pane, numbered in the pane's
    /// order (Docs/sync.md §6.7). While no connection carries the pane the
    /// event waits in the attachment, within its bound, and the next
    /// connection sends it; the host writes each event once, whichever
    /// connection brought it.
    ///
    /// - Parameters:
    ///   - sessionID: The session whose pane it is for.
    ///   - intent: The event.
    /// - Returns: Its number, or why it was not taken: `Unsendable` before
    ///   the host has named a session, as well as for an event that is not
    ///   one frame.
    [[nodiscard]] std::expected<std::uint64_t, Sync::InputRefusal> sendIntent(uint32_t sessionID, Sync::InputIntent intent);

    /// What the person is told of a session's input: events whose fate the
    /// host could not report, events waiting, events discarded.
    ///
    /// - Parameter sessionID: The session.
    /// - Returns: The status; all zero for a session not known.
    [[nodiscard]] Sync::PaneInputStatus paneInputStatus(uint32_t sessionID) const;

    /// The input events of a session's pane the host has not yet reported
    /// written: sent, waiting to be sent, or in doubt.
    ///
    /// - Parameter sessionID: The session.
    /// - Returns: How many; zero for a session not known.
    [[nodiscard]] std::size_t paneInputOutstanding(uint32_t sessionID) const;

    /// Sends a session's events in doubt again, as the person asked.
    ///
    /// - Parameter sessionID: The session.
    void resendInDoubt(uint32_t sessionID);

    /// Discards a session's events in doubt, as the person asked.
    ///
    /// - Parameter sessionID: The session.
    void discardInDoubt(uint32_t sessionID);

    /// Reports that the connection's transport is gone. The attachment keeps
    /// each pane's picture and what is known of it for the next connection.
    void noteTransportLost();

    /// Tells the host the client is about to be suspended and ends the
    /// screen plane (`ClientAttachment::suspend`); `takeScreenOutput` carries
    /// the `Bye`, and the owner then drops the transport.
    void suspendScreen();

    /// Probes the host before the connection is trusted again, as after a
    /// wake or a network-path event (Docs/sync.md §6.9);
    /// `takeScreenOutput` carries the probe.
    ///
    /// - Returns: Whether a welcomed screen plane was there to probe.
    bool checkScreenLiveness();

    /// Whether the probe `checkScreenLiveness` sent is still unanswered. Only
    /// its own `Echo` answers it; what the owner does with a silence is the
    /// owner's to time.
    [[nodiscard]] bool isScreenLivenessUnproven() const;

    /// The link as a frontend shows it.
    [[nodiscard]] Sync::LinkState screenLinkState() const;

    /// Reads the exact clock used by the retained attachment's liveness model.
    [[nodiscard]] std::uint64_t screenClockMicroseconds() const { return now(); }

    /// Whether the host has welcomed the screen plane and it is still open,
    /// which is when a call can be sent.
    ///
    /// - Returns: False before the `Welcome` and once the plane closed.
    [[nodiscard]] bool isScreenAttached() const;

    /// Sends a Sync `Call` on the screen plane; `takeScreenOutput` carries
    /// it and its answer arrives through `takeCallResults`.
    ///
    /// - Parameter request: A JSON-RPC 2.0 request with an id.
    /// - Returns: False, sending nothing, with no screen plane, once it
    ///   closed, or for text that is not one frame.
    [[nodiscard]] bool call(std::string_view request);
    /// Queues a bounded raw chunk only after the caller matched canonical upload credit.
    /// - Parameter chunk: One credited control upload's raw chunk.
    /// - Returns: Whether the current physical screen connection queued it.
    [[nodiscard]] bool sendControlUploadChunk(const Sync::BulkChunk& chunk);

    /// Asks to change only this connection's clipboard serving intent.
    ///
    /// - Parameter serving: Whether the local clipboard may serve remote reads.
    /// - Returns: The request ID, or nothing when no live connection accepts it.
    [[nodiscard]] std::optional<std::uint64_t> setClipboardServing(Clipboard::Serving serving);

    /// A number that moves whenever anything a listing of the host is built
    /// from changed on the screen plane: a workspace's structure arrived or
    /// was withdrawn, a pane was unlisted, or a pane's facts or status set
    /// changed, listed or bound. A pane newly listed moves nothing until
    /// its records follow. `Session::HostListing` assembles
    /// its tree again when this has moved.
    ///
    /// - Returns: The count; zero until the first change.
    [[nodiscard]] std::uint64_t listingRevision() const { return m_listingRevision; }

    /// Takes the host's `Notify`s received since the last take.
    ///
    /// - Returns: Each a kind this build knows and its unread record; empty
    ///   with no screen plane.
    [[nodiscard]] std::vector<Sync::Notify> takeHostNotifies();

    /// Retains one strictly decoded current roster in the existing attachment
    /// model, so a pane opened later projects the same canonical snapshot.
    /// - Parameter list: A complete validated attachments Notify.
    void noteAttachments(Sync::AttachmentsList list);

    /// Tells the host this connection is leaving for good, so its attachment
    /// is released at once rather than held for a resume; `takeScreenOutput`
    /// carries the `Bye`. Nothing is sent before the `Welcome`.
    void leaveScreen();

    /// Replaces the clock the screen plane reads.
    ///
    /// - Parameter clock: Returns microseconds on a monotonic clock.
    void setClock(Clock clock) { m_clock = std::move(clock); }

    /// Sets the largest datagram the carrier sends, 0 for a carrier without
    /// datagrams. It applies to the screen plane the next `buildHello` greets:
    /// with datagrams the plane sends its own probes as datagrams.
    ///
    /// - Parameter bytes: The size.
    void setScreenDatagramMaxBytes(std::size_t bytes) { m_screenDatagramMaxBytes = bytes; }

    /// Tells the screen plane whether the client is focused: the host probes
    /// an idle connection only while it is (Docs/sync.md §6.4). Kept for the
    /// planes later greetings make.
    ///
    /// - Parameter focus: Whether some surface of the connection has the
    ///   keyboard.
    void setScreenFocus(Ubiquitty::Modes::FocusState focus);

    /// Tells the host whether a session's pane holds the keyboard focus in
    /// this client: the host reports a pane's focus to its program under
    /// DECSET 1004 from the drivers that hold it (Docs/sync.md §6.7). Kept
    /// for the pane's next bind, on this connection or a later one. A pane
    /// whose owner never says is told nothing of, and counts as unfocused.
    ///
    /// - Parameters:
    ///   - sessionID: The session whose pane it is.
    ///   - focus: Whether its surface has the keyboard.
    void setPaneFocus(uint32_t sessionID, Ubiquitty::Modes::FocusState focus);

    /// Asks the host to set a session's pane to a size once
    /// (`pane.overrideGeometry`): an explicit resize, which holds until a
    /// driver types or reports a different size. The host's answer arrives
    /// through `takeCallResults`.
    ///
    /// - Parameters:
    ///   - sessionID: The session whose pane it is.
    ///   - rows: The rows asked for.
    ///   - columns: The columns asked for.
    /// - Returns: The call's JSON-RPC id, or `nullopt`, sending nothing, for
    ///   a pane the host has not bound on a live connection.
    [[nodiscard]] std::optional<std::uint64_t> overridePaneGeometry(uint32_t sessionID, int rows, int columns);

    /// Takes the host's answers to this client's calls received since the
    /// last take, each a JSON-RPC 2.0 response, in the order they completed.
    ///
    /// - Returns: The responses; empty with no screen plane.
    [[nodiscard]] std::vector<std::string> takeCallResults();

    /// Establishes a pane decoder from the authenticated workspace handshake.
    ///
    /// - Parameters:
    ///   - sessionID: Authoritative pane session identity.
    ///   - connection: Authenticated workspace handshake to share.
    void establishWorkspacePane(uint32_t sessionID, const SyncClientCore& connection);
    /// Delivers an admitted effect under this endpoint's authenticated pane identity.
    ///
    /// - Parameters:
    ///   - effect: Identity and count, borrowed for this call.
    ///   - body: Validated typed fields, borrowed for this call.
    ///   - disposition: The physical connection's posting decision.
    void receivePaneEffect(const Sync::EffectView& effect, const Sync::EffectBody& body, Sync::EffectDisposition disposition);

    /// Places the host this connection reaches; another machine until told.
    ///
    /// - Parameter location: Where the client's own carrier says the host runs.
    void setHostLocation(HostLocation location) { m_hostLocation = location; }
    /// Where the host this connection reaches runs.
    ///
    /// - Returns: The location ``setHostLocation`` gave, or another machine.
    [[nodiscard]] HostLocation hostLocation() const { return m_hostLocation; }
    /// The verdict this client's own configuration gives the Remote origin for
    /// one capability, read each time a host on another machine delivers an
    /// effect that needs it.
    using HostEffectDecisionSource = std::function<CapabilityDecision(Capability)>;
    /// Sets where the verdicts come from, so a settings change reaches an open
    /// connection at its next effect. Without one, the origin's defaults
    /// decide.
    ///
    /// - Parameter source: Reads the client's current verdict for a capability.
    void setHostEffectDecisions(HostEffectDecisionSource source) { m_hostEffectDecisions = std::move(source); }
    /// Applies or discards the held effect on the person's answer.
    ///
    /// A stale token answers nothing. A session or always answer also allows
    /// later effects of the same kind until this connection ends, except for a
    /// capability whose grants are good for one use.
    ///
    /// - Parameters:
    ///   - token: The token ``SyncClientObserver::onHostEffectHeld`` reported.
    ///   - choice: The person's answer; any refusal discards the effect.
    void answerHeldHostEffect(std::uint64_t token, ConsentChoice choice);
    /// A held effect waiting for an answer.
    struct HeldHostEffectQuestion {
        std::uint64_t token = 0;
        Capability capability = Capability::None;
    };
    /// The held effect that has waited longest, for a session that shows one
    /// question at a time. A newer effect that replaced one of its kind keeps
    /// that one's place in line.
    ///
    /// - Returns: The effect, or nothing when none waits.
    [[nodiscard]] std::optional<HeldHostEffectQuestion> oldestHeldHostEffect() const;

    [[nodiscard]] bool faulted() const { return m_didRefuseHost || isScreenClosed(); }

    /// Refuses the host whose HELLO_ACK was just delivered.
    ///
    /// An observer that rejects the acknowledged host calls this from inside
    /// `onHelloAck`. `feed` then dispatches nothing more from the bytes it was
    /// handed and answers false, so the refused host's screen, title and the
    /// rest of the read never reach the mirror or the observer, and the
    /// handshake counts as unacknowledged.
    void refuseHost()
    {
        m_didRefuseHost = true;
        m_sawHelloAck = false;
        noteTransportLost();
    }

    // Reset per-connection decode state (decoder + handshake/snapshot flags)
    // for a reconnect that reuses this core; the Ubiquitty::Terminal contents are kept.
    void resetForReconnect();

    [[nodiscard]] uint32_t sessionID() const { return m_sessionID; }
    /// Returns the stable workspace identity supplied by the accepted handshake.
    [[nodiscard]] uint32_t acknowledgedWorkspaceID() const { return m_acknowledgedWorkspaceID; }
    [[nodiscard]] const std::string& socketName() const { return m_socketName; }
    /// Returns the daemon process identity advertised by the authenticated connection.
    [[nodiscard]] std::uint64_t daemonInstanceID() const { return m_daemonInstanceID; }
    /// Whether the admitted Welcome continues this exact predecessor.
    /// - Parameter predecessor: The daemon identity held by the session owner.
    /// - Returns: True only after matching carrier and screen-plane admission.
    [[nodiscard]] bool isSuccessorOf(std::uint64_t predecessor) const;
    /// Whether the admitted Welcome recreated this client's workspace from
    /// the host's record after the instance it held restarted.
    /// - Parameter predecessor: The daemon identity the client's reference named.
    /// - Returns: True only when the host said it restored that reference.
    [[nodiscard]] bool isRestoredSuccessorOf(std::uint64_t predecessor) const { return predecessor && m_restoredFromDaemonInstanceID == predecessor && m_daemonInstanceID != predecessor; }
    /// The pinned daemon instance named by an admitted Workspace restore, or zero otherwise.
    [[nodiscard]] std::uint64_t restoredFromDaemonInstanceID() const { return m_restoredFromDaemonInstanceID; }
    [[nodiscard]] bool sawHelloAck() const { return m_sawHelloAck; }

    /// What the compiler did to the host, as it reported at the handshake.
    ///
    /// An accessor rather than a fifth argument to ``onHelloAck``: every
    /// existing observer would have to be changed to ignore it, and a frontend
    /// that wants to surface it can ask at any point after the handshake rather
    /// than only at the instant it lands.
    ///
    /// - Returns: The flags, or an empty set before a HELLO_ACK has arrived.
    [[nodiscard]] Protocol::Sync::HostBuildFlags hostBuildFlags() const { return m_hostBuildFlags; }
    /// What the host said about its build: the range its HELLO_ACK or its
    /// refusal named, and its app version.
    ///
    /// Recorded before the version gate, so a host this client refused still
    /// says which side is older.
    ///
    /// - Returns: The build, not ``Sync::PeerBuild::isKnown`` before the host
    ///   said one.
    [[nodiscard]] const Sync::PeerBuild& hostBuild() const { return m_hostBuild; }
    /// Keeps a run of frames the host reads: whole frames, each one a kind a
    /// client sends.
    ///
    /// - Parameters:
    ///   - frames: Whole frames this client built.
    ///   - kept: Receives the frames, appended.
    /// - Returns: False when `frames` is not a run of whole client frames.
    [[nodiscard]] bool keepFramesHostReads(std::span<const std::uint8_t> frames, std::vector<std::uint8_t>& kept) const;
    /// The `uname -sm` a refusing host reported, empty when none did.
    [[nodiscard]] const std::string& hostPlatform() const { return m_hostPlatform; }
    /// Whether this connection's session holds a screen the next update can
    /// build on: a checkpoint applied, and none asked for since.
    [[nodiscard]] bool hasScreen() const;
    [[nodiscard]] const std::string& title() const { return m_title; }

    // Every build* APPENDS its frame to `out` and never clears it, so a caller
    // may accumulate several into one buffer and write them once — the frames
    // arrive in the order they were built. A caller reusing a buffer without
    // clearing it therefore resends what is already in it; one clearing between
    // frames sends only the last. Neither is a corruption, but they are opposite
    // mistakes, and the contract has to be stated for a caller to pick the one
    // it wants: the wire is a stream of frames, not a slot holding one.
    //
    /// Builds the one canonical `Greet` opening an attachment or host control.
    ///
    /// The caller owns label, token, and intent resolution. Admission and the
    /// greeting use the same request. A control-only request binds no pane.
    ///
    /// - Parameters:
    ///   - request: The client's attachment or control intent.
    ///   - out: Receives the frames, appended.
    void buildHello(const HandshakeRequest& request, std::vector<uint8_t>& out);
    /// Tells the host the size this client shows a session's pane at
    /// (`Viewport`): the grid, and the pixels the frontend last published to
    /// this core's terminal, since the host has no window to measure and its
    /// program is told the winsize the pane's owner draws. Kept for the
    /// pane's next bind, on this connection or a later one, and sent again
    /// only when it changes. An agent's core sends none.
    ///
    /// - Parameters:
    ///   - sessionID: The session whose pane it is.
    ///   - viewport: The size the pane is shown at.
    /// - Returns: Whether the size differs from the one last stated for the
    ///   pane, which is when the host is told.
    bool setPaneViewport(uint32_t sessionID, const Sync::Viewport& viewport);

    /// Tells the host the size of this client's window (`Facts`), which is
    /// the size of a pane that is all the window shows. Kept for the next
    /// connection's greeting.
    ///
    /// - Parameter viewport: The window's grid and pixels.
    void setWindowViewport(const Sync::Viewport& viewport);

    /// Publishes the attachment's theme through `Facts` and retains it for reconnect.
    ///
    /// - Parameters:
    ///   - palette: What renders, and what OSC 4/10/11 mutate.
    ///   - baseline: What OSC 110/111/112 revert to.
    void setThemePalette(const TermPalette& palette, const TermPalette& baseline);

    /// Tells the host what this client's network path costs (`Facts.link`),
    /// now or in the next connection's greeting, and keeps it for every
    /// greeting after.
    ///
    /// - Parameter link: The link's cost.
    void setLinkCost(Sync::LinkCost link);

    /// The link cost the next greeting states.
    [[nodiscard]] Sync::LinkCost linkCost() const { return m_linkCost; }

    /// Notes the topology of the panes this client draws, from the structure
    /// the host sent for its workspace (Docs/sync.md §6.10 "Topology never
    /// overtakes content").
    ///
    /// - Parameter tree: The tree; one older than the tree held is dropped.
    void noteTopology(Sync::TopologyTree tree);

    /// Where to lay a session's pane out, and whether its picture fills that
    /// room: the leaf's size only once the picture held has reached the
    /// leaf's generation at that size.
    ///
    /// - Parameter sessionID: The session whose bound pane is asked about.
    /// - Returns: The rectangle, or `nullopt` before a picture and a tree
    ///   naming the pane.
    [[nodiscard]] std::optional<Sync::PaneRectangle> paneRectangle(std::uint32_t sessionID) const;

    /// Queues a typed clear call for the exact bound pane, leaving its replica
    /// untouched until the host publishes the resulting state.
    ///
    /// - Parameters:
    ///   - sessionID: The session whose bound handle is addressed.
    ///   - command: Which clear the user asked for.
    /// - Returns: Whether the call was queued, not whether it has completed.
    [[nodiscard]] bool clearPane(uint32_t sessionID, Ubiquitty::Screen::ClearCommand command);
    void buildExit(std::vector<uint8_t>& out) const;

    /// Builds a CLIPBOARD_DATA frame: the answer to a CLIPBOARD_REQUEST.
    ///
    /// - Parameters:
    ///   - data: The request id being answered, the status, and the items.
    ///   - out: Receives the framed bytes.
    void buildClipboardData(const ClipboardDataMessage& data, std::vector<uint8_t>& out);
    [[nodiscard]] bool sendNativeDragEvent(std::uint32_t sessionID, const DragDrop::NativeEvent&, std::span<const char>);

    /// Declares that the mirrored Terminal was changed by something other than
    /// this core, so the next update cannot be trusted to build on it.
    ///
    /// An update names the rows that moved since the generation the client
    /// acknowledged and is checked against the row hashes of what was applied.
    /// Clearing the buffer or the scrollback from a frontend moves the grid
    /// without moving those hashes, so an update would apply onto the wrong
    /// rows and still pass its digest. The session's pane asks for a
    /// checkpoint instead, and applies no update until it arrives.
    void invalidateBaseline() override;

    /// Requests a fresh host checkpoint for one bound workspace pane.
    /// - Parameter sessionID: The retained session whose picture is needed.
    void requestPaneCheckpoint(std::uint32_t sessionID);

private:
    bool consumeControlResult(std::string_view text) override;
    /// Checks carrier identity and exact predecessor before store or replica mutation.
    /// - Parameter welcome: The decoded screen-plane identity.
    /// - Returns: Whether it agrees with this connection's admission and resume.
    [[nodiscard]] bool acceptWelcome(const Sync::Welcome& welcome) override;
    /// Settles deferred carrier admission before any pane callback is delivered.
    /// - Parameter welcome: The identity admitted by both barriers.
    void hostWelcomed(const Sync::Welcome& welcome) override;
    void consentRefused(const Sync::ConsentStatus& status) override;
    /// Resolves a session to its original pane identity across host replacement.
    /// - Parameter sessionID: The host's stable session number.
    /// - Returns: The retained binding, or the current issuer's new identity.
    [[nodiscard]] Sync::ID128 paneIDForSession(std::uint32_t sessionID) const;
    /// Checks the full retained binding, not only a matching session number.
    /// - Parameter paneID: The stable identity carried by a pane frame.
    /// - Returns: Whether that exact pane belongs to the session mapping.
    [[nodiscard]] bool isSessionPane(Sync::ID128 paneID) const;
    void connectionFrameDecoded(Sync::FrameKind, std::size_t wireBytes) override;
    void connectionFrameApplied(const Sync::ClientConnection&) override;

    /// Hands canonical stream bytes to the screen plane.
    ///
    /// - Parameter bytes: Any complete or partial canonical frames from a read.
    void receiveScreenBytes(std::span<const std::uint8_t> bytes);

    /// Whether the screen plane exists and has closed.
    [[nodiscard]] bool isScreenClosed() const;

    /// The clock's reading.
    [[nodiscard]] std::uint64_t now() const;

    /// Names the terminal a bound pane replicates into: the session's own for
    /// this connection's session, the observer's for a workspace pane.
    ///
    /// - Parameter paneID: The pane the host bound.
    /// - Returns: The terminal, or null for one the connection owns.
    Ubiquitty::Terminal* terminalForPane(Sync::ID128 paneID) override;

    /// Tells the observer a pane's screen moved.
    ///
    /// - Parameters:
    ///   - handle: The pane's handle.
    ///   - paneID: The pane.
    void paneChanged(std::uint32_t handle, Sync::ID128 paneID) override;

    /// Tells the observer a session's pane can be addressed.
    ///
    /// - Parameters:
    ///   - handle: The pane's handle.
    ///   - paneID: The pane.
    void paneBound(std::uint32_t handle, Sync::ID128 paneID) override;
    void paneUnbound(std::uint32_t handle, Sync::ID128 paneID) override;
    /// Queues a session pane's failure for its observer, after the frame.
    ///
    /// - Parameters:
    ///   - handle: The pane's handle.
    ///   - paneID: The pane.
    ///   - reason: What the person is told, copied.
    void paneFailed(std::uint32_t handle, Sync::ID128 paneID, std::string_view reason) override;

    /// Notes a bound pane's records for both listing and terminal readers.
    /// A records-only change needs the same publication as a screen update.
    ///
    /// - Parameters:
    ///   - handle: The bound pane's connection-local handle.
    ///   - paneID: The pane's stable identity.
    void paneRecordsChanged(std::uint32_t handle, Sync::ID128 paneID) override;
    /// Routes one once-only effect while its decoder and attachment stay retained.
    ///
    /// - Parameters:
    ///   - handle: The admitted connection-local pane handle.
    ///   - effect: Stable pane and sequence identity.
    ///   - body: Validated fields borrowing the retained decoder.
    ///   - disposition: The central role and primary decision.
    void paneEffect(std::uint32_t handle, const Sync::EffectView& effect, const Sync::EffectBody& body, Sync::EffectDisposition disposition) override;
    /// Notes that a listed pane's records moved; see `listingRevision`.
    void listedPaneChanged(Sync::ID128) override { ++m_listingRevision; }
    /// Notes that a pane is no longer listed; see `listingRevision`.
    void paneUnlisted(Sync::ID128) override { ++m_listingRevision; }

    /// The view a pane the host binds takes: the one its owner set, or the
    /// connection's own.
    ///
    /// - Parameters:
    ///   - handle: The pane's handle.
    ///   - paneID: The pane.
    /// - Returns: The view, or `nullopt` for the connection's own.
    std::optional<Sync::ViewState> viewOnBind(std::uint32_t handle, Sync::ID128 paneID) override;

    /// Hands the observer a Sync frame the host addressed to a pane.
    ///
    /// - Parameters:
    ///   - handle: The pane's handle.
    ///   - paneID: The pane.
    ///   - message: One whole Sync frame.
    /// Routes a validated request to this pane's authorized frontend reader.
    ///
    /// - Parameters:
    ///   - handle: The connection-local bound pane handle.
    ///   - paneID: The verified daemon and session identity.
    ///   - request: The exact clipboard operation, borrowed for this call.
    /// - Returns: Whether the frontend accepted responsibility for an answer.
    bool paneClipboardRequest(std::uint32_t handle, Sync::ID128 paneID, const Sync::ClipboardRead& request) override;
    /// Routes a validated command by pane identity.
    /// - Parameters:
    ///   - handle: The bound wire channel.
    ///   - paneID: The stable pane identity.
    ///   - command: The command, borrowed for this callback.
    void paneNativeDragCommand(std::uint32_t handle, Sync::ID128 paneID, const NativeDragCommand&) override;

    /// Notes that a workspace's structure arrived or was withdrawn, for a
    /// reader of the listing (`listingRevision`), and tells the observer.
    ///
    /// - Parameter workspaceID: The workspace.
    void workspaceStructureChanged(std::uint32_t workspaceID) override;

    Ubiquitty::Terminal* m_term;
    SyncClientObserver* m_observer;
    /// What this client's own policy says of an effect its host delivered.
    ///
    /// - Parameter capability: What the effect would do.
    /// - Returns: Allow unless the host is on another machine; otherwise the
    ///   client's current Remote-origin verdict, with a configured Deny
    ///   outranking any answer.
    [[nodiscard]] CapabilityDecision hostEffectDecision(Capability capability) const;
    /// A clipboard write's one representation, owned.
    struct HeldClipboardItem {
        std::string mime;
        std::vector<char> bytes;
    };
    /// A clipboard write held for the person, with every buffer it names.
    struct HeldClipboardWrite {
        std::bitset<std::to_underlying(ClipboardBuffer::Count)> buffers;
        std::vector<HeldClipboardItem> items;
        /// The generation and sequence of the last effect it holds, so only
        /// the effect right after it can join it.
        std::uint64_t generation = 0;
        std::uint64_t lastSequence = 0;
    };
    /// A notification held for the person, with its text owned.
    struct HeldNotification {
        std::string title;
        std::string body;
        std::string identifier;
        NotificationOccasion occasion = NotificationOccasion::Always;
    };
    /// A sound held for the person, with a sample's PCM copied out of the
    /// connection that lent it for one call.
    struct HeldSound {
        PAL::PlayTonesRequest request;
        std::vector<std::int16_t> samplePCM;
    };
    /// An attention request held for the person; it carries nothing but itself.
    struct HeldAttention { };
    /// An effect waiting for the person's answer.
    struct HeldHostEffect {
        std::uint64_t token = 0;
        /// When the first effect of its kind still waiting arrived; a newer one
        /// that replaces it keeps this, and with it its place in line.
        std::uint64_t order = 0;
        Capability capability = Capability::None;
        std::variant<HeldClipboardWrite, HeldNotification, HeldSound, HeldAttention> payload;
    };
    /// Holds a clipboard write for the person.
    ///
    /// A host delivers one write that names several buffers as one effect for
    /// each, with the same items, one after another in the same generation.
    /// Only such an effect joins the write waiting, under the question already
    /// asked; any other, a later write of the same text included, replaces it
    /// and is asked about.
    ///
    /// - Parameters:
    ///   - effect: The effect's record, for its generation and sequence.
    ///   - buffer: The buffer the effect writes.
    ///   - bundle: Its items, borrowed for this call.
    void holdClipboardWrite(const EffectView& effect, ClipboardBuffer buffer, const EffectClipboardBundle& bundle);
    /// Holds an effect for the person and tells the observer.
    ///
    /// A newer effect of a kind already waiting replaces it under a new token
    /// and in its place in line, and the one replaced is reported withdrawn
    /// after the new one is held.
    ///
    /// - Parameters:
    ///   - capability: What the effect would do.
    ///   - payload: The effect, owned.
    void holdHostEffect(Capability capability, decltype(HeldHostEffect::payload) payload);
    /// Applies a released effect.
    ///
    /// - Parameter held: The effect the person allowed.
    void applyHeldHostEffect(const HeldHostEffect& held);
    /// Forgets every answer and withdraws every held effect, since they
    /// belong to the connection that ended. Every slot is emptied before the
    /// observer hears of any, so a session closing a question finds nothing
    /// waiting behind it.
    void endHostEffectConnection();
    /// The kinds a host may deliver that this client holds: clipboard writes,
    /// notifications, sounds and attention requests, one of each.
    static constexpr std::size_t heldHostEffectKinds = 4;
    static constexpr std::size_t capabilityCount = std::to_underlying(Capability::None) + 1;
    HostLocation m_hostLocation = HostLocation::AnotherMachine;
    HostEffectDecisionSource m_hostEffectDecisions;
    /// The capabilities the person allowed until this connection ends.
    std::array<bool, capabilityCount> m_standingHostEffects {};
    std::uint64_t m_nextHeldEffectToken = 1;
    std::array<std::optional<HeldHostEffect>, heldHostEffectKinds> m_heldHostEffects {};
    ActiveObjectUse<Sync::ClientConnection>* m_activeFeedUse = nullptr;
    std::uint64_t m_decodeEpoch = 0;
    /// The screen plane of the connection, or null when none is greeted.
    [[nodiscard]] Sync::ClientConnection* liveScreen() const { return m_isScreenGreeted && m_attachment ? m_attachment->connection() : nullptr; }

    // The attachment every connection's screen plane continues; made by the
    // first buildHello and kept, so a reconnect resumes it.
    std::shared_ptr<Sync::ResumeRecordStore> m_resumeStore;
    std::unique_ptr<Sync::ClientAttachment> m_attachment;
    ActiveObjectUse<Sync::ClientAttachment>* m_activeAttachmentUse = nullptr;
    // Whether the attachment's connection is this connection's: false from a
    // lost transport until the next buildHello greets.
    bool m_isScreenGreeted = false;
    // Set while `restoreResumeStore` draws the stored pane into `m_term`.
    bool m_isRestoringIntoTerminal = false;
    Sync::ID128 m_clientInstance;
    Sync::ResumeCarrier m_screenResumeCarrier = Sync::ResumeCarrier::Secret;
    /// The table published before the screen plane existed.
    std::unordered_map<std::uint32_t, RichText::TypographyFacts> m_paneTypography;
    std::unique_ptr<const OSC::TextSizing::ProportionalFaceFacts> m_proportionalFaces;
    Clock m_clock;
    std::size_t m_screenDatagramMaxBytes = 0;
    std::uint64_t m_listingRevision = 0;
    // Until a frontend says otherwise a client is taken to be focused, which
    // keeps a frontend that cannot observe focus probed.
    Ubiquitty::Modes::FocusState m_screenFocus = Ubiquitty::Modes::FocusState::Focused;
    Sync::LinkCost m_linkCost = Sync::LinkCost::Unmetered;
    std::string m_title;
    std::string m_socketName;
    std::uint64_t m_daemonInstanceID { 0 };
    std::uint64_t m_expectedDaemonInstanceID = 0;
    // The carrier daemon and Sync host are independently minted identities.
    // Only a completed Welcome associates them; a provisional HelloAck must
    // not replace the predecessor used by another reconnect attempt.
    std::optional<std::uint64_t> m_admittedDaemonInstanceID;
    std::optional<std::uint64_t> m_resumeDaemonInstanceID;
    std::optional<Sync::ID128> m_resumeHostInstanceID;
    bool m_hasPendingHelloAck = false;
    bool m_hasRefusedWelcomeIdentity = false;
    /// The instance the admitted Welcome said its workspace was restored
    /// from, zero for any other admission.
    std::uint64_t m_restoredFromDaemonInstanceID = 0;
    std::unordered_map<std::uint32_t, Sync::ID128> m_sessionPaneIDs;
    uint32_t m_sessionID = 0;
    // Whether the HELLO asked for a workspace: a workspace carrier hands each
    // pane's frames to the observer, and a connection attached to one pane
    // reads its pane's as its own.
    bool m_isWorkspaceCarrier = false;
    /// Pane endings delivered after the decoder has released its borrowed state.
    std::vector<std::uint32_t> m_pendingPaneEnds;
    /// Failures queued until the frame is applied, by session.
    std::vector<std::pair<std::uint32_t, std::string>> m_pendingPaneFailures;
    bool m_hasReportedExit = false;
    /// Coalesced until the parser returns: a record observer may stop the
    /// connection, and may not run inside its record decoder. A status-only
    /// notification can precede the pane's first facts record.
    std::unordered_map<uint32_t, std::optional<Sync::PaneFacts>> m_pendingPaneFacts;
    // The views the owner set, by session, kept across a reconnect so a pane
    // the host binds again takes the view it had.
    std::unordered_map<uint32_t, Sync::ViewState> m_paneViews;
    // The focus the owner reported for each session's pane, kept across a
    // reconnect so a pane the host binds again is told again.
    std::unordered_map<uint32_t, Ubiquitty::Modes::FocusState> m_paneFocus;
    // The size the owner shows each session's pane at, kept across a
    // reconnect so a pane the host binds again is told again.
    std::unordered_map<uint32_t, Sync::Viewport> m_paneViewports;
    // The JSON-RPC id of the next call this client makes.
    std::uint64_t m_nextCallID = 1;
    bool m_sawHelloAck = false;
    // Whether a HELLO_ACK has ever named the daemon instance: a pane's
    // identity is the session and that instance, and input waits for no
    // connection once it is known.
    bool m_isDaemonInstanceKnown = false;
    // Whether the handshake refused this host, which ends the connection for
    // every frame after it. Leaving the session unestablished closes only the
    // send side, and a peer whose HELLO_ACK was rejected still frames a title, a
    // screen, images and samples — the refusal is about the peer,
    // not about the one frame that carried it. Cleared by resetForReconnect, so
    // a client that reconnects is not held to a verdict on a connection it no
    // longer has.
    bool m_didRefuseHost = false;
    uint32_t m_acknowledgedWorkspaceID { 0 };
    Protocol::Sync::HostBuildFlags m_hostBuildFlags {};
    Sync::PeerBuild m_hostBuild;
    std::string m_hostPlatform;
};

} // namespace Ubiquitty::Protocol::Sync
