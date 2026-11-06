// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// WorkspaceStructureFixture.h — a workspace's structure as a host builds and
// sends it, for tests whose host is a double: the record a client's mirror is
// rebuilt from (Docs/sync.md §7.3), and the offer that puts it on a host
// connection's stream.

#pragma once

#include "Core/Protocol/Sync/Connection.h"
#include "Core/Protocol/Sync/Topology.h"
#include "Core/Session/SessionTree.h"
#include "Core/Session/SessionTreeWire.h"

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace UbiquittyTest {

/// One pane of a fixture's workspace.
struct StructurePane {
    /// The pane's id in the host's layout.
    std::uint32_t pane = 0;
    /// The session that fills it.
    std::uint32_t sessionID = 0;
    /// The session's grid.
    int columns = 80;
    int rows = 24;
};

/// A workspace of one window and one tab whose panes lie side by side, each
/// split off the ones after it, as a host's structure names it.
///
/// - Parameters:
///   - workspaceID: The workspace's id.
///   - name: Its name.
///   - panes: Its panes, left to right; at least one.
///   - daemonInstanceID: The host instance its panes' ids are minted under.
///   - revision: The structure's revision.
///   - focusedPane: The index in `panes` of the pane the host focuses.
/// - Returns: The workspace.
inline Ubiquitty::Session::TreeWorkspace rowWorkspace(std::uint32_t workspaceID, std::string name, std::span<const StructurePane> panes, std::uint64_t daemonInstanceID,
    std::uint64_t revision = 1, std::size_t focusedPane = 0)
{
    namespace Tree = Ubiquitty::Session;
    using Ubiquitty::Protocol::Sync::TopologyNode;
    Tree::TreeWorkspace workspace;
    workspace.id = workspaceID;
    workspace.revision = revision;
    workspace.name = std::move(name);
    Tree::TreeWindow window;
    window.id = Tree::Layout::SurfaceID { 1 };
    window.isHostActive = true;
    Tree::TreeTab tab;
    tab.id = Tree::Layout::TabID { 1 };
    tab.isHostActive = true;
    tab.hostFocusedPane = focusedPane;
    for (const StructurePane& each : panes) {
        Tree::TreePane pane;
        pane.id = Tree::Layout::PaneID { each.pane };
        pane.sessionID = each.sessionID;
        tab.panes.push_back(std::move(pane));
    }
    // Built from the last pane up, so the leaves read in pane order.
    const auto leafOf = [&](const StructurePane& each) {
        TopologyNode leaf;
        leaf.paneID = { .low = each.sessionID, .high = daemonInstanceID };
        Tree::SessionTreeDetail::sizeLeaf(leaf, each.rows, each.columns);
        return leaf;
    };
    TopologyNode node = leafOf(panes.back());
    for (std::size_t index = panes.size() - 1; index > 0; --index) {
        TopologyNode split;
        split.axis = Ubiquitty::Protocol::Sync::SplitAxis::Horizontal;
        split.children.push_back(leafOf(panes[index - 1]));
        split.children.push_back(std::move(node));
        Tree::SessionTreeDetail::sizeSplit(split);
        node = std::move(split);
    }
    Tree::SessionTreeDetail::placeNode(node, 0, 0);
    tab.split = std::move(node);
    window.tabs.push_back(std::move(tab));
    workspace.windows.push_back(std::move(window));
    return workspace;
}

/// Offers a workspace's structure on a host connection, as a daemon does
/// when the workspace changes; the connection's next service sends it.
///
/// - Parameters:
///   - connection: The host's end, attached.
///   - workspace: The workspace, its revision set.
/// - Returns: Whether the structure encoded.
[[nodiscard]] inline bool offerStructure(Ubiquitty::Protocol::Sync::HostConnection& connection, const Ubiquitty::Session::TreeWorkspace& workspace)
{
    auto record = std::make_shared<std::vector<std::uint8_t>>();
    if (!Ubiquitty::Session::encodeWorkspaceStructure(*record, workspace))
        return false;
    connection.offerWorkspaceStructure(workspace.id, workspace.revision, std::move(record));
    return true;
}

} // namespace UbiquittyTest
