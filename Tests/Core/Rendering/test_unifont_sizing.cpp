// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// Unifont point-size scaling from an explicit requested size and backing scale
// only, with native Linux and software geometry pinned to scale 1.

#include "Core/Rendering/CellMetrics.h"
#include "Core/Rendering/FontSelection.h"
#include "Core/Rendering/GlyphTiling.h"
#include "Core/Rendering/Unifont.h"
#include "Core/Rendering/UnifontRasterizer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>

using Ubiquitty::Rendering::unifontScaleForRequestedSize;

TEST(unifontSizing)
{

    const auto unifont = Ubiquitty::Rendering::selectFontFamily("GNU Unifont");
    CHECK(unifont.family == "GNU Unifont");
    CHECK(unifont.rasterizerKind == Ubiquitty::Rendering::FontRasterizerKind::EmbeddedBitmap);

    const auto native = Ubiquitty::Rendering::selectFontFamily("JetBrains Mono");
    CHECK(native.family == "JetBrains Mono");
    CHECK(native.rasterizerKind == Ubiquitty::Rendering::FontRasterizerKind::Native);

    CHECK(unifontScaleForRequestedSize(0.0, 2.0) == 1);
    CHECK(unifontScaleForRequestedSize(10.0, 0.0) == 1);
    CHECK(unifontScaleForRequestedSize(-1.0, 2.0) == 1);

    CHECK(unifontScaleForRequestedSize(8.0, 2.0) == 1);
    CHECK(unifontScaleForRequestedSize(8.01, 2.0) == 2);
    CHECK(unifontScaleForRequestedSize(10.0, 2.0) == 2);
    CHECK(unifontScaleForRequestedSize(13.0, 2.0) == 2);
    CHECK(unifontScaleForRequestedSize(16.0, 2.0) == 2);
    CHECK(unifontScaleForRequestedSize(16.01, 2.0) == 3);
    CHECK(unifontScaleForRequestedSize(96.0, 2.0) == 12);

    // The Linux and software renderers consume the native 16-pixel cell at
    // scale 1; the equivalent point-sized request produces the same geometry.
    CHECK(unifontScaleForRequestedSize(16.0, 1.0) == 1);
}

