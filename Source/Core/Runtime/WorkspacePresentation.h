// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/Runtime/WorkspaceSession.h"
#include "Core/Session/SessionTreeWire.h"

#include <algorithm>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace Ubiquitty::Runtime {

/// Retains the sessions a native workspace presents, without owning its topology.
/// Every call belongs to the frontend's presentation thread. The local model
/// or the remote physical connection remains the only authority; the windows
/// returned here are immutable drawing plans, never a second mutable workspace.
class WorkspacePresentation {
public:
    /// Describes one host-ordered tab without granting topology mutation.
    struct Tab {
        Session::Layout::TabID id = Session::Layout::invalidTab;
        std::string title;
        std::string label;
        Panes::Tree panes;
    };

    /// Describes one native window under the authoritative surface identity.
    struct Window {
        Session::Layout::SurfaceID id = Session::Layout::invalidSurface;
        std::string label;
        Session::Layout::TabID activeTab = Session::Layout::invalidTab;
        Session::Layout::PaneID zoomedPane = Session::Layout::invalidPane;
        std::vector<Tab> tabs;
    };

    /// Makes a started passive session, with the frontend's clients installed.
    /// It returns one owned reference and must not launch a program or re-enter
    /// this presentation. The source workspace supplies the only carrier.
    using MakeSession = TerminalSession* (*)(void*);

    /// Reports whether a complete owner revision changed the drawing plan.
    enum class RefreshOutcome : std::uint8_t {
        Unavailable,
        Unchanged,
        Changed,
        Refused,
    };

    explicit WorkspacePresentation(WorkspaceSession& local)
        : m_local(&local)
    {
        retainWorkspaceSession(m_local.get());
    }

    WorkspacePresentation(TerminalSession& anchor, MakeSession makeSession, void* context)
        : m_anchor(retained(anchor))
        , m_makeSession(makeSession)
        , m_makeSessionContext(context)
    {
    }

    WorkspacePresentation(const WorkspacePresentation&) = delete;
    WorkspacePresentation& operator=(const WorkspacePresentation&) = delete;

