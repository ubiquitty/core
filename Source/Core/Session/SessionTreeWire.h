// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

//
// SessionTreeWire.h — a workspace's structure as bytes (Docs/sync.md §7.3).
//
// A host describes each workspace it holds in a record of its own: the
// workspace, its windows, each window's tabs, each tab's split and what every
// level is called. A client attached to a workspace is sent that workspace's
// record whenever its structure changes, and a client listing a host is sent
// every workspace's. There is one description of a host's layout, read by
// both.
//
// The record is structure and nothing else. What a pane is doing travels in
// the two records a pane is sent (`PaneFacts`, `StatusRecords`), and a client
// puts the two together (`assembleSessionTree`). A leaf says which pane it
// is three ways: the pane as the screen plane names it, the handle a layout
// verb names it by, and the session behind it. Every node of a split carries
// its size and its place in cells as the host's layout computed them, so no
// client rounds for itself.
//
// A workspace nothing runs in has no pane records to be sent, so its leaves
// carry what its record holds of each pane: a title, a directory, a host.
//
// Nothing a tree computes crosses: no status, activity or count. The
// decoder checks every count and length against its cap before an element is
// read, and a body it refuses yields nothing.
//
// A record a legal host builds always fits. `maxWorkspaceStructureBytes` is
// the sum of every level at its cap, and a host holds no more of anything
// than the caps here say: the checks in the encoder are for a caller that
// built a workspace no host holds.
//

#include "Core/Protocol/Sync/NativeAttachment.h"
#include "Core/Protocol/Sync/Topology.h"
#include "Core/Session/HostLayoutRecord.h"
#include "Core/Session/Identity.h"
#include "Core/Session/LayoutTree.h"
#include "Core/Session/SessionTree.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace Ubiquitty::Session {

/// The most panes one workspace's structure names. Every pane a host runs has
/// a session, a host refuses a session past this many, and a workspace's
/// record of what it could restore holds no more.
inline constexpr std::size_t maxStructureLeaves = 64;
static_assert(maxHostLayoutPanes <= maxStructureLeaves, "a workspace restored from its record fits a structure");
/// The most attachments a pane's leaf names: no more than a host has clients.
inline constexpr std::size_t maxTreeAttendeesPerPane = 256;
/// The most bytes of a tab's name, the same bound as a workspace's name.
inline constexpr std::size_t maxTreeNameBytes = maxWorkspaceNameBytes;

namespace SessionTreeWireDetail {

/// A text's bytes on the wire at its cap: its length prefix and itself.
[[nodiscard]] constexpr std::size_t textBytes(std::size_t maximum) { return 3 + maximum; }
/// A level's label and tags at their caps.
inline constexpr std::size_t labelAndTagBytes = textBytes(Identity::maxLabelLength) + 1 + Identity::maxTagsPerObject * textBytes(Identity::maxLabelLength);
/// A node's size and place: four sixteen-bit cell counts.
inline constexpr std::size_t geometryBytes = 4 * 2;
/// A leaf at every cap: its kind, the three names of its pane, its geometry
/// and generation, its label and tags and its session's, everyone attached,
/// and what a record holds of a pane nothing runs in.
inline constexpr std::size_t maxLeafBytes = 1 + 16 + 4 + 4 + geometryBytes + 8 + 2 * labelAndTagBytes
    + 3 + maxTreeAttendeesPerPane * 5
    + textBytes(maxHostLayoutTitleLength) + textBytes(maxHostLayoutDirectoryLength) + textBytes(maxHostLayoutRemoteHostLength);
/// A split: its kind, axis, ratio, geometry and the count of its children.
inline constexpr std::size_t maxSplitBytes = 1 + 1 + 8 + geometryBytes + 1;
/// A tab without its split: its id, label, tags, name and two indices.
inline constexpr std::size_t maxTabBytes = 4 + labelAndTagBytes + textBytes(maxTreeNameBytes) + 2 + 2;
/// A window without its tabs: its id, label, tags, flags, indices and zoom.
inline constexpr std::size_t maxWindowBytes = 4 + labelAndTagBytes + 1 + 2 + 2 + 2 + 2;
/// A workspace without its windows.
inline constexpr std::size_t maxWorkspaceHeadBytes = 8 + 1 + 1 + 4 + textBytes(Protocol::Sync::maxWorkspaceNameLength) + labelAndTagBytes + 2 + 2;

} // namespace SessionTreeWireDetail

/// The most bytes a workspace's structure takes: every level at its cap.
///
/// A tab holds a pane at least and a window a tab at least, so a workspace of
/// `maxStructureLeaves` panes has no more tabs than panes, no more splits
/// than panes, and no more windows than the layout model holds.
inline constexpr std::size_t maxWorkspaceStructureBytes = SessionTreeWireDetail::maxWorkspaceHeadBytes
    + static_cast<std::size_t>(Layout::maxSurfaces) * SessionTreeWireDetail::maxWindowBytes
    + maxStructureLeaves * (SessionTreeWireDetail::maxTabBytes + SessionTreeWireDetail::maxSplitBytes + SessionTreeWireDetail::maxLeafBytes);
static_assert(maxWorkspaceStructureBytes <= Protocol::Sync::maxWorkspaceStructureRecordBytes, "a workspace's structure at every cap is a record a connection carries: under a mebibyte, well inside what it queues in bulk");
static_assert(Protocol::Sync::maxTopologyDepth >= static_cast<std::size_t>(Layout::maxPanesPerTab), "no tab the layout model holds nests deeper than the one bound a reader of a split uses");

/// Appends a workspace's structure.
///
/// A name, a label or a tag past its cap is cut to it. A workspace no host
/// holds is refused whole: more windows, tabs, panes, tags or attendees than
/// a reader takes, a tab whose split's leaves are not its panes, a split
/// deeper than `maxTopologyDepth` or with a ratio that is no share, a window
/// or a tab with nothing in it, or an index or a zoom naming nothing.
///
/// - Parameters:
///   - output: The buffer to extend; left as it was when refused.
///   - workspace: The workspace; what a tree derives is not read.
/// - Returns: Whether the structure was written.
[[nodiscard]] bool encodeWorkspaceStructure(std::vector<std::uint8_t>& output, const TreeWorkspace& workspace);

/// Reads a workspace's structure.
///
/// The result holds what the record says and nothing derived: a pane's
/// records are empty unless the workspace is restorable, and no status,
/// activity or attendance is rolled up. `assembleSessionTree` does both.
///
/// - Parameter body: The record's bytes.
/// - Returns: The workspace, or nothing for a body past
///   `maxWorkspaceStructureBytes`, cut short, carrying a trailing byte, a
///   count or length past its cap, an unknown enumerator or flag, an index or
///   a zoom naming no window, tab or pane, a split whose leaves are not the
///   tab's panes, or a window or tab with nothing in it.
[[nodiscard]] std::optional<TreeWorkspace> decodeWorkspaceStructure(std::span<const std::uint8_t> body);

} // namespace Ubiquitty::Session
