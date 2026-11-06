// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_image_placement_rows — a placement's row past the 16 bits it is held in.
//
// `TerminalImagePlacement::cursorY` is an `int16_t`, and a scrollback may hold
// up to a million lines. An image scrolled more than 32768 rows above the
// screen has no row the anchor can hold. Narrowed to 16 bits, its row wraps to
// a positive one, and 65536 scrolls after it was placed the image is drawn on
// the live screen again. These cases place an image under a scrollback longer
// than 32768 lines and scroll it past that, by output and by a resize.

#include "Core/Image/TerminalImage.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"
#include "KittyGraphicsFixture.h"

#include <string>

namespace {

using UbiquittyTest::Kitty::apc;
using UbiquittyTest::Kitty::base64;
using UbiquittyTest::Kitty::feed;
using UbiquittyTest::Kitty::makeRGBA;

constexpr int rows = 10;

/// A terminal of `rows` rows whose history keeps 40,000 lines, with one
/// one-row image placed in its top row and the cursor on its bottom row.
void prepare(Ubiquitty::Terminal& t)
{
    Ubiquitty::resize(t, rows, 40, Ubiquitty::Modes::ReflowMode::Fixed);
    Ubiquitty::hardReset(t);
    Ubiquitty::setScrollbackLimit(&t, 40000);
    t.cellPixelWidth = 8;
    t.cellPixelHeight = 16;
    feed(t, "\033[1;1H");
    feed(t, apc("a=T,f=32,s=2,v=2,c=2,r=1,C=1,q=2,i=1,p=1", base64(makeRGBA(2, 2))));
    Ubiquitty::drainAsyncDecodes(t);
    feed(t, "\033[" + std::to_string(rows) + ";1H");
}

/// Scrolls the screen up by `lines` lines with line feeds on the bottom row.
void scroll(Ubiquitty::Terminal& t, int lines)
{
    feed(t, std::string(static_cast<std::size_t>(lines), '\n'));
}

/// Whether any live placement is anchored on the visible screen's rows.
bool isPlacementOnScreen(const Ubiquitty::Terminal& t)
{
    for (const auto& placement : t.images.placements) {
        if (placement.used && placement.cursorY >= 0 && placement.cursorY < rows)
            return true;
    }
    return false;
}

} // namespace

TEST(testAPlacementScrolledPastItsRowRangeIsDroppedNotWrapped)
{
    Ubiquitty::Terminal t;
    prepare(t);
    REQUIRE(Ubiquitty::Image::placementCount(&t) == 1);

    // Within the anchor's range the image stays in history, where the
    // scrollback still holds its row.
    scroll(t, 32000);
    CHECK(Ubiquitty::Image::placementCount(&t) == 1);
    CHECK(!isPlacementOnScreen(t));

    // 65536 scrolls in all: a wrapped 16-bit row is back at the top row.
    scroll(t, 65536 - 32000);
    CHECK(Ubiquitty::Image::placementCount(&t) == 0);
    CHECK(!isPlacementOnScreen(t));
    CHECK(!Ubiquitty::Image::hasPlacementOnViewport(t, 0));
    CHECK(t.images.livePlacementCount == 0u);
}

TEST(testAPlacementScrolledSeventyThousandLinesUnderALongHistoryStaysOffTheScreen)
{
    Ubiquitty::Terminal t;
    prepare(t);
    for (int step = 0; step < 7; ++step) {
        scroll(t, 10000);
        CHECK(!isPlacementOnScreen(t));
    }
    CHECK(Ubiquitty::Image::placementCount(&t) == 0);
}

TEST(testAResizeThatPushesAPlacementPastItsRowRangeHoldsItAtTheOldestRow)
{
    Ubiquitty::Terminal t;
    prepare(t);
    scroll(t, 32765);
    REQUIRE(Ubiquitty::Image::placementCount(&t) == 1);

    // A reflowing shrink moves the screen's top rows into history, and the
    // placement with them: past the range its anchor can hold.
    Ubiquitty::resize(t, 2, 40, Ubiquitty::Modes::ReflowMode::Reflow);
    for (const auto& placement : t.images.placements) {
        if (placement.used)
            CHECK(placement.cursorY < 0);
    }
    CHECK(!Ubiquitty::Image::hasPlacementOnViewport(t, 0));

    // The next scroll takes it past the range and drops it.
    feed(t, "\n\n\n");
    CHECK(Ubiquitty::Image::placementCount(&t) == 0);
}

UBIQUITTY_TEST_MAIN()