// The face's own metrics, which are not the cell's once a line spacing exists.
//
// `CellMetrics::baselineFromTop` folds the leading in; `FaceMetrics::ascent` is
// the type's own. A consumer substituting one for the other — the OSC 133
// bracket, the cursor bar, anything measured as a fraction of the point size —
// is right only while the spacing is zero, which is exactly the configuration
// every default uses and so exactly the one a test must not stop at.
TEST(unifontFaceMetrics)
{
    // Spacing zero: the two agree, which is why this case proves nothing on its
    // own and is here to show the second one is a real difference rather than an
    // arithmetic slip.
    {
        const Ubiquitty::Rendering::UnifontRasterizer rasterizer(1, 0);
        const auto cell = rasterizer.cellMetrics();
        const auto face = rasterizer.faceMetrics();
        CHECK_EQ(face.fontSize, 16);
        CHECK_NEAR(face.ascent, cell.baselineFromTop, 0.001f);
        CHECK_NEAR(face.ascent + face.descent, cell.cellHeight, 0.001f);
        CHECK_NEAR(face.lineGapTopInPixels, 0.0f, 0.001f);
    }

    // Spacing 6: the cell grows by all of it, the face by none, and the baseline
    // moves down by HALF. Splitting is what keeps the glyph centred — added
    // below, the ascent would still be 13.5 and the descent would swallow the
    // whole 6, so a descender's distance to its underline would shrink as the
    // user asked for more room.
    {
        const Ubiquitty::Rendering::UnifontRasterizer rasterizer(1, 6);
        const auto cell = rasterizer.cellMetrics();
        const auto face = rasterizer.faceMetrics();
        CHECK_NEAR(face.ascent, 13.5f, 0.001f);
        CHECK_NEAR(face.descent, 2.5f, 0.001f);
        CHECK_NEAR(face.lineGapTopInPixels, 3.0f, 0.001f);
        CHECK_NEAR(cell.cellHeight, 22.0f, 0.001f);
        CHECK_MSG(cell.baselineFromTop > face.ascent,
            "the cell baseline carries the leading and the face's ascent does not");
        CHECK_NEAR(cell.baselineFromTop, face.ascent + face.lineGapTopInPixels, 0.001f);
    }

    // An ODD spacing rounds the same way in every rasterizer or two faces sit a
    // pixel apart. Integer halving: the spare pixel goes below.
    {
        const Ubiquitty::Rendering::UnifontRasterizer rasterizer(1, 5);
        const auto face = rasterizer.faceMetrics();
        CHECK_NEAR(face.lineGapTopInPixels, 2.0f, 0.001f);
    }

    // Scale carries the face with it — a doubled cell is a doubled type, not a
    // doubled box around the same type.
    {
        const Ubiquitty::Rendering::UnifontRasterizer rasterizer(2, 0);
        const auto face = rasterizer.faceMetrics();
        CHECK_EQ(face.fontSize, 32);
        CHECK_NEAR(face.ascent, 27.0f, 0.001f);
        CHECK_NEAR(face.descent, 5.0f, 0.001f);
    }

    // The bracket is MEASURED, and the assertion is against the rasterized `[`
    // rather than against numbers copied out of a passing run: the ink the
    // prompt mark centres on has to be the ink the face draws, and a recorded
    // pair agrees with whatever produced it.
    {
        const int scale = 1;
        Ubiquitty::Rendering::UnifontRasterizer rasterizer(scale, 0);
        const auto face = rasterizer.faceMetrics();
        CHECK_MSG(face.bracketInkTopInPixels > 0.0f || face.bracketInkBottomInPixels > 0.0f,
            "a face whose bracket measures zero on both sides reads as unmeasured, and the "
            "mark then centres on the metrics midpoint for a face that has a bracket");

        constexpr int box = 16;
        std::uint8_t coverage[box * box] {};
        CHECK(rasterizer.rasterizeGlyph('[', Ubiquitty::Rendering::FontFace::Regular, 0, box, box, coverage));
        int firstInkRow = -1;
        int lastInkRow = -1;
        for (int row = 0; row < box; ++row) {
            bool hasInk = false;
            for (int column = 0; column < box; ++column)
                hasInk = hasInk || coverage[row * box + column] != 0;
            if (!hasInk)
                continue;
            if (firstInkRow < 0)
                firstInkRow = row;
            lastInkRow = row;
        }
        CHECK(firstInkRow >= 0);
        // The rasterized box is the face's own 16 rows with the baseline 13.5
        // down it, so the two distances follow from the ink rows directly.
        CHECK_NEAR(face.bracketInkTopInPixels, (13.5f - static_cast<float>(firstInkRow)) * static_cast<float>(scale), 0.001f);
        CHECK_NEAR(face.bracketInkBottomInPixels, (static_cast<float>(lastInkRow + 1) - 13.5f) * static_cast<float>(scale), 0.001f);

        // The bracket sits somewhere other than the metrics midpoint, which is
        // the whole reason the mark is centred on it. Without this the pass would
        // be consistent with a rasterizer reporting the midpoint twice.
        const float bracketMid = (face.bracketInkTopInPixels - face.bracketInkBottomInPixels) * 0.5f;
        const float metricsMid = (face.ascent - face.descent) * 0.5f;
        CHECK_MSG(std::fabs(bracketMid - metricsMid) > 0.25f,
            "this face's bracket and its metrics midpoint differ, so a mark centred on either "
            "is distinguishable from one centred on the other");
    }
}

