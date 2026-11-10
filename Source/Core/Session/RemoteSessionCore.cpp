// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// RemoteSessionCore.cpp — see RemoteSessionCore.h.

#include "Core/Session/RemoteSessionCore.h"
#include "Core/Session/WorkspaceConnection.h"

#include "Core/Codec/JSON.h"
#include "Core/Crypto/SHA256.h"
#include "Core/OSC/TextSizing.h"
#include "Core/Panes/DividerPath.h"
#include "Core/Protocol/Control/Consent.h"
#include "Core/Protocol/Control/EventStream.h"
#include "Core/Protocol/Sync/AttachVerdictText.h"
#include "Core/Protocol/Sync/HistoryCache.h"

#include "Core/Protocol/Sync/NativeAttachment.h"
#include "Core/Protocol/Sync/Notify.h"
#include "Core/Protocol/Sync/ResumeProof.h"
#include "Core/Runtime/RemoteInput.h"
#include "Core/Session/DeployPolicy.h"
#include "Core/Session/HostListing.h"
#include "Core/Session/Identity.h"
#include "Core/Session/ProofOfLife.h"
#include "Core/Session/ReconnectBackoff.h"
#include "Core/Session/RemoteBootstrap.h"
#include "Core/Session/SessionTreeWire.h"
#include "Core/Session/WorkspaceWire.h"
#include "Core/TerminalOperations.h"
#include "Core/Transport/ConnectFailure.h"
#include "Core/Unicode/UTF8Repair.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>
#include <span>
#include <string>
#include <type_traits>
#include <utility>

