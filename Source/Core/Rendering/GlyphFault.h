// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/Rendering/GlyphSlotCache.h"

#include <cstdint>

namespace Ubiquitty::Rendering {

/// A cluster's slot in an atlas page, and whether this call is what put it there.
///
/// `fresh` distinguishes the caller that owes a raster from the callers that
/// found the pixels already present. A backend that rasterises unconditionally
/// re-draws every cluster on every frame it appears in; one that never checks
/// serves an empty slot.
struct ClusterSlot {
    /// Negative when the cluster earns no slot, which a caller returns an absent
    /// glyph for rather than rasterising.
    int slot = -1;
    /// True for the one call that faulted the slot in.
    bool isFresh = false;
};

/// Whether a cluster is worth an atlas slot.
///
/// A slot costs a raster and an eviction, so a cluster that cannot draw anything
/// should not take one. An empty run has no base; a NUL base is the cleared
/// cell every screen is full of; a space base carries no ink of its own, and a
/// mark over a space is the one case where refusing loses something, which is
/// why this is a shared decision rather than each backend's own — the arms
/// disagreed about spaces for as long as they each wrote it.
///
/// - Parameters:
///   - codePoints: The cluster, base first.
///   - count: How many code points the cluster carries.
/// - Returns: True when the cluster should be faulted into a slot.
[[nodiscard]] inline bool clusterWantsSlot(const std::uint32_t* codePoints, int count)
{
    if (count <= 0 || codePoints == nullptr)
        return false;
    if (codePoints[0] == 0)
        return false;
    // A lone space draws nothing. A space carrying marks does, so the count is
    // what separates them rather than the base alone.
    return !(codePoints[0] == ' ' && count == 1);
}

/// Finds or faults the slot holding a cluster's pixels.
///
/// The cache verifies contents on every hit: a cluster key folds a
/// variable-length run into a fixed width, so two different clusters can land on
/// one key, and a hit that skipped the comparison would serve the other
/// cluster's pixels. `slotForKeyVerified` re-rasterises on that collision, which
/// is what makes the key's width an efficiency question rather than a
/// correctness one.
///
/// The uv derivation is not here. A slot's extent depends on the
/// page's layout — whether the slot carries subpixel headroom around the cell
/// box, and how many cells the glyph spans — and those differ between backends
/// that are each right about their own atlas.
///
/// - Parameters:
///   - cache: The page's slot cache.
///   - key: The cluster's key in that page's keyspace.
///   - codePoints: The cluster, base first.
///   - count: How many code points the cluster carries.
/// - Returns: The slot, and whether this call must rasterise into it.
[[nodiscard]] inline ClusterSlot faultClusterSlot(GlyphSlotCache& cache, std::uint32_t key,
    const std::uint32_t* codePoints, int count)
{
    if (!clusterWantsSlot(codePoints, count))
        return {};
    ClusterSlot found;
    found.slot = cache.slotForKeyVerified(key, codePoints, count, &found.isFresh);
    if (found.slot < 0)
        found.isFresh = false;
    return found;
}

} // namespace Ubiquitty::Rendering
