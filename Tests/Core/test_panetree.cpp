// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_panetree.cpp — host coverage for the portable split/pane layout
// model (Source/Core/Panes/PaneTree.{h,cpp}).
//
// Compiles against bare libc++ — no platform include. Pins the tree
// invariants the macOS NSSplitView frontend mirrors: split keeps the
// old pane + adds a new focused one, close collapses one level and
// promotes the sibling, directional focus lands on the spatially
// nearest leaf.

#include "Core/Panes/PaneTree.h"

#include <cstdio>
#include <vector>

using Ubiquitty::Panes::Direction;
using Ubiquitty::Panes::InsertSide;
using Ubiquitty::Panes::invalidNode;
using Ubiquitty::Panes::noPane;
using Ubiquitty::Panes::Orientation;
using Ubiquitty::Panes::Tree;

// Count live leaves by walking the node array via the public accessor.
static int leafCount(const Tree& t)
{
    int n = 0;
    for (int i = 0; i < Ubiquitty::Panes::maxNodes; ++i) {
        if (t.isLeaf(i))
            ++n;
    }
    return n;
}

// A fresh tree is one focused pane and nothing else.
TEST(testSinglePane)
{
    Tree t(7);
    CHECK(t.paneCount() == 1);
    CHECK(t.focusedPaneID() == 7);
    CHECK(t.isLeaf(t.root()));
    CHECK(t.isLeaf(t.focusedNode()));
    CHECK(leafCount(t) == 1);
    // Closing the last pane is refused — a window keeps a pane.
    CHECK(!t.closeFocused());
    CHECK(t.paneCount() == 1);
    // Moving focus with nowhere to go is a no-op.
    CHECK(!t.moveFocus(Direction::Left));
    CHECK(!t.moveFocus(Direction::Down));
}

// Vertical split: old pane stays as first child, new pane is second +
// focused, root becomes a Vertical split.
TEST(testVerticalSplit)
{
    Tree t(1);
    int nl = t.split(Orientation::Vertical, 2);
    CHECK(nl != invalidNode);
    CHECK(t.paneCount() == 2);
    CHECK(t.focusedPaneID() == 2);

    int root = t.root();
    CHECK(!t.isLeaf(root));
    const auto& rn = t.node(root);
    CHECK(rn.orientation == Orientation::Vertical);
    CHECK(rn.paneID == noPane);
    CHECK(t.isLeaf(rn.first));
    CHECK(t.isLeaf(rn.second));
    CHECK(t.node(rn.first).paneID == 1);
    CHECK(t.node(rn.second).paneID == 2);
    CHECK(t.node(rn.first).parent == root);
    CHECK(t.node(rn.second).parent == root);
}

// Horizontal split records the Horizontal orientation.
TEST(testHorizontalSplit)
{
    Tree t(1);
    t.split(Orientation::Horizontal, 2);
    const auto& rn = t.node(t.root());
    CHECK(rn.orientation == Orientation::Horizontal);
    CHECK(t.paneCount() == 2);
}

// Duplicate pane ids are rejected; the tree is unchanged.
TEST(testDuplicateIdRejected)
{
    Tree t(5);
    CHECK(t.split(Orientation::Vertical, 5) == invalidNode);
    CHECK(t.paneCount() == 1);
}

// Ratio is clamped into the interior band.
TEST(testRatioClamp)
{
    Tree t(1);
    t.split(Orientation::Vertical, 2, 5.0f);
    CHECK(t.node(t.root()).ratio <= 0.95f);
    Tree u(1);
    u.split(Orientation::Vertical, 2, -1.0f);
    CHECK(u.node(u.root()).ratio >= 0.05f);
    // setRatio on the split adjusts it; on a leaf it fails.
    int root = t.root();
    CHECK(t.setRatio(root, 0.3f));
    CHECK(t.node(root).ratio > 0.29f && t.node(root).ratio < 0.31f);
    CHECK(!t.setRatio(t.node(root).first, 0.5f));
}

