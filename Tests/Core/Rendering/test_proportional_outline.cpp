// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_proportional_outline.cpp — the bundled proportional face drawn from its
// outlines (Core/Rendering/ProportionalOutline.cpp): the shear an oblique is
// drawn with, measured on the ink rather than on golden pixels.

#include "Core/Rendering/ProportionalOutline.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace {

/// The mean column of a plane's ink across a band of rows.
double inkCentre(const std::vector<std::uint8_t>& plane, int width, int firstRow, int endRow)
{
    double weighted = 0.0;
    double total = 0.0;
    for (int y = firstRow; y < endRow; ++y) {
        for (int x = 0; x < width; ++x) {
            const double coverage = plane[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)];
            weighted += coverage * x;
            total += coverage;
        }
    }
    return total > 0.0 ? weighted / total : -1.0;
}

} // namespace

TEST(anObliqueLeansTheOutlineRightByItsHeight)
{
    // Noto Sans's `l` is one upright stem: drawn upright its ink centre is
    // the same at the top as at the foot; sheared, the top stands right of
    // the foot by the slant times the height between them.
    constexpr int size = 64;
    constexpr double baseline = 52.0;
    std::vector<std::uint8_t> upright(static_cast<std::size_t>(size * size), 0);
    std::vector<std::uint8_t> oblique(static_cast<std::size_t>(size * size), 0);
    using Ubiquitty::Rendering::ProportionalStrike::Face;
    REQUIRE(Ubiquitty::Rendering::ProportionalOutline::rasterise(Face::Regular, 'l', 48.0, 1.0, 16.0, baseline, size, size, upright));
    REQUIRE(Ubiquitty::Rendering::ProportionalOutline::rasterise(Face::Regular, 'l', 48.0, 1.0, 16.0, baseline, size, size, oblique, 0.2126));
    const double uprightLean = inkCentre(upright, size, 18, 24) - inkCentre(upright, size, 44, 50);
    const double obliqueLean = inkCentre(oblique, size, 18, 24) - inkCentre(oblique, size, 44, 50);
    CHECK(std::fabs(uprightLean) < 0.5);
    // Rows 21 and 47 stand 26 pixels apart: a lean of about 5.5 pixels.
    CHECK(obliqueLean > 4.5 && obliqueLean < 6.5);
}

UBIQUITTY_TEST_MAIN()
