// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_box_glyph_raster — the CPU coverage sink for procedural box-drawing.
//
// A terminal draws █ and ▀ and ─ itself rather than asking the font, because a
// font's versions do not tile: they carry bearing, and a row of them shows the
// gaps. The sink's whole job is that the painted area lands on the cell box
// exactly — one pixel short at any edge and a run of blocks reads as stripes,
// one over and the glyph bleeds into its neighbour's margin.
//
// The atlas centres the cell inside a larger slot to give spilling glyphs
// somewhere to go, so "the cell box" is an inset rectangle rather than the whole
// buffer. A sink measuring from the buffer edge instead is wrong by the margin
// on every box glyph at once, which reads as the font being subtly off rather
// than as a bug in one function.

#include "Harness.h"

#include "Core/Rendering/BoxGlyphRaster.h"

#include <vector>

using Ubiquitty::Rendering::rasterizeBoxGlyph;

namespace {

constexpr unsigned fullBlock = 0x2588; // █
constexpr unsigned upperHalfBlock = 0x2580; // ▀
constexpr unsigned lowerHalfBlock = 0x2584; // ▄
constexpr unsigned leftHalfBlock = 0x258C; // ▌
constexpr unsigned horizontalRule = 0x2500; // ─
constexpr unsigned verticalRule = 0x2502; // │
constexpr unsigned notABoxGlyph = 0x41; // 'A'

// A slot larger than the cell on both axes, so a wrong inset cannot coincide
// with a right one, and by an even amount on each so nothing here depends on
// which way a half-margin rounds.
constexpr int slotWidth = 10;
constexpr int slotHeight = 20;
constexpr int cellWidth = 8;
constexpr int cellHeight = 16;
constexpr int marginX = (slotWidth - cellWidth) / 2;
constexpr int marginY = (slotHeight - cellHeight) / 2;

// The tell-tale byte. Filling the buffer with it before the call separates
// "written with zero" from "not written at all" — the distinction the false
// return promises, and the one a zero-filled buffer cannot show.
constexpr std::uint8_t untouchedByte = 0xAB;

struct Mask {
    std::vector<std::uint8_t> px;
    bool didDraw = false;

    explicit Mask(unsigned codePoint)
        : px(static_cast<std::size_t>(slotWidth) * slotHeight, untouchedByte)
    {
        didDraw = rasterizeBoxGlyph(codePoint, { slotWidth, slotHeight }, { cellWidth, cellHeight }, static_cast<float>(cellHeight), { px.data(), static_cast<std::size_t>(slotWidth) * static_cast<std::size_t>(slotHeight) });
    }

    [[nodiscard]] std::uint8_t at(int x, int y) const { return px[static_cast<std::size_t>(y) * slotWidth + x]; }
    [[nodiscard]] bool isInked(int x, int y) const { return at(x, y) != 0 && at(x, y) != untouchedByte; }
};

bool isInsideTheCellBox(int x, int y)
{
    return x >= marginX && x < marginX + cellWidth && y >= marginY && y < marginY + cellHeight;
}

} // namespace

TEST(testFullBlockCoversTheCellBoxAndStopsThere)
{
    const Mask mask(fullBlock);
    CHECK(mask.didDraw);

    // Every pixel of the cell box and no pixel outside it, asserted per pixel
    // rather than as an area: a fill one row short and one column long has the
    // right count and the wrong shape.
    for (int y = 0; y < slotHeight; ++y) {
        for (int x = 0; x < slotWidth; ++x) {
            if (isInsideTheCellBox(x, y))
                CHECK_MSG(mask.isInked(x, y), "the full block covers the whole cell box");
            else
                CHECK_MSG(!mask.isInked(x, y), "and paints nothing into the slot's margin");
        }
    }
}

