// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_sized_master — Core's selection of the master a sized window is cut
// from: its em size, the width-axis widening and the stretch left over, and the
// phase grid its pen lands on. Every arm that cuts sized windows reads these
// answers, so a wrong one is wrong on Linux, portable and the browser at once.

#include "Harness.h"

#include "Core/Rendering/SizedMaster.h"

#include <array>
#include <cmath>
#include <string>
#include <vector>

using namespace Ubiquitty::Rendering;

namespace {

constexpr bool near(double a, double b) { return a - b < 1e-9 && b - a < 1e-9; }

SizedTile tile(std::uint8_t scale, std::uint8_t widthScale)
{
    SizedTile out { scale, 0, 0 };
    out.widthScale = widthScale;
    return out;
}

// Draws a vertical stem of a fixed width at the master's stretch, so a cut
// window's stem measures the stretch the master was asked for.
struct StemRasterizer final : GlyphRasterizer {
    [[nodiscard]] CellMetrics cellMetrics() const override
    {
        CellMetrics metrics {};
        metrics.cellWidth = 8.0f;
        metrics.cellHeight = 16.0f;
        metrics.baselineFromTop = 13.0f;
        return metrics;
    }
    bool rasterizeGlyph(std::uint32_t, FontFace, std::uint32_t, int slotWidth, int slotHeight, std::uint8_t* coverageOut) override
    {
        SizedMaster identity;
        return rasterizeSizedMaster('l', FontFace::Regular, 0, identity, slotWidth, slotHeight, coverageOut);
    }
    [[nodiscard]] bool rasterisesScaledGlyphsSharply() const override { return true; }
    bool rasterizeSizedMaster(std::uint32_t, FontFace, std::uint32_t, const SizedMaster& master, int slotWidth, int slotHeight, std::uint8_t* coverageOut) override
    {
        lastMaster = master;
        const int stem = static_cast<int>(std::lround(2.0 * master.scale.value() * master.horizontalScale));
        const int left = (slotWidth - stem) / 2;
        for (int y = 0; y < slotHeight; ++y)
            for (int x = 0; x < slotWidth; ++x)
                coverageOut[static_cast<std::size_t>(y) * slotWidth + x] = (x >= left && x < left + stem) ? 255 : 0;
        return true;
    }
    [[nodiscard]] WidthAxisRoom widthAxisRoom(FontFace) const override { return room; }
    [[nodiscard]] double cellAdvanceAtWidthAxis(FontFace, double coordinate) override { return 8.0 * coordinate / 100.0; }

    WidthAxisRoom room;
    SizedMaster lastMaster;
};

} // namespace

// The axis widens to twice where it stands, clamped to its reach, and a face
// with no room has no target.
TEST(testDoubleWidthAxisTarget)
{
    CHECK(near(doubleWidthAxisTarget({ 100.0, 125.0 }), 125.0));
    CHECK(near(doubleWidthAxisTarget({ 60.0, 150.0 }), 120.0));
    CHECK(near(doubleWidthAxisTarget({ 100.0, 100.0 }), 0.0));
    CHECK(near(doubleWidthAxisTarget({}), 0.0));
}

// The pen is the glyph's advance centred in the master, moved by the master's
// own shift, to a 64th of a pixel.
TEST(testMasterPenCentresTheAdvanceAndTakesTheShift)
{
    SizedMaster master;
    SubpixelPhase pen = sizedMasterPen(40, 10.0, master);
    CHECK_EQ(pen.snappedPenX, 15);
    CHECK_EQ(pen.phase, 0);
    master.shiftX = 0.25;
    pen = sizedMasterPen(40, 10.0, master);
    CHECK_EQ(pen.snappedPenX, 15);
    CHECK_EQ(pen.phase, sizedMasterPenGrid / 4);
    master.shiftX = -0.25;
    pen = sizedMasterPen(41, 10.0, master);
    CHECK_EQ(pen.snappedPenX, 15);
    CHECK_EQ(pen.phase, sizedMasterPenGrid / 4);
}

// DECDHL: twice the em, no widening, no stretch.
TEST(testDoubleHeightMasterIsTheFaceAtTwiceItsSize)
{
    const SizedMaster master = selectSizedMaster(tile(2, 1), { 100.0, 125.0 }, 8.0, 10.0);
    CHECK_EQ(master.scale.numerator, 2);
    CHECK_EQ(master.widthScale, 1);
    CHECK(near(master.widthAxisCoordinate, 0.0));
    CHECK(near(master.horizontalScale, 1.0));
}

// DECDWL on Monaspace Neon: the axis takes the face from 100 to 125, and only
// the remainder of the doubling, 2 * 8 / 10 = 1.6, is a stretch.
TEST(testDoubleWidthMasterWidensThenStretchesTheRemainder)
{
    const SizedMaster master = selectSizedMaster(tile(1, 2), { 100.0, 125.0 }, 8.0, 10.0);
    CHECK_EQ(master.scale.numerator, 1);
    CHECK_EQ(master.widthScale, 2);
    CHECK(near(master.widthAxisCoordinate, 125.0));
    CHECK(near(master.horizontalScale, 1.6));
    CHECK_EQ(master.horizontalMagnification().numerator, 2);
}

