// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Rendering/AtlasCoverage.h"
#include "Harness.h"

#include <array>

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

UBIQUITTY_TEST_MAIN()
