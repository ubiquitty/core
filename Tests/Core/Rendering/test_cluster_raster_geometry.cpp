// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

#include "Core/Rendering/ClusterRasterGeometry.h"

#include <array>
#include <cmath>
#include <limits>

using namespace Ubiquitty::Rendering;

TEST(clusterFitEligibilityCoversIndicAndRelatedSyllabicScripts)
{
    constexpr std::array clusters {
        std::array<std::uint32_t, 2> { 0x915, 0x94d },
        std::array<std::uint32_t, 2> { 0x995, 0x9cd },
        std::array<std::uint32_t, 2> { 0xa15, 0xa4d },
        std::array<std::uint32_t, 2> { 0xa95, 0xacd },
        std::array<std::uint32_t, 2> { 0xb15, 0xb4d },
        std::array<std::uint32_t, 2> { 0xb95, 0xbcd },
        std::array<std::uint32_t, 2> { 0xc15, 0xc4d },
        std::array<std::uint32_t, 2> { 0xc95, 0xccd },
        std::array<std::uint32_t, 2> { 0xd15, 0xd4d },
        std::array<std::uint32_t, 2> { 0xd9a, 0xdca },
        std::array<std::uint32_t, 2> { 0xe01, 0xe48 },
        std::array<std::uint32_t, 2> { 0xe81, 0xec8 },
        std::array<std::uint32_t, 2> { 0xf40, 0xf71 },
        std::array<std::uint32_t, 2> { 0x1000, 0x1039 },
        std::array<std::uint32_t, 2> { 0x1780, 0x17d2 },
        std::array<std::uint32_t, 2> { 0x1b13, 0x1b44 },
        std::array<std::uint32_t, 2> { 0xa984, 0xa9c0 },
        std::array<std::uint32_t, 2> { 0x11005, 0x11046 },
        std::array<std::uint32_t, 2> { 0x11083, 0x110b9 },
        std::array<std::uint32_t, 2> { 0x11315, 0x1134d },
        std::array<std::uint32_t, 2> { 0x11415, 0x11442 },
        std::array<std::uint32_t, 2> { 0x11f12, 0x11f41 },
    };
    for (const auto& cluster : clusters) {
        CHECK(clusterHorizontalFitEligible(cluster));
        CHECK(clusterHorizontalFitEligible(cluster.data(), static_cast<int>(cluster.size())));
    }
}

TEST(clusterFitEligibilityPreservesOtherScriptsAndPictographs)
{
    constexpr std::array clusters {
        std::array<std::uint32_t, 2> { 'A', 0x301 },
        std::array<std::uint32_t, 2> { 0x250, 0x35d },
        std::array<std::uint32_t, 2> { 0x3b1, 0x301 },
        std::array<std::uint32_t, 2> { 0x436, 0x301 },
        std::array<std::uint32_t, 2> { 0x628, 0x64e },
        std::array<std::uint32_t, 2> { 0x5d0, 0x5b0 },
        std::array<std::uint32_t, 2> { 0x531, 0x301 },
        std::array<std::uint32_t, 2> { 0x10d0, 0x301 },
        std::array<std::uint32_t, 2> { 0x1200, 0x301 },
        std::array<std::uint32_t, 2> { 0x6f22, 0x301 },
        std::array<std::uint32_t, 2> { 0x1100, 0x1161 },
        std::array<std::uint32_t, 2> { 0x2764, 0x301 },
        std::array<std::uint32_t, 2> { 0x1f469, 0x1f3fd },
        std::array<std::uint32_t, 2> { 0x915, 0x6f22 },
        std::array<std::uint32_t, 2> { 0x915, 0x1f600 },
    };
    for (const auto& cluster : clusters)
        CHECK(!clusterHorizontalFitEligible(cluster));
}