// A face with no room, or an axis whose advance did not grow, stretches by two.
TEST(testDoubleWidthMasterWithoutRoomStretchesByTwo)
{
    const SizedMaster noAxis = selectSizedMaster(tile(1, 2), {}, 8.0, 0.0);
    CHECK(near(noAxis.widthAxisCoordinate, 0.0));
    CHECK(near(noAxis.horizontalScale, 2.0));
    const SizedMaster flat = selectSizedMaster(tile(1, 2), { 100.0, 125.0 }, 8.0, 8.0);
    CHECK(near(flat.widthAxisCoordinate, 0.0));
    CHECK(near(flat.horizontalScale, 2.0));
}

// An OSC 66 block on a doubled row: three times the em, widened as the row is.
TEST(testSizedBlockOnADoubledRowWidensAtItsScale)
{
    const SizedMaster master = selectSizedMaster(tile(3, 2), { 100.0, 125.0 }, 8.0, 10.0);
    CHECK_EQ(master.scale.numerator, 3);
    CHECK(near(master.horizontalScale, 1.6));
    CHECK_EQ(master.horizontalMagnification().numerator, 6);
}

// A fraction keeps its own size; a doubled fraction is no master's.
TEST(testFractionalTileKeepsItsSize)
{
    SizedTile fraction = fractionalSizedTile(3, 1, 2);
    const SizedMaster master = selectSizedMaster(fraction, {}, 0.0, 0.0);
    CHECK_EQ(master.scale.numerator, 3);
    CHECK_EQ(master.scale.denominator, 2);
    CHECK(isSizedMasterTile(fraction));
    fraction.widthScale = 2;
    CHECK(!isSizedMasterTile(fraction));
    CHECK(isSizedMasterTile(tile(1, 2)));
    CHECK(isSizedMasterTile(tile(2, 1)));
    CHECK(!isSizedMasterTile(tile(1, 1)));
}

// The facts come from the rasterizer and the decision from Core.
TEST(testSelectionReadsTheRasterizersFacts)
{
    StemRasterizer rasterizer;
    rasterizer.room = { 100.0, 125.0 };
    const SizedMaster widened = selectSizedMaster(rasterizer, FontFace::Regular, tile(1, 2));
    CHECK(near(widened.widthAxisCoordinate, 125.0));
    CHECK(near(widened.horizontalScale, 1.6));
    rasterizer.room = {};
    CHECK(near(selectSizedMaster(rasterizer, FontFace::Regular, tile(1, 2)).horizontalScale, 2.0));
}

// The two windows of a doubled cell are the halves of one master, cut on the
// cell grid: the stem the master drew stands across the join, not in either
// window twice.
TEST(testDoubledWindowsCutOneMaster)
{
    StemRasterizer rasterizer;
    const SizedWindowCanvas canvas { 12, 20, 2, 2 };
    std::vector<std::uint8_t> master;
    std::vector<std::uint8_t> left(12 * 20);
    std::vector<std::uint8_t> right(12 * 20);
    SizedTile first = tile(1, 2);
    SizedTile second = tile(1, 2);
    second.tile = 1;
    CHECK(rasteriseSizedWindow(rasterizer, 'l', FontFace::Regular, 0, first, 0, canvas, master, left));
    CHECK(rasteriseSizedWindow(rasterizer, 'l', FontFace::Regular, 0, second, 0, canvas, master, right));
    CHECK(near(rasterizer.lastMaster.horizontalScale, 2.0));
    // The stem is 4 wide at the master's centre, 12 of 24: columns 10-13 of
    // the master, so 2 in the left window's cell box and 2 in the right's.
    int leftInk = 0;
    int rightInk = 0;
    for (int x = 2; x < 10; ++x) {
        leftInk += left[static_cast<std::size_t>(10 * 12 + x)] ? 1 : 0;
        rightInk += right[static_cast<std::size_t>(10 * 12 + x)] ? 1 : 0;
    }
    CHECK_EQ(leftInk, 2);
    CHECK_EQ(rightInk, 2);
}

namespace {

// A master whose row `r` holds the value `r` across, so the row a window's own
// row `y` was cut from reads back directly.
struct RowNumberRasterizer final : GlyphRasterizer {
    [[nodiscard]] CellMetrics cellMetrics() const override
    {
        CellMetrics metrics {};
        metrics.cellWidth = 8.0f;
        metrics.cellHeight = 16.0f;
        metrics.baselineFromTop = 13.0f;
        return metrics;
    }
    bool rasterizeGlyph(std::uint32_t, FontFace, std::uint32_t, int slotWidth, int slotHeight, std::uint8_t* coverageOut) override
    {
        std::fill_n(coverageOut, static_cast<std::size_t>(slotWidth) * static_cast<std::size_t>(slotHeight), std::uint8_t { 0 });
        return true;
    }
    [[nodiscard]] bool rasterisesScaledGlyphsSharply() const override { return true; }
    bool rasterizeSizedMaster(std::uint32_t, FontFace, std::uint32_t, const SizedMaster&, int slotWidth, int slotHeight, std::uint8_t* coverageOut) override
    {
        for (int y = 0; y < slotHeight; ++y)
            std::fill_n(coverageOut + static_cast<std::size_t>(y) * slotWidth, slotWidth, static_cast<std::uint8_t>(y));
        return true;
    }
};

} // namespace