// The x-height is the height of the face's own `x`, measured from the bitmap
// rather than copied out of a passing run. The strike rides
// `baseline - xHeight / 2` and the braille lattice centres on the same point,
// so a value taller than the letter lifts both above the middle of the
// lowercase.
TEST(xHeightIsTheHeightOfTheFacesLowercase)
{
    using Ubiquitty::Rendering::UnifontRasterizer;

    // The face's own 16 rows, with the baseline 13.5 down them, as the bracket
    // measurement above reads them.
    constexpr int box = 16;
    std::uint8_t coverage[box * box] {};
    UnifontRasterizer native(1, 0);
    REQUIRE(native.rasterizeGlyph('x', Ubiquitty::Rendering::FontFace::Regular, 0, box, box, coverage));
    int firstInkRow = -1;
    int lastInkRow = -1;
    for (int row = 0; row < box; ++row) {
        bool hasInk = false;
        for (int column = 0; column < box; ++column)
            hasInk = hasInk || coverage[row * box + column] != 0;
        if (!hasInk)
            continue;
        if (firstInkRow < 0)
            firstInkRow = row;
        lastInkRow = row;
    }
    REQUIRE(firstInkRow >= 0);
    const float measured = static_cast<float>(lastInkRow + 1 - firstInkRow);
    const float inkMiddle = static_cast<float>(firstInkRow + lastInkRow + 1) * 0.5f;

    for (int scale = 1; scale <= 3; ++scale) {
        const float scaleFactor = static_cast<float>(scale);
        const auto metrics = UnifontRasterizer(scale, 0).cellMetrics();
        CHECK_NEAR(metrics.xHeight, measured * scaleFactor, 0.001f);
        const float strikeCentre = metrics.baselineFromTop - metrics.xHeight * 0.5f;
        CHECK_MSG(std::fabs(strikeCentre - inkMiddle * scaleFactor) <= 0.5f * scaleFactor,
            "the strike crosses the lowercase within half a face row of its middle");
    }
}

// `font.character_spacing` on the bitmap tier, which widens the CELL rather than
// a pitch laid around it.
//
// A pitch wider than the slot is a magnifying blit: the 8-column bitmap is
// resampled across the extra columns, so a face with no antialiaser draws
// duplicated stems, and a pitch carrying half a pixel puts a cell edge on a
// pixel centre where the tap can reach the texel before the slot. Widening the
// cell instead keeps the blit 1:1 and leaves the spacing as blank columns the
// glyph is centred between — the horizontal twin of what line spacing already
// does, and the reason `rasteriseUnifontIntoSlot` centres on both axes.
TEST(characterSpacingWidensTheBitmapCellAroundItsGlyph)
{
    using Ubiquitty::Rendering::UnifontRasterizer;

    for (int scale = 1; scale <= 3; ++scale) {
        for (int spacing = 0; spacing <= 4; ++spacing) {
            const UnifontRasterizer rasterizer(scale, 0, spacing);
            CHECK_NEAR(rasterizer.cellMetrics().cellWidth,
                static_cast<float>(8 * scale + spacing), 0.001f);
        }
    }

    // Tightening is what negative spacing is for, and a cell still has to have a
    // column in it: the floor is the answer, not a cell of zero or fewer.
    CHECK_NEAR(UnifontRasterizer(1, 0, -4).cellMetrics().cellWidth, 4.0f, 0.001f);
    CHECK_NEAR(UnifontRasterizer(1, 0, -32).cellMetrics().cellWidth, 1.0f, 0.001f);

    // The raster in the widened cell is the raster in the plain one, moved right
    // by half the gap — the same bytes, not a resample of them. Asserted against
    // the plain fill rather than against recorded columns so the claim is about
    // the placement and survives a change to the face.
    constexpr int scale = 2;
    constexpr int nativeWidth = 8;
    constexpr int nativeHeight = 16;
    constexpr int plainWidth = nativeWidth * scale;
    constexpr int slotHeight = nativeHeight * scale;
    constexpr int spacing = 3;
    constexpr int widenedWidth = plainWidth + spacing;

    std::uint8_t plain[plainWidth * slotHeight] {};
    std::uint8_t widened[widenedWidth * slotHeight] {};
    CHECK(Ubiquitty::Rendering::rasteriseUnifontIntoSlot('m', scale, nativeWidth, nativeHeight,
        Ubiquitty::Rendering::CoverageSlot(plain, sizeof plain), plainWidth, slotHeight));
    CHECK(Ubiquitty::Rendering::rasteriseUnifontIntoSlot('m', scale, nativeWidth, nativeHeight,
        Ubiquitty::Rendering::CoverageSlot(widened, sizeof widened), widenedWidth, slotHeight));

    // Integer halving, spare column to the right, as the vertical fill spares its
    // pixel below.
    constexpr int inset = spacing / 2;
    bool didDrawInk = false;
    for (int row = 0; row < slotHeight; ++row) {
        for (int column = 0; column < widenedWidth; ++column) {
            const std::uint8_t drawn = widened[row * widenedWidth + column];
            const int sourceColumn = column - inset;
            const std::uint8_t expected = (sourceColumn >= 0 && sourceColumn < plainWidth)
                ? plain[row * plainWidth + sourceColumn]
                : static_cast<std::uint8_t>(0);
            CHECK_EQ(static_cast<int>(drawn), static_cast<int>(expected));
            didDrawInk = didDrawInk || drawn != 0;
        }
    }
    CHECK_MSG(didDrawInk, "a blank raster would satisfy the comparison above without placing anything");

    // Tightening below the box condenses the glyph into the cell rather than
    // cutting what hangs past it: a letter missing its right side is the failure
    // a destination-major walk exists to avoid, and it is invisible in the
    // widened case above because nothing there falls outside.
    constexpr int tightWidth = plainWidth - 4;
    std::uint8_t tight[tightWidth * slotHeight] {};
    CHECK(Ubiquitty::Rendering::rasteriseUnifontIntoSlot('m', scale, nativeWidth, nativeHeight,
        Ubiquitty::Rendering::CoverageSlot(tight, sizeof tight), tightWidth, slotHeight));
    for (int row = 0; row < slotHeight; ++row) {
        bool hasPlainInk = false;
        bool hasTightInk = false;
        for (int column = 0; column < plainWidth; ++column)
            hasPlainInk = hasPlainInk || plain[row * plainWidth + column] != 0;
        for (int column = 0; column < tightWidth; ++column)
            hasTightInk = hasTightInk || tight[row * tightWidth + column] != 0;
        CHECK_EQ(hasTightInk, hasPlainInk);
    }
}

