// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_atlas_generations — each sheet's counter moves only for its own pixels.
//
// An uploader re-sends a sheet when its counter changes, so a counter shared
// between sheets is a counter that re-sends sheets nothing wrote to. The web
// page had one for all three: the colour sheet is four bytes per pixel where
// the other two are one, so a mono glyph faulting re-sent megabytes of emoji
// that had not moved.
//
// Which sheet was written is a fact the rasteriser has and an uploader cannot
// recover, so it is the rasteriser that has to keep them apart. That is what
// this pins — not that the counters advance, but that they advance
// SEPARATELY. A test asserting only "the counter moved" passes just as well on
// one counter wired to three names.

#include "Harness.h"

#include "Core/Rendering/SoftwareRenderer.h"

#include <vector>

using Ubiquitty::Rendering::SoftwareRenderer;

namespace {

// The three counters as one value, so a case can say which of them moved.
struct Generations {
    std::uint32_t mono = 0;
    std::uint32_t wide = 0;
    std::uint32_t color = 0;
};

Generations readGenerations(const SoftwareRenderer& renderer)
{
    return { renderer.monoAtlasGeneration(), renderer.wideAtlasGeneration(), renderer.colorAtlasGeneration() };
}

} // namespace

TEST(atlasGenerationsAreSeparate)
{
    constexpr int cellWidth = 8;
    constexpr int cellHeight = 16;
    constexpr std::uint32_t letterA = 0x41;
    constexpr std::uint32_t grinningFace = 0x1F600;
    const Ubiquitty::Rendering::FontFace regular {};

    // --- Rasterising a mono glyph moves the mono counter and nothing else.
    //
    // This is the case the split exists for: the common fault. Under one
    // counter it re-sent the colour sheet, which is four bytes per pixel and
    // holds emoji that a letter cannot have changed.
    {
        SoftwareRenderer renderer(cellWidth, cellHeight);
        const Generations before = readGenerations(renderer);
        renderer.rasteriseMonoGlyph(letterA, regular, 0, 0);
        const Generations after = readGenerations(renderer);

        CHECK(after.mono != before.mono);
        CHECK(after.wide == before.wide);
        CHECK(after.color == before.color);
    }

    // --- Rasterising a colour glyph moves the colour counter and nothing else.
    {
        SoftwareRenderer renderer(cellWidth, cellHeight);
        const Generations before = readGenerations(renderer);
        renderer.rasteriseColorGlyph(&grinningFace, 1, 0);
        const Generations after = readGenerations(renderer);

        CHECK(after.color != before.color);
        CHECK(after.mono == before.mono);
        CHECK(after.wide == before.wide);
    }

    // --- Repeated mono faults leave the colour sheet where it was.
    //
    // Stated over several faults rather than one, because a counter that is
    // shared through some paths and split through others would pass a
    // single-call check and fail here.
    {
        SoftwareRenderer renderer(cellWidth, cellHeight);
        const Generations before = readGenerations(renderer);
        for (std::uint32_t codePoint = letterA; codePoint < letterA + 8; ++codePoint)
            renderer.rasteriseMonoGlyph(codePoint, regular, 0, static_cast<int>(codePoint - letterA));
        const Generations after = readGenerations(renderer);

        CHECK(after.mono != before.mono);
        CHECK(after.color == before.color);
    }

    // --- Each counter starts where the others do, so a first write to one
    // sheet is distinguishable from a sheet nothing has touched.
    {
        SoftwareRenderer renderer(cellWidth, cellHeight);
        const Generations start = readGenerations(renderer);
        CHECK(start.mono == start.wide);
        CHECK(start.wide == start.color);
    }
}

UBIQUITTY_TEST_MAIN()