// The cell box is the line rounded up, and the grid's rows are the line, so a
// window steps down the master by the line: the band below starts where the
// band above ends, and the box's spare row is the next band's first.
TEST(testSizedWindowsStepDownTheMasterByTheLine)
{
    RowNumberRasterizer rasterizer;
    SizedWindowCanvas canvas { 12, 20, 2, 2, 15.4f };
    std::vector<std::uint8_t> master;
    std::vector<std::uint8_t> window(12 * 20);
    SizedTile band = tile(3, 1);
    band.band = 1;
    CHECK(rasteriseSizedWindow(rasterizer, 'l', FontFace::Regular, 0, band, 0, canvas, master, window));
    // The cell box is 16 rows; the line is 15.4, which rounds to 15 at band 1.
    // The window's first row backs off by the margin: master row 6 + 15 - 2.
    CHECK_EQ(static_cast<int>(window[0]), 6 + 15 - 2);
    CHECK_EQ(static_cast<int>(window[static_cast<std::size_t>(2 * 12)]), 6 + 15);

    canvas.referenceLineHeightInPixels = 0.0f;
    CHECK(rasteriseSizedWindow(rasterizer, 'l', FontFace::Regular, 0, band, 0, canvas, master, window));
    CHECK_EQ(static_cast<int>(window[static_cast<std::size_t>(2 * 12)]), 6 + 16);

    canvas.referenceLineHeightInPixels = 17.0f;
    CHECK(rasteriseSizedWindow(rasterizer, 'l', FontFace::Regular, 0, band, 0, canvas, master, window));
    CHECK_EQ(static_cast<int>(window[static_cast<std::size_t>(2 * 12)]), 6 + 16);
}

// The master is whole pixels, so a band begins at the row nearest its exact
// place. A whole line is cut exactly and the bands abut; a fractional one is
// within half a row at every band, so a join shows or skips less than a row.
// The Apple rasteriser steps by the exact fraction, which is what this is
// measured against.
TEST(testSizedWindowsBeginAtTheRowNearestTheirBand)
{
    RowNumberRasterizer rasterizer;
    constexpr int marginY = 2;
    constexpr int scale = 7;
    for (const float line : { 15.0f, 15.4f, 15.5f, 15.9f, 30.27f }) {
        // The cell box is the line rounded up, as an atlas slot's is.
        const int height = static_cast<int>(std::ceil(line)) + 2 * marginY;
        SizedWindowCanvas canvas { 12, height, 2, marginY, line };
        std::vector<std::uint8_t> master;
        std::vector<std::uint8_t> window(static_cast<std::size_t>(12 * height));
        float previousBegin = 0.0f;
        for (int band = 0; band < scale; ++band) {
            SizedTile bandTile = tile(scale, 1);
            bandTile.band = static_cast<std::uint8_t>(band);
            CHECK(rasteriseSizedWindow(rasterizer, 'l', FontFace::Regular, 0, bandTile, 0, canvas, master, window));
            // The window's cell box begins a margin in; the master row it shows
            // there is the row the band began at, `scale` margins down.
            const float begin = static_cast<float>(window[static_cast<std::size_t>(marginY * 12)]) - static_cast<float>(scale * marginY);
            const float exact = static_cast<float>(band) * line;
            const std::string where = "line " + std::to_string(line) + " band " + std::to_string(band) + " begins at row "
                + std::to_string(begin) + ", exactly " + std::to_string(exact);
            CHECK_MSG(std::fabs(begin - exact) <= 0.5f + 1e-3f, where.c_str());
            if (line == std::floor(line))
                CHECK_MSG(begin == exact, where.c_str());
            if (band > 0) {
                // The band above shows `line` rows from its beginning, so the
                // join is off by the difference of the two roundings.
                const float join = begin - (previousBegin + line);
                const std::string joined = "the join above band " + std::to_string(band) + " of line " + std::to_string(line) + " is off by " + std::to_string(join);
                CHECK_MSG(std::fabs(join) < 1.0f, joined.c_str());
                if (line == std::floor(line))
                    CHECK_MSG(join == 0.0f, joined.c_str());
            }
            previousBegin = begin;
        }
    }
}

// A sized colour master reduces from the nearest strike above its square, and
// takes the largest only when none is that large.
TEST(testColorStrikeIsTheNearestAboveTheTarget)
{
    const std::array<int, 4> strikes = { 20, 160, 40, 64 };
    CHECK_EQ(colorStrikeForTarget(strikes, 18), 0);
    CHECK_EQ(colorStrikeForTarget(strikes, 40), 2);
    CHECK_EQ(colorStrikeForTarget(strikes, 41), 3);
    CHECK_EQ(colorStrikeForTarget(strikes, 96), 1);
    CHECK_EQ(colorStrikeForTarget(strikes, 400), 1);
    CHECK_EQ(colorStrikeForTarget(std::span<const int> {}, 20), -1);
}

namespace {

// Paints each master pixel with its own coordinates, so a window says which
// part of the master it was cut from.
struct CoordinateColorRasterizer final : GlyphRasterizer {
    [[nodiscard]] CellMetrics cellMetrics() const override { return {}; }
    bool rasterizeGlyph(std::uint32_t, FontFace, std::uint32_t, int, int, std::uint8_t*) override { return false; }
    [[nodiscard]] bool hasColorGlyphs() const override { return true; }
    bool rasterizeColorCluster(const std::uint32_t*, int, int slotInPixels, std::uint8_t* pixelsOut) override
    {
        lastSide = slotInPixels;
        for (int y = 0; y < slotInPixels; ++y) {
            for (int x = 0; x < slotInPixels; ++x) {
                std::uint8_t* pixel = &pixelsOut[(static_cast<std::size_t>(y) * slotInPixels + x) * 4];
                pixel[0] = static_cast<std::uint8_t>(x);
                pixel[1] = static_cast<std::uint8_t>(y);
                pixel[2] = 0;
                pixel[3] = 255;
            }
        }
        return true;
    }
    int lastSide = 0;
};

} // namespace

