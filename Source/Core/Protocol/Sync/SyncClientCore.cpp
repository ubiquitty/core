// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// SyncClientCore.cpp — see SyncClientCore.h.
//

#include "Core/Protocol/Sync/SyncClientCore.h"

#include "Core/OSC/TextSizing.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"
#include "PAL/PAL.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <random>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Ubiquitty::Protocol::Sync {

namespace {

/// Microseconds on the steady clock.
std::uint64_t steadyMicroseconds()
{
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

/// A fresh client instance: random, so two windows never share one.
Sync::ID128 randomClientInstance()
{
    std::random_device entropy;
    const auto word = [&entropy] { return (std::uint64_t { entropy() } << 32) | entropy(); };
    return { .low = word(), .high = word() };
}

/// Learns the pane each session in a tree is drawn from, as the host names
/// it, keeping any pane already known for a session.
///
/// - Parameters:
///   - node: The tree, or a subtree of it.
///   - panes: The session-to-pane map, which holds at most
///     `maxPanesPerConnection` sessions.
void learnSessionPanes(const TopologyNode& node, std::unordered_map<std::uint32_t, ID128>& panes)
{
    if (node.isLeaf()) {
        if (node.paneID.low && node.paneID.low <= UINT32_MAX && panes.size() < maxPanesPerConnection)
            panes.try_emplace(static_cast<std::uint32_t>(node.paneID.low), node.paneID);
        return;
    }
    for (const TopologyNode& child : node.children)
        learnSessionPanes(child, panes);
}

} // namespace

SyncClientCore::SyncClientCore(Ubiquitty::Terminal* term, SyncClientObserver* observer)
    : m_term(term)
    , m_observer(observer)
    , m_clientInstance(randomClientInstance())
    , m_clock(steadyMicroseconds)
{
}

SyncClientCore::~SyncClientCore() = default;

bool SyncClientCore::prepareResumeStore(std::shared_ptr<Sync::ResumeRecordStore> store, Sync::ID128 clientInstance, const std::array<std::uint8_t, 32>& certificatePin)
{
    if (!store || clientInstance == Sync::ID128 {} || m_term || m_observer || m_attachment || m_isScreenGreeted)
        return false;
    auto attachment = std::unique_ptr<Sync::ClientAttachment>(new (std::nothrow) Sync::ClientAttachment(Sync::measuredClientPolicy(), Sync::measuredLivenessPolicy(),
        { .clientInstance = clientInstance, .kind = Sync::ClientKind::Human, .facts = {}, .resume = std::nullopt }, static_cast<Sync::ClientPaneDelegate*>(this), store.get(), static_cast<Sync::ClientWelcomeSink*>(this)));
    if (!attachment)
        return false;
    (void)attachment->restoreFromStore();
    if (attachment->clientInstance() != clientInstance || (attachment->held() && attachment->certificatePin() != certificatePin))
        return false;
    attachment->setCertificatePin(certificatePin);
    m_clientInstance = clientInstance;
    m_resumeStore = std::move(store);
    m_attachment = std::move(attachment);
    return true;
}

bool SyncClientCore::restoreResumeStore(std::shared_ptr<Sync::ResumeRecordStore> store, Sync::ID128 clientInstance)
{
    if (!store || clientInstance == Sync::ID128 {} || !m_term || m_attachment || m_isScreenGreeted || m_resumeStore)
        return false;
    // Another window's record is neither drawn nor kept.
    if (const auto stored = store->read(); stored && stored->clientInstance != clientInstance && store->purge() != Sync::ResumePurgeOutcome::Purged)
        return false;
    auto attachment = std::unique_ptr<Sync::ClientAttachment>(new (std::nothrow) Sync::ClientAttachment(Sync::measuredClientPolicy(), Sync::measuredLivenessPolicy(),
        { .clientInstance = clientInstance, .kind = Sync::ClientKind::Human, .facts = {}, .resume = std::nullopt }, static_cast<Sync::ClientPaneDelegate*>(this), store.get(), static_cast<Sync::ClientWelcomeSink*>(this)));
    if (!attachment)
        return false;
    m_isRestoringIntoTerminal = true;
    const bool didRestore = attachment->restoreFromStore();
    m_isRestoringIntoTerminal = false;
    if (!didRestore || attachment->clientInstance() != clientInstance) {
        m_sessionID = 0;
        m_sessionPaneIDs.clear();
        if (didRestore)
            return false;
    }
    m_clientInstance = clientInstance;
    m_resumeStore = std::move(store);
    m_attachment = std::move(attachment);
    return true;
}

bool SyncClientCore::drawPreparedPane(Sync::ID128 paneID, Ubiquitty::Terminal& destination) const
{
    if (!m_resumeStore || !m_attachment || m_attachment->connection())
        return false;
    const auto* picture = m_attachment->paneTerminal(paneID);
    if (!picture)
        return false;
    const auto policy = Sync::measuredClientPolicy();
    if (!Sync::copyPicture(*picture, destination, { .maxDecodedScreenCellBytes = policy.maxDecodedScreenCellBytes, .maxStorageCost = policy.maxCheckpointStorageCost }))
        return false;
    m_attachment->copyHistoryTo(paneID, destination);
    return true;
}

bool SyncClientCore::attachPreparedObserver(SyncClientObserver& observer)
{
    if (!m_resumeStore || !m_attachment || m_attachment->connection() || m_observer)
        return false;
    m_observer = &observer;
    return true;
}

void SyncClientCore::setResumeCertificatePin(const std::array<std::uint8_t, 32>& certificatePin)
{
    if (m_attachment)
        m_attachment->setCertificatePin(certificatePin);
}

std::uint64_t SyncClientCore::now() const
{
    return m_clock();
}

bool SyncClientCore::acceptWelcome(const Sync::Welcome& welcome)
{
    const auto daemonInstanceID = welcome.selection.daemonInstanceID;
    const bool hasChangedHost = m_resumeHostInstanceID && welcome.hostInstanceID != *m_resumeHostInstanceID;
    const bool hasChangedDaemon = m_resumeDaemonInstanceID && daemonInstanceID != *m_resumeDaemonInstanceID;
    const bool hasUnexpectedDaemon = m_expectedDaemonInstanceID && m_expectedDaemonInstanceID != daemonInstanceID;
    // The connection has already held the answer to the request's exact
    // reference (`isRestoredReferenceAnswer`); a restored workspace is a
    // restart the client asked to come back from, so it is no refused
    // identity, and what the replicas held ends as for any restart.
    const bool isRestoredWorkspace = m_isWorkspaceCarrier && welcome.selection.restoredFromDaemonInstanceID
        && welcome.selection.restoredFromDaemonInstanceID == m_expectedDaemonInstanceID;
    m_hasRefusedWelcomeIdentity = !isRestoredWorkspace
        && ((m_resumeHostInstanceID && m_resumeDaemonInstanceID && hasChangedHost != hasChangedDaemon)
            || (hasChangedHost && welcome.predecessorHostInstanceID != m_resumeHostInstanceID)
            || (hasUnexpectedDaemon && (!hasChangedHost || m_resumeDaemonInstanceID != m_expectedDaemonInstanceID)));
    if (m_hasRefusedWelcomeIdentity)
        return false;
    m_hostBuild = { .isKnown = true, .wireVersion = welcome.wireVersion, .appVersion = welcome.appVersion };
    m_sessionID = welcome.selection.sessionID;
    if (m_sessionID) {
        if (auto initial = m_paneTypography.extract(0); !initial.empty()) {
            initial.key() = m_sessionID;
            m_paneTypography.insert(std::move(initial));
        }
    }
    m_acknowledgedWorkspaceID = welcome.selection.workspaceID;
    m_restoredFromDaemonInstanceID = isRestoredWorkspace ? welcome.selection.restoredFromDaemonInstanceID : 0;
    // A restored workspace's sessions are fresh: no pane the held attachment
    // named is one of them, and a number the stopped instance gave one pane
    // may be the number the restoring instance gives another.
    if (isRestoredWorkspace)
        m_sessionPaneIDs.clear();
    m_socketName = welcome.selection.socketName;
    m_daemonInstanceID = daemonInstanceID;
    m_isDaemonInstanceKnown = true;
    m_hostBuildFlags = HostBuildFlags::fromRaw(welcome.selection.buildFlags);
    m_hostPlatform = welcome.selection.hostPlatform;
    m_sawHelloAck = true;
    m_hasPendingHelloAck = true;
    return true;
}

void SyncClientCore::hostWelcomed(const Sync::Welcome& welcome)
{
    if (welcome.admission == Sync::AttachmentAdmission::AwaitingConsent) {
        if (m_observer)
            m_observer->onConsentPending(welcome.consent.sessionID);
        return;
    }
    m_admittedDaemonInstanceID = m_daemonInstanceID;
    if (std::exchange(m_hasPendingHelloAck, false) && m_observer) {
        // The observer may greet a replacement connection synchronously;
        // its borrowed argument must not become that connection's socket.
        const std::string socketName = m_socketName;
        m_observer->onHelloAck(m_sessionID, socketName);
    }
}

bool SyncClientCore::isSuccessorOf(std::uint64_t predecessor) const
{
    const auto* screen = liveScreen();
    return screen && screen->welcome() && m_resumeHostInstanceID
        && m_resumeDaemonInstanceID == predecessor && m_daemonInstanceID != predecessor
        && m_admittedDaemonInstanceID == m_daemonInstanceID
        && screen->welcome()->hostInstanceID != *m_resumeHostInstanceID
        && screen->welcome()->predecessorHostInstanceID == m_resumeHostInstanceID;
}

void SyncClientCore::consentRefused(const Sync::ConsentStatus& status)
{
    if (m_observer)
        m_observer->onConsentDenied(status.question.sessionID, status.phase);
}

Sync::ID128 SyncClientCore::paneIDForSession(std::uint32_t sessionID) const
{
    const auto found = m_sessionPaneIDs.find(sessionID);
    return found == m_sessionPaneIDs.end() ? Sync::ID128 { .low = sessionID, .high = m_daemonInstanceID } : found->second;
}

bool SyncClientCore::isSessionPane(Sync::ID128 paneID) const
{
    return paneID.low && paneID.low <= UINT32_MAX && paneID == paneIDForSession(static_cast<std::uint32_t>(paneID.low));
}

bool SyncClientCore::feed(std::span<const std::byte> bytes)
{
    if (m_didRefuseHost || !liveScreen())
        return false;
    auto* screen = liveScreen();
    if (ActiveObjectUse<Sync::ClientConnection>::contains(m_activeFeedUse, screen))
        return false;
    // Refusal and exit callbacks still belong to this read after receiveStream
    // returns. Keep its reader, and the attachment that owns its scope stack,
    // alive through any further synchronous reconnects those callbacks make.
    ActiveObjectUse attachmentUse(m_activeAttachmentUse, *m_attachment);
    auto connectionUse = attachmentUse.get()->useConnection();
    ActiveObjectUse use(m_activeFeedUse, *screen);
    const auto epoch = m_decodeEpoch;
    receiveScreenBytes({ reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size() });
    if (m_decodeEpoch != epoch)
        return !m_didRefuseHost;
    return !m_didRefuseHost && !isScreenClosed();
}

bool SyncClientCore::feedDatagram(std::span<const std::byte> bytes)
{
    if (!liveScreen() || m_didRefuseHost)
        return !isScreenClosed();
    ActiveObjectUse use(m_activeAttachmentUse, *m_attachment);
    use.get()->receiveDatagram({ reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size() }, now());
    return !isScreenClosed();
}

void SyncClientCore::receiveScreenBytes(std::span<const std::uint8_t> bytes)
{
    // Bytes of a screen plane this client never greeted are a host speaking
    // out of turn; they are dropped rather than greeted late.
    if (!liveScreen())
        return;
    ActiveObjectUse use(m_activeAttachmentUse, *m_attachment);
    const std::uint64_t epoch = m_decodeEpoch;
    const Sync::ClientConnection* screen = liveScreen();
    use.get()->receiveStream(bytes, now());
    if (m_decodeEpoch != epoch || m_attachment.get() != use.get())
        return;
    if (const auto* closed = liveScreen(); closed && closed->bye() && closed->bye()->peer) {
        m_hostBuild = closed->bye()->peer->build;
        m_hostPlatform = closed->bye()->peer->platform;
    }
    // A host this client refused for its build said what it is in the
    // Welcome it sent, not in a Bye.
    if (const auto* closed = liveScreen(); closed && closed->refusedHostBuild())
        m_hostBuild = *closed->refusedHostBuild();
    if (m_hasRefusedWelcomeIdentity) {
        // An authenticated carrier that cannot continue the held host ends
        // those panes. It is not a transient transfer failure to retry.
        m_attachment->endHost();
        m_didRefuseHost = true;
        if (m_observer)
            m_observer->onRefused(Sync::ByeReason::SessionGone);
        return;
    }
    // A connection closed on a refusal says why the host will not serve this
    // client. The observer is told once, as it is of an AUTH_DENIED, and
    // nothing more of the read is dispatched.
    if (const Sync::ClientConnection* closed = liveScreen(); closed == screen && closed->state() == Sync::ConnectionState::Closed
        && closed->byeReason() && !m_didRefuseHost
        && (Sync::isRefusal(*closed->byeReason())
            || (!closed->welcome() && (*closed->byeReason() == Sync::ByeReason::Oversize || *closed->byeReason() == Sync::ByeReason::TransferRefused)))) {
        // A held greeting can exhaust its byte or allocation budget before
        // Welcome. Report that admission refusal too, without reclassifying
        // the same transport failures on an established connection.
        m_didRefuseHost = true;
        if (m_observer)
            m_observer->onRefused(*closed->byeReason());
        return;
    }
    if (const auto* closed = liveScreen(); closed == screen && closed->state() == Sync::ConnectionState::Closed
        && closed->byeReason() == Sync::ByeReason::Normal && !std::exchange(m_hasReportedExit, true)) {
        if (m_observer)
            m_observer->onExit();
        return;
    }
}

void SyncClientCore::connectionFrameApplied(const Sync::ClientConnection& connection)
{
    if (m_didRefuseHost || m_hasRefusedWelcomeIdentity || liveScreen() != &connection || isScreenClosed())
        return;
    const auto epoch = m_decodeEpoch;
    const auto* attachment = m_attachment.get();
    const auto* screen = &connection;
    // Deliver records after their decoder has returned, but before another
    // coalesced frame can replace them. A reconnect in the observer closes
    // this reader, so the ingress discards its remaining suffix.
    const auto ended = std::exchange(m_pendingPaneEnds, {});
    const auto failures = std::exchange(m_pendingPaneFailures, {});
    const auto facts = std::exchange(m_pendingPaneFacts, {});
    for (const auto& [sessionID, record] : facts) {
        if (m_decodeEpoch != epoch || m_didRefuseHost || m_hasRefusedWelcomeIdentity || m_attachment.get() != attachment || liveScreen() != screen || isScreenClosed())
            return;
        if (!isPaneBound(sessionID))
            continue;
        const Sync::ID128 paneID = paneIDForSession(sessionID);
        paneChanged(screen->handleForPane(paneID), paneID);
        if (m_decodeEpoch != epoch || m_didRefuseHost || m_hasRefusedWelcomeIdentity || m_attachment.get() != attachment || liveScreen() != screen || isScreenClosed())
            return;
        if (!record)
            continue;
        if (m_isWorkspaceCarrier) {
            if (m_observer)
                m_observer->onPaneFacts(sessionID, *record);
        } else if (sessionID == m_sessionID) {
            receivePaneFacts(*record);
        }
    }
    for (const auto& [sessionID, reason] : failures) {
        if (m_decodeEpoch != epoch || m_didRefuseHost || m_hasRefusedWelcomeIdentity || m_attachment.get() != attachment || liveScreen() != screen || isScreenClosed())
            return;
        if (m_observer && (m_isWorkspaceCarrier || sessionID == m_sessionID))
            m_observer->onPaneFailed(sessionID, reason);
    }
    for (const auto sessionID : ended) {
        if (m_decodeEpoch != epoch || m_didRefuseHost || m_hasRefusedWelcomeIdentity || m_attachment.get() != attachment || liveScreen() != screen || isScreenClosed())
            return;
        if (!m_observer)
            continue;
        // A failed pane was told as a failure, not an end or an exit.
        if (std::ranges::any_of(failures, [&](const auto& failure) { return failure.first == sessionID; }))
            continue;
        if (m_isWorkspaceCarrier)
            m_observer->onPaneEnded(sessionID);
        else if (sessionID == m_sessionID && !std::exchange(m_hasReportedExit, true))
            m_observer->onExit();
    }
}

bool SyncClientCore::isScreenClosed() const
{
    if (isScreenLost())
        return true;
    const Sync::ClientConnection* screen = liveScreen();
    return screen && screen->state() == Sync::ConnectionState::Closed;
}

void SyncClientCore::setScreenFocus(Ubiquitty::Modes::FocusState focus)
{
    m_screenFocus = focus;
    if (Sync::ClientConnection* screen = liveScreen())
        screen->setFocus(focus);
}

bool SyncClientCore::isScreenAttached() const
{
    const Sync::ClientConnection* screen = liveScreen();
    return screen && screen->state() == Sync::ConnectionState::Attached;
}

bool SyncClientCore::call(std::string_view request)
{
    Sync::ClientConnection* screen = liveScreen();
    return screen && screen->call(request);
}

bool SyncClientCore::sendControlUploadChunk(const Sync::BulkChunk& chunk)
{
    Sync::ClientConnection* screen = liveScreen();
    return screen && screen->sendControlUploadChunk(chunk);
}

std::optional<std::uint64_t> SyncClientCore::setClipboardServing(Clipboard::Serving serving)
{
    if (!isScreenAttached() || m_nextCallID >= 9'007'199'254'740'991ULL)
        return std::nullopt;
    const auto request = R"({"jsonrpc":"2.0","id":)" + std::to_string(m_nextCallID)
        + R"(,"method":"client.clipboardServing","params":{"enabled":)"
        + (serving == Clipboard::Serving::Enabled ? "true" : "false") + "}}";
    if (!call(request))
        return std::nullopt;
    return m_nextCallID++;
}

std::vector<Sync::Notify> SyncClientCore::takeHostNotifies()
{
    Sync::ClientConnection* screen = liveScreen();
    return screen ? screen->takeNotifies() : std::vector<Sync::Notify> {};
}

void SyncClientCore::noteAttachments(Sync::AttachmentsList list)
{
    if (m_attachment && isScreenAttached())
        m_attachment->noteAttachments(std::move(list));
}

void SyncClientCore::leaveScreen()
{
    if (Sync::ClientConnection* screen = liveScreen())
        screen->leave(Sync::ByeReason::Normal);
}

bool SyncClientCore::isScreenLost() const
{
    // The attachment drops a connection whose probe went unanswered and asks
    // for another; until the owner greets one, that verdict stands.
    if (m_isScreenGreeted && m_attachment && m_attachment->wantsRedial())
        return true;
    const Sync::ClientConnection* screen = liveScreen();
    return screen && screen->state() == Sync::ConnectionState::Lost;
}

bool SyncClientCore::wantsHandoverRedial() const
{
    return m_isScreenGreeted && m_attachment && m_attachment->wantsHandoverRedial();
}

void SyncClientCore::tickScreen()
{
    if (!liveScreen())
        return;
    ActiveObjectUse use(m_activeAttachmentUse, *m_attachment);
    use.get()->tick(now());
}

std::expected<std::uint64_t, Sync::InputRefusal> SyncClientCore::sendIntent(uint32_t sessionID, Sync::InputIntent intent)
{
    if (!m_attachment || !m_isDaemonInstanceKnown || sessionID == 0)
        return std::unexpected(Sync::InputRefusal::Unsendable);
    return m_attachment->send(paneIDForSession(sessionID), std::move(intent), now());
}

Sync::PaneInputStatus SyncClientCore::paneInputStatus(uint32_t sessionID) const
{
    return m_attachment ? m_attachment->inputStatus(paneIDForSession(sessionID)) : Sync::PaneInputStatus {};
}

std::size_t SyncClientCore::paneInputOutstanding(uint32_t sessionID) const
{
    if (!m_attachment)
        return 0;
    const Sync::ID128 paneID = paneIDForSession(sessionID);
    return m_attachment->pendingCount(paneID) + m_attachment->inDoubt(paneID).size();
}

void SyncClientCore::resendInDoubt(uint32_t sessionID)
{
    if (m_attachment)
        m_attachment->resendInDoubt(paneIDForSession(sessionID), now());
}

void SyncClientCore::discardInDoubt(uint32_t sessionID)
{
    if (m_attachment)
        m_attachment->discardInDoubt(paneIDForSession(sessionID), now());
}

void SyncClientCore::noteTransportLost()
{
    ++m_decodeEpoch;
    if (m_attachment && m_isScreenGreeted)
        m_attachment->transportLost(now());
    m_isScreenGreeted = false;
}

void SyncClientCore::suspendScreen()
{
    if (liveScreen())
        m_attachment->suspend();
}

bool SyncClientCore::checkScreenLiveness()
{
    Sync::ClientConnection* screen = liveScreen();
    if (!screen || screen->state() != Sync::ConnectionState::Attached)
        return false;
    m_attachment->checkLiveness(now());
    return true;
}

bool SyncClientCore::isScreenLivenessUnproven() const
{
    const Sync::ClientConnection* screen = liveScreen();
    return screen && screen->isLivenessUnproven();
}

Sync::LinkState SyncClientCore::screenLinkState() const
{
    return liveScreen() ? m_attachment->linkState(now()) : Sync::LinkState {};
}

std::optional<std::uint64_t> SyncClientCore::screenTickDueMicroseconds() const
{
    const Sync::ClientConnection* screen = liveScreen();
    if (!screen)
        return std::nullopt;
    std::optional<std::uint64_t> due = screen->nextTickMicroseconds();
    // The owner shows a stall when it begins, and nothing arriving is what
    // begins it.
    if (const std::optional<std::uint64_t> stall = screen->nextLinkStallMicroseconds(now()))
        due = due ? std::min(*due, *stall) : *stall;
    return due;
}

std::optional<std::uint64_t> SyncClientCore::screenTickDelayMicroseconds() const
{
    const std::optional<std::uint64_t> due = screenTickDueMicroseconds();
    if (!due)
        return std::nullopt;
    const std::uint64_t current = now();
    return *due > current ? *due - current : 0;
}

void SyncClientCore::takeScreenOutput(std::vector<uint8_t>& out)
{
    if (!liveScreen())
        return;
    const auto bytes = m_attachment->takeStreamOutput();
    out.insert(out.end(), bytes.begin(), bytes.end());
}

std::vector<std::vector<std::uint8_t>> SyncClientCore::takeScreenDatagrams()
{
    Sync::ClientConnection* screen = liveScreen();
    return screen ? screen->takeDatagrams() : std::vector<std::vector<std::uint8_t>> {};
}

bool SyncClientCore::hasScreen() const
{
    const Sync::ClientConnection* screen = liveScreen();
    if (!screen)
        return false;
    const std::uint32_t handle = screen->handleForPane(paneIDForSession(m_sessionID));
    return handle != 0 && screen->isPaneCurrent(handle);
}

Ubiquitty::Terminal* SyncClientCore::terminalForPane(Sync::ID128 paneID)
{
    // A restore into the core's own terminal gives it to the first stored
    // pane: that pane is the window's, whose session the next greeting names.
    if (m_isRestoringIntoTerminal) {
        if (m_sessionID || !paneID.low || paneID.low > UINT32_MAX)
            return nullptr;
        m_sessionID = static_cast<std::uint32_t>(paneID.low);
        m_sessionPaneIDs.emplace(m_sessionID, paneID);
        return m_term;
    }
    if (!isSessionPane(paneID))
        return nullptr;
    // A successor carries an existing pane's full identity. Only newly
    // minted sessions take the successor's issuer.
    std::erase_if(m_sessionPaneIDs, [&](const auto& entry) { return m_attachment && m_attachment->isEnded(entry.second); });
    if (!m_sessionPaneIDs.contains(static_cast<std::uint32_t>(paneID.low)) && m_sessionPaneIDs.size() >= Sync::maxPanesPerConnection)
        return nullptr;
    m_sessionPaneIDs.emplace(static_cast<std::uint32_t>(paneID.low), paneID);
    if (m_term && paneID.low == m_sessionID)
        return m_term;
    if (m_observer)
        return m_observer->screenTerminal(static_cast<std::uint32_t>(paneID.low));
    return nullptr;
}

void SyncClientCore::paneChanged(std::uint32_t, Sync::ID128 paneID)
{
    if (m_observer && isSessionPane(paneID))
        m_observer->onScreenChanged(static_cast<std::uint32_t>(paneID.low));
}

bool SyncClientCore::paneClipboardRequest(std::uint32_t, Sync::ID128 paneID, const Sync::ClipboardRead& request)
{
    // Existing native/browser read adapters serve the system clipboard only.
    // A different selector is refused, never silently read from that clipboard.
    if (!m_observer || !isSessionPane(paneID)
        || request.buffer != ClipboardBuffer::Clipboard)
        return false;
    const ClipboardRequestMessage adapted { .requestID = request.requestID,
        .kind = request.kind == Sync::EffectClipboardRequestKind::Types ? ClipboardRequestKind::Types : ClipboardRequestKind::Item,
        .mime = std::string(request.mime) };
    if (m_isWorkspaceCarrier)
        return m_observer->onPaneClipboardRequest(static_cast<std::uint32_t>(paneID.low), adapted);
    if (paneID.low != m_sessionID)
        return false;
    m_observer->onClipboardRequest(adapted);
    return true;
}

void SyncClientCore::paneNativeDragCommand(std::uint32_t, Sync::ID128 paneID, const NativeDragCommand& command)
{
    if (!m_observer || !isSessionPane(paneID))
        return;
    if (m_isWorkspaceCarrier)
        m_observer->onPaneNativeDragCommand(static_cast<std::uint32_t>(paneID.low), command);
    else if (paneID.low == m_sessionID)
        m_observer->onNativeDragCommand(command);
}

bool SyncClientCore::sendNativeDragEvent(std::uint32_t sessionID, const DragDrop::NativeEvent& event, std::span<const char> bytes)
{
    auto* connection = liveScreen();
    const auto handle = paneHandle(sessionID);
    return connection && handle && connection->sendNativeDragEvent(handle, event, bytes);
}

void SyncClientCore::paneRecordsChanged(std::uint32_t, Sync::ID128 paneID)
{
    ++m_listingRevision;
    if (!isSessionPane(paneID))
        return;
    const Sync::PaneFacts* facts = paneFacts(static_cast<uint32_t>(paneID.low));
    m_pendingPaneFacts.insert_or_assign(static_cast<uint32_t>(paneID.low), facts ? std::optional { *facts } : std::nullopt);
}

void SyncClientCore::paneEffect(std::uint32_t, const Sync::EffectView& effect, const Sync::EffectBody& body, Sync::EffectDisposition disposition)
{
    if (!isSessionPane(effect.paneID))
        return;
    if (m_isWorkspaceCarrier) {
        if (m_observer)
            m_observer->onPaneEffect(static_cast<std::uint32_t>(effect.paneID.low), effect, body, disposition);
        return;
    }
    receivePaneEffect(effect, body, disposition);
}

void SyncClientCore::receivePaneEffect(const Sync::EffectView& effect, const Sync::EffectBody& body, Sync::EffectDisposition disposition)
{
    if (!m_sawHelloAck || m_didRefuseHost || effect.paneID != paneIDForSession(m_sessionID))
        return;
    // Command completion is readable session state even for an agent, whose
    // posting disposition is Ignore. Badges are already carried in PaneFacts;
    // none of their secondary readers repeats an external effect here.
    if (const auto* command = std::get_if<Sync::CommandFinishedEffect>(&body)) {
        if (m_observer)
            m_observer->onCommandDone(effect, *command, disposition);
        return;
    }
    if (disposition != Sync::EffectDisposition::Post)
        return;
    if (std::holds_alternative<Sync::BellEffect>(body)) {
        if (m_observer)
            m_observer->onBell();
        return;
    }
    // The host admitted these for its own driver; a host on another machine
    // is not trusted to have asked this client's question, so the client's
    // own Remote-origin verdict decides each one again before it reaches this
    // machine's clipboard, banners, speakers or windows.
    if (const auto* notification = std::get_if<Sync::NotificationEffect>(&body)) {
        const CapabilityDecision decision = hostEffectDecision(Capability::DesktopNotification);
        if (decision == CapabilityDecision::Allow) {
            if (m_observer)
                m_observer->onNotificationRequest({ .title = notification->title, .body = notification->body, .identifier = notification->identifier, .occasion = notification->occasion });
        } else if (decision == CapabilityDecision::Prompt) {
            holdHostEffect(Capability::DesktopNotification,
                HeldNotification { .title = std::string(notification->title), .body = std::string(notification->body), .identifier = std::string(notification->identifier), .occasion = notification->occasion });
        }
        return;
    }
    if (const auto* sound = std::get_if<Sync::SoundEffect>(&body)) {
        const CapabilityDecision decision = hostEffectDecision(Capability::PlaySound);
        if (decision == CapabilityDecision::Allow) {
            // The typed connection retains verified exact PCM through this call.
            // A mutable Terminal sample slot is not the effect's payload identity.
            if (PAL::PlayTonesCallback play = PAL::playTones())
                play(sound->request);
        } else if (decision == CapabilityDecision::Prompt) {
            // A sample's PCM is the connection's for this call only, so the
            // held sound keeps its own copy, within the sample bound.
            HeldSound held { .request = sound->request, .samplePCM = {} };
            if (sound->request.samplePCM) {
                if (sound->request.sampleFrames > VT::sampleMaxFrames)
                    return;
                held.samplePCM.assign(sound->request.samplePCM, sound->request.samplePCM + sound->request.sampleFrames);
            }
            held.request.samplePCM = nullptr;
            holdHostEffect(Capability::PlaySound, std::move(held));
        }
        return;
    }
    if (const auto* write = std::get_if<Sync::ClipboardWriteEffect>(&body)) {
        const auto bundle = Sync::decodeEffectClipboard(write->payload.inlineBytes, Sync::maxEffectClipboardWriteBytes);
        if (!bundle)
            return;
        const CapabilityDecision decision = hostEffectDecision(Capability::ClipboardWrite);
        if (decision == CapabilityDecision::Deny)
            return;
        if (decision == CapabilityDecision::Prompt) {
            holdClipboardWrite(effect, write->buffer, *bundle);
            return;
        }
        std::array<ClipboardItem, Sync::maxEffectClipboardItems> items;
        for (std::size_t index = 0; index < bundle->count; ++index) {
            const auto& item = bundle->items[index];
            items[index] = { item.mime, { reinterpret_cast<const char*>(item.data.data()), item.data.size() } };
        }
        if (m_observer)
            m_observer->onClipboardWriteItems(write->buffer, std::span(items).first(bundle->count));
        return;
    }
    if (std::holds_alternative<Sync::AttentionEffect>(body)) {
        const CapabilityDecision decision = hostEffectDecision(Capability::RequestAttention);
        if (decision == CapabilityDecision::Allow) {
            if (m_observer)
                m_observer->onAttention();
        } else if (decision == CapabilityDecision::Prompt) {
            holdHostEffect(Capability::RequestAttention, HeldAttention {});
        }
    }
}

CapabilityDecision SyncClientCore::hostEffectDecision(Capability capability) const
{
    if (m_hostLocation != HostLocation::AnotherMachine)
        return CapabilityDecision::Allow;
    const CapabilityDecision configured = m_hostEffectDecisions ? m_hostEffectDecisions(capability) : defaultDecision(capability, CapabilityOrigin::Remote);
    if (configured != CapabilityDecision::Prompt)
        return configured;
    const std::size_t index = std::to_underlying(capability);
    return index < m_standingHostEffects.size() && m_standingHostEffects[index] ? CapabilityDecision::Allow : CapabilityDecision::Prompt;
}

void SyncClientCore::holdClipboardWrite(const Sync::EffectView& effect, ClipboardBuffer buffer, const Sync::EffectClipboardBundle& bundle)
{
    const std::size_t index = std::to_underlying(buffer);
    if (index >= std::to_underlying(ClipboardBuffer::Count))
        return;
    const auto items = std::span(bundle.items).first(bundle.count);
    const auto isSameItem = [](const HeldClipboardItem& held, const Sync::EffectClipboardItem& item) {
        return held.mime == item.mime && held.bytes.size() == item.data.size()
            && (item.data.empty() || std::memcmp(held.bytes.data(), item.data.data(), item.data.size()) == 0);
    };
    for (auto& held : m_heldHostEffects) {
        auto* write = held ? std::get_if<HeldClipboardWrite>(&held->payload) : nullptr;
        if (write && write->generation == effect.generation && effect.sequence == write->lastSequence + 1
            && std::ranges::equal(write->items, items, isSameItem)) {
            write->buffers.set(index);
            write->lastSequence = effect.sequence;
            return;
        }
    }
    HeldClipboardWrite held { .buffers = {}, .items = {}, .generation = effect.generation, .lastSequence = effect.sequence };
    held.buffers.set(index);
    held.items.reserve(items.size());
    for (const auto& item : items) {
        const auto* first = reinterpret_cast<const char*>(item.data.data());
        held.items.push_back({ .mime = std::string(item.mime), .bytes = std::vector<char>(first, first + item.data.size()) });
    }
    holdHostEffect(Capability::ClipboardWrite, std::move(held));
}

void SyncClientCore::holdHostEffect(Capability capability, decltype(HeldHostEffect::payload) payload)
{
    const std::uint64_t token = m_nextHeldEffectToken++;
    auto same = std::ranges::find_if(m_heldHostEffects, [&](const auto& held) { return held && held->capability == capability; });
    auto slot = same != m_heldHostEffects.end() ? same : std::ranges::find_if(m_heldHostEffects, [](const auto& held) { return !held.has_value(); });
    if (slot == m_heldHostEffects.end())
        return; // Unreachable: one slot per kind the gate holds.
    const std::uint64_t replaced = *slot ? (*slot)->token : 0;
    const std::uint64_t order = *slot ? (*slot)->order : token;
    *slot = HeldHostEffect { .token = token, .order = order, .capability = capability, .payload = std::move(payload) };
    // Held under its token before the observer hears of it, so an answer given
    // inside the call finds it, and announced before the one it replaces is
    // withdrawn, so a question shown for that one can take this one's token.
    if (m_observer)
        m_observer->onHostEffectHeld(token, capability);
    if (replaced && m_observer)
        m_observer->onHostEffectWithdrawn(replaced, HostEffectWithdrawal::Replaced);
}

void SyncClientCore::applyHeldHostEffect(const HeldHostEffect& held)
{
    if (const auto* write = std::get_if<HeldClipboardWrite>(&held.payload)) {
        std::array<ClipboardItem, Sync::maxEffectClipboardItems> items;
        const std::size_t count = std::min(write->items.size(), items.size());
        for (std::size_t index = 0; index < count; ++index)
            items[index] = { write->items[index].mime, { write->items[index].bytes.data(), write->items[index].bytes.size() } };
        for (std::size_t buffer = 0; buffer < write->buffers.size() && m_observer; ++buffer) {
            if (write->buffers.test(buffer))
                m_observer->onClipboardWriteItems(static_cast<ClipboardBuffer>(buffer), std::span(items).first(count));
        }
    } else if (const auto* notification = std::get_if<HeldNotification>(&held.payload)) {
        if (m_observer)
            m_observer->onNotificationRequest({ .title = notification->title, .body = notification->body, .identifier = notification->identifier, .occasion = notification->occasion });
    } else if (const auto* sound = std::get_if<HeldSound>(&held.payload)) {
        PAL::PlayTonesRequest request = sound->request;
        if (!sound->samplePCM.empty())
            request.samplePCM = sound->samplePCM.data();
        if (PAL::PlayTonesCallback play = PAL::playTones())
            play(request);
    } else if (std::holds_alternative<HeldAttention>(held.payload) && m_observer) {
        m_observer->onAttention();
    }
}

void SyncClientCore::answerHeldHostEffect(std::uint64_t token, ConsentChoice choice)
{
    if (token == 0)
        return;
    const auto found = std::ranges::find_if(m_heldHostEffects, [&](const auto& held) { return held && held->token == token; });
    if (found == m_heldHostEffects.end())
        return;
    const HeldHostEffect held = std::move(**found);
    found->reset();
    if (choice != ConsentChoice::AllowOnce && choice != ConsentChoice::AllowSession && choice != ConsentChoice::AllowAlways)
        return;
    const std::size_t index = std::to_underlying(held.capability);
    if (choice != ConsentChoice::AllowOnce && !capabilityGrantIsPerUse(held.capability, CapabilityOrigin::Remote) && index < m_standingHostEffects.size())
        m_standingHostEffects[index] = true;
    applyHeldHostEffect(held);
}

std::optional<SyncClientCore::HeldHostEffectQuestion> SyncClientCore::oldestHeldHostEffect() const
{
    const HeldHostEffect* oldest = nullptr;
    for (const auto& held : m_heldHostEffects) {
        if (held && (!oldest || held->order < oldest->order))
            oldest = &*held;
    }
    if (!oldest)
        return std::nullopt;
    return HeldHostEffectQuestion { .token = oldest->token, .capability = oldest->capability };
}

void SyncClientCore::endHostEffectConnection()
{
    m_standingHostEffects = {};
    std::array<std::uint64_t, heldHostEffectKinds> withdrawn {};
    std::size_t count = 0;
    for (auto& held : m_heldHostEffects) {
        if (held)
            withdrawn[count++] = held->token;
        held.reset();
    }
    if (m_observer) {
        for (const std::uint64_t token : std::span(withdrawn).first(count))
            m_observer->onHostEffectWithdrawn(token, HostEffectWithdrawal::ConnectionEnded);
    }
}

const Sync::PaneFacts* SyncClientCore::paneFacts(uint32_t sessionID) const
{
    const Sync::ClientConnection* screen = liveScreen();
    return screen ? screen->paneFacts(screen->handleForPane(paneIDForSession(sessionID))) : nullptr;
}

bool SyncClientCore::isGeometryOwnedElsewhere(uint32_t sessionID) const
{
    const auto* screen = liveScreen();
    if (!m_sawHelloAck || !screen || screen->state() != Sync::ConnectionState::Attached || !screen->welcome())
        return false;
    const auto* facts = paneFacts(sessionID);
    return facts && facts->geometry.owner && *facts->geometry.owner != screen->welcome()->attachmentID;
}

void SyncClientCore::receivePaneFacts(const Sync::PaneFacts& facts)
{
    m_title = facts.programTitle;
    // A workspace frontend may arrive after these records were applied to
    // the carrier's temporary terminal. Its own model takes them too.
    if (m_term)
        Sync::applyRecorded(*m_term, facts);
    if (m_observer)
        m_observer->onPaneFacts(m_sessionID, facts);
}

void SyncClientCore::setPaneFocus(uint32_t sessionID, Ubiquitty::Modes::FocusState focus)
{
    const auto known = m_paneFocus.find(sessionID);
    if (known != m_paneFocus.end() && known->second == focus)
        return;
    m_paneFocus[sessionID] = focus;
    if (Sync::ClientConnection* screen = liveScreen()) {
        if (const std::uint32_t handle = screen->handleForPane(paneIDForSession(sessionID)))
            (void)screen->sendFocus(handle, focus);
    }
}

std::optional<std::uint64_t> SyncClientCore::overridePaneGeometry(uint32_t sessionID, int rows, int columns)
{
    Sync::ClientConnection* screen = liveScreen();
    const std::uint32_t handle = screen ? screen->handleForPane(paneIDForSession(sessionID)) : 0;
    if (handle == 0)
        return std::nullopt;
    const std::uint64_t callID = m_nextCallID;
    const std::string request = R"({"jsonrpc":"2.0","id":)" + std::to_string(callID) + R"(,"method":"pane.overrideGeometry","params":{"pane":)" + std::to_string(handle)
        + R"(,"rows":)" + std::to_string(rows) + R"(,"columns":)" + std::to_string(columns) + "}}";
    if (!screen->call(request))
        return std::nullopt;
    ++m_nextCallID;
    return callID;
}

std::vector<std::string> SyncClientCore::takeCallResults()
{
    Sync::ClientConnection* screen = liveScreen();
    return screen ? screen->takeResults() : std::vector<std::string> {};
}

bool SyncClientCore::consumeControlResult(std::string_view text)
{
    return m_observer && m_observer->consumeControlResult(text);
}

bool SyncClientCore::setPaneViewport(uint32_t sessionID, const Sync::Viewport& viewport)
{
    const auto known = m_paneViewports.find(sessionID);
    if (known != m_paneViewports.end() && known->second == viewport)
        return false;
    m_paneViewports[sessionID] = viewport;
    if (Sync::ClientConnection* screen = liveScreen()) {
        if (const std::uint32_t handle = screen->handleForPane(paneIDForSession(sessionID)))
            (void)screen->sendViewport(handle, viewport);
    }
    return true;
}

void SyncClientCore::setWindowViewport(const Sync::Viewport& viewport)
{
    if (m_attachment) {
        Sync::Facts facts = m_attachment->facts();
        facts.viewportRows = viewport.rows;
        facts.viewportColumns = viewport.columns;
        facts.cellPixelWidth = viewport.cellPixelWidth;
        facts.cellPixelHeight = viewport.cellPixelHeight;
        facts.textAreaPixelWidth = viewport.textAreaPixelWidth;
        facts.textAreaPixelHeight = viewport.textAreaPixelHeight;
        m_attachment->setFacts(facts);
    }
    if (Sync::ClientConnection* screen = liveScreen())
        screen->setWindowViewport(viewport);
}

void SyncClientCore::paneBound(std::uint32_t handle, Sync::ID128 paneID)
{
    // A pane bound anew, on this connection or after a reconnect, is told the
    // focus its owner last reported: the host holds none for it.
    if (isSessionPane(paneID)) {
        if (const auto typography = m_paneTypography.find(static_cast<std::uint32_t>(paneID.low)); typography != m_paneTypography.end()) {
            if (Sync::ClientConnection* screen = liveScreen())
                screen->publishTypography(handle, typography->second);
        }
        if (const auto focus = m_paneFocus.find(static_cast<std::uint32_t>(paneID.low)); focus != m_paneFocus.end()) {
            if (Sync::ClientConnection* screen = liveScreen())
                (void)screen->sendFocus(handle, focus->second);
        }
        // And the size its owner shows it at, ahead of any input: the host
        // holds none for a pane it has only now bound.
        if (const auto viewport = m_paneViewports.find(static_cast<std::uint32_t>(paneID.low)); viewport != m_paneViewports.end()) {
            if (Sync::ClientConnection* screen = liveScreen())
                (void)screen->sendViewport(handle, viewport->second);
        }
    }
    if (m_observer && isSessionPane(paneID))
        m_observer->onPaneBound(static_cast<std::uint32_t>(paneID.low));
}

void SyncClientCore::paneFailed(std::uint32_t, Sync::ID128 paneID, std::string_view reason)
{
    if (isSessionPane(paneID))
        m_pendingPaneFailures.emplace_back(static_cast<std::uint32_t>(paneID.low), std::string(reason));
}

void SyncClientCore::paneUnbound(std::uint32_t, Sync::ID128 paneID)
{
    if (isSessionPane(paneID))
        m_pendingPaneEnds.push_back(static_cast<std::uint32_t>(paneID.low));
}

void SyncClientCore::connectionFrameDecoded(Sync::FrameKind kind, std::size_t wireBytes)
{
    if (m_observer)
        m_observer->onFrameDecoded(std::to_underlying(kind), wireBytes, wireBytes);
}

void SyncClientCore::workspaceStructureChanged(std::uint32_t workspaceID)
{
    ++m_listingRevision;
    if (m_observer)
        m_observer->onWorkspaceStructure(workspaceID);
}

std::span<const std::uint8_t> SyncClientCore::workspaceStructure(std::uint32_t workspaceID) const
{
    const Sync::ClientConnection* screen = liveScreen();
    if (!screen)
        return {};
    const auto held = screen->workspaceStructures().find(workspaceID);
    if (held == screen->workspaceStructures().end())
        return {};
    return held->second.record;
}

void SyncClientCore::setPaneView(uint32_t sessionID, Sync::ViewState state)
{
    m_paneViews[sessionID] = state;
    // The attachment keeps it too, once the daemon instance that names the
    // pane is known, so a resume names the view and a pane kept hidden costs
    // nothing when the attachment continues.
    if (m_attachment && m_daemonInstanceID != 0) {
        m_attachment->setView(paneIDForSession(sessionID), state);
        return;
    }
    if (Sync::ClientConnection* screen = liveScreen())
        screen->setView(screen->handleForPane(paneIDForSession(sessionID)), state);
}

std::optional<Sync::ViewState> SyncClientCore::viewOnBind(std::uint32_t, Sync::ID128 paneID)
{
    if (!isSessionPane(paneID))
        return std::nullopt;
    const auto set = m_paneViews.find(static_cast<uint32_t>(paneID.low));
    if (set == m_paneViews.end())
        return std::nullopt;
    return set->second;
}

std::uint64_t SyncClientCore::paneBytesReceived(uint32_t sessionID) const
{
    const Sync::ClientConnection* screen = liveScreen();
    return screen ? screen->paneBytesReceived(screen->handleForPane(paneIDForSession(sessionID))) : 0;
}

bool SyncClientCore::isPaneBound(uint32_t sessionID) const
{
    return paneHandle(sessionID) != 0;
}

std::uint32_t SyncClientCore::paneHandle(std::uint32_t sessionID) const
{
    const Sync::ClientConnection* screen = liveScreen();
    return screen ? screen->handleForPane(paneIDForSession(sessionID)) : 0;
}

std::optional<Sync::ID128> SyncClientCore::boundPaneIdentity(std::uint32_t sessionID) const
{
    const auto* screen = liveScreen();
    if (!m_sawHelloAck || !screen || screen->state() != Sync::ConnectionState::Attached)
        return std::nullopt;
    const auto found = m_sessionPaneIDs.find(sessionID);
    return found != m_sessionPaneIDs.end() && screen->handleForPane(found->second) ? std::optional { found->second } : std::nullopt;
}

bool SyncClientCore::wantPaneHistory(std::uint32_t sessionID, Sync::HistoryPriority priority, std::span<const Sync::LineRange> ranges)
{
    Sync::ClientConnection* screen = liveScreen();
    return screen && screen->wantHistory(paneHandle(sessionID), priority, ranges);
}

void SyncClientCore::setSessionTerminal(uint32_t sessionID, Ubiquitty::Terminal& terminal)
{
    if (Sync::ClientConnection* screen = liveScreen())
        screen->setPaneTerminal(screen->handleForPane(paneIDForSession(sessionID)), &terminal);
}

void SyncClientCore::releaseSessionTerminal(uint32_t sessionID)
{
    const auto publication = m_paneTypography.extract(sessionID);
    if (Sync::ClientConnection* screen = liveScreen()) {
        if (!publication.empty()) {
            auto withdrawn = publication.mapped();
            withdrawn.rendersBlocks = 0;
            withdrawn.shaping = 0;
            withdrawn.fontCatalogScope = RichText::FontCatalogScope::Unavailable;
            screen->publishTypography(paneHandle(sessionID), withdrawn);
        }
        screen->setPaneTerminal(screen->handleForPane(paneIDForSession(sessionID)), nullptr);
    }
}

void SyncClientCore::withdrawTerminal(const Ubiquitty::Terminal& terminal)
{
    if (Sync::ClientConnection* screen = liveScreen())
        screen->releaseTerminal(terminal);
    if (m_attachment)
        m_attachment->forgetLentTerminal(terminal);
}

bool SyncClientCore::keepFramesHostReads(std::span<const std::uint8_t> frames, std::vector<std::uint8_t>& kept) const
{
    auto remaining = frames;
    while (!remaining.empty()) {
        Sync::FrameHeader header;
        Sync::EnvelopeError error = Sync::EnvelopeError::None;
        std::size_t headerBytes = 0;
        for (; headerBytes < std::min(remaining.size(), Sync::maxEnvelopeHeaderBytes);) {
            ++headerBytes;
            const auto state = Sync::readFrameHeader(remaining.first(headerBytes), header, error);
            if (state == Sync::FrameHeaderState::Failed)
                return false;
            if (state == Sync::FrameHeaderState::Complete)
                break;
        }
        if (!headerBytes || Sync::readFrameHeader(remaining.first(headerBytes), header, error) != Sync::FrameHeaderState::Complete
            || Sync::directionOf(header.kind) == Sync::Direction::HostToClient
            || header.bodyBytes > remaining.size() - headerBytes)
            return false;
        remaining = remaining.subspan(headerBytes + header.bodyBytes);
    }
    kept.insert(kept.end(), frames.begin(), frames.end());
    return true;
}

void SyncClientCore::invalidateBaseline()
{
    // Only the screen's baseline goes. The connection is healthy and the
    // handshake still stands, so this is not resetForReconnect.
    // With no connection there is nothing to ask: the next one resumes, and
    // the attachment reads the terminal before it names what it holds.
    if (Sync::ClientConnection* screen = liveScreen())
        screen->requestCheckpoint(screen->handleForPane(paneIDForSession(m_sessionID)));
}

void SyncClientCore::requestPaneCheckpoint(std::uint32_t sessionID)
{
    if (auto* screen = liveScreen())
        screen->requestCheckpoint(paneHandle(sessionID));
}

void SyncClientCore::resetForReconnect()
{
    ++m_decodeEpoch;
    noteTransportLost();
    // "For this connection" ends with the carrier: a reconnect, possibly to
    // another daemon instance, asks again, and an effect held for an answer
    // is dropped with the connection that delivered it.
    endHostEffectConnection();
    m_pendingPaneEnds.clear();
    m_pendingPaneFailures.clear();
    m_hasReportedExit = false;
    m_pendingPaneFacts.clear();
    m_sawHelloAck = false;
    m_hasPendingHelloAck = false;
    m_hasRefusedWelcomeIdentity = false;
    m_restoredFromDaemonInstanceID = 0;
    m_didRefuseHost = false;
    m_hostBuild = {};
    m_hostPlatform.clear();
    m_acknowledgedWorkspaceID = 0;
}

void SyncClientCore::establishWorkspacePane(uint32_t sessionID, const SyncClientCore& connection)
{
    resetForReconnect();
    m_sessionID = sessionID;
    m_acknowledgedWorkspaceID = connection.m_acknowledgedWorkspaceID;
    m_socketName = connection.m_socketName;
    m_daemonInstanceID = connection.m_daemonInstanceID;
    m_restoredFromDaemonInstanceID = connection.m_restoredFromDaemonInstanceID;
    m_sessionPaneIDs.clear();
    m_sessionPaneIDs.emplace(sessionID, connection.paneIDForSession(sessionID));
    m_hostBuildFlags = connection.m_hostBuildFlags;
    m_hostBuild = connection.m_hostBuild;
    m_hostPlatform = connection.m_hostPlatform;
    m_sawHelloAck = connection.m_sawHelloAck;
    m_didRefuseHost = connection.m_didRefuseHost;
}

void SyncClientCore::buildHello(const HandshakeRequest& request, std::vector<uint8_t>& out)
{
    resetForReconnect();
    m_expectedDaemonInstanceID = request.expectedDaemonInstanceID;
    m_resumeHostInstanceID.reset();
    m_resumeDaemonInstanceID.reset();
    m_isWorkspaceCarrier = request.intents.contains(HelloIntent::AttachesWorkspace);
    const Sync::ClientKind kind = request.clientKind();
    Sync::Facts facts = m_attachment ? m_attachment->facts() : Sync::Facts {};
    facts.viewportRows = request.rows;
    facts.viewportColumns = request.columns;
    facts.isFocused = m_screenFocus == Ubiquitty::Modes::FocusState::Focused;
    facts.link = m_linkCost;
    // One attachment for the core's life: every greeting after the first
    // resumes it (Docs/sync.md §6.9). A core that greets as another kind of
    // client is another client.
    if (!m_attachment || m_attachment->kind() != kind) {
        // A prepared protected owner is never silently replaced by an
        // unprotected attachment of another role.
        if (m_resumeStore) {
            out.clear();
            m_didRefuseHost = true;
            return;
        }
        Sync::ClientIdentity identity;
        identity.clientInstance = m_clientInstance;
        identity.kind = kind;
        ActiveObjectUse<Sync::ClientAttachment>::preserve(m_activeAttachmentUse, m_attachment);
        auto policy = Sync::measuredClientPolicy();
        if (m_observer && m_observer->supportsNativeDrag())
            policy.capabilities.add(ClientCapability::NativeDrag);
        m_attachment = std::make_unique<Sync::ClientAttachment>(policy, Sync::measuredLivenessPolicy(), std::move(identity), static_cast<Sync::ClientPaneDelegate*>(this), nullptr, static_cast<Sync::ClientWelcomeSink*>(this));
    }
    // The host forwards what it would otherwise ask about to a client that
    // says it asks, so the bit says exactly whether this client's own gate is
    // on for this connection: decided at every Greet, from where this carrier
    // reaches. A host never asks about what the client also gates, and never
    // leaves ungated what it does not ask about.
    if (m_hostLocation == HostLocation::AnotherMachine)
        m_attachment->addCapability(ClientCapability::AsksAboutHostEffects);
    else
        m_attachment->removeCapability(ClientCapability::AsksAboutHostEffects);
    // A workspace carrier has no terminal of its own: its first bound pane
    // supplies the renderer fact before the physical connection greets.
    const Ubiquitty::Terminal* drawingTerminal = m_term;
    if (!drawingTerminal && m_observer)
        drawingTerminal = m_observer->screenTerminal(request.sessionID);
    if (drawingTerminal && OSC::TextSizing::mayOfferFaceFacts(*drawingTerminal))
        m_attachment->addCapability(ClientCapability::PublishesFaces);
    else
        m_attachment->removeCapability(ClientCapability::PublishesFaces);
    const auto held = m_attachment->held();
    m_resumeHostInstanceID = held ? std::optional { held->hostInstanceID } : std::nullopt;
    if (held) {
        m_resumeDaemonInstanceID = m_admittedDaemonInstanceID;
        if (!m_resumeDaemonInstanceID && request.expectedDaemonInstanceID)
            m_resumeDaemonInstanceID = request.expectedDaemonInstanceID;
    }
    m_sessionPaneIDs.clear();
    if (held) {
        for (const auto& pane : held->panes) {
            if (pane.paneID.low && pane.paneID.low <= UINT32_MAX)
                m_sessionPaneIDs.emplace(static_cast<std::uint32_t>(pane.paneID.low), pane.paneID);
        }
    }
    m_attachment->setFacts(facts);
    Sync::AdmissionRequest admission;
    admission.target = request.isControlOnly()
        ? (request.intents.contains(HelloIntent::RequestsManagement) ? Sync::AdmissionTarget::Management : Sync::AdmissionTarget::Control)
        : (m_isWorkspaceCarrier ? Sync::AdmissionTarget::Workspace : Sync::AdmissionTarget::Session);
    admission.sessionID = request.sessionID;
    admission.workspaceID = request.workspaceID;
    admission.expectedDaemonInstanceID = request.expectedDaemonInstanceID;
    admission.workspaceName = request.workspaceName;
    admission.agentLabel = request.agentLabel;
    admission.sshHost = request.sshHost;
    admission.isAttachOnly = request.sessionID || request.workspaceID || (request.sessionFlags & helloSessionAttachOnly);
    admission.endsWhenDetached = (request.sessionFlags & helloSessionEndWhenDetached) != 0;
    admission.servesClipboard = request.intents.contains(HelloIntent::ServesClipboard);
    std::optional<std::vector<std::uint8_t>> credential;
    if (!request.authToken.empty())
        credential.emplace(request.authToken.begin(), request.authToken.end());
    m_attachment->setAdmission(std::move(admission), std::move(credential));
    // A view set before the daemon instance that names its pane was known
    // is named by the resume as every later one is.
    if (m_daemonInstanceID != 0) {
        for (const auto& [sessionID, view] : m_paneViews)
            m_attachment->setView(paneIDForSession(sessionID), view);
    }
    m_attachment->setDatagramMaxBytes(m_screenDatagramMaxBytes);
    Sync::ClientConnection& screen = m_attachment->connect(now(), m_screenResumeCarrier);
    m_isScreenGreeted = true;
    if (m_proportionalFaces)
        screen.publishProportionalFaces(*m_proportionalFaces);
    takeScreenOutput(out);
}

void SyncClientCore::publishTypography(std::uint32_t sessionID, const RichText::TypographyFacts& facts)
{
    m_paneTypography.insert_or_assign(sessionID, facts);
    if (Sync::ClientConnection* screen = liveScreen()) {
        if (const auto handle = paneHandle(sessionID))
            screen->publishTypography(handle, facts);
    }
}

void SyncClientCore::publishProportionalFaces(const OSC::TextSizing::ProportionalFaceFacts& faces)
{
    if (m_proportionalFaces && *m_proportionalFaces == faces)
        return;
    auto held = std::unique_ptr<OSC::TextSizing::ProportionalFaceFacts>(new (std::nothrow) OSC::TextSizing::ProportionalFaceFacts(faces));
    if (!held)
        return;
    m_proportionalFaces = std::move(held);
    if (Sync::ClientConnection* screen = liveScreen())
        screen->publishProportionalFaces(*m_proportionalFaces);
}

bool SyncClientCore::clearPane(uint32_t sessionID, Ubiquitty::Screen::ClearCommand command)
{
    static constexpr std::array names { "buffer", "scrollback", "screen", "previous-mark" };
    const auto index = std::to_underlying(command);
    const auto handle = paneHandle(sessionID);
    if (!isScreenAttached() || !handle || index >= names.size() || m_nextCallID >= 9'007'199'254'740'991ULL)
        return false;
    const auto request = R"({"jsonrpc":"2.0","id":)" + std::to_string(m_nextCallID)
        + R"(,"method":"pane.clear","params":{"pane":)" + std::to_string(handle)
        + R"(,"command":")" + names[index] + R"("}})";
    if (!call(request))
        return false;
    ++m_nextCallID;
    return true;
}

void SyncClientCore::setThemePalette(const TermPalette& palette, const TermPalette& baseline)
{
    const Sync::ThemePalette theme { .live = palette, .baseline = baseline };
    if (m_attachment) {
        Sync::Facts facts = m_attachment->facts();
        facts.themePalette = theme;
        m_attachment->setFacts(facts);
    }
    if (Sync::ClientConnection* screen = liveScreen())
        screen->setThemePalette(theme);
}

void SyncClientCore::setLinkCost(Sync::LinkCost link)
{
    m_linkCost = link;
    if (m_attachment) {
        Sync::Facts facts = m_attachment->facts();
        facts.link = link;
        m_attachment->setFacts(facts);
    }
    if (Sync::ClientConnection* screen = liveScreen())
        screen->setLinkCost(link);
}

void SyncClientCore::noteTopology(Sync::TopologyTree tree)
{
    // A pane keeps the identity the instance that started its session gave
    // it, across a handover: a workspace a successor answers for an earlier
    // instance's reference names panes no attachment of this client's has
    // told it, and a pane bound under another identity than the one assumed
    // would draw nothing and take no input.
    learnSessionPanes(tree.root, m_sessionPaneIDs);
    if (m_attachment)
        m_attachment->noteTopology(std::move(tree));
}

std::optional<Sync::PaneRectangle> SyncClientCore::paneRectangle(std::uint32_t sessionID) const
{
    // The replica a lost connection left keeps its rectangle as it keeps its
    // picture, so the pane is named as the attachment holds it, bound or not.
    return m_attachment ? m_attachment->paneRectangle(paneIDForSession(sessionID)) : std::nullopt;
}

void SyncClientCore::buildExit(std::vector<uint8_t>& out) const
{
    std::vector<std::uint8_t> body;
    Sync::encode(body, Sync::Bye { .reason = Sync::ByeReason::Normal, .detail = {}, .peer = std::nullopt });
    (void)Sync::appendFrame(out, Sync::FrameKind::Bye, 0, body);
}

void SyncClientCore::buildClipboardData(const ClipboardDataMessage& message, std::vector<uint8_t>& out)
{
    Sync::ClientConnection* screen = liveScreen();
    if (!screen || message.items.size() > Sync::maxEffectClipboardItems)
        return;
    Sync::EffectClipboardBundle bundle;
    bundle.count = message.items.size();
    for (std::size_t index = 0; index < bundle.count; ++index)
        bundle.items[index] = { message.items[index].mime, message.items[index].data };
    const auto status = static_cast<Sync::EffectClipboardStatus>(message.status);
    if (screen->answerClipboard(message.requestID, status, bundle))
        takeScreenOutput(out);
}

} // namespace Ubiquitty::Protocol::Sync
