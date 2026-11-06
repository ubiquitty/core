// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
// Bridge the LIVE layout tree (Session::Layout::Workspace) to the durable serialized
// snapshot (Session::Workspace). captureToSnapshot walks the live tree and emits
// the Session::Workspace form serializeWorkspace() persists. The live model holds
// only opaque PaneIDs, so the caller supplies each pane's Origin + title via a resolver.

#include "Core/Session/LayoutTree.h"
#include "Core/Session/Workspace.h"
#include "Core/Transport/Origin.h"

#include <string>

namespace Ubiquitty::Session::Layout {

// The per-pane data the live model does not hold — supplied by the caller for
// each leaf during capture (its paneID → session lookup).
struct PaneInfo {
    Ubiquitty::Transport::Origin origin;
    std::string title;
    std::string directory;
};

/// Selects whether restore reuses a fresh model's initial window or appends.
enum class RestorePlacement {
    FreshWorkspace,
    Append,
};

// Resolve a PaneID to its Origin + title. fn-pointer + context (no std::function),
// matching the Core callback idiom.
using PaneInfoResolver = PaneInfo (*)(void* context, PaneID pane);

// Capture the live workspace into the serialized snapshot: each Surface → a
// WorkspaceWindow (carrying its activeTab), each Tab → a WorkspaceTab, the
// tab's Panes::Tree → a PaneNode tree (splits keep orientation + ratio, leaves
// carry the resolved Origin + title). Pure read over `live`.
Ubiquitty::Session::Workspace captureToSnapshot(const Workspace& live, PaneInfoResolver, void* context, SurfaceID selectedSurface = invalidSurface);

// Reported for each pane reconstructed during restore (in tree order), so the
// caller recreates that session for the freshly-allocated PaneID.
using PaneRestoredFn = void (*)(void* context, PaneID pane, const PaneInfo& info);

// Restore a serialized snapshot into `out` (which must be freshly default-
// constructed). Rebuilds surfaces/tabs/split-trees allocating FRESH ids (not
// preserved across restore), calling `onPane` for every reconstructed leaf.
// Validates all trees and model caps before calling `onPane`. Restores names,
// selection, and zoom using leaf positions rather than persisted allocator ids.
// Returns false for malformed trees or exceeded caps. Inverse of captureToSnapshot.
bool restoreFromSnapshot(const Ubiquitty::Session::Workspace& snapshot, Workspace& out, PaneRestoredFn onPane, void* context,
    RestorePlacement placement = RestorePlacement::FreshWorkspace);

} // namespace Ubiquitty::Session::Layout
