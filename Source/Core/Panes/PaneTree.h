// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// PaneTree.h — portable split/pane layout model. A binary tree whose leaves are panes (each carrying an
// opaque frontend id) and whose internal nodes are splits with an orientation + ratio. Pure data + tree
// ops; no platform types, STL, or allocation (a fixed-capacity index pool). The frontend mirrors this
// tree to build its view hierarchy and route input; all layout policy lives here.

#include "Base/Types.h"
#include "Core/Panes/PaneTreeNode.h"

#include <array>

namespace Ubiquitty::Panes {

/// The split layout of one window: a binary tree whose leaves are panes.
///
/// Pure data — a pane is an opaque frontend id, and nothing here knows what a
/// pane draws — so the same tree serves macOS, Linux and a tmux session.
class Tree {
public:
    /// Exact live-node pool and allocation holes of one split layout.
    /// Dead slots are not read or serialized; their former bytes are irrelevant.
    struct Continuation {
        std::array<Node, maxNodes> nodes {};
        std::array<bool, maxNodes> used {};
        int root = invalidNode;
        int focused = invalidNode;
        int paneCount = 0;
        int nodeCount = 0;
    };

    /// Copies the live pool without compacting indices or changing focus.
    /// - Returns: Independently owned fixed-capacity state.
    [[nodiscard]] Continuation continuation() const;

    /// Replaces a tree only after its complete graph and counters validate.
    /// - Parameter state: Exact node pool with a reachable, unique focused leaf.
    /// - Returns: Whether all live nodes form one bounded binary tree.
    [[nodiscard]] bool restoreContinuation(const Continuation& state);

    // Construct a single-leaf tree whose root pane carries `rootPaneID`.
    // That leaf is focused. A just-constructed Tree always has exactly
    // one pane and behaves like an un-split window.
    explicit Tree(int rootPaneID);

    // Default: a single-pane tree rooted at pane id 0. Lets a fixed-pool
    // container (the Layout model's Tab array) default-construct its slots;
    // a real tab re-roots via assignment with an allocated pane id.
    Tree()
        : Tree(0)
    {
    }

    /// Splits the focused leaf in two, giving the new second child a pane.
    ///
    /// The existing pane stays as the first child and the new one becomes
    /// focused.
    ///
    /// - Parameters:
    ///   - orientation: Which way the split divides.
    ///   - newPaneID: The new pane's id; must not duplicate a live one.
    ///   - ratio: The first child's fraction, clamped to an interior band.
    /// - Returns: The new leaf's node index, or `invalidNode` when the pool is
    ///   full or the id duplicates a live pane.
    int split(Orientation, int newPaneID, float ratio = defaultRatio);

    /// Closes the focused leaf and collapses its parent split.
    ///
    /// The sibling subtree replaces the parent, so the tree loses one level,
    /// and focus moves to the nearest leaf of that promoted subtree.
    ///
    /// - Returns: `false` when only one pane remains — a window always keeps at
    ///   least one, so this is a no-op rather than an error.
    bool closeFocused();

    /// Closes any leaf by pane id, collapsing its parent split.
    ///
    /// - Parameter paneID: The pane to close.
    /// - Returns: `false` when the id is not a live leaf, or it is the last
    ///   pane.
    bool closePane(int paneID);

    /// Moves focus to the spatially nearest leaf in one direction.
    ///
    /// Walks up to the first ancestor split whose orientation matches the axis
    /// and whose focused side can move that way, then descends the sibling to
    /// the leaf nearest the shared edge — so focus lands where the eye expects
    /// rather than where traversal order would put it.
    ///
    /// - Parameter direction: Which way to move.
    /// - Returns: `true` when focus changed.
    bool moveFocus(Direction);

    /// The focused leaf's node index, always valid.
    /// - Returns: A live leaf index.
    [[nodiscard]] int focusedNode() const { return m_focused; }

    /// The focused leaf's pane id.
    /// - Returns: The pane the user is typing into.
    [[nodiscard]] int focusedPaneID() const { return m_nodes[m_focused].paneID; }

