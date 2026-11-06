// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
// Source/Core/Rendering/BoxDrawingRasterizer.h — a GlyphRasterizer DECORATOR for
// box-drawing:
//   * BLOCK elements / shades / quadrants / sextants / wedges (prefersProcedural)
//     → cell-exact procedural coverage (BoxGlyphRaster); pure area fills, tiles
//     gaplessly.
//   * box-drawing LINES / corners / tees / crosses (U+2500–257F, minus dashes)
//     → the FONT glyph, vertically centred (Rendering::BoxDrawing::centerShiftY) and 9-slice
//     EDGE-STRETCHED (BoxEdgeStretch) so the font's strokes reach the cell edges
//     and tile; the font keeps its weight/style/antialiasing; procedural is the
//     fallback when the font ships no glyph.
//   * everything else (incl. dashes, non-analytic Powerline, icons) → the
//     wrapped font verbatim.
// Both Linux backends fault every glyph through one GlyphRasterizer&, so wrapping
// fixes both with the same coverage. Pure C++.

#include "Base/Geometry.h"
#include "Core/Rendering/BoxDrawing.h"
#include "Core/Rendering/BoxEdgeStretch.h"
#include "Core/Rendering/BoxGlyphRaster.h"
#include "Core/Rendering/BoxStrokeProfile.h"
#include "Core/Rendering/CellMetrics.h"
#include "Core/Rendering/GlyphRasterizer.h"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace Ubiquitty::Rendering {

class BoxDrawingRasterizer final : public GlyphRasterizer {
public:
    explicit BoxDrawingRasterizer(GlyphRasterizer& inner)
        : m_inner(&inner)
    {
    }

    // Re-point at the current font rasterizer after a font swap / scale change;
    // the family centring shift is metrics-keyed below, so it self-invalidates.
    void setInner(GlyphRasterizer& inner) { m_inner = &inner; }

    /// Sets how procedural shades are drawn.
    ///
    /// The owner rebuilds its sheet on a change, since the slots already
    /// rasterised keep the old style.
    ///
    /// - Parameter style: The configured shade style.
    void setShadeStyle(ShadeStyle style) { m_shadeStyle = style; }

    [[nodiscard]] CellMetrics cellMetrics() const override { return m_inner->cellMetrics(); }
    [[nodiscard]] GlyphOverhangInputs glyphOverhang() const override { return m_inner->glyphOverhang(); }
    // Forwarded like the two above, and for a sharper reason: the default
    // reports a zero font size, which every consumer reads as "this rasterizer
    // keeps no face" — so a decorator that forgot this would silently turn OFF
    // the caller's type-relative geometry (the OSC 133 bracket draws nothing at
    // a zero size) while every glyph still rendered. A decorator inherits its
    // inner's obligations, and the ones it drops fail quietly by construction.
    [[nodiscard]] FaceMetrics faceMetrics() const override { return m_inner->faceMetrics(); }
    // The primary face's pitch and its shaper: the backend adapter reads the
    // pitch through this decorator to publish it, and the letter layout asks
    // the shaper; the defaults would answer a fixed-pitch face with no shaper
    // and keep a proportional face on the grid.
    [[nodiscard]] FontMetrics::FacePitch primaryFacePitch() const override { return m_inner->primaryFacePitch(); }
    /// Shapes a run in the primary face at its natural advances, through the inner rasterizer.
    ///
    /// - Parameters:
    ///   - codePoints: The run's text.
    ///   - face: The pen's face.
    ///   - weight: The variable-font weight axis; 0 leaves the face as it is.
    ///   - glyphsOut: Caller-owned destination; its size is the bound.
    /// - Returns: How many entries were written; 0 refuses.
    int shapePrimaryRun(std::span<const std::uint32_t> codePoints, FontFace face, std::uint32_t weight, std::span<Ubiquitty::ProportionalShapedGlyph> glyphsOut) override
    {
        return m_inner->shapePrimaryRun(codePoints, face, weight, glyphsOut);
    }

