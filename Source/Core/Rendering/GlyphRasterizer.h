// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Source/Core/Rendering/GlyphRasterizer.h — the RASTER SEAM: the portable contract a
// glyph atlas calls to fill a slot with a glyph's coverage. It sits BELOW the
// GlyphBackend seam (Rendering/GlyphBackend.h): the atlas owns the slot
// geometry / cache / pixel storage and asks the rasterizer for a coverage mask,
// which it blits into the slot.
//
// Two implementations live in the tree:
//   * UnifontRasterizer (portable) — the embedded 1-bit GNU Unifont downscaled
//     into the slot; the byte-for-byte reference raster and no-font fallback.
//   * Platform real-font rasterizers — anti-aliased 8-bit coverage.
//
// Both the CPU SoftwareRenderer and the GPU VulkanGlyphAtlas take the same
// GlyphRasterizer&, so injecting one makes their coverage byte-identical. Pure
// C++: no platform font types (enforced by Scripts/check-boundaries.py).

#include "Core/Rendering/CellMetrics.h"
#include "Core/Rendering/FontMetrics.h"
#include "Core/Rendering/GlyphKey.h"
#include "Core/Rendering/GlyphSlotPolicy.h"
#include "Core/Unicode/Width.h"
#include "PAL/TextShaping.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>
#include <vector>

namespace Ubiquitty::Rendering {

/// The factor a scaled raster is drawn at, as a fraction of the cell's glyph.
///
/// Whole for an OSC 66 block or a doubled row, and `s * n / d` for a block
/// drawn at a fraction of its size, which a rasterizer draws at exactly that
/// size rather than at the nearest whole one. Implicit from a whole number, so
/// a call site with a whole scale passes the number alone.
struct RasterScale {
    int numerator = 1;
    int denominator = 1;

    constexpr RasterScale() = default;
    // NOLINTNEXTLINE(google-explicit-constructor): a whole scale is a scale.
    constexpr RasterScale(int whole)
        : numerator(whole)
    {
    }
    constexpr RasterScale(int top, int bottom)
        : numerator(top)
        , denominator(bottom > 0 ? bottom : 1)
    {
    }

    /// - Returns: True when the factor is a whole number.
    [[nodiscard]] constexpr bool isWhole() const { return numerator % denominator == 0; }
    /// - Returns: The whole factor, rounded down when the factor is not whole.
    [[nodiscard]] constexpr int whole() const { return numerator / denominator; }
    /// - Returns: The factor.
    [[nodiscard]] constexpr double value() const { return static_cast<double>(numerator) / denominator; }
    /// Scales a whole length to the nearest whole length; exact for a whole factor.
    ///
    /// - Parameter length: The unscaled length.
    /// - Returns: The scaled length.
    [[nodiscard]] int scaled(int length) const
    {
        return isWhole() ? length * whole() : static_cast<int>(std::lround(static_cast<double>(length) * numerator / denominator));
    }
    /// Scales a whole length up to the whole length that holds it; exact for a whole factor.
    ///
    /// - Parameter length: The unscaled length.
    /// - Returns: The scaled length, rounded up.
    [[nodiscard]] constexpr int scaledToHold(int length) const
    {
        return (length * numerator + denominator - 1) / denominator;
    }
    /// Recovers the unscaled length of a scaled one; exact for a whole factor.
    ///
    /// - Parameter length: The scaled length.
    /// - Returns: The unscaled length, rounded down.
    [[nodiscard]] constexpr int unscaled(int length) const { return numerator > 0 ? length * denominator / numerator : length; }
};

/// The raster a sized window is cut from, as Core selects it
/// (``selectSizedMaster`` in SizedMaster.h).
///
/// One master per face, target device size and horizontal scale: the glyph is
/// drawn at `scale` times its em, from the face at `widthAxisCoordinate` on its
/// `wdth` axis, with its outline stretched across by `horizontalScale`, so its
/// advance is `scale * widthScale` times the cell's, and moved by `shiftX` and
/// `shiftY`. A rasterizer draws exactly that and nothing it decides itself.
struct SizedMaster {
    /// The em factor, which is also the vertical magnification.
    RasterScale scale;
    /// The doubling across over `scale`: 2 on a DECDWL row, 1 otherwise.
    int widthScale = 1;
    /// The `wdth` coordinate the face is drawn at; zero draws the configured face.
    double widthAxisCoordinate = 0.0;
    /// The outline's horizontal factor past `scale`: what of `widthScale` the
    /// width axis left. 1 where `widthScale` is 1.
    double horizontalScale = 1.0;
    /// How far the glyph moves from the placement ``sizedMasterPen`` gives it,
    /// in device pixels of the master: right and down are positive.
    ///
    /// The window cut from the master begins on a whole texel, so what the cut
    /// cannot say is drawn into the master instead: the block's subpixel pen
    /// phase and the fraction a window's step leaves. A rasterizer places the
    /// glyph as finely as its own grid allows, and one that cannot — the base
    /// implementation magnifies the 1x raster by whole texels — leaves the
    /// glyph where it is, which is off by at most half a texel.
    double shiftX = 0.0;
    double shiftY = 0.0;

