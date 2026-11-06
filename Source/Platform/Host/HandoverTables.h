// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/Protocol/Sync/HandoverBody.h"
#include "Core/Session/HostLayoutRecord.h"
#include "Core/Session/LayoutTree.h"
#include "Platform/Host/AttachmentHandover.h"

namespace Ubiquitty::Host {

/// Receiver-owned ceilings for the private attachment-authority section.
/// Aggregate limits include every acknowledged, sent and in-flight baseline.
struct HandoverAttachmentLimits {
    std::size_t maxSectionBytes = 0;
    std::size_t maxAttachments = 0;
    std::size_t maxPaneRecords = 0;
    std::size_t maxInFlightGenerations = 0;
    std::size_t maxRowHashes = 0;
    std::size_t maxRichTextSerials = 0;
    std::size_t maxStringBytes = 0;
};

/// Encodes the daemon's frozen attachment authority without consuming secrets.
/// Physical connection/proof/handshake owners must already be stripped by
/// `exportAttachmentAuthority`; this function refuses them, never drops them.
/// - Parameters:
///   - state: The unchanged export from the authenticated predecessor.
///   - limits: Independent resource ceilings selected by the receiver.
/// - Returns: Exact section bytes, or refusal without changing source owners.
[[nodiscard]] std::expected<Protocol::Sync::HandoverBytes, Protocol::Sync::HandoverBodyError> encodeHandoverAttachments(
    const AttachmentHandover& state, const HandoverAttachmentLimits& limits);

/// Reads private attachment state without admitting it to a running daemon.
/// Complete field and aggregate-size validation precedes owned reconstruction;
/// admitted strings/vectors retain their ordinary bounded STL allocation behavior.
/// The checksum is not authentication. The caller must authenticate the local
/// predecessor, recheck its token root and current grants through
/// `inheritAttachmentAuthority`, and commit all host tables together.
/// - Parameters:
///   - bytes: Exactly one complete attachment-authority section.
///   - limits: The receiver's resource ceilings, never taken from the record.
/// - Returns: Independently owned state or refusal; no secret is consumed.
[[nodiscard]] std::expected<AttachmentHandover, Protocol::Sync::HandoverBodyError> decodeHandoverAttachments(
    std::span<const std::uint8_t> bytes, const HandoverAttachmentLimits& limits);

/// One live tab, including the node-pool holes used by its next split.
struct HandoverTab {
    std::uint32_t id = 0;
    std::string title;
    Panes::Tree::Continuation panes;
};

/// One live window in its original workspace order.
struct HandoverWindow {
    std::uint32_t id = 0;
    std::uint32_t activeTab = 0;
    std::uint32_t zoomedPane = 0;
    std::vector<HandoverTab> tabs;
};

/// A hosted workspace without references to sessions or the daemon allocator.
struct HandoverWorkspace {
    std::uint32_t id = 0;
    std::string name;
    bool shouldEndWhenDetached = false;
    bool isClosing = false;
    bool wasRestoredAfterHostRestart = false;
    /// The instances that held the workspace before the receiving one, newest
    /// first, each with the id it held the workspace under: the handing-over
    /// instance and those before it.
    std::vector<Ubiquitty::Session::HostLayoutHolder> formerHolders;
    /// When a host last held it, in Unix seconds, to the record's resolution.
    std::uint64_t heldUnixSeconds = 0;
    std::optional<Ubiquitty::Session::HostLayoutRecord> recordedLayout;
    std::uint32_t activeWindow = 0;
    std::vector<HandoverWindow> windows;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> paneSessions;
};

/// A dormant recipe with its already-issued session-space identity.
struct HandoverRestorableWorkspace {
    std::uint32_t id = 0;
    Ubiquitty::Session::HostLayoutRecord record;
};

/// The exact last publication, including a withdrawn record's revision tombstone.
struct HandoverStructureRecord {
    std::uint32_t workspaceID = 0;
    std::uint64_t revision = 0;
    std::vector<std::uint8_t> unrevised;
    std::optional<std::vector<std::uint8_t>> record;
};

/// Host topology, identity high-waters and the state of layout persistence.
/// Session owners and pending physical RPCs are admitted by the outer transaction.
struct HandoverWorkspaceTables {
    Ubiquitty::Session::Identity::Allocator::Continuation identities;
    std::uint32_t nextSessionID = 1;
    std::uint32_t nextClientID = 1;
    std::uint32_t nextStructureConsentID = 1;
    std::vector<HandoverWorkspace> workspaces;
    std::vector<HandoverRestorableWorkspace> restorable;
    std::vector<HandoverStructureRecord> structures;
    std::vector<std::pair<Ubiquitty::Session::Identity::ObjectID, std::string>> labels;
    std::vector<std::pair<Ubiquitty::Session::Identity::ObjectID, std::vector<std::string>>> tags;
    std::string layoutRecordPath;
    std::uint64_t layoutRecordDueMilliseconds = 0;
    std::uint64_t layoutRecordSignMilliseconds = 0;
    std::vector<std::uint8_t> writtenLayoutRecords;
    bool isLayoutRecordKept = false;
    bool isStructureDirty = false;
};

/// Independent aggregate limits for the workspace section, selected by its receiver.
struct HandoverWorkspaceLimits {
    std::size_t maxSectionBytes = 0;
    std::size_t maxWorkspaces = 0;
    std::size_t maxWindows = 0;
    std::size_t maxTabs = 0;
    std::size_t maxPanes = 0;
    std::size_t maxStructures = 0;
    std::size_t maxStringBytes = 0;
    std::size_t maxRecordBytes = 0;
};

/// Captures the live topology without minting IDs or changing publication state.
/// - Parameters:
///   - layout: The frozen authoritative workspace.
///   - target: Receives only windows and activeWindow; other host fields stay intact.
void captureHandoverLayout(const Ubiquitty::Session::Layout::Workspace& layout, HandoverWorkspace& target);

/// Installs an already-decoded topology into an unpublished candidate workspace.
/// Its allocator must be a private candidate shared only with other restored layouts.
/// - Parameters:
///   - source: Validated exact topology and selected window.
///   - target: Candidate owner; remains unchanged on malformed topology.
/// - Returns: Whether the topology was adopted without normalizing focus or zoom.
[[nodiscard]] bool restoreHandoverLayout(const HandoverWorkspace& source, Ubiquitty::Session::Layout::Workspace& target);

/// Encodes exact topology and identity state, including latent persistence recipes.
/// - Parameters:
///   - state: Frozen tables with no physical connection owners.
///   - limits: Independent aggregate receiver ceilings.
/// - Returns: Exact bytes or refusal without changing the source.
[[nodiscard]] std::expected<Protocol::Sync::HandoverBytes, Protocol::Sync::HandoverBodyError> encodeHandoverWorkspaces(
    const HandoverWorkspaceTables& state, const HandoverWorkspaceLimits& limits);

/// Reads exact workspace state into independent owners before daemon admission.
/// No shell is created and no recipe is executed. The outer transaction must
/// verify every pane-session binding against its carried Session table.
/// - Parameters:
///   - bytes: One complete authenticated predecessor section.
///   - limits: Receiver ceilings, never taken from the section.
/// - Returns: Owned tables or refusal; no live allocator is advanced.
[[nodiscard]] std::expected<HandoverWorkspaceTables, Protocol::Sync::HandoverBodyError> decodeHandoverWorkspaces(
    std::span<const std::uint8_t> bytes, const HandoverWorkspaceLimits& limits);

} // namespace Ubiquitty::Host
