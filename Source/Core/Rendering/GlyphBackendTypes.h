// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Base/Geometry.h"

#include "Base/SwiftInterop.h"
#include "Core/CellContent.h"
#include "Core/Rendering/CellInstance.h"
#include "Core/Rendering/GlyphKey.h"
#include "Core/Rendering/GlyphSlotCache.h"
#include "Core/Rendering/LigatureParticipation.h"
#include "PAL/TextShaping.h"
#include <cmath>
#include <cstdint>
#include <numeric>
#include <span>

// The values that cross the glyph seam, apart from the seam itself so a
// backend can name them without pulling in the builder that consumes them.
//
// All of it is POD and none of it references a model type: a backend is
// handed rectangles, a slot cache it only carries, and enums. That is what
// lets GlyphBackend.h be a contract rather than a base class.

namespace Ubiquitty::Rendering {

/// A backend's two-cell sheet, bound once so the per-cell path never calls into it.
///
/// Same terms as ``MonoAtlasBinding`` and ``ColorAtlasBinding``: ``wideGlyphKey``
/// is a Core derivation, the slots are in a portable ``GlyphSlotCache``, and the
/// grid is four numbers. A resident wide glyph crosses zero times.
///
/// No inset, unlike the colour sheet: a wide slot is the glyph's own box at its
/// true two-cell advance, so the rect is the slot.
/// Escapable, unlike most borrow-carrying types here, because Swift is only ever
/// this struct's PRODUCER. Every consumer is C++ — the cell builder and the
/// software renderer, both of which read it inside the one `buildCellInstances`
/// call it was made for. A non-escapable tag would police a Swift consumer that
/// does not exist, while making the struct unproducible from Swift at all: a
/// nonescapable value cannot be returned from a Swift function or closure, and a
/// pointer to one is not representable, so a Swift backend could fill neither the
/// entry nor an out-parameter version of it.
///
/// The frame-lifetime rule the tag would have carried belongs to the pointers
/// below and is stated on them.
struct UBIQUITTY_ESCAPABLE WideAtlasBinding {
    GlyphSlotCache* cache = nullptr;
    int columns = 0;
    int rows = 0;
    int slotWidthInPixels = 0;
    int slotHeightInPixels = 0;
    int slotCapacity = 0;

    /// Returns the slot's UV rect.
    ///
    /// - Parameter slot: The slot the cache assigned.
    /// - Returns: The rect, or a zero rect for a degenerate grid.
    [[nodiscard]] GlyphUV atlasRectForSlot(int slot) const
    {
        if (columns <= 0 || rows <= 0 || slotWidthInPixels <= 0 || slotHeightInPixels <= 0)
            return { 0.0f, 0.0f, 0.0f, 0.0f };
        const int column = slot % columns;
        const int row = slot / columns;
        return {
            static_cast<float>(column * slotWidthInPixels),
            static_cast<float>(row * slotHeightInPixels),
            static_cast<float>((column + 1) * slotWidthInPixels),
            static_cast<float>((row + 1) * slotHeightInPixels),
        };
    }
};

/// A backend's dedicated proportional-glyph sheet, bound once per frame.
///
/// Core owns its cache policy, key derivation, and slot-to-UV mapping. The
/// backend owns only the coverage storage and the platform font rasterizer,
/// reached when Core assigns a fresh slot. Keeping this distinct from
/// ``WideAtlasBinding`` prevents either population from consuming the other's
/// bounded working set even when their physical slot dimensions match.
struct UBIQUITTY_ESCAPABLE ProportionalAtlasBinding {
    GlyphSlotCache* cache = nullptr;
    int columns = 0;
    int rows = 0;
    int slotWidthInPixels = 0;
    int slotHeightInPixels = 0;
    int slotCapacity = 0;
    IntSize marginInPixels {};
    int phaseCount = 1;

    /// Returns the full pixel-space rect for one assigned slot.
    ///
    /// - Parameter slot: The slot Core's cache assigned.
    /// - Returns: The rect, or a zero rect for a degenerate grid.
    [[nodiscard]] GlyphUV atlasRectForSlot(int slot) const
    {
        if (columns <= 0 || rows <= 0 || slotWidthInPixels <= 0 || slotHeightInPixels <= 0) {
            return { 0.0f, 0.0f, 0.0f, 0.0f };
        }
        const int column = slot % columns;
        const int row = slot / columns;
        return {
            static_cast<float>(column * slotWidthInPixels),
            static_cast<float>(row * slotHeightInPixels),
            static_cast<float>((column + 1) * slotWidthInPixels),
            static_cast<float>((row + 1) * slotHeightInPixels),
        };
    }
};

/// A backend's colour sheet, bound once so the per-cell path never calls into it.
///
/// The same shape as ``MonoAtlasBinding`` and for the same reason: the keys are
/// Core derivations (``colorGlyphKey``, ``colorClusterKey``), the slots live in a
/// portable ``GlyphSlotCache``, and the layout is four numbers. So the builder
/// runs the whole hit path itself and reaches the backend only to rasterise a
/// glyph the cache had no slot for.
///
/// Its own binding rather than a flag on ``GlyphSlotGeometry``: a colour sheet is
/// not a mono atlas laid out differently. Its slots are square — a colour strike
/// carries one weight and no slant — and its rect is inset by half a texel on
/// every side, which that struct has no field for.
/// Escapable, unlike most borrow-carrying types here, because Swift is only ever
/// this struct's PRODUCER. Every consumer is C++ — the cell builder and the
/// software renderer, both of which read it inside the one `buildCellInstances`
/// call it was made for. A non-escapable tag would police a Swift consumer that
/// does not exist, while making the struct unproducible from Swift at all: a
/// nonescapable value cannot be returned from a Swift function or closure, and a
/// pointer to one is not representable, so a Swift backend could fill neither the
/// entry nor an out-parameter version of it.
///
/// The frame-lifetime rule the tag would have carried belongs to the pointers
/// below and is stated on them.
struct UBIQUITTY_ESCAPABLE ColorAtlasBinding {
    GlyphSlotCache* cache = nullptr;
    int columns = 0;
    int rows = 0;
    /// The slot edge, in atlas pixels.
    int slotInPixels = 0;
    int slotCapacity = 0;

