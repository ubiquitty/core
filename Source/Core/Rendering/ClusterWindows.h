// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/Rendering/GlyphBackendTypes.h"
#include "Core/Rendering/GlyphKey.h"

#include <algorithm>
#include <cstdint>

// Source/Core/Rendering/ClusterWindows.h — the tall slot a stacked cluster draws through.
//
// A glyph's slot is its cell plus a margin, and the margin is capped at half a
// line (GlyphSlotPolicy.h). A combining stack can reach further: a Z under six
// marks stands a line above its cell, and the slot's edge cuts the top marks
// off. Shrinking the stack to fit was tried and rejected: the letter keeps its
// size on its baseline and the marks keep theirs, overhanging the rows above
// and below the way any glyph's ink overhangs into its margin.
//
// So the slot grows, by WINDOWS. The cluster's own slot holds exactly what it
// always held, and the ink past it is cut, one slot at a time, into further
// slots of the same mono sheet. Each window is drawn as a quad on the mono
// glyph stream, stacked on the cluster's own quad at the pitch the cut steps
// by, so the stack is one tall slot on screen with the letter where a cluster
// that fits draws it: the same baseline, the same pen, the same phase. The
// windows ride the sheet, the stream and the passes every renderer already
// draws, which is what puts their ink over the neighbouring rows' backgrounds
// exactly where margin overhang already lands, and what lets a retained frame
// read their crossings from the mono stream like any glyph's.
//
// Windows overlap their neighbours by a guard row: each window's slot carries
// one row of the neighbouring window's ink past the rows its quad draws, so a
// linear tap at the quad's edge reads the row the tall drawing holds there and
// not an unrelated slot's. The cluster's own quad gives up a guard row on each
// side a window abuts it, for the same reason.
//
// A cluster earns windows by measurement, not by guess: the backend draws it
// uncut once and reports the rows its ink covers, and the answer is kept with
// the cluster's slot. Only a cluster that might overflow is measured at all —
// one with two marks or more, or one whose ink reaches its slot's edge — and
// only once while its slot stays resident, so text whose clusters fit pays one
// branch per cluster cell and nothing on any other cell.
//
// Pure arithmetic: no platform headers (Scripts/check-boundaries.py).