// The colour master is drawn once at the block's size and each window is the
// slot-sized square that many slots in; past the master it is transparent.
TEST(testSizedColorWindowsCutTheMasterAtItsSize)
{
    CoordinateColorRasterizer rasterizer;
    constexpr int slot = 10;
    std::vector<std::uint8_t> master;
    std::vector<std::uint8_t> window(slot * slot * 4);
    const std::uint32_t emoji[] = { 0x1F600 };
    SizedTile tile { 3, 2, 1 };
    CHECK(rasteriseSizedColorWindow(rasterizer, emoji, tile, slot, master, window));
    CHECK_EQ(rasterizer.lastSide, 30);
    CHECK_EQ(window[0], 10);
    CHECK_EQ(window[1], 20);
    CHECK_EQ(window[((9 * slot) + 9) * 4], 19);

    SizedTile fraction = fractionalSizedTile(3, 1, 2);
    fraction.band = 1;
    fraction.tile = 1;
    CHECK_EQ(sizedColorMasterSide(fraction, slot), 15);
    CHECK(rasteriseSizedColorWindow(rasterizer, emoji, fraction, slot, master, window));
    CHECK_EQ(window[((4 * slot) + 4) * 4 + 3], 255);
    CHECK_EQ(window[((5 * slot) + 5) * 4 + 3], 0);
}

// The colour key folds the cluster and the window, and verifies both.
TEST(testSizedColorKeyVerifiesTheClusterAndTheWindow)
{
    const std::uint32_t family[] = { 0x1F468, 0x200D, 0x1F469 };
    SizedTile first { 3, 0, 0 };
    SizedTile second { 3, 0, 1 };
    const SizedColorTileContents a = sizedColorTileContents(family, first);
    const SizedColorTileContents b = sizedColorTileContents(family, second);
    CHECK_EQ(a.count, 4);
    CHECK(a.key != b.key);
    CHECK((a.key & 0xC0000000u) == 0x40000000u);
    std::vector<std::uint32_t> tooLong(Ubiquitty::clusterPoolMaximumCodePoints + 1, 0x1F600);
    CHECK_EQ(sizedColorTileContents(tooLong, first).count, 0);
}

namespace {

// A glyph that is one rectangle, drawn two ways, to measure where a window puts
// ink against where the block's own geometry says it stands.
//
// The rectangle spans `[inkLeft, inkRight]` across and `[inkTop, inkBottom]`
// down in the 1x cell box, a glyph of advance `advance`. At a master it is
// magnified by the master and placed as Core says: the pen from
// ``sizedMasterPen``, which carries the shift across, and the box's top from the
// centred box moved by the shift down. Whether the pixels are the area a pixel
// holds or the mean of sixteen samples is the rasterizer's own, and the two
// agree to a sample's quantisation.
struct RectangleRasterizer final : GlyphRasterizer {
    enum class Sampling {
        ExactArea,
        SixteenBySixteen,
    };

    explicit RectangleRasterizer(Sampling mode)
        : sampling(mode)
    {
    }

    [[nodiscard]] CellMetrics cellMetrics() const override
    {
        CellMetrics metrics {};
        metrics.cellWidth = static_cast<float>(boxWidth);
        metrics.cellHeight = static_cast<float>(boxHeight);
        metrics.baselineFromTop = 24.0f;
        return metrics;
    }
    bool rasterizeGlyph(std::uint32_t, FontFace, std::uint32_t, int, int, std::uint8_t*) override { return false; }
    [[nodiscard]] bool rasterisesScaledGlyphsSharply() const override { return true; }
    bool rasterizeSizedMaster(std::uint32_t, FontFace, std::uint32_t, const SizedMaster& master, int slotWidth, int slotHeight, std::uint8_t* coverageOut) override
    {
        const double across = master.horizontalMagnification().value();
        const double down = master.scale.value();
        const SubpixelPhase pen = sizedMasterPen(slotWidth, across * advance, master);
        const double penX = pen.snappedPenX + static_cast<double>(pen.phase) / sizedMasterPenGrid;
        const double boxTop = (slotHeight - down * boxHeight) / 2.0 + master.shiftY;
        const double left = penX + across * inkLeft;
        const double right = penX + across * inkRight;
        const double top = boxTop + down * inkTop;
        const double bottom = boxTop + down * inkBottom;
        for (int y = 0; y < slotHeight; ++y) {
            for (int x = 0; x < slotWidth; ++x)
                coverageOut[static_cast<std::size_t>(y) * slotWidth + x] = pixel(x, y, left, right, top, bottom);
        }
        return true;
    }

    std::uint8_t pixel(int x, int y, double left, double right, double top, double bottom) const
    {
        if (sampling == Sampling::ExactArea) {
            const double wide = std::max(0.0, std::min(right, x + 1.0) - std::max(left, static_cast<double>(x)));
            const double high = std::max(0.0, std::min(bottom, y + 1.0) - std::max(top, static_cast<double>(y)));
            return static_cast<std::uint8_t>(std::lround(255.0 * wide * high));
        }
        int inside = 0;
        for (int sy = 0; sy < 16; ++sy) {
            for (int sx = 0; sx < 16; ++sx) {
                const double u = x + (sx + 0.5) / 16.0;
                const double v = y + (sy + 0.5) / 16.0;
                inside += (u >= left && u < right && v >= top && v < bottom) ? 1 : 0;
            }
        }
        return static_cast<std::uint8_t>(std::lround(255.0 * inside / 256.0));
    }

