// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include "Core/Session/LayoutTree.h"
#include "Core/Session/SessionTree.h"

#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

// A client's mirror of a workspace, rebuilt from the structure record the
// host sends (Docs/sync.md §7.3). One adopt builds a Layout::Workspace with
// the host's ids from a decoded TreeWorkspace, so the host and every frontend
// that mirrors one speak the same tree and a verb sent back names what the
// host holds.

namespace Ubiquitty::Session::Layout {

/// Told each pane an adopted workspace binds to a host session.
using PaneBoundFn = void (*)(void* context, PaneID pane, std::uint32_t sessionID);

/// Rebuilds `out` from a workspace's structure.
///
/// Each tab preserves the host's canonical binary topology and explicit
/// divider ratios. Individual pane grids can lag a layout change and do not
/// redefine those ratios. The structure is untrusted: one that names a
/// pane twice, a focus or a zoom no tab holds, or a tab with no pane leaves
/// `out` exactly as it was.
///
/// - Parameters:
///   - structure: The workspace, as `decodeWorkspaceStructure` read it.
///   - out: The mirror to rebuild.
///   - onPane: Called once per pane after a successful adopt.
///   - context: Passed to `onPane`.
/// - Returns: Whether the structure was adopted.
[[nodiscard]] bool adoptWorkspace(const TreeWorkspace& structure, Workspace& out, PaneBoundFn onPane, void* context);

/// What a mirror shows of a structure, as text two structures share exactly
/// when a mirror adopted from either is the same: the ids, names and labels, what is
/// active, focused and zoomed, and every node's cells, place and ratio.
///
/// A structure also revises for who is attached and what a pane is running,
/// which no mirror holds; a client compares this to tell a change it must
/// redraw for from one it need not.
///
/// - Parameter structure: The workspace.
/// - Returns: The text.
[[nodiscard]] std::string mirrorSignature(const TreeWorkspace& structure);

/// How a structure arranges its panes, as text two structures share exactly
/// when an operation on the tree, or a question asked about one, means the
/// same in both: the ids, names, labels and sessions, how each tab is split
/// and at what ratios, and what is active, focused and zoomed.
///
/// It is ``mirrorSignature`` without a node's cells and place: a pane resizes
/// when a zoom fills the window with it, so its cells change without anything
/// a zoom, a focus move, a divider or a question names having moved, and a
/// revision bound to them would refuse an operation asked about the
/// arrangement that still stands.
///
/// - Parameter structure: The workspace.
/// - Returns: The text.
[[nodiscard]] std::string arrangementSignature(const TreeWorkspace& structure);

/// A pane of a workspace its host restored from a record, beside the pane a
/// mirror held in its place before the host that served it stopped.
struct RestoredPane {
    /// Where the mirror held the pane, in the ids of the host that stopped.
    SurfaceID formerSurface = invalidSurface;
    TabID formerTab = invalidTab;
    PaneID formerPane = invalidPane;
    std::uint32_t formerSessionID = 0;
    /// Where the restored workspace holds it, in the restoring host's ids.
    SurfaceID surface = invalidSurface;
    TabID tab = invalidTab;
    PaneID pane = invalidPane;
    std::uint32_t sessionID = 0;

    friend bool operator==(const RestoredPane&, const RestoredPane&) = default;
};

/// Pairs the panes a mirror held with the panes that stand in their places
/// in the workspace a host restored from its record of them.
///
/// A record names no window, tab, pane or session: the host that restores
/// it mints its own ids and starts fresh shells, and lays them out in the
/// record's order, windows in order, each window's tabs in order and each
/// tab's leaves first child first. That place is what a restored pane shares
/// with the pane it stands in for, so panes pair by place, and only where
/// the two arrangements are plainly the same one: as many windows, as many
/// tabs in the window, and the tab split the same way along the same axes.
///
/// A host that could not start some of the record's panes leaves them out,
/// a split that loses one side becoming the other, and a tab or a window
/// that loses every pane going with it. A restoration with fewer panes than
/// the mirror pairs a pane wherever every way of losing that many panes
/// from the mirror's arrangement that leaves the restored one puts the same
/// pane in its place, and leaves unpaired a place two ways disagree on: of
/// two panes side by side that lost one, the one left is either. When no
/// way leaves the restored arrangement, or there are more ways than are
/// tried, the panes pair by place as above.
///
/// - Parameters:
///   - former: The mirror before the restored structure was adopted.
///   - formerSessions: Its pane-to-session bindings.
///   - restored: The mirror adopted from the restored workspace's structure.
///   - restoredSessions: Its pane-to-session bindings.
/// - Returns: One entry per paired pane whose sessions both sides name, in
///   the record's order.
[[nodiscard]] std::vector<RestoredPane> pairRestoredPanes(const Workspace& former, std::span<const std::pair<std::uint32_t, std::uint32_t>> formerSessions,
    const Workspace& restored, std::span<const std::pair<std::uint32_t, std::uint32_t>> restoredSessions);

/// What the restorations of a mirror leave a frontend that keeps views by
/// the ids the mirror held.
struct Restoration {
    /// Whether the mirror was restored since this was last taken. A
    /// restoration that paired nothing still leaves every id held before it
    /// one of the stopped host's, which names nothing in the mirror now.
    bool isRestored = false;
    /// The panes paired, from the ids held when this was last taken to the
    /// mirror's own.
    std::vector<RestoredPane> panes;
};

/// Carries a restoration nobody has taken past a later one.
///
/// A frontend that has not taken the first pairing still holds the ids from
/// before it: each of its pairs is followed through the later pairing, and
/// one whose pane the later restoration did not pair is dropped.
///
/// - Parameters:
///   - unread: The restoration not yet taken.
///   - later: The pairs of the later restoration, whose former ids are the
///     restored ids of `unread`'s.
/// - Returns: The restoration from the ids before `unread`'s to the ids after `later`'s.
[[nodiscard]] Restoration followRestoration(const Restoration& unread, std::span<const RestoredPane> later);

} // namespace Ubiquitty::Session::Layout
