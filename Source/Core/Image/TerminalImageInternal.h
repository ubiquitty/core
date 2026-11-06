// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include "Base/Types.h"
#include "Core/Image/TerminalImage.h"
#include "Forward.h"

// The internal half of Ubiquitty::Image: helpers shared between the lifecycle in
// TerminalImage.cpp and the decoder backends, operating directly on the Terminal
// image / placement arrays rather than through the public surface in TerminalImage.h.
// Internal to the layer, not to the namespace — every decoder backend calls these.

namespace Ubiquitty::Image {

// Image slot management.

/// Takes a free image slot, evicting by the budget rule when none is free.
///
/// - Parameters:
///   - t: The terminal whose store the slot comes from.
/// - Returns: The slot, or null when the store cannot yield one.
TerminalImage* allocateSlot(Ubiquitty::Terminal& t);
void dispose(TerminalImage*);
void initFrameZero(TerminalImage*);

// Stamp `keep` as most-recently-used, then evict LRU images (non-displayed first,
// then oldest) until the pixel total is within Terminal::kittyImageBytesBudget.
// `keep` is never evicted. Evicting an image drops its placements too.
void enforceBudget(Ubiquitty::Terminal* t, TerminalImage* keep);

/// How strictly a lookup matches an existing placement.
enum class PlacementMatchMode : uint8_t {
    RemoveAll,
    MatchID
};

// Placement slot management.

/// Takes a free placement slot.
///
/// - Parameters:
///   - t: The terminal whose store the slot comes from.
/// - Returns: The slot, or null when the store is full.
TerminalImagePlacement* allocatePlacement(Ubiquitty::Terminal& t);
/// Removes placements of one image.
///
/// - Parameters:
///   - t: The terminal holding them.
///   - imageID: Which image.
///   - matchMode: Whether `placementID` narrows it to one.
///   - placementID: The placement, when the mode asks for it.
/// - Returns: How many were removed.
int removePlacements(Ubiquitty::Terminal& t, uint32_t imageID, PlacementMatchMode matchMode, uint32_t placementID);
/// Removes every placement, leaving the images themselves.
///
/// - Parameters:
///   - t: The terminal holding them.
/// - Returns: How many were removed.
int removeAllPlacements(Ubiquitty::Terminal& t);
/// Whether an image is placed anywhere on the screen.
///
/// - Parameters:
///   - t: The terminal to ask.
///   - imageID: Which image.
/// - Returns: True when at least one placement references it.
bool hasPlacement(Ubiquitty::Terminal& t, uint32_t imageID);

// Mirror / dirty / GC bookkeeping.

/// Recomputes whether one image has a live placement.
///
/// - Parameters:
///   - t: The terminal that owns it.
void syncMirror(Ubiquitty::Terminal& t, TerminalImage*);
/// Recomputes whether each image has a live placement, after a bulk change.
///
/// - Parameters:
///   - t: The terminal that owns them.
void syncAllMirrors(Ubiquitty::Terminal& t);
/// Frees images no placement references any more.
///
/// - Parameters:
///   - t: The terminal to collect.
void disposeUnreferenced(Ubiquitty::Terminal& t);
/// Marks the rows an image covers, so the next frame repaints them.
///
/// - Parameters:
///   - t: The terminal to mark.
void markDirty(Ubiquitty::Terminal& t, const TerminalImage*);

} // namespace Ubiquitty::Image