    /// Returns the slot's pixel-boundary UV rect.
    ///
    /// Equal source and destination extents map pixel centres to texel centres.
    /// Linear samplers clamp the interpolated coordinate to the slot's texel
    /// centres when scaling, without contracting this rect.
    ///
    /// - Parameter slot: The slot the cache assigned.
    /// - Returns: The rect, or a zero rect for a degenerate layout.
    [[nodiscard]] GlyphUV atlasRectForSlot(int slot) const
    {
        if (columns <= 0 || rows <= 0 || slotInPixels <= 0)
            return { 0.0f, 0.0f, 0.0f, 0.0f };
        const int column = slot % columns;
        const int row = slot / columns;
        return {
            static_cast<float>(column * slotInPixels),
            static_cast<float>(row * slotInPixels),
            static_cast<float>((column + 1) * slotInPixels),
            static_cast<float>((row + 1) * slotInPixels),
        };
    }
};

/// One slot-shaped window of a glyph drawn at an OSC 66 scale.
///
/// A sized block claims `scale` rows, and a stream that goes sharp draws it
/// one ordinary slot-sized quad at a time — band `band` of the block, window
/// `tile` along it. The window grid steps by the destination the slot kind
/// already fills (a cell, two cells, the colour square), so a backend
/// rasterising the glyph at `scale` times its size cuts it along exactly the
/// boundaries the quads abut on, and the tiles meet the way adjacent cells
/// do.
///
/// `DoubleRegion` is this idea at scale 2 with the regions named instead of
/// counted; it is separate because DECDHL rows are addressed by half, not by
/// band.
struct SizedTile {
    /// The block's row count, 2..7 — `TextSizeSpec::scale`.
    std::uint8_t scale = 0;
    /// Which row of the block, 0..scale-1, top-down — `penTextBand`.
    std::uint8_t band = 0;
    /// Which window along the band, 0..per-band windows-1, left-right.
    std::uint8_t tile = 0;
    /// The horizontal factor over `scale`: 2 for a block anchored on a DECDWL
    /// row, which is drawn twice as wide as it is tall, and 1 otherwise.
    std::uint8_t widthScale = 1;
    /// A fractional raster's numerator, 1-15, or zero for a whole `scale`.
    ///
    /// OSC 66's `n`/`d` draw the glyph at `scale * n / d` of its size, and the
    /// backend rasterises it at exactly that size: a fraction that reduces to
    /// a whole scale is asked for as that scale with no fraction, so it shares
    /// the ordinary sized windows. With a fraction, `band` and `tile` name a
    /// cell-sized window of the raster at its own size — `ceil(scale * n / d)`
    /// rows of them — not a row of the block it stands in.
    std::uint8_t fractionNumerator = 0;
    /// The fractional raster's denominator, above ``fractionNumerator``, or zero.
    std::uint8_t fractionDenominator = 0;
    /// How far the window steps across its block: the glyph-stream's advance, or
    /// the cell box for a window a grid cell maps 1:1 (``SizedWindowStep``).
    SizedWindowStep step = SizedWindowStep::FaceAdvance;

    /// - Returns: The lane this window is keyed and verified under.
    [[nodiscard]] constexpr std::uint32_t scaleLane() const
    {
        return sizedTileScaleLane(scale, widthScale, fractionNumerator, fractionDenominator, step);
    }

    /// - Returns: True when the raster's size is a fraction of `scale`.
    [[nodiscard]] constexpr bool isFractional() const
    {
        return fractionNumerator != 0 && fractionDenominator > fractionNumerator;
    }