// The colour square is derived independently by the atlas that cuts the slot and
// by the builder that draws into it, and the two must agree exactly or every
// colour glyph is a resample. They agree today because every rasterizer this
// codebase ships hands out WHOLE-NUMBER metrics, which makes the floor in
// `colourGlyphSquareInPixels` and the round in `cellDimension` both no-ops — so
// the arithmetic cannot separate two callers even when they spell the cell
// differently.
//
// That is a property of the producers, not of the derivation, and nothing else
// states it. A rasterizer returning a fractional cell would leave the square
// correct and the slot a texel wider, which no per-backend golden can see
// because both arms of one backend resample identically.
//
// Unifont is the portable one and is asserted here. `FreeTypeRasterizer`
// (Linux) holds the same property for the same reason: `cellMetrics` assigns
// `cellWidth` from an `int` advance and `cellHeight` from `cellLayout`, whose
// terms are all `int`-derived and only summed.
TEST(everyShippingRasterizerHandsOutAWholeNumberCell)
{
    using Ubiquitty::Rendering::cellDimension;
    using Ubiquitty::Rendering::colourGlyphSquareInPixels;

    for (int scale = 1; scale <= 3; ++scale) {
        for (int spacing = 0; spacing <= 5; ++spacing) {
            const Ubiquitty::Rendering::UnifontRasterizer rasterizer(scale, spacing);
            const auto metrics = rasterizer.cellMetrics();

            CHECK_MSG(metrics.cellWidth == std::floor(metrics.cellWidth),
                "a fractional cell width would let the slot and the quad round apart");
            CHECK_MSG(metrics.cellHeight == std::floor(metrics.cellHeight),
                "a fractional cell height would let the slot and the quad round apart");

            // The two roundings a caller may sit behind, on the same cell. Equal
            // for a whole cell; a texel apart the moment it is not.
            CHECK_EQ(cellDimension(metrics.cellWidth), static_cast<int>(metrics.cellWidth));
            CHECK_EQ(cellDimension(metrics.cellHeight), static_cast<int>(metrics.cellHeight));
        }
    }

    // What the wholeness above buys, shown on a cell that does not have it. A
    // caller that rounds the cell before deriving and one that does not are the
    // two positions the atlas and the builder occupy, and on 13.6 by 28.4 they
    // land a texel apart — a slot of 28 texels drawn across a 27-pixel square.
    //
    // Unifont cannot demonstrate this: its cell is 8 by 16 per scale step, so
    // twice the width never exceeds the height and the height binds every time,
    // which makes any rounding of the width invisible. Asserting the separation
    // on Unifont's own metrics would be a check that cannot fail.
    {
        const float fromFloats = colourGlyphSquareInPixels(13.6f, 28.4f);
        const float fromRounded = colourGlyphSquareInPixels(
            static_cast<float>(cellDimension(13.6f)), static_cast<float>(cellDimension(28.4f)));
        CHECK_NEAR(fromFloats, 27.0f, 0.001f);
        CHECK_NEAR(fromRounded, 28.0f, 0.001f);
        CHECK_MSG(fromFloats != fromRounded,
            "a fractional cell separates the slot's derivation from the quad's");
    }
}

