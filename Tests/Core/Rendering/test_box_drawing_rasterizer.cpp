// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_box_drawing_rasterizer — the DECORATOR, as distinct from the stretch it
// applies.
//
// test_box_edge_stretch covers the geometry on a buffer handed to it directly.
// This file covers whether wrapping a rasterizer in BoxDrawingRasterizer
// actually routes a box code point through that geometry, which is the part a
// frontend gets wrong: macOS calls edgeStretchBoxScratch inline, while Linux and
// the browser compose this decorator.
//
// The inner rasterizer here draws with side bearings — a stroke that
// stops two columns short of each edge, which is what a real proportional face
// does and why two adjacent box cells show a hairline gap. That makes every
// assertion below discriminating: an unwrapped face fails them, so a decorator
// that silently passed the glyph through could not pass.
//
// A seam test through a real font cannot stand in for this. The browser's own
// fallback face (GNU Unifont) draws box glyphs edge to edge already, so a
// rendered-image check of two adjacent cells reports JOINED whether or not the
// decorator is in the chain — a check that cannot fail.
//
// What this does not cover, measured rather than assumed: whether a given
// frontend actually composes the decorator. Bypassing it in
// WebTerminal::FontStack leaves every case here green, because these test the
// decorator and not the wiring. The discriminating input for the wiring is a
// face WITH bearings, and the browser only has one inside a real browser —
// webFontMeasure needs a canvas, so under ctest and under node the face is
// unavailable and Unifont stands in. Closing that needs an injectable font stack
// rather than a cleverer assertion, and it is worth more than it costs only if a
// second frontend gets this wrong.

#include "Harness.h"

#include "Core/Rendering/BoxDrawingRasterizer.h"

#include <vector>

using Ubiquitty::Rendering::BoxDrawingRasterizer;
using Ubiquitty::Rendering::CellMetrics;
using Ubiquitty::Rendering::FaceMetrics;
using Ubiquitty::Rendering::FontFace;
using Ubiquitty::Rendering::GlyphRasterizer;

namespace {

constexpr int slotWidth = 10;
constexpr int slotHeight = 16;
constexpr int bearing = 2; // columns of empty margin the face leaves each side

// A face that draws U+2500 as a one-row stroke inside its advance, with bearing
// on both sides — the shape that tiles wrongly. Everything else is a solid block
// so a pass-through can be told from a redraw.
struct BearingFace final : GlyphRasterizer {
    int rasterizeCalls = 0;

    [[nodiscard]] CellMetrics cellMetrics() const override
    {
        CellMetrics m;
        m.cellWidth = static_cast<float>(slotWidth);
        m.cellHeight = static_cast<float>(slotHeight);
        m.baselineFromTop = 13.0f;
        m.deviceScale = 1.0f;
        m.atlasToDevicePixels = 1.0f;
        return m;
    }

    [[nodiscard]] FaceMetrics faceMetrics() const override
    {
        FaceMetrics face;
        face.fontSize = 16;
        face.ascent = 13.0f;
        face.descent = 3.0f;
        face.lineGapTopInPixels = 0.0f;
        return face;
    }

    bool rasterizeGlyph(std::uint32_t codePoint, FontFace, std::uint32_t, int width, int height, std::uint8_t* out) override
    {
        ++rasterizeCalls;
        for (int i = 0; i < width * height; ++i)
            out[i] = 0;
        if (codePoint == 0x2500) {
            const int row = height / 2;
            for (int x = bearing; x < width - bearing; ++x)
                out[row * width + x] = 255;
            return true;
        }
        for (int i = 0; i < width * height; ++i)
            out[i] = 200; // a solid block, recognisably not a stroke
        return true;
    }
};

// Ink anywhere in a column.
bool columnHasInk(const std::vector<std::uint8_t>& px, int column)
{
    for (int y = 0; y < slotHeight; ++y)
        if (px[static_cast<std::size_t>(y) * slotWidth + column] != 0)
            return true;
    return false;
}

std::vector<std::uint8_t> raster(GlyphRasterizer& rasterizer, std::uint32_t codePoint)
{
    std::vector<std::uint8_t> px(static_cast<std::size_t>(slotWidth) * slotHeight, 0);
    rasterizer.rasterizeGlyph(codePoint, FontFace::Regular, 0, slotWidth, slotHeight, px.data());
    return px;
}

} // namespace

TEST(boxDrawingRasterizerReachesTheCellEdges)
{
    BearingFace face;

    // The negative first, and it is the whole basis of the case: the face on its
    // own leaves both edge columns empty, which is the gap a user sees.
    {
        const std::vector<std::uint8_t> bare = raster(face, 0x2500);
        CHECK_MSG(!columnHasInk(bare, 0), "the unwrapped face leaves the left edge column empty");
        CHECK_MSG(!columnHasInk(bare, slotWidth - 1), "and the right edge column too");
        CHECK_MSG(columnHasInk(bare, slotWidth / 2), "but it does draw the stroke it was asked for");
    }

    // Wrapped, the same code point reaches both edges. This fails if the
    // decorator forwards to the inner without applying the stretch.
    {
        BoxDrawingRasterizer boxes(face);
        const std::vector<std::uint8_t> stretched = raster(boxes, 0x2500);
        CHECK_MSG(columnHasInk(stretched, 0), "the decorator carries the stroke to the left cell edge");
        CHECK_MSG(columnHasInk(stretched, slotWidth - 1), "and to the right cell edge, so neighbours join");
    }
}