// Close collapses one level: the sibling subtree replaces the parent.
TEST(testCloseCollapses)
{
    Tree t(1);
    t.split(Orientation::Vertical, 2); // root: [1 | 2*], focus 2
    CHECK(t.focusedPaneID() == 2);
    CHECK(t.closeFocused()); // close 2 -> back to single pane 1
    CHECK(t.paneCount() == 1);
    CHECK(t.isLeaf(t.root()));
    CHECK(t.node(t.root()).paneID == 1);
    CHECK(t.focusedPaneID() == 1);
    CHECK(leafCount(t) == 1);
}

// A deeper tree: closing an inner pane promotes its sibling and keeps
// the rest of the layout intact, with no node leak.
TEST(testClosePromotesSibling)
{
    // Build: split V -> [1 | 2], focus 2, split H -> [1 | (2 / 3)].
    Tree t(1);
    t.split(Orientation::Vertical, 2);
    t.split(Orientation::Horizontal, 3); // focus now 3, under a H split
    CHECK(t.paneCount() == 3);

    // Close pane 3: its sibling (pane 2 leaf) replaces the H split, so
    // the root V split now has children {1, 2}.
    CHECK(t.closePane(3));
    CHECK(t.paneCount() == 2);
    const auto& rn = t.node(t.root());
    CHECK(rn.orientation == Orientation::Vertical);
    CHECK(t.node(rn.first).paneID == 1);
    CHECK(t.node(rn.second).paneID == 2);
    CHECK(leafCount(t) == 2);
    // Focus moved off the closed pane onto a live leaf.
    CHECK(t.focusedPaneID() == 2);
}

// Closing a pane that isn't focused leaves focus alone.
TEST(testCloseOtherKeepsFocus)
{
    Tree t(1);
    t.split(Orientation::Vertical, 2); // focus 2
    t.focusPane(1); // refocus 1
    CHECK(t.focusedPaneID() == 1);
    CHECK(t.closePane(2)); // close the unfocused pane
    CHECK(t.focusedPaneID() == 1);
    CHECK(t.paneCount() == 1);
}

// Directional focus across a vertical split: Right from left pane lands
// on the right pane and Left returns.
TEST(testMoveFocusVertical)
{
    Tree t(1);
    t.split(Orientation::Vertical, 2); // [1 | 2*], focus 2
    t.focusPane(1);
    CHECK(t.moveFocus(Direction::Right));
    CHECK(t.focusedPaneID() == 2);
    CHECK(t.moveFocus(Direction::Left));
    CHECK(t.focusedPaneID() == 1);
    // Off-axis moves do nothing in a pure vertical split.
    CHECK(!t.moveFocus(Direction::Up));
    CHECK(!t.moveFocus(Direction::Down));
    // Past the right edge: no move.
    t.focusPane(2);
    CHECK(!t.moveFocus(Direction::Right));
}

// Directional focus across a horizontal split: Down from top, Up from
// bottom.
TEST(testMoveFocusHorizontal)
{
    Tree t(1);
    t.split(Orientation::Horizontal, 2); // [1 / 2*]
    t.focusPane(1);
    CHECK(t.moveFocus(Direction::Down));
    CHECK(t.focusedPaneID() == 2);
    CHECK(t.moveFocus(Direction::Up));
    CHECK(t.focusedPaneID() == 1);
    CHECK(!t.moveFocus(Direction::Left));
}

