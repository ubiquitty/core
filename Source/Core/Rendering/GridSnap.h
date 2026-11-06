// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

//
// Source/Core/Rendering/GridSnap.h — how a window extent becomes a whole number of cells
// during a live resize, and when it stops doing so.
//
// A terminal window that lands between two cell counts shows the smaller one
// and paints the remainder as background, so a drag that is not quantised gives
// away up to a cell of grid on each axis. Quantising it costs the opposite
// problem at the screen's edge: the last snap before the boundary lands short,
// and the window ends with a visible sliver of desktop beside it that no drag
// can close. The slack the drag quantiser lets the pointer through is what
// closes it, and what lets a window that is off the lattice reach it without
// a jump.
//
// The arithmetic is here rather than in a frontend because both macOS shells
// resize the same grid, and a window that snapped one way while its neighbour
// snapped another would report a different size to two shells of one session.
// Only the mouse sample that decides which edge a drag moves is a frontend's.
//

// The ceilings a snapped extent clamps against travel with it: a caller that
// reached these functions without them would invent its own limit.
#include "Core/GridBounds.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace Ubiquitty::Rendering {

/// Which end of an axis a live resize is moving.
///
/// A drag that moves the low end grows the window away from its high end, so
/// the room it has left is measured from the opposite side. Sampled once when
/// the drag opens: the proposed size a resize callback carries says how big the
/// window wants to be and never which handle is dragging it.
enum class DragEdge : std::uint8_t {
    /// Neither end — a corner-free drag, or a press that landed on no handle.
    Neither = 0,
    /// The low end: the left edge horizontally, the bottom edge vertically.
    Low = 1,
    /// The high end: the right edge horizontally, the top edge vertically.
    High = 2,
};

/// Which end of an axis a press opened a drag on.
///
/// Nearest wins inside the tolerance, so a press in a corner resolves to one
/// end per axis rather than to both or to neither.
///
/// - Parameters:
///   - press: Where the press landed along this axis.
///   - low: The window's low edge along this axis.
///   - high: The window's high edge along this axis.
///   - tolerance: How far from an edge a press still counts as grabbing it.
/// - Returns: The end being dragged, or `Neither` when the press is inside.
[[nodiscard]] inline DragEdge dragEdgeForPress(double press, double low, double high, double tolerance)
{
    const double toLow = std::abs(press - low);
    const double toHigh = std::abs(press - high);
    if (toLow > tolerance && toHigh > tolerance)
        return DragEdge::Neither;
    return toLow <= toHigh ? DragEdge::Low : DragEdge::High;
}

/// How much room an axis has left before it reaches the screen's edge.
///
/// Measured from the edge the drag leaves alone, because that one stays put:
/// dragging the low edge grows the window leftward from a fixed high edge, so
/// what remains is the distance from that high edge to the screen's low bound.
///
/// The screen bounds are the visible ones, which already exclude the menu bar
/// and the Dock — a window flush against the visible bound is flush against
/// everything the user can see.
///
/// - Parameters:
///   - edge: The end being dragged.
///   - windowLow: The window's low edge along this axis.
///   - windowHigh: The window's high edge along this axis.
///   - visibleLow: The screen's low visible bound along this axis.
///   - visibleHigh: The screen's high visible bound along this axis.
/// - Returns: The extent available, or zero when no edge is being dragged.
[[nodiscard]] inline double availableExtentForDrag(DragEdge edge, double windowLow, double windowHigh, double visibleLow, double visibleHigh)
{
    switch (edge) {
    case DragEdge::Low:
        return windowHigh - visibleLow;
    case DragEdge::High:
    case DragEdge::Neither:
        return visibleHigh - windowLow;
    }
    return 0.0;
}