TEST(testHalvesTileWithNoGapAndNoOverlap)
{
    // ▀ above ▄ is what a run of shaded rows is built from. A gap between them
    // is a seam across the line; an overlap costs nothing visually and means the
    // two halves disagree about where the middle is, which surfaces later as a
    // half-pixel error somewhere that looks unrelated.
    const Mask upper(upperHalfBlock);
    const Mask lower(lowerHalfBlock);

    for (int y = 0; y < slotHeight; ++y) {
        for (int x = 0; x < slotWidth; ++x) {
            const bool isInUpper = upper.isInked(x, y);
            const bool isInLower = lower.isInked(x, y);
            CHECK_MSG(!(isInUpper && isInLower), "no pixel belongs to both halves");
            if (isInsideTheCellBox(x, y))
                CHECK_MSG(isInUpper || isInLower, "and together they leave no pixel of the cell uncovered");
            else
                CHECK_MSG(!isInUpper && !isInLower, "neither half reaches outside the cell box");
        }
    }
}

TEST(testHalfBlockSplitsTheCellAndNotTheSlot)
{
    const Mask left(leftHalfBlock);

    // The split is at the CELL's midpoint. The slot's midpoint sits half a
    // margin away, and a sink measuring from the buffer lands there on every
    // left/right half block — visible only as a column of them being slightly
    // too wide.
    const int splitX = marginX + cellWidth / 2;
    for (int y = marginY; y < marginY + cellHeight; ++y) {
        CHECK_MSG(left.isInked(marginX, y), "the left half starts at the cell's left edge");
        CHECK_MSG(left.isInked(splitX - 1, y), "and runs up to the cell's midpoint");
        CHECK_MSG(!left.isInked(splitX, y), "stopping there rather than at the slot's midpoint");
        CHECK_MSG(!left.isInked(marginX + cellWidth - 1, y), "so the cell's right half stays clear");
    }
}

TEST(testRuleReachesBothCellEdgesSoARunOfThemIsContinuous)
{
    // The pixel on the cell boundary is the one that meets the next cell's. A
    // rule inset by even one pixel turns a table border into a dashed line, and
    // the row it sits on is not the subject here — only that it spans.
    const Mask rule(horizontalRule);
    int row = -1;
    for (int y = 0; y < slotHeight && row < 0; ++y)
        if (rule.isInked(marginX, y))
            row = y;
    REQUIRE_MSG(row >= 0, "the horizontal rule paints the cell's left edge");
    CHECK_MSG(rule.isInked(marginX + cellWidth - 1, row),
        "and reaches the cell's right edge on that same row");

    const Mask stile(verticalRule);
    int column = -1;
    for (int x = 0; x < slotWidth && column < 0; ++x)
        if (stile.isInked(x, marginY))
            column = x;
    REQUIRE_MSG(column >= 0, "the vertical rule paints the cell's top edge");
    CHECK_MSG(stile.isInked(column, marginY + cellHeight - 1),
        "and reaches the cell's bottom edge in that same column");
}

TEST(testGlyphWithNoProceduralFormLeavesTheBufferAlone)
{
    // The caller delegates to the font on false, and does so with the buffer it
    // passed in. Writing zeros instead would erase whatever it had staged, and
    // would be indistinguishable from a correct refusal in any test that starts
    // from a cleared buffer.
    const Mask mask(notABoxGlyph);
    CHECK_MSG(!mask.didDraw, "a letter has no procedural form");

    bool isEveryByteAsPassedIn = true;
    for (std::uint8_t value : mask.px)
        isEveryByteAsPassedIn = isEveryByteAsPassedIn && value == untouchedByte;
    CHECK_MSG(isEveryByteAsPassedIn, "and the refusal leaves the caller's buffer byte for byte");
}