    /// Focuses the leaf carrying a pane id directly.
    ///
    /// - Parameter paneID: The pane to focus.
    /// - Returns: `false` when no live leaf has that id.
    bool focusPane(int paneID);

    /// The live pane count, which equals the leaf count.
    /// - Returns: How many panes the window holds.
    [[nodiscard]] int paneCount() const { return m_paneCount; }

    /// The root node's index.
    /// - Returns: A live index, or `invalidNode` only on a moved-from tree.
    [[nodiscard]] int root() const { return m_root; }

    /// Reads one node.
    /// - Parameter index: A live node index.
    /// - Returns: The node.
    [[nodiscard]] Node node(int index) const { return m_nodes[index]; }

    /// Whether an index names a live leaf, which is to say a pane.
    /// - Parameter index: The node to test.
    /// - Returns: `true` for a live leaf.
    [[nodiscard]] bool isLeaf(int index) const;

    /// Sets an internal split's ratio.
    ///
    /// - Parameters:
    ///   - index: The split to adjust.
    ///   - ratio: The first child's fraction, clamped to an interior band.
    /// - Returns: `false` when the index is not a live split.
    bool setRatio(int index, float ratio);

    /// Inserts a new pane beside a live leaf or the complete layout.
    ///
    /// The target leaf stays in place under a fresh split, the new leaf takes
    /// `side`, and the new pane becomes focused. ``split`` is this with the
    /// focused leaf as the target and `InsertSide::Second`.
    ///
    /// - Parameters:
    ///   - newPaneID: The new pane's id; must not duplicate a live one.
    ///   - targetPaneID: The leaf to split.
    ///   - orientation: Which way the fresh split divides.
    ///   - ratio: The first child's fraction.
    ///   - side: Which side the new leaf takes.
    ///   - insertTarget: Whether to split the named pane or wrap the root.
    /// - Returns: The new leaf's node index, or `invalidNode` when the pool is
    ///   full, either id is invalid, or the target is not a live leaf.
    int insertPane(int newPaneID, int targetPaneID, Orientation, float ratio, InsertSide side, InsertTarget = InsertTarget::Pane);

    /// Inserts a complete layout beside a leaf or root, preserving both subtrees.
    ///
    /// Refuses duplicate pane identities or insufficient capacity without changing either tree.
    ///
    /// - Parameters:
    ///   - source: The layout to copy into this tree.
    ///   - targetPaneID: The live leaf beside which to insert the layout.
    ///   - orientation: The direction of the enclosing split.
    ///   - side: The side of the target occupied by the inserted layout.
    ///   - insertTarget: Whether to split the named pane or wrap the root.
    /// - Returns: Whether the target exists and the disjoint layout fits.
    bool insertTree(const Tree&, int targetPaneID, Orientation, InsertSide, InsertTarget = InsertTarget::Pane);

    /// Moves a live pane next to another.
    ///
    /// Detaches the pane — collapsing its old split one level, exactly as
    /// ``closePane`` does — and re-inserts it beside the target under a fresh
    /// split at the default ratio.
    ///
    /// Keyboard focus follows the PANE rather than the position: a moved pane
    /// that was focused stays focused, and an unmoved focused pane keeps it.
    /// Safe at the pane cap, because the detach frees two nodes before the
    /// insert takes two.
    ///
    /// - Parameters:
    ///   - paneID: The pane to move.
    ///   - targetPaneID: The pane to move it beside.
    ///   - orientation: Which way the fresh split divides.
    ///   - side: Which side the moved pane takes.
    ///   - insertTarget: Whether to split the named pane or wrap the remaining root.
    /// - Returns: `false` when either id is not a live leaf, the ids are equal,
    ///   or only one pane remains.
    bool movePane(int paneID, int targetPaneID, Orientation, InsertSide side, InsertTarget = InsertTarget::Pane);