    // The rest of the seam, forwarded for the reason above rather than because
    // this class has anything to say about any of them. Every default here is
    // plausible output rather than a failure, which is what makes a dropped one
    // invisible:
    //
    //   rasterizeSizedMaster   the default nearest-neighbour-upscales the 1x
    //                          raster, so a double-height row loses the real
    //                          re-rasterisation a vector face would give it and
    //                          still draws, at 1x resolution doubled
    //   hasColorGlyphs         the default reports no colour source, so the
    //                          caller leaves its colour hooks null and every
    //                          emoji draws as coverage in the pen colour
    //   rasterizeCluster       the default composites each code point at max
    //                          coverage, which is a shaper's output only for
    //                          the clusters where shaping changes nothing
    //
    // Measured: WebFontRasterizer overrides all three of the first two plus
    // rasterizeColorCluster, and before this every one of them was reaching the
    // base default through this class.
    /// Whether the inner rasterizer has a colour source.
    ///
    /// - Returns: Whatever the inner answered.
    [[nodiscard]] bool hasColorGlyphs() const override { return m_inner->hasColorGlyphs(); }

    // Shaping forwards for the same reason colour does, and the reason is worth
    // restating rather than cross-referenced: this class decorates the glyph
    // routes and answers the defaults for everything else, so a capability it
    // does not forward reads as absent on every face it wraps. The default here
    // is "no shaper", which is a plausible answer for a real font and so does
    // not look like a fault: rows render unligatured and nothing reports why.
    //
    // A frontend that hands one decorated instance to both its arms — which is
    // what keeps their coverage byte-identical — hands both of them that answer,
    // so the omission is invisible in a comparison of the two as well.
    [[nodiscard]] bool hasLigatureShaping() const override { return m_inner->hasLigatureShaping(); }

    /// Shapes a run on the wrapped face.
    ///
    /// Forwarded unchanged. Box drawing is decided per code point and a run is
    /// shaped whole, so there is nothing this class could substitute without
    /// splitting the run it was asked about.
    ///
    /// - Parameters:
    ///   - codePoints: The run, in shaping order.
    ///   - weight: The variable-font weight to shape at.
    ///   - glyphsOut: Filled with the shaped glyphs.
    /// - Returns: Whatever the wrapped rasterizer returns — how many entries of
    ///   `glyphsOut` were written, with 0 meaning the run was not shaped and the
    ///   caller keeps its unligatured path.
    ///
    ///   This said "a negative count on refusal", which is a convention no
    ///   rasterizer in the tree uses and a pure forwarder could not introduce.
    ///   `GlyphRasterizer` states 0 and `FreeTypeRasterizer` returns 0. A caller
    ///   writing the `< 0` test that sentence asks for gets a branch that never
    ///   runs; one trusting it enough to widen the count into a length has the
    ///   shape that put an unbounded read on the kitty shared-memory path. An
    ///   override's doc is the one a reader meets first, so it does not get to
    ///   invent a contract the base denies.
    int shapeTextRun(std::span<const std::uint32_t> codePoints, std::uint32_t weight, std::span<ShapedGlyph> glyphsOut) override
    {
        return m_inner->shapeTextRun(codePoints, weight, glyphsOut);
    }

    /// Draws an already-shaped run into a caller-supplied strip.
    ///
    /// Forwarded unchanged, and this is the entry where that is a decision
    /// rather than the obvious default: the input is glyph indices the shaper
    /// already chose, so no code point survives here for this class to
    /// recognise as box drawing. A run through a box character reaches the
    /// procedural path before shaping, not after.
    ///
    /// - Parameters:
    ///   - glyphs: The shaped run.
    ///   - stripWidthInPixels: The strip's width.
    ///   - stripHeightInPixels: The strip's height.
    ///   - coverageOut: R8 coverage for the whole strip.
    /// - Returns: Whether the strip was drawn.
    bool rasteriseShapedRun(std::span<const ShapedGlyph> glyphs, int stripWidthInPixels, int stripHeightInPixels, std::span<std::uint8_t> coverageOut) override
    {
        return m_inner->rasteriseShapedRun(glyphs, stripWidthInPixels, stripHeightInPixels, coverageOut);
    }