// Nested grid: V split [A | (B / C)] — moving Right from A enters the
// right column at its nearest (top) leaf B; from B, Down reaches C; from
// C, Left returns to A (A is the whole-height left neighbor).
TEST(testMoveFocusNested)
{
    Tree t(10); // A = 10
    t.split(Orientation::Vertical, 20); // [10 | 20*], focus 20
    t.split(Orientation::Horizontal, 30); // right column: (20 / 30*)
    // Layout: [10 | (20 top / 30 bottom)].

    t.focusPane(10);
    CHECK(t.moveFocus(Direction::Right));
    CHECK(t.focusedPaneID() == 20); // enters right column from the top

    CHECK(t.moveFocus(Direction::Down));
    CHECK(t.focusedPaneID() == 30);

    CHECK(t.moveFocus(Direction::Left));
    CHECK(t.focusedPaneID() == 10); // A spans the height, so Left -> A

    // From the top-right (20) Left also reaches A.
    t.focusPane(20);
    CHECK(t.moveFocus(Direction::Left));
    CHECK(t.focusedPaneID() == 10);
}

// Geometric directional focus: moving into a subtree split on the perpendicular
// axis must land on the leaf whose cross-axis span contains the origin pane's
// midpoint — not the first (top/left) leaf regardless of position.
TEST(testMoveFocusGeometry)
{
    // Full grid: [ (1 top / 3 bottom) | (2 top / 4 bottom) ], all even.
    Tree t(1);
    t.split(Orientation::Vertical, 2); // [1 | 2*]
    t.focusPane(1);
    t.split(Orientation::Horizontal, 3); // left col: (1 / 3*)
    t.focusPane(2);
    t.split(Orientation::Horizontal, 4); // right col: (2 / 4*)

    // Row is preserved crossing the vertical split, in both directions.
    t.focusPane(3); // bottom-left
    CHECK(t.moveFocus(Direction::Right));
    CHECK(t.focusedPaneID() == 4); // bottom-right, not 2 (the top-left-bias bug)
    t.focusPane(1); // top-left
    CHECK(t.moveFocus(Direction::Right));
    CHECK(t.focusedPaneID() == 2); // top-right
    t.focusPane(4); // bottom-right
    CHECK(t.moveFocus(Direction::Left));
    CHECK(t.focusedPaneID() == 3); // back to bottom-left
    t.focusPane(2); // top-right
    CHECK(t.moveFocus(Direction::Left));
    CHECK(t.focusedPaneID() == 1); // back to top-left

    // Asymmetric ratios: a small top / big bottom left column, even right column.
    // The origin's true span (from accumulated ratios), not tree position, decides.
    Tree u(1);
    u.split(Orientation::Vertical, 2);
    u.focusPane(1);
    u.split(Orientation::Horizontal, 3, 0.3f); // left: 1 top (0.30) / 3 bottom (0.70)
    u.focusPane(2);
    u.split(Orientation::Horizontal, 4); // right: 2 top / 4 bottom (0.50)
    u.focusPane(3); // bottom-left, spans vertical [0.30,1.0], midpoint 0.65
    CHECK(u.moveFocus(Direction::Right));
    CHECK(u.focusedPaneID() == 4); // 0.65 is below the right divider -> bottom-right
    u.focusPane(1); // top-left, spans [0,0.30], midpoint 0.15
    CHECK(u.moveFocus(Direction::Right));
    CHECK(u.focusedPaneID() == 2); // 0.15 is above the divider -> top-right

    // Tie-break: a full-height source (midpoint exactly on the divider) keeps the
    // prior default of landing on the first (top) child.
    Tree v(1);
    v.split(Orientation::Vertical, 2);
    v.focusPane(2);
    v.split(Orientation::Horizontal, 3); // right col: (2 top / 3 bottom)
    v.focusPane(1); // full-height left neighbor
    CHECK(v.moveFocus(Direction::Right));
    CHECK(v.focusedPaneID() == 2); // tie -> top

    // The same rule on the horizontal axis: column position survives Up/Down.
    Tree w(1);
    w.split(Orientation::Horizontal, 2); // [1 / 2*]
    w.focusPane(1);
    w.split(Orientation::Vertical, 3); // top row: (1 L | 3 R)
    w.focusPane(2);
    w.split(Orientation::Vertical, 4); // bottom row: (2 L | 4 R)
    w.focusPane(4); // bottom-right
    CHECK(w.moveFocus(Direction::Up));
    CHECK(w.focusedPaneID() == 3); // top-right, not 1 (top-left)
    w.focusPane(2); // bottom-left
    CHECK(w.moveFocus(Direction::Up));
    CHECK(w.focusedPaneID() == 1); // top-left
}

