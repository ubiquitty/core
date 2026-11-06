// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Sync's topology tree (Docs/sync.md §6.10 "Topology never overtakes
// content", §7.3): how a tab's panes are split, as a client is told it in a
// workspace's structure on channel 0.
//
// A leaf carries the pane's size and the generation of the pane's latest
// Structural frame, the one that gave it that size. Channel 0 and a pane's
// channel can arrive in either order, so a client trusts the generation and
// not the order: it lays a pane out at a leaf's size only once the picture it
// holds is at least that generation, and until then keeps the picture it has,
// letterboxed. `paneRectangleFor` is that rule.
//
// The tree is a value: a workspace's structure record carries it
// (Source/Core/Session/SessionTreeWire.h), and a client attachment holds the
// one for the tab it draws.

#pragma once

#include "Core/Protocol/Sync/Messages.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace Ubiquitty::Protocol::Sync {

/// The most splits above a leaf: the one nesting bound every reader of a
/// split tree uses. A tab holds at most sixty-four panes, and a tab split one
/// pane at a time nests one split a pane, so no tab a host can hold is
/// deeper.
inline constexpr std::size_t maxTopologyDepth = 64;

/// The line a split divides its rectangle along.
enum class SplitAxis : std::uint8_t {
    /// Its children are side by side.
    Horizontal,
    /// Its children are one above the other.
    Vertical,
};

/// A node of the tree: a pane, or a split of two or more nodes.
struct TopologyNode {
    /// A leaf's pane; unused in a split.
    ID128 paneID {};
    /// The node's size in cells: a leaf's grid, and a split's extent as the
    /// host's layout computed it from its children's (the sum along its axis,
    /// the larger across).
    std::uint16_t rows = 0;
    std::uint16_t columns = 0;
    /// Where the node's rectangle starts within its tab, in cells from the
    /// tab's top left, as the host's layout placed it.
    std::uint16_t offsetRows = 0;
    std::uint16_t offsetColumns = 0;
    /// The generation of the pane's latest Structural frame.
    std::uint64_t structuralGeneration = 0;
    /// A split's axis and the share of its rectangle its first child takes,
    /// between 0 and 1; unused in a leaf.
    SplitAxis axis = SplitAxis::Horizontal;
    double ratio = 0.5;
    /// A split's children; a leaf has none.
    std::vector<TopologyNode> children {};

    /// Whether the node is a pane.
    [[nodiscard]] bool isLeaf() const { return children.empty(); }

    friend bool operator==(const TopologyNode&, const TopologyNode&) = default;
};

/// A workspace's panes, as a client is told them.
struct TopologyTree {
    /// Orders the trees a host instance sends: a client keeps the tree with
    /// the highest it has seen on a connection.
    std::uint64_t revision = 0;
    TopologyNode root;
    /// The pane shown alone over the rest, when one is.
    std::optional<ID128> zoomedPane;

    friend bool operator==(const TopologyTree&, const TopologyTree&) = default;
};

/// A pane's leaf in a tree.
///
/// - Parameters:
///   - tree: The tree.
///   - paneID: The pane.
/// - Returns: The leaf, or null when the tree has no such pane.
[[nodiscard]] const TopologyNode* findLeaf(const TopologyTree& tree, ID128 paneID);

/// The picture a client holds of a pane.
struct PanePicture {
    /// The newest generation applied.
    std::uint64_t generation = 0;
    /// The size of the grid it draws.
    std::uint16_t rows = 0;
    std::uint16_t columns = 0;
};

/// Where a client lays a pane out, and whether the picture fills it.
struct PaneRectangle {
    std::uint16_t rows = 0;
    std::uint16_t columns = 0;
    /// The picture is not the leaf's: it is drawn at its own size inside
    /// whatever room the layout gives, until the frame the leaf names lands.
    bool isLetterboxed = false;

    friend bool operator==(const PaneRectangle&, const PaneRectangle&) = default;
};

/// Decides a pane's rectangle from its leaf and the picture held: the leaf's
/// size once the picture is at least the leaf's generation and is that size,
/// otherwise the picture's own size, letterboxed. A rectangle that is not
/// letterboxed is always the size of the grid drawn in it.
///
/// - Parameters:
///   - leaf: The pane's leaf in the newest tree.
///   - picture: What the client holds of the pane.
/// - Returns: The rectangle.
[[nodiscard]] PaneRectangle paneRectangleFor(const TopologyNode& leaf, const PanePicture& picture);

} // namespace Ubiquitty::Protocol::Sync
