// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/Codec/JSON.h"
#include "Core/Session/WorkspaceCapture.h"

#include <algorithm>
#include <expected>

namespace Ubiquitty::Session {

/// The last publication answer, distinct from the individual PTY handoffs.
struct SharedWorkspaceResult {
    bool isPending = false;
    std::uint32_t workspaceID = 0;
    std::string detail;
};

/// An arrangement of already adopted terminals, in depth-first leaf order.
/// Session numbers belong to one explicitly pinned daemon; no shell is launched
/// when this arrangement is installed.
struct SharedWorkspace {
    static constexpr std::size_t maxPanes = 256;
    // Leave room for the session handles and JSON envelope in one Sync call.
    static constexpr std::size_t maxArchiveBytes = 60000;

    Workspace arrangement;
    std::vector<std::uint32_t> sessions;
    /// Zero creates a workspace; otherwise this window is added to that workspace.
    std::uint32_t destinationWorkspaceID = 0;

    /// Appends a native tab's arrangement to the one shared window.
    /// - Parameters:
    ///   - other: The native tab, including all its model tabs and panes.
    ///   - isSelected: Whether this native tab is selected.
    /// - Returns: Whether the complete arrangement fit, without partial append.
    [[nodiscard]] bool append(const SharedWorkspace& other, bool isSelected)
    {
        if (other.arrangement.windows.size() != 1 || other.sessions.empty()
            || sessions.size() > maxPanes || other.sessions.size() > maxPanes - sessions.size())
            return false;
        for (const auto session : other.sessions) {
            if (!session || std::ranges::find(sessions, session) != sessions.end() || std::ranges::count(other.sessions, session) != 1)
                return false;
        }
        if (arrangement.windows.empty())
            arrangement.windows.emplace_back();
        auto& window = arrangement.windows.front();
        const auto& incoming = other.arrangement.windows.front();
        if (isSelected) {
            window.activeTab = static_cast<int>(window.tabs.size()) + incoming.activeTab;
            window.zoomedPaneIndex = incoming.zoomedPaneIndex;
        }
        window.tabs.insert(window.tabs.end(), incoming.tabs.begin(), incoming.tabs.end());
        sessions.insert(sessions.end(), other.sessions.begin(), other.sessions.end());
        return true;
    }

