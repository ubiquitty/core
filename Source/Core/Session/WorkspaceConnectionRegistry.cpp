// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0
#include "WorkspaceConnectionRegistry.h"

#include <algorithm>
#include <cassert>
#include <map>
#include <mutex>
#include <set>
#include <string_view>
#include <thread>
#include <vector>

namespace Ubiquitty::Session {

namespace {

/// Whether a daemon socket name is one a pinned transport command may carry.
bool isDaemonSocketNameAllowed(std::string_view socketName)
{
    return std::ranges::all_of(socketName, [](unsigned char character) {
        return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z')
            || (character >= '0' && character <= '9') || character == '.' || character == '_' || character == '-';
    });
}

} // namespace

struct WorkspaceConnectionRegistry::RegistryStorage {
    struct Key {
        WorkspaceAuthority authority;
        uint32_t workspaceID;
        auto operator<=>(const Key&) const = default;
    };
    struct Entry {
        WorkspaceAuthority authority;
        std::weak_ptr<WorkspaceConnection> connection;
        std::set<uint32_t> sessionIDs;
        std::string resolvedDaemonSocket;
        std::string resolvedWorkspaceName;
        uint32_t requestedSessionID { 0 };
        std::string requestedWorkspaceName;
        bool hasResolvedLayout { false };
        WorkspaceConnection::Subscription subscription;
        bool isActive { true };
    };
    const std::thread::id m_eventLoop { std::this_thread::get_id() };
    std::mutex m_mutex;
    std::map<Key, std::weak_ptr<WorkspaceConnection>> m_workspaces;
    std::vector<std::shared_ptr<Entry>> m_entries;

    /// Drops entries whose owner failed or is gone. The caller holds the mutex.
    void prune()
    {
        std::erase_if(m_entries, [](const auto& entry) { return !entry->isActive || entry->connection.expired(); });
        std::erase_if(m_workspaces, [](const auto& entry) { return entry.second.expired(); });
    }

    /// The keys an owner whose host named its workspace is reachable by: its
    /// authority under the acknowledged daemon instance and, once the host
    /// named its socket, the same authority resolved to that socket.
    static std::vector<Key> keysOf(const WorkspaceAuthority& requested, const RemoteSessionCore& physical)
    {
        auto authority = requested;
        authority.daemonInstanceID = physical.daemonInstanceID();
        std::vector<Key> keys { { authority, physical.workspaceID() } };
        if (!physical.socketName().empty()) {
            authority.daemonSocket = physical.socketName();
            keys.push_back({ std::move(authority), physical.workspaceID() });
        }
        return keys;
    }

    /// Records an owner's current topology and indexes the workspace it names.
    /// The caller holds the mutex.
    ///
    /// - Returns: Another live owner already indexed under that workspace, if any.
    std::shared_ptr<WorkspaceConnection> index(Entry& entry, const std::shared_ptr<WorkspaceConnection>& connection)
    {
        const RemoteSessionCore& physical = connection->connection();
        entry.hasResolvedLayout = physical.workspace() != nullptr;
        entry.resolvedWorkspaceName = physical.workspaceName();
        entry.sessionIDs.clear();
        for (const auto& [paneID, sessionID] : physical.workspacePaneSessions())
            entry.sessionIDs.insert(sessionID);
        entry.resolvedDaemonSocket = physical.socketName();
        std::shared_ptr<WorkspaceConnection> collision;
        for (const Key& key : keysOf(entry.authority, physical)) {
            auto& slot = m_workspaces[key];
            if (auto other = slot.lock(); other && other != connection)
                collision = std::move(other);
            else
                slot = connection;
        }
        return collision;
    }