    /// Swaps the POSITIONS of two live panes.
    ///
    /// Their leaf nodes trade pane ids; the tree shape and every ratio stay
    /// put, and focus follows the pane rather than the position.
    ///
    /// - Parameters:
    ///   - firstPaneID: One pane.
    ///   - secondPaneID: The other.
    /// - Returns: `false` when either id is not a live leaf, or they are equal.
    bool swapPanes(int firstPaneID, int secondPaneID);

    /// Equalises a subtree so every descendant leaf gets an equal share.
    ///
    /// Each split's first-child fraction becomes its leaf count over the
    /// subtree's, clamped to the interior band — which is why equalising a
    /// lopsided tree does not set every ratio to one half.
    ///
    /// - Parameter nodeIndex: The subtree root.
    /// - Returns: `false` when the index is not a live node.
    bool balance(int nodeIndex);

    /// Equalises the whole tree from its root.
    /// - Returns: `false` only on a moved-from tree.
    bool balance() { return balance(m_root); }

    /// Nudges the divider on one side of a pane, for keyboard resize.
    ///
    /// Prefers the nearest ancestor split whose divider actually borders the
    /// pane on that side, and falls back to the nearest same-axis ancestor —
    /// so a key press moves the edge the user is looking at rather than the
    /// first one up the tree. Right and Down raise the ratio; Left and Up lower
    /// it.
    ///
    /// - Parameters:
    ///   - paneID: The pane whose edge moves.
    ///   - direction: Which of its sides.
    ///   - step: A positive fraction of the split's extent.
    /// - Returns: `true` only when a ratio actually changed.
    bool adjustRatio(int paneID, Direction, float step);

    /// Answers whether a pane is a live leaf of this tree.
    ///
    /// - Parameter paneID: The pane to look for.
    /// - Returns: True when a live leaf carries it.
    [[nodiscard]] bool holdsPane(int paneID) const { return leafForPaneID(paneID) != invalidNode; }

private:
    /// Takes a node from the fixed pool, or invalidNode when it is full.
    /// - Returns: A free node index, or `invalidNode`.
    int allocNode();
    /// Returns a node to the pool; the index must be live.
    /// - Parameter index: A live node index.
    void freeNode(int index);
    /// The live leaf node carrying `paneID`, or invalidNode.
    /// - Parameter paneID: The pane to find.
    /// - Returns: Its leaf index, or `invalidNode`.
    [[nodiscard]] int leafForPaneID(int paneID) const;
    /// Live leaves in the subtree rooted at `index` (0 for a dead index).
    /// - Parameter index: The subtree root.
    /// - Returns: Its live leaf count, 0 for a dead index.
    [[nodiscard]] int subtreeLeafCount(int index) const;
    /// Re-spreads a subtree's split ratios so its leaves share the region evenly.
    /// - Parameter index: The subtree root.
    void balanceSubtree(int index);
    /// The first leaf of a subtree in traversal order.
    /// - Parameter index: The subtree root.
    /// - Returns: Its first leaf.
    [[nodiscard]] int leftmostLeaf(int index) const;
    /// The focused-side leaf's midpoint along the CROSS axis (perpendicular to
    /// `direction`), as a fraction [0,1] of `ancestor`'s region — derived from the
    /// split ratios on the path `leaf` → `ancestor`.
    ///
    /// - Parameters:
    ///   - ancestor: The split the fraction is measured within.
    ///   - leaf: The focused-side leaf.
    ///   - direction: The movement axis; the midpoint is on its cross axis.
    /// - Returns: A fraction in [0, 1].
    [[nodiscard]] float crossMidpointWithin(int ancestor, int leaf, Direction) const;
    /// Descend `subtree` to the leaf adjacent to the origin pane: toward the shared
    /// edge on the movement axis, and toward `crossMidpoint` on any cross-axis split.
    ///
    /// - Parameters:
    ///   - subtree: The sibling subtree to descend.
    ///   - direction: The movement axis.
    ///   - crossMidpoint: Where to aim on any cross-axis split.
    /// - Returns: The adjacent leaf.
    [[nodiscard]] int nearestLeafTowardEdge(int subtree, Direction, float crossMidpoint) const;
    /// Whether a live leaf carries this pane id.
    /// - Parameter paneID: The id to test.
    /// - Returns: `true` when a live leaf carries it.
    [[nodiscard]] bool paneIDLive(int paneID) const;