TEST(testShadeDithersToItsDensityAndTilesAcrossCells)
{
    // ░ paints an ordered dither keyed on absolute pixel position, so a field
    // of shade cells stitches into one texture instead of restarting at each
    // cell boundary. Light shade lights one position in four; the cell box
    // must hold both fully lit and fully dark pixels, each at the extremes —
    // an averaged grey here would resample to a different tone under the
    // atlas's scaling than the checkerboard does.
    const Mask shade(0x2591);
    CHECK(shade.didDraw);

    int litCount = 0;
    int darkCount = 0;
    for (int y = marginY; y < marginY + cellHeight; ++y)
        for (int x = marginX; x < marginX + cellWidth; ++x) {
            if (shade.at(x, y) == 255)
                litCount++;
            else if (shade.at(x, y) == 0)
                darkCount++;
        }
    CHECK_MSG(litCount + darkCount == cellWidth * cellHeight, "every shade pixel is fully lit or fully dark");
    CHECK_MSG(litCount * 3 == darkCount, "light shade lights one position in four");
}

TEST(testTriangularBlockFillsHalfTheCellHard)
{
    // 🭨-family blocks scan-convert with hard edges: a pixel is inside the
    // triangle or it is not, at full coverage either way. U+1FB68 is the cell
    // minus its left corner triangle, so the column just inside the left edge
    // carries less ink than the column at the right edge.
    const Mask triangle(0x1FB68);
    CHECK(triangle.didDraw);

    int leftColumnInk = 0;
    int rightColumnInk = 0;
    for (int y = marginY; y < marginY + cellHeight; ++y) {
        if (triangle.isInked(marginX, y))
            leftColumnInk++;
        if (triangle.isInked(marginX + cellWidth - 1, y))
            rightColumnInk++;
    }
    CHECK_MSG(rightColumnInk == cellHeight, "the kept side is a full edge");
    CHECK_MSG(leftColumnInk < cellHeight, "the cut corner removes ink from the left column");
}

TEST(testDiagonalIsAnAntialiasedStroke)
{
    // ╱ rasters as a round-capped stroke with a one-pixel coverage ramp at its
    // edge. The ramp is the point: a hard-edged diagonal stairsteps. So the
    // mask must hold intermediate coverage values, and its ink must connect
    // the cell's bottom-left region to its top-right.
    const Mask diagonal(0x2571);
    CHECK(diagonal.didDraw);

    bool hasIntermediateCoverage = false;
    for (int y = marginY; y < marginY + cellHeight; ++y)
        for (int x = marginX; x < marginX + cellWidth; ++x) {
            const std::uint8_t value = diagonal.at(x, y);
            if (value > 0 && value < 255)
                hasIntermediateCoverage = true;
        }
    CHECK_MSG(hasIntermediateCoverage, "the stroke edge ramps instead of stepping");

    bool bottomLeftInked = false;
    bool topRightInked = false;
    for (int offset = 0; offset < 3; ++offset) {
        for (int step = 0; step < 3; ++step) {
            if (diagonal.isInked(marginX + step, marginY + cellHeight - 1 - offset))
                bottomLeftInked = true;
            if (diagonal.isInked(marginX + cellWidth - 1 - step, marginY + offset))
                topRightInked = true;
        }
    }
    CHECK_MSG(bottomLeftInked && topRightInked, "the rising diagonal reaches both of its corners");
}

TEST(testShortBufferIsRefusedBeforeAnyWrite)
{
    // The span's extent is checked against the slot geometry before the clear:
    // a buffer allocated for a smaller slot returns false with every byte as
    // it was, rather than a cleared prefix and a write past the end.
    std::vector<std::uint8_t> shortBuffer(16, untouchedByte);
    const bool didDraw = rasterizeBoxGlyph(fullBlock, { slotWidth, slotHeight },
        { cellWidth, cellHeight }, static_cast<float>(cellHeight),
        { shortBuffer.data(), shortBuffer.size() });
    CHECK_MSG(!didDraw, "a buffer smaller than the slot is refused");
    bool isEveryByteAsPassedIn = true;
    for (std::uint8_t value : shortBuffer)
        isEveryByteAsPassedIn = isEveryByteAsPassedIn && value == untouchedByte;
    CHECK_MSG(isEveryByteAsPassedIn, "with nothing written before the refusal");
}

UBIQUITTY_TEST_MAIN()
