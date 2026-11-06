// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// The shade glyphs ░ ▒ ▓ and the `[appearance] shade_blocks` choice.
//
// Core decides which pixels a shade lights: the quads `emitBoxFill` writes and
// the field `isShadePixelLit` defines. These cases hold that geometry to the
// one property the reader sees — a run of shades is one continuous field with
// no seam at any cell edge, at any cell size and grid origin — and drive the
// software renderer end to end so the composite is held to it too.

#include "Core/Cell.h"
#include "Core/Rendering/BoxFillEmit.h"
#include "Core/Rendering/ShadeStyle.h"
#include "Core/Rendering/SoftwareRenderer.h"
#include "Core/TermPalette.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace Ubiquitty;
using namespace Ubiquitty::Rendering;

namespace {

constexpr std::array<std::uint32_t, 3> shadeCodePoints { 0x2591, 0x2592, 0x2593 };
constexpr std::array<int, 3> shadeLit { 1, 2, 3 };

// The ink a run of quads lays on a plane, by the rule every frontend follows:
// a pattern quad lights Core's field at full ink, any other quad covers its
// rect at its alpha. -1 marks a pixel no quad reached, and a pixel two quads
// reached is reported, since either is a seam.
struct InkPlane {
    int width;
    int height;
    std::vector<int> ink;
    bool hasOverlap = false;
};

void stamp(InkPlane& plane, const OverlayInstance& quad)
{
    const int x0 = static_cast<int>(quad.topLeftInPixels.x);
    const int y0 = static_cast<int>(quad.topLeftInPixels.y);
    const int x1 = static_cast<int>(quad.bottomRightInPixels.x);
    const int y1 = static_cast<int>(quad.bottomRightInPixels.y);
    const int lit = quad.shadeDensity > 0.0f ? shadeLitCount(quad.shadeDensity) : 0;
    const int phase = static_cast<int>(quad.plA.x + 0.5f);
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            if (x < 0 || y < 0 || x >= plane.width || y >= plane.height)
                continue;
            int& pixel = plane.ink[static_cast<std::size_t>(y) * plane.width + x];
            if (pixel >= 0)
                plane.hasOverlap = true;
            if (lit > 0)
                pixel = isShadePixelLit(x, y, lit, phase) ? 255 : 0;
            else
                pixel = static_cast<int>(quad.color.w * 255.0f + 0.5f);
        }
    }
}

// Emits a columns x rows run of one code point the way CellInstanceBuilder
// does: integer-snapped cell rects off a possibly fractional pitch, one phase
// for the whole grid.
InkPlane emitRun(std::uint32_t codePoint, ShadeStyle style, float cellWidth, float cellHeight, int originX, int originY, int columns, int rows)
{
    InkPlane plane;
    plane.width = originX + static_cast<int>(std::ceil(cellWidth * columns)) + 1;
    plane.height = originY + static_cast<int>(std::ceil(cellHeight * rows)) + 1;
    plane.ink.assign(static_cast<std::size_t>(plane.width) * plane.height, -1);
    const ShadeField field { style, shadePhase(originX, originY) };
    for (int row = 0; row < rows; ++row) {
        for (int column = 0; column < columns; ++column) {
            const float left = std::round(originX + column * cellWidth);
            const float right = std::round(originX + (column + 1) * cellWidth);
            const float top = std::round(originY + row * cellHeight);
            const float bottom = std::round(originY + (row + 1) * cellHeight);
            std::array<OverlayInstance, 8> quads {};
            const int count = emitBoxFill(codePoint, left, top, right, bottom, { 1.0f, 1.0f, 1.0f, 1.0f },
                quads.data(), static_cast<int>(quads.size()), nullptr, nullptr, 0.0f, nullptr, std::nullopt, field);
            for (int index = 0; index < count && index < static_cast<int>(quads.size()); ++index)
                stamp(plane, quads[static_cast<std::size_t>(index)]);
        }
    }
    return plane;
}

} // namespace