namespace Ubiquitty::Rendering {

/// The rows a window's slot carries past the rows its quad draws, on each side.
inline constexpr int clusterWindowGuardRows = 1;

/// The most windows a cluster takes on either side of its own slot.
///
/// Two a side reach two slots — two to three lines — past the cluster's own
/// slot: a dozen marks stacked on a capital. Past that the ink is cut, which is
/// what bounds a pathological stack at five slots of the sheet.
inline constexpr int clusterWindowLimitPerSide = 2;

/// Which side of its own slot a window extends a cluster.
enum class ClusterWindowSide : std::uint8_t {
    /// The rows above the slot, which stacked marks above reach into.
    Above,
    /// The rows below it, which stacked marks below reach into.
    Below,
};

/// How a cluster's own glyph lands on screen.
enum class ClusterOwnQuad : std::uint8_t {
    /// A quad on the subpixel glyph stream, covering the whole slot.
    WholeSlot,
    /// Baked into its cell's quad, which covers the cell box inside the margins.
    CellBox,
};

/// A run of a slot's rows: `top` inclusive, `bottom` exclusive.
struct ClusterSlotRows {
    /// The first row.
    int top = 0;
    /// One past the last row.
    int bottom = 0;
};

/// How many windows a cluster takes on each side of its own slot.
struct ClusterWindowCounts {
    /// Windows above the slot.
    int above = 0;
    /// Windows below it.
    int below = 0;
};

/// How far apart consecutive windows are cut.
///
/// - Parameter slotHeightInPixels: The mono slot's height.
/// - Returns: The slot less a guard row on each side; zero or less for a slot
///   too short to carry a window.
[[nodiscard]] constexpr int clusterWindowPitch(int slotHeightInPixels)
{
    return slotHeightInPixels - 2 * clusterWindowGuardRows;
}

/// The rows of its own slot a cluster's quad draws when no window abuts it.
///
/// - Parameters:
///   - quad: How the cluster's own glyph lands.
///   - slotHeightInPixels: The mono slot's height.
///   - marginInPixels: The slot's vertical margin.
/// - Returns: The whole slot for a glyph-stream quad, the cell box for a baked one.
[[nodiscard]] constexpr ClusterSlotRows clusterOwnRows(ClusterOwnQuad quad, int slotHeightInPixels, int marginInPixels)
{
    return quad == ClusterOwnQuad::WholeSlot ? ClusterSlotRows { 0, slotHeightInPixels }
                                             : ClusterSlotRows { marginInPixels, slotHeightInPixels - marginInPixels };
}

/// The rows of its own slot a cluster's quad draws where windows abut it.
///
/// A glyph-stream quad gives up a guard row to each abutting window, whose
/// quad draws that row from its own slot with the right neighbour beside it.
/// A baked quad is its cell and gives nothing up: the windows meet it at the
/// cell's edges.
///
/// - Parameters:
///   - quad: How the cluster's own glyph lands.
///   - slotHeightInPixels: The mono slot's height.
///   - marginInPixels: The slot's vertical margin.
/// - Returns: The rows drawn when a window abuts each side.
[[nodiscard]] constexpr ClusterSlotRows clusterOwnRowsBesideWindows(ClusterOwnQuad quad, int slotHeightInPixels, int marginInPixels)
{
    return quad == ClusterOwnQuad::WholeSlot ? ClusterSlotRows { clusterWindowGuardRows, slotHeightInPixels - clusterWindowGuardRows }
                                             : clusterOwnRows(quad, slotHeightInPixels, marginInPixels);
}

/// How far past each edge of its slot a measurement must look.
///
/// - Parameter slotHeightInPixels: The mono slot's height.
/// - Returns: Rows enough to count every window up to the limit, and the
///   guard row past the last.
[[nodiscard]] constexpr int clusterWindowReach(int slotHeightInPixels)
{
    return clusterWindowLimitPerSide * std::max(clusterWindowPitch(slotHeightInPixels), 0) + 2 * clusterWindowGuardRows;
}

/// Whether a cluster's own raster suggests it may overflow its slot.
///
/// The cheap test that decides whether to measure at all. Two marks or more
/// can stack past the margin; so can one mark on a tall letter, and that
/// shows as ink at the slot's edge, which the slot's measured ink box already
/// reports. A cluster that passes neither — an `é` — is never measured.
///
/// - Parameters:
///   - codePointCount: How many code points the cluster holds, base included.
///   - ownInk: The guarded ink box measured from the cluster's own slot.
///   - slotHeightInPixels: The mono slot's height.
/// - Returns: Whether the cluster is worth measuring.
[[nodiscard]] constexpr bool clusterMayOverflowItsSlot(int codePointCount, GlyphInkBox ownInk, int slotHeightInPixels)
{
    return codePointCount >= 3 || ownInk.minInPixels.y <= 0.0f || ownInk.maxInPixels.y >= static_cast<float>(slotHeightInPixels);
}

/// Counts the windows a measured cluster's ink needs on each side.
///
/// A side takes windows only when the ink reaches past the rows the cluster's
/// own quad draws there, so a cluster that fits takes none and draws exactly
/// as it did before windows existed. The count covers the ink from the rows
/// the quad keeps beside a window, at the window pitch, up to the limit.
///
/// - Parameters:
///   - ink: The cluster's measured rows, in its own slot's rows.
///   - quad: How the cluster's own glyph lands.
///   - slotHeightInPixels: The mono slot's height.
///   - marginInPixels: The slot's vertical margin.
/// - Returns: The windows above and below; none for a blank cluster.
[[nodiscard]] constexpr ClusterWindowCounts clusterWindowCounts(ClusterInkRows ink, ClusterOwnQuad quad, int slotHeightInPixels, int marginInPixels)
{
    const int pitch = clusterWindowPitch(slotHeightInPixels);
    if (!ink.isInked || ink.bottomInPixels <= ink.topInPixels || pitch <= 0)
        return {};
    const ClusterSlotRows own = clusterOwnRows(quad, slotHeightInPixels, marginInPixels);
    const ClusterSlotRows beside = clusterOwnRowsBesideWindows(quad, slotHeightInPixels, marginInPixels);
    ClusterWindowCounts counts;
    if (ink.topInPixels < own.top)
        counts.above = std::min(clusterWindowLimitPerSide, (beside.top - ink.topInPixels + pitch - 1) / pitch);
    if (ink.bottomInPixels > own.bottom)
        counts.below = std::min(clusterWindowLimitPerSide, (ink.bottomInPixels - beside.bottom + pitch - 1) / pitch);
    return counts;
}

/// The row of the cluster's own slot that a window shows on its top row.
///
/// Window `index` on a side draws the `index`th pitch of rows out from the
/// rows the own quad keeps beside it, and its slot begins a guard row before
/// them. A backend draws the cluster moved up by this many rows.
///
/// - Parameters:
///   - beside: The rows the own quad draws beside windows.
///   - slotHeightInPixels: The mono slot's height.
///   - side: Which side the window extends.
///   - index: The window's place on its side, from one next to the slot.
/// - Returns: The offset; negative above the slot.
[[nodiscard]] constexpr int clusterWindowRowOffset(ClusterSlotRows beside, int slotHeightInPixels, ClusterWindowSide side, int index)
{
    const int pitch = clusterWindowPitch(slotHeightInPixels);
    return side == ClusterWindowSide::Above ? beside.top - index * pitch - clusterWindowGuardRows
                                            : beside.bottom + (index - 1) * pitch - clusterWindowGuardRows;
}

/// The rows of its slot a window's quad draws: the slot less its guard rows.
///
/// - Parameter slotHeightInPixels: The mono slot's height.
/// - Returns: The rows the quad covers when the window's ink fills them.
[[nodiscard]] constexpr ClusterSlotRows clusterWindowRows(int slotHeightInPixels)
{
    return { clusterWindowGuardRows, slotHeightInPixels - clusterWindowGuardRows };
}

/// Narrows a window's rows to the ink its raster measured.
///
/// The measured box carries a guard texel of its own each side, which is the
/// row a linear tap at the narrowed quad's edge reads.
///
/// - Parameters:
///   - rows: The rows the window's quad may draw.
///   - ink: The window slot's measured ink box.
/// - Returns: The rows to draw; empty when the box and the rows do not meet or
///   the window holds no ink, unchanged for an unmeasured box.
[[nodiscard]] constexpr ClusterSlotRows clusterWindowInkRows(ClusterSlotRows rows, GlyphInkBox ink)
{
    if (ink.minInPixels.x > ink.maxInPixels.x)
        return rows;
    if (ink.maxInPixels.y <= ink.minInPixels.y)
        return { rows.top, rows.top };
    const int top = std::max(rows.top, static_cast<int>(ink.minInPixels.y));
    const int bottom = std::min(rows.bottom, static_cast<int>(ink.maxInPixels.y));
    return bottom > top ? ClusterSlotRows { top, bottom } : ClusterSlotRows { rows.top, rows.top };
}

/// The word a window's verified contents carry after the cluster's code points.
///
/// Above U+10FFFF, so no cluster's own contents ever end in it: a window's
/// slot cannot be served for its cluster's own glyph, nor one window for
/// another, whatever their keys fold to.
///
/// - Parameter rowOffsetInPixels: The window's ``clusterWindowRowOffset``.
/// - Returns: The trailing contents word.
[[nodiscard]] constexpr std::uint32_t clusterWindowWord(int rowOffsetInPixels)
{
    return 0xFF000000u | (static_cast<std::uint32_t>(rowOffsetInPixels) & 0x00FFFFFFu);
}

/// Keys one window of a cluster in the mono sheet.
///
/// The cluster keyspace over the window's contents — the code points and
/// ``clusterWindowWord`` — so a window shares its cluster's phase and face
/// lanes, and the verified probe separates it from every other run.
///
/// - Parameters:
///   - contents: The cluster's code points followed by the window word.
///   - count: How many words `contents` holds.
///   - phase: The subpixel phase, 0-7.
///   - face: The bold and italic style.
/// - Returns: The 32-bit key, to widen with the raster variant.
[[nodiscard]] inline std::uint32_t clusterWindowKey(const std::uint32_t* contents, int count, int phase, FontFace face)
{
    return coverageClusterKey(contents, count, phase, face);
}

} // namespace Ubiquitty::Rendering