TEST(clusterFitEligibilityUsesTheBaseAfterPrepend)
{
    constexpr std::array indic { 0x600u, 0xd4eu, 0x915u, 0x94du };
    constexpr std::array greek { 0xd4eu, 0x3b1u, 0x301u };
    constexpr std::array prefixOnly { 0x600u, 0xd4eu };
    constexpr std::array prefixAndBase { 0x600u, 0x915u };
    CHECK(clusterHorizontalFitEligible(indic));
    CHECK(!clusterHorizontalFitEligible(greek));
    CHECK(!clusterHorizontalFitEligible(prefixOnly));
    CHECK(!clusterHorizontalFitEligible(prefixAndBase));
}

TEST(clusterFitEligibilityRequiresVisibleMarksAndAValidNarrowBase)
{
    constexpr std::array base { 0x915u };
    constexpr std::array selectors { 0x915u, 0x200du, 0xfe0fu };
    constexpr std::array marked { 0x915u, 0x94du, 0x200du, 0xfe0fu };
    constexpr std::array zeroWidthBase { 0x94du, 0x915u };
    CHECK(!clusterHorizontalFitEligible(std::span<const std::uint32_t> {}));
    CHECK(!clusterHorizontalFitEligible(base));
    CHECK(!clusterHorizontalFitEligible(selectors));
    CHECK(clusterHorizontalFitEligible(marked));
    CHECK(!clusterHorizontalFitEligible(zeroWidthBase));
    CHECK(!clusterHorizontalFitEligible(nullptr, 0));
    CHECK(!clusterHorizontalFitEligible(nullptr, 2));
    CHECK(!clusterHorizontalFitEligible(base.data(), -1));
}

TEST(clusterFitPreservesContainedGlyphPositions)
{
    const auto fit = fitClusterHorizontal(4.25, 11.5, { 4, 8 });
    CHECK_EQ(fit.scaleX, 1);
    CHECK_EQ(fit.translationX, 0);
}

TEST(clusterFitMovesBearingsWithoutEnlargingOrChangingSpacing)
{
    const auto left = fitClusterHorizontal(-2, 4, { 1, 8 });
    CHECK_EQ(left.scaleX, 1);
    CHECK_EQ(left.translationX, 3);
    const auto right = fitClusterHorizontal(9, 15, { 1, 8 });
    CHECK_EQ(right.scaleX, 1);
    CHECK_EQ(right.translationX, -6);
}

TEST(clusterFitContractsTheCompleteGroupIntoItsCellInterval)
{
    const auto fit = fitClusterHorizontal(-2, 18, { 3, 8 });
    CHECK(std::abs(fit.scaleX - 0.4) < 1e-12);
    const auto placed = [&](double source) { return source * fit.scaleX + fit.translationX; };
    CHECK(std::abs(placed(-2) - 3) < 1e-12);
    CHECK(std::abs(placed(18) - 11) < 1e-12);
    CHECK(std::abs((placed(8) - placed(3)) - 2) < 1e-12);
    const auto wide = fitClusterHorizontal(-2, 18, { 3, 16 });
    CHECK(std::abs(wide.scaleX - 0.8) < 1e-12);
}

TEST(clusterFitRejectsInvalidMetricsAndHandlesEmptyInk)
{
    const double infinity = std::numeric_limits<double>::infinity();
    const double maximum = std::numeric_limits<double>::max();
    for (const auto fit : { fitClusterHorizontal(infinity, 8, { 0, 8 }), fitClusterHorizontal(0, infinity, { 0, 8 }),
             fitClusterHorizontal(8, 0, { 0, 8 }), fitClusterHorizontal(0, 8, { 0, 0 }),
             fitClusterHorizontal(0, 8, { infinity, 8 }), fitClusterHorizontal(maximum, maximum, { -maximum, 1 }) }) {
        CHECK_EQ(fit.scaleX, 1);
        CHECK_EQ(fit.translationX, 0);
    }
    const auto empty = fitClusterHorizontal(20, 20, { 0, 8 });
    CHECK_EQ(empty.scaleX, 1);
    CHECK_EQ(empty.translationX, -12);
}

UBIQUITTY_TEST_MAIN()
