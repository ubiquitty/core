// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Protocol/Tmux/Tiler.h"

#include <algorithm>
#include <cstddef>
#include <utility>

namespace Ubiquitty::Protocol::Tmux {

namespace {

void collectSlots(const LayoutNode& node, std::vector<PaneSlot>& out)
{
    if (node.isLeaf()) {
        out.push_back({ node.paneID, node.x, node.y, node.width, node.height });
        return;
    }
    for (const LayoutNode& child : node.children)
        collectSlots(child, out);
}

bool containsPane(const LayoutNode& node, PaneID paneID)
{
    if (node.isLeaf())
        return node.paneID == paneID;
    for (const LayoutNode& child : node.children)
        if (containsPane(child, paneID))
            return true;
    return false;
}

// Places `node` at the given rect and shares its span among its children in
// the proportion they held. Answers false when a child would get no cell.
bool refitNode(LayoutNode& node, int x, int y, int width, int height)
{
    if (width < 1 || height < 1)
        return false;
    const int oldWidth = node.width;
    const int oldHeight = node.height;
    node.x = x;
    node.y = y;
    node.width = width;
    node.height = height;
    if (node.isLeaf() || node.children.empty())
        return true;
    const bool isSideBySide = node.split == Split::Columns;
    const int count = static_cast<int>(node.children.size());
    const int dividers = count - 1;
    const int oldSpan = (isSideBySide ? oldWidth : oldHeight) - dividers;
    const int newSpan = (isSideBySide ? width : height) - dividers;
    if (newSpan < count)
        return false;
    int placed = 0;
    int offset = isSideBySide ? x : y;
    for (int index = 0; index < count; ++index) {
        LayoutNode& child = node.children[static_cast<std::size_t>(index)];
        const int childOld = isSideBySide ? child.width : child.height;
        int share = 0;
        if (index == count - 1) {
            share = newSpan - placed;
        } else {
            share = oldSpan > 0 ? static_cast<int>(static_cast<long long>(childOld) * newSpan / oldSpan) : newSpan / count;
            // Every later child still needs a cell of its own.
            share = std::clamp(share, 1, newSpan - placed - (count - 1 - index));
        }
        const bool didFit = isSideBySide ? refitNode(child, offset, y, share, height) : refitNode(child, x, offset, width, share);
        if (!didFit)
            return false;
        placed += share;
        offset += share + 1;
    }
    return true;
}

} // namespace

bool resizeLayout(LayoutNode& root, int columns, int rows)
{
    LayoutNode refitted = root;
    if (!refitNode(refitted, root.x, root.y, columns, rows))
        return false;
    root = std::move(refitted);
    return true;
}

std::vector<PaneSlot> layoutSlots(const LayoutNode& root)
{
    std::vector<PaneSlot> slots;
    collectSlots(root, slots);
    return slots;
}

PaneID representativePane(const LayoutNode& root)
{
    // The leftmost leaf is the first leaf of the children-in-order walk; every
    // leaf in a tmux layout is a live pane, so no liveness check is needed here.
    const LayoutNode* node = &root;
    while (!node->isLeaf()) {
        if (node->children.empty())
            return invalidPaneID;
        node = &node->children.front();
    }
    return node->paneID;
}

PaneID resolveFocusedPane(const LayoutNode& root, PaneID activePaneID, PaneID previousFocusedPaneID)
{
    if (activePaneID >= 0 && containsPane(root, activePaneID))
        return activePaneID;
    if (previousFocusedPaneID >= 0 && containsPane(root, previousFocusedPaneID))
        return previousFocusedPaneID;
    return representativePane(root);
}

// Twin of Session::appTitleForSurface's tab arm, and a twin by necessity: this
// module is L1 and that one is L2, so it may not be called from here; the Linux
// arm trims this file, so it may not be called from there either. Sharing one
// definition would need a header holding both this predicate and TitleSurface
// below the layer split, and TitleSurface is the session's. Each site names the
// other so a change to one is a change to both.
std::string_view chooseTabTitle(std::string_view windowName, std::string_view paneOSCTitle) { return !windowName.empty() ? windowName : paneOSCTitle; }

} // namespace Ubiquitty::Protocol::Tmux
