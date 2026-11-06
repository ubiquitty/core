// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// The layout grammar, both halves: a Panes::Tree emitted as the string and the
// string parsed and built back into one. On its own so the host and a client
// mirroring it speak the grammar without linking the tmux session neither
// runs. One string both ends already speak; a second spelling of it would be a
// second set of rounding rules for the same pixels.

#include "Core/Protocol/Tmux/PaneLock.h"

#include "Core/Panes/PaneTree.h"
#include "Core/Protocol/Tmux/Tmux.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

namespace Ubiquitty::Protocol::Tmux {

PaneLayoutExtent emitPaneLayout(const Ubiquitty::Panes::Tree& tree, int nodeIndex, PaneDimensionsLookup dimensionsForPane, void* context, std::string& out)
{
    const Ubiquitty::Panes::Node& node = tree.node(nodeIndex);
    if (tree.isLeaf(nodeIndex)) {
        PaneDimensions dimensions { 1, 1 };
        if (dimensionsForPane)
            dimensions = dimensionsForPane(context, node.paneID);
        const PaneLayoutExtent extent = leafLayoutExtent(dimensions);
        out += std::to_string(extent.columns) + "x" + std::to_string(extent.rows) + ",0,0," + std::to_string(node.paneID);
        return extent;
    }
    std::string first, second;
    const PaneLayoutExtent a = emitPaneLayout(tree, node.first, dimensionsForPane, context, first);
    const PaneLayoutExtent b = emitPaneLayout(tree, node.second, dimensionsForPane, context, second);
    const bool isVertical = (node.orientation == Ubiquitty::Panes::Orientation::Vertical);
    const PaneLayoutExtent extent = splitLayoutExtent(isVertical, a, b);
    const int width = extent.columns;
    const int height = extent.rows;
    out += std::to_string(width) + "x" + std::to_string(height) + ",0,0";
    out += isVertical ? '{' : '[';
    out += first;
    out += ',';
    out += second;
    out += isVertical ? '}' : ']';
    return { width, height };
}

// ---- layout tree → Panes::Tree (N-ary → binary) ----

PaneID leftmostPaneID(const LayoutNode& node)
{
    const LayoutNode* n = &node;
    while (!n->isLeaf()) {
        if (n->children.empty())
            return n->paneID; // malformed: an internal node with no children
        n = &n->children.front();
    }
    return n->paneID;
}

namespace {

Ubiquitty::Panes::Orientation orientationFor(Split split)
{
    // {} columns sit side-by-side (a vertical divider) = Panes::Vertical;
    // [] rows stack top/bottom (a horizontal divider) = Panes::Horizontal.
    return split == Split::Rows ? Ubiquitty::Panes::Orientation::Horizontal : Ubiquitty::Panes::Orientation::Vertical;
}

// The extent a child contributes along the split axis: a vertical divider
// divides width, a horizontal one divides height.
int axisExtent(const LayoutNode& node, Ubiquitty::Panes::Orientation orientation)
{
    return orientation == Ubiquitty::Panes::Orientation::Vertical ? node.width : node.height;
}

} // namespace

void buildPaneTreeInto(Ubiquitty::Panes::Tree& tree, const LayoutNode& node)
{
    if (node.isLeaf())
        return; // the seed leaf already carries this pane's id
    const std::vector<LayoutNode>& kids = node.children;
    if (kids.size() < 2) {
        if (!kids.empty())
            buildPaneTreeInto(tree, kids.front()); // degenerate wrapper
        return;
    }

    const Ubiquitty::Panes::Orientation orientation = orientationFor(node.split);

    int total = 0;
    for (const LayoutNode& kid : kids)
        total += axisExtent(kid, orientation);
    if (total <= 0)
        total = static_cast<int>(kids.size());

    // Right-nest the N children into binary splits: split the leftmost leaf
    // (already in the tree) into [c0 | c1 | … | ck]. split() keeps the existing
    // pane as the first child and focuses the NEW second child — the leftmost
    // leaf of the next child — so the following split lands to its right.
    // ratio = the dividing child's fraction of the still-undivided extent.
    if (!tree.focusPane(leftmostPaneID(node)))
        return;
    int remaining = total;
    for (std::size_t childIndex = 1; childIndex < kids.size(); ++childIndex) {
        const int leftExtent = axisExtent(kids[childIndex - 1], orientation);
        const float ratio = remaining > 0 ? static_cast<float>(leftExtent) / static_cast<float>(remaining) : 0.5f;
        if (tree.split(orientation, leftmostPaneID(kids[childIndex]), ratio) == Ubiquitty::Panes::invalidNode)
            return; // pool exhausted or duplicate id — the partial tree still renders
        remaining -= leftExtent;
    }

    // Recurse into internal children: each placed leftmost leaf becomes that
    // child's own subtree. buildPaneTreeInto re-focuses, so order is irrelevant.
    for (const LayoutNode& kid : kids) {
        if (!kid.isLeaf())
            buildPaneTreeInto(tree, kid);
    }
}

// ---- parseLayoutTree ----
//
// tmux layout grammar (after the leading "<checksum>," prefix):
//   node   := "<W>x<H>,<X>,<Y>" ( ",<paneID>" | "{" list "}" | "[" list "]" )
//   list   := node ( "," node )*
// "{}" = left→right columns, "[]" = top→bottom rows, ",<paneID>" = a leaf.

namespace {

struct Cursor {
    std::string_view rest;
    bool ok = true;

