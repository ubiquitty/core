// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_color_emoji_sheet — an emoji keeps its own colours instead of taking the
// pen's.
//
// The failure this pins is not a blank. Without a colour sheet an emoji still
// draws: it goes down the mono path, where the fragment stage multiplies its
// coverage by the cell foreground, so the glyph is the right SHAPE in the wrong
// colours — a grey or green emoji, sharply drawn, on a screen where everything
// else is correct. That reads as a theme choice rather than a defect, which is
// why every arm below asserts the colour and not the presence.
//
// The two halves are separable and both have to hold:
//
//   * a rasterizer with no colour source must leave the hooks null, so
//     `optionalDefaultsAreLossy` keeps reporting the loss rather than claiming a
//     path that answers absent every time;
//   * a rasterizer with one must put the rasterizer's own pixels on the screen,
//     not the pen's.
//
// The colour source here is synthetic on purpose. The only real one is the
// browser's, which needs a canvas, and a test that cannot run without one would
// not run at all — while the thing worth pinning (the key, the slot, the UV, and
// whether the composite tints) is Core's and is the same either way.

#include "Harness.h"

#include "Core/Rendering/CellInstanceBuilder.h"
#include "Core/Rendering/GlyphBackend.h"
#include "Core/Rendering/SoftwareRenderer.h"
#include "Core/TermPalette.h"

#include <algorithm>
#include <vector>

namespace {

using namespace Ubiquitty;
using namespace Ubiquitty::Rendering;

constexpr int cellWidth = 8;
constexpr int cellHeight = 16;

// U+1F600, and the code point matters only in that it is not blank: the
// rasterizer below answers for anything, so what is under test is the routing
// rather than a face's coverage.
constexpr std::uint32_t emojiCodePoint = 0x1F600;

// Opaque magenta, which no palette entry and no default pen produces. A mono
// path tinting coverage with the foreground cannot arrive at it by accident, so
// finding it in the output is evidence the colour sheet was the source.
constexpr std::uint8_t colorRed = 0xFF;
constexpr std::uint8_t colorGreen = 0x00;
constexpr std::uint8_t colorBlue = 0xFF;

/// A colour source that always answers, in one flat colour.
///
/// Built on the seam rather than on ``UnifontRasterizer``, so what is under test
/// is the contract every rasterizer implements and not one implementation's
/// behaviour leaking through.
struct MagentaRasterizer final : GlyphRasterizer {
    [[nodiscard]] CellMetrics cellMetrics() const override
    {
        CellMetrics metrics {};
        metrics.cellWidth = cellWidth;
        metrics.cellHeight = cellHeight;
        metrics.baselineFromTop = cellHeight - 3.0f;
        metrics.underlineDrop = 2.0f;
        return metrics;
    }

    // No mono coverage: an emoji must not need it, and answering here would let
    // a routing bug that sent the cluster down the mono path still draw
    // something.
    bool rasterizeGlyph(std::uint32_t, FontFace, std::uint32_t, int, int, std::uint8_t*) override { return false; }

    [[nodiscard]] bool hasColorGlyphs() const override { return true; }

    bool rasterizeColorCluster(const std::uint32_t*, int count, int slotInPixels, std::uint8_t* pixelsOut) override
    {
        if (count <= 0 || slotInPixels <= 0 || !pixelsOut)
            return false;
        ++clusterCalls;
        lastSlotInPixels = slotInPixels;
        for (int i = 0; i < slotInPixels * slotInPixels; ++i) {
            pixelsOut[i * 4 + 0] = colorRed;
            pixelsOut[i * 4 + 1] = colorGreen;
            pixelsOut[i * 4 + 2] = colorBlue;
            pixelsOut[i * 4 + 3] = 0xFF;
        }
        return true;
    }

    int clusterCalls = 0;
    int lastSlotInPixels = 0;
};

struct Pixel {
    std::uint8_t red, green, blue, alpha;
};

Pixel at(const std::vector<std::uint8_t>& rgba, int width, int x, int y)
{
    const std::size_t o = (static_cast<std::size_t>(y) * width + x) * 4;
    return { rgba[o], rgba[o + 1], rgba[o + 2], rgba[o + 3] };
}

/// One row holding a single emoji cell, with a foreground nothing else uses.
struct Scene {
    TermPalette palette;
    std::vector<Cell> cells { 4 };
    BuildInputs in;

    Scene()
    {
        palette.seedDefaults();
        for (auto& cell : cells) {
            cell = Cell {};
            cell.pen.foregroundIndex = colorDefault;
            cell.pen.backgroundIndex = colorDefault;
            cell.pen.underlineIndex = colorDefault;
        }
        cells[0].codePoint = emojiCodePoint;
        cells[0].pen.attributes.add(Ubiquitty::VT::Attribute::Emoji);

        in = BuildInputs {};
        in.cells = cells.data();
        in.rows = 1;
        in.columns = 4;
        in.palette = &palette;
        in.viewOffset = 0;
    }
};

// Somewhere inside the emoji's own box. The instance is centred in the cell span
// at the slot's square side, so the cell centre is inside it for any slot the
// renderer picks.
Pixel emojiCentre(const std::vector<std::uint8_t>& rgba, int width)
{
    return at(rgba, width, cellWidth / 2, cellHeight / 2);
}

} // namespace

