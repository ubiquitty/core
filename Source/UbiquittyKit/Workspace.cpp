// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0
#include "Workspace.h"
#include "Core/Session/SessionDirectoryCore.h"
#include "Core/Session/WorkspaceConnection.h"
#include <algorithm>
#include <map>
#include <vector>

namespace UbiquittyKit {
using namespace Ubiquitty;

struct Workspace::Impl final : Ubiquitty::Session::SessionDirectoryObserver {
    struct Pane final : Ubiquitty::Session::RemoteSessionObserver {
        Impl& owner;
        std::unique_ptr<Terminal> terminal = std::make_unique<Terminal>();
        Ubiquitty::Session::RemoteSessionCore endpoint;
        Protocol::Sync::PaneFacts facts;
        std::uint32_t sessionID = 0;
        bool hasEnded = false;

        Pane(Impl& owner, Ubiquitty::Session::WorkspaceConnection& connection)
            : owner(owner)
            , endpoint(terminal.get(), connection, this)
        {
        }
        void onScreenChanged() override { owner.changed(); }
        void onTitle(std::string_view) override { owner.changed(); }
        void onPaneFacts(const Protocol::Sync::PaneFacts& value) override
        {
            facts = value;
            owner.changed();
        }
        void onSessionEstablished(std::uint32_t value) override
        {
            sessionID = value;
            if (!owner.initialSessionID)
                owner.initialSessionID = value;
            owner.changed();
        }
        void onStateChanged(Ubiquitty::Session::State state, const std::string& detail) override
        {
            if (state == Ubiquitty::Session::State::Failed && !hasEnded)
                owner.error = detail;
            owner.changed();
        }
        void onExit() override
        {
            hasEnded = true;
            owner.changed();
        }
        void onWorkspaceVerbResult(const Ubiquitty::Session::WorkspaceIntentResult& result) override
        {
            if (!owner.stopped)
                owner.observer.onIntentResult(result);
            owner.changed();
        }
    };

    Observer& observer;
    Ubiquitty::Session::ChannelFactory* factory;
    Ubiquitty::Session::WorkspaceConnection connection;
    Ubiquitty::Session::WorkspaceConnection::Subscription subscription;
    std::unique_ptr<Ubiquitty::Session::SessionDirectoryCore> directoryCore;
    std::vector<std::unique_ptr<Pane>> panes;
    Ubiquitty::Session::SessionTree directory;
    std::optional<Ubiquitty::Session::TreeWorkspace> tree;
    std::map<std::uint32_t, Protocol::Sync::StatusRecords> statusRecords;
    std::string error;
    std::uint32_t initialSessionID = 0;
    int columns = 80;
    int rows = 24;
    bool hasStarted = false;
    bool stopped = false;
    bool hasEnded = false;