    [[nodiscard]] char peek() const { return rest.empty() ? '\0' : rest.front(); }
    void advance() { rest.remove_prefix(1); }

    int parseInt()
    {
        // long accumulator + 5-digit / tmuxMaxLayoutDimension cap so a hostile
        // %layout-change field can't integer-overflow this. Mirrors parseDimension.
        long value = 0;
        bool sawDigit = false;
        int digits = 0;
        while (!rest.empty() && rest.front() >= '0' && rest.front() <= '9') {
            value = value * 10 + (rest.front() - '0');
            rest.remove_prefix(1);
            sawDigit = true;
            if (++digits > 5 || value > tmuxMaxLayoutDimension) {
                ok = false;
                return 0;
            }
        }
        if (!sawDigit)
            ok = false;
        return static_cast<int>(value);
    }

    bool expect(char c)
    {
        if (!rest.empty() && rest.front() == c) {
            rest.remove_prefix(1);
            return true;
        }
        ok = false;
        return false;
    }
};

bool parseNode(Cursor& c, LayoutNode& node, int depth)
{
    if (!c.ok)
        return false;
    // Bound nesting so a deeply-nested layout from a hostile server can't recurse
    // the ingest-thread stack to exhaustion (hard crash under -fno-exceptions).
    if (depth > tmuxMaxLayoutDepth) {
        c.ok = false;
        return false;
    }
    node.width = c.parseInt();
    c.expect('x');
    node.height = c.parseInt();
    c.expect(',');
    node.x = c.parseInt();
    c.expect(',');
    node.y = c.parseInt();
    if (!c.ok)
        return false;

    const char next = c.peek();
    if (next == '{' || next == '[') {
        node.split = (next == '{') ? Split::Columns : Split::Rows;
        const char closer = (next == '{') ? '}' : ']';
        c.advance(); // consume opener
        while (c.ok) {
            node.children.emplace_back();
            if (!parseNode(c, node.children.back(), depth + 1))
                return false;
            if (c.peek() == ',') {
                c.advance();
                continue;
            }
            break;
        }
        return c.expect(closer);
    }
    // Leaf: ",<paneID>".
    node.split = Split::Leaf;
    if (!c.expect(','))
        return false;
    node.paneID = c.parseInt();
    return c.ok;
}

} // namespace

bool parseLayoutTree(std::string_view layout, LayoutNode& out)
{
    out = LayoutNode {};
    if (layout.empty())
        return false;

    // Strip the leading "<checksum>," if present — the first comma-delimited
    // token is the checksum iff it isn't a "WxH" geometry (geometry contains 'x').
    const std::size_t firstComma = layout.find(',');
    if (firstComma != std::string_view::npos && layout.substr(0, firstComma).find('x') == std::string_view::npos)
        layout.remove_prefix(firstComma + 1); // skip the checksum token + its comma

    Cursor c { layout };
    if (!parseNode(c, out, 0))
        return false;
    return c.ok;
}

} // namespace Ubiquitty::Protocol::Tmux
