// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

#include "Core/Rendering/ProportionalGlyphSheet.h"
#include "Core/Rendering/ProportionalStrike.h"
#include "Core/Rendering/Unifont.h"
#include "Core/Rendering/UnifontRasterizer.h"

#include <algorithm>
#include <array>
#include <vector>

using namespace Ubiquitty::Rendering;
using Ubiquitty::ProportionalShapedGlyph;

TEST(missingProportionalGlyphsKeepTheirGridFaceIdentityAndAdvance)
{
    const std::array<std::uint32_t, 3> text { 'A', 0x65E5, 0x0627 };
    for (int scale : { 1, 2 }) {
        UnifontRasterizer rasterizer(scale, 3);
        std::array<ProportionalShapedGlyph, 3> shaped;
        CHECK(rasterizer.shapeProportionalRun(text, FontFace::Regular, shaped) == 3);
        const auto latin = ProportionalStrike::lookup(ProportionalStrike::Face::Regular, scale, 'A', 0);
        CHECK(latin.present);
        CHECK(shaped[0].advanceInPixels == static_cast<float>(latin.advanceInUnits) / ProportionalStrike::unitsPerPixel);
        for (std::size_t index = 1; index < text.size(); ++index) {
            CHECK(!ProportionalStrike::lookup(ProportionalStrike::Face::Regular, scale, text[index], 0).present);
            const Unifont::Glyph fallback = Unifont::lookup(text[index]);
            CHECK(fallback.bits != nullptr);
            CHECK(shaped[index].glyph == text[index]);
            CHECK(shaped[index].advanceInPixels == fallback.width * scale);
            CHECK(shaped[index].cluster == index);
        }
    }
}

TEST(missingStrikeGlyphPixelsMatchTheExistingGridRasterizer)
{
    for (int scale : { 1, 2 }) {
        UnifontRasterizer rasterizer(scale, 3);
        for (std::uint32_t codePoint : { 0x65E5u, 0x0627u }) {
            const Unifont::Glyph fallback = Unifont::lookup(codePoint);
            const int glyphWidth = fallback.width * scale;
            const int glyphHeight = 16 * scale;
            std::vector<std::uint8_t> expected(static_cast<std::size_t>(glyphWidth) * glyphHeight);
            CHECK(rasterizer.rasterizeGlyph(codePoint, FontFace::Regular, 0, glyphWidth, glyphHeight, expected.data()));
            CHECK(std::ranges::any_of(expected, [](std::uint8_t value) { return value != 0; }));

            ProportionalShapedGlyph shaped;
            shaped.glyph = codePoint;
            shaped.offsetXInPixels = 2.75f;
            const int width = glyphWidth + 6;
            const int height = static_cast<int>(rasterizer.cellMetrics().cellHeight)
                + 2 * rasterizer.proportionalStripOverhangInPixels();
            std::vector<std::uint8_t> actual(static_cast<std::size_t>(width) * height);
            CHECK(rasterizer.rasteriseProportionalRun(std::span(&shaped, 1), FontFace::Regular, width, height, actual));
            const int top = rasterizer.proportionalStripOverhangInPixels() + 1;
            for (int y = 0; y < height; ++y) {
                for (int x = 0; x < width; ++x) {
                    const std::uint8_t expectedPixel = x >= 2 && x < 2 + glyphWidth && y >= top && y < top + glyphHeight
                        ? expected[static_cast<std::size_t>(y - top) * glyphWidth + x - 2]
                        : 0;
                    CHECK(actual[static_cast<std::size_t>(y) * width + x] == expectedPixel);
                }
            }
        }
    }
}

TEST(dedicatedProportionalSheetCachesVisibleGridFallbacks)
{
    UnifontRasterizer rasterizer;
    ProportionalGlyphSheet sheet(8, 16, 0, 0, &rasterizer, 1024);
    CHECK(sheet.isUsable());
    const std::array<std::uint32_t, 2> text { 0x65E5, 0x0627 };
    std::array<ProportionalShapedGlyph, 2> shaped;
    CHECK(sheet.shapeRun(text, FontFace::Regular, shaped) == 2);
    for (std::size_t index = 0; index < shaped.size(); ++index) {
        CHECK(shaped[index].glyph != 0);
        CHECK(sheet.rasteriseGlyph(shaped[index].glyph, FontFace::Regular, 0, 0, static_cast<int>(index)));
    }
    CHECK(sheet.generation() == 2);
    CHECK(std::ranges::any_of(sheet.coverage(), [](std::uint8_t value) { return value != 0; }));
}

TEST(unavailableCodePointsRenderAReplacementAndFormatSelectorsStayInvisible)
{
    UnifontRasterizer rasterizer;
    const std::array<std::uint32_t, 2> text { 0x1F680, 0xFE0F };
    std::array<ProportionalShapedGlyph, 2> shaped;
    CHECK(rasterizer.shapeProportionalRun(text, FontFace::Regular, shaped) == 2);
    CHECK(shaped[0].glyph == 0xFFFD);
    CHECK(shaped[0].advanceInPixels > 0.0f);
    CHECK(shaped[1].glyph == 0xFE0F);
    CHECK(shaped[1].advanceInPixels == 0.0f);
    std::vector<std::uint8_t> coverage(32 * 20);
    CHECK(rasterizer.rasteriseProportionalRun(shaped, FontFace::Regular, 32, 20, coverage));
    CHECK(std::ranges::any_of(coverage, [](std::uint8_t value) { return value != 0; }));
}

UBIQUITTY_TEST_MAIN()
