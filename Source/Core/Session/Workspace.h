// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
// A serializable, origin-agnostic arrangement of surfaces: the window/tab/pane-split
// layout plus each pane's ORIGIN (where to recreate its session), as bytes that
// restore the same arrangement across launches and machines. The durable SNAPSHOT
// of the live SurfaceRegistry. Portable, header-light.

#include "Core/Transport/Origin.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace Ubiquitty::Session {

// One node of a tab's pane layout. A leaf is a single pane (its Origin + title);
// an internal node splits two children along an axis at `ratio` (the first
// child's fraction). Mirrors the frontend's binary split tree, serializably.
struct PaneNode {
    bool leaf = true;
    // Leaf fields:
    Transport::Origin origin; // how to recreate this pane's session on restore
    std::string title;
    /// The local launch directory; empty lets the shell choose its default.
    std::string directory;
    // Split fields:
    bool horizontalSplit = false; // true: children stacked top/bottom; false: side by side
    double ratio = 0.5; // first child's fraction of the axis
    std::vector<PaneNode> children; // exactly 2 for a split; empty for a leaf

    /// Builds a leaf pane holding one session.
    ///
    /// - Parameters:
    ///   - origin: How to recreate this pane's session on restore.
    ///   - title: The pane's title, empty when it has none.
    /// - Returns: A leaf node; `children` stays empty and `ratio` unused.
    static PaneNode makeLeaf(Transport::Origin origin, std::string title = {})
    {
        PaneNode node;
        node.leaf = true;
        node.origin = std::move(origin);
        node.title = std::move(title);
        return node;
    }
    static PaneNode makeSplit(bool horizontal, double ratio, PaneNode first, PaneNode second)
    {
        PaneNode node;
        node.leaf = false;
        node.horizontalSplit = horizontal;
        node.ratio = ratio;
        node.children.push_back(std::move(first));
        node.children.push_back(std::move(second));
        return node;
    }
};

struct WorkspaceTab {
    std::string title;
    PaneNode root; // the tab's pane layout
    /// The focused leaf's index in depth-first, first-child order.
    int focusedPaneIndex = 0;
};

struct WorkspaceWindow {
    std::vector<WorkspaceTab> tabs;
    int activeTab = 0;
    /// The active tab's zoomed leaf index, or -1 when no pane is zoomed.
    int zoomedPaneIndex = -1;
};

struct Workspace {
    std::string name;
    std::vector<WorkspaceWindow> windows;
    /// The selected window's index in `windows`.
    int activeWindow = 0;
};

// Serialize `workspace` to a versioned, length-prefixed byte form appended to
// `out` (recursive over the pane tree). Pure; no allocation beyond `out`.
void serializeWorkspace(const Workspace&, std::vector<std::uint8_t>& out);

// Parse a serialized workspace. Returns false (leaving `out` partially filled)
// on a malformed / truncated / wrong-version buffer — bounds-safe over
// untrusted bytes (a restored .workspace file may be hand-edited or corrupt).
//
// The bytes arrive as a view so a caller cannot describe a length the buffer
// does not have, which for a parser over untrusted input is the whole of what
// separates a rejection from an over-read. `out` stays a parameter rather than
// a returned optional: a Workspace owns a vector of windows, each owning a pane
// tree, and the caller usually has one to fill.
[[nodiscard]] bool deserializeWorkspace(std::span<const std::uint8_t> data, Workspace& out);

/// Encodes an arrangement as base64 for storage in a text document.
///
/// - Returns: The encoded archive, or empty when encoding fails.
std::string serializeWorkspaceText(const Workspace&);

/// Decodes a bounded base64 arrangement, rejecting malformed or oversized data.
[[nodiscard]] bool deserializeWorkspaceText(const std::string&, Workspace&);

// Deep structural + value equality (origins, titles, ratios, tree shape) — for
// tests and de-dup. Inline so callers need no extra link.
bool equalPaneNodes(const PaneNode& a, const PaneNode& b);
bool equalWorkspaces(const Workspace& a, const Workspace& b);

// Bounds window, tab, and focus selections and clears invalid zoom targets.
// Split ratios are clamped to finite [0,1], with non-finite values set to 0.5.
void normalizeWorkspace(Workspace&);

} // namespace Ubiquitty::Session