    /// Draws an already-shaped run and reports each glyph's ink extent.
    ///
    /// Forwarded unchanged for the reason ``rasteriseShapedRun`` is, and it
    /// must be forwarded at all because a decorator that forwards only the
    /// unmeasured draw would hide the inner rasterizer's measurement — the
    /// consumer would fall back to the geometric cut on exactly the arms
    /// that route through this class.
    ///
    /// - Parameters:
    ///   - glyphs: The shaped run.
    ///   - stripWidthInPixels: The strip's width.
    ///   - stripHeightInPixels: The strip's height.
    ///   - coverageOut: R8 coverage for the whole strip.
    ///   - inkOut: One extent per entry of `glyphs`.
    /// - Returns: Whether the strip was drawn and every extent filled.
    bool rasteriseShapedRunMeasured(std::span<const ShapedGlyph> glyphs, int stripWidthInPixels, int stripHeightInPixels, std::span<std::uint8_t> coverageOut, std::span<GlyphInkExtent> inkOut) override
    {
        return m_inner->rasteriseShapedRunMeasured(glyphs, stripWidthInPixels, stripHeightInPixels, coverageOut, inkOut);
    }

    /// Forwards the proportional face's presence to the inner rasterizer.
    ///
    /// A decorator that left this at the base's false would hide the face on
    /// exactly the arms that route through it — every Linux surface — and the
    /// proportional runs there would draw the grid-face fallback while the
    /// strip sheet's smoke, built on the bare rasterizer, passed.
    ///
    /// - Returns: Whether the inner rasterizer has a proportional face.
    [[nodiscard]] bool hasProportionalFace() const override { return m_inner->hasProportionalFace(); }
    /// The wrapped face's proportional strip overhang; this decorator draws
    /// no proportional run of its own.
    ///
    /// - Returns: The rows per side the inner face states.
    [[nodiscard]] int proportionalStripOverhangInPixels() const override { return m_inner->proportionalStripOverhangInPixels(); }

    /// Forwards a proportional run's shaping to the inner rasterizer.
    ///
    /// - Parameters:
    ///   - codePoints: The run's text.
    ///   - face: Regular or bold.
    ///   - glyphsOut: Caller-owned destination; its size is the bound.
    /// - Returns: How many glyphs were written; 0 refuses.
    int shapeProportionalRun(std::span<const std::uint32_t> codePoints, FontFace face, std::span<Ubiquitty::ProportionalShapedGlyph> glyphsOut) override
    {
        return m_inner->shapeProportionalRun(codePoints, face, glyphsOut);
    }

    /// Forwards a proportional run's draw to the inner rasterizer.
    ///
    /// - Parameters:
    ///   - glyphs: Placed glyphs, offsets already carrying the caller's pen.
    ///   - face: The face the glyphs were shaped in.
    ///   - stripWidthInPixels: The strip's width.
    ///   - stripHeightInPixels: The strip's height.
    ///   - coverageOut: R8 coverage for the whole strip.
    /// - Returns: Whether the strip was drawn.
    bool rasteriseProportionalRun(std::span<const Ubiquitty::ProportionalShapedGlyph> glyphs, FontFace face, int stripWidthInPixels, int stripHeightInPixels, std::span<std::uint8_t> coverageOut) override
    {
        return m_inner->rasteriseProportionalRun(glyphs, face, stripWidthInPixels, stripHeightInPixels, coverageOut);
    }

