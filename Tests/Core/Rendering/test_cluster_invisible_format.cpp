// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// The portable cluster composite draws each code point of a cluster in turn and
// takes the maximum coverage, which is what preserves a combining mark on a
// backend that cannot shape. A variation selector has no glyph of its own, so
// the font answers that request with its last-resort box — and the box lands on
// top of the base, which is the tofu ubiquitty-frontends filed against
// `text_variation_selectors`.

#include "Core/Rendering/UnifontRasterizer.h"

#include <array>
#include <cstdint>
#include <vector>

namespace {

constexpr int slotWidth = 16;
constexpr int slotHeight = 16;
// Present in the bundled Unifont, so a raster that comes back empty is a
// finding rather than the font declining a code point it never had.
constexpr std::uint32_t warningSign = 0x26A0;
constexpr std::uint32_t combiningAcute = 0x0301;

using Coverage = std::vector<std::uint8_t>;

Coverage rasteriseOne(Ubiquitty::Rendering::UnifontRasterizer& font, std::uint32_t codePoint)
{
    Coverage out(static_cast<std::size_t>(slotWidth) * slotHeight);
    CHECK(font.rasterizeGlyph(codePoint, Ubiquitty::Rendering::FontFace::Regular, 0, slotWidth, slotHeight, out.data()));
    return out;
}

Coverage rasteriseCluster(Ubiquitty::Rendering::UnifontRasterizer& font, std::initializer_list<std::uint32_t> codePoints)
{
    Coverage out(static_cast<std::size_t>(slotWidth) * slotHeight);
    const std::vector<std::uint32_t> points(codePoints);
    CHECK(font.rasterizeCluster(points.data(), static_cast<int>(points.size()), Ubiquitty::Rendering::FontFace::Regular, 0, slotWidth, slotHeight, out.data()));
    return out;
}

long inkOf(const Coverage& coverage)
{
    long lit = 0;
    for (std::uint8_t byte : coverage)
        lit += byte != 0 ? 1 : 0;
    return lit;
}

} // namespace

// A selector contributes nothing, so the cluster is the base glyph and not the
// base with a box over it. Asserted as byte equality rather than as an ink
// budget: a box that happened to cover the same count would satisfy a count.
TEST(aVariationSelectorAddsNoCoverageToItsBase)
{
    Ubiquitty::Rendering::UnifontRasterizer font;
    const Coverage bare = rasteriseOne(font, warningSign);
    CHECK_MSG(inkOf(bare) > 0, "the base glyph inks, so the comparisons below are not between two empty slots");

    for (std::uint32_t selector : { 0xFE0Eu, 0xFE0Fu, 0x200Du, 0xE0100u })
        CHECK_MSG(rasteriseCluster(font, { warningSign, selector }) == bare, "the selector left the base raster alone");
}

// The control the assertion above needs: the skip must be the selector's, not
// the cluster path's. A real combining mark still composites, so a filter that
// swallowed every trailing code point would fail here.
TEST(aCombiningMarkStillCompositesOntoItsBase)
{
    Ubiquitty::Rendering::UnifontRasterizer font;
    const Coverage bare = rasteriseOne(font, warningSign);
    const Coverage marked = rasteriseCluster(font, { warningSign, combiningAcute });
    CHECK_MSG(marked != bare, "the combining mark reached the raster");
    CHECK_MSG(inkOf(marked) > inkOf(bare), "and it added coverage rather than replacing it");
}

// A cluster of nothing but selectors has no glyph to draw, which the seam
// reports as a decline so the caller leaves the slot blank instead of blitting
// a slot it believes was filled.
TEST(aClusterOfSelectorsAloneDeclines)
{
    Ubiquitty::Rendering::UnifontRasterizer font;
    Coverage out(static_cast<std::size_t>(slotWidth) * slotHeight, 0xFFu);
    const std::array<std::uint32_t, 2> selectors { 0xFE0Fu, 0x200Du };
    CHECK(!font.rasterizeCluster(selectors.data(), static_cast<int>(selectors.size()), Ubiquitty::Rendering::FontFace::Regular, 0, slotWidth, slotHeight, out.data()));
}

UBIQUITTY_TEST_MAIN()