// A decorator inherits its inner's obligations, and the ones it drops fail
// quietly. `faceMetrics` defaults to a zero font size, which every consumer reads
// as "this rasterizer keeps no face" — so a decorator that forgot to forward it
// would turn the caller's type-relative geometry OFF (the OSC 133 bracket draws
// nothing at a zero size) while every glyph still rendered correctly.
TEST(boxDrawingRasterizerForwardsTheFace)
{
    BearingFace face;
    BoxDrawingRasterizer boxes(face);

    CHECK_EQ(boxes.faceMetrics().fontSize, face.faceMetrics().fontSize);
    CHECK_NEAR(boxes.faceMetrics().ascent, face.faceMetrics().ascent, 0.001f);
    CHECK_NEAR(boxes.faceMetrics().descent, face.faceMetrics().descent, 0.001f);
    CHECK_MSG(boxes.faceMetrics().fontSize != 0,
        "a zero font size is how a rasterizer says it has no face — forwarding must not lose it");

    // The cell metrics forward too; asserted so an edit cannot fix one
    // accessor by breaking the other.
    CHECK_NEAR(boxes.cellMetrics().cellWidth, face.cellMetrics().cellWidth, 0.001f);
}

// Everything that is not a box form goes through untouched. Without this the
// decorator could pass its own tests by stretching every glyph, which would
// smear ordinary text to the cell edges.
TEST(boxDrawingRasterizerLeavesOtherGlyphsAlone)
{
    BearingFace face;
    BoxDrawingRasterizer boxes(face);

    const std::vector<std::uint8_t> direct = raster(face, 0x41); // 'A'
    const std::vector<std::uint8_t> wrapped = raster(boxes, 0x41);
    CHECK_MSG(direct == wrapped, "a non-box code point is the inner's output byte for byte");
}

namespace {

// A face whose light rule sits high in the cell, so the family centring shift
// moves it, and which ships no U+253C, so the cross is healed procedurally
// from the profile measured off the rules it does ship.
struct HighRuleFace final : GlyphRasterizer {
    static constexpr int ruleTop = 3;
    static constexpr int ruleBottom = 5; // exclusive
    static constexpr int stemLeft = 4;
    static constexpr int stemRight = 6; // exclusive

    [[nodiscard]] CellMetrics cellMetrics() const override { return BearingFace {}.cellMetrics(); }
    [[nodiscard]] FaceMetrics faceMetrics() const override { return BearingFace {}.faceMetrics(); }

    bool rasterizeGlyph(std::uint32_t codePoint, FontFace, std::uint32_t, int width, int height, std::uint8_t* out) override
    {
        for (int i = 0; i < width * height; ++i)
            out[i] = 0;
        if (codePoint == 0x2500) {
            for (int y = ruleTop; y < ruleBottom; ++y)
                for (int x = bearing; x < width - bearing; ++x)
                    out[y * width + x] = 255;
            return true;
        }
        if (codePoint == 0x2502) {
            for (int y = 0; y < height; ++y)
                for (int x = stemLeft; x < stemRight; ++x)
                    out[y * width + x] = 255;
            return true;
        }
        return false;
    }
};

// One column of a raster, top to bottom.
std::vector<std::uint8_t> column(const std::vector<std::uint8_t>& px, int x)
{
    std::vector<std::uint8_t> values;
    for (int y = 0; y < slotHeight; ++y)
        values.push_back(px[static_cast<std::size_t>(y) * slotWidth + x]);
    return values;
}

// One row of a raster, left to right.
std::vector<std::uint8_t> row(const std::vector<std::uint8_t>& px, int y)
{
    return { px.begin() + static_cast<std::ptrdiff_t>(y) * slotWidth, px.begin() + static_cast<std::ptrdiff_t>(y + 1) * slotWidth };
}

} // namespace

// The alignment rule at a seam between the two sources: a healed piece's
// stroke is the font piece's stroke where they meet, row for row and column
// for column. The face's rule sits high and the family shift moves every font
// piece, so a profile read from the face's own raster puts the healed cross's
// horizontal `shift` rows from the rule it continues.
TEST(aHealedPieceMeetsTheFontPieceExactlyAtTheSeam)
{
    HighRuleFace face;
    BoxDrawingRasterizer boxes(face);

    const std::vector<std::uint8_t> fontRule = raster(boxes, 0x2500);
    const std::vector<std::uint8_t> fontStem = raster(boxes, 0x2502);
    const std::vector<std::uint8_t> healedCross = raster(boxes, 0x253C);

    // Discriminating only if the font piece moved: a shift of zero leaves the
    // face's raster where the profile reads it either way.
    bool isRuleOnFaceRows = true;
    for (int y = 0; y < slotHeight; ++y) {
        const bool isFaceRow = y >= HighRuleFace::ruleTop && y < HighRuleFace::ruleBottom;
        isRuleOnFaceRows = isRuleOnFaceRows && (fontRule[static_cast<std::size_t>(y) * slotWidth] != 0) == isFaceRow;
    }
    CHECK_MSG(!isRuleOnFaceRows, "the family shift moves the font rule off the rows the face drew it on");

    // The cross's right edge meets the next cell's rule at its left edge, and
    // its left edge meets the previous cell's rule at its right edge.
    CHECK_MSG(column(healedCross, slotWidth - 1) == column(fontRule, 0), "the healed cross's horizontal is the font rule's, row for row, at its right seam");
    CHECK_MSG(column(healedCross, 0) == column(fontRule, slotWidth - 1), "and at its left seam");
    // Likewise its vertical against the font stem above and below.
    CHECK_MSG(row(healedCross, 0) == row(fontStem, slotHeight - 1), "the healed cross's vertical is the font stem's, column for column, at its top seam");
    CHECK_MSG(row(healedCross, slotHeight - 1) == row(fontStem, 0), "and at its bottom seam");
}

UBIQUITTY_TEST_MAIN()