    /// Forwards a sized proportional strip to the decorated rasterizer.
    ///
    /// - Parameters:
    ///   - glyphs: Placed glyphs, offsets in master pixels.
    ///   - face: The face the glyphs were shaped in.
    ///   - master: The master Core selected.
    ///   - stripWidthInPixels: The destination's width.
    ///   - stripHeightInPixels: The destination's height.
    ///   - coverageOut: Caller-owned `stripWidth * stripHeight` bytes.
    /// - Returns: What the decorated rasterizer answers.
    bool rasteriseSizedProportionalRun(std::span<const Ubiquitty::ProportionalShapedGlyph> glyphs, FontFace face, const SizedMaster& master, int stripWidthInPixels, int stripHeightInPixels, std::span<std::uint8_t> coverageOut) override
    {
        return m_inner->rasteriseSizedProportionalRun(glyphs, face, master, stripWidthInPixels, stripHeightInPixels, coverageOut);
    }

    /// Forwards the colour cluster to the inner rasterizer.
    ///
    /// - Parameters:
    ///   - codePoints: The cluster.
    ///   - count: How many code points; 1 is a lone one.
    ///   - slotInPixels: The square slot edge.
    ///   - pixelsOut: The RGBA8 destination.
    /// - Returns: Whatever the inner answered.
    bool rasterizeColorCluster(const std::uint32_t* codePoints, int count, int slotInPixels, std::uint8_t* pixelsOut) override
    {
        return m_inner->rasterizeColorCluster(codePoints, count, slotInPixels, pixelsOut);
    }

    /// Forwards the grapheme cluster to the inner rasterizer.
    ///
    /// - Parameters:
    ///   - codePoints: The cluster.
    ///   - count: How many code points.
    ///   - face: The style to draw.
    ///   - weight: The variable weight, where the face has one.
    ///   - slotWidth: The slot's width.
    ///   - slotHeight: The slot's height.
    ///   - coverageOut: The R8 destination.
    /// - Returns: Whatever the inner answered.
    bool rasterizeCluster(const std::uint32_t* codePoints, int count, FontFace face, std::uint32_t weight, int slotWidth, int slotHeight, std::uint8_t* coverageOut) override
    {
        return m_inner->rasterizeCluster(codePoints, count, face, weight, slotWidth, slotHeight, coverageOut);
    }

    /// Forwards the scaled raster to the inner rasterizer.
    ///
    /// - Parameters:
    ///   - codePoint: The code point to draw.
    ///   - face: The style to draw.
    ///   - weight: The variable weight, where the face has one.
    ///   - scale: The multiple of the base cell to draw at.
    ///   - slotWidth: The slot's width.
    ///   - slotHeight: The slot's height.
    ///   - coverageOut: The R8 destination.
    /// - Returns: Whatever the inner answered.
    /// Forwards the inner rasterizer's answer.
    ///
    /// This decorator draws box glyphs procedurally and delegates the rest, so
    /// whether a scaled raster is worth an atlas slot is the inner face's
    /// property and never this wrapper's.
    ///
    /// - Returns: What the decorated rasterizer answers.
    [[nodiscard]] bool rasterisesScaledGlyphsSharply() const override { return m_inner->rasterisesScaledGlyphsSharply(); }

    /// Forwards a sized master to the decorated rasterizer.
    ///
    /// Not intercepted the way ``rasterizeGlyph`` is: a procedural box fill is
    /// computed for the destination it is given, so a master is drawn by the
    /// inner face at the size the caller asked for rather than by this wrapper
    /// at one size and magnified.
    ///
    /// - Parameters:
    ///   - codePoint: The glyph to draw.
    ///   - face: The style to draw it in.
    ///   - weight: The pen's weight.
    ///   - master: The master Core selected.
    ///   - slotWidth: The destination's width.
    ///   - slotHeight: The destination's height.
    ///   - coverageOut: Caller-owned `slotWidth * slotHeight` bytes.
    /// - Returns: What the decorated rasterizer answers.
    bool rasterizeSizedMaster(std::uint32_t codePoint, FontFace face, std::uint32_t weight, const SizedMaster& master, int slotWidth, int slotHeight, std::uint8_t* coverageOut) override
    {
        return m_inner->rasterizeSizedMaster(codePoint, face, weight, master, slotWidth, slotHeight, coverageOut);
    }