// A rasterizer with no colour source leaves the seam null and says so.
//
// Filling the hooks unconditionally would make `optionalDefaultsAreLossy`
// report a complete backend on a build whose emoji are all still grey, and
// nothing downstream re-asks the question.
TEST(testNoColorSourceLeavesTheSeamNull)
{
    SoftwareRenderer renderer(cellWidth, cellHeight);
    const GlyphBackend backend = renderer.glyphBackend();

    CHECK(backend.colorGlyph == nullptr);
    CHECK(backend.bindColorAtlas == nullptr);
    CHECK(backend.rasteriseColorGlyph == nullptr);
    CHECK(optionalDefaultsAreLossy(backend));
    // And no sheet is allocated to answer absent with.
    CHECK(renderer.colorAtlas().pixels == nullptr);
}

// A rasterizer with a colour source fills all three, and stops reporting a loss
// it no longer has.
TEST(testColorSourceFillsTheSeam)
{
    MagentaRasterizer rasterizer;
    SoftwareRenderer renderer(cellWidth, cellHeight, rasterizer);
    const GlyphBackend backend = renderer.glyphBackend();

    CHECK(backend.colorGlyph != nullptr);
    CHECK(backend.bindColorAtlas != nullptr);
    CHECK(backend.rasteriseColorGlyph != nullptr);
    CHECK(renderer.colorAtlas().pixels != nullptr);

    // Square at the cell height, so a two-cell emoji is not squashed into a
    // one-cell box.
    const ColorAtlasBinding binding = renderer.bindColorAtlas();
    CHECK_EQ(binding.slotInPixels, cellHeight);
}

// The emoji reaches the screen in the rasterizer's colours.
//
// The pen's foreground is the palette default and the sheet is magenta, so a
// magenta centre can only have come from the colour path.
TEST(testEmojiKeepsItsOwnColours)
{
    MagentaRasterizer rasterizer;
    SoftwareRenderer renderer(cellWidth, cellHeight, rasterizer);
    Scene scene;

    std::vector<std::uint8_t> rgba;
    int width = 0;
    int height = 0;
    CHECK(renderer.render(scene.in, rgba, width, height));

    CHECK(rasterizer.clusterCalls > 0);
    CHECK_EQ(rasterizer.lastSlotInPixels, cellHeight);

    const Pixel centre = emojiCentre(rgba, width);
    CHECK_EQ(static_cast<int>(centre.red), static_cast<int>(colorRed));
    CHECK_EQ(static_cast<int>(centre.green), static_cast<int>(colorGreen));
    CHECK_EQ(static_cast<int>(centre.blue), static_cast<int>(colorBlue));
}

// The same scene without a colour source produces something else.
//
// Without this arm the one above passes on any renderer that happens to paint
// magenta for another reason, and the two together are what make the assertion
// about the colour sheet rather than about the pixel.
TEST(testWithoutAColorSourceTheEmojiIsNotMagenta)
{
    SoftwareRenderer renderer(cellWidth, cellHeight);
    Scene scene;

    std::vector<std::uint8_t> rgba;
    int width = 0;
    int height = 0;
    CHECK(renderer.render(scene.in, rgba, width, height));

    const Pixel centre = emojiCentre(rgba, width);
    const bool isMagenta = centre.red == colorRed && centre.green == colorGreen && centre.blue == colorBlue;
    CHECK(!isMagenta);
}

// A second frame reuses the slot instead of rasterising again.
//
// The cache is what makes an emoji cost one raster rather than one per frame,
// and a key that disagreed with the builder's own probe would fault a fresh slot
// every time while still drawing correctly — visible only as a cost.
TEST(testAResidentEmojiIsNotRasterisedTwice)
{
    MagentaRasterizer rasterizer;
    SoftwareRenderer renderer(cellWidth, cellHeight, rasterizer);
    Scene scene;

    std::vector<std::uint8_t> rgba;
    int width = 0;
    int height = 0;
    CHECK(renderer.render(scene.in, rgba, width, height));
    const int afterFirstFrame = rasterizer.clusterCalls;
    CHECK(afterFirstFrame > 0);

    CHECK(renderer.render(scene.in, rgba, width, height));
    CHECK_EQ(rasterizer.clusterCalls, afterFirstFrame);
}

/// A colour source that also answers for mono coverage.
///
/// `MagentaRasterizer` refuses every mono glyph, so an emoji that took the mono
/// path there would draw nothing rather than draw grey — which is what makes
/// the routing arms above read the colour sheet and not a coincidence. A real
/// face answers both, and the builder has branches that exist only when a cell
/// has a mono coverage as well, so a row-level question needs a rasterizer that
/// fills them.
struct MagentaWithCoverageRasterizer final : GlyphRasterizer {
    MagentaWithCoverageRasterizer(int width, int height)
        : m_cellWidth(width)
        , m_cellHeight(height)
    {
    }

