// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>

namespace Ubiquitty::Panes {

/// Split orientation of an internal node. Vertical stacks its two
/// children side-by-side (a vertical divider between them); Horizontal
/// stacks them top/bottom (a horizontal divider). Mirrors how a user
/// reads "split vertically" = two columns.
enum class Orientation : uint8_t {
    Vertical = 0,
    Horizontal = 1,
};

/// Direction for moveFocus. Maps to arrow keys in the frontend.
enum class Direction : uint8_t {
    Left = 0,
    Right = 1,
    Up = 2,
    Down = 3,
};

/// Which side of a fresh split an inserted or moved pane takes. First is
/// the left/top child, Second the right/bottom — mirroring Node.first /
/// Node.second.
enum class InsertSide : uint8_t {
    First = 0,
    Second = 1,
};

/// Identifies whether insertion divides one pane or wraps the complete layout.
enum class InsertTarget : uint8_t {
    Pane = 0,
    Root = 1,
};

/// Sentinel index meaning "no node".
constexpr int invalidNode = -1;

/// Sentinel pane id for an internal (non-leaf) node.
constexpr int noPane = -1;

/// One tree node. A leaf has children == invalidNode and a valid
/// paneID; an internal split has two valid children, paneID == noPane,
/// and a meaningful orientation + ratio. `parent` is invalidNode only
/// for the root.
struct Node {
    int parent;
    int first; // child shown left/top, or invalidNode on a leaf
    int second; // child shown right/bottom, or invalidNode on a leaf
    int paneID; // opaque frontend id on a leaf, noPane on a split
    Orientation orientation; // meaningful only on a split
    float ratio; // first child's fraction in [0,1] on a split
};

/// Maximum simultaneous panes. A binary tree of N leaves has N-1 internal
/// nodes, so the node pool is sized 2*maxPanes-1. 64 panes is far past
/// any practical window split.
constexpr int maxPanes = 64;
constexpr int maxNodes = 2 * maxPanes - 1;

/// Default first-child fraction for a fresh split: an even halving.
constexpr float defaultRatio = 0.5f;

} // namespace Ubiquitty::Panes