    /// - Returns: How many times the cell's width the master's advance is.
    [[nodiscard]] constexpr RasterScale horizontalMagnification() const { return { scale.numerator * widthScale, scale.denominator }; }
    /// - Returns: True when the master is the cell's own glyph.
    [[nodiscard]] constexpr bool isIdentity() const { return scale.isWhole() && scale.whole() <= 1 && widthScale == 1; }
    /// - Returns: True when the outline is drawn as the face designs it at
    ///   `scale`, with no widening and no stretch across.
    [[nodiscard]] constexpr bool isUniform() const { return widthScale == 1 && widthAxisCoordinate == 0.0 && horizontalScale == 1.0; }
};

/// Where a face's `wdth` axis stands and how far it reaches, in design units.
///
/// `maximum <= current` means the face has no room to widen, which includes a
/// face with no axis at all.
struct WidthAxisRoom {
    double current = 0.0;
    double maximum = 0.0;

    /// - Returns: True when the axis can widen the face past where it stands.
    [[nodiscard]] constexpr bool canWiden() const { return current > 0.0 && maximum > current; }
};

struct GlyphRasterizer {
    // The cell geometry this rasterizer's glyphs are sized for: the cell
    // box (cellWidth × cellHeight, in device px) the backend sizes its atlas
    // slots to, plus the decoration metrics (baseline, underline, x-height)
    // the builder positions rules against. The backend queries this once
    // (after the rasterizer is injected, before allocating the atlas) so a
    // font-derived cell — not a hardcoded one — drives the whole pipeline.
    // CellMetrics is a plain POD (Rendering/CellMetrics.h), so this keeps the
    // seam portable.
    [[nodiscard]] virtual CellMetrics cellMetrics() const = 0;

    /// The primary face's pitch: ``FontMetrics::facePitch`` over the unhinted
    /// advances the cell was measured from. A proportional primary lays its
    /// rows out as proportional runs (Rendering/LetterLayout.h), and the
    /// backend adapter publishes this verdict as ``GlyphBackend::primaryFacePitch``.
    ///
    /// - Returns: `Fixed` unless the face's printable-ASCII advances differ.
    [[nodiscard]] virtual FontMetrics::FacePitch primaryFacePitch() const { return FontMetrics::FacePitch::Fixed; }

    // The font's overhang metrics (primary + glyph-fallback face bounding boxes,
    // baseline-relative y-up, in atlas px) the core feeds Rendering::computeGlyphSlot
    // to size the wide-glyph CANVAS for the subpixel overflow pass. The default is
    // ZERO overhang — the cell-tight, cell-baked path (Unifont / no real font); a
    // real-font rasterizer overrides it so the slot grows to hold spilling ink.
    [[nodiscard]] virtual GlyphOverhangInputs glyphOverhang() const { return {}; }

