// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_software_ligature_pass.cpp — the CPU renderer draws a shaped run when its
// rasterizer can shape one.
//
// SoftwareRenderer is the arm the GPU backends are compared against byte for
// byte, so what it can draw decides what the comparison can hold still. It
// refused ligature shaping outright until the capability moved onto the shared
// GlyphRasterizer; now it publishes the hook, keeps its own strip sheet, and
// composites the result.
//
// Nothing else here exercises that. The bundled Unifont rasterizer has no
// shaper, so every other test in this suite takes the declining path and would
// pass identically with the whole pass deleted. This one supplies a rasterizer
// that shapes.
//
// The fixture is built so a single question answers it: mono glyphs rasterize
// blank and the shaped strip rasterizes solid, and the two arms differ in one
// answer — whether the rasterizer says it can shape. The measurement is the
// pixels that differ between them.
//
// Counting lit pixels would measure the wrong thing: every cell paints its
// background, so the control lights the whole surface and the count says
// nothing about glyphs. A difference between two renders of one grid has no
// background in it.

#include "Core/Cell.h"
#include "Core/Color.h"
#include "Core/Rendering/CellInstanceBuilder.h"
#include "Core/Rendering/GlyphRasterizer.h"
#include "Core/Rendering/SoftwareRenderer.h"
#include "Core/TermPalette.h"
#include "Harness.h"

#include <cstdint>
#include <span>
#include <vector>

using namespace Ubiquitty;
using namespace Ubiquitty::Rendering;

namespace {

constexpr int cellWidth = 8;
constexpr int cellHeight = 16;

// What the rasterizer answers about shaping, which is the only thing the two
// arms differ in.
enum class Shaping {
    Declined,
    Available
};

// A rasterizer whose only interesting property is whether it shapes.
//
// Both arms share one class so the two runs differ in exactly one answer. A
// separate non-shaping stub would also differ in whatever else its author wrote,
// and the comparison would no longer be about shaping.
struct ShapingRasterizer : GlyphRasterizer {
    bool canShape = true;

    [[nodiscard]] CellMetrics cellMetrics() const override
    {
        CellMetrics metrics;
        metrics.cellWidth = cellWidth;
        metrics.cellHeight = cellHeight;
        metrics.baselineFromTop = 13;
        return metrics;
    }

    // Blank on purpose: it makes any ink below attributable.
    bool rasterizeGlyph(std::uint32_t, FontFace, std::uint32_t, int slotWidth, int slotHeight, std::uint8_t* coverageOut) override
    {
        for (int i = 0; i < slotWidth * slotHeight; ++i)
            coverageOut[i] = 0;
        return true;
    }

    [[nodiscard]] bool hasLigatureShaping() const override { return canShape; }

    // One glyph for the whole run, which is what a ligature is: the substitution
    // that makes the count come back smaller than it went in.
    int shapeTextRun(std::span<const std::uint32_t> codePoints, std::uint32_t, std::span<ShapedGlyph> glyphsOut) override
    {
        if (!canShape || glyphsOut.empty() || codePoints.empty())
            return 0;
        glyphsOut[0] = ShapedGlyph { /*glyph=*/1u,
            /*advanceInPixels=*/cellWidth * static_cast<int>(codePoints.size()),
            /*offsetXInPixels=*/0, /*offsetYInPixels=*/0, /*cluster=*/0 };
        return 1;
    }

