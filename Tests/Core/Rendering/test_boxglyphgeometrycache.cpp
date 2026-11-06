// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Rendering/BoxDrawing.h"
#include "Harness.h"

#include <array>
#include <cstddef>
#include <cstring>
#include <initializer_list>

using Ubiquitty::Rendering::BoxDrawing::Glyph;
using Ubiquitty::Rendering::BoxDrawing::GlyphGeometryCache;

TEST(a_cached_decompose_is_byte_identical_to_a_fresh_one)
{
    GlyphGeometryCache cache;
    for (unsigned codePoint : std::initializer_list<unsigned> { 0x2500, 0x2502, 0x253C, 0x2588, 0x2591, 0x2571 }) {
        Glyph fresh {};
        Ubiquitty::Rendering::BoxDrawing::decompose(codePoint, &fresh);

        Glyph first {}, second {};
        const int firstCount = cache.decomposeCached(codePoint, &first);
        const int secondCount = cache.decomposeCached(codePoint, &second); // now a hit
        CHECK_EQ(firstCount, fresh.count);
        CHECK_EQ(secondCount, fresh.count);
        CHECK(std::memcmp(&first, &fresh, sizeof(Glyph)) == 0);
        CHECK(std::memcmp(&second, &fresh, sizeof(Glyph)) == 0);
    }
}

TEST(a_codepoint_with_no_geometry_reports_zero_and_stays_zero)
{
    GlyphGeometryCache cache;
    Glyph out {};
    CHECK_EQ(cache.decomposeCached('A', &out), 0);
    CHECK_EQ(cache.decomposeCached('A', &out), 0);
}

// The families a screen draws together, each a whole 256-wide block: box
// drawing and block elements, braille, legacy computing, the octants and the
// Powerline separators. Warmed once, a second pass over all of them must
// decompose nothing — a family evicting another's glyphs shows up as a
// recompute on that pass.
TEST(the_families_drawn_together_never_evict_one_another)
{
    const std::array<unsigned, 5> blocks { 0x2500, 0x2800, 0x1FB00, 0x1CD00, 0xE000 };
    GlyphGeometryCache cache;
    for (unsigned base : blocks) {
        for (unsigned codePoint = base; codePoint < base + 0x100; ++codePoint) {
            Glyph cached {};
            cache.decomposeCached(codePoint, &cached);
        }
    }
    const std::size_t warmed = cache.decompositions();
    CHECK_EQ(warmed, static_cast<std::size_t>(blocks.size() * 0x100));
    for (unsigned base : blocks) {
        for (unsigned codePoint = base; codePoint < base + 0x100; ++codePoint) {
            Glyph fresh {}, cached {};
            Ubiquitty::Rendering::BoxDrawing::decompose(codePoint, &fresh);
            cache.decomposeCached(codePoint, &cached);
            CHECK(std::memcmp(&cached, &fresh, sizeof(Glyph)) == 0);
        }
    }
    CHECK_EQ(cache.decompositions(), warmed);
}

// Two code points on the shared page with one low byte take turns in the
// slot: each lookup after the other's is a recompute, and every answer is
// still the code point's own glyph.
TEST(a_shared_page_collision_returns_the_right_glyph)
{
    GlyphGeometryCache cache;
    for (int round = 0; round < 3; ++round) {
        for (unsigned codePoint : std::initializer_list<unsigned> { 0x23BA, 0x24BA }) {
            Glyph fresh {}, cached {};
            Ubiquitty::Rendering::BoxDrawing::decompose(codePoint, &fresh);
            CHECK_EQ(cache.decomposeCached(codePoint, &cached), fresh.count);
            CHECK(std::memcmp(&cached, &fresh, sizeof(Glyph)) == 0);
        }
    }
    CHECK_EQ(cache.decompositions(), static_cast<std::size_t>(6));
}

UBIQUITTY_TEST_MAIN()