    /// - Returns: The size the window's raster is drawn at, as a factor of the
    ///   cell's own glyph.
    [[nodiscard]] constexpr double rasterScale() const
    {
        return isFractional() ? static_cast<double>(scale) * fractionNumerator / fractionDenominator : static_cast<double>(scale);
    }
};

/// The most windows one band of a sized glyph is cut into: scale seven, a
/// glyph two cells wide or a block on a doubled row.
///
/// The count is the glyph's own width, never the block's. An explicit `w`
/// widens the block without widening the glyph, and the columns past the
/// glyph hold nothing to draw; counting them would refuse a block the
/// windows draw sharp and send it to the magnified crop.
inline constexpr int maxSizedWindowsPerBand = 14;

/// How many atlas rows one sized band shows of its window's cell box.
///
/// A slot's cell box is the font's line height rounded up to whole pixels, and
/// the grid's rows are the line height itself, so a band that showed the whole
/// box would reach past its own row into the next band's by the rounding, and
/// the two would blend the rows they share twice. Band `k` of a block shows
/// rows `[k * line, (k + 1) * line)` of the glyph drawn at the block's scale,
/// so the bands partition it: the rasteriser cuts at this step, and the builder
/// maps this many rows onto one grid row.
///
/// - Parameters:
///   - referenceLineHeightInPixels: The font's line height, un-rounded; zero
///     when the backend reports none.
///   - cellBoxHeightInPixels: The slot's cell box, margins excluded.
/// - Returns: The line height where it is shorter than the box, else the box.
[[nodiscard]] constexpr float sizedBandHeightInPixels(float referenceLineHeightInPixels, float cellBoxHeightInPixels)
{
    return referenceLineHeightInPixels > 0.0f && referenceLineHeightInPixels < cellBoxHeightInPixels ? referenceLineHeightInPixels : cellBoxHeightInPixels;
}

/// How far one sized window steps across a block, in device pixels.
///
/// The cells a block's windows land on advance by the face's own fractional
/// width, so a window cut a whole cell box along shows a slice the window
/// before it showed: a seam through the ink, one fraction wider per window.
/// Window `k` of a block therefore begins `k * step` along it, where the step is
/// the face's reference advance where the backend reports one, else the cell box.
///
/// - Parameters:
///   - referenceAdvanceInPixels: The face's advance, un-rounded; zero when the
///     backend reports none.
///   - cellBoxWidthInPixels: The window's cell box, margins excluded.
/// - Returns: The step.
[[nodiscard]] constexpr float sizedWindowStepInPixels(float referenceAdvanceInPixels, float cellBoxWidthInPixels)
{
    return referenceAdvanceInPixels > 0.0f ? referenceAdvanceInPixels : cellBoxWidthInPixels;
}

/// Where window `window` of a sized block begins across, from the block's pen.
///
/// `window` steps along, on the nearest whole pixel. The step is the face's
/// fractional advance, and a window begun at a fraction of a pixel puts its
/// texels between the device's pixels, where the sampler blends two of them:
/// a window beside one blended at another fraction shows the same stroke a
/// little narrower or wider, a seam at every window join. On whole pixels a
/// window's texels are the pixels it lands on, and the windows still advance
/// by the step, a pixel wider or narrower where its fraction carries over.
/// The rasteriser cuts window `window` from here and the builder lands its quad
/// here, so the texel a pixel shows is the one cut for it.
///
/// - Parameters:
///   - window: The window's index across the block.
///   - step: ``sizedWindowStepInPixels``, in the atlas pixels the window is cut in.
/// - Returns: The window's left edge, in whole atlas pixels from the block's pen.
[[nodiscard]] inline float sizedWindowLeftInPixels(int window, float step)
{
    return std::round(static_cast<float>(window) * step);
}

/// The sized raster an OSC 66 fraction is drawn from.
///
/// The fraction is reduced and folded into the scale wherever the product is
/// whole: `s=4` at `1/2` is the ordinary scale-2 raster, keyed and cut as one.
/// A product of exactly one is the cell's own glyph, which needs no sized raster.
///
/// - Parameters:
///   - scale: The block's `s`, 1-7.
///   - numerator: The spec's `n`.
///   - denominator: The spec's `d`, above `n`.
/// - Returns: The tile to ask for with `band` and `tile` zero; its `scale` is
///   zero when the product is one.
constexpr SizedTile fractionalSizedTile(std::uint8_t scale, std::uint8_t numerator, std::uint8_t denominator)
{
    SizedTile tile;
    const unsigned product = static_cast<unsigned>(scale) * numerator;
    if (denominator == 0 || product % denominator == 0) {
        const unsigned whole = denominator == 0 ? scale : product / denominator;
        tile.scale = static_cast<std::uint8_t>(whole == 1 ? 0 : whole);
        return tile;
    }
    // Not whole: the block's scale and the fraction reduced on its own, so a
    // fraction keys one raster however its spec spelled it.
    const unsigned divisor = std::gcd(static_cast<unsigned>(numerator), static_cast<unsigned>(denominator));
    tile.scale = scale;
    tile.fractionNumerator = static_cast<std::uint8_t>(numerator / divisor);
    tile.fractionDenominator = static_cast<std::uint8_t>(denominator / divisor);
    return tile;
}

/// A glyph's rectangle in its atlas page, in atlas pixels.
///
/// `present` is false when the atlas has no slot for the glyph this frame
/// (full) or the glyph has no coverage (e.g. a space) — the cell then renders
/// background-only, with `minUV == maxUV`.
struct UBIQUITTY_ESCAPABLE AtlasGlyph {
    Vec2 minUV { 0.0f, 0.0f };
    Vec2 maxUV { 0.0f, 0.0f };
    bool present = false;
    /// Whether ``inkMinUV``/``inkMaxUV`` hold a measured ink box.
    ///
    /// A backend that measures its raster reports the box the ink actually
    /// covers, and the builder sizes the glyph quad to it instead of to the
    /// slot: the slot carries margins and phase headroom on every side, so
    /// slot-sized quads overlap their neighbours and the pass shades far more
    /// than the screen. False costs only the slot-sized quad the builder
    /// already emits, so a backend that cannot measure — a staged plane it did
    /// not draw, a bitmap face — leaves it unset.
    bool hasInkBounds = false;
    /// The measured ink box in atlas pixels, one guard texel out on each side.
    ///
    /// The guard is part of the contract: a linear tap at the quad edge reads
    /// half a texel outward, so a box flush against the ink would feather the
    /// outermost coverage against whatever lies beyond it. One zero texel of
    /// slack keeps every tap the fragment shader can make inside pixels the
    /// rasteriser wrote. A box TIGHTER than the ink clips glyphs, which is a
    /// visible defect; a looser one only shades more nothing.
    Vec2 inkMinUV { 0.0f, 0.0f };
    /// The ink box's bottom-right, in the same absolute atlas pixels.
    Vec2 inkMaxUV { 0.0f, 0.0f };
};

/// Where a proportional run's strip goes (Docs/proportional-text.md), as the
/// backend that shaped it reports.
///
/// The builder hands the backend the run's pen as a whole pixel plus a
/// residual; the backend aligns the text inside the run's cells, floors the
/// aligned pen as CoreText floors every pen of a line, draws the text at that
/// pen's residual, and answers with the quad: `leftInPixels` from the whole
/// pixel to the quad's left edge — the alignment's whole pixels less the
/// strip's margin — and `widthInPixels` the width of the rect `strip` names.
/// Geometry lives with the raster because only the shaper knows the text's
/// width, and the alignment is a function of it.
struct UBIQUITTY_ESCAPABLE ProportionalStripPlacement {
    /// The strip's rect in atlas pixels; absent when the backend refused.
    AtlasGlyph strip {};
    /// The quad's left edge, from the pen's whole pixel.
    float leftInPixels = 0.0f;
    /// The quad's width, the strip rect's width in atlas pixels.
    float widthInPixels = 0.0f;
    /// The quad's top edge from the row's top: the strip's margin and any
    /// overhang above the cell, so at most zero. The quad is as tall as the
    /// rect `strip` names.
    float topInPixels = 0.0f;
    /// The number of tiles Core needs to submit for the complete run.
    int tileCount = 1;
    /// How many times its ordinary width the backend drew the run across:
    /// the request's width scale when it drew a DECDWL run wide, 1 when it
    /// drew the run at its own width and leaves the builder to stretch it.
    float drawnWidthScale = 1.0f;
    /// How many times its ordinary size the backend drew the run down: the
    /// request's scale when it drew the strip at its size and cut the band's
    /// slot height from it, `topInPixels` then measured from the block's top
    /// row rather than the row's; 1 when it drew the run one row tall and
    /// leaves the builder to stretch it.
    float drawnScale = 1.0f;
};

/// Returns the UV rect a double-size master's region occupies in its slot.
///
/// Samples the physical slot, doubled margins included, so fallback-icon
/// ink that overhangs the doubled cell box renders into the margins instead of
/// being cropped; the quad backs its origin off by the scaled margins to
/// compensate. The margins are symmetric, so the slot's vertical midline is
/// the cell box's: the `Top` and `Bottom` halves split there and meet without
/// a seam. UVs name pixel boundaries, matching the quad's extent. Insetting
/// only the UVs would stretch the raster by an amount that depends on padding;
/// a pixel-centred fragment already samples half a texel inside the boundary.
///
/// One mapping for every backend that packs a double sheet: a glyph placed by
/// one arithmetic and sampled by another is off by a margin, and the frame
/// draws.
///
/// - Parameters:
///   - slot: The slot the cache assigned.
///   - grid: The sheet's extent in slots.
///   - slotSize: One slot's extent in pixels.
///   - region: The slice the row draws.
/// - Returns: The region's rect, marked present; a zero rect for a degenerate
///   layout.
[[nodiscard]] inline AtlasGlyph doubleRegionUV(int slot, GridSize grid, IntSize slotSize, DoubleRegion region)
{
    if (grid.columns <= 0 || grid.rows <= 0 || slotSize.width <= 0 || slotSize.height <= 0)
        return {};
    const int column = slot % grid.columns;
    const int row = slot / grid.columns;
    const int x0 = column * slotSize.width;
    const int yTop = row * slotSize.height;
    float minY = static_cast<float>(yTop);
    float maxY = static_cast<float>(yTop + slotSize.height);
    if (region == DoubleRegion::Top)
        maxY = yTop + slotSize.height * 0.5f;
    else if (region == DoubleRegion::Bottom)
        minY = yTop + slotSize.height * 0.5f;
    AtlasGlyph glyph;
    glyph.minUV.x = static_cast<float>(x0);
    glyph.minUV.y = minY;
    glyph.maxUV.x = static_cast<float>(x0 + slotSize.width);
    glyph.maxUV.y = maxY;
    glyph.present = true;
    return glyph;
}

/// Which rectangle of its slot an atlas rect names.
///
/// A slot is the cell box plus a margin on every side, and one more column of
/// subpixel phase headroom beside the box. The mappings Core and the backends
/// hand around each name a different part of it, and a quad built for one that
/// is given another draws the glyph a margin out of place, so the builder
/// converts to the rect its arm expects at the one place a rect is received.
enum class SlotRectConvention : std::uint8_t {
    /// The cell box alone: the rect a cell-baked quad maps onto its cell.
    CellBox,
    /// The slot contracted by its margins: the cell box and its headroom
    /// column. The subpixel quad expands it by the margins to reach the slot.
    ContractedSlot,
    /// The whole slot, margins included.
    PaddedSlot,
    /// The raster's measured ink box, which names no part of the slot. The
    /// slot's own geometry supplies the rest.
    MeasuredInk,
};

/// A backend's mono atlas, bound once so the per-cell path never calls into it.
///
/// The cache and the slot layout are both portable and a lookup is a hash probe,
/// so the builder runs the whole hit path itself and reaches the backend only to
/// RASTERISE a glyph the cache had no slot for — once per glyph per style rather
/// than once per cell.
///
/// There is no fallback and no per-cell choice of any kind: this loop has no
/// registers to spare, and anything added here is paid for in stack reloads.
/// A raster's measured ink box within its slot (see ``MonoAtlasBinding``).
struct UBIQUITTY_ESCAPABLE GlyphInkBox {
    /// The box's top-left, in atlas pixels relative to the slot origin.
    Vec2 minInPixels { 1.0f, 1.0f };
    /// The box's bottom-right; less than ``minInPixels`` marks an unmeasured slot.
    Vec2 maxInPixels { -1.0f, -1.0f };
};

/// A slot nothing has measured: the builder keeps the whole slot quad.
///
/// Named rather than default-constructed, because the two languages disagree
/// about what default construction means. Swift's synthesised `init()` for a
/// C++ struct zero-fills the storage and does not run these member
/// initialisers, so a Swift `GlyphInkBox()` is `{0,0}`/`{0,0}` — which the
/// builder reads as MEASURED with zero area, and clips the glyph to nothing.
/// The sentinel is stated once, here, and both languages call for it.
[[nodiscard]] constexpr GlyphInkBox unmeasuredGlyphInkBox()
{
    return GlyphInkBox {};
}

/// A slot measured and found to carry no ink, as a blank raster does.
///
/// Distinct from ``unmeasuredGlyphInkBox``: this one asserts the measurement
/// happened and found nothing, which is what stops the builder measuring the
/// same absent glyph again.
[[nodiscard]] constexpr GlyphInkBox emptyGlyphInkBox()
{
    return GlyphInkBox { { 0.0f, 0.0f }, { 0.0f, 0.0f } };
}

/// A borrowed view, not a value: `cache` and `slotUV` point into storage the
/// backend owns for the frame. Tagged so a Swift implementer cannot store one
/// past the frame that produced it — the pointers would dangle, and the symptom
/// would be a glyph atlas read after its owner released it.
/// Escapable, unlike most borrow-carrying types here, because Swift is only ever
/// this struct's PRODUCER. Every consumer is C++ — the cell builder and the
/// software renderer, both of which read it inside the one `buildCellInstances`
/// call it was made for. A non-escapable tag would police a Swift consumer that
/// does not exist, while making the struct unproducible from Swift at all: a
/// nonescapable value cannot be returned from a Swift function or closure, and a
/// pointer to one is not representable, so a Swift backend could fill neither the
/// entry nor an out-parameter version of it.
///
/// The frame-lifetime rule the tag would have carried belongs to the pointers
/// below and is stated on them.
struct UBIQUITTY_ESCAPABLE MonoAtlasBinding {
    GlyphSlotCache* cache = nullptr;
    GlyphSlotGeometry geometry;