    /// The rows a proportional strip reaches past the cell above and below,
    /// for a face whose proportional glyphs do not fit the grid's cell: the
    /// strip's slot grows by it on both sides, and the run's quad with it.
    /// Zero for a face whose slot margins already hold its ink.
    ///
    /// - Returns: The rows per side, in pixels.
    [[nodiscard]] virtual int proportionalStripOverhangInPixels() const { return 0; }

    /// The face's own vertical metrics, for geometry measured against the type
    /// rather than against the cell.
    ///
    /// Separate from ``cellMetrics`` because the cell has the line spacing folded
    /// into its baseline and the face does not, and a consumer substituting one
    /// for the other is right only while that spacing is zero.
    ///
    /// The default reports a zero font size, which every consumer reads as "this
    /// rasterizer keeps no face" and draws no type-relative geometry — so a
    /// rasterizer that does not override this loses nothing it had.
    ///
    /// - Returns: The em size, ascent, descent and the line gap above the ascent.
    [[nodiscard]] virtual FaceMetrics faceMetrics() const { return {}; }

    // Rasterize `codePoint` (with the requested style) into `coverageOut`, a
    // caller-owned slotWidth*slotHeight R8 buffer, row-major, top-left origin.
    // Fill it with the glyph's coverage (0 = background, 255 = full ink, partial =
    // antialiased edge), positioned/fit into the slot, and return true. Return
    // false when this rasterizer has no glyph for the code point — the caller
    // then leaves the slot blank (or delegates to a fallback). The caller does
    // not pre-clear the buffer; an implementation that returns true must write
    // every pixel it leaves visible (clear-then-draw).
    virtual bool rasterizeGlyph(std::uint32_t codePoint, FontFace face, std::uint32_t weight, int slotWidth, int slotHeight, std::uint8_t* coverageOut) = 0;

    // Draws a grapheme cluster into one coverage slot. A shaping rasterizer
    // overrides this and positions the marks; the portable fallback has no
    // shaper, so it draws each code point over the last and keeps the darker
    // ink per pixel, which is enough for a combining mark to land on its base.
    // A code point that selects a form rather than carrying one is skipped, so
    // a cluster of nothing but selectors declines and leaves the slot blank.
    virtual bool rasterizeCluster(const std::uint32_t* codePoints, int count, FontFace face, std::uint32_t weight, int slotWidth, int slotHeight, std::uint8_t* coverageOut)
    {
        if (count <= 0 || !codePoints || slotWidth <= 0 || slotHeight <= 0)
            return false;
        const std::span<std::uint8_t> composite(coverageOut, static_cast<std::size_t>(slotWidth) * static_cast<std::size_t>(slotHeight));
        std::ranges::fill(composite, std::uint8_t { 0 });
        std::vector<std::uint8_t> glyph(composite.size());
        bool drewSomething = false;
        for (const std::uint32_t codePoint : std::span(codePoints, static_cast<std::size_t>(count))) {
            if (Ubiquitty::Unicode::Width::isInvisibleFormat(codePoint) || !rasterizeGlyph(codePoint, face, weight, slotWidth, slotHeight, glyph.data()))
                continue;
            // A mark overprints its base: wherever either has ink, the darker wins.
            std::ranges::transform(composite, glyph, composite.begin(), [](std::uint8_t under, std::uint8_t over) { return std::max(under, over); });
            drewSomething = true;
        }
        return drewSomething;
    }

