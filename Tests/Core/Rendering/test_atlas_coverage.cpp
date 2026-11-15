// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Rendering/AtlasCoverage.h"
#include "Harness.h"

#include <array>
#include <cmath>
#include <limits>

TEST(atlasCoverageWritesOnlyTheRequestedRectangle)
{
    std::array<std::uint8_t, 7 * 5> atlas;
    atlas.fill(0xA5);
    const std::array<std::uint8_t, 6> glyph { 0, 32, 255, 64, 128, 192 };
    const Ubiquitty::Rendering::AtlasSlotRegion region { 2, 1, 3, 2 };
    Ubiquitty::Rendering::writeAtlasCoverage(atlas, 7, region, glyph);
    for (int row = 0; row < 5; ++row) {
        for (int column = 0; column < 7; ++column) {
            const bool isInside = row >= 1 && row < 3 && column >= 2 && column < 5;
            const auto expected = isInside ? glyph[(row - 1) * 3 + column - 2] : 0xA5;
            CHECK_EQ(atlas[row * 7 + column], expected);
        }
    }
    Ubiquitty::Rendering::writeAtlasCoverage(atlas, 7, region, {});
    for (int row = 0; row < 5; ++row) {
        for (int column = 0; column < 7; ++column) {
            const bool isInside = row >= 1 && row < 3 && column >= 2 && column < 5;
            CHECK_EQ(atlas[row * 7 + column], isInside ? 0 : 0xA5);
        }
    }
}

TEST(nativeCoverageInterpolatesTexelCentresWithTransparentSheetBorders)
{
    using namespace Ubiquitty::Rendering;
    // The expected values are independent bilinear weights, not samples from
    // the implementation. A nonbinary corner also pins byte normalization.
    constexpr std::array<std::uint8_t, 4> coverage { 0, 255, 128, 64 };
    const auto sample = [&](float x, float y) {
        return sampleAtlasCoverage(coverage, 2, 2, x, y, CoverageSampling::Native);
    };
    CHECK_EQ(sample(0.5f, 0.5f), 0.0f);
    CHECK_EQ(sample(1.5f, 0.5f), 1.0f);
    CHECK(std::fabs(sample(1.0f, 1.0f) - 447.0f / 1020.0f) < 0.000001f);
    CHECK(std::fabs(sample(1.25f, 0.75f) - (255.0f * 0.5625f + 128.0f * 0.0625f + 64.0f * 0.1875f) / 255.0f) < 0.000001f);
    // Metal's clamp-to-zero border participates in interpolation. Clamping
    // to the last texel instead would incorrectly retain full coverage.
    CHECK_EQ(sample(2.0f, 0.5f), 0.5f);
    CHECK_EQ(sample(2.5f, 0.5f), 0.0f);
    CHECK_EQ(sample(1.5f, 0.0f), 0.5f);
    CHECK_EQ(sample(1.5f, -0.5f), 0.0f);
    CHECK_EQ(sample(1.5f, -1000000.0f), 0.0f);
}

TEST(bitmapCoverageKeepsContainingTexelsAndClampedEdges)
{
    using namespace Ubiquitty::Rendering;
    constexpr std::array<std::uint8_t, 4> coverage { 0, 255, 128, 64 };
    const auto sample = [&](float x, float y) {
        return sampleAtlasCoverage(coverage, 2, 2, x, y, CoverageSampling::Bitmap);
    };
    CHECK_EQ(sample(0.999f, 0.5f), 0.0f);
    CHECK_EQ(sample(1.0f, 0.5f), 1.0f);
    CHECK_EQ(sample(2.0f, 0.5f), 1.0f);
    CHECK_EQ(sample(1000000.0f, -1000000.0f), 1.0f);
    CHECK_EQ(sample(0.5f, 1.25f), 128.0f / 255.0f);
    CHECK_EQ(sample(1.25f, 1.25f), 64.0f / 255.0f);
}

TEST(coverageSamplingRejectsInvalidPlanesAndNonfiniteCoordinates)
{
    using namespace Ubiquitty::Rendering;
    constexpr std::array<std::uint8_t, 1> coverage { 255 };
    for (auto sampling : { CoverageSampling::Bitmap, CoverageSampling::Native }) {
        CHECK_EQ(sampleAtlasCoverage(coverage, 0, 1, 0.5f, 0.5f, sampling), 0.0f);
        CHECK_EQ(sampleAtlasCoverage(coverage, 2, 1, 0.5f, 0.5f, sampling), 0.0f);
        CHECK_EQ(sampleAtlasCoverage(coverage, 1, 1, std::numeric_limits<float>::infinity(), 0.5f, sampling), 0.0f);
        CHECK_EQ(sampleAtlasCoverage(coverage, 1, 1, 0.5f, std::numeric_limits<float>::quiet_NaN(), sampling), 0.0f);
    }
}

UBIQUITTY_TEST_MAIN()