    /// One rect per slot, in the form the builder wants, or null to derive them.
    ///
    /// `geometry.atlasRectForBuilder` is frame-invariant in everything but the slot,
    /// so a backend that already keeps the table — the Metal one builds it when the
    /// atlas is built — hands it over instead of the walk deriving it per cell.
    ///
    /// Indexed by the slot the cache returns, so it must hold `slotCapacity`
    /// entries. Null is the portable default and costs a branch per cell that a
    /// backend supplying the table pays once and predicts perfectly.
    const GlyphUV* slotUV = nullptr;
    /// The part of the slot each ``slotUV`` entry names; unused while that table is null.
    ///
    /// The builder converts an entry to the contracted slot the subpixel quad
    /// expands, so a backend that keeps its table in another convention is
    /// drawn where its raster is rather than a margin or an ink offset away.
    SlotRectConvention slotUVConvention = SlotRectConvention::ContractedSlot;
    /// One measured ink box per slot, or null for a backend that does not measure.
    ///
    /// The box is the raster's nonzero coverage, one guard texel out on each
    /// side, in atlas pixels relative to the slot origin; `minInPixels > maxInPixels` marks
    /// a slot whose current occupant was not measured. The builder shrinks the
    /// glyph quad to the box, so the invariant every writer owes is the same as
    /// the sheet's own: a slot's entry describes the raster the slot holds.
    /// A stale "unmeasured" costs a slot-sized quad; a stale box from the
    /// previous occupant clips the new glyph on screen.
    ///
    /// Indexed like ``slotUV`` and null under the same contract: the portable
    /// backends leave it unset and keep slot-sized quads.
    const GlyphInkBox* inkBoxForSlot = nullptr;
    int slotCapacity = 0;
    /// The variable-font weight the keys are derived with, fixed for a frame.
    std::uint32_t weight = 0;
};

/// Which pass a row's mono glyphs take, and so where their pen lands.
///
/// ``CellBaked`` puts the glyph in its cell's atlas UV at phase 0; ``Subpixel``
/// emits it as its own quad at a pen snapped to a subpixel phase. A shaped run
/// is rasterised at its run pen, so it has to be told which of the two the row
/// is drawing at or the strip lands a fraction of a pixel off its cells.
enum class GlyphPass {
    CellBaked,
    Subpixel,
};

/// The drawn column a shaped glyph's cluster names, or -1 when it names none.
///
/// A run is shaped with context on both flanks so the face's rules see across
/// its edges, so the shaper's cluster index counts from the first CONTEXT
/// entry while a column counts from the first drawn one. Context clusters draw
/// — their ink lands in the slot's margins — but they belong to a neighbouring
/// chunk, and a producer that handed one to a drawn cell would give that cell a
/// quad over the margin the context insets exist to keep it out of.
///
/// One spelling, because two producers derive from it: the ink ledger folds a
/// glyph into its column through this, and the shaped-answer producer names the
/// column it publishes through the same call. A second copy would drift on the
/// first change to how context is counted, and the drift would show as
/// letterforms one cell out.
///
/// - Parameters:
///   - cluster: The glyph's source index in the shaped text, context included.
///   - leadingContext: How many context entries precede the drawn run.
///   - drawnCells: How many cells the run draws.
/// - Returns: The drawn column, or -1 for a cluster outside the run.
constexpr int shapedGlyphColumn(int cluster, int leadingContext, int drawnCells)
{
    const int column = cluster - leadingContext;
    return (column < 0 || column >= drawnCells) ? -1 : column;
}

/// What the shaper answered for one cell.
///
/// The glyph id is the identity the atlas keys on (``shapedGlyphKey``), so a
/// healed letterform costs a slot per glyph the face actually draws rather
/// than a slot per text fragment that produced one. `coveredCells` is 1 for a
/// substitution, N for the cell carrying an N-cell cluster's whole image, and
/// 0 for a cell that cluster covers and that therefore draws nothing of its
/// own.
struct UBIQUITTY_ESCAPABLE ShapedCell {
    std::uint32_t glyphID = 0;
    float xOffsetInPixels = 0.0f;
    std::uint8_t coveredCells = 1;
};

/// One column's membership in a shaped ligature run.
///
/// A run is a span of cells whose glyphs the shaper drew as one image — `=>` and
/// `!==` in a programming face. It is rasterised whole, into one strip, and every
/// covered cell then draws the slice of that strip its own column occupies, so a
/// colour or style boundary inside the run recolours without moving a glyph.
///
/// `length` is zero for a column no run covers, which takes the ordinary mono
/// path. The strip rect spans the FULL slot rather than the ink: the builder
/// maps a cell's atlas-pixel range onto it linearly, a mapping every atlas
/// layout supports and none has to express.
struct UBIQUITTY_ESCAPABLE LigatureRun {
    AtlasGlyph strip;
    /// The strip's occupied width, in the same atlas pixels as the slot.
    float usedWidthInAtlasPixels = 0.0f;
    /// The share of the slot at each edge that belongs to a neighbouring
    /// chunk, in the same atlas pixels — the answer to "is the margin beside
    /// this edge mine?".
    ///
    /// A run longer than ``ligatureMaxCells`` is shaped in chunks, and each
    /// chunk is shaped with its neighbours as context so the font's rules see
    /// across the split. Those context glyphs are drawn: they land in the
    /// margins, which is also where a first or last cell reaching the slot
    /// edge would look for its own overhang. Reaching there at a chunk seam
    /// paints the neighbouring chunk's glyph on top of the cells that chunk
    /// already drew, and the seam reads bolder than the text around it.
    ///
    /// A backend fills these through ``ligatureStripContextInsets``. Zero is
    /// the answer for a run shaped alone, which is every run short enough not
    /// to split, so a producer that never chunks leaves them as they are.
    float contextInsetLeftInAtlasPixels = 0.0f;
    float contextInsetRightInAtlasPixels = 0.0f;
    /// The slot the rect spans, the denominator of the sub-slice map.
    float slotWidthInAtlasPixels = 0.0f;
    /// This column's own ink, in the same atlas pixels as the slot — the
    /// coverage its cluster's glyphs drew, wherever that ink landed, rather
    /// than the geometric band over the column.
    ///
    /// A healed alternate overhangs its cell box, and a cut at the cell edge
    /// hands the overhang to the neighbouring column's quad, whose colours
    /// may differ — the tip of a wide glyph then renders in the neighbour's
    /// foreground. An ownership cut keeps every cluster's ink under its own
    /// column's colours. `right <= left` is a covered column that owns no
    /// ink — the tail cells of a multi-cell ligature — and emits nothing;
    /// the cluster's lead column carries the whole glyph, so a colour
    /// boundary inside one ligature colours it whole instead of splitting
    /// its ink at a cell edge.
    float ownInkLeftInAtlasPixels = 0.0f;
    float ownInkRightInAtlasPixels = 0.0f;
    /// This column's ink itself, in the same atlas pixels: the span above
    /// before the producer's fringe widened it and before neighbouring
    /// columns were settled against it. The slice above is what the column
    /// draws; this is where its letter ends, and so where a rule that follows
    /// the ink ends.
    float inkLeftInAtlasPixels = 0.0f;
    float inkRightInAtlasPixels = 0.0f;
    /// Whether the producer measured ink ownership for this strip. False
    /// keeps the consumer on the geometric cell slice, so a backend whose
    /// rasteriser cannot report per-glyph coverage renders exactly as it
    /// did before ownership existed.
    bool hasOwnedInk = false;
    /// The glyph the shaper chose for this column, valid when
    /// `hasShapedGlyph`.
    ///
    /// The strip above and this answer come out of ONE shaping pass and say
    /// the same thing two ways: the strip says which pixels, keyed by the text
    /// that produced them, and this says which glyph, keyed by the face's own
    /// identity. A producer fills whichever it can, and a producer that fills
    /// both costs nothing extra — the shaper already answered.
    ///
    /// Why the second answer exists: a strip's key is the run's text, and text
    /// fragments do not repeat, while the glyphs a face actually draws saturate.
    /// A slot keyed by the glyph is therefore bounded where a slot keyed by the
    /// fragment is not.
    ShapedCell shaped;
    /// Whether the producer reported `shaped`. False keeps the consumer on the
    /// strip, so a backend whose shaper cannot report glyph identities renders
    /// from the strip alone.
    bool hasShapedGlyph = false;
    /// The strip's inked rows, in the same atlas pixels as the slot: the first
    /// inked row and one past the last, valid when `hasInkRows`.
    ///
    /// The strip rect spans the whole slot, margins included, and a quad
    /// drawn over all of it reaches past the cell box above and below whether
    /// or not any ink does. Sized to these rows, with a guard row a side for
    /// the sampler, the quad carries the same pixels, and the rows say where
    /// the ink is, which a retained frame reads to tell a strip that crosses
    /// into the next row from one that does not.
    bool hasInkRows = false;
    float inkTopInAtlasPixels = 0.0f;
    float inkBottomInAtlasPixels = 0.0f;
    int startColumn = 0;
    int length = 0;
};

/// Hands one covered column the inked rows its strip's raster measured.
///
/// - Parameters:
///   - run: The column's run.
///   - rows: The slot's measured rows, unmeasured for a slot no raster reported.
inline void attachInkRows(LigatureRun& run, const GlyphSlotCache::InkRows& rows)
{
    run.hasInkRows = rows.isMeasured;
    run.inkTopInAtlasPixels = rows.topInPixels;
    run.inkBottomInAtlasPixels = rows.bottomInPixels;
}

/// Hands one covered column the ownership its strip's ledger measured.
///
/// Every producer copies the ledger into its runs through this one call, so a
/// field the ledger gains reaches every arm's runs together.
///
/// - Parameters:
///   - run: The column's run, which takes the extent and is marked measured.
///   - extent: The column's entry in the slot's owned-ink ledger.
inline void attachOwnedInk(LigatureRun& run, const GlyphSlotCache::OwnedInkExtent& extent)
{
    run.hasOwnedInk = true;
    run.ownInkLeftInAtlasPixels = extent.leftInPixels;
    run.ownInkRightInAtlasPixels = extent.rightInPixels;
    run.inkLeftInAtlasPixels = extent.inkLeftInPixels;
    run.inkRightInAtlasPixels = extent.inkRightInPixels;
}

/// A column's ligature-shaping inputs, distilled from its cell by Core.
///
/// Code points alone are not enough to shape a row, which is the one place this
/// contract nearly kept a model type. Shaping asks two questions per column and
/// both are answered from the PEN: whether the cell may shape at all
/// (``ligatureCellEligible`` — printable base-font ASCII, and none of the eight
/// attributes that force the per-glyph path), and whether two columns belong to
/// one run (``ligatureCellsHaveSameShape`` — bold, variable weight,
/// super/subscript, font selection, proportional). A backend handed
/// `const std::uint32_t*` would have to answer both from code points it cannot
/// answer them from, and would join runs across a style boundary.
///
/// Core answers both here, so the backend shapes from two integers per column
/// and the contract still names no model type. ``distilLigatureRow``
/// (`Core/Rendering/CellDecisions.h`) is the one producer, and both the portable
/// builder and the macOS inline walk go through it — a second distillation would
/// be a second answer to "may these two join", and the arms would split runs
/// differently on exactly the rows a parity comparison is least likely to hold.
struct UBIQUITTY_ESCAPABLE LigatureCell {
    /// The character to shape, or zero on a column that may not shape at all.
    ///
    /// An eligible cell is printable base-font ASCII (0x20..0x7E), so zero is a
    /// value no eligible column can hold and needs no second field to mean
    /// "skip me".
    std::uint32_t codePoint = 0;