    /// Whether ``rasterizeSizedMaster`` draws at the master's size rather than
    /// magnifying the 1x raster.
    ///
    /// The two answers are pictures, not qualities: an outline face asked for
    /// three times its size draws three times the detail, and a bitmap face
    /// asked the same duplicates pixels. A caller spends an atlas slot per
    /// window of a sized block, and spending one to reproduce the magnified
    /// crop it would otherwise get for free is the trade this predicate exists
    /// to refuse.
    ///
    /// False by default, because the base implementation is itself the
    /// magnification — so a rasterizer that does not override the raster must not
    /// claim the sharpness, and one that does says so here.
    ///
    /// - Returns: True when a scaled raster carries detail the 1x one lacks.
    [[nodiscard]] virtual bool rasterisesScaledGlyphsSharply() const { return false; }

    /// Rasterizes a glyph at `scale` times the cell's size into a slot that size.
    ///
    /// The uniform master: ``rasterizeSizedMaster`` with no doubling across.
    ///
    /// - Parameters:
    ///   - codePoint: The character to draw.
    ///   - face: Which face to draw it in.
    ///   - weight: Variable-font weight, or zero for the face's own.
    ///   - scale: The factor, whole or a fraction.
    ///   - slotWidth: The slot's width, the cell slot's scaled.
    ///   - slotHeight: The slot's height, the cell slot's scaled.
    ///   - coverageOut: `slotWidth * slotHeight` coverage bytes.
    /// - Returns: False when nothing could draw the glyph.
    bool rasterizeGlyphScaled(std::uint32_t codePoint, FontFace face, std::uint32_t weight, RasterScale scale, int slotWidth, int slotHeight, std::uint8_t* coverageOut)
    {
        SizedMaster master;
        master.scale = scale;
        return rasterizeSizedMaster(codePoint, face, weight, master, slotWidth, slotHeight, coverageOut);
    }

    /// Rasterizes the master Core selected into a slot `master` times the cell slot.
    ///
    /// The slot is `horizontalMagnification` times the cell slot across and
    /// `scale` times it down, and the glyph's cell box is centred in it, then
    /// moved by the master's shift. The default magnifies the 1x raster by nearest
    /// neighbour, which is exactly a bitmap face's doubling and the floor for any
    /// other, and leaves the shift to the cut; an outline rasterizer overrides it
    /// and says so in ``rasterisesScaledGlyphsSharply``.
    ///
    /// - Parameters:
    ///   - codePoint: The character to draw.
    ///   - face: Which face to draw it in.
    ///   - weight: Variable-font weight, or zero for the face's own.
    ///   - master: The size, widening, stretch and phase grid to draw at.
    ///   - slotWidth: The slot's width.
    ///   - slotHeight: The slot's height.
    ///   - coverageOut: `slotWidth * slotHeight` coverage bytes.
    /// - Returns: False when nothing could draw the glyph.
    virtual bool rasterizeSizedMaster(std::uint32_t codePoint, FontFace face, std::uint32_t weight, const SizedMaster& master, int slotWidth, int slotHeight, std::uint8_t* coverageOut)
    {
        if (master.isIdentity())
            return rasterizeGlyph(codePoint, face, weight, slotWidth, slotHeight, coverageOut);
        const RasterScale across = master.horizontalMagnification();
        const RasterScale down = master.scale;
        const int baseWidth = across.unscaled(slotWidth);
        const int baseHeight = down.unscaled(slotHeight);
        if (baseWidth < 1 || baseHeight < 1)
            return false;
        std::vector<std::uint8_t> base(static_cast<std::size_t>(baseWidth) * static_cast<std::size_t>(baseHeight));
        if (!rasterizeGlyph(codePoint, face, weight, baseWidth, baseHeight, base.data()))
            return false;
        for (int y = 0; y < slotHeight; ++y) {
            const int sourceY = std::min(down.unscaled(y), baseHeight - 1);
            const std::uint8_t* sourceRow = &base[static_cast<std::size_t>(sourceY) * baseWidth];
            std::uint8_t* destinationRow = &coverageOut[static_cast<std::size_t>(y) * slotWidth];
            for (int x = 0; x < slotWidth; ++x)
                destinationRow[x] = sourceRow[std::min(across.unscaled(x), baseWidth - 1)];
        }
        return true;
    }

