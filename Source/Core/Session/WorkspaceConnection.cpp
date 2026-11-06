// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "WorkspaceConnection.h"

#include "Core/Protocol/Sync/Connection.h"
#include "Core/Session/ScreenAccessHistory.h"

#include <algorithm>
#include <deque>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <utility>
#include <vector>

namespace Ubiquitty::Session {
namespace Sync = Protocol::Sync;

namespace {
constexpr std::size_t maxPendingVerbs = 256;
constexpr std::size_t maxRetiredPanes = 256;
// The most questions one connection holds for panes still joining it; one
// past it is routed on arrival.
constexpr std::size_t maxHeldQuestions = 64;

}

struct WorkspaceConnectionStorage final : RemoteSessionObserver, std::enable_shared_from_this<WorkspaceConnectionStorage> {
    struct Pane {
        WeakPtr<RemoteSessionCore> endpoint;
        bool isRetired { false };
        bool hasExited { false };
        bool hasReceivedLayout { false };
        // Whether the pane's surface has the keyboard; unset until it reports.
        std::optional<Ubiquitty::Modes::FocusState> focus;
        std::vector<Event> pendingScreenAccesses;
    };
    /// Whether the carrier's client counts as focused: some attached pane is,
    /// or one has not said, or none is attached.
    [[nodiscard]] Ubiquitty::Modes::FocusState connectionFocus() const
    {
        const bool isAnyAttached = std::ranges::any_of(m_panes, [](const auto& entry) { return entry.second.endpoint.get() != nullptr; });
        const bool isAnyFocused = std::ranges::any_of(m_panes, [](const auto& entry) {
            return entry.second.endpoint.get() && entry.second.focus.value_or(Ubiquitty::Modes::FocusState::Focused) == Ubiquitty::Modes::FocusState::Focused;
        });
        return isAnyFocused || !isAnyAttached ? Ubiquitty::Modes::FocusState::Focused : Ubiquitty::Modes::FocusState::Unfocused;
    }
    std::unique_ptr<Dispatcher> m_dispatcher;
    std::unique_ptr<ChannelFactory> m_factory;
    RemoteSessionCore m_physical;
    std::map<std::uint32_t, Pane> m_panes;
    std::deque<uint32_t> m_completedPaneIDs;
    // Panes whose screens changed during the carrier's current feed; each is
    // told once when the feed ends.
    std::set<uint32_t> m_changedPanes;
    std::map<std::uint64_t, std::function<void()>> m_observers;
    std::map<std::uint32_t, WeakPtr<RemoteSessionCore>> m_verbs;
    WeakPtr<RemoteSessionCore> m_directoryRequester;
    uint32_t m_directoryRequestID { 0 };
    std::uint64_t m_nextObserverID { 0 };
    std::uint32_t m_lastInputPane { 0 };
    std::optional<uint32_t> m_initialRequestedSessionID;
    // Questions waiting for an endpoint to join the pane they name; see
    // ``WorkspaceConnection::holdQuestionsWhileJoining``.
    unsigned m_questionHolds { 0 };
    std::vector<Sync::ConsentQuestion> m_heldConsents;
    std::vector<Sync::StructureConsent> m_heldStructureConsents;
    // The connection of the same workspace this one retires into; see
    // ``WorkspaceConnection::retireInto``.
    std::weak_ptr<WorkspaceConnection> m_successor;
    bool m_hasStarted { false };
    bool m_isFailing { false };

    WorkspaceConnectionStorage(std::unique_ptr<Dispatcher> dispatcher, std::unique_ptr<ChannelFactory> factory)
        : m_dispatcher(std::move(dispatcher))
        , m_factory(std::move(factory))
        , m_physical(nullptr, this->m_dispatcher.get(), this->m_factory.get(), this)
    {
        m_physical.enableWorkspaceMultiplexing();
    }
    ~WorkspaceConnectionStorage() { m_physical.stop(); }

