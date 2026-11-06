// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// WorkspaceMirrorFixture.h — a workspace model as text, and the structure a
// host builds of one, for tests that hold a client's mirror to the host's
// model: two models that read alike here have the same windows, tabs, panes,
// splits and choices, under the same ids.

#pragma once

#include "Core/Panes/PaneTree.h"
#include "Core/Protocol/Tmux/PaneLock.h"
#include "Core/Session/Identity.h"
#include "Core/Session/LayoutTree.h"
#include "Core/Session/SessionTree.h"

#include <cstdint>
#include <functional>
#include <string>
#include <utility>

namespace UbiquittyTest {

/// What a test's host says of one pane.
struct MirrorPane {
    /// The session that fills the pane.
    std::uint32_t sessionID = 0;
    /// The session's grid.
    int columns = 80;
    int rows = 24;
};

/// Answers for a pane of a model.
using MirrorPaneLookup = std::function<MirrorPane(Ubiquitty::Session::Layout::PaneID)>;

/// A workspace model as text: every window, tab and pane with its id, what is
/// active, focused and zoomed, each tab's title, and each tab's split in the
/// layout grammar at the cells `paneOf` gives its panes.
///
/// - Parameters:
///   - model: The model.
///   - paneOf: Answers each pane's session and grid.
/// - Returns: The text.
inline std::string mirrorText(const Ubiquitty::Session::Layout::Workspace& model, const MirrorPaneLookup& paneOf)
{
    namespace Layout = Ubiquitty::Session::Layout;
    std::string text = "active window " + std::to_string(std::to_underlying(model.activeSurface())) + "\n";
    for (int surfaceIndex = 0; surfaceIndex < model.surfaceCount(); ++surfaceIndex) {
        const Layout::SurfaceID surface = model.surfaceIDAt(surfaceIndex);
        text += "window " + std::to_string(std::to_underlying(surface)) + " active tab " + std::to_string(std::to_underlying(model.activeTab(surface))) + " zoomed "
            + std::to_string(std::to_underlying(model.zoomedPane(surface))) + "\n";
        for (int tabIndex = 0; tabIndex < model.tabCount(surface); ++tabIndex) {
            const Layout::TabID tab = model.tabAt(surface, tabIndex);
            const Ubiquitty::Panes::Tree* tree = model.paneTree(surface, tab);
            text += "  tab " + std::to_string(std::to_underlying(tab)) + " \"" + std::string(model.tabTitle(surface, tab)) + "\"";
            if (!tree) {
                text += " no tree\n";
                continue;
            }
            text += " focused " + std::to_string(tree->focusedPaneID()) + " ";
            const auto dimensions = [](void* context, Ubiquitty::Protocol::Tmux::PaneID pane) -> Ubiquitty::Protocol::Tmux::PaneDimensions {
                const MirrorPane found = (*static_cast<const MirrorPaneLookup*>(context))(Layout::PaneID { static_cast<std::uint32_t>(pane) });
                return { found.columns, found.rows };
            };
            Ubiquitty::Protocol::Tmux::emitPaneLayout(*tree, tree->root(), dimensions, const_cast<void*>(static_cast<const void*>(&paneOf)), text);
            text += " sessions";
            Ubiquitty::Panes::forEachLeaf(*tree, tree->root(), [&](int pane) {
                text += " " + std::to_string(pane) + ":" + std::to_string(paneOf(Layout::PaneID { static_cast<std::uint32_t>(pane) }).sessionID);
            });
            text += "\n";
        }
    }
    return text;
}

/// What a host says of a workspace beside its model.
struct MirrorIdentity {
    /// The workspace's id.
    std::uint32_t workspaceID = 0;
    /// Its name.
    std::string name;
    /// Whether the host restored it after a restart.
    bool wasRestoredAfterHostRestart = false; // bool-ok: names the one state it reports
    /// The host instance its panes' ids are minted under.
    std::uint64_t daemonInstanceID = 17;
};

/// The structure a host builds of a workspace model, by the builder a host
/// uses.
///
/// - Parameters:
///   - model: The host's model.
///   - identity: What the host says of the workspace beside it.
///   - paneOf: Answers each pane's session and grid.
///   - revision: The structure's revision.
/// - Returns: The structure.
inline Ubiquitty::Session::TreeWorkspace structureOf(const Ubiquitty::Session::Layout::Workspace& model, const MirrorIdentity& identity, const MirrorPaneLookup& paneOf, std::uint64_t revision = 1)
{
    namespace Tree = Ubiquitty::Session;
    const Tree::TreeWorkspaceSource source { .id = identity.workspaceID, .name = identity.name, .layout = &model, .endsWhenDetached = false, .wasRestoredAfterHostRestart = identity.wasRestoredAfterHostRestart };
    const Tree::TreeHost host { .revision = revision, .nowUnixMilliseconds = 0, .workspaces = { &source, 1 }, .tags = nullptr, .restorable = {} };
    const Tree::Identity::LabelBook labels;
    Tree::SessionTree tree = Tree::buildSessionTree(host, labels, [&](std::uint32_t, Tree::Layout::PaneID pane) {
        const MirrorPane found = paneOf(pane);
        Tree::TreePaneFacts facts;
        facts.sessionID = found.sessionID;
        facts.paneID = { .low = found.sessionID, .high = identity.daemonInstanceID };
        facts.facts.rows = static_cast<std::uint16_t>(found.rows);
        facts.facts.columns = static_cast<std::uint16_t>(found.columns);
        return facts;
    });
    Tree::TreeWorkspace workspace = std::move(tree.workspaces.front());
    workspace.revision = revision;
    return workspace;
}

} // namespace UbiquittyTest