// Pool exhaustion is reported, not crashed, and a refused split leaves
// the tree consistent.
TEST(testCapacity)
{
    Tree t(0);
    int made = 1;
    for (int id = 1; id < Ubiquitty::Panes::maxPanes + 5; ++id) {
        int nl = t.split(Orientation::Vertical, id);
        if (nl != invalidNode)
            ++made;
    }
    CHECK(made == Ubiquitty::Panes::maxPanes);
    CHECK(t.paneCount() == Ubiquitty::Panes::maxPanes);
    CHECK(leafCount(t) == Ubiquitty::Panes::maxPanes);
    // Past capacity, split is refused.
    CHECK(t.split(Orientation::Vertical, 9999) == invalidNode);
}

// Repeated split/close cycles must not leak pool nodes (the divider
// node and leaf are both freed on collapse).
TEST(testNoNodeLeak)
{
    Tree t(1);
    for (int i = 0; i < 200; ++i) {
        int id = 1000 + i;
        CHECK(t.split(Orientation::Vertical, id) != invalidNode);
        CHECK(t.closeFocused());
    }
    CHECK(t.paneCount() == 1);
    CHECK(leafCount(t) == 1);
    // The single remaining pane is still pane 1, and the pool is clean
    // enough to split again.
    CHECK(t.split(Orientation::Horizontal, 42) != invalidNode);
    CHECK(t.paneCount() == 2);
}

// forEachLeaf visits every live leaf L→R (first-before-second), and only
// leaves — internal splits are skipped.
TEST(testForEachLeafOrder)
{
    // Build [10 | (20 top / 30 bottom)]: a left leaf and a right column.
    Tree t(10);
    t.split(Orientation::Vertical, 20); // [10 | 20*]
    t.split(Orientation::Horizontal, 30); // right column: (20 / 30*)

    std::vector<int> seen;
    Ubiquitty::Panes::forEachLeaf(t, [&](int paneID) { seen.push_back(paneID); });
    // L→R: the left leaf first, then the right column top-to-bottom.
    CHECK(seen.size() == 3);
    CHECK(seen.size() == 3 && seen[0] == 10 && seen[1] == 20 && seen[2] == 30);

    // The subtree overload starting at the right column visits only {20,30}.
    const auto& root = t.node(t.root());
    std::vector<int> rightOnly;
    Ubiquitty::Panes::forEachLeaf(t, root.second, [&](int paneID) { rightOnly.push_back(paneID); });
    CHECK(rightOnly.size() == 2);
    CHECK(rightOnly.size() == 2 && rightOnly[0] == 20 && rightOnly[1] == 30);

    // A single-pane tree visits exactly its one leaf.
    Tree solo(99);
    int count = 0, last = -1;
    Ubiquitty::Panes::forEachLeaf(solo, [&](int paneID) {
        ++count;
        last = paneID;
    });
    CHECK(count == 1 && last == 99);

    // An out-of-range / invalid start node visits nothing.
    int touched = 0;
    Ubiquitty::Panes::forEachLeaf(t, invalidNode, [&](int) { ++touched; });
    CHECK(touched == 0);
}