/// The extent an axis takes when a live resize is quantised to whole cells.
///
/// The reserve is everything along this axis that is not grid — the padding and
/// the window's own chrome — so the caller works in one coordinate space rather
/// than converting between the frame and the content box around this call.
///
/// The result is rounded up to a whole point. The window server applies a frame
/// extent on whole points, rounding up, whatever the backing scale — AppKit
/// hands a 300.1-point frame back as 301 — so an extent chosen on any finer
/// grid is not the one the window takes, and a step from that frame finds the
/// window off the lattice this function put it on. An exact-cell extent is
/// rarely a whole point, and rounded to nearest it could land short, which is a
/// whole cell once the caller divides the realised extent back down. The
/// overshoot is under a point, blank under the last row, and the screen edge
/// clamps it.
///
/// - Parameters:
///   - proposed: The extent the drag proposes, in points.
///   - cellExtent: One cell along this axis, in points. Non-positive leaves the
///     proposal alone, which is what an unmeasured font answers.
///   - reserve: The padding and chrome the grid does not occupy.
///   - cellLimit: The most cells this axis may carry.
/// - Returns: The snapped extent, in points.
[[nodiscard]] inline double snapExtentToWholeCells(double proposed, double cellExtent, double reserve, int cellLimit)
{
    if (cellExtent <= 0.0 || cellLimit < 1)
        return proposed;
    long cells = std::lround((proposed - reserve) / cellExtent);
    if (cells < 1)
        cells = 1;
    if (cells > static_cast<long>(cellLimit))
        cells = static_cast<long>(cellLimit);
    return std::ceil(static_cast<double>(cells) * cellExtent + reserve);
}

/// One axis of one live-resize step: what the drag proposes, where the window
/// is, and what bounds it.
struct DragAxisRequest {
    /// The extent the drag proposes, in points.
    double proposed = 0.0;
    /// The extent the window has now, in points.
    double current = 0.0;
    /// One cell along this axis, in points. Non-positive leaves the proposal
    /// alone, which is what an unmeasured font answers.
    double cellExtent = 0.0;
    /// The padding and chrome the grid does not occupy.
    double reserve = 0.0;
    /// The most cells this axis may carry.
    int cellLimit = 0;
    /// The room left to the screen's visible bound. Non-positive means no
    /// bound is known.
    double available = 0.0;
};

/// The extent one axis of a live-resize step settles on.
///
/// The extents a window can snap to are the lattice: a whole number of cells
/// plus the reserve, rounded up to a whole point as `snapExtentToWholeCells`
/// rounds it. A step lands on the lattice except across two kinds of slack,
/// where it follows the pointer:
///
/// - Between the last lattice extent that fits and the screen's visible bound.
///   Snapping there would give back part of a cell and leave a strip of
///   desktop no further drag can close, so a drag to the bound ends flush with
///   it, and a drag past it is clamped to it.
/// - Between an extent off the lattice and the neighbouring lattice extent in
///   the drag's direction. A window filled to the screen, or sized by anything
///   but this quantiser, holds a fraction of a cell beyond its grid; snapping
///   its first step would jump the edge by that fraction at once. The edge
///   follows the pointer through it instead, and the grid holds its cell count
///   throughout because the slack is not a cell. Once the lattice is reached
///   the drag is quantised again.
///
/// - Parameter request: The axis.
/// - Returns: The extent to apply, in points.
[[nodiscard]] inline double quantiseDragExtent(const DragAxisRequest& request)
{
    const double cellExtent = request.cellExtent;
    if (cellExtent <= 0.0 || request.cellLimit < 1)
        return request.proposed;
    const auto latticeExtent = [&](long cells) {
        return std::ceil(static_cast<double>(cells) * cellExtent + request.reserve);
    };
    // The lattice and an applied frame are both on whole points, so an extent
    // this function chose reads back as itself and any other is a point or
    // more away; half a point tells them apart with room for the arithmetic.
    // A lattice finer than the point puts every other cell count on a half
    // point; the frame the window server applies reads back half a point
    // above it, and the window is taken for one off the lattice at every step.
    const double tolerance = 0.5;
    // The largest cell count whose lattice extent fits in `extent`, never
    // below one cell nor above the limit.
    const auto cellsFitting = [&](double extent) {
        long cells = static_cast<long>(std::floor((extent - request.reserve) / cellExtent));
        cells = std::clamp(cells, 1L, static_cast<long>(request.cellLimit));
        if (cells > 1 && latticeExtent(cells) > extent + tolerance)
            --cells;
        return cells;
    };

    if (request.available > 0.0) {
        const double lastFitting = latticeExtent(cellsFitting(request.available));
        if (request.proposed > lastFitting)
            return std::min(request.proposed, request.available);
    }

    const long cellsNow = cellsFitting(request.current);
    const double below = latticeExtent(cellsNow);
    if (std::abs(request.current - below) > tolerance) {
        const double above = latticeExtent(std::min(cellsNow + 1, static_cast<long>(request.cellLimit)));
        if (request.proposed < request.current && request.proposed >= below)
            return request.proposed;
        if (request.proposed > request.current && request.proposed <= above)
            return request.proposed;
    }

    return snapExtentToWholeCells(request.proposed, cellExtent, request.reserve, request.cellLimit);
}

} // namespace Ubiquitty::Rendering