    /// Where the face's `wdth` axis stands for `face` and how far it reaches.
    ///
    /// A fact for Core's master selection; the default has no axis, so a
    /// doubled row is the face stretched across by two.
    ///
    /// - Parameter face: The style asked about.
    /// - Returns: The axis position and maximum, or no room.
    [[nodiscard]] virtual WidthAxisRoom widthAxisRoom(FontFace face) const
    {
        (void)face;
        return {};
    }

    /// The cell advance the face draws at `coordinate` on its `wdth` axis.
    ///
    /// Measured, not derived: an axis widens letters by design, not linearly,
    /// so the stretch left over is the ratio of two measured advances.
    ///
    /// - Parameters:
    ///   - face: The style asked about.
    ///   - coordinate: The `wdth` design coordinate.
    /// - Returns: The advance in device pixels, or zero when it cannot be measured.
    [[nodiscard]] virtual double cellAdvanceAtWidthAxis(FontFace face, double coordinate)
    {
        (void)face;
        (void)coordinate;
        return 0.0;
    }

    /// Whether this rasterizer can produce colour glyphs at all.
    ///
    /// Asked once, before an atlas is allocated, and separate from
    /// ``rasterizeColorCluster`` returning false for one cluster: a rasterizer
    /// with a colour source that has no glyph for this emoji declines a cluster,
    /// and one with no colour source at all has nothing to offer any of them.
    /// The distinction is what lets a backend leave ``GlyphBackend::colorGlyph``
    /// null and so report itself lossy, rather than claiming a colour path that
    /// answers absent every time.
    ///
    /// - Returns: True when ``rasterizeColorCluster`` can succeed for some input.
    [[nodiscard]] virtual bool hasColorGlyphs() const { return false; }

    /// Rasterises a colour glyph as RGBA8 into a square slot.
    ///
    /// `pixelsOut` is a caller-owned `slotInPixels * slotInPixels * 4` buffer,
    /// row-major, top-left origin, one byte per channel in R, G, B, A order.
    /// Straight alpha rather than premultiplied, because the sheet is sampled by
    /// a shader on one frontend and composited on the CPU by another, and the
    /// two disagree about premultiplication far more readily than about order.
    ///
    /// Square because the sheet is: ``ColorAtlasBinding`` carries one
    /// `slotInPixels` for both axes, and an emoji is drawn to its own square box
    /// rather than to the cell, which is why the binding insets its UV rect.
    ///
    /// `count == 1` is a lone code point and more is a cluster — a flag sequence
    /// or an emoji with a variation selector or a ZWJ join. A rasterizer may
    /// serve the two by different routes, and the count is how it tells.
    ///
    /// The caller does not pre-clear the buffer, so an implementation returning
    /// true must write every pixel it leaves visible.
    ///
    /// - Parameters:
    ///   - codePoints: The cluster, already normalised by the caller.
    ///   - count: How many code points; 1 is a lone one.
    ///   - slotInPixels: The square slot edge.
    ///   - pixelsOut: The RGBA8 destination.
    /// - Returns: True when the cluster was drawn; false leaves it to the mono path.
    virtual bool rasterizeColorCluster(const std::uint32_t* codePoints, int count, int slotInPixels, std::uint8_t* pixelsOut)
    {
        (void)codePoints;
        (void)count;
        (void)slotInPixels;
        (void)pixelsOut;
        return false;
    }