    std::vector<WeakPtr<RemoteSessionCore>> endpoints() const
    {
        std::vector<WeakPtr<RemoteSessionCore>> result;
        for (const auto& [identity, pane] : m_panes) {
            result.push_back(pane.endpoint);
        }
        return result;
    }
    RemoteSessionCore* selectedEndpoint()
    {
        if (auto found = m_panes.find(m_lastInputPane); found != m_panes.end()) {
            if (auto* endpoint = found->second.endpoint.get()) {
                return endpoint;
            }
        }
        for (auto& [identity, pane] : m_panes) {
            if (auto* endpoint = pane.endpoint.get()) {
                return endpoint;
            }
        }
        return nullptr;
    }
    bool isAttached(const RemoteSessionCore& endpoint) const
    {
        auto found = m_panes.find(endpoint.sessionID());
        return found != m_panes.end() && !found->second.isRetired && !found->second.hasExited
            && found->second.endpoint.get() == &endpoint && endpoint.acknowledged();
    }
    /// Whether a question about a session waits for an endpoint still
    /// joining its pane: a hold lasts, the host places the session here, and
    /// no attached endpoint shows it.
    [[nodiscard]] bool shouldHoldQuestion(std::uint32_t sessionID) const
    {
        if (!m_questionHolds || m_heldConsents.size() + m_heldStructureConsents.size() >= maxHeldQuestions
            || m_physical.paneForSession(sessionID) == Layout::invalidPane)
            return false;
        const auto found = m_panes.find(sessionID);
        const RemoteSessionCore* endpoint = found == m_panes.end() ? nullptr : found->second.endpoint.get();
        return !endpoint || !isAttached(*endpoint);
    }
    /// Ends one hold; the last routes each held question as it would have
    /// been routed on arrival.
    void releaseQuestionHold()
    {
        if (!m_questionHolds || --m_questionHolds)
            return;
        auto retained = weak_from_this().lock();
        for (const Sync::ConsentQuestion& question : std::exchange(m_heldConsents, {}))
            onConsentRequest(question);
        for (const Sync::StructureConsent& question : std::exchange(m_heldStructureConsents, {}))
            onStructureConsent(question);
    }
    void rememberCompletedPane(uint32_t identity)
    {
        m_completedPaneIDs.push_back(identity);
        if (m_completedPaneIDs.size() > maxRetiredPanes) {
            m_completedPaneIDs.pop_front();
        }
    }
    void notify()
    {
        auto callbacks = m_observers;
        for (auto& [identity, callback] : callbacks) {
            if (m_observers.contains(identity)) {
                callback();
            }
        }
    }
    void fail(const std::string& reason)
    {
        if (m_isFailing) {
            return;
        }
        m_isFailing = true;
        m_physical.stop();
        for (auto weak : endpoints()) {
            if (auto* endpoint = weak.get()) {
                endpoint->workspaceConnectionChanged(State::Failed, reason);
            }
        }
        notify();
    }
    void onStateChanged(State state, const std::string& detail) override
    {
        auto retained = weak_from_this().lock();
        if (state == State::Connected) {
            if (m_initialRequestedSessionID) {
                auto initial = m_panes.extract(*m_initialRequestedSessionID);
                m_initialRequestedSessionID.reset();
                if (!initial.empty()) {
                    initial.key() = m_physical.sessionID();
                    if (auto* endpoint = initial.mapped().endpoint.get()) {
                        endpoint->m_sessionID = m_physical.sessionID();
                    }
                    m_panes.insert(std::move(initial));
                }
            }
        } else {
            m_verbs.clear();
            m_directoryRequester = nullptr;
            m_directoryRequestID = 0;
            m_changedPanes.clear();
            // A question asked on the connection that dropped has no answer
            // on the next one.
            m_heldConsents.clear();
            m_heldStructureConsents.clear();
            for (auto& [identity, pane] : m_panes) {
                pane.hasReceivedLayout = false;
            }
        }
        for (auto weak : endpoints()) {
            if (auto* endpoint = weak.get()) {
                if (state == State::Connected) {
                    // Layout admission precedes pane activation after a reconnect.
                    continue;
                }
                endpoint->workspaceConnectionChanged(state, detail);
            }
        }
        notify();
    }
    /// Carries each pane to the session its host restored in its place, and
    /// ends every pane the restoration did not pair.
    ///
    /// The restoring host numbers sessions afresh, and a number the stopped
    /// host gave one pane may be the one it gives another: every pane the
    /// mirror held is the stopped host's, so none is matched to the restored
    /// workspace by number, and the completed ones it remembers name nothing
    /// there either.
    ///
    /// - Parameter carried: From each paired session to its restored one.
    void carryRestoredPanes(std::span<const std::pair<std::uint32_t, std::uint32_t>> carried)
    {
        std::map<std::uint32_t, Pane> former = std::exchange(m_panes, {});
        const std::uint32_t lastInputPane = std::exchange(m_lastInputPane, 0);
        m_completedPaneIDs.clear();
        std::vector<std::pair<std::uint32_t, WeakPtr<RemoteSessionCore>>> moved;
        for (const auto& [formerSessionID, sessionID] : carried) {
            auto entry = former.extract(formerSessionID);
            if (entry.empty() || entry.mapped().isRetired || entry.mapped().hasExited || m_panes.contains(sessionID))
                continue;
            entry.key() = sessionID;
            entry.mapped().hasReceivedLayout = false;
            if (formerSessionID == lastInputPane)
                m_lastInputPane = sessionID;
            moved.emplace_back(sessionID, entry.mapped().endpoint);
            m_panes.insert(std::move(entry));
        }
        for (const auto& [sessionID, weak] : moved) {
            RemoteSessionCore* endpoint = weak.get();
            if (!endpoint)
                continue;
            endpoint->m_sessionID = sessionID;
            // A pane the carrier bound before its structure arrived holds its
            // picture in a terminal of the carrier's until it is lent this one.
            if (endpoint->m_term)
                m_physical.m_core->setSessionTerminal(sessionID, *endpoint->m_term);
        }
        for (auto& [formerSessionID, pane] : former) {
            if (pane.isRetired || pane.hasExited)
                continue;
            if (auto* endpoint = pane.endpoint.get())
                endpoint->onExit();
        }
    }
    void onWorkspaceChanged() override
    {
        auto retained = weak_from_this().lock();
        if (auto carried = std::exchange(m_physical.m_carriedSessions, std::nullopt))
            carryRestoredPanes(*carried);
        std::set<std::uint32_t> admitted;
        for (const auto& [paneID, sessionID] : m_physical.workspacePaneSessions()) {
            admitted.insert(sessionID);
            m_panes[sessionID].hasReceivedLayout = true;
        }
        // A shared window's carrier, pinned to the workspace it published,
        // greets again by a session that workspace still holds: the pane it
        // was opened through may have been reclaimed while the others go on.
        if (m_physical.m_shouldFollowAdoptedSessionWorkspace && m_physical.m_requestedWorkspaceID && !admitted.empty()
            && !admitted.contains(m_physical.m_sessionID))
            m_physical.m_sessionID = *admitted.begin();
        auto currentEndpoints = endpoints();
        for (auto iterator = m_panes.begin(); iterator != m_panes.end();) {
            if (admitted.contains(iterator->first)) {
                ++iterator;
                continue;
            }
            if (iterator->second.isRetired) {
                ++iterator;
                continue;
            }
            auto endpoint = iterator->second.endpoint;
            if (iterator->second.hasExited) {
                rememberCompletedPane(iterator->first);
                iterator = m_panes.erase(iterator);
                continue;
            }
            iterator->second.isRetired = true;
            ++iterator;
            if (auto* session = endpoint.get()) {
                session->onExit();
            }
        }
        const auto retiredCount = std::count_if(m_panes.begin(), m_panes.end(), [](const auto& entry) { return entry.second.isRetired; });
        if (static_cast<std::size_t>(retiredCount) > maxRetiredPanes) {
            fail("The host exceeded the workspace pane retirement budget.");
            return;
        }
        // A pane established here flushes the carrier, which routes the
        // questions read ahead of this structure; one for a pane not yet
        // re-established waits until every endpoint here is.
        ++m_questionHolds;
        for (auto weak : currentEndpoints) {
            if (auto* endpoint = weak.get()) {
                std::uint32_t identity = endpoint->sessionID();
                if (!identity && admitted.contains(m_physical.sessionID())) {
                    identity = m_physical.sessionID();
                }
                if (!admitted.contains(identity)) {
                    continue;
                }
                if (!endpoint->acknowledged()) {
                    endpoint->establishWorkspacePane(identity);
                }
                if (auto* live = weak.get()) {
                    live->m_observer->onWorkspaceChanged();
                }
            }
        }
        releaseQuestionHold();
        notify();
    }
    Ubiquitty::Terminal* paneTerminal(std::uint32_t sessionID) override
    {
        auto found = m_panes.find(sessionID);
        if (found == m_panes.end() || found->second.isRetired || found->second.hasExited)
            return nullptr;
        auto* endpoint = found->second.endpoint.get();
        return endpoint ? endpoint->m_term : nullptr;
    }
    void onPaneScreenChanged(std::uint32_t sessionID) override
    {
        auto found = m_panes.find(sessionID);
        if (found == m_panes.end())
            return;
        if (found->second.endpoint.get())
            m_changedPanes.insert(sessionID);
    }
    void onWorkspacePaneFacts(std::uint32_t sessionID, const Protocol::Sync::PaneFacts& facts) override
    {
        auto retained = weak_from_this().lock();
        auto found = m_panes.find(sessionID);
        if (found == m_panes.end() || found->second.isRetired || found->second.hasExited)
            return;
        if (auto* endpoint = found->second.endpoint.get(); endpoint && endpoint->acknowledged())
            endpoint->receiveWorkspacePaneFacts(facts);
    }
    void onPaneEffect(uint32_t sessionID, const Protocol::Sync::EffectView& effect, const Protocol::Sync::EffectBody& body, Protocol::Sync::EffectDisposition disposition) override
    {
        auto retained = weak_from_this().lock();
        auto found = m_panes.find(sessionID);
        if (found == m_panes.end() || found->second.isRetired || found->second.hasExited)
            return;
        if (auto* endpoint = found->second.endpoint.get(); endpoint && endpoint->acknowledged())
            endpoint->receiveWorkspaceEffect(effect, body, disposition);
    }
    void onPaneInputSettled() override
    {
        auto retained = weak_from_this().lock();
        for (const WeakPtr<RemoteSessionCore>& weakEndpoint : endpoints()) {
            // An observer may release its core, or another pane's, on hearing.
            if (RemoteSessionCore* endpoint = weakEndpoint.get())
                endpoint->reportInputStatus();
        }
    }
    void onLinkState(const Protocol::Sync::LinkState& link) override
    {
        auto retained = weak_from_this().lock();
        for (const WeakPtr<RemoteSessionCore>& weakEndpoint : endpoints()) {
            if (auto* endpoint = weakEndpoint.get(); endpoint && endpoint->acknowledged())
                endpoint->m_observer->onLinkState(link);
        }
    }
    bool onPaneClipboardRequest(uint32_t sessionID, const Sync::ClipboardRequestMessage& request) override
    {
        auto retained = weak_from_this().lock();
        auto found = m_panes.find(sessionID);
        if (found == m_panes.end() || found->second.isRetired || found->second.hasExited)
            return false;
        auto* endpoint = found->second.endpoint.get();
        return endpoint && endpoint->acknowledged() && endpoint->receiveWorkspaceClipboardRequest(request);
    }
    bool supportsNativeDrag() const override
    {
        return std::ranges::any_of(m_panes, [](const auto& entry) {
            const auto* endpoint = entry.second.endpoint.get();
            return endpoint && endpoint->m_observer && endpoint->m_observer->supportsNativeDrag();
        });
    }
    void onPaneNativeDragCommand(std::uint32_t sessionID, const Sync::NativeDragCommand& command) override
    {
        auto retained = weak_from_this().lock();
        const auto found = m_panes.find(sessionID);
        if (found == m_panes.end() || found->second.isRetired || found->second.hasExited)
            return;
        if (auto* endpoint = found->second.endpoint.get(); endpoint && endpoint->acknowledged())
            endpoint->onNativeDragCommand(command);
    }
    void onScreenChanged() override
    {
        auto retained = weak_from_this().lock();
        for (const std::uint32_t sessionID : std::exchange(m_changedPanes, {})) {
            auto found = m_panes.find(sessionID);
            if (found == m_panes.end())
                continue;
            if (auto* endpoint = found->second.endpoint.get(); endpoint && endpoint->acknowledged())
                endpoint->m_observer->onScreenChanged();
        }
    }
    void onPaneEnded(std::uint32_t sessionID) override
    {
        auto retained = weak_from_this().lock();
        auto found = m_panes.find(sessionID);
        if (found == m_panes.end() && std::ranges::find(m_completedPaneIDs, sessionID) != m_completedPaneIDs.end())
            return;
        if (!sessionID || found == m_panes.end()
            || (m_physical.paneForSession(sessionID) == Layout::invalidPane && !found->second.isRetired)) {
            fail("The host ended a pane outside this workspace.");
            return;
        }
        if (found->second.isRetired) {
            m_panes.erase(found);
            rememberCompletedPane(sessionID);
            return;
        }
        if (std::exchange(found->second.hasExited, true))
            return;
        const auto endpoint = found->second.endpoint;
        if (auto* live = endpoint.get())
            live->onExit();
    }
    void onPaneFailed(std::uint32_t sessionID, std::string_view reason) override
    {
        auto retained = weak_from_this().lock();
        auto found = m_panes.find(sessionID);
        if (found == m_panes.end() || found->second.isRetired)
            return;
        // The pane's own core shows the reason as its session's failure.
        const auto endpoint = found->second.endpoint;
        if (auto* live = endpoint.get())
            live->onPaneFailed(sessionID, reason);
    }
    void onPaneBound(std::uint32_t sessionID) override
    {
        auto retained = weak_from_this().lock();
        auto found = m_panes.find(sessionID);
        if (found == m_panes.end() || found->second.isRetired)
            return;
        // A pane bound again after it failed is shown again by its own core.
        const auto endpoint = found->second.endpoint;
        if (auto* live = endpoint.get())
            live->onPaneBound(sessionID);
    }
    void onWorkspaceVerbResult(const Ubiquitty::Session::WorkspaceIntentResult& result) override
    {
        auto found = m_verbs.find(result.requestID);
        if (found == m_verbs.end()) {
            return;
        }
        auto weak = found->second;
        m_verbs.erase(found);
        if (auto* endpoint = weak.get()) {
            endpoint->m_observer->onWorkspaceVerbResult(result);
        }
    }
    void onSessionList(uint32_t requestID, const std::vector<SessionRow>& sessions) override
    {
        if (!requestID || requestID != m_directoryRequestID) {
            return;
        }
        m_directoryRequestID = 0;
        auto weak = std::exchange(m_directoryRequester, nullptr);
        if (auto* endpoint = weak.get()) {
            endpoint->m_observer->onSessionList(requestID, sessions);
        }
    }
    void onSessionListFailed(uint32_t requestID) override
    {
        if (!requestID || requestID != m_directoryRequestID)
            return;
        m_directoryRequestID = 0;
        auto weak = std::exchange(m_directoryRequester, nullptr);
        if (auto* endpoint = weak.get())
            endpoint->m_observer->onSessionListFailed(requestID);
    }
    void onSharedWorkspaceChanged() override
    {
        for (auto weak : endpoints()) {
            if (auto* endpoint = weak.get())
                endpoint->m_observer->onSharedWorkspaceChanged();
        }
        notify();
    }
    void onClipboardRoutingChanged() override
    {
        for (auto weak : endpoints()) {
            if (auto* endpoint = weak.get()) {
                endpoint->m_observer->onClipboardRoutingChanged();
            }
        }
        notify();
    }
    void onClipboardRequest(const Sync::ClipboardRequestMessage& request) override
    {
        m_physical.serveClipboard(request.requestID, Sync::ClipboardServeStatus::Refused, {});
    }
    void onConsentRequest(const Protocol::Sync::ConsentQuestion& request) override
    {
        if (shouldHoldQuestion(request.sessionID)) {
            m_heldConsents.push_back(request);
            return;
        }
        auto found = m_panes.find(request.sessionID);
        if (found != m_panes.end()) {
            if (auto* endpoint = found->second.endpoint.get(); endpoint && isAttached(*endpoint)) {
                endpoint->m_observer->onConsentRequest(request);
                return;
            }
        }
        // A request without a live pane cannot be attributed to the focused
        // pane, so nobody here is shown it; another recipient still may be.
        m_physical.respondToConsent(request.requestID, static_cast<Protocol::Sync::ConsentKind>(request.action), static_cast<uint8_t>(Protocol::Sync::ConsentChoice::Unshown));
    }
    void onPresence(const PanePresenceSnapshot& presence) override
    {
        const auto found = m_panes.find(presence.sessionID);
        if (found != m_panes.end())
            if (auto* endpoint = found->second.endpoint.get(); endpoint && isAttached(*endpoint))
                endpoint->m_observer->onPresence(presence);
    }
    void onScreenAccess(const Event& event) override
    {
        const auto found = m_panes.find(event.sessionID);
        if (found == m_panes.end() || found->second.isRetired || found->second.hasExited)
            return;
        if (auto* endpoint = found->second.endpoint.get(); endpoint && isAttached(*endpoint)) {
            endpoint->m_observer->onScreenAccess(event);
            return;
        }
        auto& pending = found->second.pendingScreenAccesses;
        if (pending.size() == ScreenAccessHistory::maxRecords)
            pending.erase(pending.begin());
        pending.push_back(event);
    }
    void onStructureConsent(const Protocol::Sync::StructureConsent& request) override
    {
        if (shouldHoldQuestion(request.sessionID)) {
            m_heldStructureConsents.push_back(request);
            return;
        }
        auto found = m_panes.find(request.sessionID);
        if (found != m_panes.end()) {
            if (auto* endpoint = found->second.endpoint.get(); endpoint && isAttached(*endpoint)) {
                endpoint->m_observer->onStructureConsent(request);
                return;
            }
        }
        // Nobody here is shown it, which is not an answer for the person;
        // another recipient still may be.
        m_physical.respondToConsent(request.requestID, Protocol::Sync::ConsentKind::Structure, static_cast<uint8_t>(Protocol::Sync::ConsentChoice::Unshown));
    }
    void onConsentResolved(const Protocol::Sync::ConsentResolution& resolution) override
    {
        // A question retired while it waited is never shown.
        std::erase_if(m_heldConsents, [&](const Sync::ConsentQuestion& held) {
            return held.requestID == resolution.requestID && held.sessionID == resolution.sessionID
                && static_cast<Sync::ConsentKind>(held.action) == resolution.kind;
        });
        std::erase_if(m_heldStructureConsents, [&](const Sync::StructureConsent& held) {
            return held.requestID == resolution.requestID && held.sessionID == resolution.sessionID && resolution.kind == Sync::ConsentKind::Structure;
        });
        const auto found = m_panes.find(resolution.sessionID);
        if (found != m_panes.end())
            if (auto* endpoint = found->second.endpoint.get(); endpoint && isAttached(*endpoint))
                endpoint->m_observer->onConsentResolved(resolution);
    }
    void onDeployRequested(const std::string& platform) override
    {
        if (auto* endpoint = selectedEndpoint()) {
            endpoint->m_observer->onDeployRequested(platform);
        }
    }
    void onReattachRefused(uint32_t identity, Protocol::Sync::ByeReason reason) override
    {
        for (auto weak : endpoints()) {
            if (auto* endpoint = weak.get()) {
                endpoint->m_observer->onReattachRefused(identity, reason);
            }
        }
    }
    void onExit() override
    {
        for (auto weak : endpoints()) {
            if (auto* endpoint = weak.get()) {
                endpoint->onExit();
            }
        }
    }
};

WorkspaceConnection::WorkspaceConnection(std::unique_ptr<Dispatcher> dispatcher, std::unique_ptr<ChannelFactory> factory)
    : m_storage(std::make_shared<Storage>(std::move(dispatcher), std::move(factory)))
{
    m_storage->m_physical.m_dispatchLifetime = m_storage;
}
WorkspaceConnection::~WorkspaceConnection() = default;
RemoteSessionCore& WorkspaceConnection::connection() { return m_storage->m_physical; }
const RemoteSessionCore& WorkspaceConnection::connection() const { return m_storage->m_physical; }
Dispatcher& WorkspaceConnection::dispatcher() { return *m_storage->m_dispatcher; }
bool WorkspaceConnection::containsSession(uint32_t identity) const { return identity && connection().paneForSession(identity) != Layout::invalidPane; }
void WorkspaceConnection::refuseDuplicateAttachment()
{
    m_storage->fail("This workspace is already attached in another window.");
}
WorkspaceConnection::QuestionHold WorkspaceConnection::holdQuestionsWhileJoining()
{
    ++m_storage->m_questionHolds;
    return QuestionHold(m_storage);
}
bool WorkspaceConnection::canAttachSession(uint32_t identity) const
{
    if (!connection().acknowledged() || connection().state() != State::Connected || !containsSession(identity)) {
        return false;
    }
    auto found = m_storage->m_panes.find(identity);
    return found == m_storage->m_panes.end() || !found->second.endpoint.get();
}
WorkspaceConnection::Subscription WorkspaceConnection::subscribe(std::function<void()> callback)
{
    const auto identity = ++m_storage->m_nextObserverID;
    m_storage->m_observers.emplace(identity, std::move(callback));
    return Subscription(m_storage, identity);
}
bool WorkspaceConnection::attach(RemoteSessionCore& endpoint, uint32_t identity)
{
    auto retained = m_storage;
    if (retained->m_hasStarted && (!retained->m_physical.acknowledged() || retained->m_physical.state() != State::Connected || !containsSession(identity))) {
        return false;
    }
    auto& pane = retained->m_panes[identity];
    if (auto* existing = pane.endpoint.get(); existing && existing != &endpoint) {
        return false;
    }
    pane.endpoint = endpoint.weakPtr();
    pane.focus = endpoint.m_surfaceFocus;
    retained->m_physical.applyScreenFocus(retained->connectionFocus());
    if (!retained->m_hasStarted) {
        retained->m_hasStarted = true;
        retained->m_initialRequestedSessionID = identity;
        retained->m_physical.start(identity, endpoint.m_columns, endpoint.m_rows);
        return true;
    }
    auto weakEndpoint = pane.endpoint;
    const bool hasExited = pane.hasExited;
    endpoint.establishWorkspacePane(identity);
    // The pane may have been bound before this core attached, its screen then
    // replicated into a terminal the carrier kept; the core's own takes over.
    // Its observer may have released the core on hearing it was established,
    // and a core that is gone lends no terminal.
    if (const RemoteSessionCore* established = weakEndpoint.get(); established && established->m_term)
        retained->m_physical.m_core->setSessionTerminal(identity, *established->m_term);
    else
        retained->m_physical.m_core->releaseSessionTerminal(identity);
    // The resync that asks for the mirror's checkpoint goes now, not with
    // whatever the carrier writes next.
    retained->m_physical.flushScreen();
    if (const auto found = retained->m_panes.find(identity); found != retained->m_panes.end()) {
        for (const Event& event : std::exchange(found->second.pendingScreenAccesses, {})) {
            if (auto* live = weakEndpoint.get(); live && retained->isAttached(*live))
                live->m_observer->onScreenAccess(event);
        }
    }
    if (hasExited)
        if (auto* live = weakEndpoint.get())
            live->onExit();
    // A pane that joins a connection on its way out moves on with the rest.
    if (!retained->m_successor.expired())
        if (auto* live = weakEndpoint.get(); live && live->m_observer)
            live->m_observer->onConnectionRetiring();
    return true;
}
bool WorkspaceConnection::retireInto(const std::shared_ptr<WorkspaceConnection>& successor)
{
    auto retained = m_storage;
    if (!successor || successor.get() == this || retained->m_isFailing)
        return false;
    // Its panes were told once, when it began to retire.
    if (const auto current = retained->m_successor.lock())
        return current == successor;
    // A successor on its own way out would hand the panes on again, or back.
    if (successor->successor())
        return false;
    const RemoteSessionCore& from = connection();
    const RemoteSessionCore& to = successor->connection();
    if (!from.acknowledged() || !to.acknowledged() || from.state() != State::Connected || to.state() != State::Connected
        || !from.workspaceID() || from.workspaceID() != to.workspaceID() || !from.daemonInstanceID()
        || from.daemonInstanceID() != to.daemonInstanceID())
        return false;
    for (const auto& [sessionID, pane] : retained->m_panes) {
        if (pane.endpoint.get() && successor->containsSession(sessionID) && !successor->canAttachSession(sessionID))
            return false;
    }
    retained->m_successor = successor;
    for (const WeakPtr<RemoteSessionCore>& weak : retained->endpoints()) {
        // A pane told may release its own core, or another's, on hearing.
        if (auto* endpoint = weak.get(); endpoint && endpoint->m_observer)
            endpoint->m_observer->onConnectionRetiring();
    }
    return true;
}
std::shared_ptr<WorkspaceConnection> WorkspaceConnection::successor() const
{
    return m_storage->m_successor.lock();
}
bool WorkspaceConnection::holdsPicture(std::uint32_t sessionID) const
{
    const Protocol::Sync::SyncClientCore& core = *connection().m_core;
    const Protocol::Sync::ClientConnection* screen = core.screen();
    const std::uint32_t handle = sessionID ? core.paneHandle(sessionID) : 0;
    return screen && handle && screen->paneFrameClass(handle).has_value();
}
bool WorkspaceConnection::hasRequestsFor(const RemoteSessionCore& endpoint) const
{
    const auto names = [&](const WeakPtr<RemoteSessionCore>& requester) { return requester.get() == &endpoint; };
    return std::ranges::any_of(m_storage->m_verbs, [&](const auto& entry) { return names(entry.second); })
        || (m_storage->m_directoryRequestID && names(m_storage->m_directoryRequester));
}
void WorkspaceConnection::detach(RemoteSessionCore& endpoint)
{
    for (auto& [identity, pane] : m_storage->m_panes) {
        if (pane.endpoint.get() != &endpoint) {
            continue;
        }
        pane.endpoint = nullptr;
        pane.focus.reset();
        m_storage->m_physical.applyScreenFocus(m_storage->connectionFocus());
        // The terminal goes with the core; the carrier keeps the binding on
        // one of its own until another core attaches.
        m_storage->m_physical.m_core->releaseSessionTerminal(identity);
        m_storage->m_physical.flushScreen();
        break;
    }
    // Nor is anything else of the carrier's left in it: a pane the host
    // unbound or retired, or one held across a reconnect, has no binding to
    // move and would keep its picture in the freed terminal.
    if (endpoint.m_term)
        m_storage->m_physical.m_core->withdrawTerminal(*endpoint.m_term);
}
void WorkspaceConnection::noteFocus(RemoteSessionCore& endpoint, Ubiquitty::Modes::FocusState focus)
{
    for (auto& [identity, pane] : m_storage->m_panes) {
        if (pane.endpoint.get() != &endpoint)
            continue;
        pane.focus = focus;
        m_storage->m_physical.applyScreenFocus(m_storage->connectionFocus());
        return;
    }
}

void WorkspaceConnection::noteInput(RemoteSessionCore& endpoint)
{
    if (m_storage->isAttached(endpoint))
        m_storage->m_lastInputPane = endpoint.sessionID();
}

void WorkspaceConnection::noteReclaimed(RemoteSessionCore& endpoint)
{
    const auto found = m_storage->m_panes.find(endpoint.sessionID());
    if (found != m_storage->m_panes.end() && found->second.endpoint.get() == &endpoint)
        found->second.hasExited = true;
}

uint32_t WorkspaceConnection::sendWorkspaceVerb(RemoteSessionCore& endpoint, Ubiquitty::Session::WorkspaceIntent verb)
{
    if (verb.verb == Ubiquitty::Session::WorkspaceIntentKind::CloseSurface || verb.verb == Ubiquitty::Session::WorkspaceIntentKind::CloseFocusedPane)
        return 0;
    if (!m_storage->isAttached(endpoint) || !containsSession(endpoint.sessionID()) || m_storage->m_verbs.size() >= maxPendingVerbs) {
        return 0;
    }
    auto retained = m_storage;
    auto& physical = retained->m_physical;
    if (!physical.acknowledged() || physical.m_nextWorkspaceVerbRequestID == std::numeric_limits<std::uint32_t>::max()) {
        return 0;
    }
    const auto identity = ++physical.m_nextWorkspaceVerbRequestID;
    verb.requestID = identity;
    verb.workspaceID = physical.workspaceID();
    retained->m_verbs.emplace(identity, endpoint.weakPtr());
    if (!physical.sendStructureCall(verb)) {
        retained->m_verbs.erase(identity);
        return 0;
    }
    return identity;
}
uint32_t WorkspaceConnection::sendWindowClose(RemoteSessionCore& endpoint, Layout::SurfaceID surface, std::uint64_t revision)
{
    auto retained = m_storage;
    auto& physical = retained->m_physical;
    if (!retained->isAttached(endpoint) || !containsSession(endpoint.sessionID()) || retained->m_verbs.size() >= maxPendingVerbs
        || !physical.acknowledged() || physical.m_nextWorkspaceVerbRequestID == std::numeric_limits<std::uint32_t>::max())
        return 0;
    const auto identity = ++physical.m_nextWorkspaceVerbRequestID;
    retained->m_verbs.emplace(identity, endpoint.weakPtr());
    if (!physical.sendWindowCloseCall(identity, surface, revision)) {
        retained->m_verbs.erase(identity);
        return 0;
    }
    return identity;
}
uint32_t WorkspaceConnection::requestSessionList(RemoteSessionCore& endpoint)
{
    auto retained = m_storage;
    auto& physical = retained->m_physical;
    if (!retained->isAttached(endpoint) || retained->m_directoryRequestID || !physical.acknowledged()) {
        return 0;
    }
    if (physical.m_nextSessionListRequestID == std::numeric_limits<std::uint32_t>::max())
        return 0;
    const auto identity = ++physical.m_nextSessionListRequestID;
    retained->m_directoryRequestID = identity;
    retained->m_directoryRequester = endpoint.weakPtr();
    if (!physical.beginSessionList(identity)) {
        retained->m_directoryRequestID = 0;
        retained->m_directoryRequester = nullptr;
        physical.m_sessionListRequestID = 0;
        return 0;
    }
    return identity;
}
WorkspaceConnection::QuestionHold::QuestionHold(std::weak_ptr<Storage> owner)
    : m_owner(std::move(owner))
{
}
WorkspaceConnection::QuestionHold::~QuestionHold()
{
    if (auto owner = m_owner.lock())
        owner->releaseQuestionHold();
}
WorkspaceConnection::Subscription::Subscription(std::weak_ptr<Storage> owner, std::uint64_t identity)
    : m_owner(std::move(owner))
    , m_identity(identity)
{
}
WorkspaceConnection::Subscription::~Subscription()
{
    if (auto owner = m_owner.lock()) {
        owner->m_observers.erase(m_identity);
    }
}
WorkspaceConnection::Subscription::Subscription(Subscription&& other) noexcept
    : m_owner(std::move(other.m_owner))
    , m_identity(std::exchange(other.m_identity, 0))
{
}
WorkspaceConnection::Subscription& WorkspaceConnection::Subscription::operator=(Subscription&& other) noexcept
{
    if (this != &other) {
        if (auto owner = m_owner.lock()) {
            owner->m_observers.erase(m_identity);
        }
        m_owner = std::move(other.m_owner);
        m_identity = std::exchange(other.m_identity, 0);
    }
    return *this;
}

} // namespace Ubiquitty::Session