// sameShape: equal leaf ids + positions + orientations → true (ratios ignored);
// any structural difference → false.
TEST(testSameShape)
{
    // Two independently built trees with identical structure but different
    // ratios and different internal node numbering.
    Tree a(1);
    a.split(Orientation::Vertical, 2); // [1 | 2*]
    a.split(Orientation::Horizontal, 3); // [1 | (2 / 3*)]
    a.setRatio(a.root(), 0.3f);

    Tree b(1);
    b.split(Orientation::Vertical, 2);
    b.split(Orientation::Horizontal, 3);
    b.setRatio(b.root(), 0.7f); // different ratio — still same SHAPE

    CHECK(Ubiquitty::Panes::sameShape(a, b));
    CHECK(Ubiquitty::Panes::sameShape(a, a.root(), b, b.root()));

    // Different leaf id at a matching position → not the same shape.
    Tree c(1);
    c.split(Orientation::Vertical, 2);
    c.split(Orientation::Horizontal, 99); // 99 where b had 3
    CHECK(!Ubiquitty::Panes::sameShape(a, c));

    // Different split orientation → not the same shape.
    Tree d(1);
    d.split(Orientation::Horizontal, 2); // root H where a's root is V
    d.split(Orientation::Horizontal, 3);
    CHECK(!Ubiquitty::Panes::sameShape(a, d));

    // Different leaf COUNT (a 2-pane vs a 3-pane) → not the same shape.
    Tree e(1);
    e.split(Orientation::Vertical, 2); // just [1 | 2]
    CHECK(!Ubiquitty::Panes::sameShape(a, e));

    // A tree is the same shape as itself.
    CHECK(Ubiquitty::Panes::sameShape(a, a));
}

// ratioFromPosition / positionFromRatio round-trip, with and without a divider
// thickness, and clamp degenerate extents instead of dividing by zero.
TEST(testRatioPositionRoundtrip)
{
    using Ubiquitty::Panes::positionFromRatio;
    using Ubiquitty::Panes::ratioFromPosition;

    // No divider thickness (the GTK convention): ratio = position / extent.
    {
        const float extent = 800.0f;
        const float ratio = 0.25f;
        const float pos = positionFromRatio(ratio, extent, 0.0f);
        CHECK(pos > 199.5f && pos < 200.5f); // 0.25 * 800 = 200
        const float back = ratioFromPosition(pos, extent, 0.0f);
        CHECK(back > 0.249f && back < 0.251f);
    }

    // With a divider thickness (the NSSplitView convention): the usable extent
    // is (extent - thickness), so a half ratio yields two equal children.
    {
        const float extent = 410.0f;
        const float thickness = 10.0f; // usable 400
        const float pos = positionFromRatio(0.5f, extent, thickness);
        CHECK(pos > 199.5f && pos < 200.5f); // 0.5 * 400 = 200
        const float back = ratioFromPosition(pos, extent, thickness);
        CHECK(back > 0.499f && back < 0.501f);
    }

    // Round-trip a sweep of ratios through both maps (thickness present).
    {
        const float extent = 1234.0f;
        const float thickness = 6.0f;
        for (int i = 1; i < 20; ++i) {
            const float r = static_cast<float>(i) / 20.0f;
            const float back = ratioFromPosition(positionFromRatio(r, extent, thickness), extent, thickness);
            CHECK(back > r - 0.001f && back < r + 0.001f);
        }
    }

    // Degenerate extent (thickness ≥ extent) clamps the usable span to ≥1 so
    // the maps never divide by zero or go negative.
    {
        const float pos = positionFromRatio(0.5f, 4.0f, 10.0f);
        CHECK(pos >= 0.0f); // usable clamped to 1 → 0.5
        const float r = ratioFromPosition(50.0f, 4.0f, 10.0f);
        CHECK(r == 50.0f); // 50 / 1
    }
}