    /// One glyph a shaper placed, in whole pixels.
    ///
    /// Carried here rather than in the backend that shapes, because the whole
    /// point of shaping on the rasterizer is that two backends drawing the same
    /// run get the same glyphs at the same offsets. A type owned by one of them
    /// puts the other on a conversion, and a conversion is where two arms start
    /// to differ.
    ///
    /// Whole pixels, already rounded: a shaper works in a fixed-point subunit
    /// (HarfBuzz's 26.6), and rounding once at this seam is what makes the two
    /// arms agree rather than each rounding its own way at its own time.
    struct ShapedGlyph {
        /// Face-local glyph index, meaningless outside the face that shaped it.
        unsigned int glyph = 0;
        /// Pen advance.
        int advanceInPixels = 0;
        /// Offset from the pen, y up.
        int offsetXInPixels = 0;
        int offsetYInPixels = 0;
        /// Which entry in `codePoints` this glyph came from. A ligature's
        /// glyphs share one cluster, which is how a caller maps ink back to
        /// cells after substitution has changed the count.
        int cluster = 0;
    };

    /// Reports whether this rasterizer can shape a run into ligatures.
    ///
    /// Defaults to no, so every existing rasterizer keeps refusing without an
    /// edit and only one that opts in changes any picture. The same shape as
    /// ``hasColorGlyphs`` above and for the same reason: a backend consults this
    /// to decide whether to publish ``GlyphBackend::shapeLigatureRuns`` at all,
    /// and a null there means text renders unligatured rather than wrongly.
    ///
    /// Why it lives on the rasterizer and not on the backend: a frontend runs a
    /// CPU and a GPU backend over one screen and compares them byte for byte,
    /// and that comparison holds only while both fault through the same
    /// instance. Shaping reached through a second, separately-installed handle
    /// gave one arm ligatures and the other none, which the comparison correctly
    /// reported as a disagreement of 1083 pixels.
    ///
    /// - Returns: True when ``shapeTextRun`` can substitute for some input.
    [[nodiscard]] virtual bool hasLigatureShaping() const { return false; }

    /// Shapes a run of same-attribute cells against the primary text face.
    ///
    /// For the ligatures that live in a face's GSUB table and are unreachable
    /// through its character map: the run goes in as code points and comes back
    /// as placed glyphs, which is the only way to see a substitution that turns
    /// three cells into one piece of ink.
    ///
    /// `glyphsOut` is caller-owned and its size bounds the answer. Shaping can
    /// return FEWER glyphs than code points — that is what a ligature is — and a
    /// face with contextual forms can return more, so the count comes back
    /// rather than being assumed from the input.
    ///
    /// - Parameters:
    ///   - codePoints: The run, in visual order, including any context the
    ///     caller wants the face's rules to see.
    ///   - weight: The variable-font weight axis; 0 leaves the face as it is.
    ///   - glyphsOut: Caller-owned destination; its size is the bound.
    /// - Returns: How many entries of `glyphsOut` were written; 0 means the run
    ///   was not shaped and the caller keeps its unligatured path.
    virtual int shapeTextRun(std::span<const std::uint32_t> codePoints, std::uint32_t weight, std::span<ShapedGlyph> glyphsOut)
    {
        (void)codePoints;
        (void)weight;
        (void)glyphsOut;
        return 0;
    }

    /// Shapes a run of same-pen cells in the primary face at its natural
    /// advances, for a proportional primary face's rows
    /// (Rendering/LetterLayout.h, ``GlyphBackend::shapePrimaryRun``).
    ///
    /// ``shapeTextRun`` against the same face, answering the fraction the
    /// shaper placed each glyph at rather than whole pixels, and without
    /// discretionary ligatures: the builder draws each letter from the mono
    /// sheet at the pen this answer puts it on, so a fused glyph would still
    /// draw as its letters. The face is the pen's — bold and italic take
    /// their own faces, since a bold letter is wider than its regular.
    ///
    /// - Parameters:
    ///   - codePoints: The run's text, cell after cell with each cell's marks.
    ///   - face: Regular, bold, italic or both.
    ///   - weight: The variable-font weight axis; 0 leaves the face as it is.
    ///   - glyphsOut: Caller-owned destination; its size is the bound.
    /// - Returns: How many entries of `glyphsOut` were written; 0 refuses, and
    ///   the run then keeps the grid's pitch.
    virtual int shapePrimaryRun(std::span<const std::uint32_t> codePoints, FontFace face, std::uint32_t weight, std::span<Ubiquitty::ProportionalShapedGlyph> glyphsOut)
    {
        (void)codePoints;
        (void)face;
        (void)weight;
        (void)glyphsOut;
        return 0;
    }

