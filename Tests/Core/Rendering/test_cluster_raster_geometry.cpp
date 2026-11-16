// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

#include "Core/Rendering/ClusterRasterGeometry.h"

#include <cmath>
#include <limits>

using namespace Ubiquitty::Rendering;

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