// insertPane: the target stays in place, the new leaf takes the chosen
// side and the focus; split() is exactly insertPane at the focused leaf
// with the new pane second.
TEST(testInsertPaneSides)
{
    Tree t(1);
    // New pane FIRST: [2|1], both leaves under a fresh vertical split.
    int newLeafFirst = t.insertPane(2, 1, Orientation::Vertical, 0.5f, InsertSide::First);
    CHECK(newLeafFirst != invalidNode);
    CHECK(t.paneCount() == 2);
    CHECK(t.focusedPaneID() == 2);
    const auto& root = t.node(t.root());
    CHECK(t.node(root.first).paneID == 2);
    CHECK(t.node(root.second).paneID == 1);

    // New pane SECOND next to a NON-focused target (1): [2|[1|3]].
    int newLeafSecond = t.insertPane(3, 1, Orientation::Vertical, 0.5f, InsertSide::Second);
    CHECK(newLeafSecond != invalidNode);
    CHECK(t.focusedPaneID() == 3);
    const auto& rootAfter = t.node(t.root());
    const auto& inner = t.node(rootAfter.second);
    CHECK(t.node(inner.first).paneID == 1);
    CHECK(t.node(inner.second).paneID == 3);

    // Guards: duplicate id, dead target, noPane id.
    CHECK(t.insertPane(3, 1, Orientation::Vertical, 0.5f, InsertSide::Second) == invalidNode);
    CHECK(t.insertPane(9, 42, Orientation::Vertical, 0.5f, InsertSide::Second) == invalidNode);
    CHECK(t.insertPane(noPane, 1, Orientation::Vertical, 0.5f, InsertSide::Second) == invalidNode);

    // split == insertPane(focused, InsertSide::Second): same shape.
    Tree bySplit(1);
    bySplit.split(Orientation::Horizontal, 2, 0.3f);
    Tree byInsert(1);
    byInsert.insertPane(2, 1, Orientation::Horizontal, 0.3f, InsertSide::Second);
    CHECK(sameShape(bySplit, byInsert));
    CHECK(bySplit.focusedPaneID() == byInsert.focusedPaneID());
}

// swapPanes trades the two leaves' pane ids: shape and ratios stay put,
// focus follows the pane.
TEST(testSwapPanes)
{
    Tree t(1);
    t.split(Orientation::Vertical, 2, 0.7f);
    t.split(Orientation::Horizontal, 3);
    // Leaves L→R: 1, then (2 over 3). Focus is 3.
    CHECK(t.swapPanes(1, 3));
    std::vector<int> order;
    forEachLeaf(t, [&](int paneID) { order.push_back(paneID); });
    CHECK(order.size() == 3 && order[0] == 3 && order[1] == 2 && order[2] == 1);
    // Focus followed pane 3 to its new position.
    CHECK(t.focusedPaneID() == 3);
    // Ratios untouched.
    CHECK(t.node(t.root()).ratio > 0.699f && t.node(t.root()).ratio < 0.701f);
    // Guards.
    CHECK(!t.swapPanes(2, 2));
    CHECK(!t.swapPanes(2, 42));
}

// movePane detaches a pane and re-inserts it next to the target; focus
// stays with the pane that had it.
TEST(testMovePane)
{
    // [1|2] → move 1 BELOW 2: [2/1] (horizontal, moved pane second).
    Tree t(1);
    t.split(Orientation::Vertical, 2);
    t.focusPane(2);
    CHECK(t.movePane(1, 2, Orientation::Horizontal, InsertSide::Second));
    CHECK(t.paneCount() == 2);
    const auto& root = t.node(t.root());
    CHECK(root.orientation == Orientation::Horizontal);
    CHECK(t.node(root.first).paneID == 2);
    CHECK(t.node(root.second).paneID == 1);
    CHECK(t.focusedPaneID() == 2); // unmoved focus stayed put

    // Moved pane keeps focus when it was focused.
    t.focusPane(1);
    CHECK(t.movePane(1, 2, Orientation::Vertical, InsertSide::First));
    CHECK(t.focusedPaneID() == 1);
    CHECK(t.node(t.node(t.root()).first).paneID == 1);

    // Deep move: 4 panes, drag 1 next to 4.
    Tree deep(1);
    deep.split(Orientation::Vertical, 2);
    deep.split(Orientation::Horizontal, 3);
    deep.split(Orientation::Vertical, 4);
    CHECK(deep.movePane(1, 4, Orientation::Horizontal, InsertSide::Second));
    CHECK(deep.paneCount() == 4);
    std::vector<int> order;
    forEachLeaf(deep, [&](int paneID) { order.push_back(paneID); });
    CHECK(order.size() == 4);
    // 1 left its slot and sits directly after 4 (as its split-sibling).
    bool isAdjacent = false;
    for (size_t i = 0; i + 1 < order.size(); ++i)
        if (order[i] == 4 && order[i + 1] == 1)
            isAdjacent = true;
    CHECK(isAdjacent);

    // Guards: self-move, dead ids, single pane.
    CHECK(!deep.movePane(2, 2, Orientation::Vertical, InsertSide::Second));
    CHECK(!deep.movePane(42, 1, Orientation::Vertical, InsertSide::Second));
    CHECK(!deep.movePane(1, 42, Orientation::Vertical, InsertSide::Second));
    Tree lone(9);
    CHECK(!lone.movePane(9, 9, Orientation::Vertical, InsertSide::Second));
}

