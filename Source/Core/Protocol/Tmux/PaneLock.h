// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/Protocol/Tmux/LayoutNode.h"
#include "Core/Protocol/Tmux/PaneID.h"

#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>

namespace Ubiquitty::Panes {
class Tree;
}

namespace Ubiquitty::Protocol::Tmux {

// Parse a tmux layout string (e.g. "bf3a,80x24,0,0,1" or with {}/[] nesting) into a
// LayoutNode tree. The leading "<checksum>," prefix is tolerated. Returns false (and
// leaves `out` empty) on a malformed string.
bool parseLayoutTree(std::string_view layout, LayoutNode& out);

/// The parse above, spelled as pointer and length for the caller that
/// cannot build a view — one grammar either way, this overload delegates
/// to the view form.
///
/// - Parameters:
///   - layout: The layout string's bytes.
///   - length: How many bytes `layout` holds.
///   - out: Receives the parsed tree.
/// - Returns: Whether the string parsed.
inline bool parseLayoutTree(const char* layout, std::ptrdiff_t length, LayoutNode& out)
{
    return parseLayoutTree(std::string_view { layout, static_cast<std::size_t>(length > 0 ? length : 0) }, out);
}

// tmux flattens same-axis splits into one N-ary node ({a,b,c}); the frontends
// mirror a BINARY Ubiquitty::Panes::Tree. These bridge the two, portably (the
// tree math is identical for macOS NSSplitView and Linux GTK paned).
//
// leftmostPaneID: the pane id of `node`'s leftmost leaf (descend first child).
// A Panes::Tree's root leaf keeps its id while split() adds the rest, so the
// tree must be seeded as Panes::Tree(leftmostPaneID(root)) before building.
PaneID leftmostPaneID(const LayoutNode&);

/// Right-nests a parsed tmux layout node's children into a binary pane tree.
///
/// An N-ary `{a,b,c}` becomes `a | (b | c)`, with split ratios matching each
/// child's cell fraction along its axis — Columns side-by-side, Rows stacked.
/// Every leaf carries its tmux pane id on return, which is what lets the
/// frontend map a leaf to its view.
///
/// - Parameters:
///   - tree: Must already hold the single leaf `leftmostPaneID(node)`; on
///     return it mirrors the full layout.
///   - node: The parsed layout to nest in.
void buildPaneTreeInto(Ubiquitty::Panes::Tree&, const LayoutNode&);

// The ENCODE half of buildPaneTreeInto / parseLayoutTree: serialize a binary
// Ubiquitty::Panes::Tree subtree (rooted at `nodeIndex`) to tmux's layout grammar,
// which parseLayoutTree round-trips.
//
// Each leaf emits its pane's real columns×rows via `dimensionsForPane` (≤ 0 falls
// back to 1). A split emits its children's bounding box (sum along the split axis,
// max across), so ratios are re-derived from the extents — there is no ratio field.
// `{}` = Vertical (columns), `[]` = Horizontal (rows). The geometry origin is always
// "0,0" (absolute positions re-derived from the extents). Appends to `out` and
// returns the subtree's {columns, rows}. A full layout string prefixes "0000,"
// (a constant checksum parseLayoutTree tolerates).
struct PaneDimensions {
    int columns;
    int rows;
};
// paneID → its grid size. `context` is the caller's opaque pane registry.
using PaneDimensionsLookup = PaneDimensions (*)(void* context, PaneID paneID);

struct PaneLayoutExtent {
    int columns;
    int rows;
};

/// The extent a leaf is laid out at: its pane's grid, each dimension at
/// least one cell.
///
/// The one rule for a pane the host has not sized yet, shared by the layout
/// string and by every other description of the same layout.
///
/// - Parameter dimensions: The pane's grid as its owner reports it.
/// - Returns: The leaf's columns and rows.
[[nodiscard]] constexpr PaneLayoutExtent leafLayoutExtent(PaneDimensions dimensions)
{
    return { dimensions.columns > 0 ? dimensions.columns : 1, dimensions.rows > 0 ? dimensions.rows : 1 };
}

/// The extent of a split of two laid-out children: their sum along the
/// split's axis and the larger of the two across it.
///
/// The one rounding rule of the host's layout. The layout string and a
/// workspace's structure record both take a split's size from here, so no
/// two descriptions of a layout disagree by a cell.
///
/// - Parameters:
///   - areSideBySide: Whether the children are columns, not rows.
///   - first: The first child's extent.
///   - second: The second child's extent.
/// - Returns: The split's columns and rows.
[[nodiscard]] constexpr PaneLayoutExtent splitLayoutExtent(bool areSideBySide, PaneLayoutExtent first, PaneLayoutExtent second) // bool-ok: the split's one axis
{
    return areSideBySide ? PaneLayoutExtent { first.columns + second.columns, first.rows > second.rows ? first.rows : second.rows }
                         : PaneLayoutExtent { first.columns > second.columns ? first.columns : second.columns, first.rows + second.rows };
}
/// Serializes a pane-tree subtree to tmux's layout grammar.
///
/// The encode half of `buildPaneTreeInto` / `parseLayoutTree`, which round-trip
/// it. A split emits its children's bounding box — sum along the split axis, max
/// across — so ratios are re-derived from extents; the grammar has no ratio
/// field. `{}` is Vertical (columns), `[]` is Horizontal (rows), and the
/// geometry origin is always "0,0" since absolute positions come back out of the
/// extents too. A full layout string prefixes "0000,", a constant checksum
/// `parseLayoutTree` tolerates.
///
/// - Parameters:
///   - tree: The tree to read.
///   - nodeIndex: The subtree root.
///   - dimensionsForPane: Supplies each leaf's real grid size; a non-positive
///     dimension falls back to 1.
///   - context: The caller's opaque pane registry, passed through.
///   - out: Appended to, never cleared.
/// - Returns: The subtree's columns and rows.
PaneLayoutExtent emitPaneLayout(const Ubiquitty::Panes::Tree& tree, int nodeIndex, PaneDimensionsLookup dimensionsForPane, void* context, std::string& out);

// A small per-pane lock guarding Ubiquitty::Terminal access: the deliver/ingest path
// is the sole writer, the render/serialize paths are readers. A portable std::mutex,
// so no platform lock type leaks into this module; the frontend adopts the same mutex
// via nativeHandle (one canonical lock per pane).
class PaneLock {
public:
    void lock() { m_lock.lock(); }
    void unlock() { m_lock.unlock(); }
    // The underlying mutex, so a frontend's render adapter adopts the same lock
    // the deliver path takes — one canonical lock per pane.
    std::mutex* nativeHandle() { return &m_lock; }

private:
    std::mutex m_lock;
};

} // namespace Ubiquitty::Protocol::Tmux
