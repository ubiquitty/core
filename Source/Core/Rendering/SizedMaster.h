// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/ClusterPool.h"
#include "Core/Rendering/GlyphBackendTypes.h"
#include "Core/Rendering/GlyphKey.h"
#include "Core/Rendering/GlyphRasterizer.h"
#include "Core/Rendering/SubpixelPhase.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <span>
#include <vector>

// The sized-master decision every arm that cuts sized windows shares: the
// software renderer (portable, Linux's CPU adapter and the browser), the Vulkan
// atlas and Apple's glyph atlas. One selection, one placement and one cut, so a
// window is the same bytes on each of them. A platform keeps only what is its
// own: drawing the master, at the size and with the shift the placement names.
//
// A sized window is one cell-shaped piece of a glyph drawn larger than the
// cell: an OSC 66 block at its scale, a DECDHL half of the glyph at twice the
// height, or a DECDWL cell drawn twice as wide. The master is drawn at the
// target device size, never magnified from the 1x raster.

namespace Ubiquitty::Rendering {

/// The `wdth` coordinate a DECDWL row draws its face at.
///
/// Twice where the face stands, as far as the axis reaches: Monaspace Neon at
/// 100 widens to its maximum 125, a face already at 60 widens to 120. The
/// doubling the axis does not reach is a horizontal stretch.
///
/// - Parameter room: Where the axis stands and how far it reaches.
/// - Returns: The coordinate, or zero when the face cannot widen.
[[nodiscard]] constexpr double doubleWidthAxisTarget(WidthAxisRoom room)
{
    if (!room.canWiden())
        return 0.0;
    return std::min(room.maximum, 2.0 * room.current);
}

/// Selects the master a sized tile's windows are cut from.
///
/// The em factor is the tile's raster scale. A tile doubled across draws the
/// face widened on its `wdth` axis toward twice where it stands, and only the
/// remainder of the doubling — `2 * baseAdvance / widenedAdvance` — is a
/// stretch of the outline, so stems keep the weight the axis gives them. A face
/// with no room, or an advance that did not grow, stretches by the whole two.
///
/// - Parameters:
///   - tile: The window's block: scale, fraction and doubling.
///   - room: The face's `wdth` axis, from the rasterizer.
///   - baseAdvance: The cell advance at the axis' current position.
///   - widenedAdvance: The cell advance at ``doubleWidthAxisTarget`` of `room`.
/// - Returns: The master.
[[nodiscard]] constexpr SizedMaster selectSizedMaster(SizedTile tile, WidthAxisRoom room, double baseAdvance, double widenedAdvance)
{
    SizedMaster master;
    const int scale = tile.scale > 0 ? tile.scale : 1;
    master.scale = tile.isFractional() ? RasterScale { scale * tile.fractionNumerator, tile.fractionDenominator } : RasterScale { scale };
    master.widthScale = tile.widthScale == 2 ? 2 : 1;
    if (master.widthScale == 2) {
        const double target = doubleWidthAxisTarget(room);
        if (target > 0.0 && baseAdvance > 0.0 && widenedAdvance > baseAdvance) {
            master.widthAxisCoordinate = target;
            master.horizontalScale = 2.0 * baseAdvance / widenedAdvance;
        } else {
            master.horizontalScale = 2.0;
        }
    }
    return master;
}

/// The pen positions per device pixel a master's glyph is placed on.
///
/// Fine enough that the quantisation is below what a window shows: a fraction of
/// 1/64 pixel is the unit FreeType's outlines move in, and a CoreText draw
/// snaps to its own coarser grid after this one.
inline constexpr int sizedMasterPenGrid = 64;

/// Where a master's pen stands in its slot.
///
/// The glyph's advance centred in the slot, as the 1x glyph is centred in its
/// cell, then moved by the master's own shift, at the nearest of
/// ``sizedMasterPenGrid`` positions per device pixel. The rasterizer reads its
/// pen here and moves the outline by the phase's fraction before it
/// rasterises, so every arm places a master's glyph identically.
///
/// - Parameters:
///   - slotWidthInPixels: The master's width.
///   - advanceInPixels: The glyph's drawn advance in the master, stretch included.
///   - master: The master, for its shift.
/// - Returns: The whole-pixel pen and its fraction, in 1/``sizedMasterPenGrid`` pixels.
[[nodiscard]] inline SubpixelPhase sizedMasterPen(int slotWidthInPixels, double advanceInPixels, const SizedMaster& master)
{
    return phaseForPen(static_cast<float>((slotWidthInPixels - advanceInPixels) / 2.0 + master.shiftX), sizedMasterPenGrid);
}

/// Where a master's baseline stands, in device pixels below the master's top.
///
/// The slot's baseline magnified by the master's scale, then moved down by the
/// master's shift, and not rounded. Each band of a block is cut from its own
/// master at a whole texel, and the shift is the part of a texel that band's
/// place leaves. The bands step by the font's fractional line height, so that
/// part differs from band to band: a baseline rounded to a whole row draws each
/// band up to half a row from where its neighbour expects it, and a diagonal
/// stroke steps sideways by its slope times that at every join.
///
/// - Parameters:
///   - slotBaselineFromTopInPixels: The unscaled slot's baseline, below the slot's top.
///   - master: The master, for its scale and shift.
/// - Returns: The baseline, below the master's top.
[[nodiscard]] constexpr double sizedMasterBaselineFromTop(double slotBaselineFromTopInPixels, const SizedMaster& master)
{
    return master.scale.value() * slotBaselineFromTopInPixels + master.shiftY;
}

/// Selects a tile's master from the facts `rasterizer` reports about `face`.
///
/// - Parameters:
///   - rasterizer: The rasterizer that will draw the master.
///   - face: The style the window is drawn in.
///   - tile: The window's block.
/// - Returns: The master.
[[nodiscard]] inline SizedMaster selectSizedMaster(GlyphRasterizer& rasterizer, FontFace face, SizedTile tile)
{
    if (tile.widthScale != 2)
        return selectSizedMaster(tile, {}, 0.0, 0.0);
    const WidthAxisRoom room = rasterizer.widthAxisRoom(face);
    const double target = doubleWidthAxisTarget(room);
    if (target <= 0.0)
        return selectSizedMaster(tile, {}, 0.0, 0.0);
    return selectSizedMaster(tile, room, rasterizer.cellAdvanceAtWidthAxis(face, room.current), rasterizer.cellAdvanceAtWidthAxis(face, target));
}

/// The canvas a sized window fills inside its atlas slot.
struct SizedWindowCanvas {
    /// The canvas the unsized glyph fills: the cell box plus a margin a side.
    int width = 0;
    int height = 0;
    int marginX = 0;
    int marginY = 0;
    /// The font's line height, un-rounded, which a band's window steps by; zero
    /// steps by the cell box. See ``sizedBandHeightInPixels``.
    float referenceLineHeightInPixels = 0.0f;
    /// The face's advance across the cells one window covers, un-rounded, which
    /// a window steps by; zero steps by the cell box. See ``sizedWindowStepInPixels``.
    float referenceAdvanceInPixels = 0.0f;
    /// Whether the slot stands its glyph's baseline the descent above the box's
    /// bottom, so that a box taller than the line has its spare rows above the
    /// line's top and not below its bottom. A block begins at the line's top,
    /// where the glyph's ascent says, and not at the box's.
    bool isGlyphBottomAligned = false; // bool-ok: a struct member reading as a predicate
};

/// The canvas a slot's sized windows fill, stepping by the cells they cover.
///
/// Every frontend names its canvas here, so the step a window is cut by is the
/// step its quad advances by: the face's fractional advance for a glyph-stream
/// quad, and the width of the rect a cell maps for a window a grid cell shows
/// texel for texel. That rect is the cell box the backend names, which is not
/// always the canvas less its margins: a slot may carry a column of subpixel
/// headroom past the box, and a window stepped by the canvas would be shown
/// compressed by the column.
///
/// - Parameters:
///   - width: The slot canvas's width, the cell box plus a margin a side.
///   - height: The slot canvas's height.
///   - marginX: The margin on each side of the box.
///   - marginY: The margin above and below the box.
///   - referenceLineHeightInPixels: The font's line height, un-rounded; zero steps bands by the box.
///   - referenceAdvanceInPixels: One cell's advance, un-rounded; zero steps by the box.
///   - cellsAcross: The cells the slot covers: 1 for a narrow window, 2 for a wide one.
///   - step: Whether the window steps by the advance or by the cell box.
///   - cellBoxWidthInPixels: The width of the rect the backend names for a window,
///     across the `cellsAcross` cells it covers; zero steps by the canvas's own box.
/// - Returns: The canvas, its step the advance of the cells it covers or the cell box's width.
[[nodiscard]] constexpr SizedWindowCanvas sizedWindowCanvas(int width, int height, int marginX, int marginY,
    float referenceLineHeightInPixels, float referenceAdvanceInPixels, int cellsAcross, SizedWindowStep step, float cellBoxWidthInPixels)
{
    return { width, height, marginX, marginY, referenceLineHeightInPixels,
        step == SizedWindowStep::CellBox ? cellBoxWidthInPixels : static_cast<float>(cellsAcross) * referenceAdvanceInPixels };
}

/// A drawn master's coverage bytes, which a platform hands the cut.
using SizedMasterBytes = std::span<const std::uint8_t>;
/// The bytes a window is cut into.
using SizedWindowBytes = std::span<std::uint8_t>;

/// Where a sized window lies in its master, and the master it asks for.
///
/// A platform that rasterises the master itself reads this and nothing else:
/// the buffer's size, the whole texel the window begins at, and the shift the
/// master is drawn with. ``cutSizedWindow`` then lifts the window out.
struct SizedWindowCut {
    /// The master, with the shift the window's place leaves.
    SizedMaster master;
    /// The buffer the master is drawn into.
    int masterWidth = 0;
    int masterHeight = 0;
    /// The master's texel the window's first texel is cut from.
    int sourceX = 0;
    int sourceY = 0;
    /// Where the glyph's cell box begins in the master: the margin, magnified.
    double boxLeft = 0.0;
    double boxTop = 0.0;
    /// How far a glyph standing at the block's own origin moves across to land
    /// where the cut expects it, for a rasterizer that places the pen itself
    /// rather than leaving it to ``sizedMasterPen``: the pen is then
    /// `boxLeft` plus the glyph's offset from its cell, magnified, plus this.
    double blockShiftX = 0.0;
};

/// Places one window of a sized block in its master.
///
/// The master holds the canvas at `horizontalMagnification` across and `scale`
/// down, with the glyph's cell box a margin, magnified, in from its top left,
/// and the block is that box seen through windows: window `k` across begins `k`
/// steps along it, on the whole pixel ``sizedWindowLeftInPixels`` names, and
/// band `j` down begins `j` lines down from the box's top, which is the block's
/// top. The step across is the face's own advance where the canvas names it and
/// the cell box otherwise, because the cells the windows land on advance by it.
///
/// Every window of a block, in every band, is cut from a master of the same
/// size with the glyph at the same place in the block: only the texel the
/// window begins at moves, and the shift takes up what that texel leaves. A
/// rasterizer that draws the shift exactly (``sizedMasterPen``,
/// ``sizedMasterBaselineFromTop``) therefore draws one glyph across the whole
/// block, whose strokes run on across every window and band join.
///
/// The glyph stands on the block's pen, and the pen on the anchor's subpixel
/// phase: the quads sit on whole device pixels at the snapped pen, so the
/// fraction of a pixel the pen was snapped from is drawn into the glyph. A
/// glyph is placed by its advance, centred in the extent the windows step
/// across, and not in the box, which a fractional advance leaves a part of a
/// pixel wider; at seven times the size that part is seven times as large.
/// ``sizedMasterPen`` centres in the master, so the difference between the two
/// centres is part of the shift.
///
/// A window begins where the exact place says and the master is whole texels, so
/// the window is cut at the nearest texel and the master is drawn already moved
/// by what that leaves, in ``SizedMaster::shiftX`` and ``SizedMaster::shiftY``.
/// Nothing is resampled: a soft window is what a fractional cut of a drawn
/// master gives.
///
/// - Parameters:
///   - master: The master Core selected for the tile.
///   - tile: Which window of the block.
///   - phase: The anchor's raster variant, whose phase bits give its subpixel pen.
///   - canvas: The slot canvas the window fills.
/// - Returns: The master to draw, its size, and the texel the window is cut from.
[[nodiscard]] inline SizedWindowCut planSizedWindow(SizedMaster master, SizedTile tile, std::uint32_t phase, SizedWindowCanvas canvas)
{
    SizedWindowCut cut;
    const RasterScale across = master.horizontalMagnification();
    const RasterScale down = master.scale;
    cut.masterWidth = across.scaledToHold(canvas.width);
    cut.masterHeight = down.scaledToHold(canvas.height);
    const double boxWidth = canvas.width - 2 * canvas.marginX;
    const double boxHeight = canvas.height - 2 * canvas.marginY;
    const double step = sizedWindowStepInPixels(canvas.referenceAdvanceInPixels, static_cast<float>(boxWidth));
    const double bandStep = sizedBandHeightInPixels(canvas.referenceLineHeightInPixels, static_cast<float>(boxHeight));
    cut.boxLeft = across.value() * canvas.marginX;
    cut.boxTop = down.value() * canvas.marginY;
    const double penFraction = static_cast<double>(variantPhase(phase)) / subpixelPhaseCount;
    const double blockLeft = cut.boxLeft + sizedWindowLeftInPixels(tile.tile, static_cast<float>(step)) - canvas.marginX - penFraction;
    // A box taller than the line has its spare rows at the top when the glyph
    // stands on the box's bottom; the block's first band is the line, so it
    // begins past them, at the line's top.
    const double spareRowsAboveLine = canvas.isGlyphBottomAligned ? std::max(0.0, boxHeight - bandStep) : 0.0;
    const double blockTop = cut.boxTop + down.value() * spareRowsAboveLine + static_cast<double>(tile.band) * bandStep - canvas.marginY;
    // Where a glyph centred in the master stands against one centred in the
    // block's extent: the master's centre less the block's.
    const double centring = (cut.masterWidth - across.value() * step) / 2.0 - cut.boxLeft;
    cut.sourceX = static_cast<int>(std::lround(blockLeft + centring));
    cut.sourceY = static_cast<int>(std::lround(blockTop));
    cut.blockShiftX = static_cast<double>(cut.sourceX) - blockLeft;
    cut.master = master;
    cut.master.shiftX = cut.blockShiftX - centring;
    cut.master.shiftY = static_cast<double>(cut.sourceY) - blockTop;
    return cut;
}

/// Lifts one window out of a drawn master.
///
/// A window past the master's edge is blank rather than clamped, which would
/// smear the last column across the block.
///
/// - Parameters:
///   - master: `cut.masterWidth * cut.masterHeight` coverage bytes, top row first.
///   - cut: Where the window lies, from ``planSizedWindow``.
///   - canvas: The slot canvas the window fills.
///   - window: `canvas.width * canvas.height` bytes to fill.
inline void cutSizedWindow(SizedMasterBytes master, const SizedWindowCut& cut, SizedWindowCanvas canvas, SizedWindowBytes window)
{
    const std::size_t masterWidth = static_cast<std::size_t>(cut.masterWidth);
    for (int y = 0; y < canvas.height; ++y) {
        const std::span<std::uint8_t> destinationRow = window.subspan(static_cast<std::size_t>(y) * static_cast<std::size_t>(canvas.width), static_cast<std::size_t>(canvas.width));
        const int masterY = cut.sourceY + y;
        if (masterY < 0 || masterY >= cut.masterHeight) {
            std::ranges::fill(destinationRow, std::uint8_t { 0 });
            continue;
        }
        const std::span<const std::uint8_t> sourceRow = master.subspan(static_cast<std::size_t>(masterY) * masterWidth, masterWidth);
        for (int x = 0; x < canvas.width; ++x) {
            const int masterX = cut.sourceX + x;
            destinationRow[static_cast<std::size_t>(x)] = (masterX >= 0 && masterX < cut.masterWidth) ? sourceRow[static_cast<std::size_t>(masterX)] : std::uint8_t { 0 };
        }
    }
}

/// Draws one window of a sized tile's master into a slot-shaped canvas.
///
/// The master is selected, placed and drawn by the rasterizer at the size
/// ``planSizedWindow`` names, then cut by ``cutSizedWindow``. A platform whose
/// rasterizer is not a ``GlyphRasterizer`` draws the master itself and makes the
/// same two calls around it.
///
/// - Parameters:
///   - rasterizer: The rasterizer that draws the master.
///   - codePoint: The character.
///   - face: The style.
///   - weight: The variable-weight lane.
///   - tile: Which block, band and window.
///   - phase: The anchor's raster variant; its phase bits are its subpixel pen.
///   - canvas: The slot canvas the window fills.
///   - masterScratch: Reused storage for the master; resized here.
///   - window: `canvas.width * canvas.height` bytes to fill.
/// - Returns: False when nothing drew the glyph, with `window` untouched.
inline bool rasteriseSizedWindow(GlyphRasterizer& rasterizer, std::uint32_t codePoint, FontFace face, std::uint32_t weight, SizedTile tile, std::uint32_t phase,
    SizedWindowCanvas canvas, std::vector<std::uint8_t>& masterScratch, std::span<std::uint8_t> window)
{
    const SizedWindowCut cut = planSizedWindow(selectSizedMaster(rasterizer, face, tile), tile, phase, canvas);
    if (cut.masterWidth <= 0 || cut.masterHeight <= 0 || window.size() < static_cast<std::size_t>(canvas.width) * static_cast<std::size_t>(canvas.height))
        return false;
    masterScratch.assign(static_cast<std::size_t>(cut.masterWidth) * static_cast<std::size_t>(cut.masterHeight), 0);
    if (!rasterizer.rasterizeSizedMaster(codePoint, face, weight, cut.master, cut.masterWidth, cut.masterHeight, masterScratch.data()))
        return false;
    cutSizedWindow(masterScratch, cut, canvas, window);
    return true;
}

/// Whether a backend cuts this tile's windows from a sized master.
///
/// Every whole-scale tile past the cell's own glyph, doubled across or not,
/// and every fraction that is not doubled across. A doubled fraction keeps the
/// builder's crop.
///
/// - Parameter tile: The window's block.
/// - Returns: True when a master serves it.
[[nodiscard]] constexpr bool isSizedMasterTile(SizedTile tile)
{
    if (tile.isFractional())
        return tile.widthScale == 1;
    return tile.scale >= 2 || (tile.scale == 1 && tile.widthScale == 2);
}

/// The bitmap strike a colour face draws a target size from.
///
/// The smallest strike at least as tall as the target, so the raster is a
/// reduction and keeps the detail the face drew; only when every strike is
/// smaller does the largest stand in, because no larger one exists to reduce.
///
/// - Parameters:
///   - strikeHeights: The face's strike heights, in pixels, in any order.
///   - targetInPixels: The square the glyph is drawn into.
/// - Returns: The strike's index, or -1 when the face has none.
[[nodiscard]] constexpr int colorStrikeForTarget(std::span<const int> strikeHeights, int targetInPixels)
{
    int above = -1;
    int largest = -1;
    for (int index = 0; index < static_cast<int>(strikeHeights.size()); ++index) {
        const int height = strikeHeights[static_cast<std::size_t>(index)];
        if (largest < 0 || height > strikeHeights[static_cast<std::size_t>(largest)])
            largest = index;
        if (height >= targetInPixels && (above < 0 || height < strikeHeights[static_cast<std::size_t>(above)]))
            above = index;
    }
    return above >= 0 ? above : largest;
}

/// The square a colour cluster's sized master is drawn at.
///
/// - Parameters:
///   - tile: The window's block.
///   - slotInPixels: The colour slot's edge, one window's side.
/// - Returns: The master's edge, rounded up where a fraction leaves part of a pixel.
[[nodiscard]] constexpr int sizedColorMasterSide(SizedTile tile, int slotInPixels)
{
    return selectSizedMaster(tile, {}, 0.0, 0.0).scale.scaledToHold(slotInPixels);
}

/// The verified contents of a sized colour window's key: the cluster, then the
/// window's tile word, as ``sizedColorTileKey`` documents.
struct SizedColorTileContents {
    std::array<std::uint32_t, clusterPoolMaximumCodePoints + 1> words {};
    int count = 0;
    std::uint32_t key = 0;
};

/// Derives a sized colour window's key and the words it is verified against.
///
/// - Parameters:
///   - cluster: The cluster, base first.
///   - tile: The window.
/// - Returns: The key and its contents; `count` is zero for a cluster too long
///   to key, which the caller declines.
[[nodiscard]] inline SizedColorTileContents sizedColorTileContents(std::span<const std::uint32_t> cluster, SizedTile tile)
{
    SizedColorTileContents out;
    if (cluster.empty() || cluster.size() > clusterPoolMaximumCodePoints)
        return out;
    std::ranges::copy(cluster, out.words.begin());
    const std::uint32_t scaleLane = tile.scaleLane();
    const std::uint32_t tileWord = sizedTileWord(0u, FontFace::Regular, 0u, scaleLane, tile.band, tile.tile);
    out.words[cluster.size()] = tileWord;
    out.count = static_cast<int>(cluster.size()) + 1;
    out.key = sizedColorTileKey(cluster.data(), static_cast<int>(cluster.size()), tileWord);
    return out;
}

/// Lifts one window out of a colour cluster's drawn master.
///
/// The window `(band, tile)` is the slot-sized square that many slots across and
/// down. A window past the master's edge is transparent.
///
/// This is every arm's colour semantics, and it is a square's, not a glyph's: a
/// colour cluster has no pen, advance or line of its own. The builder centres a
/// square of the block's size in the block and maps each slot-sized window onto
/// its place in that square, so the step is the slot's edge, there is no subpixel
/// phase to draw in, and bands are rows of the square rather than of the font's
/// line. The advance, phase and line-height semantics of ``planSizedWindow``
/// would place the square off the one the builder centres.
///
/// - Parameters:
///   - master: `side * side * 4` bytes of RGBA, top row first.
///   - side: The master's edge, ``sizedColorMasterSide``.
///   - tile: The window's block, band and window.
///   - slotInPixels: The colour slot's edge.
///   - window: `slotInPixels * slotInPixels * 4` bytes to fill.
inline void cutSizedColorWindow(SizedMasterBytes master, int side, SizedTile tile, int slotInPixels, SizedWindowBytes window)
{
    const std::size_t rowBytes = static_cast<std::size_t>(slotInPixels) * 4;
    const int left = static_cast<int>(tile.tile) * slotInPixels;
    const int top = static_cast<int>(tile.band) * slotInPixels;
    for (int y = 0; y < slotInPixels; ++y) {
        const std::span<std::uint8_t> destination = window.subspan(static_cast<std::size_t>(y) * rowBytes, rowBytes);
        std::ranges::fill(destination, std::uint8_t { 0 });
        const int masterY = top + y;
        if (masterY >= side)
            continue;
        const int visible = std::clamp(side - left, 0, slotInPixels);
        const std::span<const std::uint8_t> source = master.subspan((static_cast<std::size_t>(masterY) * static_cast<std::size_t>(side) + static_cast<std::size_t>(left)) * 4,
            static_cast<std::size_t>(visible) * 4);
        std::ranges::copy(source, destination.begin());
    }
}

/// Draws one window of a colour cluster's sized master into a slot-sized RGBA canvas.
///
/// The cluster is drawn once at the block's size — the face's vector paint
/// where it has one, a bitmap strike reduced from the nearest one above the
/// size where it does not — and the window `(band, tile)` is cut from it by
/// ``cutSizedColorWindow``.
///
/// - Parameters:
///   - rasterizer: The rasterizer that draws the master.
///   - cluster: The cluster, base first.
///   - tile: The window's block, band and window.
///   - slotInPixels: The colour slot's edge.
///   - masterScratch: Reused storage for the master; resized here.
///   - window: `slotInPixels * slotInPixels * 4` bytes of straight RGBA to fill.
/// - Returns: False when the rasterizer declined the cluster.
inline bool rasteriseSizedColorWindow(GlyphRasterizer& rasterizer, std::span<const std::uint32_t> cluster, SizedTile tile, int slotInPixels,
    std::vector<std::uint8_t>& masterScratch, std::span<std::uint8_t> window)
{
    const int side = sizedColorMasterSide(tile, slotInPixels);
    const std::size_t rowBytes = static_cast<std::size_t>(slotInPixels) * 4;
    if (side <= 0 || cluster.empty() || window.size() < rowBytes * static_cast<std::size_t>(slotInPixels))
        return false;
    masterScratch.assign(static_cast<std::size_t>(side) * static_cast<std::size_t>(side) * 4, 0);
    if (!rasterizer.rasterizeColorCluster(cluster.data(), static_cast<int>(cluster.size()), side, masterScratch.data()))
        return false;
    cutSizedColorWindow(masterScratch, side, tile, slotInPixels, window);
    return true;
}

} // namespace Ubiquitty::Rendering
