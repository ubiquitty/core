// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/Protocol/Tmux/PaneID.h"

#include <cstdint>
#include <vector>

namespace Ubiquitty::Protocol::Tmux {

// How a layout node divides its children. tmux layout strings encode this as
// `{}` (left→right columns) and `[]` (top→bottom rows); a leaf is a single pane.
enum class Split : std::uint8_t {
    Leaf,
    Columns, // {} — children laid out left → right
    Rows, // [] — children laid out top → bottom
};

// One node of a window's layout tree. Geometry is the absolute CELL rect tmux
// reports (origin top-left). Internal nodes (Columns/Rows) have children and
// paneID == invalidPaneID; leaves carry a paneID and no children.
struct LayoutNode {
    Split split = Split::Leaf;
    PaneID paneID = invalidPaneID;
    int x = 0, y = 0, width = 0, height = 0; // cells
    std::vector<LayoutNode> children; // internal nodes only

    [[nodiscard]] bool isLeaf() const { return split == Split::Leaf; }
};

} // namespace Ubiquitty::Protocol::Tmux
