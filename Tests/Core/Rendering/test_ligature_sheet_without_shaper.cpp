// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// A ligature sheet over a face that cannot shape is no sheet at all.
//
// The software renderer constructs its sheet member with a one-pixel cell and
// no rasterizer when the face has no shaper, as a placeholder. The sheet used
// to plan that cell like any other: a strip slot one pixel tall, and a byte
// budget's worth of them -- half a million slots, eight megabytes allocated
// and zeroed, per renderer, for rows that could never ligate. Under a debug
// standard library the reset alone was slow enough that `test_web_terminal`
// run whole read as hung once a few dozen cases had each built a renderer.

#include "Harness.h"

#include "Core/Rendering/GlyphRasterizer.h"
#include "Core/Rendering/LigatureSheet.h"

#include <cstdint>
#include <span>

using namespace Ubiquitty;
using namespace Ubiquitty::Rendering;

namespace {

constexpr int cellWidth = 8;
constexpr int cellHeight = 16;

/// A rasterizer whose one interesting property is whether it shapes.
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

    bool rasterizeGlyph(std::uint32_t, FontFace, std::uint32_t, int slotWidth, int slotHeight, std::uint8_t* coverageOut) override
    {
        for (int i = 0; i < slotWidth * slotHeight; ++i)
            coverageOut[i] = 0;
        return true;
    }

    [[nodiscard]] bool hasLigatureShaping() const override { return canShape; }

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
            coverageOut[i] = 255;
        return true;
    }
};

}

TEST(theShapingFaceGetsASheet)
{
    // The control: the same construction over a face that shapes plans and
    // allocates, so an empty sheet below is the shaper's absence and not a
    // fixture that never reaches the planner.
    ShapingRasterizer face;
    const LigatureSheet sheet(cellWidth, cellHeight, 1, 1, &face);
    CHECK_MSG(sheet.isUsable(), "a shaping face gets a usable sheet");
    CHECK_MSG(!sheet.coverage().empty(), "and the sheet has its pixels");
}

TEST(aFaceWithoutAShaperGetsNoSheet)
{
    ShapingRasterizer face;
    face.canShape = false;
    const LigatureSheet sheet(cellWidth, cellHeight, 1, 1, &face);
    CHECK_MSG(!sheet.isUsable(), "no shaper, no usable sheet");
    CHECK_MSG(sheet.coverage().empty(), "and nothing allocated for strips that cannot be shaped");
}

TEST(theRenderersPlaceholderSheetAllocatesNothing)
{
    // The exact construction the software renderer uses over a face with no
    // shaper: a one-pixel cell and no rasterizer.
    const LigatureSheet sheet(1, 1, 0, 0, nullptr);
    CHECK_MSG(!sheet.isUsable(), "the placeholder is unusable");
    CHECK_MSG(sheet.coverage().empty(), "and it holds no pixels: the byte budget is not spent on one-pixel slots");
}

UBIQUITTY_TEST_MAIN()