TEST(missingUnifontGlyphClearsAReusedCoverageSlot)
{
    std::array<std::uint8_t, 16 * 36> coverage;
    coverage.fill(255);
    CHECK(!Ubiquitty::Rendering::rasteriseUnifontIntoSlot(0x10FFFF, 2, 8, 16, coverage, 16, 36));
    CHECK(std::ranges::all_of(coverage, [](std::uint8_t value) { return value == 0; }));
}

TEST(wideUnifontSourceRetainsColumnsInASingleScaledCell)
{
    Ubiquitty::Rendering::UnifontRasterizer scaled(2);
    std::array<std::uint8_t, 16 * 36> actual {};
    std::array<std::uint8_t, 16 * 36> expected {};
    for (const auto codePoint : { 0x2605u, 0x2606u, 0x2934u, 0x2935u }) {
        CHECK(Ubiquitty::Rendering::rasteriseUnifontIntoSlot(codePoint, 2, 8, 16, actual, 16, 36));
        CHECK(scaled.rasterizeGlyphInCell(codePoint, 16, 32, 16, 36, expected));
        CHECK(actual == expected);
    }
}

TEST(nativeUnifontCoverageMatchesThePackedFont)
{
    using namespace Ubiquitty::Rendering;
    UnifontRasterizer rasterizer(1);
    constexpr int slotWidth = 24;
    constexpr int slotHeight = 24;
    std::array<std::uint8_t, slotWidth * slotHeight> actual;
    int compared = 0;
    for (std::uint32_t codePoint = 0; codePoint <= 0xffff; ++codePoint) {
        const auto glyph = Unifont::lookup(codePoint);
        if (!glyph.bits || tilesOnTextPen(codePoint))
            continue;
        REQUIRE(glyph.width == 8 || glyph.width == 16);
        std::array<std::uint8_t, slotWidth * slotHeight> expected {};
        for (int row = 0; row < 16; ++row) {
            for (int column = 0; column < glyph.width; ++column) {
                const auto packed = glyph.bits[row * (glyph.width / 8) + column / 8];
                if (packed & (0x80u >> (column % 8)))
                    expected[(row + 4) * slotWidth + (slotWidth - glyph.width) / 2 + column] = 255;
            }
        }
        actual.fill(0xA5); // The margins must be cleared, not just the ink written.
        REQUIRE(rasterizer.rasterizeGlyphInCell(codePoint, glyph.width, 16, slotWidth, slotHeight, actual));
        CHECK(actual == expected);
        ++compared;
    }
    CHECK(compared > 10000);
}

UBIQUITTY_TEST_MAIN()