namespace Ubiquitty::Session {

namespace {
// A suspension shorter than this leaves a QUIC connection worth keeping. The
// host's idle timer (90 s) counts from the last packet it heard, and with a
// 15 s keepalive that packet may already be that old when the suspension began.
constexpr double suspensionKeepsQUICSeconds = 60.0;

// How long a dial that presents a resume proof has to reach HELLO_ACK before
// the bootstrap is tried instead. A host that refuses a proof drops the
// connection without a word, and a path that carries no datagrams says as
// little, so the wait is what ends either. Provisional: no recorded trace of
// a proof dial on a slow link.
constexpr double resumeDialDeadlineSeconds = 2.0;

// The most bytes one `Raw` event carries. A longer run is sent as several, in
// order, each one frame.
constexpr std::size_t maxRawEventBytes = 16 * 1024;

// What a person is told when the daemon a client was attached to stopped and
// the host's current one holds no record of the workspace.
constexpr std::string_view restartedWithoutWorkspaceNotice = "Ubiquitty restarted on the host, and the saved workspace is no longer available.";

/// Names the local decoder's rejection without substituting a peer's text.
std::string readFailureNotice(const Protocol::Sync::SyncClientCore& core)
{
    const auto* screen = core.screen();
    const auto detail = screen ? screen->localCloseDetail() : std::string_view {};
    return detail.empty() ? "The connection ended on data that couldn’t be read." : "The connection ended: " + std::string(detail) + ".";
}

} // namespace

std::string reattachRefusedNotice(std::uint32_t sessionID, Protocol::Sync::ByeReason reason)
{
    return "The host refused to reattach session " + std::to_string(sessionID) + ". "
        + std::string(Protocol::Sync::byeNotice(reason));
}

RemoteSessionCore::RemoteSessionCore(Ubiquitty::Terminal* term, Dispatcher* dispatcher, ChannelFactory* factory, RemoteSessionObserver* observer)
    : m_term(term)
    , m_dispatcher(dispatcher)
    , m_factory(factory)
    , m_observer(observer)
    , m_core(std::make_unique<Protocol::Sync::SyncClientCore>(term, static_cast<Protocol::Sync::SyncClientObserver*>(this)))
{
    // Terminal::mirrorClient is not set here, though this object
    // holds both the grid and the client that mirrors it. It owns only one of
    // the two lifetimes: the Terminal is caller-owned, and callers destroy it in
    // either order. Registering makes both orders a fault — clearing on the way
    // out reads a Terminal the caller may already have freed, and not clearing
    // leaves the client destroyed while the grid still points at it. Whoever
    // owns both lifetimes registers; see Agent/AgentSession.cpp, which owns
    // the pair outright, as the web frontend's terminal does.
    if (term) {
        if (term->columns > 0)
            m_columns = term->columns;
        if (term->rows > 0)
            m_rows = term->rows;
    }
}

RemoteSessionCore::RemoteSessionCore(Ubiquitty::Terminal* term, WorkspaceConnection& workspace, RemoteSessionObserver* observer)
    : RemoteSessionCore(term, &workspace.dispatcher(), nullptr, observer)
{
    m_workspaceConnection = &workspace;
    // A pane reaches the host its workspace's carrier reaches. Its verdicts
    // are its own session's, which the session installs.
    setHostLocation(workspace.connection().m_hostLocation);
}

std::size_t RemoteSessionCore::resumedPaneCount(Protocol::Sync::ResumeOutcome outcome) const
{
    const Protocol::Sync::ClientConnection* screen = m_core ? m_core->screen() : nullptr;
    if (!screen || !screen->welcome())
        return 0;
    return static_cast<std::size_t>(std::ranges::count(screen->welcome()->resumed, outcome, &Protocol::Sync::PaneResumed::outcome));
}

bool RemoteSessionCore::adoptPreparedResume(std::unique_ptr<Protocol::Sync::SyncClientCore> prepared)
{
    if (!prepared || m_term || m_workspaceConnection || m_channel || m_state != State::Idle || !m_isMultiplexedWorkspace
        || !prepared->attachPreparedObserver(*this))
        return false;
    // The protected record permits attempting a proof, not authenticated
    // input. The factory must independently have the pinned cached reach;
    // the new transport and Welcome still establish every live authority.
    m_isAcknowledgedCarrierQUIC = prepared->heldAttachment().has_value();
    m_core = std::move(prepared);
    m_core->setHostLocation(m_hostLocation);
    m_core->setHostEffectDecisions(m_hostEffectDecisions);
    return true;
}

void RemoteSessionCore::setHostLocation(Protocol::Sync::HostLocation location)
{
    m_hostLocation = location;
    m_core->setHostLocation(location);
}

void RemoteSessionCore::setHostEffectDecisions(Protocol::Sync::SyncClientCore::HostEffectDecisionSource source)
{
    m_hostEffectDecisions = std::move(source);
    m_core->setHostEffectDecisions(m_hostEffectDecisions);
}

void RemoteSessionCore::establishWorkspacePane(uint32_t sessionID)
{
    m_core->establishWorkspacePane(sessionID, *m_workspaceConnection->connection().m_core);
    const std::uint64_t generation = screenOwner().m_generation.load(std::memory_order_acquire);
    auto weakThis = weakPtr();
    onHelloAck(sessionID, m_core->socketName());
    if (weakThis.get() && !m_stopped && m_ackSeen && m_sessionID == sessionID
        && screenOwner().m_generation.load(std::memory_order_acquire) == generation) {
        if (const auto* facts = m_workspaceConnection->connection().m_core->paneFacts(sessionID)) {
            // An observer can end the carrier; callbacks borrow this copy,
            // never the record whose connection they can release.
            const auto record = *facts;
            receiveWorkspacePaneFacts(record);
            if (weakThis.get() && !m_stopped && m_ackSeen && m_sessionID == sessionID
                && screenOwner().m_generation.load(std::memory_order_acquire) == generation)
                m_observer->onScreenChanged();
        }
    }
    if (!weakThis || m_stopped || !m_ackSeen || m_sessionID != sessionID
        || screenOwner().m_generation.load(std::memory_order_acquire) != generation)
        return;
    const auto& physical = m_workspaceConnection->connection();
    const auto* attachment = physical.m_core->attachment();
    const auto paneID = physical.m_core->boundPaneIdentity(sessionID);
    if (attachment && attachment->attachmentsRevision() && paneID) {
        // Copy the projection before presentation: the callback may replace
        // this endpoint or the physical attachment that owns the roster.
        const auto snapshot = presenceForPane(attachment->attachments(), *paneID, sessionID);
        m_observer->onPresence(snapshot);
    }
}

void RemoteSessionCore::receiveWorkspacePaneFacts(const Protocol::Sync::PaneFacts& facts)
{
    if (!m_stopped && m_ackSeen)
        m_core->receivePaneFacts(facts);
}

void RemoteSessionCore::receiveWorkspaceEffect(const Protocol::Sync::EffectView& effect, const Protocol::Sync::EffectBody& body, Protocol::Sync::EffectDisposition disposition)
{
    if (m_stopped || !m_ackSeen)
        return;
    // Both connections a pane moved between carried its effects; one the
    // connection it left already delivered is not delivered again.
    if (m_effectsReceivedBeforeMove && m_effectsReceivedBeforeMove->paneID == effect.paneID) {
        if (effect.sequence <= m_effectsReceivedBeforeMove->sequence)
            return;
        m_effectsReceivedBeforeMove.reset();
    }
    m_lastReceivedEffect = ReceivedEffect { .paneID = effect.paneID, .sequence = effect.sequence };
    m_core->receivePaneEffect(effect, body, disposition);
}

bool RemoteSessionCore::isSettledForMove() const
{
    if (m_stopped || !m_ackSeen || !m_sessionID || m_reclaimRequested || m_pendingFileDownload || m_pendingWindowClose
        || !m_structureCalls.empty() || m_core->oldestHeldHostEffect())
        return false;
    const RemoteSessionCore& owner = screenOwner();
    const auto isMine = [this](const WeakPtr<RemoteSessionCore>& endpoint) { return endpoint.get() == this; };
    if (std::ranges::any_of(owner.m_fileCalls, [&](const std::optional<FileCall>& call) { return call && isMine(call->endpoint); })
        || (owner.m_pasteCall && isMine(owner.m_pasteCall->endpoint))
        || std::ranges::any_of(owner.m_reclaimCalls, [&](const ReclaimCall& call) { return isMine(call.endpoint); })
        || (m_workspaceConnection && m_workspaceConnection->hasRequestsFor(*this)))
        return false;
    // Every event the pane sent here is written or refused before it sends
    // any on another connection, so its program reads them in order.
    return owner.m_core->paneInputOutstanding(m_sessionID) == 0;
}

void RemoteSessionCore::continueEndpoint(const RemoteSessionCore& predecessor)
{
    m_columns = predecessor.m_columns;
    m_rows = predecessor.m_rows;
    m_isSizeReported = predecessor.m_isSizeReported;
    m_pixelGeometry = predecessor.m_pixelGeometry;
    m_surfaceFocus = predecessor.m_surfaceFocus;
    m_view = predecessor.m_view;
    m_effectsReceivedBeforeMove = predecessor.m_lastReceivedEffect;
}

void RemoteSessionCore::restateView()
{
    if (m_view && m_ackSeen && !m_stopped)
        setView(*m_view);
}

bool RemoteSessionCore::receiveWorkspaceClipboardRequest(const Protocol::Sync::ClipboardRequestMessage& request)
{
    if (m_stopped || !m_ackSeen || !m_observer)
        return false;
    m_observer->onClipboardRequest(request);
    return true;
}

void RemoteSessionCore::workspaceConnectionChanged(State state, const std::string& detail)
{
    if (m_stopped) {
        return;
    }
    if (state != State::Connected) {
        m_pendingFileDownload.reset();
        m_pendingWindowClose.reset();
        m_structureCalls.clear();
        for (auto& file : m_fileCalls)
            file.reset();
        m_pasteCall.reset();
        m_isPasteTickPosted = false;
        if (m_fileReadGeneration != std::numeric_limits<std::uint64_t>::max())
            ++m_fileReadGeneration;
        m_workspaceStructureRevision = 0;
        m_workspaceStructureSnapshot.reset();
        m_ackSeen = false;
        m_core->resetForReconnect();
    }
    transition(state, detail);
}

const Layout::Workspace* RemoteSessionCore::workspace() const
{
    return m_workspaceConnection ? m_workspaceConnection->connection().workspace() : m_workspace.get();
}

uint32_t RemoteSessionCore::workspaceID() const
{
    return m_workspaceConnection ? m_workspaceConnection->connection().workspaceID() : m_workspaceID;
}

bool RemoteSessionCore::isUsingQUIC() const
{
    return m_workspaceConnection ? m_workspaceConnection->connection().isUsingQUIC() : m_transportIsQUIC;
}

const std::string& RemoteSessionCore::socketName() const
{
    return m_workspaceConnection ? m_workspaceConnection->connection().socketName() : m_core->socketName();
}

bool RemoteSessionCore::didClaimClipboardServe() const
{
    return m_workspaceConnection ? m_workspaceConnection->connection().didClaimClipboardServe() : m_didClaimClipboardServe;
}

bool RemoteSessionCore::isClipboardRoutingPending() const
{
    if (m_workspaceConnection) {
        return m_workspaceConnection->connection().isClipboardRoutingPending();
    }
    return m_ackSeen && (m_clipboardServing == Clipboard::Serving::Enabled) != m_didClaimClipboardServe;
}

bool RemoteSessionCore::queryTransportStats(Ubiquitty::Transport::TransportStats& out) const
{
    if (m_workspaceConnection) {
        return m_workspaceConnection->connection().queryTransportStats(out);
    }
    return m_channel && m_channel->queryStats(out);
}

RemoteSessionCore::~RemoteSessionCore()
{
    // The workspace finds this core's pane by its weak reference, so it is
    // told before the references are severed: a detach after them matched no
    // pane and left this core's terminal lent to the carrier, which read it
    // after it was freed. The detach runs synchronously and posts nothing.
    if (m_workspaceConnection) {
        m_workspaceConnection->detach(*this);
    }
    // Severed next: a closure the dispatcher already holds cannot be recalled,
    // and reading this object as gone is the only thing that stops it.
    revokeWeakPtrs();
    teardownChannel(); // joins the channel's I/O thread before our fields tear down
}

// ---- public API -------------------------------------------------------

void RemoteSessionCore::start(uint32_t attachSessionID, int columns, int rows)
{
    if (m_workspaceConnection) {
        if (!m_stopped && !m_ackSeen) {
            m_sessionID = attachSessionID;
            m_columns = columns;
            m_rows = rows;
            if (!m_workspaceConnection->attach(*this, attachSessionID)) {
                transition(State::Failed, "The session does not belong to this workspace.");
            }
        }
        return;
    }
    if (m_stopped || m_channel || m_deployPending)
        return; // already started / connecting / installing
    m_channelCreationFailures = 0;
    m_initialSetupFailures = 0;
    m_initialSetupBudgetExhausted = false;
    m_sessionID = attachSessionID;
    if (columns > 0)
        m_columns = columns;
    if (rows > 0)
        m_rows = rows;
    if (m_updateBeforeConnectPlatform) {
        const std::string platformArch = std::move(*m_updateBeforeConnectPlatform);
        m_updateBeforeConnectPlatform.reset();
        m_deployedOnce = true;
        m_deployPending = true;
        transition(State::Deploying, platformArch);
        // A turn later, so the embedder that called start has armed the
        // install the request reaches.
        m_dispatcher->post([weakThis = weakPtr(), platformArch = std::string(platformArch)] {
            auto* session = weakThis.get();
            if (!session || session->m_stopped || !session->m_deployPending)
                return;
            session->m_observer->onDeployRequested(platformArch);
        });
        return;
    }
    connect();
}

void RemoteSessionCore::stop() { stopWithReason("detached"); }

void RemoteSessionCore::stopWithReason(const std::string& reason)
{
    m_directoryListing.reset();
    m_sessionListRequestID = 0;
    m_pendingFileDownload.reset();
    m_pendingWindowClose.reset();
    m_structureCalls.clear();
    for (auto& file : m_fileCalls)
        file.reset();
    m_pasteCall.reset();
    m_isPasteTickPosted = false;
    if (m_fileReadGeneration != std::numeric_limits<std::uint64_t>::max())
        ++m_fileReadGeneration;
    m_workspaceStructureRevision = 0;
    m_workspaceStructureSnapshot.reset();
    if (m_workspaceConnection) {
        m_stopped = true;
        m_ackSeen = false;
        m_workspaceConnection->detach(*this);
        return;
    }
    if (m_stopped)
        return;
    m_stopped = true;
    // Best-effort clean detach so the daemon keeps the session alive (it treats
    // a dropped transport like EXIT, so a non-flushed write is harmless).
    if (m_ackSeen && m_channel) {
        std::vector<uint8_t> frame;
        m_core->buildExit(frame);
        writeFrame(frame);
    }
    teardownChannel();
    transition(State::Failed, reason);
}

void RemoteSessionCore::refuseAcknowledgedHost(const std::string& reason)
{
    m_core->refuseHost();
    if (m_workspaceConnection) {
        stopWithReason(reason);
        transition(State::Failed, reason);
        return;
    }
    stopWithReason(reason);
}

RemoteSessionCore& RemoteSessionCore::screenOwner()
{
    return m_workspaceConnection ? m_workspaceConnection->connection() : *this;
}

const RemoteSessionCore& RemoteSessionCore::screenOwner() const
{
    return m_workspaceConnection ? m_workspaceConnection->connection() : *this;
}

bool RemoteSessionCore::isGeometryOwnedElsewhere() const
{
    const auto& owner = screenOwner();
    if (m_stopped || !m_ackSeen || owner.m_stopped || !owner.m_ackSeen || owner.m_state != State::Connected)
        return false;
    return owner.m_core->isGeometryOwnedElsewhere(m_sessionID);
}

bool RemoteSessionCore::send(Protocol::Sync::InputIntent intent)
{
    if (m_stopped || m_sessionID == 0)
        return false;
    // The attachment numbers the event and sends it, or keeps it for the
    // connection that continues the session; the Ack the screen plane owes
    // leaves with it (Docs/sync.md §6.4). The plane lives on the workspace's
    // carrier when the pane rides one.
    RemoteSessionCore& owner = screenOwner();
    const bool isTaken = owner.m_core->sendIntent(m_sessionID, std::move(intent)).has_value();
    if (isTaken)
        m_trace.recordInputSent();
    if (isTaken && m_workspaceConnection)
        m_workspaceConnection->noteInput(*this);
    if (owner.m_channel)
        owner.flushScreen();
    return isTaken;
}

bool RemoteSessionCore::send(const PaneInput& input)
{
    return std::visit([&](const auto& intent) -> bool {
        using Intent = std::decay_t<decltype(intent)>;
        if constexpr (std::is_same_v<Intent, PaneInput::Key>)
            return send(Protocol::Sync::KeyInput { .events = { Runtime::keyEventOf(intent.press) } });
        else if constexpr (std::is_same_v<Intent, PaneInput::Paste>)
            return send(Protocol::Sync::PasteInput { .text = { intent.text.begin(), intent.text.end() }, .offset = 0, .end = Protocol::Sync::PasteEnd::Final });
        else if constexpr (std::is_same_v<Intent, PaneInput::Pointer>)
            return send(Protocol::Sync::PointerInput { .events = { intent.event } });
        else {
            static_assert(std::is_same_v<Intent, PaneInput::Raw>);
            auto bytes = input.localEncoding;
            // Raw input that writes nothing is refused (`PaneLink::send`).
            if (bytes.empty())
                return false;
            const auto weak = weakPtr();
            while (!bytes.empty()) {
                const auto piece = bytes.first(std::min(bytes.size(), maxRawEventBytes));
                if (!send(Protocol::Sync::RawInput { .bytes = { piece.begin(), piece.end() } }) || !weak)
                    return false;
                bytes = bytes.subspan(piece.size());
            }
            return true;
        }
    },
        input.intent);
}

std::optional<std::uint8_t> RemoteSessionCore::lineEraseCharacter() const
{
    if (m_stopped || !m_sessionID)
        return 0x7f;
    const RemoteSessionCore& owner = screenOwner();
    const auto* screen = owner.m_core->screen();
    const auto context = screen ? screen->echoContext(owner.m_core->paneHandle(m_sessionID)) : std::nullopt;
    return context ? context->eraseCharacter : std::optional<std::uint8_t> { 0x7f };
}

PaneView RemoteSessionCore::view()
{
    return viewOnSessionThread();
}

PaneView RemoteSessionCore::viewOnSessionThread()
{
    if (m_stopped || !m_sessionID)
        return { .terminal = m_term };
    // The terminal outlives a carrier. History and records borrow the live
    // physical owner's exact binding and never the pane decoder's private
    // screen plane, which is not the workspace's replica owner.
    const RemoteSessionCore& owner = screenOwner();
    const auto* screen = owner.m_core->screen();
    const std::uint32_t handle = owner.m_core->paneHandle(m_sessionID);
    return { .terminal = m_term,
        .history = screen ? screen->historyCache(handle) : nullptr,
        .facts = owner.m_core->paneFacts(m_sessionID) };
}

bool RemoteSessionCore::wantHistory(Protocol::Sync::HistoryPriority priority, std::span<const Protocol::Sync::LineRange> ranges)
{
    if (m_stopped || !m_ackSeen || !m_sessionID)
        return false;
    RemoteSessionCore& owner = screenOwner();
    const bool accepted = owner.m_core->wantPaneHistory(m_sessionID, priority, ranges);
    if (accepted && owner.m_channel)
        owner.flushScreen();
    return accepted;
}

namespace {

/// The topology of every tab a workspace structure lays out, as one tree
/// whose root holds each tab's split: a client draws every pane of its
/// workspace, and a pane's leaf is found under whichever tab holds it.
///
/// - Parameter structure: The decoded structure.
/// - Returns: The tree, at the structure's revision.
Protocol::Sync::TopologyTree topologyOf(const TreeWorkspace& structure)
{
    Protocol::Sync::TopologyTree tree { .revision = structure.revision, .root = {}, .zoomedPane = std::nullopt };
    for (const TreeWindow& window : structure.windows) {
        for (const TreeTab& tab : window.tabs)
            tree.root.children.push_back(tab.split);
    }
    return tree;
}

/// The link cost a connection states for a network path's.
///
/// - Parameter cost: What the platform says the path costs.
/// - Returns: The `Facts.link` it is.
Protocol::Sync::LinkCost linkCostFor(PAL::NetworkCost cost)
{
    switch (cost) {
    case PAL::NetworkCost::Expensive:
        return Protocol::Sync::LinkCost::Metered;
    case PAL::NetworkCost::Constrained:
        return Protocol::Sync::LinkCost::Constrained;
    case PAL::NetworkCost::Unmetered:
        break;
    }
    return Protocol::Sync::LinkCost::Unmetered;
}

} // namespace

void RemoteSessionCore::setViewOffset(int rowsAboveTail)
{
    // Read on the session thread, the only writer of the replica and the
    // cache, so neither moves under the decision.
    if (m_stopped || !m_ackSeen || !m_sessionID || !m_term)
        return;
    const PaneView pane = viewOnSessionThread();
    if (!pane.history)
        return;
    const std::optional<Protocol::Sync::HistoryPull::Want> want = m_historyPull.wantFor(*pane.history, *m_term, rowsAboveTail);
    if (want && wantHistory(Protocol::Sync::HistoryPriority::Viewport, want->gaps))
        m_historyPull.noteSent(*want);
}

void RemoteSessionCore::setNetworkCost(PAL::NetworkCost cost)
{
    if (m_workspaceConnection) {
        m_workspaceConnection->connection().setNetworkCost(cost);
        return;
    }
    m_core->setLinkCost(linkCostFor(cost));
    if (m_channel)
        flushScreen();
}

std::optional<Protocol::Sync::PaneRectangle> RemoteSessionCore::paneRectangle() const
{
    if (m_stopped || !m_sessionID)
        return std::nullopt;
    return screenOwner().m_core->paneRectangle(m_sessionID);
}

LinkState RemoteSessionCore::linkState(std::uint64_t now) const
{
    const RemoteSessionCore& owner = screenOwner();
    LinkPhase phase = LinkPhase::Connecting;
    if (m_stopped || m_hostExited || m_state == State::Failed)
        phase = LinkPhase::Ended;
    else if (owner.m_state == State::Reconnecting)
        phase = LinkPhase::Lost;
    else if (m_ackSeen && owner.m_state == State::Connected)
        phase = LinkPhase::Live;
    const auto* attachment = owner.m_core->attachment();
    return { .phase = phase,
        .transport = owner.m_core->screen() && attachment ? attachment->linkState(now) : Protocol::Sync::LinkState {} };
}

std::uint64_t RemoteSessionCore::clockMicroseconds() const
{
    return screenOwner().m_core->screenClockMicroseconds();
}

void RemoteSessionCore::pollEvents(std::uint64_t now)
{
    if (!m_linkEvents.hasObserver())
        return;
    m_linkEvents.noteState(linkState(now));
    // Frames are told only to a Live view (`PaneLink::pollEvents`). A view
    // not yet set is Live: a person's client shows a pane it is bound to
    // unless its owner says otherwise.
    const bool isLive = m_view.value_or(Protocol::Sync::ViewState::Live) == Protocol::Sync::ViewState::Live;
    if (!m_stopped && isLive && m_restoredPreview)
        m_linkEvents.noteFrame(*m_restoredPreview);
    if (!m_stopped && m_ackSeen && m_view != Protocol::Sync::ViewState::Dormant) {
        const RemoteSessionCore& owner = screenOwner();
        if (const auto* screen = owner.m_core->screen()) {
            const std::uint32_t handle = owner.m_core->paneHandle(m_sessionID);
            if (const auto frameClass = screen->paneFrameClass(handle); frameClass && isLive) {
                m_restoredPreview.reset();
                m_linkEvents.noteFrame({ .frameClass = *frameClass,
                    .generation = screen->paneGeneration(handle),
                    .lagMicroseconds = screen->paneLagMicroseconds(handle) });
            }
            const auto* facts = owner.m_core->paneFacts(m_sessionID);
            if (facts && m_linkFactsRevision != facts->revision) {
                m_linkFactsRevision = facts->revision;
                m_linkEvents.noteFacts();
            }
        }
    }
    // Delivery is the final operation: an observer may release this pane.
    m_linkEvents.deliver();
}

void RemoteSessionCore::sendInput(std::string_view bytes)
{
    (void)send(PaneInput { .intent = PaneInput::Raw {}, .localEncoding = bytes });
}

Protocol::Sync::PaneInputStatus RemoteSessionCore::inputStatus() const
{
    return m_sessionID == 0 ? Protocol::Sync::PaneInputStatus {} : screenOwner().m_core->paneInputStatus(m_sessionID);
}

void RemoteSessionCore::resendInDoubt()
{
    RemoteSessionCore& owner = screenOwner();
    owner.m_core->resendInDoubt(m_sessionID);
    if (owner.m_channel)
        owner.flushScreen();
    reportInputStatus();
}

void RemoteSessionCore::discardInDoubt()
{
    RemoteSessionCore& owner = screenOwner();
    owner.m_core->discardInDoubt(m_sessionID);
    if (owner.m_channel)
        owner.flushScreen();
    reportInputStatus();
}

void RemoteSessionCore::reportInputStatus()
{
    const Protocol::Sync::PaneInputStatus status = inputStatus();
    if (status.inDoubt == m_reportedInDoubt && status.discarded == m_reportedDiscarded)
        return;
    m_reportedInDoubt = status.inDoubt;
    m_reportedDiscarded = status.discarded;
    m_observer->onInDoubt(status);
}

void RemoteSessionCore::publishTypography(const RichText::TypographyFacts& facts)
{
    auto& owner = screenOwner();
    owner.m_core->publishTypography(m_sessionID, facts);
    owner.flushScreen();
}

void RemoteSessionCore::publishProportionalFaces(const OSC::TextSizing::ProportionalFaceFacts& faces)
{
    if (!mayPublishProportionalFaces())
        return;
    auto& owner = screenOwner();
    owner.m_core->publishProportionalFaces(faces);
    owner.flushScreen();
}

bool RemoteSessionCore::mayPublishProportionalFaces() const
{
    if (!m_term || !OSC::TextSizing::mayOfferFaceFacts(*m_term) || m_stopped || !m_ackSeen || m_state != State::Connected)
        return false;
    const auto* screen = screenOwner().m_core->screen();
    if (!screen || !screen->welcome())
        return false;
    const auto role = screen->welcome()->grantedRole;
    return role == Protocol::Sync::Role::Owner || role == Protocol::Sync::Role::Driver;
}

bool RemoteSessionCore::canChangeClipboardServing() const
{
    if (m_workspaceConnection) {
        return m_workspaceConnection->connection().canChangeClipboardServing();
    }
    return m_ackSeen && !m_stopped && m_core->isScreenAttached();
}

bool RemoteSessionCore::setClipboardServing(Clipboard::Serving serving)
{
    if (m_workspaceConnection) {
        return m_workspaceConnection->connection().setClipboardServing(serving);
    }
    const bool enabled = serving == Clipboard::Serving::Enabled;
    if (serving == m_clipboardServing)
        return true;
    if (!enabled)
        m_didClaimClipboardServe = false;
    if (m_ackSeen) {
        const auto requestID = canChangeClipboardServing() ? m_core->setClipboardServing(serving) : std::nullopt;
        if (!requestID) {
            if (!enabled)
                m_clipboardServing = serving;
            return false;
        }
        m_clipboardServingCallID = *requestID;
    }
    m_clipboardServing = serving;
    if (m_ackSeen)
        flushScreen();
    return true;
}

bool RemoteSessionCore::sendPalette(const TermPalette& palette, const TermPalette& baseline)
{
    if (!m_ackSeen || m_stopped)
        return false;
    auto& owner = screenOwner();
    owner.m_core->setThemePalette(palette, baseline);
    owner.flushScreen();
    return true;
}

bool RemoteSessionCore::sendClearBuffer(Ubiquitty::Screen::ClearCommand command)
{
    if (!m_ackSeen || m_stopped)
        return false;
    auto& owner = screenOwner();
    if (!owner.m_core->clearPane(m_sessionID, command))
        return false;
    owner.flushScreen();
    return true;
}

void RemoteSessionCore::setSize(int columns, int rows)
{
    if (columns < 1 || rows < 1)
        return;
    // The grid this client's drawable holds, kept as its own: what the host
    // sizes the pane to is the host's decision and arrives in its frames, and
    // neither overwrites the other.
    m_columns = columns;
    m_rows = rows;
    m_isSizeReported = true;
    stateViewport();
}

void RemoteSessionCore::setPixelGeometry(const PixelGeometry& geometry)
{
    const auto pixels = [](int extent) { return static_cast<std::uint16_t>(std::clamp(extent, 0, 65535)); };
    m_pixelGeometry = std::array<std::uint16_t, 6> { pixels(geometry.cell.width), pixels(geometry.cell.height), pixels(geometry.textArea.width),
        pixels(geometry.textArea.height), pixels(geometry.drawable.width), pixels(geometry.drawable.height) };
}

void RemoteSessionCore::stateViewport()
{
    // Before the acknowledgement the host has not said which session this
    // is; the acknowledgement states the size then held. A workspace's
    // carrier shows no pane of its own, so it has no size to state.
    if (!m_ackSeen || m_isMultiplexedWorkspace)
        return;
    // A pane whose frontend has reported no grid and published no pixels is
    // not laid out yet: the grid held is the one it was launched with, which
    // nobody's drawable holds, and stating it would size the host's pane to
    // it until the layout arrived.
    if (!m_isSizeReported && !m_pixelGeometry)
        return;
    sendViewport(m_columns, m_rows);
}

void RemoteSessionCore::sendViewport(int columns, int rows)
{
    if (m_sessionID == 0)
        return;
    const auto pixels = m_pixelGeometry.value_or(std::array<std::uint16_t, 6> {});
    const Protocol::Sync::Viewport viewport { .rows = static_cast<std::uint16_t>(rows), .columns = static_cast<std::uint16_t>(columns), .cellPixelWidth = pixels[0], .cellPixelHeight = pixels[1], .textAreaPixelWidth = pixels[2], .textAreaPixelHeight = pixels[3], .drawablePixelWidth = pixels[4], .drawablePixelHeight = pixels[5] };
    // The plane lives on the workspace's carrier when the pane rides one.
    RemoteSessionCore& owner = screenOwner();
    // A session with a carrier of its own is all its window shows, so the
    // window's size is the pane's; a pane laid out in a workspace is one of
    // several, and the window's size is not this pane's to say.
    if (!m_workspaceConnection)
        m_core->setWindowViewport(viewport);
    if (owner.m_core->setPaneViewport(m_sessionID, viewport))
        m_trace.recordViewportStated();
    if (owner.m_channel)
        owner.flushScreen();
}

bool RemoteSessionCore::requestReclaim()
{
    const auto endpoint = weakPtr();
    const auto owner = screenOwner().weakPtr();
    if (!m_sessionID || !m_ackSeen || m_stopped || m_reclaimRequested || !owner->m_ackSeen || owner->m_stopped
        || !owner->m_channel || !owner->m_core->isScreenAttached() || owner->m_reclaimCalls.size() >= maxPendingReclaimCalls
        || owner->m_nextReclaimCallID == UINT64_MAX)
        return false;
    const auto* screen = owner->m_core->screen();
    const auto& welcome = screen->welcome();
    const auto handle = owner->m_core->paneHandle(m_sessionID);
    const auto paneID = owner->m_core->boundPaneIdentity(m_sessionID);
    if (!welcome || !handle || !paneID || *paneID == Protocol::Sync::ID128 {})
        return false;
    const auto generation = owner->m_generation.load(std::memory_order_acquire);
    const auto endpointGeneration = m_generation.load(std::memory_order_acquire);
    const Protocol::Control::ReclaimTarget target { .handle = handle, .sessionID = m_sessionID, .daemonInstanceID = owner->m_daemonInstanceID, .hostInstanceID = welcome->hostInstanceID, .attachmentID = welcome->attachmentID, .paneID = *paneID };
    const auto correlation = "reclaim:" + std::to_string(++owner->m_nextReclaimCallID);
    auto request = Protocol::Control::reclaimRequest({ .handle = handle, .daemonInstanceID = target.daemonInstanceID }, correlation);
    if (request.empty())
        return false;
    // The host refuses a second reclaim from an attachment with one awaiting
    // release: this one waits, and goes once the one out is answered.
    if (!owner->m_reclaimCalls.empty()) {
        owner->m_reclaimCalls.push_back(ReclaimCall { .endpoint = endpoint,
            .generation = generation,
            .endpointGeneration = endpointGeneration,
            .correlation = correlation,
            .target = target,
            .request = std::move(request),
            .isSent = false });
        m_reclaimRequested = true;
        return true;
    }
    if (!owner->m_core->call(request))
        return false;
    owner->m_reclaimCalls.push_back(ReclaimCall { .endpoint = endpoint,
        .generation = generation,
        .endpointGeneration = endpointGeneration,
        .correlation = correlation,
        .target = target,
        .request = {},
        .isSent = true });
    m_reclaimRequested = true;
    owner->flushScreen();
    return endpoint && owner && !endpoint->m_stopped && !owner->m_stopped
        && endpoint->m_generation.load(std::memory_order_acquire) == endpointGeneration
        && owner->m_generation.load(std::memory_order_acquire) == generation;
}

bool RemoteSessionCore::publishSharedWorkspace(const SharedWorkspace& workspace)
{
    RemoteSessionCore& owner = screenOwner();
    if (!m_ackSeen || m_stopped || !owner.m_ackSeen || owner.m_stopped || !owner.m_channel || !owner.m_core->isScreenAttached()
        || owner.m_sharedWorkspaceResult.isPending || workspace.sessions.empty()
        || std::ranges::find(workspace.sessions, m_sessionID) == workspace.sessions.end())
        return false;
    const auto correlation = "ubiquitty.share." + std::to_string(workspace.sessions.front());
    const auto request = workspace.request(owner.m_daemonInstanceID, correlation);
    if (request.empty() || !owner.m_core->call(request))
        return false;
    owner.m_sharedWorkspaceCorrelation = correlation;
    owner.m_sharedWorkspaceResult = {};
    owner.m_sharedWorkspaceResult.isPending = true;
    owner.flushScreen();
    return true;
}

bool RemoteSessionCore::consumeControlResult(std::string_view text)
{
    if (m_sharedWorkspaceResult.isPending && text.size() <= Protocol::Sync::maxFrameBodyBytes) {
        bool isJSON = false;
        const auto answer = Codec::JSON::parse(std::string(text), isJSON);
        if (isJSON && answer.stringOr("id") == m_sharedWorkspaceCorrelation) {
            m_sharedWorkspaceResult = {};
            const auto* result = answer.find("result");
            const auto* error = answer.find("error");
            const double workspace = result ? result->numberOr("workspace") : 0;
            if (!error && workspace >= 1 && workspace <= UINT32_MAX && workspace == static_cast<std::uint32_t>(workspace))
                m_sharedWorkspaceResult.workspaceID = static_cast<std::uint32_t>(workspace);
            else
                m_sharedWorkspaceResult.detail = error ? error->stringOr("message") : "The host did not confirm the shared workspace.";
            m_observer->onSharedWorkspaceChanged();
            return true;
        }
    }
    if (m_reclaimCalls.empty() || text.size() > Protocol::Sync::maxFrameBodyBytes)
        return false;
    bool isJSON = false;
    const auto answer = Codec::JSON::parse(std::string(text), isJSON);
    if (!isJSON)
        return false;
    const std::string id = answer.stringOr("id");
    const auto found = std::ranges::find_if(m_reclaimCalls, [&](const ReclaimCall& call) { return call.isSent && call.correlation == id; });
    if (found == m_reclaimCalls.end())
        return false;
    auto held = std::move(*found);
    m_reclaimCalls.erase(found);
    auto* endpoint = held.endpoint.get();
    if (!endpoint)
        return true;
    endpoint->m_reclaimRequested = false;
    const auto* screen = m_core->screen();
    if (!screen || !screen->welcome() || !m_ackSeen || m_stopped || endpoint->m_stopped || !endpoint->m_ackSeen
        || m_generation.load(std::memory_order_acquire) != held.generation
        || endpoint->m_generation.load(std::memory_order_acquire) != held.endpointGeneration
        || endpoint->m_sessionID != held.target.sessionID || m_daemonInstanceID != held.target.daemonInstanceID
        || screen->welcome()->hostInstanceID != held.target.hostInstanceID || screen->welcome()->attachmentID != held.target.attachmentID
        || !Protocol::Control::isReclaimReleasedResult(answer, held.correlation, held.target))
        return true;
    if (endpoint->m_workspaceConnection)
        endpoint->m_workspaceConnection->noteReclaimed(*endpoint);
    endpoint->finishReclaim();
    return true;
}

void RemoteSessionCore::serviceReclaims()
{
    while (!m_reclaimCalls.empty() && !m_reclaimCalls.front().isSent) {
        ReclaimCall& next = m_reclaimCalls.front();
        RemoteSessionCore* endpoint = next.endpoint.get();
        const bool isCurrent = endpoint && !endpoint->m_stopped && endpoint->m_ackSeen && endpoint->m_sessionID == next.target.sessionID
            && endpoint->m_generation.load(std::memory_order_acquire) == next.endpointGeneration
            && m_generation.load(std::memory_order_acquire) == next.generation && m_ackSeen && !m_stopped && m_channel
            && m_core->isScreenAttached();
        if (isCurrent && m_core->call(next.request)) {
            next.isSent = true;
            next.request.clear();
            return;
        }
        if (endpoint)
            endpoint->m_reclaimRequested = false;
        m_reclaimCalls.erase(m_reclaimCalls.begin());
    }
}

void RemoteSessionCore::revokeConsent(uint8_t action, const std::string& attachmentID, const std::string& identity)
{
    RemoteSessionCore& owner = screenOwner();
    if (!m_ackSeen || m_sessionID == 0 || owner.m_stopped || !owner.m_core->isScreenAttached()
        || owner.m_nextConsentRevocationID == UINT64_MAX)
        return;
    Protocol::Control::ConsentRevocation value {
        .sessionID = m_sessionID,
        .action = static_cast<Protocol::Sync::ConsentRevocationKind>(action),
        .attachmentID = {},
        .identity = identity,
    };
    if (value.action == Protocol::Sync::ConsentRevocationKind::DisconnectNow) {
        const auto parsed = Protocol::Sync::parseID128(attachmentID);
        if (!parsed)
            return;
        value.attachmentID = *parsed;
    } else if (!attachmentID.empty()) {
        return;
    }
    const auto request = Protocol::Control::consentRevocationRequest(value, "consent-revoke:" + std::to_string(++owner.m_nextConsentRevocationID));
    if (!request.empty() && owner.m_core->call(request))
        owner.flushScreen();
}

void RemoteSessionCore::respondToConsent(uint32_t requestID, Protocol::Sync::ConsentKind kind, uint8_t decision)
{
    RemoteSessionCore& owner = screenOwner();
    if (!owner.m_ackSeen || owner.m_stopped || !owner.m_core->isScreenAttached() || requestID == 0)
        return;
    const Protocol::Control::ConsentResponse response { .requestID = requestID, .kind = kind, .decision = static_cast<Protocol::Sync::ConsentChoice>(decision) };
    const auto call = Protocol::Control::consentResponseRequest(response, "consent:" + std::to_string(std::to_underlying(kind)) + ":" + std::to_string(requestID));
    if (!call.empty() && owner.m_core->call(call))
        owner.flushScreen();
}

bool RemoteSessionCore::respondToStructureConsent(std::uint32_t requestID, bool allow) // bool-ok: maps the local presentation choice to canonical AllowOnce or Deny
{
    if (m_workspaceConnection)
        return m_workspaceConnection->connection().respondToStructureConsent(requestID, allow);
    if (!m_ackSeen || !m_core->isScreenAttached() || requestID == 0)
        return false;
    const Protocol::Control::ConsentResponse response { .requestID = requestID, .kind = Protocol::Sync::ConsentKind::Structure, .decision = allow ? Protocol::Sync::ConsentChoice::AllowOnce : Protocol::Sync::ConsentChoice::Deny };
    const auto call = Protocol::Control::consentResponseRequest(response, "consent:structure:" + std::to_string(requestID));
    if (call.empty() || !m_core->call(call))
        return false;
    flushScreen();
    return true;
}

uint32_t RemoteSessionCore::pushPaste(const std::string& mime, std::span<const std::uint8_t> bytes)
{
    if (!m_ackSeen || m_stopped || bytes.empty() || bytes.size() > Protocol::Control::maxPastePushBytes
        || m_nextFileRequestID == std::numeric_limits<std::uint32_t>::max())
        return 0;
    const auto endpoint = weakPtr();
    const auto owner = screenOwner().weakPtr();
    if (!owner->m_ackSeen || owner->m_stopped || !owner->m_channel || !owner->m_core->isScreenAttached()
        || owner->m_pasteCall || owner->m_nextPasteCallID == std::numeric_limits<std::uint64_t>::max())
        return 0;
    const auto pane = owner->m_core->paneHandle(m_sessionID);
    if (!pane)
        return 0;
    Protocol::Control::PasteUploadSource source;
    const auto correlation = "paste:" + std::to_string(++owner->m_nextPasteCallID);
    if (!source.begin(pane, mime, correlation, bytes, [](std::size_t amount) {
            // One physical owner reserves the payload and all bounded retained
            // metadata. Carrier queues keep their independent existing cap;
            // chunks ask their current headroom before any frame is materialized.
            return amount <= Protocol::Control::maxPastePushBytes + Protocol::Control::PasteUploadSource::metadataReservationBytes;
        }))
        return 0;
    const auto generation = owner->m_generation.load(std::memory_order_acquire);
    const auto fileGeneration = m_fileReadGeneration;
    const auto sessionID = m_sessionID;
    const auto pushID = ++m_nextFileRequestID;
    if (!owner->m_core->call(source.call()))
        return 0;
    owner->m_pasteCall.emplace(PasteCall { endpoint, fileGeneration, sessionID, pane, pushID, std::move(source) });
    owner->flushScreen();
    if (!endpoint || !owner || endpoint->m_stopped || !endpoint->m_ackSeen || endpoint->m_sessionID != sessionID
        || endpoint->m_fileReadGeneration != fileGeneration || owner->m_generation.load(std::memory_order_acquire) != generation)
        return 0;
    return pushID;
}

void RemoteSessionCore::serveClipboard(std::uint32_t requestID, Protocol::Sync::ClipboardServeStatus status,
    std::vector<Protocol::Sync::ClipboardServeItem> items)
{
    if (!m_ackSeen)
        return;
    const Protocol::Sync::ClipboardDataMessage message
        = Protocol::Sync::makeClipboardData(requestID, status, std::move(items));
    RemoteSessionCore& owner = screenOwner();
    const auto weakOwner = owner.weakPtr();
    std::vector<uint8_t> frame;
    owner.m_core->buildClipboardData(message, frame);
    owner.writeFrame(frame);
    if (RemoteSessionCore* current = weakOwner.get())
        current->flushScreen();
}

double RemoteSessionCore::pastePushProgress() const
{
    const auto& owner = screenOwner();
    if (!owner.m_pasteCall || owner.m_pasteCall->endpoint.get() != this || !owner.m_channel)
        return -1;
    const std::int64_t pending = owner.m_channel->pendingOutboundBytes();
    if (pending < 0)
        return -1;
    const auto queued = owner.m_pasteCall->source.queuedBytes();
    const auto sent = static_cast<std::uint64_t>(pending) >= queued ? 0 : queued - static_cast<std::size_t>(pending);
    return static_cast<double>(sent) / static_cast<double>(owner.m_pasteCall->source.heldBytes());
}

void RemoteSessionCore::receivePasteReady(std::span<const std::uint8_t> record)
{
    if (!m_pasteCall)
        return;
    const auto ready = Protocol::Control::decodePasteReady(record);
    if (!ready)
        return;
    auto& held = *m_pasteCall;
    auto* endpoint = held.endpoint.get();
    if (!endpoint || endpoint->m_stopped || !endpoint->m_ackSeen || endpoint->m_sessionID != held.sessionID
        || endpoint->m_fileReadGeneration != held.generation || m_core->paneHandle(held.sessionID) != held.pane) {
        m_pasteCall.reset();
        return;
    }
    if (held.source.credit(*ready) != Protocol::Control::PasteCreditDisposition::Refused)
        return;
    const auto pushID = held.pushID;
    m_pasteCall.reset();
    const Protocol::Control::PastePushReply refused { .pushID = pushID, .status = Protocol::Control::PastePushStatus::IOError };
    endpoint->m_observer->onPastePushAck(refused);
}

void RemoteSessionCore::receivePasteAnswer(std::string_view text)
{
    if (!m_pasteCall)
        return;
    bool isJSON = false;
    const auto answer = Codec::JSON::parse(std::string(text), isJSON);
    if (!isJSON)
        return;
    auto decoded = Protocol::Control::decodePastePushReply(answer, m_pasteCall->source.requestID(), m_pasteCall->pane);
    if (!decoded && decoded.error() == Protocol::Control::PasteReplyFailure::Unrelated)
        return;
    PasteCall held = std::move(*m_pasteCall);
    m_pasteCall.reset();
    auto* endpoint = held.endpoint.get();
    if (!endpoint || endpoint->m_stopped || !endpoint->m_ackSeen || endpoint->m_sessionID != held.sessionID
        || endpoint->m_fileReadGeneration != held.generation || m_core->paneHandle(held.sessionID) != held.pane)
        return;
    Protocol::Control::PastePushReply reply { .pushID = held.pushID, .status = Protocol::Control::PastePushStatus::IOError };
    if (decoded) {
        reply.status = decoded->status;
        reply.path = std::move(decoded->path);
    }
    held.source.clear();
    endpoint->m_observer->onPastePushAck(reply);
}

void RemoteSessionCore::servicePasteSource()
{
    if (!m_pasteCall || !m_channel)
        return;
    auto& held = *m_pasteCall;
    auto* endpoint = held.endpoint.get();
    if (!endpoint || endpoint->m_stopped || !endpoint->m_ackSeen || endpoint->m_sessionID != held.sessionID
        || endpoint->m_fileReadGeneration != held.generation || m_core->paneHandle(held.sessionID) != held.pane
        || !m_core->isScreenAttached()) {
        m_pasteCall.reset();
        return;
    }
    if (!held.source.hasBytesToSend())
        return;
    const auto pending = m_channel->pendingOutboundBytes();
    const auto capacity = m_channel->outboundCapacityBytes();
    const auto maximum = std::min(Protocol::Sync::bulkChunkPayloadBytes, held.source.heldBytes() - held.source.queuedBytes())
        + Protocol::Sync::bulkChunkOverheadBytes + Protocol::Sync::maxEnvelopeHeaderBytes + Protocol::Sync::frameHeaderLength;
    if (pending < 0 || capacity < 0 || (static_cast<std::uint64_t>(pending) <= static_cast<std::uint64_t>(capacity) && maximum <= static_cast<std::uint64_t>(capacity - pending))) {
        // This callback only appends a bounded frame to the canonical client's
        // memory queue. No observer, dispatcher or carrier callback runs until
        // sendOne has returned and advanced its immutable source.
        (void)held.source.sendOne([this](const Protocol::Sync::BulkChunk& chunk) { return m_core->sendControlUploadChunk(chunk); });
    }
    if (!held.source.hasBytesToSend() || m_isPasteTickPosted)
        return;
    m_isPasteTickPosted = true;
    const auto generation = m_generation.load(std::memory_order_acquire);
    m_dispatcher->postDelayed(0.01, [weakThis = weakPtr(), generation] {
        auto* owner = weakThis.get();
        if (!owner || owner->m_stopped || owner->m_generation.load(std::memory_order_acquire) != generation)
            return;
        owner->m_isPasteTickPosted = false;
        owner->flushScreen();
    });
}

uint32_t RemoteSessionCore::requestFile(const std::string& path)
{
    if (!m_ackSeen || m_stopped || m_nextFileRequestID == std::numeric_limits<std::uint32_t>::max()
        || m_fileReadGeneration == std::numeric_limits<std::uint64_t>::max())
        return 0;
    const auto endpoint = weakPtr();
    const auto owner = screenOwner().weakPtr();
    if (!owner->m_ackSeen || owner->m_stopped || !owner->m_core->isScreenAttached()
        || owner->m_nextFileCallID == std::numeric_limits<std::uint64_t>::max())
        return 0;
    const auto pane = owner->m_core->paneHandle(m_sessionID);
    if (!pane)
        return 0;
    for (auto& held : owner->m_fileCalls) {
        if (held && (!held->endpoint || held->endpoint->m_stopped || held->endpoint->m_fileReadGeneration != held->generation))
            held.reset();
    }
    const auto available = std::ranges::find_if(owner->m_fileCalls, [](const auto& held) { return !held; });
    if (available == owner->m_fileCalls.end())
        return 0;
    const auto correlation = "file:" + std::to_string(++owner->m_nextFileCallID);
    const auto request = Protocol::Control::fileReadRequest(pane, path, correlation);
    if (request.empty())
        return 0;
    const auto generation = owner->m_generation.load(std::memory_order_acquire);
    const auto fileGeneration = m_fileReadGeneration;
    const auto sessionID = m_sessionID;
    const uint32_t requestID = ++m_nextFileRequestID;
    if (!owner->m_core->call(request))
        return 0;
    available->emplace(FileCall { endpoint, fileGeneration, sessionID, pane, requestID, correlation, path });
    owner->flushScreen();
    // A send may synchronously lose or replace the attachment. Its request
    // must not become a correlation on the new connection.
    if (!endpoint || !owner || !endpoint->m_ackSeen || endpoint->m_stopped || endpoint->m_sessionID != sessionID
        || fileGeneration != endpoint->m_fileReadGeneration || generation != owner->m_generation.load(std::memory_order_acquire))
        return 0;
    return requestID;
}

void RemoteSessionCore::receiveFileAnswer(std::string_view text)
{
    bool valid = false;
    const auto answer = Codec::JSON::parse(std::string(text), valid);
    if (!valid)
        return;
    const auto* id = answer.find("id");
    if (!id || !id->isString())
        return;
    const auto found = std::ranges::find_if(m_fileCalls, [&](const auto& held) { return held && held->correlation == id->asString(); });
    if (found == m_fileCalls.end())
        return;
    FileCall held = std::move(**found);
    found->reset();
    auto* endpoint = held.endpoint.get();
    if (!endpoint || endpoint->m_stopped || !endpoint->m_ackSeen || endpoint->m_sessionID != held.sessionID
        || endpoint->m_fileReadGeneration != held.generation || m_core->paneHandle(held.sessionID) != held.pane)
        return;
    auto decoded = Protocol::Control::decodeFileReadReply(answer, held.correlation, held.pane, held.path);
    Protocol::Control::FileReadReply reply;
    if (decoded)
        reply = std::move(*decoded);
    else
        reply.path = std::move(held.path);
    reply.requestID = held.requestID;
    endpoint->m_observer->onFileData(std::move(reply));
}

bool RemoteSessionCore::prepareFileDownload(std::uint64_t token, const std::string& host, const std::string& path, FileLinkLine line)
{
    m_pendingFileDownload.reset();
    const auto& owner = screenOwner();
    if (!token || host.empty() || path.empty() || !m_ackSeen || m_stopped || !owner.m_ackSeen || owner.m_stopped)
        return false;
    m_pendingFileDownload = FileDownload { token, owner.m_generation.load(std::memory_order_acquire),
        owner.m_daemonInstanceID, m_sessionID, host, path, line };
    return true;
}

const RemoteSessionCore::FileDownload* RemoteSessionCore::pendingFileDownload() const
{
    const auto& owner = screenOwner();
    if (!m_pendingFileDownload || !m_ackSeen || m_stopped || !owner.m_ackSeen || owner.m_stopped)
        return nullptr;
    const auto& pending = *m_pendingFileDownload;
    if (pending.generation != owner.m_generation.load(std::memory_order_acquire)
        || pending.daemonInstanceID != owner.m_daemonInstanceID || pending.sessionID != m_sessionID)
        return nullptr;
    return &pending;
}

std::uint32_t RemoteSessionCore::answerFileDownload(std::uint64_t token, FileDownloadDecision decision)
{
    const auto* pending = pendingFileDownload();
    if (!pending || pending->token != token)
        return 0;
    const std::string path = pending->path;
    m_pendingFileDownload.reset();
    return decision == FileDownloadDecision::Download ? requestFile(path) : 0;
}

uint32_t RemoteSessionCore::requestSessionList()
{
    if (m_workspaceConnection) {
        return m_workspaceConnection->requestSessionList(*this);
    }
    if (!m_ackSeen || m_sessionListRequestID || m_nextSessionListRequestID == std::numeric_limits<std::uint32_t>::max())
        return 0;
    const uint32_t requestID = ++m_nextSessionListRequestID;
    return beginSessionList(requestID) ? requestID : 0;
}

bool RemoteSessionCore::beginSessionList(std::uint32_t requestID)
{
    if (!requestID || m_directoryListing || !m_core->isScreenAttached())
        return false;
    const std::string id = "ubiquitty.directory." + std::to_string(requestID);
    if (!m_core->call(HostListing::subscribeRequest(id)))
        return false;
    m_directoryListing = std::make_unique<HostListing>(id);
    m_sessionListRequestID = requestID;
    const std::uint64_t generation = m_generation.load(std::memory_order_acquire);
    m_dispatcher->postDelayed(3.0, [weakThis = weakPtr(), generation, requestID] {
        auto* session = weakThis.get();
        if (!session || session->m_stopped || generation != session->m_generation.load(std::memory_order_acquire) || session->m_sessionListRequestID != requestID)
            return;
        session->finishSessionList(DirectoryCompletion::Failed);
        if (weakThis && !session->m_stopped)
            session->flushScreen();
    });
    const auto weakThis = weakPtr();
    const auto* core = m_core.get();
    flushScreen();
    return weakThis && !m_stopped && m_core.get() == core && generation == m_generation.load(std::memory_order_acquire);
}

void RemoteSessionCore::serviceSessionList()
{
    if (!m_directoryListing)
        return;
    if (m_directoryListing->isRefused()) {
        finishSessionList(DirectoryCompletion::Failed);
        return;
    }
    const auto* connection = m_core->screen();
    if (!connection)
        return;
    const auto now = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
    if (m_directoryListing->refresh(*connection, m_core->listingRevision(), now) == HostListing::Change::Malformed)
        finishSessionList(DirectoryCompletion::Failed);
    else if (m_directoryListing->isListing() && m_directoryListing->isWhole())
        finishSessionList(DirectoryCompletion::Complete);
}

void RemoteSessionCore::finishSessionList(DirectoryCompletion completion)
{
    if (!m_directoryListing)
        return;
    const bool isComplete = completion == DirectoryCompletion::Complete;
    const auto requestID = std::exchange(m_sessionListRequestID, 0);
    auto listing = std::move(m_directoryListing);
    const auto rows = isComplete ? sessionRows(listing->tree()) : std::vector<SessionRow> {};
    const bool didEnd = m_core->call(HostListing::unsubscribeRequest("ubiquitty.directory.end." + std::to_string(requestID)));
    const auto weakThis = weakPtr();
    const std::uint64_t generation = m_generation.load(std::memory_order_acquire);
    if (isComplete && didEnd)
        m_observer->onSessionList(requestID, rows);
    else
        m_observer->onSessionListFailed(requestID);
    // A full control queue cannot leave an unowned subscription running.
    if (!didEnd && weakThis && !m_stopped && generation == m_generation.load(std::memory_order_acquire))
        replaceChannel("the directory subscription could not be retired");
}

// ---- connect / reconnect ---------------------------------------------

void RemoteSessionCore::connect()
{
    auto protectedLifetime = m_dispatchLifetime.lock();
    m_core->resetForReconnect();
    m_ackSeen = false;
    m_bootstrapResponse = {};
    // The outstanding ask belonged to the connection that is gone: its answer
    // cannot arrive on this one, and a held id would let an unasked-for frame
    // pass as the reply to a question this attempt never put.
    m_sessionListRequestID = 0;
    m_directoryListing.reset();
    const std::uint64_t generation = ++m_generation; // this attempt's stamp
    transition(State::Connecting, "");
    auto ready = [weakThis = weakPtr(), generation](std::unique_ptr<Ubiquitty::Transport::Channel> channel, bool isQUIC) {
        auto* session = weakThis.get();
        if (!session)
            return; // the owner is gone; nothing wants this channel
        if (generation != session->m_generation.load(std::memory_order_acquire) || session->m_stopped)
            return; // a superseded attempt, or stopped while bootstrapping
        // The factory sets its answer before invoking this completion. Copy
        // its failure now: another request must not replace this attempt's
        // cause before it is consumed or handed to an observer.
        auto failure = channel ? std::nullopt : session->m_factory->creationFailure();
        session->handleChannelReady(std::move(channel), isQUIC, std::move(failure));
    };
    // An attachment held, and a carrier the factory can dial without a
    // login: the proof is tried first, and the bootstrap only if the host
    // refuses it or never answers (Docs/sync.md §6.9 "Resume without SSH").
    std::optional<ResumeKey> key = m_isResumeDialRefused || !m_isAcknowledgedCarrierQUIC || !m_factory->canResumeChannel() ? std::nullopt : resumeKey();
    m_isResumeDial = key.has_value();
    if (!key) {
        m_factory->createChannel((!m_quicFailed) ? TransportMode::QUIC : TransportMode::TCP, std::move(ready));
        return;
    }
    m_dispatcher->postDelayed(resumeDialDeadlineSeconds, [weakThis = weakPtr(), generation] {
        auto* session = weakThis.get();
        if (!session || generation != session->m_generation.load(std::memory_order_acquire) || session->m_stopped)
            return;
        if (session->m_isResumeDial && !session->m_ackSeen)
            session->abandonResumeDial();
    });
    m_factory->resumeChannel(std::move(*key), std::move(ready));
}

std::optional<ResumeKey> RemoteSessionCore::resumeKey() const
{
    const std::optional<Protocol::Sync::HeldAttachment> held = m_core->heldAttachment();
    if (!held)
        return std::nullopt;
    return ResumeKey {
        .exporterLabel = std::string(Protocol::Sync::resumeExporterLabel),
        .buildPreamble = [secret = held->resumeSecret, attachmentID = held->attachmentID, hostInstanceID = held->hostInstanceID](std::span<const std::uint8_t, 32> exporter) {
            return Protocol::Sync::resumePreamble(secret, exporter, attachmentID, hostInstanceID);
        },
    };
}

void RemoteSessionCore::abandonResumeDial(ResumeDialFailure failure)
{
    teardownChannel();
    m_isResumeDialRefused = true;
    m_hasRetriedAfterLifecycleChange = true;
    if (failure == ResumeDialFailure::Unanswered)
        latchQUICHandshakeFailure();
    if (m_stopped || m_hostExited || m_deployPending)
        return;
    connect();
}

Protocol::Sync::LinkState RemoteSessionCore::linkState() const
{
    if (m_workspaceConnection)
        return m_workspaceConnection->connection().linkState();
    return m_core->screenLinkState();
}

void RemoteSessionCore::suspend()
{
    auto protectedLifetime = m_dispatchLifetime.lock();
    if (m_workspaceConnection) {
        m_workspaceConnection->connection().suspend();
        return;
    }
    if (m_stopped || m_hostExited || m_deployPending || !m_channel || !m_ackSeen)
        return;
    // The Bye leaves before the carrier does, so the host hears the reason
    // and not only the silence.
    m_core->suspendScreen();
    std::vector<uint8_t> frame;
    m_core->takeScreenOutput(frame);
    if (!frame.empty())
        writeFrame(frame);
    teardownChannel();
    // No timer redials a suspended client: resumeAfterSuspension does.
    transition(State::Reconnecting, "This device went to sleep.");
}

void RemoteSessionCore::handleChannelReady(std::unique_ptr<Ubiquitty::Transport::Channel> channel, bool isQUIC, std::optional<ChannelCreationFailure> failure)
{
    auto protectedLifetime = m_dispatchLifetime.lock();
    m_transportIsQUIC = isQUIC;
    if (!channel && m_isResumeDial) {
        abandonResumeDial();
        return;
    }
    if (!channel) {
        if (!m_stopped && !m_hostExited) {
            if (failure) {
                if (failure->recovery == ChannelFailureRecovery::InstanceGone) {
                    reresolveGoneInstance();
                    return;
                }
                const std::string detail = failure->detail.empty() ? "Ubiquitty couldn’t prepare the connection." : failure->detail;
                if (failure->recovery == ChannelFailureRecovery::UseStream && !m_quicFailed) {
                    m_quicFailed = true;
                    m_quicHandshakeFailure.clear();
                    m_channelCreationFailures = 0;
                    const auto weakThis = weakPtr();
                    transition(State::Reconnecting, detail);
                    if (weakThis && !m_stopped)
                        connect();
                    return;
                }
                const auto delay = Transport::connectRetryDelaySeconds(++m_channelCreationFailures);
                if (failure->recovery != ChannelFailureRecovery::Retry || !delay) {
                    m_initialSetupBudgetExhausted = failure->recovery == ChannelFailureRecovery::Retry && !delay;
                    ++m_reconnectEpoch;
                    m_reconnectScheduled = false;
                    transition(State::Failed, detail);
                    return;
                }
                transition(State::Reconnecting, detail);
                scheduleReconnect(detail, delay);
                return;
            }
            transition(State::Reconnecting, "No connection to the host is available.");
            scheduleReconnect("No connection to the host is available.");
        }
        return;
    }
    m_channelCreationFailures = 0;
    m_channel = std::move(channel);
    if (!m_channel->start(static_cast<Ubiquitty::Transport::ChannelObserver*>(this))) {
        // Immediate local setup error: Channel::start fires no callback, so
        // synthesize the drop path here.
        const bool didAuthenticationFail = m_channel->peerAuthenticationFailed();
        const bool boundsInitialSetup = m_channel->boundsInitialSetupFailures() && !m_hasEstablishedConnection;
        m_channel.reset();
        if (didAuthenticationFail) {
            m_hasPermanentAuthDenial = true;
            m_authDenialNotice = "The host’s identity doesn’t match the one this app trusts for it, so it didn’t connect.";
            transition(State::Failed, m_authDenialNotice);
            return;
        }
        if (m_isResumeDial) {
            abandonResumeDial();
            return;
        }
        if (m_transportIsQUIC && !m_ackSeen)
            latchQUICHandshakeFailure();
        if (!m_stopped && !m_hostExited) {
            if (boundsInitialSetup) {
                const auto delay = Transport::connectRetryDelaySeconds(++m_initialSetupFailures);
                if (!delay) {
                    m_initialSetupBudgetExhausted = true;
                    transition(State::Failed, "The host didn’t open the connection. Check that Ubiquitty’s host is running there, then try again.");
                    return;
                }
                transition(State::Reconnecting, "The connection to the host couldn’t start.");
                scheduleReconnect("The connection to the host couldn’t start.", delay);
                return;
            }
            transition(State::Reconnecting, "The connection to the host couldn’t start.");
            scheduleReconnect("The connection to the host couldn’t start.");
        }
    }
}

void RemoteSessionCore::scheduleReconnect(const std::string&, std::optional<double> delaySeconds)
{
    if (m_reconnectScheduled || m_stopped || m_hostExited || m_deployPending)
        return;
    m_reconnectScheduled = true;
    ++m_reconnectAttempts;
    const unsigned epoch = m_reconnectEpoch;
    m_dispatcher->postDelayed(delaySeconds.value_or(reconnectDelayForAttempt(m_reconnectAttempts)), [weakThis = weakPtr(), epoch] {
        auto* session = weakThis.get();
        if (!session)
            return; // the pane closed while this was waiting
        // A network change redialled while this was waiting: that attempt owns the
        // reconnect now, and firing here as well would open a second channel beside it.
        if (epoch != session->m_reconnectEpoch)
            return;
        session->m_reconnectScheduled = false;
        // A deploy that began after this timer was scheduled owns the next
        // connect (via deployFinished); don't race it.
        if (session->m_stopped || session->m_hostExited || session->m_deployPending || session->m_channel)
            return;
        session->connect();
    });
}

void RemoteSessionCore::setView(Protocol::Sync::ViewState state)
{
    m_view = state;
    if (m_workspaceConnection) {
        RemoteSessionCore& carrier = m_workspaceConnection->connection();
        carrier.m_core->setPaneView(m_sessionID, state);
        if (carrier.m_channel)
            carrier.flushScreen();
        return;
    }
    if (m_sessionID == 0)
        return;
    m_core->setPaneView(m_sessionID, state);
    if (m_channel)
        flushScreen();
}

void RemoteSessionCore::setSurfaceFocus(Ubiquitty::Modes::FocusState focus)
{
    m_surfaceFocus = focus;
    // The pane's own focus, which the host reports to its program; the
    // connection's, below, is whether any of its surfaces has the keyboard.
    reportPaneFocus();
    if (m_workspaceConnection) {
        m_workspaceConnection->noteFocus(*this, focus);
        return;
    }
    applyScreenFocus(focus);
}

void RemoteSessionCore::reportPaneFocus()
{
    // A frontend that cannot observe focus says nothing, and its pane is
    // told nothing of.
    if (!m_surfaceFocus || m_sessionID == 0)
        return;
    RemoteSessionCore& owner = screenOwner();
    owner.m_core->setPaneFocus(m_sessionID, *m_surfaceFocus);
    if (owner.m_channel)
        owner.flushScreen();
}

void RemoteSessionCore::applyScreenFocus(Ubiquitty::Modes::FocusState focus)
{
    m_core->setScreenFocus(focus);
    if (m_channel)
        flushScreen();
}

std::uint64_t RemoteSessionCore::paneBytesReceived() const
{
    if (m_workspaceConnection)
        return m_workspaceConnection->connection().m_core->paneBytesReceived(m_sessionID);
    return m_core->paneBytesReceived(m_sessionID);
}

void RemoteSessionCore::networkPathChanged()
{
    auto protectedLifetime = m_dispatchLifetime.lock();
    if (m_workspaceConnection) {
        m_workspaceConnection->connection().networkPathChanged();
        return;
    }
    if (m_stopped || m_hostExited || m_deployPending || m_hasPermanentAuthDenial)
        return;
    if (!takeLifecycleReport(m_isPathChangeReported))
        return;

    if (m_state == State::Connected || m_state == State::Connecting) {
        // An established QUIC connection migrates to the new path by itself, and a client that
        // tore it down here would throw away a working session on every WiFi-to-cellular hop.
        // Migration is a promise rather than a fact, though: a tunnel that stays up and stops
        // passing traffic looks connected until the idle timer, so the host is asked to answer
        // on the new path and the connection is replaced only if it does not.
        if (m_state == State::Connected && m_transportIsQUIC) {
            // The carrier moves to a fresh socket on the new path at once,
            // and does not wait out the silence that would tell it to.
            if (m_channel)
                m_channel->repath();
            askForProofOfLife("the host did not answer on the new network path");
            return;
        }
        // A stream connection survives every move that leaves its own route alone: a VPN's
        // tunnel coming up beside it, a second interface joining, a gateway set changing. It is
        // asked to answer, as a QUIC one is, and replaced only when it is silent.
        if (m_state == State::Connected && askForProofOfLife("the host did not answer after the network changed"))
            return;
    } else if (m_state != State::Reconnecting) {
        // Idle or Failed: nothing is waiting on a timer that the new path invalidates.
        return;
    }
    retryAfterLifecycleChange("The network changed.");
}

void RemoteSessionCore::resumeAfterSuspension(double secondsSuspended)
{
    auto protectedLifetime = m_dispatchLifetime.lock();
    if (m_workspaceConnection) {
        m_workspaceConnection->connection().resumeAfterSuspension(secondsSuspended);
        return;
    }
    if (m_stopped || m_hostExited || m_deployPending || m_hasPermanentAuthDenial
        || (m_state != State::Connected && m_state != State::Connecting && m_state != State::Reconnecting)) {
        return;
    }
    if (!takeLifecycleReport(m_isResumeReported))
        return;
    // A QUIC connection survives a short suspension: the host's idle window is
    // still open, and replacing it costs an SSH login on port 22 at the moment
    // the path is least ready for one. An unmeasured suspension is assumed to
    // be past that window. A stream connection has no idle window at the host
    // and survives a suspension of any length whose path held. Either is kept
    // only once it proves it still carries frames.
    const bool mayHaveSurvived = m_state == State::Connected && m_channel
        && (!m_transportIsQUIC || (secondsSuspended >= 0 && secondsSuspended < suspensionKeepsQUICSeconds));
    if (mayHaveSurvived && askForProofOfLife("the host did not answer after the app resumed"))
        return;
    retryAfterLifecycleChange("Reconnecting after this device woke.");
}

void RemoteSessionCore::retryAfterLifecycleChange(const std::string& reason)
{
    // Interrupting an unacknowledged proof consumes its attempt just as a
    // close or deadline does. No new path may renew its bootstrap-free wait.
    if (m_state == State::Connecting && m_isResumeDial && !m_ackSeen) {
        abandonResumeDial();
        return;
    }
    // One accelerated retry may retire an old socket or an old-path timer.
    // Further notifications cannot starve the bootstrap or its backoff.
    if (m_hasRetriedAfterLifecycleChange)
        return;
    replaceChannel(reason);
}

bool RemoteSessionCore::takeLifecycleReport(bool& isReported)
{
    if (isReported)
        return false;
    isReported = true;
    m_dispatcher->post([weakThis = weakPtr(), flag = &isReported] {
        if (weakThis.get())
            *flag = false;
    });
    return true;
}

bool RemoteSessionCore::reconnectNow()
{
    auto protectedLifetime = m_dispatchLifetime.lock();
    if (m_workspaceConnection)
        return m_workspaceConnection->connection().reconnectNow();
    const bool mayRetry = m_state == State::Reconnecting || (m_state == State::Failed && m_initialSetupBudgetExhausted);
    if (m_stopped || m_hostExited || m_deployPending || m_hasPermanentAuthDenial || !mayRetry || m_channel)
        return false;
    // The pending timer's epoch no longer matches, so it returns unused rather
    // than opening a second channel beside this one.
    ++m_reconnectEpoch;
    m_reconnectScheduled = false;
    m_reconnectAttempts = 0;
    m_backlogDrops = 0;
    m_channelCreationFailures = 0;
    m_initialSetupFailures = 0;
    m_initialSetupBudgetExhausted = false;
    connect();
    return true;
}

bool RemoteSessionCore::askForProofOfLife(std::string reason)
{
    // The screen plane's probe is an ask the host is bound to answer, after
    // every frame it sent before reading it, and only its own Echo is taken
    // for the answer. A connection whose plane is not welcomed yet cannot be
    // asked.
    if (!m_ackSeen)
        return false;
    // A proof already asked for is the one being judged: a second report in
    // its window neither restarts the wait nor asks again.
    if (m_isProvingLife)
        return true;
    if (!m_core->checkScreenLiveness())
        return false;
    m_isProvingLife = true;
    const std::uint64_t serial = ++m_proofSerial;
    flushScreen();
    const std::uint64_t generation = m_generation.load(std::memory_order_acquire);
    m_dispatcher->postDelayed(proofOfLifeDeadlineSeconds, [weakThis = weakPtr(), generation, serial, reason = std::move(reason)] {
        auto* session = weakThis.get();
        if (!session)
            return;
        // A connection that closed, or was replaced, since the ask is no longer
        // the one being judged.
        if (generation != session->m_generation.load(std::memory_order_acquire) || session->m_stopped)
            return;
        // Answered, or a newer ask owns the judgement.
        if (!session->m_isProvingLife || session->m_proofSerial != serial)
            return;
        session->replaceChannel(reason);
    });
    return true;
}

void RemoteSessionCore::noteProofOfLife()
{
    if (!m_isProvingLife || m_core->isScreenLivenessUnproven())
        return;
    m_isProvingLife = false;
}

void RemoteSessionCore::replaceChannel(const std::string& reason)
{
    m_hasRetriedAfterLifecycleChange = true;
    teardownChannel();
    ++m_reconnectEpoch;
    m_reconnectScheduled = false;
    transition(State::Reconnecting, reason);
    connect();
}

bool RemoteSessionCore::sniffDeploySentinel(std::vector<std::uint8_t>& bytes)
{
    const auto result = m_bootstrapResponse.consume(bytes);
    switch (result.status) {
    case BootstrapResponse::Status::Waiting:
        return true;
    case BootstrapResponse::Status::Invalid:
        teardownChannel();
        transition(State::Failed, "The host’s login printed more than Ubiquitty can read past before its host started. Check the shell startup files there for output.");
        return true;
    case BootstrapResponse::Status::Ready:
        bytes.erase(bytes.begin(), bytes.end() - static_cast<std::ptrdiff_t>(result.protocolBytes.size()));
        bytes.insert(bytes.begin(), result.protocolPrefix.begin(), result.protocolPrefix.end());
        return false;
    case BootstrapResponse::Status::InstanceGone:
        // The channel's own close follows the line and is ignored: the
        // teardown moved the generation on.
        teardownChannel();
        reresolveGoneInstance();
        return true;
    case BootstrapResponse::Status::Deploy:
        break;
    }
    const std::string platformArch = m_bootstrapResponse.platform();

    // Drop this channel; teardownChannel bumps the generation so the channel's
    // queued onClose (from the ssh exit that follows the sentinel) is ignored.
    teardownChannel();
    if (m_deployedOnce) {
        // We already pushed a binary this session, yet the host still has none
        // — the deployed binary is broken. Fail rather than loop forever.
        transition(State::Failed, "The Ubiquitty host installed there didn’t start.");
        return true;
    }
    m_deployedOnce = true;
    m_deployPending = true;
    transition(State::Deploying, platformArch);
    m_observer->onDeployRequested(platformArch);
    return true;
}

void RemoteSessionCore::reresolveGoneInstance()
{
    if (m_stopped || m_hostExited)
        return;
    ++m_reconnectEpoch;
    m_reconnectScheduled = false;
    if (m_hasDroppedGonePin || !m_factory) {
        transition(State::Failed, std::string(restartedWithoutWorkspaceNotice));
        return;
    }
    m_hasDroppedGonePin = true;
    m_factory->clearHandoverReach();
    m_daemonSocketName.clear();
    m_channelCreationFailures = 0;
    m_initialSetupFailures = 0;
    m_initialSetupBudgetExhausted = false;
    const auto weakThis = weakPtr();
    transition(State::Reconnecting, "Ubiquitty restarted on the host. Looking for this workspace there…");
    if (weakThis && !m_stopped)
        connect();
}

std::string hostUploadDigest(std::string_view bytes)
{
    std::array<unsigned char, 32> digest {};
    Crypto::sha256({ reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size() }, digest);
    static constexpr char hexDigits[] = "0123456789abcdef";
    std::string hex;
    hex.reserve(digest.size() * 2);
    for (const unsigned char byte : digest) {
        hex.push_back(hexDigits[byte >> 4]);
        hex.push_back(hexDigits[byte & 0x0F]);
    }
    return hex;
}

bool RemoteSessionCore::updateHostBeforeConnect(std::string platformArch)
{
    if (m_workspaceConnection)
        return m_workspaceConnection->connection().updateHostBeforeConnect(std::move(platformArch));
    if (m_stopped || m_channel || m_deployPending || m_state != State::Idle)
        return false;
    m_updateBeforeConnectPlatform = std::move(platformArch);
    return true;
}

CarrierFact RemoteSessionCore::carrierFact() const
{
    CarrierFact fact;
    if (m_workspaceConnection)
        return m_workspaceConnection->connection().carrierFact();
    if (m_channel)
        fact.carrier = m_transportIsQUIC ? Carrier::QUIC : Carrier::Stream;
    if (m_factory && fact.carrier != Carrier::QUIC)
        fact.quicFallbackReason = m_factory->quicFallbackReason();
    // The factory says why it handed out no QUIC carrier; a carrier it did
    // hand out and that failed its handshake is this core's to say. Either
    // way the words name QUIC: the SSH bootstrap that pointed at it worked.
    if (fact.quicFallbackReason.empty() && fact.carrier != Carrier::QUIC && m_quicFailed)
        fact.quicFallbackReason = m_quicHandshakeFailure;
    return fact;
}

void RemoteSessionCore::latchQUICHandshakeFailure()
{
    m_quicFailed = true;
    m_quicHandshakeFailure = "QUIC could not connect, so this session uses SSH";
}

void RemoteSessionCore::deployFinished(DeployResult result, const std::string& error)
{
    auto protectedLifetime = m_dispatchLifetime.lock();
    if (m_workspaceConnection) {
        m_workspaceConnection->connection().deployFinished(result, error);
        return;
    }
    if (result == DeployResult::Succeeded && m_factory) {
        m_factory->hostDeploymentFinished();
    }
    if (!m_deployPending || m_stopped)
        return;
    m_deployPending = false;
    if (result == DeployResult::Failed) {
        transition(State::Failed, error);
        return;
    }
    connect(); // the bootstrap now finds the freshly-installed binary
}

// ---- ChannelObserver (I/O thread → marshal to the session thread) ------

void RemoteSessionCore::onConnect()
{
    const std::uint64_t generation = m_generation.load(std::memory_order_acquire);
    m_dispatcher->post([weakThis = weakPtr(), generation] {
        if (auto* session = weakThis.get())
            session->handleConnect(generation);
    });
}

void RemoteSessionCore::onData(std::span<const std::uint8_t> data)
{
    const std::uint64_t generation = m_generation.load(std::memory_order_acquire);
    bool shouldPostDrain = false;
    bool shouldDisconnect = false;
    std::size_t stagedBytes = 0;
    {
        const std::scoped_lock lock(m_inboundMutex);
        stageGenerationLocked(generation);
        if (m_isReliableDisconnectPosted) {
            return;
        }
        // Pending and draining batches each have a fixed budget independent of peer pacing.
        constexpr std::size_t maxPendingBytes = 4 * Protocol::Sync::maxPayloadLength;
        if (data.size() > maxPendingBytes - m_inboundPending.size()) {
            m_isReliableDisconnectPosted = true;
            shouldDisconnect = true;
            m_inboundPending.clear();
        } else {
            m_inboundPending.insert(m_inboundPending.end(), data.begin(), data.end());
            shouldPostDrain = !m_isInboundDrainPosted || m_inboundDrainGeneration != generation;
            if (shouldPostDrain) {
                m_isInboundDrainPosted = true;
                m_inboundDrainGeneration = generation;
            }
            stagedBytes = m_inboundPending.size();
        }
    }
    if (shouldDisconnect) {
        m_dispatcher->post([weakThis = weakPtr(), generation] {
            if (auto* session = weakThis.get()) {
                session->handleClose(generation, "The workspace sent more than this connection can hold at once.");
            }
        });
        return;
    }
    if (shouldPostDrain) {
        m_dispatcher->post([weakThis = weakPtr(), generation] {
            if (auto* session = weakThis.get()) {
                session->drainInbound(generation);
            }
        });
    }
    m_inboundBackpressure.applyTo(m_channel.get(), stagedBytes);
}

void RemoteSessionCore::onDatagram(std::span<const std::uint8_t> data)
{
    const std::uint64_t generation = m_generation.load(std::memory_order_acquire);
    bool shouldPostDrain = false;
    {
        const std::scoped_lock lock(m_inboundMutex);
        stageGenerationLocked(generation);
        shouldPostDrain = stageDatagramLocked(data) && !m_isDatagramDrainActive
            && (!m_isDatagramDrainPosted || m_datagramDrainGeneration != generation);
        if (shouldPostDrain) {
            m_isDatagramDrainPosted = true;
            m_datagramDrainGeneration = generation;
        }
    }
    if (shouldPostDrain)
        m_dispatcher->post([weakThis = weakPtr(), generation] {
            if (auto* session = weakThis.get())
                session->drainDatagrams(generation);
        });
}

bool RemoteSessionCore::stageDatagramLocked(std::span<const std::uint8_t> bytes)
{
    if (bytes.empty())
        return false;
    if (!m_datagramPending) {
        m_datagramPending.reset(new (std::nothrow) DatagramIngressBatch);
        if (!m_datagramPending)
            return false;
    }
    const std::size_t drainingBytes = m_datagramDraining ? m_datagramDraining->byteCount : 0;
    const std::size_t drainingFrames = m_datagramDraining ? m_datagramDraining->frameCount : 0;
    const std::size_t retainedBytes = drainingBytes + m_datagramPending->byteCount;
    const std::size_t retainedFrames = drainingFrames + m_datagramPending->frameCount;
    const bool fits = m_datagramPending->frameCount < datagramIngressBatchMaximumFrames
        && bytes.size() <= datagramIngressBatchMaximumBytes - m_datagramPending->byteCount
        && retainedFrames < datagramIngressMaximumRetainedFrames
        && retainedBytes <= datagramIngressMaximumRetainedBytes
        && bytes.size() <= datagramIngressMaximumRetainedBytes - retainedBytes;
    if (!fits)
        return false;
    std::memcpy(m_datagramPending->bytes.data() + m_datagramPending->byteCount, bytes.data(), bytes.size());
    m_datagramPending->lengths[m_datagramPending->frameCount++] = bytes.size();
    m_datagramPending->byteCount += bytes.size();
    return true;
}

void RemoteSessionCore::stageGenerationLocked(std::uint64_t generation)
{
    if (m_inboundGeneration == generation)
        return;
    // A new attempt: whatever the previous one left staged is the middle of a
    // stream this decoder never saw the start of.
    m_inboundPending.clear();
    m_isReliableDisconnectPosted = false;
    if (m_datagramPending)
        m_datagramPending->clear();
    m_inboundGeneration = generation;
}

void RemoteSessionCore::drainInbound(std::uint64_t generation)
{
    auto protectedLifetime = m_dispatchLifetime.lock();
    std::size_t stagedBytes = 0;
    {
        const std::scoped_lock lock(m_inboundMutex);
        if (m_isInboundDrainPosted && m_inboundDrainGeneration == generation)
            m_isInboundDrainPosted = false;
        m_inboundDraining.clear();
        if (m_inboundGeneration == generation)
            m_inboundDraining.swap(m_inboundPending);
        stagedBytes = m_inboundPending.size();
    }
    // What is left staged after taking this batch, which is what decides
    // whether the carrier may start again.
    m_inboundBackpressure.applyTo(m_channel.get(), stagedBytes);
    if (m_inboundDraining.empty())
        return;
    handleData(generation, m_inboundDraining);
}

void RemoteSessionCore::drainDatagrams(std::uint64_t generation)
{
    auto protectedLifetime = m_dispatchLifetime.lock();
    {
        const std::scoped_lock lock(m_inboundMutex);
        if (m_isDatagramDrainPosted && m_datagramDrainGeneration == generation)
            m_isDatagramDrainPosted = false;
        if (m_isDatagramDrainActive || generation != m_generation.load(std::memory_order_acquire) || m_stopped)
            return;
        m_isDatagramDrainActive = true;
        if (m_datagramDraining)
            m_datagramDraining->clear();
        if (m_inboundGeneration == generation)
            m_datagramDraining.swap(m_datagramPending);
    }
    // One observer notification for the whole burst, not one per datagram: a
    // repaint shows the latest grid either way.
    const bool wasScreenDirty = std::exchange(m_isScreenDirty, false);
    std::size_t offset = 0;
    if (m_datagramDraining) {
        for (std::size_t frame = 0; frame < m_datagramDraining->frameCount; ++frame) {
            const std::size_t length = m_datagramDraining->lengths[frame];
            assert(offset + length <= m_datagramDraining->byteCount);
            handleDatagram(generation, std::span { m_datagramDraining->bytes.data() + offset, length });
            offset += length;
        }
        assert(offset == m_datagramDraining->byteCount);
    }
    flushScreen();
    // Hand the now-empty fixed block back to the producer when it has not
    // already allocated the second one while this batch was being consumed.
    bool shouldPostDrain = false;
    const std::uint64_t nextGeneration = m_generation.load(std::memory_order_acquire);
    const bool shouldNotify = m_isScreenDirty && generation == nextGeneration && !m_stopped;
    m_isScreenDirty = wasScreenDirty;
    {
        const std::scoped_lock lock(m_inboundMutex);
        if (m_datagramDraining)
            m_datagramDraining->clear();
        if (!m_datagramPending)
            m_datagramPending.swap(m_datagramDraining);
        m_isDatagramDrainActive = false;
        shouldPostDrain = !m_stopped && m_inboundGeneration == nextGeneration
            && m_datagramPending && m_datagramPending->frameCount
            && (!m_isDatagramDrainPosted || m_datagramDrainGeneration != nextGeneration);
        if (shouldPostDrain) {
            m_isDatagramDrainPosted = true;
            m_datagramDrainGeneration = nextGeneration;
        }
    }
    if (shouldPostDrain)
        m_dispatcher->post([weakThis = weakPtr(), nextGeneration] {
            if (auto* session = weakThis.get())
                session->drainDatagrams(nextGeneration);
        });
    if (shouldNotify)
        m_observer->onScreenChanged();
}

void RemoteSessionCore::onClose(const std::string& reason)
{
    const std::uint64_t generation = m_generation.load(std::memory_order_acquire);
    std::string copy = reason;
    m_dispatcher->post([weakThis = weakPtr(), generation, copy = std::move(copy)]() mutable {
        if (auto* session = weakThis.get())
            session->handleClose(generation, std::move(copy));
    });
}

void RemoteSessionCore::handleConnect(std::uint64_t generation)
{
    auto protectedLifetime = m_dispatchLifetime.lock();
    if (generation != m_generation.load(std::memory_order_acquire) || m_stopped)
        return;
    m_core->setResumeCertificatePin(m_channel ? m_channel->authenticatedCertificatePin().value_or(std::array<std::uint8_t, 32> {}) : std::array<std::uint8_t, 32> {});
    sendHello();
}

void RemoteSessionCore::handleData(std::uint64_t generation, std::vector<std::uint8_t>& bytes)
{
    if (generation != m_generation.load(std::memory_order_acquire) || m_stopped)
        return;
    if (!m_ackSeen && sniffDeploySentinel(bytes)) {
        return;
    }
    // A protocol observer can run a nested session loop. Its feed owns its
    // repaint flag and restores the outer feed's pending notification.
    const auto weakThis = weakPtr();
    const bool wasScreenDirty = std::exchange(m_isScreenDirty, false);
    const bool ok = m_core->feed(std::as_bytes(std::span { bytes.data(), bytes.size() }));
    const bool shouldNotify = m_isScreenDirty;
    m_isScreenDirty = wasScreenDirty;
    // The acknowledgements this feed owes, and a screen plane's Bye when it
    // closed, go out before anything below can tear the channel down.
    flushScreen();
    // Completing a directory request may retire or replace this exact owner.
    if (!weakThis || m_stopped || generation != m_generation.load(std::memory_order_acquire))
        return;
    noteProofOfLife();
    // A Welcome settles what was pending: written, in doubt, or discarded
    // with a pane that ended. Each pane's own core tells its observer.
    if (m_isMultiplexedWorkspace)
        m_observer->onPaneInputSettled();
    else
        reportInputStatus();
    if (!weakThis || m_stopped || generation != m_generation.load(std::memory_order_acquire))
        return;
    if (shouldNotify)
        m_observer->onScreenChanged();
    if (!weakThis || m_stopped || generation != m_generation.load(std::memory_order_acquire))
        return;
    if (m_hasPermanentAuthDenial) {
        handleClose(generation, m_authDenialNotice);
        return;
    }
    if (m_core->wantsHandoverRedial() && m_ackSeen && !m_hostExited) {
        // Rediscover the successor, but keep the old instance in HELLO and
        // the resume proof: only an authenticated inherited issuer may take it.
        m_factory->clearHandoverReach();
        m_daemonSocketName.clear();
        replaceChannel("the host is handing over");
        return;
    }
    if (!ok) {
        // feed() also returns false for a valid Bye: the connection cannot
        // continue, but its authenticated normal end is not a decode fault.
        // Use this reader's typed verdict, not only an earlier exit callback,
        // so malformed input after an unrelated pane end stays a fault.
        const auto* screen = m_core->screen();
        const bool didEndNormally = m_hostExited && screen && screen->byeReason() == Protocol::Sync::ByeReason::Normal;
        if (didEndNormally) {
            handleClose(generation, "The session ended.");
            return;
        }
        // A Bye the host sent says why; what is shown is its reason, not a fault.
        const std::optional<Protocol::Sync::Bye> bye = screen ? screen->bye() : std::nullopt;
        if (!bye || bye->reason == Protocol::Sync::ByeReason::Normal) {
            handleClose(generation, readFailureNotice(*m_core));
            return;
        }
        handleClose(generation, std::string(Protocol::Sync::byeNotice(bye->reason)), reconnectDelayAfterHostClose(bye->reason));
    }
}

void RemoteSessionCore::handleDatagram(std::uint64_t generation, std::span<const std::uint8_t> bytes)
{
    if (generation != m_generation.load(std::memory_order_acquire) || m_stopped)
        return;
    // A datagram before the handshake has no screen plane to apply to, and
    // the host resends what the client never acknowledges.
    if (!m_ackSeen)
        return;
    if (!m_core->feedDatagram(std::as_bytes(bytes)))
        handleClose(generation, readFailureNotice(*m_core));
}

std::optional<double> RemoteSessionCore::reconnectDelayAfterHostClose(Protocol::Sync::ByeReason reason)
{
    if (reason != Protocol::Sync::ByeReason::OutputBacklog)
        return std::nullopt;
    return reconnectDelayForAttempt(reconnectFastAttempts + ++m_backlogDrops);
}

void RemoteSessionCore::handleClose(std::uint64_t generation, std::string reason, std::optional<double> reconnectDelaySeconds)
{
    auto protectedLifetime = m_dispatchLifetime.lock();
    if (generation != m_generation.load(std::memory_order_acquire))
        return; // a superseded attempt's late close
    if (!m_channel)
        return; // already torn down (e.g. a decode fault then the EOF)
    // Authentication is not a path failure. In particular, a failed resume
    // pin must stop before either a new bootstrap or a stream fallback.
    if (m_channel->peerAuthenticationFailed()) {
        m_hasPermanentAuthDenial = true;
        m_authDenialNotice = reason.empty() ? "The host’s identity doesn’t match the one this app trusts for it, so it didn’t connect." : reason;
        teardownChannel();
        transition(State::Failed, m_authDenialNotice);
        return;
    }
    // A carrier whose close carries the host's reason (a QUIC application
    // CONNECTION_CLOSE) says what a Bye on its stream would have.
    ResumeDialFailure resumeFailure = ResumeDialFailure::Unanswered;
    if (!reconnectDelaySeconds) {
        if (const auto code = m_channel->peerApplicationCloseCode()) {
            if (const auto closeReason = Protocol::Sync::byeReasonOfQUICClose(*code)) {
                if (m_isResumeDial && *closeReason == Protocol::Sync::ByeReason::Malformed)
                    resumeFailure = ResumeDialFailure::HostRefused;
                reason = Protocol::Sync::byeNotice(*closeReason);
                reconnectDelaySeconds = reconnectDelayAfterHostClose(*closeReason);
                // The host refused this client, whatever carried the words:
                // a QUIC close is not a path that failed and may work on the
                // stream. A resume proof's refusal is the exception, answered
                // by the bootstrap below.
                if (!m_isResumeDial && Protocol::Sync::isRefusal(*closeReason)) {
                    onRefused(*closeReason);
                    if (m_hasPermanentAuthDenial)
                        reason = m_authDenialNotice;
                }
            }
        }
    }
    // A refused proof and an unanswered path both need a bootstrap. Only
    // the host's coded refusal proves that QUIC itself still reaches it.
    if (m_isResumeDial && !m_ackSeen && !m_stopped && !m_hostExited && !m_hasPermanentAuthDenial) {
        abandonResumeDial(resumeFailure);
        return;
    }
    if (m_transportIsQUIC && !m_ackSeen)
        latchQUICHandshakeFailure(); // QUIC that never reached HELLO_ACK → fall back
    const bool hasSSHAuthenticationFailure = !m_ackSeen
        && (reason.find("Permission denied") != std::string::npos
            || reason.find("Host key verification failed") != std::string::npos
            || reason.find("REMOTE HOST IDENTIFICATION HAS CHANGED") != std::string::npos);
    const bool boundsInitialSetup = !m_ackSeen && !m_hasEstablishedConnection && m_channel->boundsInitialSetupFailures();
    if (!hasSSHAuthenticationFailure && m_bootstrapResponse.isIncomplete() && !boundsInitialSetup) {
        reason = "The host’s login ended before Ubiquitty’s host started there.";
    }
    teardownChannel();
    if (m_stopped || m_hostExited || m_hasPermanentAuthDenial || hasSSHAuthenticationFailure) {
        transition(State::Failed, m_hasPermanentAuthDenial ? m_authDenialNotice : reason);
        return;
    }
    if (boundsInitialSetup) {
        const auto delay = Transport::connectRetryDelaySeconds(++m_initialSetupFailures);
        if (!delay) {
            m_initialSetupBudgetExhausted = true;
            transition(State::Failed, "The host didn’t finish connecting. Check that Ubiquitty’s host is running there, then try again. " + reason);
            return;
        }
        transition(State::Reconnecting, reason);
        scheduleReconnect(reason, delay);
        return;
    }
    if (!reconnectDelaySeconds)
        m_backlogDrops = 0;
    transition(State::Reconnecting, reason);
    scheduleReconnect(reason, reconnectDelaySeconds);
}

// ---- SyncClientObserver (session thread, inside feed()) ----------------

void RemoteSessionCore::onRefused(Protocol::Sync::ByeReason reason)
{
    m_authDenialNotice = Protocol::Sync::byeNotice(reason);
    m_lastRefusal.hasRefusal = true;
    m_lastRefusal.reason = reason;
    m_lastRefusal.hostBuild = m_core->hostBuild();
    m_lastRefusal.olderSide = Protocol::Sync::olderSideOfRefusal(reason, m_lastRefusal.hostBuild);
    m_lastRefusal.hostPlatform = m_core->hostPlatform();
    if (reason == Protocol::Sync::ByeReason::VersionMismatch) {
        // The sentence names the older end and both builds; the window that
        // knows the host's name composes it again with that name.
        UpdateFacts facts;
        facts.host = m_lastRefusal.hostBuild;
        m_authDenialNotice = versionRefusalSentence(facts);
    }
    // The host's current daemon, asked after the pinned one was reported
    // gone, holds no record of the workspace.
    const bool isGoneWithoutRecord = m_hasDroppedGonePin && reason == Protocol::Sync::ByeReason::SessionGone;
    if (isGoneWithoutRecord)
        m_authDenialNotice = restartedWithoutWorkspaceNotice;
    m_lastRefusal.detail = m_authDenialNotice;
    // A refused reattach is terminal for the session the handshake named,
    // whatever the reason's own retry rule says: a reconnect presents that same
    // id, so the answer to the next attempt is the answer to this one. Consent
    // and Scope are the two the host refuses a named session with; the frontend
    // hears which session was refused and falls back to listing or to a fresh
    // one, neither of which this core may choose on its own.
    if (m_helloAttachSessionID != 0
        && (reason == Protocol::Sync::ByeReason::NoConsent || reason == Protocol::Sync::ByeReason::OutsideScope
            || reason == Protocol::Sync::ByeReason::SessionGone)) {
        if (!isGoneWithoutRecord)
            m_authDenialNotice = reattachRefusedNotice(m_helloAttachSessionID, reason);
        m_lastRefusal.detail = m_authDenialNotice;
        m_hasPermanentAuthDenial = true;
        m_observer->onReattachRefused(m_helloAttachSessionID, reason);
        return;
    }
    if (!Protocol::Sync::isPermanentRefusal(reason))
        return;
    // The denial arrives inside feed(), with the channel still up: the daemon
    // drops us next. Recording it here is what makes that drop terminal, and
    // handleData closes the channel itself so the person is not shown a
    // reconnect that cannot land.
    m_hasPermanentAuthDenial = true;
}

void RemoteSessionCore::onFrameDecoded(uint8_t type, std::size_t wireBytes, std::size_t logicalBytes) { m_trace.recordInbound(type, wireBytes, logicalBytes); }

void RemoteSessionCore::onHelloAck(uint32_t sessionID, const std::string&)
{
    const auto instance = m_core->daemonInstanceID();
    const bool isSuccessor = m_daemonInstanceID && screenOwner().m_core->isSuccessorOf(m_daemonInstanceID);
    // A host that restarted and recreated this workspace from its record
    // says so for the exact instance asked for; that is the workspace coming
    // back, not another host answering in its place.
    const bool isRestored = m_daemonInstanceID && m_core->isRestoredSuccessorOf(m_daemonInstanceID);
    if (m_daemonInstanceID && instance != m_daemonInstanceID && !isSuccessor && !isRestored) {
        onRefused(Protocol::Sync::ByeReason::SessionGone);
        refuseAcknowledgedHost(std::string(restartedWithoutWorkspaceNotice));
        return;
    }
    m_daemonInstanceID = instance;
    // The structure that follows is the restored workspace, laid out in the
    // places the mirror's panes held: it is adopted beside the mirror, not
    // over it, so each pane can be paired with the one restored in its place.
    // Held until a structure is adopted, which a drop before it may delay.
    if (isRestored && m_isMultiplexedWorkspace && m_workspace)
        m_isAwaitingRestoredMirror = true;
    // The first structure on a connection is always adopted and told: what
    // waits on the mirror after a reconnect waits on that.
    m_mirrorSignature.clear();
    m_workspaceStructureSnapshot.reset();
    // An ask the previous connection carried died with it unanswered.
    m_frontTabAsks.clear();
    // A view set before the host named the session applies to the pane it
    // binds for it.
    if (m_view && !m_workspaceConnection)
        m_core->setPaneView(sessionID, *m_view);
    if (m_isMultiplexedWorkspace) {
        const auto identity = m_core->acknowledgedWorkspaceID();
        // The host that restored the workspace numbered it among its own,
        // and asked for again it is reached by that id on that instance:
        // the one the reference held may be another workspace's there.
        if (isRestored && identity) {
            if (m_requestedWorkspaceID)
                m_requestedWorkspaceID = identity;
            if (m_workspaceID)
                m_workspaceID = identity;
        }
        if (!identity || (m_requestedWorkspaceID && identity != m_requestedWorkspaceID)
            || (!m_shouldFollowAdoptedSessionWorkspace && m_workspaceID && identity != m_workspaceID)) {
            refuseAcknowledgedHost("The host returned a different workspace.");
            return;
        }
        m_workspaceID = identity;
    }
    if (m_factory) {
        const auto& socketName = m_core->socketName();
        if (isSuccessor) {
            m_factory->clearHandoverReach();
            m_daemonSocketName.clear();
        } else if (isRestored) {
            // The host that holds a restored workspace now may run another
            // build on another socket: a stopped host's record is taken in by
            // the next one on a sibling socket, and a holder's socket is served
            // by the host it handed its workspaces to. The carrier's pin moves
            // to the socket that answered. A pin the report of a gone daemon
            // has already dropped, or one to that very socket, is kept, and
            // with it the reach the carrier holds.
            m_daemonSocketName.clear();
            if (!socketName.empty() && !m_factory->pinDaemonSocket(socketName))
                m_factory->clearHandoverReach();
        }
        if ((!m_daemonSocketName.empty() && socketName != m_daemonSocketName)
            || (!socketName.empty() && !m_factory->pinDaemonSocket(socketName))) {
            refuseAcknowledgedHost("A different Ubiquitty host answered than the one this window was connected to.");
            return;
        }
        m_daemonSocketName = socketName;
    }
    m_sessionID = sessionID;
    m_ackSeen = true;
    m_hasEstablishedConnection = true;
    // The host holds no want of the connection before this one.
    m_historyPull.reset();
    m_initialSetupFailures = 0;
    m_initialSetupBudgetExhausted = false;
    // A focus reported before the host named the session is the pane's.
    reportPaneFocus();
    // The next drop tries the proof again: this handshake holds a secret
    // the host just issued.
    m_isResumeDialRefused = false;
    m_hasRetriedAfterLifecycleChange = false;
    m_hasDroppedGonePin = false;
    m_isAcknowledgedCarrierQUIC = m_transportIsQUIC;
    m_reconnectAttempts = 0; // the handshake landed; the next drop starts fast again
    // The size this client shows the pane at, with the pixels its frontend
    // published: HELLO carries a grid and no pixels, a workspace pane sends no
    // HELLO at all, and the drawable may have moved since either. Stated once
    // here, and again only when it changes.
    stateViewport();
    auto weakThis = weakPtr();
    m_observer->onSessionEstablished(sessionID);
    if (weakThis.get()) {
        transition(State::Connected, "");
    }
}

Ubiquitty::Terminal* RemoteSessionCore::screenTerminal(uint32_t sessionID)
{
    // A workspace carrier holds no terminal of its own: each pane's screen
    // replicates into the terminal of the pane core attached to it.
    return m_isMultiplexedWorkspace ? m_observer->paneTerminal(sessionID) : nullptr;
}

void RemoteSessionCore::onScreenChanged(uint32_t sessionID)
{
    m_isScreenDirty = true; // coalesced: handleData fires the observer once per feed
    m_trace.recordScreenApplied();
    if (m_isMultiplexedWorkspace) {
        m_observer->onPaneScreenChanged(sessionID);
    }
}

void RemoteSessionCore::onPaneFacts(uint32_t sessionID, const Protocol::Sync::PaneFacts& facts)
{
    if (m_isMultiplexedWorkspace) {
        m_observer->onWorkspacePaneFacts(sessionID, facts);
        return;
    }
    if (sessionID != m_sessionID)
        return;
    m_foregroundIsShell = facts.isAtPrompt;
    const std::uint64_t generation = screenOwner().m_generation.load(std::memory_order_acquire);
    auto weakThis = weakPtr();
    m_observer->onPaneFacts(facts);
    if (weakThis.get() && !m_stopped && m_ackSeen && m_sessionID == sessionID
        && screenOwner().m_generation.load(std::memory_order_acquire) == generation)
        m_observer->onTitle(facts.programTitle);
}

void RemoteSessionCore::onImagesChanged()
{
    // An image arrives on its own frame with no cell change behind it, so a client
    // that repaints on grid movement alone holds the frame it has until something
    // unrelated moves. Raised through the same coalescing flag a screen change
    // uses: handleData fires the observer once per feed however many landed.
    m_isScreenDirty = true;
}

void RemoteSessionCore::onWorkspaceStructure(std::uint32_t workspaceID)
{
    // A connection is sent the structure of the workspace its session is in,
    // and a listing is sent every workspace's; the mirror is of its own.
    const std::uint32_t own = m_workspaceID ? m_workspaceID : m_core->acknowledgedWorkspaceID();
    const bool shouldFollowSession = m_shouldFollowAdoptedSessionWorkspace && !m_requestedWorkspaceID;
    if (own == 0 || (workspaceID != own && !shouldFollowSession))
        return;
    // A withdrawn structure is a workspace that ended, which each pane's
    // exit says: the mirror stays what it was.
    const std::span<const std::uint8_t> record = m_core->workspaceStructure(workspaceID);
    if (record.empty())
        return;
    auto decoded = decodeWorkspaceStructure(record);
    if (!decoded || decoded->id != workspaceID || decoded->isRestorable())
        return;
    if (workspaceID != own && !std::ranges::any_of(decoded->windows, [&](const auto& window) {
            return std::ranges::any_of(window.tabs, [&](const auto& tab) {
                return std::ranges::any_of(tab.panes, [&](const auto& pane) { return pane.sessionID == m_sessionID; });
            });
        }))
        return;
    // Every structure, whether or not it changes what the mirror shows: a
    // leaf's generation is what keeps the rectangle on the picture held
    // until the checkpoint that resizes it lands.
    m_core->noteTopology(topologyOf(*decoded));
    std::unique_ptr<TreeWorkspace> structure(new (std::nothrow) TreeWorkspace(std::move(*decoded)));
    if (!structure)
        return;
    // A structure also revises for who is attached and what a pane runs,
    // which the mirror does not hold: the observer hears of the revisions
    // that change what it shows.
    std::string signature = Layout::mirrorSignature(*structure);
    if (signature == m_mirrorSignature) {
        m_workspaceStructureRevision = structure->revision;
        m_workspaceStructureSnapshot = std::move(structure);
        return;
    }
    if (!m_workspace) {
        m_workspace.reset(new (std::nothrow) Layout::Workspace());
        if (!m_workspace)
            return;
    }
    std::unique_ptr<Layout::Workspace> former;
    if (m_isAwaitingRestoredMirror) {
        former.reset(new (std::nothrow) Layout::Workspace());
        if (!former)
            return;
        std::swap(former, m_workspace);
    }
    std::vector<std::pair<uint32_t, uint32_t>> bound;
    const bool wasAdopted = Layout::adoptWorkspace(*structure, *m_workspace, [](void* context, Layout::PaneID pane, uint32_t sessionID) { static_cast<decltype(bound)*>(context)->emplace_back(std::to_underlying(pane), sessionID); }, &bound);
    if (!wasAdopted) {
        // A structure that does not hold together leaves the mirror as it was.
        if (former)
            m_workspace = std::move(former);
        return;
    }
    if (former) {
        // Every id the restoring host sent is its own; the places are what
        // the record kept. See Layout::pairRestoredPanes.
        const std::vector<Layout::RestoredPane> paired = Layout::pairRestoredPanes(*former, m_paneSessions, *m_workspace, bound);
        std::vector<std::pair<std::uint32_t, std::uint32_t>> carried;
        carried.reserve(paired.size());
        for (const Layout::RestoredPane& pane : paired)
            carried.emplace_back(pane.formerSessionID, pane.sessionID);
        m_carriedSessions = std::move(carried);
        m_restoration = Layout::followRestoration(m_restoration, paired);
        m_isAwaitingRestoredMirror = false;
    }
    m_workspaceStructureRevision = structure->revision;
    m_paneSessions = std::move(bound);
    m_workspaceName = structure->name;
    m_workspaceID = structure->id;
    m_isWorkspaceRestoredAfterHostRestart = structure->state == TreeWorkspaceState::RestoredAfterHostRestart;
    m_mirrorSignature = std::move(signature);
    m_workspaceStructureSnapshot = std::move(structure);
    if (m_mirrorGeneration != std::numeric_limits<std::uint64_t>::max())
        ++m_mirrorGeneration;
    // An ask the host performed is shown by this structure, whatever it
    // names: a tab another client chose since is the host's later word. One
    // not yet answered stands unless the structure already shows it, since
    // the structure may have left before the host read the ask.
    std::erase_if(m_frontTabAsks, [&](const FrontTabAsk& ask) {
        return ask.isAnswered || m_workspace->activeTab(ask.surface) == ask.tab;
    });
    m_observer->onWorkspaceChanged();
}

void RemoteSessionCore::onPaneEnded(std::uint32_t sessionID)
{
    if (!m_isMultiplexedWorkspace) {
        return;
    }
    m_observer->onPaneEnded(sessionID);
}

void RemoteSessionCore::onPaneFailed(std::uint32_t sessionID, std::string_view reason)
{
    if (m_isMultiplexedWorkspace) {
        m_observer->onPaneFailed(sessionID, reason);
        return;
    }
    m_failedPaneSession = sessionID;
    transition(State::Failed, std::string(reason));
}

void RemoteSessionCore::onPaneEffect(uint32_t sessionID, const Protocol::Sync::EffectView& effect, const Protocol::Sync::EffectBody& body, Protocol::Sync::EffectDisposition disposition)
{
    if (m_isMultiplexedWorkspace && m_observer)
        m_observer->onPaneEffect(sessionID, effect, body, disposition);
}

bool RemoteSessionCore::onPaneClipboardRequest(uint32_t sessionID, const Protocol::Sync::ClipboardRequestMessage& request)
{
    return m_isMultiplexedWorkspace && m_observer && m_observer->onPaneClipboardRequest(sessionID, request);
}

void RemoteSessionCore::onPaneNativeDragCommand(std::uint32_t sessionID, const Protocol::Sync::NativeDragCommand& command)
{
    if (m_isMultiplexedWorkspace && m_observer)
        m_observer->onPaneNativeDragCommand(sessionID, command);
}

void RemoteSessionCore::onNativeDragCommand(const Protocol::Sync::NativeDragCommand& command)
{
    if (!m_stopped && m_ackSeen && m_observer)
        m_observer->onNativeDragCommand(command);
}

bool RemoteSessionCore::sendNativeDragEvent(const DragDrop::NativeEvent& event, std::span<const char> bytes)
{
    if (m_stopped || !m_ackSeen)
        return false;
    auto& owner = screenOwner();
    if (!owner.m_core->sendNativeDragEvent(m_sessionID, event, bytes))
        return false;
    owner.flushScreen();
    return true;
}

void RemoteSessionCore::onPaneBound(uint32_t sessionID)
{
    if (m_isMultiplexedWorkspace) {
        m_observer->onPaneBound(sessionID);
        return;
    }
    // The host bound the failed pane again: its picture follows, and the
    // session is no longer failed.
    if (sessionID && sessionID == std::exchange(m_failedPaneSession, 0) && m_state == State::Failed)
        transition(State::Connected, {});
}

RemoteSessionCore& RemoteSessionCore::mirrorOwner()
{
    return m_workspaceConnection ? m_workspaceConnection->connection() : *this;
}

const RemoteSessionCore& RemoteSessionCore::mirrorOwner() const
{
    return m_workspaceConnection ? m_workspaceConnection->connection() : *this;
}

bool RemoteSessionCore::bringTabToFront(Layout::SurfaceID surface, Layout::TabID tab)
{
    RemoteSessionCore& owner = mirrorOwner();
    if (!owner.m_workspace || surface == Layout::invalidSurface || tab == Layout::invalidTab)
        return false;
    const auto asked = std::ranges::find(owner.m_frontTabAsks, surface, &FrontTabAsk::surface);
    if (asked != owner.m_frontTabAsks.end() ? asked->tab == tab : owner.m_workspace->activeTab(surface) == tab)
        return false;
    Ubiquitty::Session::WorkspaceIntent verb;
    verb.verb = Ubiquitty::Session::WorkspaceIntentKind::SetActiveTab;
    verb.surfaceID = std::to_underlying(surface);
    verb.tabID = std::to_underlying(tab);
    const uint32_t requestID = sendWorkspaceVerb(std::move(verb));
    if (requestID == 0)
        return false;
    // The later ask for a window replaces the earlier: the host performs them
    // in order, and the last is where the person left the window.
    if (asked != owner.m_frontTabAsks.end())
        owner.m_frontTabAsks.erase(asked);
    owner.m_frontTabAsks.push_back({ .requestID = requestID, .surface = surface, .tab = tab });
    return true;
}

Layout::TabID RemoteSessionCore::frontTab(Layout::SurfaceID surface) const
{
    const RemoteSessionCore& owner = mirrorOwner();
    if (!owner.m_workspace)
        return Layout::invalidTab;
    const auto asked = std::ranges::find(owner.m_frontTabAsks, surface, &FrontTabAsk::surface);
    return asked != owner.m_frontTabAsks.end() ? asked->tab : owner.m_workspace->activeTab(surface);
}

void RemoteSessionCore::noteFrontTabAnswer(const Ubiquitty::Session::WorkspaceIntentResult& result)
{
    const auto asked = std::ranges::find(m_frontTabAsks, result.requestID, &FrontTabAsk::requestID);
    if (asked == m_frontTabAsks.end())
        return;
    // A refusal leaves the host's tab in front, and a structure that already
    // shows the tab has nothing further to wait for.
    if (!result.wasPerformed || (m_workspace && m_workspace->activeTab(asked->surface) == asked->tab))
        m_frontTabAsks.erase(asked);
    else
        asked->isAnswered = true;
}

const std::string& RemoteSessionCore::workspaceName() const
{
    return m_workspaceConnection ? m_workspaceConnection->connection().workspaceName() : m_workspaceName;
}

bool RemoteSessionCore::isWorkspaceRestoredAfterHostRestart() const
{
    if (m_workspaceConnection)
        return m_workspaceConnection->connection().isWorkspaceRestoredAfterHostRestart();
    return m_isWorkspaceRestoredAfterHostRestart;
}

uint32_t RemoteSessionCore::sessionForPane(Layout::PaneID pane) const
{
    if (m_workspaceConnection) {
        return m_workspaceConnection->connection().sessionForPane(pane);
    }
    for (const auto& [bound, sessionID] : m_paneSessions)
        if (bound == std::to_underlying(pane))
            return sessionID;
    return 0;
}

Layout::PaneID RemoteSessionCore::paneForSession(uint32_t sessionID) const
{
    if (m_workspaceConnection) {
        return m_workspaceConnection->connection().paneForSession(sessionID);
    }
    for (const auto& [pane, bound] : m_paneSessions)
        if (bound == sessionID)
            return Layout::PaneID { pane };
    return Layout::invalidPane;
}

Layout::Restoration RemoteSessionCore::takeRestoration()
{
    return std::exchange(mirrorOwner().m_restoration, {});
}

std::uint64_t RemoteSessionCore::workspaceMirrorGeneration() const
{
    const auto& owner = mirrorOwner();
    return m_ackSeen && !m_stopped && owner.m_ackSeen && !owner.m_stopped && owner.m_workspace
            && !owner.m_mirrorSignature.empty() && owner.m_mirrorGeneration != std::numeric_limits<std::uint64_t>::max()
        ? owner.m_mirrorGeneration
        : 0;
}

const TreeWorkspace* RemoteSessionCore::workspaceStructureSnapshot() const
{
    const auto& owner = mirrorOwner();
    return workspaceMirrorGeneration() && owner.m_workspaceStructureSnapshot
            && owner.m_workspaceStructureSnapshot->revision == owner.m_workspaceStructureRevision
        ? owner.m_workspaceStructureSnapshot.get()
        : nullptr;
}

uint32_t RemoteSessionCore::sendWorkspaceVerb(Ubiquitty::Session::WorkspaceIntent verb)
{
    // Window closure requires the separately captured revision and confirmation.
    if (verb.verb == Ubiquitty::Session::WorkspaceIntentKind::CloseSurface || verb.verb == Ubiquitty::Session::WorkspaceIntentKind::CloseFocusedPane)
        return 0;
    if (m_workspaceConnection) {
        return m_workspaceConnection->sendWorkspaceVerb(*this, std::move(verb));
    }
    if (!m_ackSeen || m_nextWorkspaceVerbRequestID == std::numeric_limits<std::uint32_t>::max())
        return 0;
    verb.requestID = ++m_nextWorkspaceVerbRequestID;
    verb.workspaceID = m_workspaceID;
    return sendStructureCall(verb) ? verb.requestID : 0;
}

bool RemoteSessionCore::prepareWindowClose(std::uint64_t token)
{
    m_pendingWindowClose.reset();
    const auto& owner = mirrorOwner();
    if (!token || !m_ackSeen || m_stopped || !owner.m_ackSeen || owner.m_stopped || !owner.m_workspace || owner.m_mirrorSignature.empty()
        || !owner.m_workspaceID || !owner.m_mirrorGeneration || owner.m_mirrorGeneration == std::numeric_limits<std::uint64_t>::max())
        return false;
    const auto pane = paneForSession(m_sessionID);
    const auto surface = owner.m_workspace->surfaceHolding(pane);
    if (surface == Layout::invalidSurface)
        return false;
    PendingWindowClose pending;
    pending.token = token;
    pending.generation = owner.m_generation.load(std::memory_order_acquire);
    pending.mirrorGeneration = owner.m_mirrorGeneration;
    pending.structureRevision = owner.m_workspaceStructureRevision;
    if (!pending.structureRevision)
        return false;
    pending.daemonInstanceID = owner.m_daemonInstanceID;
    pending.workspaceID = owner.m_workspaceID;
    pending.sessionID = m_sessionID;
    pending.surface = surface;
    for (const auto& binding : owner.m_paneSessions) {
        if (owner.m_workspace->surfaceHolding(Layout::PaneID { binding.first }) == surface) {
            if (!binding.second)
                return false;
            pending.paneSessions.push_back(binding);
        }
    }
    std::size_t paneCount = 0;
    for (int tab = 0; tab < owner.m_workspace->tabCount(surface); ++tab) {
        const auto* tree = owner.m_workspace->paneTree(surface, owner.m_workspace->tabAt(surface, tab));
        if (!tree)
            return false;
        paneCount += static_cast<std::size_t>(tree->paneCount());
    }
    if (!paneCount || pending.paneSessions.size() != paneCount)
        return false;
    m_pendingWindowClose = std::move(pending);
    return true;
}

const RemoteSessionCore::PendingWindowClose* RemoteSessionCore::pendingWindowClose() const
{
    const auto& owner = mirrorOwner();
    if (!m_pendingWindowClose || !m_ackSeen || m_stopped || !owner.m_ackSeen || owner.m_stopped || !owner.m_workspace || owner.m_mirrorSignature.empty())
        return nullptr;
    const auto& pending = *m_pendingWindowClose;
    if (pending.generation != owner.m_generation.load(std::memory_order_acquire)
        || pending.mirrorGeneration != owner.m_mirrorGeneration || pending.daemonInstanceID != owner.m_daemonInstanceID
        || pending.structureRevision != owner.m_workspaceStructureRevision
        || pending.workspaceID != owner.m_workspaceID || pending.sessionID != m_sessionID
        || owner.m_workspace->surfaceHolding(paneForSession(m_sessionID)) != pending.surface)
        return nullptr;
    std::size_t index = 0;
    for (const auto& binding : owner.m_paneSessions) {
        if (owner.m_workspace->surfaceHolding(Layout::PaneID { binding.first }) != pending.surface)
            continue;
        if (index == pending.paneSessions.size() || binding != pending.paneSessions[index++])
            return nullptr;
    }
    return index == pending.paneSessions.size() ? &pending : nullptr;
}

std::uint64_t RemoteSessionCore::pendingWindowCloseToken() const
{
    const auto* pending = pendingWindowClose();
    return pending ? pending->token : 0;
}

std::uint32_t RemoteSessionCore::pendingWindowCloseSessionCount() const
{
    const auto* pending = pendingWindowClose();
    return pending ? static_cast<std::uint32_t>(pending->paneSessions.size()) : 0;
}

std::uint32_t RemoteSessionCore::answerWindowClose(std::uint64_t token, RemoteWindowCloseDecision decision)
{
    if (!m_pendingWindowClose || m_pendingWindowClose->token != token)
        return 0;
    const bool isCurrent = pendingWindowClose() != nullptr;
    const auto surface = m_pendingWindowClose->surface;
    const auto generation = m_pendingWindowClose->generation;
    const auto revision = m_pendingWindowClose->structureRevision;
    const auto sessionID = m_pendingWindowClose->sessionID;
    m_pendingWindowClose.reset();
    if (!isCurrent || decision != RemoteWindowCloseDecision::CloseWindow)
        return 0;
    auto endpoint = weakPtr();
    auto owner = mirrorOwner().weakPtr();
    std::uint32_t requestID = 0;
    if (m_workspaceConnection)
        requestID = m_workspaceConnection->sendWindowClose(*this, surface, revision);
    else if (m_nextWorkspaceVerbRequestID != std::numeric_limits<std::uint32_t>::max()) {
        const auto candidate = ++m_nextWorkspaceVerbRequestID;
        if (sendWindowCloseCall(candidate, surface, revision))
            requestID = candidate;
    }
    // A synchronous transport callback can retire the original attachment.
    if (!endpoint || !owner || !endpoint->m_ackSeen || endpoint->m_stopped || endpoint->m_sessionID != sessionID
        || generation != owner->m_generation.load(std::memory_order_acquire))
        return 0;
    return requestID;
}

bool RemoteSessionCore::sendWindowCloseCall(std::uint32_t requestID, Layout::SurfaceID surface, std::uint64_t revision)
{
    Ubiquitty::Session::WorkspaceIntent verb;
    verb.requestID = requestID;
    verb.verb = Ubiquitty::Session::WorkspaceIntentKind::CloseSurface;
    verb.surfaceID = std::to_underlying(surface);
    return revision && sendStructureCall(verb, revision);
}

bool RemoteSessionCore::sendStructureCall(const Ubiquitty::Session::WorkspaceIntent& verb, std::uint64_t revision)
{
    if (!verb.requestID || !m_channel || !m_ackSeen || m_stopped
        || !m_core->isScreenAttached() || m_structureCalls.size() >= Protocol::Sync::maxPendingStructureConsents)
        return false;
    using Codec::JSON::Value;
    using WorkspaceVerb = Ubiquitty::Session::WorkspaceIntentKind;
    Value parameters = Value::object();
    std::string_view method;
    std::uint32_t targetID = 0;
    std::string expectedText;
    std::uint32_t destinationID = 0;
    std::uint32_t destinationIndex = 0;
    std::uint32_t sourceTabID = 0;
    std::uint32_t destinationPaneID = 0;
    std::uint32_t destinationTabID = 0;
    const bool hasSourcePane = m_workspace && verb.paneID && verb.surfaceID
        && m_workspace->surfaceHolding(Layout::PaneID { verb.paneID }) == Layout::SurfaceID { verb.surfaceID };
    switch (verb.verb) {
    case WorkspaceVerb::BalanceActiveTab:
    case WorkspaceVerb::AdjustFocusedPaneRatio:
    case WorkspaceVerb::SetDividerRatio: {
        if (!m_workspace || !m_workspaceStructureRevision)
            return false;
        const Layout::SurfaceID window { verb.surfaceID };
        const auto tab = verb.tabID ? Layout::TabID { verb.tabID } : verb.paneID ? m_workspace->tabHolding(window, Layout::PaneID { verb.paneID })
                                                                                 : m_workspace->activeTab(window);
        const auto* tree = m_workspace->paneTree(window, tab);
        if (!tree)
            return false;
        sourceTabID = std::to_underlying(tab);
        parameters.set("window", Value::number(verb.surfaceID));
        parameters.set("tab", Value::number(sourceTabID));
        parameters.set("expectedRevision", Value::string(std::to_string(m_workspaceStructureRevision)));
        if (verb.verb == WorkspaceVerb::BalanceActiveTab) {
            method = "tab.balance";
        } else if (verb.verb == WorkspaceVerb::SetDividerRatio) {
            if (!std::isfinite(verb.ratio) || verb.ratio < 0 || verb.ratio > 1)
                return false;
            const auto path = Panes::dividerPath(*tree, verb.splitIndex);
            if (!path)
                return false;
            auto selector = Value::array();
            for (std::size_t index = 0; index < path->length; ++index)
                selector.push(Value::number(path->children[index]));
            expectedText = Codec::JSON::serialize(selector);
            method = "divider.setRatio";
            parameters.set("path", std::move(selector));
            parameters.set("ratio", Value::number(verb.ratio));
        } else {
            targetID = verb.paneID ? verb.paneID : static_cast<std::uint32_t>(tree->focusedPaneID());
            if (!tree->holdsPane(static_cast<int>(targetID)) || !std::isfinite(verb.ratio) || verb.ratio <= 0 || verb.ratio > 1)
                return false;
            switch (static_cast<Panes::Direction>(verb.direction)) {
            case Panes::Direction::Left:
                expectedText = "left";
                break;
            case Panes::Direction::Right:
                expectedText = "right";
                break;
            case Panes::Direction::Up:
                expectedText = "up";
                break;
            case Panes::Direction::Down:
                expectedText = "down";
                break;
            default:
                return false;
            }
            method = "pane.adjustRatio";
            parameters.set("pane", Value::number(targetID));
            parameters.set("direction", Value::string(expectedText));
            parameters.set("step", Value::number(verb.ratio));
        }
        break;
    }
    case WorkspaceVerb::MovePaneToTab:
    case WorkspaceVerb::MoveTabToPane:
    case WorkspaceVerb::MovePaneToNewTab: {
        if (!m_workspace || !m_workspaceStructureRevision)
            return false;
        const bool isTab = verb.verb == WorkspaceVerb::MoveTabToPane;
        if (isTab ? !m_workspace->paneTree(Layout::SurfaceID { verb.surfaceID }, Layout::TabID { verb.tabID }) : !hasSourcePane)
            return false;
        const auto targetWindow = m_workspace->surfaceHolding(Layout::PaneID { verb.targetPaneID });
        if (targetWindow == Layout::invalidSurface)
            return false;
        sourceTabID = isTab ? verb.tabID : std::to_underlying(m_workspace->tabHolding(Layout::SurfaceID { verb.surfaceID }, Layout::PaneID { verb.paneID }));
        destinationID = std::to_underlying(targetWindow);
        targetID = isTab ? verb.tabID : verb.paneID;
        parameters.set("window", Value::number(verb.surfaceID));
        parameters.set(isTab ? "tab" : "pane", Value::number(targetID));
        parameters.set("expectedRevision", Value::string(std::to_string(m_workspaceStructureRevision)));
        if (verb.verb == WorkspaceVerb::MovePaneToNewTab) {
            method = "pane.moveToNewTab";
            parameters.set("targetWindow", Value::number(destinationID));
        } else {
            destinationPaneID = verb.targetPaneID;
            destinationTabID = std::to_underlying(m_workspace->tabHolding(targetWindow, Layout::PaneID { destinationPaneID }));
            if (isTab ? sourceTabID == destinationTabID : verb.targetTabID != destinationTabID || verb.paneID == destinationPaneID)
                return false;
            if ((verb.orientation != std::to_underlying(Panes::Orientation::Horizontal) && verb.orientation != std::to_underlying(Panes::Orientation::Vertical))
                || (verb.side != std::to_underlying(Panes::InsertSide::First) && verb.side != std::to_underlying(Panes::InsertSide::Second))
                || (verb.insertTarget != std::to_underlying(Panes::InsertTarget::Pane) && verb.insertTarget != std::to_underlying(Panes::InsertTarget::Root)))
                return false;
            method = isTab ? "tab.moveToPane" : "pane.moveToTab";
            if (!isTab)
                parameters.set("tab", Value::number(destinationTabID));
            parameters.set("target", Value::number(destinationPaneID));
            const std::string orientation = verb.orientation == std::to_underlying(Panes::Orientation::Horizontal) ? "horizontal" : "vertical";
            const std::string side = verb.side == std::to_underlying(Panes::InsertSide::First) ? "before" : "after";
            const std::string insertTarget = verb.insertTarget == std::to_underlying(Panes::InsertTarget::Root) ? "root" : "pane";
            parameters.set("orientation", Value::string(orientation));
            parameters.set("side", Value::string(side));
            parameters.set("insertTarget", Value::string(insertTarget));
            expectedText = orientation + ":" + side + ":" + insertTarget;
        }
        break;
    }
    case WorkspaceVerb::ExtractPaneToTab:
    case WorkspaceVerb::ExtractPaneToSurface:
    case WorkspaceVerb::ExtractTabToSurface: {
        if (!m_workspace || !m_workspaceStructureRevision)
            return false;
        const bool isTab = verb.verb == WorkspaceVerb::ExtractTabToSurface;
        if (isTab ? !m_workspace->paneTree(Layout::SurfaceID { verb.surfaceID }, Layout::TabID { verb.tabID }) : !hasSourcePane)
            return false;
        targetID = isTab ? verb.tabID : verb.paneID;
        destinationID = isTab ? verb.tabID : std::to_underlying(m_workspace->tabHolding(Layout::SurfaceID { verb.surfaceID }, Layout::PaneID { verb.paneID }));
        method = isTab ? "tab.extractWindow" : verb.verb == WorkspaceVerb::ExtractPaneToTab ? "pane.extractTab"
                                                                                            : "pane.extractWindow";
        parameters.set("window", Value::number(verb.surfaceID));
        parameters.set(isTab ? "tab" : "pane", Value::number(targetID));
        parameters.set("expectedRevision", Value::string(std::to_string(m_workspaceStructureRevision)));
        break;
    }
    case WorkspaceVerb::MovePane:
    case WorkspaceVerb::SwapPanes: {
        if (!hasSourcePane || !m_workspaceStructureRevision || verb.paneID == verb.targetPaneID
            || m_workspace->surfaceHolding(Layout::PaneID { verb.targetPaneID }) != Layout::SurfaceID { verb.surfaceID })
            return false;
        const Layout::SurfaceID window { verb.surfaceID };
        const auto tab = m_workspace->activeTab(window);
        if (m_workspace->tabHolding(window, Layout::PaneID { verb.paneID }) != tab
            || m_workspace->tabHolding(window, Layout::PaneID { verb.targetPaneID }) != tab)
            return false;
        targetID = verb.paneID;
        destinationID = verb.targetPaneID;
        method = verb.verb == WorkspaceVerb::MovePane ? "pane.move" : "pane.swap";
        parameters.set("window", Value::number(verb.surfaceID));
        parameters.set("pane", Value::number(targetID));
        parameters.set("target", Value::number(destinationID));
        parameters.set("expectedRevision", Value::string(std::to_string(m_workspaceStructureRevision)));
        if (verb.verb == WorkspaceVerb::MovePane) {
            if ((verb.orientation != std::to_underlying(Panes::Orientation::Horizontal) && verb.orientation != std::to_underlying(Panes::Orientation::Vertical))
                || (verb.side != std::to_underlying(Panes::InsertSide::First) && verb.side != std::to_underlying(Panes::InsertSide::Second)))
                return false;
            const std::string orientation = verb.orientation == std::to_underlying(Panes::Orientation::Horizontal) ? "horizontal" : "vertical";
            const std::string side = verb.side == std::to_underlying(Panes::InsertSide::First) ? "before" : "after";
            parameters.set("orientation", Value::string(orientation));
            parameters.set("side", Value::string(side));
            expectedText = orientation + ":" + side;
        }
        break;
    }
    case WorkspaceVerb::SetActiveSurface:
        if (!m_workspace || !m_workspace->surface(Layout::SurfaceID { verb.surfaceID }))
            return false;
        method = "window.activate";
        targetID = verb.surfaceID;
        parameters.set("window", Value::number(targetID));
        break;
    case WorkspaceVerb::SetActiveTab:
        if (!m_workspace || !m_workspace->paneTree(Layout::SurfaceID { verb.surfaceID }, Layout::TabID { verb.tabID }))
            return false;
        method = "tab.activate";
        targetID = verb.tabID;
        destinationID = verb.surfaceID;
        parameters.set("tab", Value::number(targetID));
        break;
    case WorkspaceVerb::FocusPane:
        if (!hasSourcePane)
            return false;
        method = "pane.focus";
        targetID = verb.paneID;
        destinationID = verb.surfaceID;
        parameters.set("pane", Value::number(targetID));
        break;
    case WorkspaceVerb::MoveFocus: {
        if (!m_workspace || !m_workspaceStructureRevision || !m_workspace->surface(Layout::SurfaceID { verb.surfaceID }))
            return false;
        switch (static_cast<Panes::Direction>(verb.direction)) {
        case Panes::Direction::Left:
            expectedText = "left";
            break;
        case Panes::Direction::Right:
            expectedText = "right";
            break;
        case Panes::Direction::Up:
            expectedText = "up";
            break;
        case Panes::Direction::Down:
            expectedText = "down";
            break;
        default:
            return false;
        }
        targetID = std::to_underlying(m_workspace->focusedPane(Layout::SurfaceID { verb.surfaceID }));
        if (!targetID)
            return false;
        method = "window.moveFocus";
        destinationID = verb.surfaceID;
        parameters.set("window", Value::number(destinationID));
        parameters.set("pane", Value::number(targetID));
        parameters.set("direction", Value::string(expectedText));
        parameters.set("expectedRevision", Value::string(std::to_string(m_workspaceStructureRevision)));
        break;
    }
    case WorkspaceVerb::ToggleZoom:
        if (!hasSourcePane || !m_workspaceStructureRevision)
            return false;
        method = "pane.zoom";
        targetID = verb.paneID;
        expectedText = m_workspace->zoomedPane(Layout::SurfaceID { verb.surfaceID }) == Layout::PaneID { verb.paneID } ? "unzoomed" : "zoomed";
        parameters.set("pane", Value::number(targetID));
        parameters.set("state", Value::string(expectedText));
        parameters.set("expectedRevision", Value::string(std::to_string(m_workspaceStructureRevision)));
        break;
    case WorkspaceVerb::CloseSurface:
        if (!revision || !verb.surfaceID)
            return false;
        method = "window.close";
        targetID = verb.surfaceID;
        parameters.set("window", Value::number(verb.surfaceID));
        parameters.set("expectedRevision", Value::string(std::to_string(revision)));
        break;
    case WorkspaceVerb::AddSurface:
        if (!hasSourcePane || !m_workspaceStructureRevision)
            return false;
        method = "workspace.addWindow";
        targetID = verb.paneID;
        parameters.set("window", Value::number(verb.surfaceID));
        parameters.set("pane", Value::number(verb.paneID));
        parameters.set("expectedRevision", Value::string(std::to_string(m_workspaceStructureRevision)));
        break;
    case WorkspaceVerb::AddTab:
        if (!hasSourcePane)
            return false;
        method = "workspace.addTab";
        targetID = verb.surfaceID;
        parameters.set("window", Value::number(verb.surfaceID));
        parameters.set("pane", Value::number(verb.paneID));
        break;
    case WorkspaceVerb::SplitFocusedPane:
        if (!hasSourcePane || (verb.orientation != std::to_underlying(Panes::Orientation::Vertical) && verb.orientation != std::to_underlying(Panes::Orientation::Horizontal))
            || !std::isfinite(verb.ratio) || verb.ratio < 0 || verb.ratio > 1)
            return false;
        method = "workspace.split";
        targetID = verb.paneID;
        parameters.set("pane", Value::number(verb.paneID));
        parameters.set("orientation", Value::string(verb.orientation == std::to_underlying(Panes::Orientation::Horizontal) ? "horizontal" : "vertical"));
        parameters.set("ratio", Value::number(verb.ratio));
        break;
    case WorkspaceVerb::CloseTab:
        if (!m_workspace || !m_workspace->paneTree(Layout::SurfaceID { verb.surfaceID }, Layout::TabID { verb.tabID }))
            return false;
        method = "workspace.closeTab";
        targetID = verb.tabID;
        parameters.set("tab", Value::number(verb.tabID));
        break;
    case WorkspaceVerb::ClosePane:
        if (!hasSourcePane)
            return false;
        method = "workspace.closePane";
        targetID = verb.paneID;
        parameters.set("pane", Value::number(verb.paneID));
        break;
    case WorkspaceVerb::MoveTabToSurface: {
        if (!m_workspace || !m_workspace->paneTree(Layout::SurfaceID { verb.surfaceID }, Layout::TabID { verb.tabID }))
            return false;
        const auto destination = m_workspace->surfaceHolding(Layout::PaneID { verb.targetPaneID });
        if (destination == Layout::invalidSurface || destination == Layout::SurfaceID { verb.surfaceID })
            return false;
        const auto count = m_workspace->tabCount(destination);
        if (count < 0 || count >= Layout::maxTabsPerSurface)
            return false;
        method = "tab.move";
        targetID = verb.tabID;
        destinationID = std::to_underlying(destination);
        destinationIndex = static_cast<std::uint32_t>(count);
        parameters.set("tab", Value::number(targetID));
        parameters.set("window", Value::number(destinationID));
        parameters.set("index", Value::number(destinationIndex));
        break;
    }
    case WorkspaceVerb::RenameWorkspace:
    case WorkspaceVerb::RenameTab:
        if (!m_workspace || verb.title.size() > maxWorkspaceNameBytes
            || Unicode::wellFormedUTF8PrefixLength(verb.title) != verb.title.size())
            return false;
        if (verb.verb == WorkspaceVerb::RenameWorkspace) {
            if (!m_workspaceID)
                return false;
            method = "workspace.rename";
            targetID = m_workspaceID;
            parameters.set("workspace", Value::number(targetID));
        } else {
            if (!m_workspace->paneTree(Layout::SurfaceID { verb.surfaceID }, Layout::TabID { verb.tabID }))
                return false;
            method = "tab.rename";
            targetID = verb.tabID;
            parameters.set("tab", Value::number(targetID));
        }
        expectedText = verb.title;
        parameters.set("name", Value::string(expectedText));
        break;
    case WorkspaceVerb::LabelClient:
        if (!m_workspace || !m_workspaceID || Identity::checkLabel(verb.title) != Identity::LabelVerdict::Accepted
            || Unicode::wellFormedUTF8PrefixLength(verb.title) != verb.title.size())
            return false;
        method = "client.label";
        expectedText = verb.title;
        parameters.set("label", Value::string(expectedText));
        break;
    case WorkspaceVerb::LabelSurface:
    case WorkspaceVerb::LabelPane:
    case WorkspaceVerb::LabelSession: {
        const bool hasTarget = verb.verb == WorkspaceVerb::LabelSurface ? m_workspace && m_workspace->surface(Layout::SurfaceID { verb.surfaceID }) : hasSourcePane;
        if (!hasTarget || Identity::checkLabel(verb.title) != Identity::LabelVerdict::Accepted
            || Unicode::wellFormedUTF8PrefixLength(verb.title) != verb.title.size())
            return false;
        const auto kind = verb.verb == WorkspaceVerb::LabelSurface ? Identity::ObjectKind::Window
            : verb.verb == WorkspaceVerb::LabelPane                ? Identity::ObjectKind::Pane
                                                                   : Identity::ObjectKind::Session;
        targetID = verb.verb == WorkspaceVerb::LabelSurface ? verb.surfaceID
            : verb.verb == WorkspaceVerb::LabelPane         ? verb.paneID
                                                            : sessionForPane(Layout::PaneID { verb.paneID });
        if (!targetID)
            return false;
        method = "label.set";
        parameters.set("object", Value::string(Identity::format({ kind, targetID })));
        expectedText = verb.title;
        parameters.set("label", Value::string(expectedText));
        break;
    }
    default:
        return false;
    }
    Value request = Value::object();
    request.set("jsonrpc", Value::string("2.0"));
    const auto prefix = verb.verb == WorkspaceVerb::CloseSurface ? "ubiquitty.window-close." : "ubiquitty.tree.";
    request.set("id", Value::string(prefix + std::to_string(verb.requestID)));
    request.set("method", Value::string(std::string(method)));
    request.set("params", std::move(parameters));
    if (!m_core->call(Codec::JSON::serialize(request)))
        return false;
    m_structureCalls.push_back({ verb.requestID, verb.verb, targetID, std::move(expectedText), destinationID, destinationIndex, verb.surfaceID, sourceTabID, destinationPaneID, destinationTabID, verb.ratio });
    const auto generation = m_generation.load(std::memory_order_acquire);
    const auto weakThis = weakPtr();
    flushScreen();
    return weakThis && !weakThis->m_stopped && weakThis->m_ackSeen
        && generation == weakThis->m_generation.load(std::memory_order_acquire);
}

void RemoteSessionCore::receiveStructureAnswer(const std::string& response)
{
    bool isJSON = false; // bool-ok: the parser's output flag
    const auto answer = Codec::JSON::parse(response, isJSON);
    const auto* id = isJSON ? answer.find("id") : nullptr;
    if (!id || !id->isString())
        return;
    const auto found = std::ranges::find_if(m_structureCalls, [&](const auto& call) {
        const auto prefix = call.verb == Ubiquitty::Session::WorkspaceIntentKind::CloseSurface ? "ubiquitty.window-close." : "ubiquitty.tree.";
        return id->asString() == prefix + std::to_string(call.requestID);
    });
    if (found == m_structureCalls.end())
        return;
    const auto held = std::move(*found);
    m_structureCalls.erase(found);
    const auto* result = answer.find("result");
    Ubiquitty::Session::WorkspaceIntentResult resultAdapter;
    resultAdapter.requestID = held.requestID;
    if (answer.stringOr("jsonrpc") == "2.0" && !answer.find("error") && result && result->isObject()) {
        using WorkspaceVerb = Ubiquitty::Session::WorkspaceIntentKind;
        const auto identifier = [&](const char* name, Identity::ObjectKind kind) -> std::uint32_t {
            const auto parsed = Identity::parseObjectID(result->stringOr(name));
            return parsed && parsed->kind == kind && parsed->number <= std::numeric_limits<std::uint32_t>::max()
                ? static_cast<std::uint32_t>(parsed->number)
                : 0;
        };
        if (held.verb == WorkspaceVerb::BalanceActiveTab || held.verb == WorkspaceVerb::AdjustFocusedPaneRatio || held.verb == WorkspaceVerb::SetDividerRatio) {
            resultAdapter.wasPerformed = identifier("window", Identity::ObjectKind::Window) == held.sourceWindowID
                && identifier("tab", Identity::ObjectKind::Tab) == held.sourceTabID;
            if (held.verb != WorkspaceVerb::BalanceActiveTab) {
                const auto* ratio = result->find(held.verb == WorkspaceVerb::SetDividerRatio ? "ratio" : "step");
                resultAdapter.wasPerformed = resultAdapter.wasPerformed && ratio && ratio->isNumber() && ratio->asNumber() == held.requestedRatio;
                if (held.verb == WorkspaceVerb::SetDividerRatio) {
                    const auto* path = result->find("path");
                    resultAdapter.wasPerformed = resultAdapter.wasPerformed && path && path->isArray() && Codec::JSON::serialize(*path) == held.expectedText;
                } else {
                    resultAdapter.wasPerformed = resultAdapter.wasPerformed && identifier("pane", Identity::ObjectKind::Pane) == held.targetID
                        && result->stringOr("direction") == held.expectedText;
                }
            }
        } else if (held.verb == WorkspaceVerb::AddSurface) {
            resultAdapter.surfaceID = identifier("window", Identity::ObjectKind::Window);
            resultAdapter.tabID = identifier("tab", Identity::ObjectKind::Tab);
            resultAdapter.paneID = identifier("pane", Identity::ObjectKind::Pane);
            resultAdapter.sessionID = identifier("session", Identity::ObjectKind::Session);
            resultAdapter.wasPerformed = identifier("sourceWindow", Identity::ObjectKind::Window) == held.sourceWindowID
                && identifier("sourcePane", Identity::ObjectKind::Pane) == held.targetID
                && resultAdapter.surfaceID && resultAdapter.surfaceID != held.sourceWindowID
                && resultAdapter.tabID && resultAdapter.paneID && resultAdapter.paneID != held.targetID && resultAdapter.sessionID;
        } else if (held.verb == WorkspaceVerb::AddTab || held.verb == WorkspaceVerb::SplitFocusedPane) {
            resultAdapter.paneID = identifier("pane", Identity::ObjectKind::Pane);
            resultAdapter.sessionID = identifier("session", Identity::ObjectKind::Session);
            resultAdapter.tabID = held.verb == WorkspaceVerb::AddTab ? identifier("tab", Identity::ObjectKind::Tab) : 0;
            resultAdapter.wasPerformed = resultAdapter.paneID && resultAdapter.sessionID
                && (held.verb != WorkspaceVerb::AddTab || resultAdapter.tabID);
        } else if (held.verb == WorkspaceVerb::MovePaneToTab || held.verb == WorkspaceVerb::MoveTabToPane || held.verb == WorkspaceVerb::MovePaneToNewTab) {
            resultAdapter.wasPerformed = identifier("sourceWindow", Identity::ObjectKind::Window) == held.sourceWindowID
                && identifier("sourceTab", Identity::ObjectKind::Tab) == held.sourceTabID
                && identifier("window", Identity::ObjectKind::Window) == held.destinationID
                && (held.verb == WorkspaceVerb::MoveTabToPane || identifier("pane", Identity::ObjectKind::Pane) == held.targetID)
                && (held.verb == WorkspaceVerb::MovePaneToNewTab ? identifier("tab", Identity::ObjectKind::Tab) != 0
                                                                 : identifier("tab", Identity::ObjectKind::Tab) == held.destinationTabID
                            && identifier("target", Identity::ObjectKind::Pane) == held.destinationPaneID
                            && result->stringOr("orientation") + ":" + result->stringOr("side") + ":" + result->stringOr("insertTarget") == held.expectedText);
        } else if (held.verb == WorkspaceVerb::ExtractPaneToTab || held.verb == WorkspaceVerb::ExtractPaneToSurface || held.verb == WorkspaceVerb::ExtractTabToSurface) {
            const auto window = identifier("window", Identity::ObjectKind::Window);
            const auto tab = identifier("tab", Identity::ObjectKind::Tab);
            const bool isTab = held.verb == WorkspaceVerb::ExtractTabToSurface;
            resultAdapter.wasPerformed = identifier("sourceWindow", Identity::ObjectKind::Window) == held.sourceWindowID
                && window && tab
                && (isTab ? tab == held.targetID
                          : identifier("pane", Identity::ObjectKind::Pane) == held.targetID
                            && identifier("sourceTab", Identity::ObjectKind::Tab) == held.destinationID && tab != held.destinationID)
                && (held.verb == WorkspaceVerb::ExtractPaneToTab ? window == held.sourceWindowID : window != held.sourceWindowID);
            if (held.verb == WorkspaceVerb::ExtractPaneToTab)
                resultAdapter.tabID = tab;
            else
                resultAdapter.surfaceID = window;
        } else if (held.verb == WorkspaceVerb::MovePane || held.verb == WorkspaceVerb::SwapPanes) {
            resultAdapter.wasPerformed = identifier("pane", Identity::ObjectKind::Pane) == held.targetID
                && identifier("target", Identity::ObjectKind::Pane) == held.destinationID
                && identifier("window", Identity::ObjectKind::Window) == held.sourceWindowID
                && (held.verb == WorkspaceVerb::SwapPanes || result->stringOr("orientation") + ":" + result->stringOr("side") == held.expectedText);
        } else if (held.verb == WorkspaceVerb::SetActiveSurface) {
            resultAdapter.wasPerformed = identifier("window", Identity::ObjectKind::Window) == held.targetID;
        } else if (held.verb == WorkspaceVerb::SetActiveTab || held.verb == WorkspaceVerb::FocusPane) {
            resultAdapter.wasPerformed = identifier("window", Identity::ObjectKind::Window) == held.destinationID
                && identifier(held.verb == WorkspaceVerb::SetActiveTab ? "tab" : "pane", held.verb == WorkspaceVerb::SetActiveTab ? Identity::ObjectKind::Tab : Identity::ObjectKind::Pane) == held.targetID;
        } else if (held.verb == WorkspaceVerb::MoveFocus) {
            resultAdapter.wasPerformed = identifier("window", Identity::ObjectKind::Window) == held.destinationID
                && identifier("fromPane", Identity::ObjectKind::Pane) == held.targetID
                && identifier("pane", Identity::ObjectKind::Pane) && result->stringOr("direction") == held.expectedText;
        } else if (held.verb == WorkspaceVerb::ToggleZoom) {
            resultAdapter.wasPerformed = identifier("pane", Identity::ObjectKind::Pane) == held.targetID
                && result->stringOr("state") == held.expectedText;
        } else if (held.verb == WorkspaceVerb::MoveTabToSurface) {
            const auto* index = result->find("index");
            resultAdapter.wasPerformed = identifier("tab", Identity::ObjectKind::Tab) == held.targetID
                && identifier("window", Identity::ObjectKind::Window) == held.destinationID
                && index && index->isNumber() && index->asNumber() == held.destinationIndex;
        } else if (held.verb == WorkspaceVerb::RenameWorkspace || held.verb == WorkspaceVerb::RenameTab) {
            const auto* name = result->find("name");
            const auto kind = held.verb == WorkspaceVerb::RenameWorkspace ? Identity::ObjectKind::Workspace : Identity::ObjectKind::Tab;
            resultAdapter.wasPerformed = identifier(held.verb == WorkspaceVerb::RenameWorkspace ? "workspace" : "tab", kind) == held.targetID
                && name && name->isString() && name->asString() == held.expectedText;
        } else if (held.verb == WorkspaceVerb::LabelClient) {
            const auto* label = result->find("label");
            resultAdapter.wasPerformed = identifier("client", Identity::ObjectKind::Client) != 0
                && label && label->isString() && label->asString() == held.expectedText;
        } else if (held.verb == WorkspaceVerb::LabelSurface || held.verb == WorkspaceVerb::LabelPane || held.verb == WorkspaceVerb::LabelSession) {
            const auto* label = result->find("label");
            const auto kind = held.verb == WorkspaceVerb::LabelSurface ? Identity::ObjectKind::Window
                : held.verb == WorkspaceVerb::LabelPane                ? Identity::ObjectKind::Pane
                                                                       : Identity::ObjectKind::Session;
            resultAdapter.wasPerformed = identifier("object", kind) == held.targetID
                && label && label->isString() && label->asString() == held.expectedText;
        } else {
            const auto* ended = result->find("sessionsEnded");
            const auto kind = held.verb == WorkspaceVerb::CloseSurface ? Identity::ObjectKind::Window
                : held.verb == WorkspaceVerb::CloseTab                 ? Identity::ObjectKind::Tab
                                                                       : Identity::ObjectKind::Pane;
            const auto* name = held.verb == WorkspaceVerb::CloseSurface ? "window" : held.verb == WorkspaceVerb::CloseTab ? "tab"
                                                                                                                          : "pane";
            resultAdapter.wasPerformed = identifier(name, kind) == held.targetID
                && ended && ended->isNumber() && ended->asNumber() >= 0 && ended->asNumber() <= maxStructureLeaves
                && ended->asNumber() == static_cast<double>(static_cast<std::uint32_t>(ended->asNumber()));
        }
    }
    if (!resultAdapter.wasPerformed)
        resultAdapter.paneID = resultAdapter.sessionID = resultAdapter.tabID = resultAdapter.surfaceID = 0;
    // This local completion value comes only from the correlated canonical Result.
    noteFrontTabAnswer(resultAdapter);
    m_observer->onWorkspaceVerbResult(resultAdapter);
}

void RemoteSessionCore::receiveClipboardServingAnswer(const std::string& answer)
{
    if (!m_clipboardServingCallID || !canChangeClipboardServing())
        return;
    bool parsed = false;
    const auto decoded = Codec::JSON::parse(answer, parsed);
    if (!parsed || !decoded.isObject())
        return;
    const auto* id = decoded.find("id");
    if (!id || !id->isNumber() || id->asNumber() != static_cast<double>(*m_clipboardServingCallID))
        return;
    m_clipboardServingCallID.reset();
    const auto* result = decoded.find("result");
    const auto* enabled = result && result->isObject() ? result->find("enabled") : nullptr;
    if (decoded.stringOr("jsonrpc") != "2.0" || decoded.find("error") || !result || result->members().size() != 1
        || !enabled || !enabled->isBool() || enabled->asBool() != (m_clipboardServing == Clipboard::Serving::Enabled))
        return;
    m_didClaimClipboardServe = enabled->asBool();
    if (m_observer) {
        m_observer->onClipboardRoutingChanged();
    }
}

void RemoteSessionCore::onClipboardRequest(const Protocol::Sync::ClipboardRequestMessage& request)
{
    if (m_observer)
        m_observer->onClipboardRequest(request);
}

void RemoteSessionCore::onClipboardWriteItems(ClipboardBuffer buffer, std::span<const ClipboardItem> items)
{
    if (m_observer)
        m_observer->onClipboardWriteItems(buffer, items);
}

void RemoteSessionCore::onNotificationRequest(const NotificationRequest& request)
{
    if (m_observer)
        m_observer->onNotificationRequest(request);
}

void RemoteSessionCore::onAttention()
{
    if (m_observer)
        m_observer->onAttention();
}

void RemoteSessionCore::onHostEffectHeld(std::uint64_t token, Capability capability)
{
    if (m_observer)
        m_observer->onHostEffectHeld(token, capability);
}

void RemoteSessionCore::onHostEffectWithdrawn(std::uint64_t token, Protocol::Sync::HostEffectWithdrawal reason)
{
    if (m_observer)
        m_observer->onHostEffectWithdrawn(token, reason);
}

void RemoteSessionCore::onCommandDone(const Protocol::Sync::EffectView& effect, const Protocol::Sync::CommandFinishedEffect& command, Protocol::Sync::EffectDisposition disposition) { m_observer->onCommandDone(effect, command, disposition); }

void RemoteSessionCore::onBell() { m_observer->onBell(); }

void RemoteSessionCore::onExit()
{
    if (m_hostExited) {
        return;
    }
    m_hostExited = true; // the host shell ended — don't reconnect
    m_observer->onExit();
}

// ---- helpers ----------------------------------------------------------

void RemoteSessionCore::transition(State state, const std::string& detail)
{
    m_state = state;
    // Record the phase change on the trace timeline so the Sync Debug window
    // surfaces reconnect churn / deploy / fallback with timestamps.
    const char* label = "";
    switch (state) {
    case State::Idle:
        label = "Idle";
        break;
    case State::Connecting:
        label = "Connecting";
        break;
    case State::Connected:
        label = "Connected";
        break;
    case State::Reconnecting:
        label = "Reconnecting";
        break;
    case State::Failed:
        label = "Failed";
        break;
    case State::Deploying:
        label = "Deploying";
        break;
    }
    m_trace.recordEvent(detail.empty() ? std::string(label) : std::string(label) + " — " + detail);
    // A link that is gone shows neither indicator: the state says so instead.
    reportLinkState();
    m_observer->onStateChanged(state, detail);
}

void RemoteSessionCore::teardownChannel()
{
    if (m_sharedWorkspaceResult.isPending) {
        m_sharedWorkspaceResult = { .detail = "The connection ended before the workspace was confirmed. Retry sharing to check the same handoff." };
    }
    for (const ReclaimCall& call : std::exchange(m_reclaimCalls, {})) {
        if (auto* endpoint = call.endpoint.get())
            endpoint->m_reclaimRequested = false;
    }
    m_pendingFileDownload.reset();
    m_pendingWindowClose.reset();
    m_structureCalls.clear();
    for (auto& file : m_fileCalls)
        file.reset();
    m_pasteCall.reset();
    m_isPasteTickPosted = false;
    if (m_fileReadGeneration != std::numeric_limits<std::uint64_t>::max())
        ++m_fileReadGeneration;
    m_workspaceStructureRevision = 0;
    m_workspaceStructureSnapshot.reset();
    ++m_generation; // invalidate any in-flight callback from the old channel
    // The ask went out on the carrier that is going, and its answer cannot
    // arrive on the next one.
    m_isProvingLife = false;
    if (m_channel) {
        m_channel->stop(); // joins the I/O thread; no further observer callbacks
        m_channel.reset();
    }
    m_ackSeen = false;
    m_clipboardServingCallID.reset();
    // The attachment keeps each pane's picture and what is known of it for
    // the connection that continues it.
    m_core->noteTransportLost();
}

void RemoteSessionCore::reportLinkState()
{
    const Protocol::Sync::LinkState link = m_core->screenLinkState();
    if (link.isSilent == m_isLinkSilentReported && link.isInputUndelivered == m_isLinkInputUndeliveredReported)
        return;
    m_isLinkSilentReported = link.isSilent;
    m_isLinkInputUndeliveredReported = link.isInputUndelivered;
    m_observer->onLinkState(link);
}

void RemoteSessionCore::sendHello()
{
    Protocol::Sync::HandshakeRequest hello;
    hello.columns = static_cast<uint16_t>(m_columns > 0 ? m_columns : 80);
    hello.rows = static_cast<uint16_t>(m_rows > 0 ? m_rows : 24);
    hello.sessionID = m_sessionID; // 0 = fresh; nonzero = reattach
    hello.expectedDaemonInstanceID = m_daemonInstanceID;
    // What went out, kept for the denial path: HELLO_ACK overwrites m_sessionID
    // with the host's answer, and a refusal has to name the session that was
    // asked for rather than the one that was never granted.
    m_helloAttachSessionID = m_sessionID;
    // A workspace carrier binds every pane of its workspace on the one
    // connection; the identity it reconnects to rides beside the intent.
    if (m_isMultiplexedWorkspace) {
        hello.intents.add(Protocol::Sync::HelloIntent::AttachesWorkspace);
        hello.workspaceID = m_shouldFollowAdoptedSessionWorkspace ? m_requestedWorkspaceID : (m_workspaceID ? m_workspaceID : m_requestedWorkspaceID);
    }
    hello.workspaceName = m_workspaceName;
    if (m_endsWhenDetached)
        hello.sessionFlags |= Protocol::Sync::helloSessionEndWhenDetached;
    if (m_reattachPolicy == ReattachPolicy::AttachOnly && hello.sessionID != 0)
        hello.sessionFlags |= Protocol::Sync::helloSessionAttachOnly;
    // Serving the clipboard is what the user turns on, because it gives
    // something away: a program on the host that shells out for a clipboard is
    // answered from the pasteboard of the machine the user is sitting at, so
    // every process on the host can read what was copied here. Latched into the
    // frame rather than read at request time — the host records what a HELLO
    // stated, and a client that answered a request it never offered to serve
    // would be replying to a question the host believes it never asked.
    m_didClaimClipboardServe = m_clipboardServing == Clipboard::Serving::Enabled;
    if (m_didClaimClipboardServe)
        hello.intents.add(Protocol::Sync::HelloIntent::ServesClipboard);
    hello.authToken = m_authToken;
    // The size every QUIC path carries (RFC 9000 §14); a probe is far below it,
    // and a carrier that refuses a datagram leaves the probe the stream.
    constexpr std::size_t quicScreenDatagramMaxBytes = 1200;
    m_core->setScreenDatagramMaxBytes(m_transportIsQUIC ? quicScreenDatagramMaxBytes : 0);
    // Inside a stream the login authenticated the secret itself resumes. A
    // QUIC connection resumes only through its preamble, and its Greet
    // carries none: one a bootstrap token admitted attaches fresh.
    m_core->setScreenResumeCarrier(m_transportIsQUIC ? Protocol::Sync::ResumeCarrier::Proof : Protocol::Sync::ResumeCarrier::Secret);
    std::vector<uint8_t> frame;
    m_core->buildHello(hello, frame);
    m_tracedResyncs = 0;
    writeFrame(frame);
}

bool RemoteSessionCore::writeFrame(const std::vector<uint8_t>& built)
{
    auto protectedLifetime = m_dispatchLifetime.lock();
    const std::vector<uint8_t>& frame = built;
    if (m_workspaceConnection) {
        // Pane operations use the physical owner's canonical Calls/Facts.
        // No raw outer frame is a pane-scoped escape from that authority.
        transition(State::Failed, "The workspace couldn’t accept this pane’s output.");
        return false;
    }
    if (frame.empty() || !m_channel)
        return false; // record only what actually reaches the wire
    traceOutbound(frame);
    m_channel->send(frame);
    return true;
}

void RemoteSessionCore::traceOutbound(std::span<const uint8_t> frames)
{
    // One write may carry several frames, an owed Ack ahead of input; each is
    // recorded as its own type.
    while (!frames.empty()) {
        Protocol::Sync::FrameHeader header;
        Protocol::Sync::EnvelopeError error = Protocol::Sync::EnvelopeError::None;
        std::size_t headerBytes = 0;
        bool isComplete = false;
        while (headerBytes < std::min(frames.size(), Protocol::Sync::maxEnvelopeHeaderBytes)) {
            ++headerBytes;
            const auto state = Protocol::Sync::readFrameHeader(frames.first(headerBytes), header, error);
            if (state == Protocol::Sync::FrameHeaderState::Failed)
                return;
            if (state == Protocol::Sync::FrameHeaderState::Complete) {
                isComplete = true;
                break;
            }
        }
        if (!isComplete || header.bodyBytes > frames.size() - headerBytes)
            return;
        const auto frameBytes = headerBytes + header.bodyBytes;
        m_trace.recordOutbound(std::to_underlying(header.kind), frameBytes);
        frames = frames.subspan(frameBytes);
    }
}

void RemoteSessionCore::flushScreen()
{
    const auto weakThis = weakPtr();
    for (const Protocol::Sync::Notify& notify : m_core->takeHostNotifies()) {
        using Protocol::Sync::NotifyKind;
        switch (notify.kind) {
        case NotifyKind::PasteReady:
            receivePasteReady(notify.record);
            break;
        case NotifyKind::Attachments:
            if (const auto attachments = Protocol::Sync::decodeAttachments(notify.record); attachments && !receiveAttachments(*attachments))
                return;
            break;
        case NotifyKind::ConsentResolved:
            if (const auto resolved = Protocol::Sync::decodeConsentResolved(notify.record))
                m_observer->onConsentResolved(*resolved);
            break;
        case NotifyKind::StructureConsent:
            if (const auto question = Protocol::Sync::decodeStructureConsent(notify.record))
                m_observer->onStructureConsent(*question);
            break;
        case NotifyKind::ConsentQuestion:
            if (const auto question = Protocol::Sync::decodeConsentQuestion(notify.record))
                m_observer->onConsentRequest(*question);
            break;
        case NotifyKind::Events:
            if (const auto events = Protocol::Control::decodeEvents(notify.record)) {
                const auto generation = m_generation.load(std::memory_order_acquire);
                for (const auto& event : *events) {
                    if (event.kind != EventKind::ScreenRead || !event.sessionID
                        || (!m_isMultiplexedWorkspace && event.sessionID != m_sessionID)
                        || !m_core->boundPaneIdentity(event.sessionID))
                        continue;
                    m_observer->onScreenAccess(event);
                    if (!weakThis || m_stopped || generation != m_generation.load(std::memory_order_acquire))
                        return;
                }
            }
            break;
        case NotifyKind::ConsentStatus:
            break;
        }
        if (!weakThis || m_stopped)
            return;
    }
    for (const std::string& answer : m_core->takeCallResults()) {
        receivePasteAnswer(answer);
        if (!weakThis || m_stopped)
            return;
        receiveClipboardServingAnswer(answer);
        if (!weakThis || m_stopped)
            return;
        if (m_directoryListing) {
            const auto now = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
            (void)m_directoryListing->noteAnswer(answer, now);
        }
        receiveStructureAnswer(answer);
        if (!weakThis || m_stopped)
            return;
        receiveFileAnswer(answer);
        if (!weakThis || m_stopped)
            return;
    }
    serviceSessionList();
    if (!weakThis || m_stopped)
        return;
    // An answered reclaim lets the next one waiting go out with this write.
    serviceReclaims();
    if (const Protocol::Sync::ClientConnection* screen = m_core->screen()) {
        for (; m_tracedResyncs < screen->counters().resyncsSent; ++m_tracedResyncs)
            m_trace.recordResync();
    }
    // An acknowledgement already due — a checkpoint's is owed at once — goes
    // with this write rather than a dispatcher hop later.
    m_core->tickScreen();
    servicePasteSource();
    if (!weakThis || m_stopped)
        return;
    std::vector<uint8_t> frame;
    // A datagram the carrier will not take — it has none, or its queue is
    // full — rides the stream: an `Echo` that never leaves is a path the host
    // declares lost (Docs/sync.md §6.4).
    for (const std::vector<std::uint8_t>& datagram : m_core->takeScreenDatagrams()) {
        if (!m_channel || !m_channel->sendDatagram(datagram))
            frame.insert(frame.end(), datagram.begin(), datagram.end());
    }
    m_core->takeScreenOutput(frame);
    if (!frame.empty())
        writeFrame(frame);
    if (!weakThis)
        return;
    reportLinkState();
    if (m_stopped)
        return;
    const std::optional<std::uint64_t> due = m_core->screenTickDueMicroseconds();
    const std::optional<std::uint64_t> delay = m_core->screenTickDelayMicroseconds();
    if (!due || !delay)
        return;
    // The tick already posted serves unless this deadline is sooner: a
    // keystroke's stall bound is not kept waiting behind a quiet host's
    // silence bound.
    if (m_isScreenTickPosted && *due >= m_screenTickDue)
        return;
    m_isScreenTickPosted = true;
    m_screenTickDue = *due;
    const std::uint64_t serial = ++m_screenTickSerial;
    const std::uint64_t generation = m_generation.load(std::memory_order_acquire);
    m_dispatcher->postDelayed(static_cast<double>(*delay) / 1'000'000.0, [weakThis = weakPtr(), generation, serial] {
        auto* session = weakThis.get();
        if (!session || serial != session->m_screenTickSerial)
            return;
        session->m_isScreenTickPosted = false;
        if (generation != session->m_generation.load(std::memory_order_acquire) || session->m_stopped)
            return;
        session->m_core->tickScreen();
        // Input went unanswered through a probe: the path is dead though no
        // transport event says so, and a new dial is the way back. It begins
        // at once: the backoff spaces attempts at a host that refused one,
        // and nothing has been refused (Docs/sync.md §6.9).
        if (session->m_core->isScreenLost()) {
            if (session->m_hostExited || session->m_hasPermanentAuthDenial || !session->m_channel)
                session->handleClose(generation, "The host stopped answering.");
            else
                session->replaceChannel("The host stopped answering.");
            return;
        }
        session->flushScreen();
    });
}

bool RemoteSessionCore::receiveAttachments(const Protocol::Sync::AttachmentsList& list)
{
    const auto* attachment = m_core->attachment();
    if (!attachment || !m_core->isScreenAttached() || list.revision <= attachment->attachmentsRevision())
        return true;
    const auto generation = m_generation.load(std::memory_order_acquire);
    const auto weakThis = weakPtr();
    std::vector<Protocol::Sync::ID128> panes;
    const auto notePanes = [&](std::span<const Protocol::Sync::AttachmentEntry> entries) {
        for (const auto& entry : entries) {
            for (const auto pane : entry.panes) {
                if (!pane.low || pane.low > UINT32_MAX || std::ranges::find(panes, pane) != panes.end())
                    continue;
                const auto sessionID = static_cast<std::uint32_t>(pane.low);
                if ((!m_isMultiplexedWorkspace && sessionID != m_sessionID) || m_core->boundPaneIdentity(sessionID) != pane)
                    continue;
                panes.push_back(pane);
            }
        }
    };
    // A withdrawn row still owes its bound pane an empty projection. The
    // attachment model remains the sole retained roster; only affected exact
    // identities are copied before replacing it with the new snapshot.
    notePanes(attachment->attachments());
    notePanes(list.attachments);
    m_core->noteAttachments(list);
    for (const auto pane : panes) {
        if (m_core->boundPaneIdentity(static_cast<std::uint32_t>(pane.low)) != pane)
            continue;
        const auto snapshot = presenceForPane(list.attachments, pane, static_cast<std::uint32_t>(pane.low));
        m_observer->onPresence(snapshot);
        if (!weakThis || m_stopped || generation != m_generation.load(std::memory_order_acquire))
            return false;
    }
    return true;
}

void RemoteSessionCore::finishReclaim()
{
    if (m_hostExited)
        return;
    m_hostExited = true; // a clean end — suppress reconnect on the drop
    m_observer->onReclaimed();
}

} // namespace Ubiquitty::Session