    static constexpr int boxWidth = 16;
    static constexpr int boxHeight = 30;
    static constexpr double advance = 15.5;
    static constexpr double inkLeft = 14.0;
    static constexpr double inkRight = 17.5;
    static constexpr double inkTop = 13.0;
    static constexpr double inkBottom = 17.25;
    Sampling sampling;
};

// The coverage a window's texel holds where the block's geometry puts the
// rectangle: the block's own coordinates, from the snapped pen, which no master
// or cut takes part in.
double expectedWindowCoverage(int x, int y, int tileIndex, int bandIndex, int scale, double step, double bandStep, int phase, int marginX, int marginY)
{
    const double advanceAtScale = scale * RectangleRasterizer::advance;
    const double penInBlock = static_cast<double>(phase) / subpixelPhaseCount + (scale * step - advanceAtScale) / 2.0;
    const double left = penInBlock + scale * RectangleRasterizer::inkLeft;
    const double right = penInBlock + scale * RectangleRasterizer::inkRight;
    const double top = scale * RectangleRasterizer::inkTop;
    const double bottom = scale * RectangleRasterizer::inkBottom;
    const double texelLeft = sizedWindowLeftInPixels(tileIndex, static_cast<float>(step)) - marginX + x;
    const double texelTop = bandIndex * bandStep - marginY + y;
    const double wide = std::max(0.0, std::min(right, texelLeft + 1.0) - std::max(left, texelLeft));
    const double high = std::max(0.0, std::min(bottom, texelTop + 1.0) - std::max(top, texelTop));
    return wide * high;
}

} // namespace

// Windows step by the face's own fractional advance, each on its whole pixel,
// and by the line; the glyph stands on the anchor's subpixel pen, and a
// rectangle that crosses a window's join reads the same on both sides of it:
// every texel of every window is what the block's geometry says, to the
// master's own quantisation, whichever rasterizer drew the master and whatever
// fraction the cut left.
TEST(testWindowsAreTheBlocksGeometryOnTwoRasterizers)
{
    constexpr int scale = 3;
    const SizedWindowCanvas canvas { 20, 34, 2, 2, 29.6f, 15.66f };
    for (const RectangleRasterizer::Sampling sampling : { RectangleRasterizer::Sampling::ExactArea, RectangleRasterizer::Sampling::SixteenBySixteen }) {
        RectangleRasterizer rasterizer(sampling);
        std::vector<std::uint8_t> master;
        std::vector<std::uint8_t> window(static_cast<std::size_t>(canvas.width * canvas.height));
        for (const int phase : { 0, 3, 7 }) {
            for (int bandIndex = 0; bandIndex < scale; ++bandIndex) {
                for (int tileIndex = 0; tileIndex < scale; ++tileIndex) {
                    SizedTile windowTile = tile(scale, 1);
                    windowTile.band = static_cast<std::uint8_t>(bandIndex);
                    windowTile.tile = static_cast<std::uint8_t>(tileIndex);
                    CHECK(rasteriseSizedWindow(rasterizer, 'l', FontFace::Regular, 0, windowTile, static_cast<std::uint32_t>(phase), canvas, master, window));
                    double worst = 0.0;
                    for (int y = 0; y < canvas.height; ++y) {
                        for (int x = 0; x < canvas.width; ++x) {
                            const double want = expectedWindowCoverage(x, y, tileIndex, bandIndex, scale, 15.66, 29.6, phase, canvas.marginX, canvas.marginY);
                            const double got = window[static_cast<std::size_t>(y * canvas.width + x)] / 255.0;
                            worst = std::max(worst, std::fabs(got - want));
                        }
                    }
                    const std::string where = "sampling " + std::to_string(static_cast<int>(sampling)) + " phase " + std::to_string(phase)
                        + " band " + std::to_string(bandIndex) + " tile " + std::to_string(tileIndex) + " is off by " + std::to_string(worst);
                    CHECK_MSG(worst < 0.05, where.c_str());
                }
            }
        }
    }
}

