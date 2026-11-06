// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_glyph_tiling — which code points no pen may re-place.
//
// The predicate alone keeps a stacked bracket's stem in one column across its
// rows, so it is pinned by name rather than by range: the symbol-fallback pen
// centres a glyph on its own ink,
// and a hook's ink is wider than the extension's stem it continues, so the two
// are offset by half the difference. Exempting the family is what leaves the
// face's own placement — which tiles, in every face that ships these — intact.
//
// The negative cases are the discriminating ones. A predicate that answered
// true for everything would fix the brackets and un-fix every symbol the
// centring exists for, and only a case that expects false can catch that.

#include "Harness.h"

#include "Core/Rendering/BoxEdgeStretch.h"
#include "Core/Rendering/GlyphTiling.h"
#include "Core/Rendering/UnifontRasterizer.h"

#include <cmath>
#include <cstdint>
#include <vector>

namespace {

using Ubiquitty::Rendering::edgeStretchBoxScratch;
using Ubiquitty::Rendering::tilesOnTextPen;
using Ubiquitty::Rendering::tilingReach;

TEST(testEveryPieceOfAStackedBracketIsExempt)
{
    // The pieces, by family. Each must answer alike: a family with one piece
    // exempt and one not is misaligned exactly as if neither were.
    constexpr unsigned pieces[] = {
        0x2320,
        0x2321,
        0x23AE, // integral: top, bottom, extension
        0x239B,
        0x239C,
        0x239D, // left parenthesis
        0x239E,
        0x239F,
        0x23A0, // right parenthesis
        0x23A1,
        0x23A2,
        0x23A3, // left square bracket
        0x23A4,
        0x23A5,
        0x23A6, // right square bracket
        0x23A7,
        0x23A8,
        0x23A9,
        0x23AA, // left curly bracket
        0x23AB,
        0x23AC,
        0x23AD, // right curly bracket
        0x23B0,
        0x23B1, // the S-shaped brace sections
    };
    for (unsigned codePoint : pieces) {
        CHECK_MSG(tilesOnTextPen(codePoint), "a bracket piece draws where the face put it");
    }
}

TEST(testTheVerticalBoxLinesAreExempt)
{
    // Their content is the edge they stand on; a centred one is U+2502.
    CHECK_MSG(tilesOnTextPen(0x23B8), "U+23B8 stands on the left edge");
    CHECK_MSG(tilesOnTextPen(0x23B9), "U+23B9 stands on the right edge");
}

TEST(testOrdinarySymbolsStillCentre)
{
    // The glyphs the centring exists for: a fallback face's em is not the
    // primary's, and these are improved by being centred on the cell box. If
    // the exemption reached them it would undo that silently.
    constexpr unsigned centred[] = {
        0x2318, // `U+2318 PLACE OF INTEREST SIGN`
        0x2211, // N-ARY SUMMATION
        0x2192, // RIGHTWARDS ARROW
        0x25CF, // BLACK CIRCLE
        0x2502, // BOX DRAWINGS LIGHT VERTICAL — centred is its meaning
        0x23BF, // a dentistry corner: the box family places it, not this
        0x0041, // an ordinary letter
        0x1F600, // an emoji
    };
    for (unsigned codePoint : centred) {
        CHECK_MSG(!tilesOnTextPen(codePoint), "an ordinary symbol is still centred");
    }
}

TEST(testABoxCornerStandsOnTheColumnItsVerticalDoes)
{
    // A face on a 12px advance inks `│` at columns 5..6 of the advance and `┌`
    // from 5 to 12, its arm running past the advance to meet the next cell.
    // Centred on their own ink the two land three columns apart, and a frame
    // breaks at every corner.
    using Ubiquitty::Rendering::rasterLeftInSlot;
    constexpr int slotWidth = 18;
    constexpr double advance = 12;
    const int vertical = rasterLeftInSlot(0x2502, slotWidth, 5, 2, advance);
    const int corner = rasterLeftInSlot(0x250C, slotWidth, 5, 8, advance);
    const int rightCorner = rasterLeftInSlot(0x2510, slotWidth, -1, 8, advance);
    CHECK_EQ(vertical, 8);
    CHECK_EQ(corner, vertical);
    CHECK_EQ(rightCorner + 6, vertical);
    // The double family's rails, which centring crossed the same way.
    CHECK_EQ(rasterLeftInSlot(0x2554, slotWidth, 4, 9, advance), vertical - 1);
}

TEST(testAnOrdinaryGlyphIsStillCentredOnItsInk)
{
    // The control: the pen governs the box family and the bracket pieces only.
    using Ubiquitty::Rendering::keepsFacePlacement;
    using Ubiquitty::Rendering::rasterLeftInSlot;
    CHECK_EQ(rasterLeftInSlot('r', 18, 2, 7, 12), 5);
    CHECK_EQ(rasterLeftInSlot(0x2318, 18, 0, 12, 12), 3);
    CHECK_MSG(keepsFacePlacement(0x2500) && keepsFacePlacement(0x257F), "the whole box-drawing block");
    CHECK_MSG(keepsFacePlacement(0x239B), "and the bracket pieces");
    CHECK_MSG(!keepsFacePlacement(0x24FF) && !keepsFacePlacement(0x2580), "and nothing either side of it");
    CHECK_MSG(!keepsFacePlacement('A'), "an ordinary letter");
}

// The run's edges, so a later trim cannot quietly drop a piece, and its
// neighbours, so a later widening cannot quietly swallow one.
//
// U+23B4..U+23B6 and U+23DC..U+23E1 are the over- and under-brackets. They
// assemble too, but ACROSS a row rather than down a column: a top curly bracket
// spans the columns above an expression. Their seam is a horizontal one and
// their reach is left and right, so they are not this set and must not be
// swept into it by a range that grew.
TEST(testTheRunStopsWhereTheFamilyDoes)
{
    CHECK_MSG(!tilesOnTextPen(0x239A), "before the family");
    CHECK_MSG(!tilesOnTextPen(0x231F), "before the integral halves");
    CHECK_MSG(!tilesOnTextPen(0x2322), "after them");
    CHECK_MSG(tilesOnTextPen(0x23B2), "the summation's halves tile on the text pen, pinned to their joining edge");
    CHECK_MSG(tilesOnTextPen(0x23B3), "both of them");
    CHECK_MSG(!tilesOnTextPen(0x23B4), "the top square bracket assembles across a row");
    CHECK_MSG(!tilesOnTextPen(0x23B6), "and so does the one that is both of them");
    CHECK_MSG(!tilesOnTextPen(0x23DC), "the top parenthesis, likewise");
    CHECK_MSG(!tilesOnTextPen(0x23DF), "and the bottom curly bracket");
    CHECK_MSG(!tilesOnTextPen(0x23AF), "the HORIZONTAL line extension is the other axis");
    CHECK_MSG(!tilesOnTextPen(0x23D1), "past the vertical line extension");
    CHECK_MSG(!tilesOnTextPen(0x2E58), "before the half parentheses");
    CHECK_MSG(!tilesOnTextPen(0x2E5D), "after them");
}

TEST(testAHookReachesOnlyTheEdgeItContinuesInto)
{
    // Stretching a hook toward the edge it has already curved away from would
    // draw a stem where the bracket ends.
    CHECK_MSG(tilingReach(0x239B).reachesBottom && !tilingReach(0x239B).reachesTop, "an upper hook continues DOWN");
    CHECK_MSG(tilingReach(0x239D).reachesTop && !tilingReach(0x239D).reachesBottom, "a lower hook continues UP");
    CHECK_MSG(tilingReach(0x239C).reachesTop && tilingReach(0x239C).reachesBottom,
        "an extension continues both ways");
    CHECK_MSG(!tilingReach(0x239C).isMiddle, "and it is the piece that repeats");
    // A brace's middle reaches both edges and is still a fixed shape: TeX's
    // recipe separates `mid` from `rep` for exactly this reason.
    CHECK_MSG(tilingReach(0x23A8).isMiddle && tilingReach(0x23AC).isMiddle,
        "the braces' middles are not extenders");
    CHECK_MSG(tilingReach(0x23A8).reachesTop && tilingReach(0x23A8).reachesBottom,
        "though they do continue both ways");
    CHECK_MSG(!tilingReach(0x23AA).isMiddle, "the curly extension is the repeating piece");
    CHECK_MSG(!tilingReach(0x0041).isTilingPiece, "an ordinary letter is not stretched");
    CHECK_MSG(!tilingReach(0x2318).isTilingPiece, "nor an ordinary symbol");
}

// The stretch must replicate the solid stroke, never the antialiased row at
// its end. A face that lands a stem's last row on a pixel boundary leaves that row
// at partial coverage; replicating it would run a faint stem down the whole
// extension while the rows above stayed solid, which reads as a stroke that
// fades exactly where it should be joining.
TEST(testTheStretchSamplesSolidInkNotTheAntialiasedEdge)
{
    constexpr int width = 12;
    constexpr int height = 20;
    constexpr int boxLeft = 1;
    constexpr int boxRight = 11;
    constexpr int boxTop = 1;
    constexpr int boxBottom = 19;
    constexpr std::uint8_t faint = 60; // a half-covered edge row
    std::vector<std::uint8_t> plane(static_cast<std::size_t>(width) * height, 0);
    auto at = [&](int x, int y) -> std::uint8_t& { return plane[static_cast<std::size_t>(y) * width + x]; };

    // A stem two columns wide: solid through the body, and a faint final row
    // above a bearing gap that reaches neither the box's bottom nor the plane's.
    constexpr int stemLeft = 5;
    constexpr int stemRight = 7;
    constexpr int lastSolidRow = 14;
    for (int y = boxTop; y <= lastSolidRow; ++y) {
        for (int x = stemLeft; x < stemRight; ++x) {
            at(x, y) = 255;
        }
    }
    for (int x = stemLeft; x < stemRight; ++x) {
        at(x, lastSolidRow + 1) = faint;
    }

    const auto reach = tilingReach(0x239C); // an extension: both edges
    CHECK_MSG(reach.isTilingPiece && reach.reachesBottom, "the fixture uses a piece that continues down");
    edgeStretchBoxScratch(plane.data(), width, height, boxLeft, boxRight, boxTop, boxBottom,
        false, false, reach.reachesTop, reach.reachesBottom);

    // Every row below the sample carries the SOLID stem, and the faint row is
    // gone rather than smeared down the cell.
    for (int y = lastSolidRow + 1; y < height; ++y) {
        CHECK_MSG(at(stemLeft, y) == 255, "the stem runs solid to the plane's bottom edge");
        CHECK_MSG(at(stemLeft, y) != faint, "the antialiased row was not the one replicated");
    }
    // And the stretch did not widen the stem into the columns beside it.
    for (int y = lastSolidRow + 1; y < height; ++y) {
        CHECK_MSG(at(stemLeft - 1, y) == 0, "the empty column beside the stem stays empty");
    }
}

TEST(tilingPlacementPreservesHooksAndPinsTheirJoiningEdges)
{
    using Ubiquitty::Rendering::fitTilingGlyph;
    const auto upper = fitTilingGlyph(0x239B, -3, 10, 2, 20);
    CHECK(upper.isApplicable);
    CHECK_EQ(upper.verticalScale, 1.0);
    CHECK_EQ((upper.baseline - 3) * upper.verticalScale, 2.0);
    const auto lower = fitTilingGlyph(0x239D, -3, 10, 2, 20);
    CHECK_EQ((lower.baseline + 7) * lower.verticalScale, 22.0);
    const auto extension = fitTilingGlyph(0x239C, -3, 10, 2, 20);
    CHECK_EQ(extension.verticalScale, 2.0);
    CHECK_EQ((extension.baseline - 3) * extension.verticalScale, 2.0);
    const auto middle = fitTilingGlyph(0x23A8, -3, 10, 2, 20);
    CHECK_EQ(middle.verticalScale, 1.0);
    CHECK_EQ((middle.baseline + 2) * middle.verticalScale, 12.0);
    CHECK_EQ(fitTilingGlyph(0x23A8, -3, 40, 2, 20).verticalScale, 0.5);
    CHECK_EQ(fitTilingGlyph(0x23B2, -3, 40, 2, 20).verticalScale, 1.0);
    CHECK(!fitTilingGlyph('A', -3, 10, 2, 20).isApplicable);
    CHECK(!fitTilingGlyph(0x239C, 0, 0, 2, 20).isApplicable);
}

TEST(tilingCoverageClosesFringesWithoutPaintingNeighbourRows)
{
    constexpr int width = 8;
    constexpr int height = 10;
    std::vector<std::uint8_t> plane(width * height, 0);
    for (int row = 0; row < height; ++row) {
        plane[row * width + 3] = row == 2 || row == 7 ? 128 : 255;
    }
    Ubiquitty::Rendering::TilingCoverage(plane, width, height, 1, 7, 2, 8).stretch(0x239C);
    for (int row = 0; row < height; ++row) {
        CHECK_EQ(plane[row * width + 3], row >= 2 && row < 8 ? 255 : 0);
        CHECK_EQ(plane[row * width + 2], 0);
    }
    // An invalid plane cannot be indexed, and a blank donor cannot make ink.
    Ubiquitty::Rendering::TilingCoverage({}, width, height, 1, 7, 2, 8).stretch(0x239C);
    std::fill(plane.begin(), plane.end(), 0);
    CHECK(!Ubiquitty::Rendering::TilingCoverage(plane, width, height, 1, 7, 2, 8).fillFromStemRow());
}

TEST(unifontExtensionsMatchTheirHooksAtEveryScaleAndSpacing)
{
    using namespace Ubiquitty::Rendering;
    for (int scale : { 1, 2, 3 }) {
        for (int spacing : { 0, 3, 8 }) {
            const int width = 8 * scale;
            const int height = 16 * scale + spacing;
            UnifontRasterizer rasterizer(scale, spacing);
            for (unsigned extension : { 0x239C, 0x239F, 0x23A2, 0x23A5, 0x23AE }) {
                std::vector<std::uint8_t> hook(width * height);
                std::vector<std::uint8_t> stem(width * height);
                CHECK(rasterizer.rasterizeGlyph(stemDonor(extension), FontFace::Regular, 0, width, height, hook.data()));
                CHECK(rasterizer.rasterizeGlyph(extension, FontFace::Regular, 0, width, height, stem.data()));
                CHECK(std::any_of(stem.begin(), stem.end(), [](auto value) { return value != 0; }));
                for (int row = 0; row < height; ++row) {
                    CHECK(std::equal(hook.end() - width, hook.end(), stem.begin() + row * width));
                }
                std::vector<std::uint8_t> metal(width * height, 127);
                CHECK(rasteriseUnifontIntoSlot(extension, scale, 8, 16, metal, width, height));
                CHECK(metal == stem);
            }
        }
    }
}

TEST(unifontAssembliesHaveInkAcrossEveryJoiningRow)
{
    using namespace Ubiquitty::Rendering;
    const std::vector<std::vector<unsigned>> assemblies {
        { 0x2320, 0x23AE, 0x2321 },
        { 0x239B, 0x239C, 0x239D },
        { 0x239E, 0x239F, 0x23A0 },
        { 0x23A1, 0x23A2, 0x23A3 },
        { 0x23A4, 0x23A5, 0x23A6 },
        { 0x23A7, 0x23AA, 0x23A8, 0x23AA, 0x23A9 },
        { 0x23AB, 0x23AA, 0x23AC, 0x23AA, 0x23AD },
        { 0x23D0, 0x23D0 }
    };
    for (int scale : { 1, 2, 3 }) {
        const int width = 8 * scale;
        const int height = 16 * scale + 7;
        UnifontRasterizer rasterizer(scale, 7);
        for (const auto& assembly : assemblies) {
            std::vector<std::uint8_t> previous(width * height);
            std::vector<std::uint8_t> current(width * height);
            for (std::size_t piece = 0; piece < assembly.size(); ++piece) {
                CHECK(rasterizer.rasterizeGlyph(assembly[piece], FontFace::Regular, 0, width, height, current.data()));
                if (piece != 0) {
                    bool isJoined = false;
                    for (int column = 0; column < width; ++column) {
                        isJoined |= previous[(height - 1) * width + column] != 0 && current[column] != 0;
                    }
                    CHECK_MSG(isJoined, "adjacent pieces share an ink column on their joining rows");
                }
                previous.swap(current);
            }
        }
    }
}

// The box family is drawn from unhinted outlines and moved by one offset, and
// the offset is what decides whether the family lands on whole pixels. The
// first case is the measured face: JetBrains Mono at 20 px draws its light
// rule at y [-8.41, -6.41) and x [5, 7), and the offset that lands that rule
// on whole rows is +0.41 on y and nothing on x.
TEST(boxFamilyPixelOffsetLandsTheReferenceRuleOnWholePixels)
{
    using Ubiquitty::Rendering::boxFamilyPixelOffset;
    using Ubiquitty::Rendering::strokePixelOffset;

    const auto jetBrains = boxFamilyPixelOffset(-8.41, -6.41, 5.0, 7.0);
    CHECK_NEAR(jetBrains.y, 0.41, 1e-9);
    CHECK_NEAR(jetBrains.x, 0.0, 1e-9);

    // A one-pixel rule centres on a pixel centre, not a boundary: centred on a
    // boundary it would cover two half pixels.
    CHECK_NEAR(strokePixelOffset(2.2, 3.2), -0.2, 1e-9);
    // A fractional width centres on the parity of its rounded width, so a
    // 1.6 px rule straddles a boundary symmetrically like a 2 px one.
    CHECK_NEAR(strokePixelOffset(3.1, 4.7), 0.1, 1e-9);

    // A face without a reference leaves that axis alone.
    CHECK_NEAR(strokePixelOffset(0.0, 0.0), 0.0, 1e-12);
    CHECK_NEAR(strokePixelOffset(std::nan(""), 1.0), 0.0, 1e-12);

    // Never more than half a pixel, which is what keeps the move invisible
    // as a move: the stroke stays on the pixel its design put it nearest.
    bool isBounded = true;
    for (int step = -400; step <= 400; ++step) {
        const double low = step / 64.0;
        for (const double width : { 0.7, 1.0, 1.4, 2.0, 2.6, 3.0 }) {
            const double offset = strokePixelOffset(low, low + width);
            isBounded = isBounded && offset >= -0.5 && offset <= 0.5;
        }
    }
    CHECK_MSG(isBounded, "the family offset never moves a stroke more than half a pixel");
}

// The unhinted set is the box-drawing block and nothing else: a bracket piece
// or a block element keeps whatever load its face gives it.
TEST(onlyTheBoxDrawingBlockRastersUnhinted)
{
    using Ubiquitty::Rendering::rastersFromUnhintedOutline;
    CHECK(rastersFromUnhintedOutline(0x2500));
    CHECK(rastersFromUnhintedOutline(0x253C));
    CHECK(rastersFromUnhintedOutline(0x257F));
    CHECK(!rastersFromUnhintedOutline(0x24FF));
    CHECK(!rastersFromUnhintedOutline(0x2580));
    CHECK(!rastersFromUnhintedOutline(0x23D0));
    CHECK(!rastersFromUnhintedOutline('A'));
}

} // namespace

UBIQUITTY_TEST_MAIN()