    /// Builds every destination before retiring any previous binding, so a pane
    /// moved between windows keeps its terminal and receives no second attach.
    /// An incomplete owner or a failed new binding leaves the drawing plan intact.
    [[nodiscard]] RefreshOutcome refresh()
    {
        if (m_isRefreshing)
            return RefreshOutcome::Unavailable;
        m_isRefreshing = true;
        struct RefreshScope {
            bool& isRefreshing;
            ~RefreshScope() { isRefreshing = false; }
        } scope { m_isRefreshing };

        std::vector<Window> windows;
        std::vector<Binding> bindings;
        Session::Layout::SurfaceID activeWindow = Session::Layout::invalidSurface;
        const auto* structure = m_local ? nullptr : m_anchor->remoteWorkspaceStructureSnapshot();
        const auto surfaceCount = m_local ? m_local->surfaceCount() : m_anchor->remoteWorkspaceSurfaceCount();
        if (!m_local && (!structure || !surfaceCount))
            return RefreshOutcome::Unavailable;
        if (surfaceCount < 0 || surfaceCount > Session::Layout::maxSurfaces)
            return RefreshOutcome::Refused;
        if (structure && structure->windows.size() != static_cast<std::size_t>(surfaceCount))
            return RefreshOutcome::Refused;
        activeWindow = m_local ? m_local->activeSurface() : m_anchor->remoteWorkspaceActiveSurface();

        for (std::ptrdiff_t windowIndex = 0; windowIndex < surfaceCount; ++windowIndex) {
            Window window;
            window.id = m_local ? m_local->surfaceAt(static_cast<int>(windowIndex)) : m_anchor->remoteWorkspaceSurfaceAt(windowIndex);
            const auto* sourceWindow = structure ? &structure->windows[static_cast<std::size_t>(windowIndex)] : nullptr;
            if (sourceWindow) {
                if (sourceWindow->id != window.id)
                    return RefreshOutcome::Refused;
                window.label = sourceWindow->label;
            }
            window.activeTab = m_local ? m_local->activeTab(window.id) : m_anchor->remoteWorkspaceActiveTab(window.id);
            window.zoomedPane = m_local ? m_local->zoomedPane(window.id) : m_anchor->remoteWorkspaceZoomedPane(window.id);
            const auto tabCount = m_local ? m_local->tabCount(window.id) : m_anchor->remoteWorkspaceTabCount(window.id);
            if (window.id == Session::Layout::invalidSurface || tabCount <= 0 || tabCount > Session::Layout::maxTabsPerSurface)
                return RefreshOutcome::Refused;
            if (sourceWindow && sourceWindow->tabs.size() != static_cast<std::size_t>(tabCount))
                return RefreshOutcome::Refused;
            bool hasUnboundLocalPane = false;
            const auto bindingsBefore = bindings.size();
            for (std::ptrdiff_t tabIndex = 0; tabIndex < tabCount; ++tabIndex) {
                Tab tab;
                tab.id = m_local ? m_local->tabAt(window.id, static_cast<int>(tabIndex)) : m_anchor->remoteWorkspaceTabAt(window.id, tabIndex);
                if (tab.id == Session::Layout::invalidTab)
                    return RefreshOutcome::Refused;
                tab.title = m_local ? m_local->tabTitle(window.id, tab.id) : m_anchor->remoteWorkspaceTabTitle(window.id, tab.id);
                if (sourceWindow) {
                    const auto& sourceTab = sourceWindow->tabs[static_cast<std::size_t>(tabIndex)];
                    if (sourceTab.id != tab.id)
                        return RefreshOutcome::Refused;
                    tab.label = sourceTab.label;
                }
                if (m_local) {
                    const auto* tree = m_local->paneTree(window.id, tab.id);
                    if (!tree)
                        return RefreshOutcome::Refused;
                    tab.panes = *tree;
                } else
                    tab.panes = m_anchor->remoteWorkspacePaneTreeSnapshot(window.id, tab.id);
                for (int index = 0; index < Panes::maxNodes; ++index) {
                    if (!tab.panes.isLeaf(index))
                        continue;
                    const auto pane = Session::Layout::PaneID { static_cast<std::uint32_t>(tab.panes.node(index).paneID) };
                    if (pane == Session::Layout::invalidPane || find(bindings, pane))
                        return RefreshOutcome::Refused;
                    if (m_local) {
                        if (auto* session = m_local->session(pane))
                            bindings.push_back({ pane, 0, retained(*session) });
                        else
                            hasUnboundLocalPane = true;
                        continue;
                    }
                    if (bindings.size() >= Session::maxStructureLeaves)
                        return RefreshOutcome::Refused;
                    const auto sessionID = m_anchor->remoteWorkspaceSessionForPane(pane);
                    if (!sessionID)
                        return RefreshOutcome::Unavailable;
                    if (std::ranges::any_of(bindings, [&](const auto& binding) { return binding.sessionID == sessionID; }))
                        return RefreshOutcome::Refused;
                    if (const auto* previous = find(m_bindings, pane)) {
                        if (previous->sessionID != sessionID)
                            return RefreshOutcome::Refused;
                        bindings.push_back({ pane, sessionID, retained(*previous->session) });
                    } else if (m_anchor->remoteSessionID() == sessionID)
                        bindings.push_back({ pane, sessionID, retained(*m_anchor) });
                    else {
                        SessionReference session(m_makeSession ? m_makeSession(m_makeSessionContext) : nullptr);
                        if (!session || !session->attachRemoteSibling(*m_anchor, sessionID))
                            return RefreshOutcome::Refused;
                        bindings.push_back({ pane, sessionID, std::move(session) });
                    }
                }
                window.tabs.push_back(std::move(tab));
            }
            // Layout::Workspace begins with an unbound placeholder. It is not
            // a native window until WorkspaceSession has made its first pane.
            if (hasUnboundLocalPane) {
                if (bindings.size() != bindingsBefore)
                    return RefreshOutcome::Refused;
                continue;
            }
            windows.push_back(std::move(window));
        }
        if (std::ranges::none_of(windows, [&](const auto& window) { return window.id == activeWindow; }))
            activeWindow = Session::Layout::invalidSurface;
        const std::string name = structure ? structure->name : std::string {};
        const std::string label = structure ? structure->label : std::string {};
        const bool didChange = !sameWindows(m_windows, windows) || m_activeWindow != activeWindow || !sameBindings(m_bindings, bindings)
            || m_name != name || m_label != label;
        m_windows = std::move(windows);
        m_activeWindow = activeWindow;
        m_bindings = std::move(bindings);
        m_name = name;
        m_label = label;
        if (m_anchor && !m_bindings.empty() && std::ranges::none_of(m_bindings, [&](const auto& binding) { return binding.session.get() == m_anchor.get(); }))
            m_anchor = retained(*m_bindings.front().session);
        return didChange ? RefreshOutcome::Changed : RefreshOutcome::Unchanged;
    }