    /// Forwards the decorated face's width axis, which this wrapper does not own.
    ///
    /// - Parameter face: The style asked about.
    /// - Returns: What the decorated rasterizer answers.
    [[nodiscard]] WidthAxisRoom widthAxisRoom(FontFace face) const override { return m_inner->widthAxisRoom(face); }

    /// Forwards the decorated face's widened advance.
    ///
    /// - Parameters:
    ///   - face: The style asked about.
    ///   - coordinate: The `wdth` design coordinate.
    /// - Returns: What the decorated rasterizer answers.
    [[nodiscard]] double cellAdvanceAtWidthAxis(FontFace face, double coordinate) override { return m_inner->cellAdvanceAtWidthAxis(face, coordinate); }

    bool rasterizeGlyph(std::uint32_t codePoint, FontFace face, std::uint32_t weight, int slotWidth, int slotHeight, std::uint8_t* coverageOut) override
    {
        if (Rendering::BoxDrawing::prefersProcedural(codePoint)) {
            // Blocks/shades/quadrants/sextants/wedges: cell-exact procedural fill.
            if (rasterizeProcedural(codePoint, { slotWidth, slotHeight }, coverageOut))
                return true;
        } else if (isBoxLine(codePoint)) {
            // Lines/corners: font glyph, centred + edge-stretched to tile.
            if (edgeStretchFontGlyph(codePoint, face, weight, { slotWidth, slotHeight }, coverageOut))
                return true;
            // Font has no glyph for it — fall back to procedural geometry.
            if (rasterizeProcedural(codePoint, { slotWidth, slotHeight }, coverageOut))
                return true;
        }
        return m_inner->rasterizeGlyph(codePoint, face, weight, slotWidth, slotHeight, coverageOut);
    }

private:
    /// Returns whether a code point is a box-drawing line this rasterizer draws.
    ///
    /// Corners, tees, crosses, lines and diagonals. Dashes are excluded and left
    /// to the font, because a font's dash tiles as its designer intended and a
    /// procedural one does not. Disjoint from `prefersProcedural()`, whose blocks
    /// live at U+2580–U+259F.
    ///
    /// Dashes included. They were excluded, and the family centring shift below
    /// is the reason they cannot be: it moves every box glyph's stroke to a
    /// shared row so neighbours meet, and a dash left out of it keeps the row
    /// the face drew it on. A dashed line then runs one row above the solid one
    /// it continues — which is what a box of mixed strokes looked like.
    ///
    /// The horizontal stretch is decided separately, by `boxReachEdges` reading
    /// the glyph's own decomposition, so a dash is shifted without having its
    /// gaps stretched away.
    ///
    /// - Parameter codePoint: The code point to classify.
    /// - Returns: True for any box-drawing code point.
    static bool isBoxLine(std::uint32_t codePoint) { return codePoint >= 0x2500 && codePoint <= 0x257F; }

    /// Returns the cell box inside an atlas slot, in slot-local pixels.
    ///
    /// - Returns: The cell box, rounded to whole pixels.
    [[nodiscard]] IntSize cellBox() const
    {
        const CellMetrics m = m_inner->cellMetrics();
        return { static_cast<int>(m.cellWidth + 0.5f), static_cast<int>(m.cellHeight + 0.5f) };
    }

    /// The type's height inside the cell, which is the cell less its line gap.
    ///
    /// The stroke weights derive from this rather than from the cell, so a
    /// reader raising the line spacing gets more room between rows and the same
    /// box drawing rather than heavier box drawing.
    ///
    /// - Returns: The em height in pixels, or zero when the face reports no
    ///   ascent — in which case the raster falls back to the cell.
    [[nodiscard]] float emHeight() const
    {
        const FaceMetrics face = m_inner->faceMetrics();
        return face.ascent + face.descent;
    }