    bool rasteriseShapedRun(std::span<const ShapedGlyph> glyphs, int stripWidthInPixels, int stripHeightInPixels, std::span<std::uint8_t> coverageOut) override
    {
        if (!canShape || glyphs.empty())
            return false;
        const std::size_t wanted = static_cast<std::size_t>(stripWidthInPixels) * stripHeightInPixels;
        if (coverageOut.size() < wanted)
            return false;
        for (std::size_t i = 0; i < wanted; ++i)
            coverageOut[i] = 255; // solid, so the pass is unmistakable
        return true;
    }
};

// "->" — two operator characters under one pen, which is what
// identifyLigatureRuns asks for: at least two same-shape cells with at least one
// operator among them.
std::vector<Cell> arrowRow(int columns)
{
    std::vector<Cell> cells(static_cast<std::size_t>(columns), defaultBlankCell);
    cells[0].codePoint = U'-';
    cells[1].codePoint = U'>';
    return cells;
}

// Pixels where two renders of the same grid disagree. Every cell paints its
// background in both, so the background cancels and what remains is what one
// render drew that the other did not.
int differingPixels(const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b)
{
    if (a.size() != b.size())
        return -1;
    int differing = 0;
    for (std::size_t i = 0; i + 3 < a.size(); i += 4) {
        if (a[i] != b[i] || a[i + 1] != b[i + 1] || a[i + 2] != b[i + 2] || a[i + 3] != b[i + 3])
            ++differing;
    }
    return differing;
}

enum class SlotDimensions {
    Provided,
    Omitted
};

std::vector<std::uint8_t> renderSurface(Shaping shaping, float presentationScale = 1, SlotDimensions slotDimensions = SlotDimensions::Provided)
{
    constexpr int columns = 8;
    ShapingRasterizer rasterizer;
    rasterizer.canShape = shaping == Shaping::Available;
    SoftwareRenderer renderer { cellWidth, cellHeight, rasterizer };

    TermPalette palette;
    palette.seedDefaults();
    std::vector<Cell> cells = arrowRow(columns);

    BuildInputs in;
    in.cells = cells.data();
    in.rows = 1;
    in.columns = columns;
    in.palette = &palette;
    in.metrics.cellWidth = static_cast<float>(cellWidth);
    in.metrics.cellHeight = static_cast<float>(cellHeight);
    in.metrics.baselineFromTop = 13.0f;
    in.metrics.underlineThickness = 1.0f;
    in.metrics = scaledCellMetrics(in.metrics, presentationScale);
    in.glyphReferenceAdvanceInPixels = cellWidth;
    in.glyphReferenceLineHeightInPixels = cellHeight;
    // Fit changes the destination pitch, not this rasterizer's atlas slots.
    if (slotDimensions == SlotDimensions::Provided) {
        in.glyphSlotWidthInPixels = cellWidth;
        in.glyphSlotHeightInPixels = cellHeight;
    }

    std::vector<std::uint8_t> rgba;
    int width = 0;
    int height = 0;
    if (!renderer.render(in, rgba, width, height))
        return {};
    return rgba;
}

TEST(uniformFitScalesTheActualLigatureRasterInBothDimensions)
{
    const auto natural = renderSurface(Shaping::Available);
    const auto plain = renderSurface(Shaping::Declined);
    const auto fitted = renderSurface(Shaping::Available, 0.5f);
    const auto fittedPlain = renderSurface(Shaping::Declined, 0.5f);
    REQUIRE(!natural.empty() && !fitted.empty());
    // The drawable stays unchanged: only the run's ink takes the transform.
    REQUIRE(fitted.size() == natural.size());
    const auto naturalInk = differingPixels(natural, plain);
    const auto fittedInk = differingPixels(fitted, fittedPlain);
    REQUIRE(naturalInk > 0 && fittedInk > 0);
    CHECK_EQ(fittedInk * 4, naturalInk);
    CHECK(renderSurface(Shaping::Available, 0.5f, SlotDimensions::Omitted) == fitted);
}

// A rasterizer whose first glyph's ink overhangs its cell, and which can say
// so. Per-character shaping — glyph = code point, cluster = index — makes
// ownership unambiguous, and the geometry is built so one pixel answers the
// question: the '-' glyph's ink reaches three pixels into the '>' glyph's
// cell, and the '>' glyph's own ink starts past that overhang, so no pixel
// is ambiguous between the two. The arms share this class and differ in one
// answer — whether the drawing pass reports where each glyph's ink landed.
struct OverhangRasterizer : GlyphRasterizer {
    bool canMeasure = true;

    [[nodiscard]] CellMetrics cellMetrics() const override
    {
        CellMetrics metrics;
        metrics.cellWidth = cellWidth;
        metrics.cellHeight = cellHeight;
        metrics.baselineFromTop = 13;
        return metrics;
    }

    // Blank on purpose: it makes any ink below attributable to the strip.
    bool rasterizeGlyph(std::uint32_t, FontFace, std::uint32_t, int slotWidth, int slotHeight, std::uint8_t* coverageOut) override
    {
        for (int i = 0; i < slotWidth * slotHeight; ++i)
            coverageOut[i] = 0;
        return true;
    }

    [[nodiscard]] bool hasLigatureShaping() const override { return true; }

    // A face that ligates maps a run to glyphs its characters do not carry alone,
    // and a strip is published only where that happened — a face substituting
    // nothing has nothing to draw differently, so the run takes the mono path. The
    // isolated form is therefore distinct from the joined one here, which is what
    // makes this fixture a ligating face rather than a pass-through.
    //
    // The joined glyphs stay the code points because `inkSpanFor` reads them to
    // place this fixture's ink.
    static constexpr std::uint32_t isolatedForm = 0x8000'0000u;