// movePane is pool-safe at the pane cap: the detach frees two nodes
// before the insert takes two, so a full tree can still rearrange.
TEST(testMovePaneAtCapacity)
{
    Tree t(1);
    int next = 2;
    while (t.paneCount() < Ubiquitty::Panes::maxPanes)
        CHECK(t.split(Orientation::Vertical, next++) != invalidNode);
    CHECK(t.paneCount() == Ubiquitty::Panes::maxPanes);
    CHECK(t.movePane(1, 2, Orientation::Horizontal, InsertSide::Second));
    CHECK(t.paneCount() == Ubiquitty::Panes::maxPanes);
    CHECK(leafCount(t) == Ubiquitty::Panes::maxPanes);
    // The moved-pane-first side is pool-safe too.
    CHECK(t.movePane(1, 2, Orientation::Vertical, InsertSide::First));
    CHECK(t.paneCount() == Ubiquitty::Panes::maxPanes);
    CHECK(leafCount(t) == Ubiquitty::Panes::maxPanes);
}

// balance equalizes every split's leaf share: a 3-pane vertical chain
// becomes exact thirds (root 1/3, inner 1/2).
TEST(testBalance)
{
    Tree t(1);
    t.split(Orientation::Vertical, 2);
    t.split(Orientation::Vertical, 3);
    CHECK(t.balance());
    const auto& root = t.node(t.root());
    CHECK(root.ratio > 0.332f && root.ratio < 0.334f);
    const auto& inner = t.node(root.second);
    CHECK(inner.ratio > 0.499f && inner.ratio < 0.501f);
    // Dead node refused.
    CHECK(!t.balance(invalidNode));
}

// balance on a FIRST-child-heavy shape: the first child's fraction is its
// LEAF SHARE (2/3 here), not a constant 1/leaves — and balancing a
// subtree leaves the rest of the tree untouched.
TEST(testBalanceFirstChildHeavyAndSubtree)
{
    // [[1|3]|2]: split 1→2, refocus 1, split 1→3.
    Tree t(1);
    t.split(Orientation::Vertical, 2);
    t.focusPane(1);
    t.split(Orientation::Vertical, 3);
    CHECK(t.balance());
    const auto& root = t.node(t.root());
    CHECK(root.ratio > 0.665f && root.ratio < 0.668f); // 2 of 3 leaves left of the divider
    const auto& inner = t.node(root.first);
    CHECK(inner.ratio > 0.499f && inner.ratio < 0.501f);

    // Subtree balance: skew both splits, balance only the inner one — the
    // root's skew must survive.
    CHECK(t.setRatio(t.root(), 0.9f));
    CHECK(t.setRatio(root.first, 0.8f));
    CHECK(t.balance(root.first));
    CHECK(t.node(root.first).ratio > 0.499f && t.node(root.first).ratio < 0.501f);
    CHECK(t.node(t.root()).ratio > 0.899f && t.node(t.root()).ratio < 0.901f);
}