    /// Draws a box-drawing glyph from its geometry rather than from the font.
    ///
    /// - Parameters:
    ///   - codePoint: The box-drawing code point.
    ///   - slot: The atlas slot, in pixels.
    ///   - outputBuffer: Destination coverage plane, `slot.width * slot.height`.
    /// - Returns: `true` when the code point has a procedural form.
    bool rasterizeProcedural(std::uint32_t codePoint, IntSize slot, std::uint8_t* outputBuffer)
    {
        return rasterizeBoxGlyph(codePoint, slot, cellBox(), emHeight(),
            { outputBuffer, static_cast<std::size_t>(slot.width) * static_cast<std::size_t>(slot.height) },
            strokeProfile(), m_shadeStyle);
    }

    // Font-first edge-stretch (the macOS path). Returns false when the font has no
    // glyph for the codepoint (the caller then falls back to procedural).
    /// Rasterises a box glyph from the font, then stretches it to the cell edges.
    ///
    /// The font-first path. Every glyph takes one family-wide centring shift,
    /// measured once per metrics from a reference `─`, so their strokes land on a
    /// shared row and meet with no gap; shifting each glyph by its own ink would put
    /// neighbours a pixel apart at the join.
    ///
    /// - Parameters:
    ///   - codePoint: The box-drawing code point.
    ///   - face: The bold/italic pair to rasterise in.
    ///   - weight: Variable-font weight.
    ///   - slot: The atlas slot, in pixels.
    ///   - outputBuffer: Destination coverage plane.
    /// - Returns: `false` when the font has no glyph, and the caller falls back to
    ///   the procedural path.
    bool edgeStretchFontGlyph(std::uint32_t codePoint, FontFace face, std::uint32_t weight, IntSize slot, std::uint8_t* outputBuffer)
    {
        if (!m_inner->rasterizeGlyph(codePoint, face, weight, slot.width, slot.height, outputBuffer))
            return false;
        const IntSize cell = cellBox();
        const int gx0 = (slot.width - cell.width) / 2;
        const int gy0 = (slot.height - cell.height) / 2;
        const int gx1 = gx0 + cell.width;
        const int gy1 = gy0 + cell.height;
        // Single family-wide centring shift (measured once per metrics from a
        // reference ─) so every box glyph's stroke moves to the cell centre by the
        // same amount and stays on a shared row, so neighbours meet with no gap.
        const int shift = familyCenterShift(face, weight, slot, gx0, gx1, gy0, gy1);
        translateScratchVertical(outputBuffer, slot.width, slot.height, shift);
        // A dash takes the shift and not the stretch. Its decomposition reaches
        // the cell edges like any line's, so the stretch pulls its ink out to
        // them — and the gaps that make it a dash are what lies between. Widened
        // that way it renders as a solid rule, which is the same defect as the
        // misalignment, arrived at from the other side.
        bool reachL, reachR, reachT, reachB;
        if (!Rendering::BoxDrawing::isDash(codePoint)
            && boxReachEdges(codePoint, &reachL, &reachR, &reachT, &reachB))
            edgeStretchBoxScratch(outputBuffer, slot.width, slot.height, gx0, gx1, gy0, gy1, reachL, reachR, reachT, reachB);
        return true;
    }

    /// Returns the vertical shift that centres this family's box strokes.
    ///
    /// Measured from a reference `─` rather than per glyph, because tiling requires
    /// every glyph in the family to move by the same amount.
    ///
    /// - Parameters:
    ///   - face: The bold/italic pair.
    ///   - weight: Variable-font weight.
    ///   - slot: The atlas slot, in pixels.
    ///   - gx0: Cell box left edge inside the slot.
    ///   - gx1: Cell box right edge.
    ///   - gy0: Cell box top edge.
    ///   - gy1: Cell box bottom edge.
    /// - Returns: The shift in whole pixels.
    int familyCenterShift(FontFace face, std::uint32_t weight, IntSize slot, int gx0, int gx1, int gy0, int gy1)
    {
        const int cellWidth = gx1 - gx0;
        const int cellHeight = gy1 - gy0;
        if (m_shiftValid && m_shiftCellW == cellWidth && m_shiftCellH == cellHeight)
            return m_shift;
        m_shiftValid = true;
        m_shiftCellW = cellWidth;
        m_shiftCellH = cellHeight;
        m_shift = 0;
        m_scratch.assign(static_cast<std::size_t>(slot.width) * slot.height, 0);
        // Reference ─ (U+2500): its horizontal ink band's row extent is the shared
        // connection row for the whole box family.
        if (!m_inner->rasterizeGlyph(0x2500, face, weight, slot.width, slot.height, m_scratch.data()))
            return 0;
        constexpr std::uint8_t ink = 128;
        int minY = gy1;
        int maxY = gy0 - 1;
        for (int y = gy0; y < gy1; ++y) {
            bool inked = false;
            for (int x = gx0; x < gx1; ++x)
                if (m_scratch[static_cast<std::size_t>(y) * slot.width + x] >= ink) {
                    inked = true;
                    break;
                }
            if (inked) {
                if (y < minY)
                    minY = y;
                maxY = y;
            }
        }
        if (maxY < minY)
            return 0;
        m_shift = Rendering::BoxDrawing::centerShiftY(minY, maxY, gy0, gy1);
        return m_shift;
    }