    Impl(std::unique_ptr<Ubiquitty::Session::Dispatcher> dispatcher, std::unique_ptr<Ubiquitty::Session::ChannelFactory> factory, Observer& observer)
        : observer(observer)
        , factory(factory.get())
        , connection(std::move(dispatcher), std::move(factory))
    {
        subscription = connection.subscribe([this] { changed(); });
    }
    ~Impl() { stop(); }
    void changed()
    {
        if (!stopped)
            observer.onChanged();
    }
    void stop()
    {
        if (stopped)
            return;
        stopped = true;
        subscription = {};
        directoryCore.reset();
        for (auto& pane : panes)
            pane->endpoint.stop();
        panes.clear();
        connection.connection().stop();
        tree.reset();
    }
    Pane* pane(std::uint32_t sessionID) const
    {
        auto found = std::ranges::find_if(panes, [sessionID](const std::unique_ptr<Pane>& value) { return value->sessionID == sessionID && !value->hasEnded; });
        return found == panes.end() ? nullptr : found->get();
    }
    void start(const Target& target, int width, int height)
    {
        if (hasStarted || stopped)
            return;
        hasStarted = true;
        columns = std::clamp(width, 1, maxColumns);
        rows = std::clamp(height, 1, maxRows);
        initialSessionID = target.sessionID;
        auto& physical = connection.connection();
        physical.setReattachPolicy(Ubiquitty::Session::ReattachPolicy::AttachOnly);
        physical.setAttachWorkspaceID(target.workspaceID);
        if (!target.name.empty())
            physical.setWorkspace(target.name, false);
        auto value = std::make_unique<Pane>(*this, connection);
        value->sessionID = target.sessionID;
        auto* endpoint = &value->endpoint;
        panes.push_back(std::move(value));
        endpoint->start(target.sessionID, columns, rows);
    }
    void watchDirectory()
    {
        if (stopped || !connection.connection().acknowledged())
            return;
        directoryCore.reset();
        directoryCore = std::make_unique<Ubiquitty::Session::SessionDirectoryCore>(&connection.dispatcher(), factory, this);
        if (!directoryCore->watch())
            observer.onDirectoryError("Workspace listing unavailable");
    }
    void synchronize()
    {
        if (!hasStarted || stopped)
            return;
        auto& physical = connection.connection();
        if (physical.acknowledged() && !directoryCore)
            watchDirectory();
        const auto* structure = physical.workspaceStructureSnapshot();
        if (!structure)
            return;
        std::vector<std::uint32_t> admitted;
        for (const auto& window : structure->windows)
            for (const auto& tab : window.tabs)
                for (const auto& pane : tab.panes)
                    if (pane.sessionID)
                        admitted.push_back(pane.sessionID);
        if (admitted.size() > 256) {
            error = "Workspace exceeds the 256-pane embedder limit";
            stop();
            return;
        }
        std::erase_if(panes, [&](const auto& value) {
            return physical.acknowledged() && value->sessionID && std::ranges::find(admitted, value->sessionID) == admitted.end();
        });
        auto hold = connection.holdQuestionsWhileJoining();
        for (auto sessionID : admitted) {
            if (pane(sessionID) || !connection.canAttachSession(sessionID))
                continue;
            auto value = std::make_unique<Pane>(*this, connection);
            value->sessionID = sessionID;
            auto* endpoint = &value->endpoint;
            panes.push_back(std::move(value));
            endpoint->setView(Protocol::Sync::ViewState::Dormant);
            endpoint->start(sessionID, columns, rows);
        }
        Ubiquitty::Session::SessionTree derived;
        derived.workspaces.push_back(*structure);
        for (auto& window : derived.workspaces.front().windows)
            for (auto& tab : window.tabs)
                for (auto& entry : tab.panes) {
                    if (const auto* value = pane(entry.sessionID)) {
                        entry.facts = value->facts;
                        auto status = statusRecords.find(entry.sessionID);
                        entry.records = status == statusRecords.end() ? Protocol::Sync::statusRecordsOf(*value->terminal) : status->second;
                    }
                }
        Ubiquitty::Session::deriveSessionTree(derived);
        tree = std::move(derived.workspaces.front());
        hasEnded = admitted.empty() && physical.acknowledged();
    }
    void onSessionTree(const Ubiquitty::Session::SessionTree& value) override
    {
        directory = value;
        statusRecords.clear();
        for (const auto& workspace : value.workspaces)
            for (const auto& window : workspace.windows)
                for (const auto& tab : window.tabs)
                    for (const auto& entry : tab.panes)
                        if (connection.containsSession(entry.sessionID))
                            statusRecords.emplace(entry.sessionID, entry.records);
        changed();
    }
    void onDirectoryFailed(const Ubiquitty::Session::DirectoryFailureReport& report) override
    {
        if (!stopped)
            observer.onDirectoryError(Ubiquitty::Session::listingFailureNotice(report, Ubiquitty::Session::NoticePlacement::Standalone));
        changed();
    }
    void onWatchInterrupted(const Ubiquitty::Session::DirectoryFailureReport&) override { changed(); }
};

Workspace::Workspace(std::unique_ptr<Ubiquitty::Session::Dispatcher> dispatcher, std::unique_ptr<Ubiquitty::Session::ChannelFactory> factory, Observer& observer)
    : m_impl(std::make_unique<Impl>(std::move(dispatcher), std::move(factory), observer))
{
}
Workspace::~Workspace() = default;
void Workspace::start(const Target& target, int columns, int rows) { m_impl->start(target, columns, rows); }
void Workspace::synchronize() { m_impl->synchronize(); }
void Workspace::stop() { m_impl->stop(); }
Ubiquitty::Session::Dispatcher& Workspace::dispatcher() { return m_impl->connection.dispatcher(); }
const Ubiquitty::Session::TreeWorkspace* Workspace::tree() const { return m_impl->tree ? &*m_impl->tree : nullptr; }
const Ubiquitty::Session::SessionTree& Workspace::directory() const { return m_impl->directory; }
bool Workspace::directoryIsLive() const { return m_impl->directoryCore && m_impl->directoryCore->isWatchLive(); }
void Workspace::watchDirectory() { m_impl->watchDirectory(); }
const Terminal* Workspace::terminal(std::uint32_t sessionID) const
{
    auto* pane = m_impl->pane(sessionID);
    return pane ? pane->terminal.get() : nullptr;
}
std::uint32_t Workspace::initialSessionID() const { return m_impl->initialSessionID; }
std::string Workspace::socketName() const { return m_impl->connection.connection().socketName(); }
Ubiquitty::Session::State Workspace::state() const { return m_impl->connection.connection().state(); }
std::string_view Workspace::error() const { return m_impl->error; }
bool Workspace::hasEnded() const
{
    return m_impl->hasEnded || (state() == Ubiquitty::Session::State::Failed && !m_impl->panes.empty() && std::ranges::all_of(m_impl->panes, [](const std::unique_ptr<Impl::Pane>& pane) { return pane->hasEnded; }));
}
bool Workspace::setView(std::uint32_t sessionID, int columns, int rows, Modes::FocusState focus)
{
    auto* pane = m_impl->pane(sessionID);
    if (!pane)
        return false;
    const bool visible = columns > 0 && rows > 0;
    pane->endpoint.setView(visible ? Protocol::Sync::ViewState::Live : Protocol::Sync::ViewState::Dormant);
    pane->endpoint.setSurfaceFocus(focus);
    if (visible)
        pane->endpoint.setSize(std::clamp(columns, 1, maxColumns), std::clamp(rows, 1, maxRows));
    return true;
}
bool Workspace::sendInput(std::uint32_t sessionID, std::string_view bytes)
{
    auto* pane = m_impl->pane(sessionID);
    if (!pane)
        return false;
    pane->endpoint.sendInput(bytes);
    return true;
}
bool Workspace::sendIntent(std::uint32_t sessionID, Ubiquitty::Session::WorkspaceIntent intent)
{
    auto* pane = m_impl->pane(sessionID);
    return pane && pane->endpoint.sendWorkspaceVerb(std::move(intent));
}

} // namespace UbiquittyKit