    int shapeTextRun(std::span<const std::uint32_t> codePoints, std::uint32_t, std::span<ShapedGlyph> glyphsOut) override
    {
        const std::size_t count = std::min(codePoints.size(), glyphsOut.size());
        const bool isJoined = codePoints.size() > 1;
        for (std::size_t i = 0; i < count; ++i) {
            const std::uint32_t glyph = isJoined ? codePoints[i] : (codePoints[i] | isolatedForm);
            glyphsOut[i] = ShapedGlyph { glyph, /*advanceInPixels=*/cellWidth,
                /*offsetXInPixels=*/0, /*offsetYInPixels=*/0, /*cluster=*/static_cast<int>(i) };
        }
        return static_cast<int>(count);
    }

    // The fixture's ink, relative to the glyph's placed cell origin: '-'
    // spans its cell and three pixels beyond; '>' starts five pixels in,
    // clear of the neighbour's overhang.
    static void inkSpanFor(std::uint32_t glyph, int& bearing, int& width)
    {
        if (glyph == U'-') {
            bearing = 2;
            width = cellWidth + 1;
        } else {
            bearing = 5;
            width = 2;
        }
    }

    bool drawInk(std::span<const ShapedGlyph> glyphs, int stripWidthInPixels, int stripHeightInPixels,
        std::span<std::uint8_t> coverageOut, GlyphInkExtent* inkOut)
    {
        const std::size_t expected = static_cast<std::size_t>(stripWidthInPixels) * static_cast<std::size_t>(stripHeightInPixels);
        if (coverageOut.size() != expected)
            return false;
        for (std::size_t i = 0; i < expected; ++i)
            coverageOut[i] = 0;
        int penX = 0;
        std::size_t index = 0;
        for (const ShapedGlyph& shaped : glyphs) {
            int bearing = 0;
            int width = 0;
            inkSpanFor(shaped.glyph, bearing, width);
            const int left = penX + shaped.offsetXInPixels + bearing;
            const int right = left + width;
            const int clippedLeft = left < 0 ? 0 : left;
            const int clippedRight = right > stripWidthInPixels ? stripWidthInPixels : right;
            for (int y = 0; y < stripHeightInPixels; ++y) {
                for (int x = clippedLeft; x < clippedRight; ++x)
                    coverageOut[static_cast<std::size_t>(y) * static_cast<std::size_t>(stripWidthInPixels) + static_cast<std::size_t>(x)] = 255;
            }
            if (inkOut != nullptr)
                inkOut[index] = clippedRight > clippedLeft ? GlyphInkExtent { clippedLeft, clippedRight } : GlyphInkExtent {};
            penX += shaped.advanceInPixels;
            ++index;
        }
        return true;
    }

    bool rasteriseShapedRun(std::span<const ShapedGlyph> glyphs, int stripWidthInPixels, int stripHeightInPixels, std::span<std::uint8_t> coverageOut) override
    {
        return drawInk(glyphs, stripWidthInPixels, stripHeightInPixels, coverageOut, nullptr);
    }

    bool rasteriseShapedRunMeasured(std::span<const ShapedGlyph> glyphs, int stripWidthInPixels, int stripHeightInPixels, std::span<std::uint8_t> coverageOut, std::span<GlyphInkExtent> inkOut) override
    {
        if (!canMeasure || inkOut.size() != glyphs.size())
            return false;
        return drawInk(glyphs, stripWidthInPixels, stripHeightInPixels, coverageOut, inkOut.data());
    }
};

// Whether the rasterizer arm reports ink extents, the one answer the
// ownership pair differs in.
enum class Measurement {
    Declined,
    Available
};

// "->" under two foregrounds: the '-' cell pure red, the '>' cell pure green,
// so a probed pixel names the quad that painted it by colour alone.
std::vector<std::uint8_t> renderOwnershipSurface(Measurement measurement)
{
    constexpr int columns = 8;
    OverhangRasterizer rasterizer;
    rasterizer.canMeasure = measurement == Measurement::Available;
    SoftwareRenderer renderer { cellWidth, cellHeight, rasterizer };

    TermPalette palette;
    palette.seedDefaults();
    std::vector<Cell> cells = arrowRow(columns);
    cells[0].pen.foregroundIndex = colorRGB;
    cells[0].pen.foregroundRed = 255;
    cells[1].pen.foregroundIndex = colorRGB;
    cells[1].pen.foregroundGreen = 255;

    BuildInputs in;
    in.cells = cells.data();
    in.rows = 1;
    in.columns = columns;
    in.palette = &palette;
    in.metrics.cellWidth = static_cast<float>(cellWidth);
    in.metrics.cellHeight = static_cast<float>(cellHeight);
    in.metrics.baselineFromTop = 13.0f;
    in.metrics.underlineThickness = 1.0f;

    std::vector<std::uint8_t> rgba;
    int width = 0;
    int height = 0;
    if (!renderer.render(in, rgba, width, height))
        return {};
    return rgba;
}

// The rendered red and green channels at (x, y), row 0 of the fixture grid.
struct ProbedChannels {
    int red = -1;
    int green = -1;
};
ProbedChannels probeOwnership(const std::vector<std::uint8_t>& surface, int x, int y)
{
    constexpr int columns = 8;
    const std::size_t offset = (static_cast<std::size_t>(y) * static_cast<std::size_t>(columns * cellWidth) + static_cast<std::size_t>(x)) * 4;
    if (offset + 3 >= surface.size())
        return {};
    return { surface[offset], surface[offset + 1] };
}

} // namespace