    /// The measured stroke profile the procedural path stitches against.
    ///
    /// Measured through the inner chain — fallbacks included, because the
    /// profile describes what reaches the screen — and re-measured when the
    /// cell changes, the same laziness the family shift keeps and for the
    /// same reason: the references cannot move otherwise.
    ///
    /// Each reference is drawn by ``edgeStretchFontGlyph``, the path a font
    /// piece takes, and not by the inner rasterizer alone. A font piece is
    /// moved by the family centring shift after the face draws it, so a band
    /// read from the face's own raster sits that many rows from the rule it is
    /// meant to continue, and every healed piece meets its font neighbour with
    /// a step of exactly the shift.
    ///
    /// - Returns: The profile for the current cell metrics, or null before a
    ///   cell exists to measure at.
    [[nodiscard]] const Rendering::BoxDrawing::BoxStrokeProfile* strokeProfile()
    {
        const IntSize cell = cellBox();
        if (cell.width <= 0 || cell.height <= 0)
            return nullptr;
        if (!m_isProfileMeasured || m_profileCellWidth != cell.width || m_profileCellHeight != cell.height) {
            m_strokeProfile = measureFontStrokeProfile(cell);
            m_isProfileMeasured = true;
            m_profileCellWidth = cell.width;
            m_profileCellHeight = cell.height;
        }
        return &m_strokeProfile;
    }

    /// Folds every stroke reference, drawn as a font piece, into a profile.
    ///
    /// - Parameter cell: The cell box, which is also the slot the references
    ///   are drawn in.
    /// - Returns: The profile; a reference the chain cannot draw leaves its
    ///   band invalid.
    [[nodiscard]] Rendering::BoxDrawing::BoxStrokeProfile measureFontStrokeProfile(IntSize cell)
    {
        Rendering::BoxDrawing::BoxStrokeProfile profile;
        std::vector<std::uint8_t> reference(static_cast<std::size_t>(cell.width) * static_cast<std::size_t>(cell.height));
        for (const std::uint32_t codePoint : Rendering::BoxDrawing::boxStrokeReferenceCodePoints()) {
            std::ranges::fill(reference, std::uint8_t { 0 });
            if (!edgeStretchFontGlyph(codePoint, FontFace::Regular, 0u, cell, reference.data()))
                continue;
            Rendering::BoxDrawing::foldBoxReferenceScan(&profile, codePoint, reference, cell.width, cell.height);
        }
        return profile;
    }

    GlyphRasterizer* m_inner;
    ShadeStyle m_shadeStyle { defaultShadeStyle };
    std::vector<std::uint8_t> m_scratch; // reference-glyph buffer for the family shift
    int m_shift = 0;
    int m_shiftCellW = -1;
    int m_shiftCellH = -1;
    bool m_shiftValid = false;
    Rendering::BoxDrawing::BoxStrokeProfile m_strokeProfile;
    int m_profileCellWidth = -1;
    int m_profileCellHeight = -1;
    bool m_isProfileMeasured = false;
};

} // namespace Ubiquitty::Rendering