// Every window of a block, in every band, is cut from a master that stands the
// glyph at the same place in the block: the pen across and the baseline down,
// each taken from where its window begins, are one place for the whole block,
// at every phase, on a canvas whose bands step by a fractional line and whose
// windows step by a fractional advance, bottom-aligned or not. A placement that
// rounded either per window would move each window by its own rounding, and a
// diagonal stroke would step at every join.
TEST(testEveryWindowOfABlockStandsItsGlyphAtOnePlace)
{
    constexpr double advance = 15.5;
    constexpr double slotBaselineFromTop = 26.0;
    for (const bool isGlyphBottomAligned : { false, true }) { // bool-ok: a loop over both alignments
        SizedWindowCanvas canvas { 20, 34, 2, 2, 29.6f, 15.66f };
        canvas.isGlyphBottomAligned = isGlyphBottomAligned;
        const double step = sizedWindowStepInPixels(canvas.referenceAdvanceInPixels, static_cast<float>(canvas.width - 2 * canvas.marginX));
        const double bandStep = sizedBandHeightInPixels(canvas.referenceLineHeightInPixels, static_cast<float>(canvas.height - 2 * canvas.marginY));
        for (int scale = 2; scale <= 7; ++scale) {
            for (std::uint32_t phase = 0; phase < subpixelPhaseCount; ++phase) {
                double firstAcross = 0.0;
                double firstDown = 0.0;
                double worstAcross = 0.0;
                double worstDown = 0.0;
                for (int band = 0; band < scale; ++band) {
                    for (int tileIndex = 0; tileIndex < scale; ++tileIndex) {
                        SizedTile windowTile = tile(static_cast<std::uint8_t>(scale), 1);
                        windowTile.band = static_cast<std::uint8_t>(band);
                        windowTile.tile = static_cast<std::uint8_t>(tileIndex);
                        const SizedWindowCut cut = planSizedWindow(selectSizedMaster(windowTile, {}, 0.0, 0.0), windowTile, phase, canvas);
                        const SubpixelPhase pen = sizedMasterPen(cut.masterWidth, cut.master.horizontalMagnification().value() * advance, cut.master);
                        const double penX = pen.snappedPenX + static_cast<double>(pen.phase) / sizedMasterPenGrid;
                        const double across = penX - cut.sourceX + sizedWindowLeftInPixels(tileIndex, static_cast<float>(step));
                        const double down = sizedMasterBaselineFromTop(slotBaselineFromTop, cut.master) - cut.sourceY + band * bandStep;
                        if (band == 0 && tileIndex == 0) {
                            firstAcross = across;
                            firstDown = down;
                        }
                        worstAcross = std::max(worstAcross, std::fabs(across - firstAcross));
                        worstDown = std::max(worstDown, std::fabs(down - firstDown));
                    }
                }
                const std::string where = "s=" + std::to_string(scale) + " phase " + std::to_string(phase) + (isGlyphBottomAligned ? " bottom-aligned" : "")
                    + ": the pen moves " + std::to_string(worstAcross) + " across and the baseline " + std::to_string(worstDown) + " down between windows";
                CHECK_MSG(worstAcross <= 1.0 / sizedMasterPenGrid && worstDown < 1e-6, where.c_str());
            }
        }
    }
}

// Two rasterizers that follow the contract draw the same window: the placement
// is Core's, so nothing a rasterizer decides drifts between arms.
TEST(testTwoRasterizersCutTheSameTileTheSame)
{
    const SizedWindowCanvas canvas { 20, 34, 2, 2, 29.6f, 15.66f };
    RectangleRasterizer exact(RectangleRasterizer::Sampling::ExactArea);
    RectangleRasterizer sampled(RectangleRasterizer::Sampling::SixteenBySixteen);
    std::vector<std::uint8_t> master;
    std::vector<std::uint8_t> first(static_cast<std::size_t>(canvas.width * canvas.height));
    std::vector<std::uint8_t> second(first.size());
    SizedTile windowTile = tile(3, 1);
    windowTile.band = 1;
    windowTile.tile = 2;
    CHECK(rasteriseSizedWindow(exact, 'l', FontFace::Regular, 0, windowTile, 5, canvas, master, first));
    CHECK(rasteriseSizedWindow(sampled, 'l', FontFace::Regular, 0, windowTile, 5, canvas, master, second));
    int ink = 0;
    for (std::size_t index = 0; index < first.size(); ++index) {
        CHECK(std::abs(static_cast<int>(first[index]) - static_cast<int>(second[index])) <= 8);
        ink += first[index] > 0 ? 1 : 0;
    }
    CHECK(ink > 0);
}

// A rasterizer that cannot place finer than a texel leaves the glyph where the
// centring put it, and the window is off by the half texel the cut left at most.
TEST(testAWholeTexelRasterizerIsOffByHalfATexelAtMost)
{
    struct Whole final : GlyphRasterizer {
        [[nodiscard]] CellMetrics cellMetrics() const override { return inner.cellMetrics(); }
        bool rasterizeGlyph(std::uint32_t, FontFace, std::uint32_t, int, int, std::uint8_t*) override { return false; }
        [[nodiscard]] bool rasterisesScaledGlyphsSharply() const override { return true; }
        bool rasterizeSizedMaster(std::uint32_t codePoint, FontFace face, std::uint32_t weight, const SizedMaster& master, int width, int height, std::uint8_t* out) override
        {
            SizedMaster unshifted = master;
            unshifted.shiftX = 0.0;
            unshifted.shiftY = 0.0;
            return inner.rasterizeSizedMaster(codePoint, face, weight, unshifted, width, height, out);
        }
        RectangleRasterizer inner { RectangleRasterizer::Sampling::ExactArea };
    } rasterizer;
    constexpr int scale = 3;
    const SizedWindowCanvas canvas { 20, 34, 2, 2, 29.6f, 15.66f };
    std::vector<std::uint8_t> master;
    std::vector<std::uint8_t> window(static_cast<std::size_t>(canvas.width * canvas.height));
    SizedTile windowTile = tile(scale, 1);
    windowTile.tile = 2;
    windowTile.band = 1;
    CHECK(rasteriseSizedWindow(rasterizer, 'l', FontFace::Regular, 0, windowTile, 3, canvas, master, window));
    // The rectangle's ink across the window's rows, against where the geometry puts it.
    double gotWeight = 0.0;
    double gotMoment = 0.0;
    double wantWeight = 0.0;
    double wantMoment = 0.0;
    for (int y = 0; y < canvas.height; ++y) {
        for (int x = 0; x < canvas.width; ++x) {
            const double got = window[static_cast<std::size_t>(y * canvas.width + x)] / 255.0;
            const double want = expectedWindowCoverage(x, y, 2, 1, scale, 15.66, 29.6, 3, canvas.marginX, canvas.marginY);
            gotWeight += got;
            gotMoment += got * (x + 0.5);
            wantWeight += want;
            wantMoment += want * (x + 0.5);
        }
    }
    REQUIRE(gotWeight > 0.0 && wantWeight > 0.0);
    CHECK(std::fabs(gotMoment / gotWeight - wantMoment / wantWeight) <= 0.5 + 1e-3);
}

