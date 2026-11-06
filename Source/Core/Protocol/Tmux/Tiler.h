// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/Protocol/Tmux/LayoutNode.h"

#include <string_view>
#include <vector>

namespace Ubiquitty::Protocol::Tmux {

// The authoritative cell grid of one leaf pane, taken from tmux's layout tree. A
// pane's Ubiquitty::Terminal must be sized to this, or cursor-addressed %output lands at the
// wrong column and interleaves (the split-garble). Sizing is tmux's to own; the
// frontend renders each pane at its slot.
struct PaneSlot {
    PaneID paneID = invalidPaneID;
    int x = 0;
    int y = 0; // cell origin, top-left
    int columns = 0;
    int rows = 0;
};

// Flatten a window's layout tree into one PaneSlot per leaf, in top-left-biased
// depth-first order (the order the frontend mounts panes). Internal nodes carry
// no pane and are not emitted.
[[nodiscard]] std::vector<PaneSlot> layoutSlots(const LayoutNode& root);

// The @window → %pane representative: the leftmost leaf of the current layout, so
// a later %window-renamed still resolves after a reconcile drops the old
// representative. invalidPaneID only for a tree with no leaf.
[[nodiscard]] PaneID representativePane(const LayoutNode& root);

// The focus target for a reconcile. tmux's active pane wins when it is present in
// the tree; else the previously-focused pane if still present; else the leftmost
// leaf (representativePane). invalidPaneID only for an empty tree.
[[nodiscard]] PaneID resolveFocusedPane(const LayoutNode& root, PaneID activePaneID, PaneID previousFocusedPaneID);

/// Refits a window's layout to a new window size, keeping its splits in
/// proportion.
///
/// The shape tmux gives a window when its client resizes: each split keeps its
/// one-cell dividers and shares the rest among its children in the ratio they
/// had, the last child taking the rounding. An estimate — tmux's own
/// `%layout-change` is the authority and corrects any cell it placed
/// differently — made so a window hidden behind another tab is the right size
/// before that confirmation arrives.
///
/// - Parameters:
///   - root: The layout, refitted in place.
///   - columns: The window's new columns.
///   - rows: The window's new rows.
/// - Returns: Whether every pane kept at least one cell; false leaves `root`
///   untouched, for a size too small to hold the split.
bool resizeLayout(LayoutNode& root, int columns, int rows);

// Tab-title precedence: the tmux WINDOW name wins when non-empty; otherwise the
// focused pane app's OSC-2 title. The window name is never merged into a pane
// Terminal's own title — this only chooses what the TAB shows.
[[nodiscard]] std::string_view chooseTabTitle(std::string_view windowName, std::string_view paneOSCTitle);

} // namespace Ubiquitty::Protocol::Tmux