    Node m_nodes[maxNodes];
    bool m_used[maxNodes];
    int m_root;
    int m_focused;
    int m_paneCount;
    int m_nodeCount;
};

// Portable tree helpers (no platform types), shared by both frontends so they don't drift.

// Visit every live leaf of the subtree rooted at `nodeIndex` in left-to-right (first-before-second) order,
// calling `fn(paneID)`. Internal splits are not visited. Safe on invalidNode / out-of-range (visits nothing).
template<typename Fn> void forEachLeaf(const Tree& tree, int nodeIndex, Fn fn)
{
    if (nodeIndex < 0 || nodeIndex >= maxNodes)
        return;
    if (tree.isLeaf(nodeIndex)) {
        fn(tree.node(nodeIndex).paneID);
        return;
    }
    const Node& n = tree.node(nodeIndex);
    forEachLeaf(tree, n.first, fn);
    forEachLeaf(tree, n.second, fn);
}

// Whole-tree convenience: visit every live leaf L→R from the root.
template<typename Fn> void forEachLeaf(const Tree& tree, Fn fn) { forEachLeaf(tree, tree.root(), fn); }

/// True when the subtrees at `a[ia]` and `b[ib]` have the same shape: same leaf pane ids in the same
/// positions and same split orientations, IGNORING ratios. Lets a frontend tell a divider move (shape
/// unchanged → reposition views) from a restructure (shape changed → rebuild the hierarchy).
///
/// - Parameters:
///   - a: One tree.
///   - ia: A node in `a`.
///   - b: The other tree.
///   - ib: The node in `b` to compare against.
/// - Returns: `true` when the two subtrees share a shape.
bool sameShape(const Tree& a, int ia, const Tree& b, int ib);

/// Compares two whole trees from their roots.
///
/// - Parameters:
///   - a: One tree.
///   - b: The other.
/// - Returns: `true` when they share a shape, ignoring ratios.
inline bool sameShape(const Tree& a, const Tree& b) { return sameShape(a, a.root(), b, b.root()); }

/// The axis extent panes may occupy, once the divider is taken out.
///
/// Subtracting the divider FIRST is what makes ratio 0.5 place two equal
/// children and a dragged ratio round-trip exactly; mapping against the full
/// extent leaves both off by half a divider. The result is clamped positive so
/// the maps never divide by zero on a degenerate split.
///
/// - Parameters:
///   - axisExtent: The split's full extent along its orientation.
///   - dividerThickness: The divider drawn between the two children.
/// - Returns: The extent the two children share.
inline float paneUsableExtent(float axisExtent, float dividerThickness)
{
    const float usable = axisExtent - dividerThickness;
    return usable > 1.0f ? usable : 1.0f;
}

/// Maps a divider's pixel position to the first child's ratio.
///
/// - Parameters:
///   - position: The divider's offset along the axis.
///   - axisExtent: The split's full extent.
///   - dividerThickness: The divider drawn between the children.
/// - Returns: The first child's fraction.
inline float ratioFromPosition(float position, float axisExtent, float dividerThickness) { return position / paneUsableExtent(axisExtent, dividerThickness); }

/// Maps the first child's ratio to a divider pixel position.
///
/// - Parameters:
///   - ratio: The first child's fraction.
///   - axisExtent: The split's full extent.
///   - dividerThickness: The divider drawn between the children.
/// - Returns: The divider's offset along the axis.
inline float positionFromRatio(float ratio, float axisExtent, float dividerThickness) { return ratio * paneUsableExtent(axisExtent, dividerThickness); }

} // namespace Ubiquitty::Panes