    /// Builds a correlated, bounded request; the host validates the arrangement.
    [[nodiscard]] std::string request(std::uint64_t daemonInstanceID, const std::string& correlation) const
    {
        if (!daemonInstanceID || correlation.empty() || sessions.empty() || sessions.size() > maxPanes)
            return {};
        const auto archive = serializeWorkspaceText(arrangement);
        if (archive.empty() || archive.size() > maxArchiveBytes)
            return {};
        using Codec::JSON::Value;
        auto parameters = Value::object();
        parameters.set("daemonInstanceID", Value::string(std::to_string(daemonInstanceID)));
        parameters.set("arrangement", Value::string(archive));
        parameters.set("workspace", Value::number(destinationWorkspaceID));
        auto ids = Value::array();
        for (const auto session : sessions)
            ids.push(Value::number(session));
        parameters.set("sessions", std::move(ids));
        auto call = Value::object();
        call.set("jsonrpc", Value::string("2.0"));
        call.set("id", Value::string(correlation));
        call.set("method", Value::string("workspace.share"));
        call.set("params", std::move(parameters));
        return Codec::JSON::serialize(call);
    }
};

/// Why an incoming arrangement cannot identify a whole set of adopted panes.
enum class SharedWorkspaceError : std::uint8_t {
    InvalidArrangement,
    InvalidSessions,
};

/// Rebinds restored leaves to their existing host pane identities atomically.
/// Other windows, tab identities, focus and split ratios remain authoritative.
[[nodiscard]] inline bool rebindSharedPanes(Layout::Workspace& layout,
    std::span<const std::pair<Layout::PaneID, Layout::PaneID>> replacements)
{
    std::size_t tabCount = 0;
    for (int index = 0; index < layout.surfaceCount(); ++index)
        tabCount += static_cast<std::size_t>(layout.tabCount(layout.surfaceIDAt(index)));
    std::vector<Panes::Tree> trees;
    std::vector<Layout::AdoptedTab> tabs;
    std::vector<Layout::AdoptedSurface> surfaces;
    trees.reserve(tabCount);
    tabs.reserve(tabCount);
    surfaces.reserve(static_cast<std::size_t>(layout.surfaceCount()));
    const auto remap = [&](Layout::PaneID pane) {
        const auto found = std::ranges::find(replacements, pane, &std::pair<Layout::PaneID, Layout::PaneID>::first);
        return found == replacements.end() ? pane : found->second;
    };
    for (int index = 0; index < layout.surfaceCount(); ++index) {
        const auto* surface = layout.surface(layout.surfaceIDAt(index));
        const auto first = tabs.size();
        for (int tabIndex = 0; tabIndex < surface->tabCount; ++tabIndex) {
            const auto& tab = surface->tabs[tabIndex];
            auto state = tab.panes.continuation();
            for (std::size_t node = 0; node < state.nodes.size(); ++node) {
                if (state.used[node] && tab.panes.isLeaf(static_cast<int>(node)))
                    state.nodes[node].paneID = static_cast<int>(remap(static_cast<Layout::PaneID>(state.nodes[node].paneID)));
            }
            trees.emplace_back();
            if (!trees.back().restoreContinuation(state))
                return false;
            tabs.push_back({ tab.id, &trees.back(), tab.title });
        }
        surfaces.push_back({ surface->id, surface->tabs[surface->activeTab].id, remap(surface->zoomedPane),
            std::span { tabs }.subspan(first) });
    }
    return layout.adoptWorkspace(surfaces, layout.activeSurface());
}

/// Reads a bounded arrangement and requires exactly one distinct session per leaf.
[[nodiscard]] inline std::expected<SharedWorkspace, SharedWorkspaceError> readSharedWorkspace(const Codec::JSON::Value& parameters)
{
    SharedWorkspace result;
    const double destination = parameters.numberOr("workspace");
    if (!(destination >= 0 && destination <= UINT32_MAX) || destination != static_cast<std::uint32_t>(destination))
        return std::unexpected(SharedWorkspaceError::InvalidArrangement);
    result.destinationWorkspaceID = static_cast<std::uint32_t>(destination);
    const auto archive = parameters.stringOr("arrangement");
    const auto* sessions = parameters.find("sessions");
    if (archive.empty() || archive.size() > SharedWorkspace::maxArchiveBytes
        || !deserializeWorkspaceText(archive, result.arrangement) || result.arrangement.windows.size() != 1)
        return std::unexpected(SharedWorkspaceError::InvalidArrangement);
    if (!sessions || !sessions->isArray() || sessions->items().empty() || sessions->items().size() > SharedWorkspace::maxPanes)
        return std::unexpected(SharedWorkspaceError::InvalidSessions);
    for (const auto& value : sessions->items()) {
        const double number = value.asNumber();
        if (!value.isNumber() || !(number >= 1 && number <= UINT32_MAX) || number != static_cast<std::uint32_t>(number))
            return std::unexpected(SharedWorkspaceError::InvalidSessions);
        const auto session = static_cast<std::uint32_t>(number);
        if (std::ranges::find(result.sessions, session) != result.sessions.end())
            return std::unexpected(SharedWorkspaceError::InvalidSessions);
        result.sessions.push_back(session);
    }
    Layout::Workspace checked;
    std::size_t leaves = 0;
    if (!Layout::restoreFromSnapshot(result.arrangement, checked, [](void* context, Layout::PaneID, const Layout::PaneInfo&) { ++*static_cast<std::size_t*>(context); }, &leaves) || leaves != result.sessions.size())
        return std::unexpected(SharedWorkspaceError::InvalidArrangement);
    return result;
}

} // namespace Ubiquitty::Session
