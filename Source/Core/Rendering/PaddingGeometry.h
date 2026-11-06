// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Base/Geometry.h"
#include "Base/SwiftInterop.h"
#include "Core/GridBounds.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

// Source/Core/Rendering/PaddingGeometry.h — portable window-padding geometry.
//
// The cell grid sits inside per-edge insets (left/right/top/bottom), painted in
// the background color. An optional extra top inset reserves top-edge space (the
// OSC 9;4 progress bar) without growing the bottom margin. The clamp rules (floor
// at 0, scale a too-large pair down so a non-negative span always remains) live
// here once, shared by every frontend's grid-origin + usable-size resolve.
//
// Unit-agnostic: works in device pixels or points as long as the viewport,
// insets, and extraTop share the same unit. A plain POD seam — no platform / GPU
// / font types (see Scripts/check-boundaries.py).

namespace Ubiquitty::Rendering {

/// Behavioral-mode enum replacing a former boolean-trap parameter
/// (WebKit no-boolean-arguments). First enumerator == old `false`.
enum class ColumnMode : uint8_t {
    Standard,
    Columns132
};

/// The window padding around a grid, per edge, in the caller's unit.
///
/// Points at the frontend and device pixels once scaled; this struct does not
/// say which, because every consumer scales before it arrives and a unit named
/// here would be wrong for the other caller.
struct UBIQUITTY_ESCAPABLE EdgeInsets {
    float left = 0.0f;
    float right = 0.0f;
    float top = 0.0f;
    float bottom = 0.0f;
};

/// Clamps one opposing-edge pair to a viewport axis, in place.
///
/// Each edge floors at zero, and a pair whose sum would consume the whole axis
/// scales down proportionally rather than either edge winning — so a
/// non-negative usable span always remains and the padding stays symmetric
/// about the intent that set it.
///
/// - Parameters:
///   - a: One edge of the pair (left, or top).
///   - b: Its opposite (right, or bottom).
///   - total: The axis the two must fit inside.
constexpr void clampEdgePair(float& a, float& b, float total)
{
    if (a < 0.0f)
        a = 0.0f;
    if (b < 0.0f)
        b = 0.0f;
    const float sum = a + b;
    if (sum > total && sum > 0.0f) {
        const float factor = total / sum;
        a *= factor;
        // Round only the first edge. Independent products can leave a tiny
        // positive or negative content span instead of an empty viewport.
        b = total - a;
    }
}

// Each of the three below stands in for a bool parameter. `pointToCell(point, origin,
// cell, grid, true)` says nothing at the call site about what `true` selects,
// and the two calls that differ only in it read identically — first enumerator
// equals `false`.

/// Whether a cell's height is floored to a whole device pixel.
enum class DevicePixelSnap : std::uint8_t {
    Unsnapped = 0,
    Snapped = 1
};

/// The smallest grid a viewport too small for one cell may report.
///
/// `None` returns the raw floored counts, which may be zero or negative. That
/// is the answer a caller wants when it would rather keep its previous grid
/// than draw a degenerate one — a clamped 1x1 is indistinguishable from a
/// window that genuinely holds one cell, so the case cannot be detected after
/// the fact.
enum class GridFloor : std::uint8_t {
    None = 0,
    OneCell = 1
};

/// Whether a point above or below the grid reports a row outside it.
///
/// `Unclamped` is what lets a drag past an edge read as an out-of-range row,
/// which is how the caller knows to auto-scroll rather than to stop.
enum class RowClamp : std::uint8_t {
    Unclamped = 0,
    Clamped = 1
};

/// Returns the usable grid box inside a viewport.
///
/// `extraTop` is added to the TOP inset only — it is chrome above the grid, not
/// padding around it — and the height is clamped so the box never goes negative
/// when the chrome exceeds the window.
///
/// - Parameters:
///   - viewport: The window's pixel extent.
///   - insets: Padding on each edge.
///   - extraTop: Additional space reserved above the grid.
/// - Returns: The origin and extent the grid may draw in.
[[nodiscard]] constexpr FloatRect resolveGridContentRect(FloatSize viewport, EdgeInsets insets, float extraTop)
{
    clampEdgePair(insets.left, insets.right, viewport.width);
    clampEdgePair(insets.top, insets.bottom, viewport.height);
    if (extraTop < 0.0f)
        extraTop = 0.0f;
    const float maxExtra = viewport.height - insets.top - insets.bottom;
    if (extraTop > maxExtra)
        extraTop = (maxExtra > 0.0f) ? maxExtra : 0.0f;

    return FloatRect {
        { insets.left, insets.top + extraTop },
        { viewport.width - insets.left - insets.right,
            viewport.height - insets.top - insets.bottom - extraTop }
    };
}

// Padding-geometry companions.
//
// The structs above describe where the grid sits; the functions below give the
// cell size that fills it and the forward/inverse maps between window pixels and
// cell coordinates. Every function is pure and unit-agnostic — pass everything in
// the same unit (device pixels or points). No platform / GPU / font types.

// {width,height} result for the cell-size resolver below.

/// Returns the per-cell pixel size that fills `content` for the given grid.
///
/// Cells draw at their NATURAL font advance — the grid does not stretch, and
/// slack becomes right and bottom margin — with two corrections. DECCOLM-132
/// compresses the cell width to fit when the natural advance would overflow the
/// window, and `Snapped` floors the cell HEIGHT to a whole device
/// pixel so every row lands on a crisp boundary.
///
/// A natural advance of zero or less means unavailable, and the cell then
/// stretches to fill instead.
///
/// - Parameters:
///   - content: The box the grid fills.
///   - grid: The column and row counts to divide it by.
///   - naturalAdvance: The font's own advance and line height, already in the
///     working unit; zero on an axis falls back to stretch-to-fill.
///   - columnMode: Whether DECCOLM has forced 132 columns.
///   - snapToDevicePixels: Whether to floor the height to a device pixel.
///   - backingScale: Accepted so a point-unit caller can document its unit; the
///     advance is already scaled, so this is unused. Pass 1 when already in
///     pixels.
/// - Returns: The cell extent to lay out with.
[[nodiscard]] constexpr FloatSize resolveCellSize(FloatRect content, GridSize grid, FloatSize naturalAdvance, ColumnMode columnMode, DevicePixelSnap snap, float backingScale)
{
    (void)backingScale; // natural advance is already in the working unit.
    FloatSize cell;
    cell.width = (naturalAdvance.width > 0.0f) ? naturalAdvance.width : ((grid.columns > 0) ? content.size.width / static_cast<float>(grid.columns) : 1.0f);
    // DECCOLM-132: squeeze the forced 132 columns into the fixed window.
    if (columnMode == ColumnMode::Columns132 && grid.columns > 0 && naturalAdvance.width > 0.0f) {
        const float fitWidth = content.size.width / static_cast<float>(grid.columns);
        if (fitWidth < cell.width)
            cell.width = fitWidth;
    }
    cell.height = (naturalAdvance.height > 0.0f) ? naturalAdvance.height : ((grid.rows > 0) ? content.size.height / static_cast<float>(grid.rows) : 1.0f);
    if (snap == DevicePixelSnap::Snapped && grid.rows > 0) {
        // Truncation toward zero, not floorf: a cell height is never negative,
        // and the cast is usable in a constant expression where floorf is not.
        cell.height = static_cast<float>(static_cast<long>(cell.height));
        if (cell.height < 1.0f)
            cell.height = 1.0f;
    }
    return cell;
}

/// The grid's box inside a padded image, in pixels.
struct UBIQUITTY_ESCAPABLE GridBox {
    /// Where the grid's left edge sits.
    int left = 0;
    /// Where the grid's top edge sits.
    int top = 0;
    /// How wide the grid is.
    int width = 0;
    /// How tall the grid is.
    int height = 0;
};

/// Paints the region of an image outside its grid box with one color.
///
/// The complement of a composite that drew into the grid box: what a renderer
/// given an origin and a larger viewport leaves untouched is exactly the
/// border, so the border is all that remains to be filled. Filling the whole
/// image and drawing the grid over it writes most of the frame twice.
///
/// - Parameters:
///   - rgba: The image, four bytes per pixel, at least `width * height * 4` long.
///   - width: The image's width in pixels.
///   - height: The image's height in pixels.
///   - grid: The box to leave alone, clamped here to the image.
///   - background: The color to paint, as RGBA bytes.
inline void fillImageBorder(std::vector<std::uint8_t>& rgba, int width, int height, GridBox grid, std::array<std::uint8_t, 4> background)
{
    if (width <= 0 || height <= 0 || rgba.size() < static_cast<std::size_t>(width) * height * 4)
        return;
    const int gridLeft = std::clamp(grid.left, 0, width);
    const int gridTop = std::clamp(grid.top, 0, height);
    const int gridRight = std::clamp(grid.left + grid.width, gridLeft, width);
    const int gridBottom = std::clamp(grid.top + grid.height, gridTop, height);

    const auto paintSpan = [&](int y, int firstColumn, int lastColumn) {
        std::uint8_t* out = rgba.data() + (static_cast<std::size_t>(y) * width + firstColumn) * 4;
        for (int x = firstColumn; x < lastColumn; ++x, out += 4)
            std::copy_n(background.data(), background.size(), out);
    };
    for (int y = 0; y < gridTop; ++y)
        paintSpan(y, 0, width);
    for (int y = gridBottom; y < height; ++y)
        paintSpan(y, 0, width);
    for (int y = gridTop; y < gridBottom; ++y) {
        paintSpan(y, 0, gridLeft);
        paintSpan(y, gridRight, width);
    }
}

/// A frame's grid box and the cell that fills it.
struct UBIQUITTY_ESCAPABLE FrameGeometry {
    /// The box the grid draws in, in the caller's working unit.
    FloatRect content;
    /// One cell's extent, resolved against that same box.
    FloatSize cell;
};

/// Returns where a frame's grid sits and how large one of its cells is.
///
/// The two halves are separately useful and jointly a contract: a cell resolved
/// against a box the caller does not then draw in scales every glyph by the
/// ratio between them. Composing them here is what makes a mismatched pair
/// unspellable rather than something each call site has to get right.
///
/// - Parameters:
///   - viewport: The window's extent, in the working unit.
///   - insets: Padding on each edge.
///   - extraTop: Chrome reserved above the grid.
///   - grid: The column and row counts.
///   - naturalAdvance: The font's advance and line height, already in the
///     working unit.
///   - columnMode: Whether DECCOLM has forced 132 columns.
///   - snap: Whether to floor the row height to a whole device pixel.
///   - backingScale: The caller's unit marker; the advance is already scaled.
/// - Returns: The grid box and the cell resolved against it.
[[nodiscard]] constexpr FrameGeometry resolveFrameGeometry(FloatSize viewport, EdgeInsets insets, float extraTop,
    GridSize grid, FloatSize naturalAdvance, ColumnMode columnMode, DevicePixelSnap snap, float backingScale)
{
    const FloatRect content = resolveGridContentRect(viewport, insets, extraTop);
    return FrameGeometry { content, resolveCellSize(content, grid, naturalAdvance, columnMode, snap, backingScale) };
}

// The geometry a frame is laid out by, pinned where a change is a compile error
// rather than a pixel nobody diffed. Every value below is reachable from a
// shipping frame, and each names a correction the resolver exists to make.
namespace Pinned {
// A 100x50 box with 10pt padding all round, no chrome, an 8x16 face.
inline constexpr FrameGeometry padded = resolveFrameGeometry(FloatSize { 100.0f, 50.0f },
    EdgeInsets { 10.0f, 10.0f, 10.0f, 10.0f }, 0.0f, GridSize { 10, 2 },
    FloatSize { 8.0f, 16.0f }, ColumnMode::Standard, DevicePixelSnap::Unsnapped, 1.0f);
static_assert(padded.content.origin.x == 10.0f && padded.content.origin.y == 10.0f);
static_assert(padded.content.size.width == 80.0f && padded.content.size.height == 30.0f);
// Slack becomes margin: the cell keeps its natural advance rather than growing
// to fill the box, which is what stops a short grid from stretching its type.
static_assert(padded.cell.width == 8.0f && padded.cell.height == 16.0f);

// Chrome above the grid moves the origin down and shortens the box; it is not
// padding and does not apply to the bottom edge.
inline constexpr FrameGeometry chromed = resolveFrameGeometry(FloatSize { 100.0f, 50.0f },
    EdgeInsets { 0.0f, 0.0f, 0.0f, 0.0f }, 12.0f, GridSize { 10, 2 },
    FloatSize { 8.0f, 16.0f }, ColumnMode::Standard, DevicePixelSnap::Unsnapped, 1.0f);
static_assert(chromed.content.origin.y == 12.0f && chromed.content.size.height == 38.0f);

// Padding that would consume the axis scales down proportionally, so the box
// stays non-negative rather than inverting. 75 and 25 against 50 halve exactly,
// so the assert rests on the clamp and not on a float that rounds back.
inline constexpr FrameGeometry crushed = resolveFrameGeometry(FloatSize { 50.0f, 50.0f },
    EdgeInsets { 75.0f, 25.0f, 0.0f, 0.0f }, 0.0f, GridSize { 10, 2 },
    FloatSize { 8.0f, 16.0f }, ColumnMode::Standard, DevicePixelSnap::Unsnapped, 1.0f);
static_assert(crushed.content.origin.x == 37.5f);
static_assert(crushed.content.size.width == 0.0f);

// DECCOLM-132 compresses the advance to fit instead of clipping the columns
// that do not. 132 cells of 8pt need 1056pt; the window offers 132.
inline constexpr FrameGeometry columns132 = resolveFrameGeometry(FloatSize { 132.0f, 50.0f },
    EdgeInsets {}, 0.0f, GridSize { 132, 2 }, FloatSize { 8.0f, 16.0f },
    ColumnMode::Columns132, DevicePixelSnap::Unsnapped, 1.0f);
static_assert(columns132.cell.width == 1.0f);
// The same window in standard mode keeps the natural advance and lets the grid
// overflow: the compression belongs to DECCOLM, not to the window being small.
inline constexpr FrameGeometry overflowing = resolveFrameGeometry(FloatSize { 132.0f, 50.0f },
    EdgeInsets {}, 0.0f, GridSize { 132, 2 }, FloatSize { 8.0f, 16.0f },
    ColumnMode::Standard, DevicePixelSnap::Unsnapped, 1.0f);
static_assert(overflowing.cell.width == 8.0f);

// A snapped row floors to a whole device pixel, and never below one.
inline constexpr FrameGeometry snapped = resolveFrameGeometry(FloatSize { 100.0f, 50.0f },
    EdgeInsets {}, 0.0f, GridSize { 10, 2 }, FloatSize { 8.0f, 16.7f },
    ColumnMode::Standard, DevicePixelSnap::Snapped, 1.0f);
static_assert(snapped.cell.height == 16.0f);
inline constexpr FrameGeometry snappedTiny = resolveFrameGeometry(FloatSize { 100.0f, 50.0f },
    EdgeInsets {}, 0.0f, GridSize { 10, 2 }, FloatSize { 8.0f, 0.4f },
    ColumnMode::Standard, DevicePixelSnap::Snapped, 1.0f);
static_assert(snappedTiny.cell.height == 1.0f);

// The composition is the two halves, so a caller that keeps calling them apart
// gets the same answer and the pair cannot drift.
static_assert(padded.content.origin.x == resolveGridContentRect(FloatSize { 100.0f, 50.0f }, EdgeInsets { 10.0f, 10.0f, 10.0f, 10.0f }, 0.0f).origin.x);
} // namespace Pinned

// Floor `value` to an int the way static_cast<int> does for the non-negative
// offsets these maps produce, but correctly toward negative infinity for the
// signed-row (`RowClamp::Unclamped`, the auto-scroll case) where a point above the grid
// yields a negative row. Local helper; not part of the public surface.
inline int floorToInt(float value)
{
    const int truncated = static_cast<int>(value);
    return (value < 0.0f && static_cast<float>(truncated) != value) ? truncated - 1 : truncated;
}

/// Returns the column and row counts a viewport can hold.
///
/// `OneCell` floors both axes at 1, so a window too small for a
/// single cell still has a grid. A caller that would rather keep its previous
/// grid in that case passes false and gets the raw floored counts, which may be
/// zero or negative, and applies its own gate.
///
/// - Parameters:
///   - viewport: The window's pixel extent.
///   - insets: Padding on each edge.
///   - reservedHeight: Vertical space reserved for chrome, above and below.
///   - cell: The cell extent to divide by.
///   - gridFloor: The smallest grid to report when the viewport cannot hold a cell.
/// - Returns: The grid that fits.
inline GridSize gridFromViewport(FloatSize viewport, EdgeInsets insets, float reservedHeight, FloatSize cell, GridFloor gridFloor = GridFloor::OneCell)
{
    GridSize dims;
    const float usableWidth = viewport.width - insets.left - insets.right;
    const float usableHeight = viewport.height - reservedHeight;
    dims.columns = (cell.width > 0.0f) ? floorToInt(usableWidth / cell.width) : 1;
    dims.rows = (cell.height > 0.0f) ? floorToInt(usableHeight / cell.height) : 1;
    if (gridFloor == GridFloor::OneCell) {
        if (dims.columns < 1)
            dims.columns = 1;
        if (dims.rows < 1)
            dims.rows = 1;
    }
    return dims;
}

/// The vertical extent a viewport must not divide into rows.
///
/// The padding on both edges, plus whatever chrome stands above the grid — the
/// OSC 9;4 progress bar's inset. Separate from ``gridFromViewport``'s insets
/// because that fit consults `left` and `right` alone: the horizontal padding
/// is symmetric and always fits, while the vertical reserve competes with the
/// rows for a height a window can be shorter than.
///
/// The extra is clamped into what the padding leaves and never runs negative,
/// so a window too short for its own chrome still reports a reserve the fit
/// subtracts rather than a height it hands back.
///
/// - Parameters:
///   - insets: The padding on each edge; `top` and `bottom` are read.
///   - extraTop: Chrome above the grid, in the same units.
///   - contentHeight: The box being divided.
/// - Returns: The height to reserve.
[[nodiscard]] inline float gridVerticalReserve(EdgeInsets insets, float extraTop, float contentHeight)
{
    const float padding = insets.top + insets.bottom;
    if (extraTop < 0.0f)
        extraTop = 0.0f;
    const float roomForExtra = contentHeight - padding;
    if (extraTop > roomForExtra)
        extraTop = roomForExtra > 0.0f ? roomForExtra : 0.0f;
    return padding + extraTop;
}

/// What a frontend knows about the box it is fitting a terminal grid into.
struct UBIQUITTY_ESCAPABLE GridFitRequest {
    /// The box, in the units the cell is measured in.
    FloatSize viewport {};
    /// The padding on each edge, chrome excluded.
    EdgeInsets insets {};
    /// Chrome standing above the grid, in the same units.
    float extraTop = 0.0f;
    /// The cell to divide by.
    FloatSize cell {};
    /// The column count to keep while ``isColumnCountHeld``.
    int heldColumns = 0;
    /// Whether the application owns the column count — DECCOLM's 132-column
    /// mode. The cells stretch to the new width instead.
    bool isColumnCountHeld = false;
};

/// Whether a viewport has been laid out, so that a fit of it names a grid.
///
/// A view that has not been given a frame yet — a pane re-parented into a
/// container whose own frame comes later — has an empty box. Its fit is the
/// grid floor, and a thin client that reported that floor would resize the
/// host's session to it; every window mirroring the session then follows the
/// host down to two columns. A box with no extent asks for nothing.
///
/// - Parameter viewport: The box a frontend is about to fit.
/// - Returns: Whether the box has width and height.
[[nodiscard]] inline bool isViewportLaidOut(FloatSize viewport)
{
    return viewport.width > 0.0f && viewport.height > 0.0f;
}

/// Returns the grid a frontend should drive its terminal to for a viewport.
///
/// The whole fit, so the arms cannot each assemble a different one: the
/// vertical reserve above, the division, the 132-column hold, and the clamp
/// into what the grid can actually hold.
///
/// The clamp is what makes the answer a fact rather than a wish. A resize
/// clamps into `[minRows, maxRows]` on the way in (GridBounds.h), so a
/// frontend that laid its chrome out against the count it ASKED for lays it
/// out against a grid the terminal does not have — every geometry downstream
/// then disagrees with no error raised anywhere.
///
/// - Parameter request: The box, the cell and the application's claim on it.
/// - Returns: The grid, floored and ceilinged the way a resize represents it.
[[nodiscard]] inline GridSize fitGridToViewport(const GridFitRequest& request)
{
    const GridSize fit = gridFromViewport(request.viewport, request.insets,
        gridVerticalReserve(request.insets, request.extraTop, request.viewport.height), request.cell);
    GridSize grid;
    grid.columns = clampGridColumns(request.isColumnCountHeld ? request.heldColumns : fit.columns);
    grid.rows = clampGridRows(fit.rows);
    return grid;
}

/// Returns the viewport extent a grid needs — the inverse of ``gridFromViewport``.
///
/// - Parameters:
///   - grid: The column and row counts to size for.
///   - cell: The cell extent.
///   - insets: Padding on each edge.
///   - extraTop: Additional space reserved above the grid.
/// - Returns: The window extent that holds exactly this grid.
inline FloatSize viewportForGrid(GridSize grid, FloatSize cell, EdgeInsets insets, float extraTop)
{
    FloatSize extent;
    extent.width = static_cast<float>(grid.columns) * cell.width + insets.left + insets.right;
    extent.height = static_cast<float>(grid.rows) * cell.height + insets.top + insets.bottom + extraTop;
    return extent;
}

/// What a settings change that moves the cell does to the window holding the
/// grid.
enum class CellMetricChangeResponse : std::uint8_t {
    /// The terminal chose the window's size, so the window follows the font:
    /// it resizes so the pre-change columns and rows render at the new cell.
    WindowFollowsGrid,
    /// The window manager chose the size — or a split has no single grid to
    /// preserve — so the window holds and the grid reflows into it, the
    /// sub-cell remainder letterboxed.
    GridReflowsInWindow,
};

/// The geometry a shell records ahead of a settings change, while the cell is
/// still the old one, so the change can be classified against what the window
/// was. Facts only: the
/// verdict is ``classifyCellMetricChange``'s, and both macOS shells hand it
/// the same facts so a font change answers the same way on each.
struct UBIQUITTY_ESCAPABLE CellMetricChangeSnapshot {
    bool isValid = false;
    /// Fullscreen, zoomed, tiled: the window manager forced the size.
    bool isPinned = false;
    /// A split's content is panes plus dividers, not one grid.
    bool isSplit = false;
    /// The grid before the change — what a following window preserves.
    GridSize grid {};
    /// The window's content extent before the change, in points.
    FloatSize content {};
    /// The cell before the change, in points.
    FloatSize cell {};
    EdgeInsets insets {};
    float extraTop = 0.0f;
};

/// How far a window's content may sit from an exact cell multiple and still
/// count as the terminal's own size. AppKit aligns a frame to the backing
/// grid and a width is not pixel-snapped, so a free-floating window is off by
/// a point or so; a window-manager-sized one is off by a sub-cell remainder,
/// a meaningful fraction of a cell. Two points absorbs the rounding and stays
/// under any cell — and this emptiness is a signal that holds on every
/// backend, which is what catches a snap to another window too.
inline constexpr float cellMultipleTolerancePoints = 2.0f;

/// Whether `content` is the extent `grid` occupies at `cell`, to the tolerance.
/// Whether one extent sits within the tolerance of another.
///
/// - Parameter offset: The signed difference between two extents, in points.
/// - Returns: Whether the difference reads as alignment rather than as a gap.
[[nodiscard]] inline bool extentIsWithinCellMultipleTolerance(float offset)
{
    return std::fabs(offset) <= cellMultipleTolerancePoints;
}

/// Whether `content` is the extent `grid` occupies at `cell`, to the tolerance.
///
/// Both axes must agree: a window matching on one alone is a window the grid
/// does not describe.
[[nodiscard]] inline bool isCleanCellMultiple(FloatSize content, GridSize grid, FloatSize cell, EdgeInsets insets, float extraTop)
{
    const FloatSize want = viewportForGrid(grid, cell, insets, extraTop);
    return extentIsWithinCellMultipleTolerance(content.width - want.width)
        && extentIsWithinCellMultipleTolerance(content.height - want.height);
}

/// Whether the snapshot describes a window at all.
///
/// A shell with no cell yet records nothing, and sizing a window from that
/// would be sizing it from zeroes.
///
/// - Parameter snapshot: The pre-change facts.
/// - Returns: Whether there is a window to classify.
[[nodiscard]] inline bool snapshotDescribesAWindow(const CellMetricChangeSnapshot& snapshot)
{
    return snapshot.isValid;
}

/// Whether something other than the grid chose this window's size.
///
/// Full screen, zoomed or tiled means the window manager picked the extent;
/// a split means several panes share it and no single grid owns it. Either
/// way the grid is the follower, so the frame is what holds.
///
/// - Parameter snapshot: The pre-change facts.
/// - Returns: Whether the size belongs to someone other than the grid.
[[nodiscard]] inline bool windowSizeIsOwnedBySomethingOtherThanTheGrid(const CellMetricChangeSnapshot& snapshot)
{
    return snapshot.isPinned || snapshot.isSplit;
}

/// Whether the cell is too degenerate to measure a grid against.
///
/// A zero or negative cell gives the grid no extent, so the comparison would
/// be answering about arithmetic rather than about fit.
///
/// - Parameter snapshot: The pre-change facts.
/// - Returns: Whether the cell has no usable extent.
[[nodiscard]] inline bool cellHasNoUsableExtent(const CellMetricChangeSnapshot& snapshot)
{
    return snapshot.cell.width <= 0.0f || snapshot.cell.height <= 0.0f;
}

/// Classifies a cell-metric change from the geometry snapshotted before it.
///
/// - Parameter snapshot: The pre-change facts.
/// - Returns: Whether the window follows the grid or the grid reflows.
[[nodiscard]] inline CellMetricChangeResponse classifyCellMetricChange(const CellMetricChangeSnapshot& snapshot)
{
    if (!snapshotDescribesAWindow(snapshot) || windowSizeIsOwnedBySomethingOtherThanTheGrid(snapshot))
        return CellMetricChangeResponse::GridReflowsInWindow;
    if (cellHasNoUsableExtent(snapshot))
        return CellMetricChangeResponse::GridReflowsInWindow;
    return isCleanCellMultiple(snapshot.content, snapshot.grid, snapshot.cell, snapshot.insets, snapshot.extraTop)
        ? CellMetricChangeResponse::WindowFollowsGrid
        : CellMetricChangeResponse::GridReflowsInWindow;
}

/// Which way a toolkit's window coordinates grow, so "the top edge" can be
/// named: AppKit's grow upward and the top is `maxY`; a y-down toolkit's top
/// is `minY`.
enum class WindowYAxis : std::uint8_t {
    Up,
    Down,
};

/// The frame a window takes to hold `size` while keeping its top-left corner
/// where it was, nudged so the whole of it stays inside `visible`.
///
/// - Parameters:
///   - frame: The window's frame now.
///   - size: The frame extent wanted.
///   - visible: The screen area a window may occupy.
///   - axis: Which way y grows in these coordinates.
/// - Returns: The frame to apply.
[[nodiscard]] inline FloatRect anchoredWindowFrame(FloatRect frame, FloatSize size, FloatRect visible, WindowYAxis axis)
{
    FloatRect target;
    target.size = size;
    target.origin.x = frame.origin.x;
    target.origin.y = axis == WindowYAxis::Up ? frame.origin.y + frame.size.height - size.height : frame.origin.y;
    if (target.origin.x + target.size.width > visible.origin.x + visible.size.width)
        target.origin.x = visible.origin.x + visible.size.width - target.size.width;
    if (target.origin.x < visible.origin.x)
        target.origin.x = visible.origin.x;
    if (target.origin.y + target.size.height > visible.origin.y + visible.size.height)
        target.origin.y = visible.origin.y + visible.size.height - target.size.height;
    if (target.origin.y < visible.origin.y)
        target.origin.y = visible.origin.y;
    return target;
}

/// Maps a window point to the zero-based cell under it.
///
/// Subtract the grid origin, divide by the cell, floor. The column always
/// clamps into the grid; the row clamps only under `RowClamp::Clamped`, because a
/// drag past the top or bottom edge must be allowed to read as a row OUTSIDE
/// the grid — that is what tells the caller to auto-scroll rather than to stop
/// at the last row.
///
/// - Parameters:
///   - point: The window point, in pixels with a top-left origin.
///   - gridOrigin: The grid's top-left in the same space.
///   - cell: The cell extent.
///   - grid: The column and row counts to clamp against.
///   - rowClamp: Whether to clamp the row into the grid.
/// - Returns: The cell the point falls in.
inline CellPosition pointToCell(FloatPoint point, FloatPoint gridOrigin, FloatSize cell, GridSize grid, RowClamp rowClamp)
{
    CellPosition coord;
    const float xInGrid = point.x - gridOrigin.x;
    const float yInGrid = point.y - gridOrigin.y;
    coord.column = (cell.width > 0.0f) ? floorToInt(xInGrid / cell.width) : 0;
    coord.row = (cell.height > 0.0f) ? floorToInt(yInGrid / cell.height) : 0;
    if (coord.column < 0)
        coord.column = 0;
    if (grid.columns > 0 && coord.column > grid.columns - 1)
        coord.column = grid.columns - 1;
    if (rowClamp == RowClamp::Clamped) {
        if (coord.row < 0)
            coord.row = 0;
        if (grid.rows > 0 && coord.row > grid.rows - 1)
            coord.row = grid.rows - 1;
    }
    return coord;
}

/// A text caret: a row of the grid, and a cell BOUNDARY on it.
struct CaretPosition {
    int row = 0;
    /// 0..columns. One more position than there are cells, because a caret
    /// sits between characters rather than on one.
    int caret = 0;
};

/// Maps a window point to the caret nearest it.
///
/// The difference from ``pointToCell`` is the horizontal rounding, and it is
/// the whole reason this exists: a cell is the one the point falls inside,
/// found by flooring, while a caret is the nearest boundary, found by rounding.
/// Crossing a cell's midpoint moves the caret, so a single character can be
/// selected and half a cell of travel picks up the character under the pointer
/// — the behaviour a reader knows from every text field.
///
/// Both axes clamp into the grid: a caret drag reads its out-of-grid rows from
/// ``pointToCell`` with `RowClamp::Unclamped` instead, because the two want
/// opposite answers there — the walk past an edge needs to know how far out the
/// pointer is, and the selection wants the furthest row the reader can have
/// meant.
///
/// - Parameters:
///   - point: The window point, in pixels with a top-left origin.
///   - gridOrigin: The grid's top-left in the same space.
///   - cell: The cell extent.
///   - grid: The column and row counts to clamp against.
/// - Returns: The caret the point is nearest.
inline CaretPosition pointToCaret(FloatPoint point, FloatPoint gridOrigin, FloatSize cell, GridSize grid)
{
    CaretPosition position;
    const float xInGrid = point.x - gridOrigin.x;
    const float yInGrid = point.y - gridOrigin.y;
    position.row = (cell.height > 0.0f) ? floorToInt(yInGrid / cell.height) : 0;
    // Nearest boundary rather than the cell it falls in: floor(v + 0.5).
    position.caret = (cell.width > 0.0f) ? floorToInt(xInGrid / cell.width + 0.5f) : 0;
    if (position.row < 0)
        position.row = 0;
    if (grid.rows > 0 && position.row > grid.rows - 1)
        position.row = grid.rows - 1;
    if (position.caret < 0)
        position.caret = 0;
    if (grid.columns > 0 && position.caret > grid.columns)
        position.caret = grid.columns;
    return position;
}

/// Maps a window point to a 1-based device pixel, for DECSET 1016 reports.
///
/// Reported in TRUE backing pixels — the space the grid is actually DRAWN in,
/// off the same contents scale the renderer paints with — and not the integer
/// cell-pixel grid, which rounds differently. Clamped into the published text
/// area so a drag past the right or bottom edge still reports a pixel inside it.
///
/// - Parameters:
///   - point: The window point, in pixels with a top-left origin.
///   - gridOrigin: The grid's top-left in the same space.
///   - scale: Points to backing pixels.
///   - textAreaPixels: The published whole-area extent to clamp within.
/// - Returns: The 1-based device pixel to report.
inline IntPoint pointToDevicePixel(FloatPoint point, FloatPoint gridOrigin, float scale, IntSize textAreaPixels)
{
    IntPoint px;
    px.x = floorToInt((point.x - gridOrigin.x) * scale) + 1;
    px.y = floorToInt((point.y - gridOrigin.y) * scale) + 1;
    if (px.x < 1)
        px.x = 1;
    if (px.y < 1)
        px.y = 1;
    if (textAreaPixels.width > 0 && px.x > textAreaPixels.width)
        px.x = textAreaPixels.width;
    if (textAreaPixels.height > 0 && px.y > textAreaPixels.height)
        px.y = textAreaPixels.height;
    return px;
}

/// Where a DECSET 1016 report measures from, as the frontend read the mode.
enum class ReportOrigin : std::uint8_t {
    /// The padded grid's top-left corner: the text's own space.
    Grid,
    /// The drawable's corner: under DECSET 7402 a fitted image fills the
    /// drawable from there, and the report describes the picture's space.
    Drawable,
};

/// The 1-based device pixel a DECSET 1016 report names for a window point,
/// from the frontend's facts: the point, where its grid starts, the scale it
/// draws at, and which origin the mode calls for.
///
/// One function for both arms, so the origin under DECSET 7402 is decided
/// once: measured against the padded grid there, a report lands every pixel
/// one padding-width past the picture the image protocol fitted to the
/// drawable's corner. The clamp to the advertised extent is the pointer
/// ladder's, against what CSI 14 t answers, so no extent is taken here — a
/// frontend clamping to the grid it fitted reports, under the mode, a space
/// smaller than the one the picture fills.
///
/// - Parameters:
///   - point: The window point, in points with a top-left origin.
///   - gridOrigin: The padded grid's top-left in the same space.
///   - scale: Points to backing pixels, the scale the frame is drawn at.
///   - origin: Which corner the report measures from.
/// - Returns: The 1-based device pixel to report, unclamped on the far edges.
inline IntPoint reportedDevicePixel(FloatPoint point, FloatPoint gridOrigin, float scale, ReportOrigin origin)
{
    const FloatPoint from = origin == ReportOrigin::Drawable ? FloatPoint { 0.0f, 0.0f } : gridOrigin;
    return pointToDevicePixel(point, from, scale, IntSize { 0, 0 });
}

/// Returns the pixel rect of one cell, measured from the top left.
///
/// For cursor placement, IME anchoring and overlays. The caller applies any
/// y-flip — Cocoa's bottom-left origin — itself, because only the caller knows
/// which space it is drawing into.
///
/// - Parameters:
///   - at: The cell to locate.
///   - cell: The cell extent.
///   - insets: Padding on each edge.
///   - extraTop: Additional space reserved above the grid.
/// - Returns: The cell's box in window pixels.
inline FloatRect cellRect(CellPosition at, FloatSize cell, EdgeInsets insets, float extraTop)
{
    return FloatRect {
        { insets.left + static_cast<float>(at.column) * cell.width,
            insets.top + extraTop + static_cast<float>(at.row) * cell.height },
        { cell.width, cell.height }
    };
}

/// Returns the offscreen texture extent for a grid — ``resolveGridContentRect``
/// inverted, then lifted to device pixels.
///
/// Each axis floors at 1 and clamps at 16384, so a pathological geometry cannot
/// ask for a texture wider than the GPU will allocate; the allocation would fail
/// at a layer that cannot explain why.
///
/// - Parameters:
///   - grid: The column and row counts.
///   - cell: The cell extent, in the pre-scale unit.
///   - paddingSum: Total padding per axis, in the pre-scale unit.
///   - scale: Points to device pixels.
/// - Returns: The texture extent to allocate.
inline IntSize surfaceSizeForGrid(GridSize grid, FloatSize cell, FloatSize paddingSum, float scale)
{
    IntSize size;
    const float w = (static_cast<float>(grid.columns) * cell.width + paddingSum.width) * scale;
    const float h = (static_cast<float>(grid.rows) * cell.height + paddingSum.height) * scale;
    // lround-equivalent: add 0.5 and floor (inputs here are non-negative).
    size.width = static_cast<int>(w + 0.5f);
    size.height = static_cast<int>(h + 0.5f);
    if (size.width < 1)
        size.width = 1;
    if (size.height < 1)
        size.height = 1;
    constexpr int maxDimension = 16384;
    if (size.width > maxDimension)
        size.width = maxDimension;
    if (size.height > maxDimension)
        size.height = maxDimension;
    return size;
}

} // namespace Ubiquitty::Rendering