// UbiquittyKit's `public enum ShadeStyle` publishes these raw values; a reorder
// or an added case here must move it as well.
TEST(testEachShadeStyleKeepsTheOrdinalSwiftPublishes)
{
    CHECK_MSG(static_cast<unsigned>(ShadeStyle::Pattern) == 0u, "UbiquittyKit's `public enum ShadeStyle` declares `case pattern = 0`");
    CHECK_MSG(static_cast<unsigned>(ShadeStyle::Fill) == 1u, "UbiquittyKit's `public enum ShadeStyle` declares `case fill = 1`");
    CHECK_MSG(static_cast<unsigned>(ShadeStyle::Fill) + 1 == 2u,
        "a ShadeStyle enumerator was added — add its case to UbiquittyKit's `public enum ShadeStyle` and its `core`/`from` arms");
}

TEST(testShadeTokensRoundTrip)
{
    CHECK(shadeStyleFromString("pattern") == ShadeStyle::Pattern);
    CHECK(shadeStyleFromString("fill") == ShadeStyle::Fill);
    CHECK(shadeStyleFromString("dither") == defaultShadeStyle);
    CHECK(defaultShadeStyle == ShadeStyle::Pattern);
    CHECK(shadeStyleToString(ShadeStyle::Pattern) == "pattern");
    CHECK(shadeStyleToString(ShadeStyle::Fill) == "fill");
}

// Every row of the field carries ink at every density, the medium and dark
// shades ink every column, and the light shade never leaves two adjacent
// columns empty — the pattern has texture but no stripe.
TEST(testTheShadeFieldInksEveryRowAndNeverStripes)
{
    for (int phase = 0; phase < 4; ++phase) {
        for (std::size_t shade = 0; shade < shadeLit.size(); ++shade) {
            const int lit = shadeLit[shade];
            for (int y = 0; y < 8; ++y) {
                bool isRowInked = false;
                for (int x = 0; x < 8; ++x)
                    isRowInked |= isShadePixelLit(x, y, lit, phase);
                CHECK(isRowInked);
            }
            for (int x = 0; x < 8; ++x) {
                const bool isColumnInked = isShadePixelLit(x, 0, lit, phase) || isShadePixelLit(x, 1, lit, phase);
                const bool isNextInked = isShadePixelLit(x + 1, 0, lit, phase) || isShadePixelLit(x + 1, 1, lit, phase);
                if (lit >= 2)
                    CHECK(isColumnInked);
                else
                    CHECK(isColumnInked || isNextInked);
            }
        }
    }
}

// The seam property itself. A run is one field anchored at the grid origin:
// every pixel is reached by exactly one quad, and its ink is the field's at
// that pixel, whatever the cell's width, height or fractional pitch — not
// only where the cell happens to be a multiple of the field's period.
TEST(testARunOfPatternShadesIsOneFieldAtEveryCellSize)
{
    int failures = 0;
    for (float cellWidth = 5.0f; cellWidth <= 19.0f; cellWidth += 0.75f) {
        for (float cellHeight = 9.0f; cellHeight <= 23.0f; cellHeight += 1.0f) {
            for (int originX = 0; originX < 4; ++originX) {
                for (int originY = 0; originY < 2; ++originY) {
                    for (std::size_t shade = 0; shade < shadeCodePoints.size(); ++shade) {
                        const InkPlane plane = emitRun(shadeCodePoints[shade], ShadeStyle::Pattern, cellWidth, cellHeight, originX, originY, 4, 2);
                        const int phase = shadePhase(originX, originY);
                        const int right = static_cast<int>(std::round(originX + 4 * cellWidth));
                        const int bottom = static_cast<int>(std::round(originY + 2 * cellHeight));
                        bool isField = !plane.hasOverlap;
                        for (int y = originY; y < bottom && isField; ++y) {
                            for (int x = originX; x < right && isField; ++x) {
                                const int expected = isShadePixelLit(x - originX, y - originY, shadeLit[shade], 0) ? 255 : 0;
                                const int actual = plane.ink[static_cast<std::size_t>(y) * plane.width + x];
                                isField = actual == expected && isShadePixelLit(x, y, shadeLit[shade], phase) == (expected == 255);
                            }
                        }
                        if (!isField && failures++ < 4) {
                            std::printf("  seam: U+%04X cell %.2fx%.0f origin (%d,%d)\n", static_cast<unsigned>(shadeCodePoints[shade]),
                                static_cast<double>(cellWidth), static_cast<double>(cellHeight), originX, originY);
                        }
                    }
                }
            }
        }
    }
    CHECK_EQ(failures, 0);
}

