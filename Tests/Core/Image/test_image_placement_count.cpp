// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_image_placement_count — the live-placement count against the slots.
//
// `livePlacementCount` gates every path that walks placements: the scroll
// shift, the frame capture's placement copy, and the reflow that cannot defer
// while a placement must be remapped. It is only an answer while it equals the
// number of used slots. A path that frees a slot without counting it down
// leaves those gates open with nothing on the screen, and one that counts down
// twice closes them over a live image. Every way a placement can go is driven
// here from the wire, and each one has to leave the count equal to the slots.

#include "Core/Image/TerminalImage.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"
#include "KittyGraphicsFixture.h"

#include <cstdint>
#include <functional>
#include <string>

namespace {

using UbiquittyTest::Kitty::apc;
using UbiquittyTest::Kitty::base64;
using UbiquittyTest::Kitty::feed;
using UbiquittyTest::Kitty::makeRGBA;

void prepare(Ubiquitty::Terminal& t)
{
    Ubiquitty::resize(t, 10, 40, Ubiquitty::Modes::ReflowMode::Fixed);
    Ubiquitty::hardReset(t);
    t.cellPixelWidth = 8;
    t.cellPixelHeight = 16;
}

/// Places image `id` with the same placement id at the ZERO-based cell
/// (column, row) on layer `id`, leaving the cursor where it was.
void placeAt(Ubiquitty::Terminal& t, int id, int column, int row)
{
    const std::string number = std::to_string(id);
    feed(t, "\033[" + std::to_string(row + 1) + ";" + std::to_string(column + 1) + "H");
    feed(t, apc("a=T,f=32,s=2,v=2,c=2,r=1,C=1,q=2,i=" + number + ",p=" + number + ",z=" + number, base64(makeRGBA(2, 2))));
    Ubiquitty::drainAsyncDecodes(t);
}

/// Three placements on distinct rows, columns and layers, so every selector
/// below picks a strict subset or all of them.
void placeThree(Ubiquitty::Terminal& t)
{
    placeAt(t, 1, 0, 0);
    placeAt(t, 2, 10, 3);
    placeAt(t, 3, 20, 6);
}

struct DeletePath {
    const char* name;
    std::function<void(Ubiquitty::Terminal&)> remove;
    int placementsLeft;
};

} // namespace

TEST(testEveryDeletePathKeepsTheLiveCountEqualToTheSlots)
{
    const DeletePath paths[] = {
        { "d=a all", [](Ubiquitty::Terminal& t) { feed(t, apc("a=d,d=a,q=2", "")); }, 0 },
        { "d=A all with data", [](Ubiquitty::Terminal& t) { feed(t, apc("a=d,d=A,q=2", "")); }, 0 },
        { "d=I by id", [](Ubiquitty::Terminal& t) { feed(t, apc("a=d,d=I,i=2,q=2", "")); }, 2 },
        { "d=i by placement id", [](Ubiquitty::Terminal& t) { feed(t, apc("a=d,d=i,i=2,p=2,q=2", "")); }, 2 },
        { "d=R by id range", [](Ubiquitty::Terminal& t) { feed(t, apc("a=d,d=R,x=1,y=2,q=2", "")); }, 1 },
        { "d=p by cell", [](Ubiquitty::Terminal& t) { feed(t, apc("a=d,d=p,x=11,y=4,q=2", "")); }, 2 },
        { "d=q by cell and layer", [](Ubiquitty::Terminal& t) { feed(t, apc("a=d,d=q,x=11,y=4,z=2,q=2", "")); }, 2 },
        { "d=x by column", [](Ubiquitty::Terminal& t) { feed(t, apc("a=d,d=x,x=21,q=2", "")); }, 2 },
        { "d=y by row", [](Ubiquitty::Terminal& t) { feed(t, apc("a=d,d=y,y=1,q=2", "")); }, 2 },
        { "d=z by layer", [](Ubiquitty::Terminal& t) { feed(t, apc("a=d,d=z,z=3,q=2", "")); }, 2 },
        { "d=C at the cursor", [](Ubiquitty::Terminal& t) { feed(t, "\033[4;11H" + apc("a=d,d=C,q=2", "")); }, 2 },
        { "ED 2", [](Ubiquitty::Terminal& t) { feed(t, "\033[2J"); }, 0 },
        { "RIS", [](Ubiquitty::Terminal& t) { feed(t, "\033c"); }, 0 },
        { "scrolled past the history", [](Ubiquitty::Terminal& t) {
             t.scrollbackLimit = 2;
             feed(t, "\033[10;1H");
             for (int line = 0; line < 30; ++line)
                 feed(t, "\n");
         },
            0 },
    };

    for (const DeletePath& path : paths) {
        Ubiquitty::Terminal t;
        prepare(t);
        placeThree(t);
        CHECK_MSG(Ubiquitty::Image::placementCount(&t) == 3, path.name);
        CHECK_MSG(t.images.livePlacementCount == 3u, path.name);

        path.remove(t);
        Ubiquitty::drainAsyncDecodes(t);
        const int slots = Ubiquitty::Image::placementCount(&t);
        CHECK_MSG(slots == path.placementsLeft, path.name);
        CHECK_MSG(t.images.livePlacementCount == static_cast<std::uint32_t>(slots), path.name);
    }
}

TEST(testAnAlternateScreenReentryKeepsTheLiveCountEqualToTheSlots)
{
    // Entering the alternate screen erases it, which drops the placements left
    // there by the last program that used it; the primary screen's stay.
    Ubiquitty::Terminal t;
    prepare(t);
    placeAt(t, 1, 0, 0);
    feed(t, "\033[?1049h");
    placeAt(t, 2, 10, 3);
    placeAt(t, 3, 20, 6);
    feed(t, "\033[?1049l\033[?1049h");
    CHECK(Ubiquitty::Image::placementCount(&t) == 1);
    CHECK(t.images.livePlacementCount == 1u);
}

UBIQUITTY_TEST_MAIN()
