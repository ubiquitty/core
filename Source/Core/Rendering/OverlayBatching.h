// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
//
// Source/Core/Rendering/OverlayBatching.h — how many overlay quads may be drawn
// together.
//
// A backend that blends overlays by reading the attachment back — the Vulkan
// arm's `subpassLoad`, which is what reproduces the CPU renderer's integer
// over-blend byte for byte — has to order the reads against the writes. Doing
// that between every quad is one barrier per quad, and a dragged selection
// emits one quad per row: a sixty-row window becomes sixty draws with
// fifty-nine barriers, each draining rasterisation before the next quad starts.
//
// Most of those quads cannot see each other. A selection covers one span per
// row and the spans sit on different rows, so no fragment of one reads a pixel
// another wrote. Only the cursor, drawn last, lands on top of anything.
//
// So the ordering is needed exactly where quads overlap, and that is decidable
// from the quads themselves. This says where a run of mutually disjoint quads
// ends; a backend draws the run in one instanced call and orders only across
// the boundary.
//
// This does not change what is drawn. Within a run each fragment reads pixels
// no other quad in the run writes, so the result is what the per-quad schedule
// produced — the same shader, the same blend, the same order where order can be
// observed.

#include "Core/Rendering/CellInstance.h"

#include <cstddef>
#include <span>

namespace Ubiquitty::Rendering {

/// Whether two overlay quads cover any pixel in common.
///
/// Touching counts as overlapping. Rasterisation's top-left rule means quads
/// that share an edge do not actually share a pixel, but the rects are floats
/// that arrived through a rounding, and the cost of being wrong is a blend
/// ordered against nothing while the cost of being conservative is one more
/// barrier. A quad's painted area is bounded by its rect — a corner radius and
/// a stroke both cut inward — so the rect is also its read footprint.
///
/// - Parameters:
///   - first: One quad.
///   - second: The other.
/// - Returns: Whether their rects meet.
[[nodiscard]] inline bool overlayQuadsMeet(const OverlayInstance& first, const OverlayInstance& second)
{
    return first.topLeftInPixels.x <= second.bottomRightInPixels.x
        && second.topLeftInPixels.x <= first.bottomRightInPixels.x
        && first.topLeftInPixels.y <= second.bottomRightInPixels.y
        && second.topLeftInPixels.y <= first.bottomRightInPixels.y;
}

/// The longest run of mutually disjoint quads starting at `first`.
///
/// Quads are checked against every quad already in the run rather than against
/// the run's bounding box: per-row spans of different widths share a bounding
/// box that covers rows none of them touch, and a box test would split a
/// selection into one run per row.
///
/// The run is capped so the pairwise scan cannot dominate a frame. A cap only
/// ends a run early, which costs one barrier and never changes a pixel.
///
/// - Parameters:
///   - overlays: The quads, in the order they are drawn.
///   - first: Where the run starts.
/// - Returns: How many quads may be drawn together, starting at `first`. Zero
///   when `first` is past the end.
[[nodiscard]] inline std::size_t disjointOverlayRun(std::span<const OverlayInstance> overlays, std::size_t first)
{
    // Past the point where a longer run saves anything measurable, and short
    // enough that the scan stays a rounding error on a frame.
    constexpr std::size_t longestRun = 256;
    if (first >= overlays.size())
        return 0;
    std::size_t run = 1;
    while (run < longestRun && first + run < overlays.size()) {
        bool doesMeet = false;
        for (std::size_t earlier = 0; earlier < run; ++earlier) {
            if (overlayQuadsMeet(overlays[first + earlier], overlays[first + run])) {
                doesMeet = true;
                break;
            }
        }
        if (doesMeet)
            break;
        ++run;
    }
    return run;
}

} // namespace Ubiquitty::Rendering