    [[nodiscard]] std::span<const Window> windows() const { return m_windows; }
    [[nodiscard]] Session::Layout::SurfaceID activeWindow() const { return m_activeWindow; }
    /// Returns the name from the most recent complete owner revision.
    [[nodiscard]] std::string_view name() const { return m_name; }
    /// Returns the workspace label separately from its display name.
    [[nodiscard]] std::string_view label() const { return m_label; }

    /// Borrows the retained endpoint of an exact pane in the latest complete plan.
    [[nodiscard]] TerminalSession* session(Session::Layout::PaneID pane) const
    {
        const auto* binding = find(m_bindings, pane);
        return binding ? binding->session.get() : nullptr;
    }

private:
    /// Releases the retained local topology after every pane binding.
    struct ModelRelease {
        void operator()(WorkspaceSession* model) const { releaseWorkspaceSession(model); }
    };

    /// Releases one retained terminal endpoint.
    struct SessionRelease {
        void operator()(TerminalSession* session) const { releaseTerminalSession(session); }
    };
    using SessionReference = std::unique_ptr<TerminalSession, SessionRelease>;

    [[nodiscard]] static SessionReference retained(TerminalSession& session)
    {
        retainTerminalSession(&session);
        return SessionReference(&session);
    }

    /// Pins an authoritative pane identity to its stable terminal endpoint.
    struct Binding {
        Session::Layout::PaneID pane = Session::Layout::invalidPane;
        std::uint32_t sessionID = 0;
        SessionReference session;
    };

    [[nodiscard]] static const Binding* find(const std::vector<Binding>& bindings, Session::Layout::PaneID pane)
    {
        const auto found = std::ranges::find(bindings, pane, &Binding::pane);
        return found != bindings.end() ? &*found : nullptr;
    }

    [[nodiscard]] static bool sameBindings(const std::vector<Binding>& previous, const std::vector<Binding>& next)
    {
        return previous.size() == next.size() && std::ranges::all_of(next, [&](const auto& binding) {
            const auto* old = find(previous, binding.pane);
            return old && old->sessionID == binding.sessionID && old->session.get() == binding.session.get();
        });
    }

    [[nodiscard]] static bool sameTree(const Panes::Tree& first, const Panes::Tree& second)
    {
        const auto a = first.continuation();
        const auto b = second.continuation();
        if (a.used != b.used || a.root != b.root || a.focused != b.focused || a.paneCount != b.paneCount || a.nodeCount != b.nodeCount)
            return false;
        for (int index = 0; index < Panes::maxNodes; ++index) {
            if (!a.used[index])
                continue;
            const auto& left = a.nodes[index];
            const auto& right = b.nodes[index];
            if (left.parent != right.parent || left.first != right.first || left.second != right.second || left.paneID != right.paneID)
                return false;
            if (left.first != Panes::invalidNode && (left.orientation != right.orientation || left.ratio != right.ratio))
                return false;
        }
        return true;
    }

    [[nodiscard]] static bool sameWindows(const std::vector<Window>& previous, const std::vector<Window>& next)
    {
        if (previous.size() != next.size())
            return false;
        for (std::size_t index = 0; index < next.size(); ++index) {
            const auto& a = previous[index];
            const auto& b = next[index];
            if (a.id != b.id || a.label != b.label || a.activeTab != b.activeTab || a.zoomedPane != b.zoomedPane || a.tabs.size() != b.tabs.size())
                return false;
            for (std::size_t tab = 0; tab < a.tabs.size(); ++tab) {
                if (a.tabs[tab].id != b.tabs[tab].id || a.tabs[tab].title != b.tabs[tab].title || a.tabs[tab].label != b.tabs[tab].label || !sameTree(a.tabs[tab].panes, b.tabs[tab].panes))
                    return false;
            }
        }
        return true;
    }

    std::unique_ptr<WorkspaceSession, ModelRelease> m_local;
    SessionReference m_anchor;
    MakeSession m_makeSession = nullptr;
    void* m_makeSessionContext = nullptr;
    std::vector<Window> m_windows;
    std::string m_name;
    std::string m_label;
    Session::Layout::SurfaceID m_activeWindow = Session::Layout::invalidSurface;
    std::vector<Binding> m_bindings;
    bool m_isRefreshing = false;
};

} // namespace Ubiquitty::Runtime
