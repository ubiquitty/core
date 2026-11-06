// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// PaneTree.cpp — see PaneTree.h. STL-free, allocation-free binary
// split tree over a fixed node pool.

#include "Core/Panes/PaneTree.h"

#include <cmath>

namespace Ubiquitty::Panes {

namespace {

// Interior clamp band for a split ratio: a divider never collapses a
// child to nothing, so both children always have a paintable extent.
constexpr float minRatio = 0.05f;
constexpr float maxRatio = 0.95f;

float clampRatio(float ratio)
{
    if (ratio < minRatio)
        return minRatio;
    if (ratio > maxRatio)
        return maxRatio;
    return ratio;
}

// The two axes. Vertical splits resolve Left/Right; Horizontal splits
// resolve Up/Down.
bool axisMatches(Orientation orientation, Direction direction)
{
    if (orientation == Orientation::Vertical)
        return direction == Direction::Left || direction == Direction::Right;
    return direction == Direction::Up || direction == Direction::Down;
}

// For a matching split, does `direction` point from the first child toward the
// second? Vertical: Right. Horizontal: Down.
bool towardSecond(Direction direction) { return direction == Direction::Right || direction == Direction::Down; }

} // namespace

Tree::Continuation Tree::continuation() const
{
    Continuation result;
    result.root = m_root;
    result.focused = m_focused;
    result.paneCount = m_paneCount;
    result.nodeCount = m_nodeCount;
    for (int index = 0; index < maxNodes; ++index) {
        result.used[index] = m_used[index];
        if (m_used[index])
            result.nodes[index] = m_nodes[index];
    }
    return result;
}

bool Tree::restoreContinuation(const Continuation& state)
{
    const auto isLive = [&](int index) { return index >= 0 && index < maxNodes && state.used[index]; };
    if (!isLive(state.root) || !isLive(state.focused) || state.paneCount < 1 || state.paneCount > maxPanes
        || state.nodeCount != state.paneCount * 2 - 1 || state.nodes[state.root].parent != invalidNode)
        return false;
    std::array<int, maxNodes> stack {};
    std::array<bool, maxNodes> visited {};
    int pending = 1;
    int nodes = 0;
    int leaves = 0;
    stack[0] = state.root;
    while (pending) {
        const auto index = stack[--pending];
        if (!isLive(index) || visited[index])
            return false;
        visited[index] = true;
        ++nodes;
        const auto& node = state.nodes[index];
        if (node.orientation > Orientation::Horizontal || !std::isfinite(node.ratio))
            return false;
        if (node.first == invalidNode) {
            if (node.second != invalidNode || node.paneID == noPane)
                return false;
            for (int previous = 0; previous < maxNodes; ++previous) {
                if (previous != index && visited[previous] && state.nodes[previous].first == invalidNode
                    && state.nodes[previous].paneID == node.paneID)
                    return false;
            }
            ++leaves;
        } else {
            if (!isLive(node.first) || !isLive(node.second) || node.first == node.second || node.paneID != noPane
                || node.ratio < minRatio || node.ratio > maxRatio || pending > maxNodes - 2
                || state.nodes[node.first].parent != index || state.nodes[node.second].parent != index)
                return false;
            stack[pending++] = node.second;
            stack[pending++] = node.first;
        }
    }
    if (nodes != state.nodeCount || leaves != state.paneCount || state.nodes[state.focused].first != invalidNode)
        return false;
    for (int index = 0; index < maxNodes; ++index) {
        if (visited[index] != state.used[index])
            return false;
    }
    for (int index = 0; index < maxNodes; ++index) {
        m_used[index] = state.used[index];
        if (m_used[index])
            m_nodes[index] = state.nodes[index];
    }
    m_root = state.root;
    m_focused = state.focused;
    m_paneCount = state.paneCount;
    m_nodeCount = state.nodeCount;
    return true;
}

Tree::Tree(int rootPaneID)
{
    for (bool& used : m_used)
        used = false;
    m_root = invalidNode;
    m_focused = invalidNode;
    m_paneCount = 0;
    m_nodeCount = 0;

    int r = allocNode();
    // r is 0 here — the pool is empty, so allocNode always succeeds.
    Node& n = m_nodes[r];
    n.parent = invalidNode;
    n.first = invalidNode;
    n.second = invalidNode;
    n.paneID = rootPaneID;
    n.orientation = Orientation::Vertical;
    n.ratio = defaultRatio;

    m_root = r;
    m_focused = r;
    m_paneCount = 1;
}

int Tree::allocNode()
{
    for (int i = 0; i < maxNodes; ++i) {
        if (!m_used[i]) {
            m_used[i] = true;
            ++m_nodeCount;
            return i;
        }
    }
    return invalidNode;
}

void Tree::freeNode(int index)
{
    if (index < 0 || index >= maxNodes || !m_used[index])
        return;
    m_used[index] = false;
    --m_nodeCount;
}

bool Tree::isLeaf(int index) const
{
    if (index < 0 || index >= maxNodes || !m_used[index])
        return false;
    return m_nodes[index].first == invalidNode;
}

int Tree::leafForPaneID(int paneID) const
{
    if (paneID == noPane)
        return invalidNode;
    for (int i = 0; i < maxNodes; ++i) {
        if (m_used[i] && m_nodes[i].first == invalidNode && m_nodes[i].paneID == paneID)
            return i;
    }
    return invalidNode;
}

bool Tree::paneIDLive(int paneID) const { return leafForPaneID(paneID) != invalidNode; }

int Tree::split(Orientation orientation, int newPaneID, float ratio)
{
    // The focused-leaf, new-pane-second special case of insertPane: the
    // existing pane stays in place as the first child, the new pane takes
    // the second side and the focus.
    return insertPane(newPaneID, m_nodes[m_focused].paneID, orientation, ratio, InsertSide::Second);
}

int Tree::insertPane(int newPaneID, int targetPaneID, Orientation orientation, float ratio, InsertSide side, InsertTarget insertTarget)
{
    if (m_paneCount >= maxPanes)
        return invalidNode;
    if (newPaneID == noPane || paneIDLive(newPaneID))
        return invalidNode;
    int leaf = leafForPaneID(targetPaneID);
    if (leaf == invalidNode)
        return invalidNode;
    if (insertTarget == InsertTarget::Root) {
        leaf = m_root;
    }

    // Need two free nodes: the new internal split and the new leaf.
    int splitNode = allocNode();
    if (splitNode == invalidNode)
        return invalidNode;
    int newLeaf = allocNode();
    if (newLeaf == invalidNode) {
        freeNode(splitNode);
        return invalidNode;
    }

    int grandparent = m_nodes[leaf].parent;
    // The existing subtree keeps its identities, shape, and ratios.
    Node& sp = m_nodes[splitNode];
    sp.parent = grandparent;
    sp.first = side == InsertSide::First ? newLeaf : leaf;
    sp.second = side == InsertSide::First ? leaf : newLeaf;
    sp.paneID = noPane;
    sp.orientation = orientation;
    sp.ratio = clampRatio(ratio);

    m_nodes[leaf].parent = splitNode;

    Node& nl = m_nodes[newLeaf];
    nl.parent = splitNode;
    nl.first = invalidNode;
    nl.second = invalidNode;
    nl.paneID = newPaneID;
    nl.orientation = Orientation::Vertical;
    nl.ratio = defaultRatio;

    // Re-link the grandparent (or root) to point at the new split.
    if (grandparent == invalidNode) {
        m_root = splitNode;
    } else {
        Node& gp = m_nodes[grandparent];
        if (gp.first == leaf)
            gp.first = splitNode;
        else
            gp.second = splitNode;
    }

    ++m_paneCount;
    m_focused = newLeaf;
    return newLeaf;
}

bool Tree::insertTree(const Tree& source, int targetPaneID, Orientation orientation, InsertSide side, InsertTarget insertTarget)
{
    int target = leafForPaneID(targetPaneID);
    if (target == invalidNode || m_paneCount + source.m_paneCount > maxPanes) {
        return false;
    }
    if (insertTarget == InsertTarget::Root) {
        target = m_root;
    }
    for (int index = 0; index < maxNodes; ++index) {
        if (source.isLeaf(index) && paneIDLive(source.m_nodes[index].paneID)) {
            return false;
        }
    }
    // Capacity and disjoint identities are established before the pool is mutated.
    const auto copySubtree = [&](auto&& copy, int index, int parent) -> int {
        const int created = allocNode();
        m_nodes[created] = source.m_nodes[index];
        m_nodes[created].parent = parent;
        if (!source.isLeaf(index)) {
            m_nodes[created].first = copy(copy, source.m_nodes[index].first, created);
            m_nodes[created].second = copy(copy, source.m_nodes[index].second, created);
        }
        return created;
    };
    const int parent = m_nodes[target].parent;
    const int split = allocNode();
    const int inserted = copySubtree(copySubtree, source.m_root, split);
    m_nodes[split] = { parent, side == InsertSide::First ? inserted : target,
        side == InsertSide::First ? target : inserted, noPane, orientation, defaultRatio };
    m_nodes[target].parent = split;
    if (parent == invalidNode) {
        m_root = split;
    } else if (m_nodes[parent].first == target) {
        m_nodes[parent].first = split;
    } else {
        m_nodes[parent].second = split;
    }
    m_paneCount += source.m_paneCount;
    focusPane(source.focusedPaneID());
    return true;
}

bool Tree::movePane(int paneID, int targetPaneID, Orientation orientation, InsertSide side, InsertTarget insertTarget)
{
    if (paneID == targetPaneID || m_paneCount < 2)
        return false;
    if (leafForPaneID(paneID) == invalidNode || leafForPaneID(targetPaneID) == invalidNode)
        return false;

    // Detach via the closePane collapse (frees the leaf + parent split), then re-insert next to the target.
    // Freeing two nodes before taking two keeps it pool-safe at the pane cap. The pane that had focus keeps it.
    const int rememberedFocusPaneID = m_nodes[m_focused].paneID;
    closePane(paneID);
    insertPane(paneID, targetPaneID, orientation, defaultRatio, side, insertTarget);
    if (rememberedFocusPaneID != paneID)
        focusPane(rememberedFocusPaneID);
    return true;
}

bool Tree::swapPanes(int firstPaneID, int secondPaneID)
{
    if (firstPaneID == secondPaneID)
        return false;
    const int firstLeaf = leafForPaneID(firstPaneID);
    const int secondLeaf = leafForPaneID(secondPaneID);
    if (firstLeaf == invalidNode || secondLeaf == invalidNode)
        return false;
    // The leaf NODES trade pane ids; shape and ratios stay put. Focus
    // follows the pane, not the position.
    m_nodes[firstLeaf].paneID = secondPaneID;
    m_nodes[secondLeaf].paneID = firstPaneID;
    if (m_focused == firstLeaf)
        m_focused = secondLeaf;
    else if (m_focused == secondLeaf)
        m_focused = firstLeaf;
    return true;
}

int Tree::subtreeLeafCount(int index) const
{
    if (index < 0 || index >= maxNodes || !m_used[index])
        return 0;
    if (m_nodes[index].first == invalidNode)
        return 1;
    return subtreeLeafCount(m_nodes[index].first) + subtreeLeafCount(m_nodes[index].second);
}

void Tree::balanceSubtree(int index)
{
    if (index < 0 || index >= maxNodes || !m_used[index] || m_nodes[index].first == invalidNode)
        return;
    Node& split = m_nodes[index];
    const int firstLeaves = subtreeLeafCount(split.first);
    const int totalLeaves = firstLeaves + subtreeLeafCount(split.second);
    // Equal share per leaf: the first child's fraction is its leaf share.
    // The interior clamp band still applies, so a very lopsided subtree
    // (past ~1:19) balances to the band edge rather than exactly equal.
    split.ratio = clampRatio(static_cast<float>(firstLeaves) / static_cast<float>(totalLeaves));
    balanceSubtree(split.first);
    balanceSubtree(split.second);
}

bool Tree::balance(int nodeIndex)
{
    if (nodeIndex < 0 || nodeIndex >= maxNodes || !m_used[nodeIndex])
        return false;
    balanceSubtree(nodeIndex);
    return true;
}

bool Tree::adjustRatio(int paneID, Direction direction, float step)
{
    if (step <= 0.0f)
        return false;
    const int leaf = leafForPaneID(paneID);
    if (leaf == invalidNode)
        return false;

    // Prefer the nearest ancestor split whose divider borders the pane on its `direction` side (Right/Down:
    // pane is the FIRST child; Left/Up: SECOND). Fallback to the nearest matching-axis ancestor is what
    // makes grow-right with no right divider shrink from the left instead of doing nothing.
    int borderingSplit = invalidNode;
    int fallbackSplit = invalidNode;
    int child = leaf;
    int parent = m_nodes[child].parent;
    while (parent != invalidNode) {
        const Node& p = m_nodes[parent];
        if (axisMatches(p.orientation, direction)) {
            if (fallbackSplit == invalidNode)
                fallbackSplit = parent;
            const bool isFirstChild = (p.first == child);
            if (isFirstChild == towardSecond(direction)) {
                borderingSplit = parent;
                break;
            }
        }
        child = parent;
        parent = m_nodes[child].parent;
    }
    const int target = borderingSplit != invalidNode ? borderingSplit : fallbackSplit;
    if (target == invalidNode)
        return false;

    // Right/Down moves the divider toward the second child (ratio up);
    // Left/Up toward the first (ratio down). Report a change only when the
    // clamp band let the ratio actually move.
    const float previousRatio = m_nodes[target].ratio;
    const float adjustedRatio = clampRatio(previousRatio + (towardSecond(direction) ? step : -step));
    if (adjustedRatio == previousRatio)
        return false;
    m_nodes[target].ratio = adjustedRatio;
    return true;
}

int Tree::leftmostLeaf(int index) const
{
    int current = index;
    while (current != invalidNode && m_nodes[current].first != invalidNode)
        current = m_nodes[current].first;
    return current;
}

bool Tree::closeFocused() { return closePane(m_nodes[m_focused].paneID); }

bool Tree::closePane(int paneID)
{
    if (m_paneCount <= 1)
        return false;

    const int leaf = leafForPaneID(paneID);
    if (leaf == invalidNode)
        return false;

    int parent = m_nodes[leaf].parent;
    // Non-last leaf always has a parent split (root with >1 pane is a
    // split), but guard anyway.
    if (parent == invalidNode)
        return false;

    Node& p = m_nodes[parent];
    int sibling = (p.first == leaf) ? p.second : p.first;
    int grandparent = p.parent;

    bool closingFocused = (leaf == m_focused);

    // Promote the sibling subtree into the parent's slot.
    m_nodes[sibling].parent = grandparent;
    if (grandparent == invalidNode) {
        m_root = sibling;
    } else {
        Node& gp = m_nodes[grandparent];
        if (gp.first == parent)
            gp.first = sibling;
        else
            gp.second = sibling;
    }

    freeNode(leaf);
    freeNode(parent);
    --m_paneCount;

    if (closingFocused)
        m_focused = leftmostLeaf(sibling);

    return true;
}

bool Tree::moveFocus(Direction direction)
{
    // Walk up to the first ancestor split on the matching axis where the
    // focused side can move in `direction`, then descend the sibling toward the
    // shared edge — and toward the focused pane's cross-axis position — to land
    // on the geometrically adjacent leaf.
    int child = m_focused;
    int parent = m_nodes[child].parent;
    while (parent != invalidNode) {
        const Node& p = m_nodes[parent];
        if (axisMatches(p.orientation, direction)) {
            bool childIsFirst = (p.first == child);
            // Moving toward the second child requires being the first
            // child (and vice versa).
            if (towardSecond(direction) && childIsFirst) {
                float crossMidpoint = crossMidpointWithin(parent, m_focused, direction);
                int target = nearestLeafTowardEdge(p.second, direction, crossMidpoint);
                if (target != invalidNode) {
                    m_focused = target;
                    return true;
                }
            } else if (!towardSecond(direction) && !childIsFirst) {
                float crossMidpoint = crossMidpointWithin(parent, m_focused, direction);
                int target = nearestLeafTowardEdge(p.first, direction, crossMidpoint);
                if (target != invalidNode) {
                    m_focused = target;
                    return true;
                }
            }
        }
        child = parent;
        parent = m_nodes[child].parent;
    }
    return false;
}

float Tree::crossMidpointWithin(int ancestor, int leaf, Direction direction) const
{
    // Walk `leaf` → `ancestor`, narrowing [lo,hi] at each CROSS-axis split (perpendicular to `direction`):
    // the child occupies first=[0,ratio] or second=[ratio,1] of the cross extent, so remap into that slice.
    // Movement-axis splits span the full cross extent and don't narrow. Result: the leaf's cross midpoint.
    float lo = 0.0f;
    float hi = 1.0f;
    int current = leaf;
    while (current != ancestor && current != invalidNode) {
        int parent = m_nodes[current].parent;
        if (parent == invalidNode)
            break;
        const Node& p = m_nodes[parent];
        if (!axisMatches(p.orientation, direction)) {
            const float ratio = p.ratio;
            if (p.first == current) {
                lo *= ratio;
                hi *= ratio;
            } else {
                lo = ratio + lo * (1.0f - ratio);
                hi = ratio + hi * (1.0f - ratio);
            }
        }
        current = parent;
    }
    return (lo + hi) * 0.5f;
}

int Tree::nearestLeafTowardEdge(int subtree, Direction direction, float crossMidpoint) const
{
    // Descend `subtree` to the leaf adjacent to the origin pane. Movement-axis split: enter from the shared
    // edge (Right/Down → first, Left/Up → second). Cross-axis split: follow `crossMidpoint` into the child
    // whose slice contains it, rescaling. The `<=` tie-break lands a centered origin on the first child.
    int current = subtree;
    while (current != invalidNode && m_nodes[current].first != invalidNode) {
        const Node& n = m_nodes[current];
        if (axisMatches(n.orientation, direction)) {
            current = towardSecond(direction) ? n.first : n.second;
        } else {
            const float ratio = n.ratio;
            if (crossMidpoint <= ratio) {
                current = n.first;
                crossMidpoint = ratio > 0.0f ? crossMidpoint / ratio : 0.0f;
            } else {
                current = n.second;
                crossMidpoint = ratio < 1.0f ? (crossMidpoint - ratio) / (1.0f - ratio) : 0.0f;
            }
        }
    }
    return current;
}

bool Tree::focusPane(int paneID)
{
    const int leaf = leafForPaneID(paneID);
    if (leaf == invalidNode)
        return false;
    m_focused = leaf;
    return true;
}

bool Tree::setRatio(int index, float ratio)
{
    if (index < 0 || index >= maxNodes || !m_used[index])
        return false;
    if (m_nodes[index].first == invalidNode)
        return false; // not a split
    m_nodes[index].ratio = clampRatio(ratio);
    return true;
}

bool sameShape(const Tree& a, int ia, const Tree& b, int ib)
{
    const bool aLeaf = a.isLeaf(ia);
    if (aLeaf != b.isLeaf(ib))
        return false;
    if (aLeaf)
        return a.node(ia).paneID == b.node(ib).paneID;
    const Node& na = a.node(ia);
    const Node& nb = b.node(ib);
    if (na.orientation != nb.orientation)
        return false;
    return sameShape(a, na.first, b, nb.first) && sameShape(a, na.second, b, nb.second);
}

} // namespace Ubiquitty::Panes