// The fill reaches every pixel of the run once, at the shade's density.
TEST(testARunOfFillShadesIsOneUniformTint)
{
    int failures = 0;
    for (float cellWidth = 5.0f; cellWidth <= 13.0f; cellWidth += 0.5f) {
        for (std::size_t shade = 0; shade < shadeCodePoints.size(); ++shade) {
            const InkPlane plane = emitRun(shadeCodePoints[shade], ShadeStyle::Fill, cellWidth, 17.0f, 1, 1, 4, 2);
            const int expected = static_cast<int>(static_cast<float>(shadeLit[shade]) * 0.25f * 255.0f + 0.5f);
            const int right = static_cast<int>(std::round(1 + 4 * cellWidth));
            bool isUniform = !plane.hasOverlap;
            for (int y = 1; y < 1 + 34 && isUniform; ++y)
                for (int x = 1; x < right && isUniform; ++x)
                    isUniform = plane.ink[static_cast<std::size_t>(y) * plane.width + x] == expected;
            failures += isUniform ? 0 : 1;
        }
    }
    CHECK_EQ(failures, 0);
}

// A fill emits no dither quad at all, so no frontend can draw a fill as a
// pattern or the reverse: the choice is made before a quad exists.
TEST(testTheStyleDecidesTheQuadKind)
{
    std::array<OverlayInstance, 4> quads {};
    REQUIRE(emitBoxFill(0x2592, 0, 0, 8, 16, { 1, 1, 1, 1 }, quads.data(), 4, nullptr, nullptr, 0, nullptr, std::nullopt, { ShadeStyle::Pattern, 3 }) == 1);
    CHECK(quads[0].shadeDensity == 0.5f);
    CHECK(quads[0].plA.x == 3.0f);
    CHECK(quads[0].color.w == 1.0f);
    REQUIRE(emitBoxFill(0x2592, 0, 0, 8, 16, { 1, 1, 1, 1 }, quads.data(), 4, nullptr, nullptr, 0, nullptr, std::nullopt, { ShadeStyle::Fill, 3 }) == 1);
    CHECK(quads[0].shadeDensity == 0.0f);
    CHECK(quads[0].color.w == 0.5f);
    // The legacy medium-shade half takes the same choice.
    REQUIRE(emitBoxFill(0x1FB8C, 0, 0, 8, 16, { 1, 1, 1, 1 }, quads.data(), 4, nullptr, nullptr, 0, nullptr, std::nullopt, { ShadeStyle::Fill, 0 }) == 1);
    CHECK(quads[0].shadeDensity == 0.0f);
    CHECK(quads[0].color.w == 0.5f);
}