// The master is drawn already moved by what the whole-texel cut leaves, so the
// cut itself never resamples: the shift is the distance from the exact place to
// the texel, under half a texel.
TEST(testThePlanLeavesAtMostHalfATexelToTheShift)
{
    const SizedWindowCanvas canvas { 20, 34, 2, 2, 29.6f, 15.66f };
    for (int scale = 2; scale <= 7; ++scale) {
        for (int band = 0; band < scale; ++band) {
            for (int tileIndex = 0; tileIndex < scale; ++tileIndex) {
                SizedTile windowTile = tile(static_cast<std::uint8_t>(scale), 1);
                windowTile.band = static_cast<std::uint8_t>(band);
                windowTile.tile = static_cast<std::uint8_t>(tileIndex);
                for (std::uint32_t phase = 0; phase < subpixelPhaseCount; ++phase) {
                    const SizedWindowCut cut = planSizedWindow(selectSizedMaster(windowTile, {}, 0.0, 0.0), windowTile, phase, canvas);
                    CHECK(std::fabs(cut.master.shiftX) <= 0.5 + 1e-9);
                    CHECK(std::fabs(cut.master.shiftY) <= 0.5 + 1e-9);
                }
            }
        }
    }
}

// Windows begin a fractional step apart and a whole box apart only when the
// canvas names no advance; the line steps down the same way.
TEST(testWindowsStepByTheAdvanceTheCanvasNames)
{
    SizedTile windowTile = tile(7, 1);
    windowTile.tile = 6;
    const SizedMaster master = selectSizedMaster(windowTile, {}, 0.0, 0.0);
    const SizedWindowCut whole = planSizedWindow(master, windowTile, 0, { 20, 34, 2, 2, 0.0f, 0.0f });
    const SizedWindowCut fractional = planSizedWindow(master, windowTile, 0, { 20, 34, 2, 2, 0.0f, 15.66f });
    // Six windows along, the box steps 96 and the advance 93.96, two pixels
    // less; the glyph, centred in the extent the advance steps across, stands
    // 7 * 0.34 / 2 = 1.19 pixels further left than the box would put it, so
    // the cut begins 2.04 - 1.19 = 0.85 pixels less along, which is one texel.
    CHECK_EQ(whole.sourceX - fractional.sourceX, 1);
    CHECK(whole.master.shiftX != fractional.master.shiftX);
}

// The pen is the advance centred in the extent the windows step across, so a
// face whose advance is the step stands at the block's left, where the box that
// the advance rounds up to would have moved it right by the difference.
TEST(testTheGlyphStandsAtTheBlocksLeftWhenTheAdvanceIsTheStep)
{
    RectangleRasterizer rasterizer(RectangleRasterizer::Sampling::ExactArea);
    // Advance 15.5 in a 15.5 step: the pen is the block's origin, and a block
    // at scale 7 puts the rectangle at 7 * 14 = 98 from it.
    const SizedWindowCanvas canvas { 20, 34, 2, 2, 30.0f, 15.5f };
    std::vector<std::uint8_t> master;
    std::vector<std::uint8_t> window(static_cast<std::size_t>(canvas.width * canvas.height));
    SizedTile windowTile = tile(7, 1);
    windowTile.tile = 6;
    windowTile.band = 3;
    CHECK(rasteriseSizedWindow(rasterizer, 'l', FontFace::Regular, 0, windowTile, 0, canvas, master, window));
    // Window 6 begins at 93 along the block, two texels of margin before its
    // cell box, so its column 7 is the block's 98 where the rectangle begins.
    const int row = 10;
    CHECK_EQ(static_cast<int>(window[static_cast<std::size_t>(row * canvas.width + 6)]), 0);
    CHECK_EQ(static_cast<int>(window[static_cast<std::size_t>(row * canvas.width + 7)]), 255);
}