/// A rasterizer that shapes gives the CPU renderer a ligature; one that does not
/// gives it nothing, through the same code.
///
/// The two arms differ in one answer — ``hasLigatureShaping`` — so the ink is
/// attributable to shaping rather than to any other difference between two
/// fixtures.
TEST(testTheCPURendererDrawsAShapedRunAndOnlyWhenItCanShape)
{
    const std::vector<std::uint8_t> shaped = renderSurface(Shaping::Available);
    const std::vector<std::uint8_t> unshaped = renderSurface(Shaping::Declined);
    CHECK(!shaped.empty()); // the render itself succeeded in both arms
    CHECK(!unshaped.empty());

    // The null first, because it is the arm that can fail quietly. Two renders
    // that cannot shape must agree exactly; if they did not, the difference
    // measured below would be noise and would prove nothing about shaping.
    CHECK_EQ(differingPixels(unshaped, renderSurface(Shaping::Declined)), 0);

    // The strip is solid over the run's two cells, so shaping changes at least
    // one cell's worth of pixels. Bounded below rather than pinned: the exact
    // count is the slot geometry's business and would re-mint on a margin change
    // that has nothing to do with whether the pass ran.
    CHECK(differingPixels(shaped, unshaped) >= cellWidth * cellHeight);
}

/// Measured ownership keeps a glyph's overhang under its own cell's colour.
///
/// The '-' glyph's ink reaches three pixels into the '>' cell; with the
/// drawing pass reporting extents, the overhang pixel is painted by the '-'
/// cell's quad and carries its red, while each glyph's home ink keeps its own
/// colour. The geometric cut hands that same pixel to the '>' column — the
/// companion test pins that — so the pair shows the cut, not the fixture,
/// decides the colour.
TEST(aMeasuredStripKeepsOverhangUnderItsOwnersColour)
{
    const std::vector<std::uint8_t> surface = renderOwnershipSurface(Measurement::Available);
    CHECK(!surface.empty());
    const int y = cellHeight / 2;

    const ProbedChannels home = probeOwnership(surface, 4, y);
    CHECK_MSG(home.red >= 200 && home.green <= 60, "the '-' cell's own ink is red");

    const ProbedChannels overhang = probeOwnership(surface, cellWidth + 1, y);
    CHECK_MSG(overhang.red >= 200 && overhang.green <= 60, "the overhang into the '>' cell keeps the owner's red");

    const ProbedChannels neighbour = probeOwnership(surface, cellWidth + 5, y);
    CHECK_MSG(neighbour.green >= 200 && neighbour.red <= 60, "the '>' cell's own ink is green");
}

/// The unmeasured fallback colours by column, which is the pinned behaviour a
/// backend without extents keeps.
///
/// The control for the test above: the same fixture through the same code,
/// differing only in the rasterizer declining to measure. The overhang pixel
/// then sits in the '>' column's geometric band and takes its green — the
/// documented limitation of the geometric cut, held still here so a change to
/// the fallback shows up as this test moving rather than as silence.
TEST(anUnmeasuredStripHandsOverhangToTheColumnItLandsOn)
{
    const std::vector<std::uint8_t> surface = renderOwnershipSurface(Measurement::Declined);
    CHECK(!surface.empty());
    const int y = cellHeight / 2;

    const ProbedChannels home = probeOwnership(surface, 4, y);
    CHECK_MSG(home.red >= 200 && home.green <= 60, "the '-' cell's own ink is red");

    const ProbedChannels overhang = probeOwnership(surface, cellWidth + 1, y);
    CHECK_MSG(overhang.green >= 200 && overhang.red <= 60, "the geometric cut hands the overhang to the '>' column");
}

UBIQUITTY_TEST_MAIN()
