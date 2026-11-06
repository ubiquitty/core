// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_rasterizer_interleaving — a raster of one glyph does not depend on what
// was rasterised before it.
//
// The seam hands every backend one `GlyphRasterizer&`, and a backend calls it in
// whatever order its rows fault. Nothing in the contract says an implementation
// may not keep state between calls, and one that does turns "which glyph" into
// "which glyph, after which other glyph" — a property no per-glyph correctness
// case can see, because each of those rasterises one glyph into a fresh buffer
// and asks whether the pixels are right.
//
// The failure this exists for was measured on the web seam's font
// rasterizer: the colour path set `textAlign`
// on a shared 2D context and the mono path never reset it, so every glyph
// rasterised after any emoji was drawn half an advance to the left. It reached
// the ATLAS, so it stayed wrong for as long as the slot did, and it surfaced
// three layers downstream as a parity disagreement whose sign was inverted by
// which arm rasterised first.
//
// The property is one sentence and it holds for every implementation: rasterise
// A, rasterise B, rasterise A again, and the two A rasters agree byte for byte.

#include "Core/Rendering/GlyphRasterizer.h"
#include "Core/Rendering/UnifontRasterizer.h"

#include <cstdint>
#include <vector>

namespace {

using Ubiquitty::Rendering::FontFace;
using Ubiquitty::Rendering::GlyphRasterizer;

constexpr int slotWidth = 16;
constexpr int slotHeight = 16;
constexpr std::uint32_t regularWeight = 400;

/// One glyph's coverage, sized for the slot both cases use.
std::vector<std::uint8_t> rasterOf(GlyphRasterizer& rasterizer, std::uint32_t codePoint)
{
    std::vector<std::uint8_t> coverage(static_cast<std::size_t>(slotWidth) * slotHeight, 0);
    rasterizer.rasterizeGlyph(codePoint, FontFace::Regular, regularWeight,
        slotWidth, slotHeight, coverage.data());
    return coverage;
}

/// Rasterises `first`, then `between`, then `first` again.
///
/// - Returns: True when the two rasters of `first` agree byte for byte.
bool survivesInterleaving(GlyphRasterizer& rasterizer, std::uint32_t first, std::uint32_t between)
{
    const std::vector<std::uint8_t> before = rasterOf(rasterizer, first);
    (void)rasterOf(rasterizer, between);
    const std::vector<std::uint8_t> after = rasterOf(rasterizer, first);
    return before == after;
}

/// A rasterizer carrying the defect the case exists to catch, so the case has a
/// control: one call path leaves a setting behind and the other never clears it.
///
/// Without this, a green run would mean either that the property holds or that
/// nothing here could ever break it, and those read the same.
struct LeakyRasterizer final : GlyphRasterizer {
    [[nodiscard]] Ubiquitty::Rendering::CellMetrics cellMetrics() const override
    {
        return Ubiquitty::Rendering::CellMetrics { .cellWidth = slotWidth, .cellHeight = slotHeight };
    }

    bool rasterizeGlyph(std::uint32_t codePoint, FontFace, std::uint32_t, int width, int height,
        std::uint8_t* coverageOut) override
    {
        // The leak: a wide code point sets the shift, and no later call resets
        // it. Mirrors a context setting applied by one path and assumed clear by
        // the other.
        if (codePoint > 0x2000)
            m_shift = 3;
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                const int source = x + m_shift;
                coverageOut[y * width + x] = (source < width && ((source + y) & 1))
                    ? static_cast<std::uint8_t>(codePoint & 0xFF)
                    : 0;
            }
        }
        return true;
    }

private:
    int m_shift = 0;
};

} // namespace

TEST(aRasterizerWithLeakedStateFailsTheInterleavingProperty)
{
    // The control, first: it establishes that the property below is one this
    // file is able to break, and it fails for the reason the web seam did.
    LeakyRasterizer leaky;
    CHECK(!survivesInterleaving(leaky, 'a', 0x1F600));

    // And a fresh one agrees with itself when the interleaved glyph sets
    // nothing, so the case above fails on the interleaving rather than on a
    // rasterizer that never repeats itself.
    LeakyRasterizer quiet;
    CHECK(survivesInterleaving(quiet, 'a', 'b'));
}

TEST(unifontRasterOfAGlyphDoesNotDependOnWhatCameBefore)
{
    Ubiquitty::Rendering::UnifontRasterizer unifont;

    // A wide code point between two rasters of a narrow one, which is the shape
    // that broke the web seam: the wide glyph is what a colour or fallback path
    // takes, and the narrow one is what every later cell samples.
    CHECK(survivesInterleaving(unifont, 'a', 0x65E5));
    CHECK(survivesInterleaving(unifont, 'W', 0x1F600));

    // And the reverse order, because a path taken first and a path taken second
    // leave different state behind.
    CHECK(survivesInterleaving(unifont, 0x65E5, 'a'));
}

TEST(unifontRasterSurvivesAClusterBetweenTwoGlyphRasters)
{
    Ubiquitty::Rendering::UnifontRasterizer unifont;

    const std::vector<std::uint8_t> before = rasterOf(unifont, 'a');

    const std::uint32_t cluster[] = { 'e', 0x0301 }; // e + combining acute
    std::vector<std::uint8_t> clusterCoverage(
        static_cast<std::size_t>(slotWidth) * slotHeight, 0);
    unifont.rasterizeCluster(cluster, 2, FontFace::Regular, regularWeight,
        slotWidth, slotHeight, clusterCoverage.data());

    const std::vector<std::uint8_t> after = rasterOf(unifont, 'a');
    CHECK(before == after);
}

UBIQUITTY_TEST_MAIN()