    /// Opaque shaping identity: two columns join one run only if these are equal.
    ///
    /// Colour is not among the axes it folds. A run that split where only paint
    /// changed would re-anchor its pen, and unrelated cells would wobble when
    /// their styling changed and their shape did not.
    ///
    /// ``ligatureShapeBold`` is bit 0 because a backend needs bold before it can
    /// pick a face or key a strip. The rest is opaque, so the packing is not part
    /// of the contract.
    std::uint32_t shape = 0;

    /// The raster variant the cell's paint selects (GlyphKey.h: the smoothing
    /// tier, no phase), filled by the builder after the distil.
    ///
    /// Kept out of ``shape`` on purpose: a run still joins across a colour
    /// change. A backend whose rasteriser weights a strip by its fill keys the
    /// strip under each variant its cells use and hands every cell the strip
    /// drawn at its own, so the run stays one pen and every cell its own weight.
    std::uint32_t variant = 0;
};

/// The bold bit inside ``LigatureCell::shape``.
inline constexpr std::uint32_t ligatureShapeBold = 1u;

/// The bits of ``LigatureCell::shape`` that name a FACE rather than a layout.
///
/// Bold and the variable weight beside it choose which face draws a cell;
/// everything else in the identity — script, font selection, proportional
/// spacing, the sized-text band — changes where a glyph goes or how large it
/// is, which one shaped image cannot express whatever the shaper can do.
///
/// Bit 0 is bold and bits 1 to 3 the variable weight, which is where
/// ligatureShapeIdentity packs them; the script field begins at bit 4 and
/// stays outside this mask.
///
/// A backend that shapes one run through several faces is allowed to ignore
/// exactly these bits when it decides where a run ends, because the healed
/// form of a letter depends on the letter beside it and not on the weight it
/// is drawn at.
inline constexpr std::uint32_t ligatureShapeFaceMask = 0x0Fu;

/// Whether a backend can shape one run through several faces.
///
/// CoreText can: an attributed line carrying a bold range and a regular range
/// shapes as one line, and the contextual forms cross the boundary — a bold
/// `t` before a regular `mux` still heals the `m`. A shaper handed one face
/// per call cannot, and for it a face change has to end the run, or half the
/// run would be drawn in the wrong weight.
///
/// Core owns the rule; the backend states which it is, because the answer is
/// a property of the shaper it brought.
enum class LigatureFaceMixing : std::uint8_t {
    /// A face change ends the run.
    SplitByFace,
    /// A face change stays inside the run, and the backend draws each cell in
    /// its own face.
    ShapesMixedFaces,
};

/// The longest run one strip holds, in drawn cells.
///
/// A same-shape stretch longer than this is split into chunks of this many
/// cells, bridged by shaping context, so the font's rules still see across
/// the split. Part of the seam contract rather than a backend tunable: two
/// backends capping differently would split one row's runs differently, and
/// the arms would disagree exactly where a comparison is least likely to
/// hold.
///
/// Sixteen rather than a longer run because every slot is cut for this many
/// cells whether its run needs them or not, and a screen's runs measure a
/// median of five: the width past the median is a cache that holds a quarter
/// of the strips it could.
///
/// Not shorter, though the font's contextual rules were measured to reach
/// only two cells and every chunk is shaped with ``ligatureContextCells`` of
/// real text on each side. That argument covers which glyph a chunk draws,
/// not where it draws: a seam re-anchors the pen, so the halves of a split
/// run take separate subpixel phases and the join moves by a fraction of a
/// pixel. Eight cells is measurably visible at a fractional advance.
inline constexpr int ligatureMaxCells = 16;

/// How many cells wide a strip slot is cut.
///
/// Ligature runs fit one slot; proportional runs retain their complete
/// shaping context and tile across slots with disjoint visible interiors.
inline constexpr int stripSlotCells = 32;

/// How much texture one strip sheet may spend, in bytes of R8 coverage.
///
/// Slots are cut for the longest run the sheet accepts, so a sheet sized for
/// the worst strip count at the widest slot asks for two extremes at once and
/// costs an order of magnitude more than the ink it holds. The budget caps
/// that; a sheet that hits it reports the strips it then had to refuse rather
/// than thrashing quietly.
///
/// Per mode because the populations were measured and differ by a factor of
/// three: a screen of prose under a healing face wants ~1,200 distinct
/// strips, a screen of code under an operator face ~380. Both budgets carry
/// better than twice their screen, which is what the retained pins need — a
/// sheet holding exactly one screen has nothing evictable once that screen's
/// pins are taken.
///
/// Sized against a ``stripSlotCells`` slot. The slot was widened from the
/// ligature chunk to the widest proportional run, which doubled the bytes a
/// slot costs; the operator budget rose with it so the sheet still carries
/// twice its screen, while the prose budget already had that headroom.
///
/// - Parameter participation: The sheet's probe verdict, which decides
///   whether the population is every word or only the operators.
/// - Returns: The bytes of R8 coverage the sheet may spend.
inline constexpr int ligatureStripSheetByteBudget(LigatureParticipation participation)
{
    return participation == LigatureParticipation::AllEligibleRuns ? 48 * 1024 * 1024 : 12 * 1024 * 1024;
}

/// Same-shape cells fed to the shaper on each side of a run.
///
/// Context cells are shaped but never drawn — they exist so contextual rules
/// see across a run's edges, most importantly across the ``ligatureMaxCells``
/// splits of one long stretch. Contract for the same reason the cap is: the
/// context length changes which alternates the font picks.
inline constexpr int ligatureContextCells = 8;

/// How far a chunk seam may retreat from the cap to reach a safe cut.
///
/// A seam through the middle of a multi-cell ligature loses ink: the font is
/// free to shape the cluster as zero-advance glyphs whose one carrier draws
/// the whole image, and the carrier's back-reach exceeds the strip slot's
/// one-cell margin — `///` shaped at the cap loses its first cell exactly
/// this way. Context cells cannot help, because they decide which glyphs a
/// chunk draws, not whether a neighbouring chunk's cells own ink at all. So
/// the cut walks back from the cap to a boundary no cluster spans, and this
/// is how far it may walk before giving up and cutting hard. Bounded above
/// by ``ligatureContextCells`` so the seam's flanks still shape inside each
/// other's context, and comfortably past the longest cluster a coding face
/// composes.
inline constexpr int ligatureSeamWindowCells = 8;

} // namespace Ubiquitty::Rendering
