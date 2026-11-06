// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_tiler.cpp — the pure tmux -CC tiling engine (Source/Core/Protocol/Tmux/Tiler).
// Layout-tree logic kept out of the frontends so it is headless-testable:
// per-pane grids, representative pane, focus policy, tab-title precedence.

#include "Core/Protocol/Tmux/Tiler.h"

#include <cstdio>
#include <string_view>

using namespace Ubiquitty::Protocol::Tmux;

static LayoutNode leaf(PaneID paneID, int x, int y, int w, int h)
{
    LayoutNode n;
    n.split = Split::Leaf;
    n.paneID = paneID;
    n.x = x;
    n.y = y;
    n.width = w;
    n.height = h;
    return n;
}

// A left/right (Columns) split of two 40x24 panes with a 1-col divider gap.
static LayoutNode lrSplit()
{
    LayoutNode root;
    root.split = Split::Columns;
    root.x = 0;
    root.y = 0;
    root.width = 81;
    root.height = 24;
    root.children.push_back(leaf(0, 0, 0, 40, 24));
    root.children.push_back(leaf(1, 41, 0, 40, 24));
    return root;
}

TEST(testLayoutSlots)
{
    auto slots = layoutSlots(lrSplit());
    CHECK(slots.size() == 2);
    // Each pane's authoritative grid comes from tmux's layout cells (bug 2 sizer).
    CHECK(slots[0].paneID == 0 && slots[0].x == 0 && slots[0].columns == 40 && slots[0].rows == 24);
    CHECK(slots[1].paneID == 1 && slots[1].x == 41 && slots[1].columns == 40 && slots[1].rows == 24);
    // A lone pane (leaf root) yields a single full-window slot.
    auto single = layoutSlots(leaf(7, 0, 0, 80, 24));
    CHECK(single.size() == 1 && single[0].paneID == 7 && single[0].columns == 80);
}

TEST(testRepresentativeAndFocus)
{
    auto root = lrSplit();
    CHECK(representativePane(root) == 0); // leftmost leaf
    CHECK(representativePane(leaf(9, 0, 0, 80, 24)) == 9);
    // active present -> active
    CHECK(resolveFocusedPane(root, 1, -1) == 1);
    // active absent, previous present -> previous
    CHECK(resolveFocusedPane(root, 99, 0) == 0);
    // neither present -> leftmost
    CHECK(resolveFocusedPane(root, 99, 99) == 0);
    CHECK(resolveFocusedPane(root, -1, -1) == 0);
}

TEST(testTabTitle)
{
    CHECK(chooseTabTitle("build", "vim") == std::string_view("build")); // window wins
    CHECK(chooseTabTitle("", "vim") == std::string_view("vim")); // no window name
    CHECK(chooseTabTitle("", "") == std::string_view(""));
}

UBIQUITTY_TEST_MAIN()