    /// Draws placed glyphs as 8-bit coverage into a caller-owned strip.
    ///
    /// The other half of ``shapeTextRun``, separate from it so a caller can
    /// shape once and draw the result into whichever slot its own atlas hands
    /// back — the same division as ``rasterizeColorCluster``, where the
    /// rasterizer owns the ink and the caller owns the storage.
    ///
    /// The caller does not pre-clear, so an implementation returning true must
    /// write every pixel it leaves visible.
    ///
    /// - Parameters:
    ///   - glyphs: Placed glyphs, offsets already carrying the caller's pen.
    ///   - stripWidthInPixels: The destination's width.
    ///   - stripHeightInPixels: The destination's height.
    ///   - coverageOut: Caller-owned `stripWidth * stripHeight` bytes, row-major.
    /// - Returns: True when the strip was drawn.
    virtual bool rasteriseShapedRun(std::span<const ShapedGlyph> glyphs, int stripWidthInPixels, int stripHeightInPixels, std::span<std::uint8_t> coverageOut)
    {
        (void)glyphs;
        (void)stripWidthInPixels;
        (void)stripHeightInPixels;
        (void)coverageOut;
        return false;
    }

    /// One glyph's horizontal coverage inside a drawn strip.
    struct GlyphInkExtent {
        /// The leftmost strip x the glyph's coverage reaches, inclusive.
        int leftInPixels = 0;
        /// One past the rightmost strip x the glyph's coverage reaches.
        /// At or below `leftInPixels`, the glyph drew nothing.
        int rightInPixels = 0;
    };