// U+1FB90 INVERSE MEDIUM SHADE lights exactly the pixels ▒ leaves dark, at
// every cell size and grid phase, so the two set side by side or stacked fill
// the field between them. The half-cell legacy forms take the same inverse.
TEST(testTheInverseMediumShadeLightsTheComplementOfTheMediumShade)
{
    int failures = 0;
    for (float cellWidth = 5.0f; cellWidth <= 13.0f; cellWidth += 0.75f) {
        for (int originX = 0; originX < 4; ++originX) {
            for (int originY = 0; originY < 2; ++originY) {
                const InkPlane medium = emitRun(0x2592, ShadeStyle::Pattern, cellWidth, 15.0f, originX, originY, 3, 2);
                const InkPlane inverse = emitRun(0x1FB90, ShadeStyle::Pattern, cellWidth, 15.0f, originX, originY, 3, 2);
                const int right = static_cast<int>(std::round(originX + 3 * cellWidth));
                bool isComplement = !medium.hasOverlap && !inverse.hasOverlap;
                for (int y = originY; y < originY + 30 && isComplement; ++y) {
                    for (int x = originX; x < right && isComplement; ++x) {
                        const std::size_t index = static_cast<std::size_t>(y) * medium.width + x;
                        isComplement = medium.ink[index] + inverse.ink[index] == 255;
                    }
                }
                failures += isComplement ? 0 : 1;
            }
        }
    }
    CHECK_EQ(failures, 0);

    std::array<OverlayInstance, 4> quads {};
    for (const std::uint32_t codePoint : { 0x1FB90u, 0x1FB91u, 0x1FB92u, 0x1FB94u }) {
        const int count = emitBoxFill(codePoint, 0, 0, 8, 16, { 1, 1, 1, 1 }, quads.data(), 4, nullptr, nullptr, 0, nullptr, std::nullopt, { ShadeStyle::Pattern, 1 });
        int shadeQuads = 0;
        for (int index = 0; index < count; ++index) {
            if (quads[static_cast<std::size_t>(index)].shadeDensity > 0.0f) {
                ++shadeQuads;
                CHECK(quads[static_cast<std::size_t>(index)].plA.x == static_cast<float>((1 + inverseShadePhaseOffset) & 3));
            }
        }
        CHECK_EQ(shadeQuads, 1);
    }
    // The plain medium-shade halves keep the field's own phase.
    REQUIRE(emitBoxFill(0x1FB8C, 0, 0, 8, 16, { 1, 1, 1, 1 }, quads.data(), 4, nullptr, nullptr, 0, nullptr, std::nullopt, { ShadeStyle::Pattern, 1 }) == 1);
    CHECK(quads[0].plA.x == 1.0f);
}

// End to end through the software renderer at a cell 7 pixels wide — not a
// multiple of the field's period — and an odd grid origin: inside each run no
// column of the medium and dark shades is background, the light shade never
// leaves two, and the lit pixels are the grid-anchored field.
TEST(testTheSoftwareRendererDrawsShadeRunsWithoutASeam)
{
    TermPalette palette;
    palette.seedDefaults();
    constexpr int columns = 6;
    constexpr int rows = 2;
    for (const ShadeStyle style : { ShadeStyle::Pattern, ShadeStyle::Fill }) {
        for (std::size_t shade = 0; shade < shadeCodePoints.size(); ++shade) {
            std::vector<Cell> grid(columns * rows, defaultBlankCell);
            for (Cell& cell : grid) {
                cell.codePoint = shadeCodePoints[shade];
                cell.pen.foregroundIndex = 15;
                cell.pen.backgroundIndex = 0;
                cell.pen.underlineIndex = colorDefault;
            }
            BuildInputs input;
            input.cells = grid.data();
            input.rows = rows;
            input.columns = columns;
            input.palette = &palette;
            input.shadeStyle = style;
            input.metrics.cellWidth = 7.0f;
            input.metrics.cellHeight = 15.0f;
            input.originInPixels = { 3.0f, 1.0f };
            input.viewportSizeInPixels = { 48.0f, 32.0f };
            SoftwareRenderer renderer(7, 15);
            std::vector<std::uint8_t> pixels;
            int width = 0;
            int height = 0;
            REQUIRE(renderer.render(input, pixels, width, height));
            const auto red = [&](int x, int y) { return static_cast<int>(pixels[(static_cast<std::size_t>(y) * width + x) * 4]); };
            const int phase = shadePhase(3, 1);
            int emptyRun = 0;
            int widestEmptyRun = 0;
            bool isField = true;
            for (int x = 3; x < 3 + columns * 7; ++x) {
                bool isColumnInked = false;
                for (int y = 1; y < 1 + rows * 15; ++y) {
                    const int value = red(x, y);
                    isColumnInked |= value > 0;
                    if (style == ShadeStyle::Pattern)
                        isField &= (value == 255) == isShadePixelLit(x, y, shadeLit[shade], phase) && (value == 0 || value == 255);
                    else
                        isField &= value == red(3, 1) && value > 0 && value < 255;
                }
                emptyRun = isColumnInked ? 0 : emptyRun + 1;
                widestEmptyRun = std::max(widestEmptyRun, emptyRun);
            }
            CHECK(isField);
            CHECK(widestEmptyRun <= (style == ShadeStyle::Pattern && shadeLit[shade] == 1 ? 1 : 0));
        }
    }
}

UBIQUITTY_TEST_MAIN()
