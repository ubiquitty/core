// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_rasterizer_decorator_forwards — every entry on the raster seam reaches
// the inner rasterizer.
//
// `BoxDrawingRasterizer` intercepts one thing, box-drawing glyphs, and is a pass
// through for the rest. Its own header says a decorator inherits its inner's
// obligations and the ones it drops fail quietly, and a dropped one stays
// invisible for the reason that sentence gives.
//
// Every default on this seam is plausible OUTPUT rather than a failure, which is
// what makes a missing forward impossible to see from the screen:
//
//   hasColorGlyphs         reports no colour source, so the caller leaves its
//                          colour hooks null and every emoji draws as coverage
//                          in the pen colour — the right shape, wrong colours
//   rasterizeSizedMaster   nearest-neighbour-upscales the 1x raster, so a
//                          double-height row still draws, at doubled pixels
//                          instead of a real re-raster
//   rasterizeCluster       composites each code point at max coverage, which
//                          equals a shaper's output only where shaping does
//                          nothing
//
// So this file counts calls at the inner rather than comparing pixels. A test
// that rendered and looked would pass on all three defaults.

#include "Harness.h"

#include "Core/Rendering/BoxDrawingRasterizer.h"
#include "Core/Rendering/GlyphRasterizer.h"

#include <vector>

namespace {

using namespace Ubiquitty::Rendering;

/// An inner that records which entries reached it.
///
/// Every answer differs from the base default, so a dropped forward shows up as
/// both a zero count and a wrong answer.
struct CountingInner final : GlyphRasterizer {
    [[nodiscard]] CellMetrics cellMetrics() const override
    {
        CellMetrics metrics {};
        metrics.cellWidth = 9.0f;
        metrics.cellHeight = 17.0f;
        return metrics;
    }

    bool rasterizeGlyph(std::uint32_t, FontFace, std::uint32_t, int, int, std::uint8_t*) override
    {
        ++glyphCalls;
        return true;
    }

    [[nodiscard]] bool hasColorGlyphs() const override { return true; }

    bool rasterizeColorCluster(const std::uint32_t*, int, int, std::uint8_t*) override
    {
        ++colorClusterCalls;
        return true;
    }

    bool rasterizeCluster(const std::uint32_t*, int, FontFace, std::uint32_t, int, int, std::uint8_t*) override
    {
        ++clusterCalls;
        return true;
    }

    bool rasterizeSizedMaster(std::uint32_t, FontFace, std::uint32_t, const SizedMaster&, int, int, std::uint8_t*) override
    {
        ++scaledCalls;
        return true;
    }

    [[nodiscard]] WidthAxisRoom widthAxisRoom(FontFace) const override { return { 100.0, 125.0 }; }

    [[nodiscard]] double cellAdvanceAtWidthAxis(FontFace, double coordinate) override { return coordinate / 10.0; }

    int glyphCalls = 0;
    int colorClusterCalls = 0;
    int clusterCalls = 0;
    int scaledCalls = 0;
};

// A code point the decorator has no box-drawing opinion about, so the call is
// the pass-through case rather than the intercepted one.
constexpr std::uint32_t ordinaryCodePoint = 'A';

} // namespace

// The colour pair reaches the inner.
//
// Without the forward `hasColorGlyphs` answers false, the caller allocates no
// colour sheet, and `rasterizeColorCluster` is never called at all — so the
// count and the capability fail together, which is how it presented.
TEST(testColorEntriesReachTheInner)
{
    CountingInner inner;
    BoxDrawingRasterizer decorator(inner);

    CHECK(decorator.hasColorGlyphs());

    std::vector<std::uint8_t> pixels(4 * 4 * 4, 0);
    const std::uint32_t cluster[] = { 0x1F600 };
    CHECK(decorator.rasterizeColorCluster(cluster, 1, 4, pixels.data()));
    CHECK_EQ(inner.colorClusterCalls, 1);
}

// The scaled raster reaches the inner.
//
// The base default would upscale the 1x raster and return true, so the verdict
// and the pixels both look right — only the count separates a real re-raster at
// the scaled size from a doubled one.
TEST(testScaledRasterReachesTheInner)
{
    CountingInner inner;
    BoxDrawingRasterizer decorator(inner);

    std::vector<std::uint8_t> coverage(16 * 32, 0);
    CHECK(decorator.rasterizeGlyphScaled(ordinaryCodePoint, FontFace::Regular, 0u, 2, 16, 32, coverage.data()));
    CHECK_EQ(inner.scaledCalls, 1);
    // The default implementation reaches rasterizeGlyph instead, so a zero here
    // is what distinguishes the forward from the fallback.
    CHECK_EQ(inner.glyphCalls, 0);
}

// The width-axis facts reach the inner, so a doubled row on a decorated face
// widens on its axis rather than stretching by two.
TEST(testWidthAxisFactsReachTheInner)
{
    CountingInner inner;
    BoxDrawingRasterizer decorator(inner);

    const WidthAxisRoom room = decorator.widthAxisRoom(FontFace::Regular);
    CHECK_EQ(room.current, 100.0);
    CHECK_EQ(room.maximum, 125.0);
    CHECK_EQ(decorator.cellAdvanceAtWidthAxis(FontFace::Regular, 125.0), 12.5);
}

// The cluster raster reaches the inner.
TEST(testClusterRasterReachesTheInner)
{
    CountingInner inner;
    BoxDrawingRasterizer decorator(inner);

    std::vector<std::uint8_t> coverage(16 * 16, 0);
    const std::uint32_t cluster[] = { 'e', 0x0301 };
    CHECK(decorator.rasterizeCluster(cluster, 2, FontFace::Regular, 0u, 16, 16, coverage.data()));
    CHECK_EQ(inner.clusterCalls, 1);
    CHECK_EQ(inner.glyphCalls, 0);
}

// The metrics reach the inner, which is the case that was already forwarded.
//
// Kept because it is the control: if the inner were unreachable for some reason
// unrelated to the forwards, every assertion above would fail and this one would
// too, which says the fixture is wrong rather than the decorator.
TEST(testMetricsReachTheInner)
{
    CountingInner inner;
    BoxDrawingRasterizer decorator(inner);

    CHECK_EQ(static_cast<int>(decorator.cellMetrics().cellWidth), 9);
    CHECK_EQ(static_cast<int>(decorator.cellMetrics().cellHeight), 17);
}

UBIQUITTY_TEST_MAIN()