    /// Whether a proportional face stands beside the grid face, for
    /// proportional runs (Docs/proportional-text.md).
    ///
    /// False leaves every proportional run to the grid-face fallback the
    /// builder draws itself: a strip sheet publishes the proportional hook
    /// only when this answers true, because the hook moves the run off the
    /// fallback, and a hook with no face behind it would draw nothing.
    ///
    /// - Returns: True when ``shapeProportionalRun`` can shape.
    [[nodiscard]] virtual bool hasProportionalFace() const { return false; }
    /// Shapes a proportional run in the proportional face at its natural
    /// advances.
    ///
    /// The same contract as ``shapeTextRun`` — `glyphsOut` bounds the answer,
    /// the count comes back — against the proportional face instead of the
    /// grid's, regular or bold by `face`. The advances are the face's own: a
    /// proportional run keeps them, which is what makes it proportional.
    ///
    /// Callable from a thread other than the drawing one, because the
    /// measurement a query asks for arrives on the reader thread; an
    /// implementation sharing state between the two guards it.
    ///
    /// - Parameters:
    ///   - codePoints: The run's text.
    ///   - face: Regular or bold; italic resolves to its upright.
    ///   - glyphsOut: Caller-owned destination; its size is the bound. Its
    ///     advances and offsets keep the shaper's fraction: a proportional run
    ///     is placed by Core in pixels and a fraction lost here moves every
    ///     later glyph left of where the face put it, which a strip of
    ///     grid-aligned ligatures never shows and a run of prose always does.
    /// - Returns: How many entries of `glyphsOut` were written; 0 refuses.
    virtual int shapeProportionalRun(std::span<const std::uint32_t> codePoints, FontFace face, std::span<Ubiquitty::ProportionalShapedGlyph> glyphsOut)
    {
        (void)codePoints;
        (void)face;
        (void)glyphsOut;
        return 0;
    }
    /// Draws glyphs ``shapeProportionalRun`` placed, as ``rasteriseShapedRun``
    /// does, in the proportional face.
    ///
    /// - Parameters:
    ///   - glyphs: Placed glyphs, offsets already carrying the caller's pen,
    ///     fractional; the rasteriser draws each at its fraction rather than
    ///     at the nearest whole pixel.
    ///   - face: The face the glyphs were shaped in.
    ///   - stripWidthInPixels: The destination's width.
    ///   - stripHeightInPixels: The destination's height.
    ///   - coverageOut: Caller-owned `stripWidth * stripHeight` bytes, row-major.
    /// - Returns: True when the strip was drawn.
    virtual bool rasteriseProportionalRun(std::span<const Ubiquitty::ProportionalShapedGlyph> glyphs, FontFace face, int stripWidthInPixels, int stripHeightInPixels, std::span<std::uint8_t> coverageOut)
    {
        (void)glyphs;
        (void)face;
        (void)stripWidthInPixels;
        (void)stripHeightInPixels;
        (void)coverageOut;
        return false;
    }
    /// Draws placed proportional glyphs at a sized master Core selected.
    ///
    /// ``rasteriseProportionalRun`` with the face at the master's em, widened
    /// on its `wdth` axis and stretched across as the master says. Offsets are
    /// in the master's pixels, and the baseline is the strip's at 1x taken
    /// `scale` times, so a DECDHL pair's halves join at the midline. The
    /// default declines, which leaves the builder its stretched quad.
    ///
    /// - Parameters:
    ///   - glyphs: Placed glyphs, offsets in master pixels.
    ///   - face: The face the glyphs were shaped in.
    ///   - master: The size, widening and stretch.
    ///   - stripWidthInPixels: The destination's width.
    ///   - stripHeightInPixels: The destination's height, `scale` times the 1x strip's.
    ///   - coverageOut: Caller-owned `stripWidth * stripHeight` bytes, row-major.
    /// - Returns: True when the strip was drawn.
    virtual bool rasteriseSizedProportionalRun(std::span<const Ubiquitty::ProportionalShapedGlyph> glyphs, FontFace face, const SizedMaster& master, int stripWidthInPixels, int stripHeightInPixels, std::span<std::uint8_t> coverageOut)
    {
        if (master.isIdentity())
            return rasteriseProportionalRun(glyphs, face, stripWidthInPixels, stripHeightInPixels, coverageOut);
        return false;
    }
    /// Draws placed glyphs as ``rasteriseShapedRun`` does, and reports where
    /// each one's ink landed.
    ///
    /// The extents are what lets a consumer cut a strip by ownership instead
    /// of by cell geometry: a contextual alternate's ink overhangs its cell
    /// box, and only the drawing pass knows by how much, because each
    /// bitmap's bearing and width pass through its hands at placement. An
    /// implementation that draws the strip must fill one extent per glyph;
    /// the default refuses instead, so a caller falls back to the unmeasured
    /// draw and the geometric cut — a strip is measured whole or not at all,
    /// never half.
    ///
    /// - Parameters:
    ///   - glyphs: Placed glyphs, offsets already carrying the caller's pen.
    ///   - stripWidthInPixels: The destination's width.
    ///   - stripHeightInPixels: The destination's height.
    ///   - coverageOut: Caller-owned `stripWidth * stripHeight` bytes, row-major.
    ///   - inkOut: Caller-owned, one entry per entry of `glyphs`.
    /// - Returns: True when the strip was drawn and every extent filled.
    virtual bool rasteriseShapedRunMeasured(std::span<const ShapedGlyph> glyphs, int stripWidthInPixels, int stripHeightInPixels, std::span<std::uint8_t> coverageOut, std::span<GlyphInkExtent> inkOut)
    {
        (void)glyphs;
        (void)stripWidthInPixels;
        (void)stripHeightInPixels;
        (void)coverageOut;
        (void)inkOut;
        return false;
    }

protected:
    ~GlyphRasterizer() = default;
};

} // namespace Ubiquitty::Rendering