// Every frontend names its canvas through sizedWindowCanvas, so the step a
// window is cut by is the advance a quad moves by. A cell 15.66 wide, cut at
// seven times the size, puts window 6 at 94 pixels along; a step of the box's
// 16 would put it at 96 and drift two pixels from the quads. A wide slot covers
// two cells and steps two advances, 31.32, where its box would step 32.
TEST(testACanvasStepsByTheFractionalAdvanceOfTheCellsItCovers)
{
    const SizedWindowCanvas narrow = sizedWindowCanvas(20, 34, 2, 2, 29.6f, 15.66f, 1, SizedWindowStep::FaceAdvance, 16.0f);
    const SizedWindowCanvas wide = sizedWindowCanvas(32, 34, 0, 2, 29.6f, 15.66f, 2, SizedWindowStep::FaceAdvance, 32.0f);
    const SizedWindowCanvas boxedNarrow = sizedWindowCanvas(20, 34, 2, 2, 29.6f, 0.0f, 1, SizedWindowStep::FaceAdvance, 16.0f);
    const SizedWindowCanvas boxedWide = sizedWindowCanvas(32, 34, 0, 2, 29.6f, 0.0f, 2, SizedWindowStep::FaceAdvance, 32.0f);
    CHECK(std::fabs(narrow.referenceAdvanceInPixels - 15.66) < 1e-4);
    CHECK(std::fabs(wide.referenceAdvanceInPixels - 31.32) < 1e-4);
    const auto alongOf = [&](SizedWindowCanvas canvas, int tileIndex) {
        SizedTile windowTile = tile(7, 1);
        windowTile.tile = static_cast<std::uint8_t>(tileIndex);
        const SizedMaster master = selectSizedMaster(windowTile, {}, 0.0, 0.0);
        return planSizedWindow(master, windowTile, 0, canvas).sourceX;
    };
    CHECK_EQ(alongOf(narrow, 6) - alongOf(narrow, 0), 94);
    CHECK_EQ(alongOf(boxedNarrow, 6) - alongOf(boxedNarrow, 0), 96);
    CHECK_EQ(alongOf(wide, 3) - alongOf(wide, 0), 94);
    CHECK_EQ(alongOf(boxedWide, 3) - alongOf(boxedWide, 0), 96);
}

// A window a grid cell maps 1:1 steps as far as the cell does: the cell box the
// backend names, whatever the face's advance and however much headroom the slot
// carries past the box. Cut at 15.66 and drawn on a 16 pixel cell it would be
// stretched by 2 percent, and a block of seven would end two pixels wider than
// the glyph-stream's; the two windows are different pictures, so they key apart.
TEST(testACellBoxWindowStepsByTheCellBoxAndKeysApartFromTheAdvanceWindow)
{
    // A slot with a column of subpixel headroom past the box: 21 wide, a 17 pixel
    // canvas box over a 16 pixel cell box.
    const SizedWindowCanvas baked = sizedWindowCanvas(21, 34, 2, 2, 29.6f, 15.66f, 1, SizedWindowStep::CellBox, 16.0f);
    const SizedWindowCanvas wideBaked = sizedWindowCanvas(32, 34, 0, 2, 29.6f, 15.66f, 2, SizedWindowStep::CellBox, 32.0f);
    CHECK_EQ(baked.referenceAdvanceInPixels, 16.0f);
    CHECK_EQ(wideBaked.referenceAdvanceInPixels, 32.0f);
    const auto alongOf = [&](SizedWindowCanvas canvas, int tileIndex) {
        SizedTile windowTile = tile(7, 1);
        windowTile.tile = static_cast<std::uint8_t>(tileIndex);
        return planSizedWindow(selectSizedMaster(windowTile, {}, 0.0, 0.0), windowTile, 0, canvas).sourceX;
    };
    CHECK_EQ(alongOf(baked, 6) - alongOf(baked, 0), 96);
    CHECK_EQ(alongOf(wideBaked, 3) - alongOf(wideBaked, 0), 96);

    SizedTile streamTile = tile(7, 1);
    SizedTile cellTile = streamTile;
    cellTile.step = SizedWindowStep::CellBox;
    CHECK(streamTile.scaleLane() != cellTile.scaleLane());
    CHECK(sizedTileWord(0, FontFace::Regular, 0, streamTile.scaleLane(), 0, 0) != sizedTileWord(0, FontFace::Regular, 0, cellTile.scaleLane(), 0, 0));
    CHECK(sizedTileKey(0x58u, 0, FontFace::Regular, 0, streamTile.scaleLane(), 0, 0) != sizedTileKey(0x58u, 0, FontFace::Regular, 0, cellTile.scaleLane(), 0, 0));
    // The step does not reach the window index or the band beside it.
    cellTile.tile = 13;
    cellTile.band = 6;
    CHECK(sizedTileWord(0, FontFace::Regular, 0, cellTile.scaleLane(), cellTile.band, cellTile.tile)
        != sizedTileWord(0, FontFace::Regular, 0, streamTile.scaleLane(), cellTile.band, cellTile.tile));
}

// A slot that stands its glyph on the box's bottom has its spare rows above the
// line, so the block begins at the line's top: the first window is cut as far
// further down as the box is taller than the line, scaled with the block. A box
// 31 tall for a line of 30.27 has 0.73 spare rows, which at seven times the size
// is five rows of the master; a slot that stands its glyph from the top, or a
// box as tall as its line, has none.
TEST(testABottomAlignedBlockBeginsAtTheLinesTop)
{
    const SizedTile windowTile = tile(7, 1);
    const SizedMaster master = selectSizedMaster(windowTile, {}, 0.0, 0.0);
    const auto planned = [&](int height, float line, bool isBottomAligned) {
        SizedWindowCanvas canvas { 20, height, 2, 4, line, 15.65f };
        canvas.isGlyphBottomAligned = isBottomAligned;
        return planSizedWindow(master, windowTile, 0, canvas);
    };
    const SizedWindowCut topAligned = planned(39, 30.265625f, false);
    const SizedWindowCut bottomAligned = planned(39, 30.265625f, true);
    CHECK_EQ(bottomAligned.sourceY - topAligned.sourceY, 5);
    CHECK(std::fabs(bottomAligned.master.shiftY) <= 0.5);
    CHECK_EQ(planned(38, 30.0f, true).sourceY, planned(38, 30.0f, false).sourceY);
    CHECK_EQ(planned(39, 0.0f, true).sourceY, planned(39, 0.0f, false).sourceY);
}

UBIQUITTY_TEST_MAIN()