    // The renderer lays the grid out on the metrics this returns rather than on
    // the constructor's arguments, so a fixture answering one size here and
    // passing another there places every glyph at a pitch its own assertions
    // then fail to find.
    [[nodiscard]] CellMetrics cellMetrics() const override
    {
        CellMetrics metrics {};
        metrics.cellWidth = m_cellWidth;
        metrics.cellHeight = m_cellHeight;
        metrics.baselineFromTop = static_cast<float>(m_cellHeight) - 3.0f;
        metrics.underlineDrop = 2.0f;
        return metrics;
    }

    bool rasterizeGlyph(std::uint32_t, FontFace, std::uint32_t, int width, int height, std::uint8_t* coverageOut) override
    {
        if (width <= 0 || height <= 0 || !coverageOut)
            return false;
        std::fill_n(coverageOut, static_cast<std::size_t>(width) * height, static_cast<std::uint8_t>(0xFF));
        return true;
    }

    [[nodiscard]] bool hasColorGlyphs() const override { return true; }

    bool rasterizeColorCluster(const std::uint32_t*, int count, int slotInPixels, std::uint8_t* pixelsOut) override
    {
        if (count <= 0 || slotInPixels <= 0 || !pixelsOut)
            return false;
        ++clusterCalls;
        for (int i = 0; i < slotInPixels * slotInPixels; ++i) {
            pixelsOut[i * 4 + 0] = colorRed;
            pixelsOut[i * 4 + 1] = colorGreen;
            pixelsOut[i * 4 + 2] = colorBlue;
            pixelsOut[i * 4 + 3] = 0xFF;
        }
        return true;
    }

    int clusterCalls = 0;

private:
    int m_cellWidth;
    int m_cellHeight;
};

// Every emoji in a row reaches the screen, at its own cell.
//
// Every other arm here holds a row with ONE emoji, so all of them pass on a
// renderer that draws only some of a row's colour glyphs. The Linux arm does
// exactly that: `linux_color_emoji` renders four and the first is blank, and a
// row of three identical faces there draws only the third.
//
// This does not reproduce that, and the passing is the point. It drives the
// route the case's own note names — `Rendering::SoftwareRenderer`'s colour
// path — with five lead cells at the arm's real geometry, and loses none of
// them: one rasterisation, one slot, five instances, each on its own span.
// Whatever drops those glyphs on Linux is not reached from here, so the note's
// account of where the case lives does not survive it.
//
// The assertion can fire. A fixture that reports 8x16 from `cellMetrics` while
// handing the renderer 10x23 lands the glyphs on the pitch the rasterizer names
// rather than the one the test samples — two of five centres read background
// and the case goes red. That is the fixture being wrong rather than the
// renderer, which is why the metrics come from one place; the red is stated
// here because a test whose failure nobody has seen is a test nobody has
// checked.

TEST(testEveryEmojiInARowDraws)
{
    constexpr int wideCellWidth = 10;
    constexpr int tallCellHeight = 23;
    constexpr int columns = 40;

    MagentaWithCoverageRasterizer rasterizer(wideCellWidth, tallCellHeight);
    SoftwareRenderer renderer(wideCellWidth, tallCellHeight, rasterizer);

    TermPalette palette;
    palette.seedDefaults();
    std::vector<Cell> cells(columns);
    for (auto& cell : cells) {
        cell = Cell {};
        cell.pen.foregroundIndex = colorDefault;
        cell.pen.backgroundIndex = colorDefault;
        cell.pen.underlineIndex = colorDefault;
    }
    // Lead, continuation, blank — three times, which is `😀 😀 😀`.
    constexpr int leads[] = { 2, 5, 8, 11, 14 };
    for (const int lead : leads) {
        cells[lead].codePoint = emojiCodePoint;
        cells[lead].pen.attributes.add(Ubiquitty::VT::Attribute::Emoji);
        cells[lead].pen.attributes.add(Ubiquitty::VT::Attribute::Wide);
        cells[lead + 1].pen.attributes.add(Ubiquitty::VT::Attribute::WideContinuation);
    }

    BuildInputs in {};
    in.cells = cells.data();
    in.rows = 1;
    in.columns = columns;
    in.palette = &palette;
    in.viewOffset = 0;

    std::vector<std::uint8_t> rgba;
    int width = 0;
    int height = 0;
    CHECK(renderer.render(in, rgba, width, height));

    const auto isMagentaAt = [&](int lead) {
        const Pixel centre = at(rgba, width, lead * wideCellWidth + wideCellWidth / 2, tallCellHeight / 2);
        return centre.red == colorRed && centre.blue == colorBlue;
    };
    for (const int lead : leads)
        CHECK(isMagentaAt(lead));
}

UBIQUITTY_TEST_MAIN()