// adjustRatio nudges the divider on the pane's side, with the tmux
// fallback when the pane has no divider on that side.
TEST(testAdjustRatio)
{
    Tree t(1);
    t.split(Orientation::Vertical, 2); // [1|2] at 0.5
    const int root = t.root();

    // Pane 1 grows right: its own right divider moves right.
    CHECK(t.adjustRatio(1, Direction::Right, 0.1f));
    CHECK(t.node(root).ratio > 0.599f && t.node(root).ratio < 0.601f);
    // Pane 1 "resize left" has no left divider → fallback moves the same
    // divider left (the pane shrinks).
    CHECK(t.adjustRatio(1, Direction::Left, 0.1f));
    CHECK(t.node(root).ratio > 0.499f && t.node(root).ratio < 0.501f);
    // Pane 2 "resize right" has no right divider → fallback: divider moves
    // right (pane 2 shrinks).
    CHECK(t.adjustRatio(2, Direction::Right, 0.1f));
    CHECK(t.node(root).ratio > 0.599f && t.node(root).ratio < 0.601f);

    // Wrong axis entirely: nothing to adjust.
    CHECK(!t.adjustRatio(1, Direction::Down, 0.1f));
    // Pinned at the clamp band: no change → false.
    CHECK(t.setRatio(root, 0.95f));
    CHECK(!t.adjustRatio(1, Direction::Right, 0.1f));
    // Bad step / dead pane.
    CHECK(!t.adjustRatio(1, Direction::Right, 0.0f));
    CHECK(!t.adjustRatio(42, Direction::Right, 0.1f));
}

// adjustRatio picks the RIGHT divider on nested same-axis splits (the
// bordering preference, not merely the nearest matching-axis ancestor)
// and skips cross-axis splits on the walk up.
TEST(testAdjustRatioDividerIdentity)
{
    // [1|[2|3]]: root vertical, inner vertical.
    Tree t(1);
    t.split(Orientation::Vertical, 2);
    t.split(Orientation::Vertical, 3);
    const int root = t.root();
    const int inner = t.node(root).second;

    // Pane 2 grows LEFT: its left edge is the ROOT divider (pane 2 sits in
    // the root's second subtree) — a nearest-ancestor-only walk would move
    // the INNER divider instead.
    CHECK(t.adjustRatio(2, Direction::Left, 0.1f));
    CHECK(t.node(root).ratio > 0.399f && t.node(root).ratio < 0.401f);
    CHECK(t.node(inner).ratio > 0.499f && t.node(inner).ratio < 0.501f); // untouched

    // Pane 3 "grows right" has no right divider: the fallback is the
    // NEAREST matching-axis ancestor (inner), whose divider moves right.
    CHECK(t.adjustRatio(3, Direction::Right, 0.1f));
    CHECK(t.node(inner).ratio > 0.599f && t.node(inner).ratio < 0.601f);
    CHECK(t.node(root).ratio > 0.399f && t.node(root).ratio < 0.401f); // untouched

    // Cross-axis interleave [[1/2]|3]: pane 2's right edge is the VERTICAL
    // root; the horizontal parent between must be skipped.
    Tree mixed(1);
    mixed.split(Orientation::Vertical, 3);
    mixed.focusPane(1);
    mixed.split(Orientation::Horizontal, 2);
    const int mixedRoot = mixed.root();
    const int horizontal = mixed.node(mixedRoot).first;
    CHECK(mixed.adjustRatio(2, Direction::Right, 0.1f));
    CHECK(mixed.node(mixedRoot).ratio > 0.599f && mixed.node(mixedRoot).ratio < 0.601f);
    CHECK(mixed.node(horizontal).ratio > 0.499f && mixed.node(horizontal).ratio < 0.501f); // untouched
}

UBIQUITTY_TEST_MAIN()