    /// Follows an indexed owner's identity and topology. One that resolved to
    /// a workspace another owner already carries, as two windows of a partial
    /// restore attached by session can, retires into that owner, its panes
    /// moving there; one that shows a pane the other already shows is
    /// refused, since a second view of a pane is not a move.
    static std::function<void()> observer(const std::shared_ptr<RegistryStorage>& storage, const std::shared_ptr<Entry>& indexed)
    {
        return [weakState = std::weak_ptr<RegistryStorage>(storage), weakEntry = std::weak_ptr<Entry>(indexed)] {
            auto state = weakState.lock();
            auto entry = weakEntry.lock();
            if (!state || !entry) {
                return;
            }
            auto connection = entry->connection.lock();
            if (!connection) {
                return;
            }
            if (connection->connection().state() == Session::State::Failed) {
                std::scoped_lock lock(state->m_mutex);
                entry->isActive = false;
                std::erase_if(state->m_workspaces, [&](const auto& item) { return item.second.lock() == connection; });
                return;
            }
            if (!connection->connection().workspaceID()) {
                return;
            }
            std::shared_ptr<WorkspaceConnection> collision;
            {
                std::scoped_lock lock(state->m_mutex);
                collision = state->index(*entry, connection);
            }
            // Unknown workspace requests bind only after the host identifies them.
            if (collision && !connection->retireInto(collision)) {
                connection->refuseDuplicateAttachment();
            }
        };
    }
};

WorkspaceConnectionRegistry::WorkspaceConnectionRegistry()
    : m_state(std::make_shared<RegistryStorage>())
{
}
WorkspaceConnectionRegistry::~WorkspaceConnectionRegistry()
{
    assert(std::this_thread::get_id() == m_state->m_eventLoop);
}

std::expected<WorkspaceConnectionRegistry::Lease, WorkspaceConnectionRegistry::Error>
WorkspaceConnectionRegistry::acquire(const WorkspaceAttachRequest& request, Create create)
{
    if (std::this_thread::get_id() != m_state->m_eventLoop) {
        return std::unexpected(Error::DifferentEventLoop);
    }
    if (request.authority.endpointIdentity.empty() || !isDaemonSocketNameAllowed(request.authority.daemonSocket)) {
        return std::unexpected(Error::InvalidAuthority);
    }
    auto state = m_state;
    std::scoped_lock lock(state->m_mutex);
    state->prune();
    if (request.workspaceID) {
        auto found = state->m_workspaces.find({ request.authority, request.workspaceID });
        if (found != state->m_workspaces.end()) {
            if (auto connection = found->second.lock()) {
                for (const auto& entry : state->m_entries) {
                    if (entry->connection.lock() != connection) {
                        continue;
                    }
                    return Lease { std::move(connection), true };
                }
            }
        }
    }
    for (const auto& entry : state->m_entries) {
        auto connection = entry->connection.lock();
        if (!connection) {
            continue;
        }
        auto authority = entry->authority;
        authority.daemonInstanceID = request.authority.daemonInstanceID ? connection->connection().daemonInstanceID() : 0;
        if (!request.authority.daemonSocket.empty() && authority.daemonSocket.empty()) {
            authority.daemonSocket = entry->resolvedDaemonSocket;
        }
        if (authority != request.authority) {
            continue;
        }
        bool matches = false;
        if (request.workspaceID) {
            matches = connection->connection().workspaceID() == request.workspaceID;
        } else if (request.sessionID) {
            matches = entry->sessionIDs.contains(request.sessionID)
                || (!entry->hasResolvedLayout && entry->requestedSessionID == request.sessionID);
        } else if (!request.workspaceName.empty()) {
            matches = (entry->hasResolvedLayout ? entry->resolvedWorkspaceName : entry->requestedWorkspaceName) == request.workspaceName;
        }
        if (matches) {
            return Lease { std::move(connection), true };
        }
    }
    auto connection = create();
    if (!connection) {
        return std::unexpected(Error::CreationFailed);
    }
    auto entry = std::make_shared<RegistryStorage::Entry>();
    entry->authority = request.authority;
    entry->connection = connection;
    entry->requestedSessionID = request.sessionID;
    entry->requestedWorkspaceName = request.workspaceName;
    state->m_entries.push_back(entry);
    if (request.workspaceID) {
        state->m_workspaces[{ request.authority, request.workspaceID }] = connection;
    }
    entry->subscription = connection->subscribe(RegistryStorage::observer(state, entry));
    return Lease { std::move(connection), false };
}

std::expected<void, WorkspaceConnectionRegistry::Error>
WorkspaceConnectionRegistry::adopt(const WorkspaceAuthority& authority, const std::shared_ptr<WorkspaceConnection>& connection)
{
    if (std::this_thread::get_id() != m_state->m_eventLoop)
        return std::unexpected(Error::DifferentEventLoop);
    if (authority.endpointIdentity.empty() || !isDaemonSocketNameAllowed(authority.daemonSocket))
        return std::unexpected(Error::InvalidAuthority);
    if (!connection)
        return std::unexpected(Error::Unresolved);
    const RemoteSessionCore& physical = connection->connection();
    if (!physical.acknowledged() || physical.state() != State::Connected || !physical.workspaceID() || !physical.workspace())
        return std::unexpected(Error::Unresolved);
    auto state = m_state;
    std::scoped_lock lock(state->m_mutex);
    state->prune();
    if (std::ranges::any_of(state->m_entries, [&](const auto& entry) { return entry->connection.lock() == connection; }))
        return {};
    // Checked whole before anything is written: a refusal leaves the owner
    // already indexed exactly as it was, and this one unindexed and running.
    for (const RegistryStorage::Key& key : RegistryStorage::keysOf(authority, physical)) {
        const auto found = state->m_workspaces.find(key);
        if (found == state->m_workspaces.end())
            continue;
        if (auto other = found->second.lock(); other && other != connection)
            return std::unexpected(Error::Duplicate);
    }
    auto entry = std::make_shared<RegistryStorage::Entry>();
    entry->authority = authority;
    entry->connection = connection;
    entry->requestedSessionID = physical.sessionID();
    state->m_entries.push_back(entry);
    (void)state->index(*entry, connection);
    entry->subscription = connection->subscribe(RegistryStorage::observer(state, entry));
    return {};
}

std::shared_ptr<WorkspaceConnection> WorkspaceConnectionRegistry::indexed(const WorkspaceAuthority& authority, const WorkspaceConnection& connection) const
{
    if (std::this_thread::get_id() != m_state->m_eventLoop)
        return nullptr;
    const RemoteSessionCore& physical = connection.connection();
    if (!physical.workspaceID() || !physical.daemonInstanceID())
        return nullptr;
    std::scoped_lock lock(m_state->m_mutex);
    for (const RegistryStorage::Key& key : RegistryStorage::keysOf(authority, physical)) {
        const auto found = m_state->m_workspaces.find(key);
        if (found == m_state->m_workspaces.end())
            continue;
        auto owner = found->second.lock();
        if (!owner || owner->connection().state() == State::Failed)
            continue;
        if (std::ranges::any_of(m_state->m_entries, [&](const auto& entry) { return entry->isActive && entry->connection.lock() == owner; }))
            return owner;
    }
    return nullptr;
}

} // namespace Ubiquitty::Session
