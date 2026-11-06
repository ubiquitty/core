// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Screen/ViewPlacement.h"
#include "Harness.h"

// Where a jump lands the row it reveals. Every frontend routes here, because the
// placement is something a user compares directly: the same command block,
// reached the same way, has to sit in the same place in the browser as it does
// on macOS. Two of them computed it privately and disagreed — the block jump
// landed a quarter down on macOS and a third down on GTK.

using Ubiquitty::Screen::landingRowFor;
using Ubiquitty::Screen::RevealPlacement;
using Ubiquitty::Screen::viewOffsetToReveal;

// The offset inverts the address map `visibleCell` reads, so the revealed row
// lands on exactly the placement's row whenever there is room on both sides.
// Checked by reading the map forward rather than by restating the arithmetic,
// which would pass on any two expressions that agree with each other.
TEST(viewPlacementInvertsTheAddressMap)
{
    const int history = 1000;
    const int rows = 24;
    for (RevealPlacement placement : { RevealPlacement::Match, RevealPlacement::CommandBlock }) {
        for (int target : { 200, 500, 999 }) {
            const int offset = viewOffsetToReveal(history, rows, target, placement);
            // Read the address map forward rather than restating the offset:
            // the row the viewport shows on the landing row is the one asked for.
            const int shown = history - offset + landingRowFor(rows, placement);
            CHECK(shown == target);
        }
    }
}

// A command block sits higher than a match, so more of the output it produced is
// below it. This is the difference the frontends disagreed about, and it is the
// one thing about the two placements that has to stay true.
TEST(viewPlacementPutsACommandBlockAboveAMatch)
{
    CHECK(landingRowFor(24, RevealPlacement::CommandBlock) < landingRowFor(24, RevealPlacement::Match));
    CHECK(landingRowFor(100, RevealPlacement::CommandBlock) < landingRowFor(100, RevealPlacement::Match));

    // The same target therefore scrolls further into history for a block than
    // for a match: a smaller landing row means a smaller offset.
    const int forBlock = viewOffsetToReveal(1000, 24, 500, RevealPlacement::CommandBlock);
    const int forMatch = viewOffsetToReveal(1000, 24, 500, RevealPlacement::Match);
    CHECK(forBlock < forMatch);
}

// A row past either end resolves to the nearest end. Without the clamp a jump to
// a row that scrolled away would assign an offset outside the buffer, and the
// renderer would address history it does not have.
TEST(viewPlacementClampsToBothEnds)
{
    CHECK(viewOffsetToReveal(1000, 24, 5000, RevealPlacement::Match) == 0); // past the live tail
    CHECK(viewOffsetToReveal(1000, 24, -1, RevealPlacement::Match) == 1000); // above the oldest row
    CHECK(viewOffsetToReveal(1000, 24, 1000, RevealPlacement::CommandBlock) == 6);

    // The alternate screen keeps no history, so every jump resolves to the live
    // screen and no offset is representable.
    for (int target : { -5, 0, 3, 50 })
        for (RevealPlacement placement : { RevealPlacement::Match, RevealPlacement::CommandBlock })
            CHECK(viewOffsetToReveal(0, 24, target, placement) == 0);
}

// A viewport too short to divide lands on row 0 rather than dividing by zero or
// landing off-screen. One row is the smallest a resize can produce.
TEST(viewPlacementHandlesAViewportTooShortToDivide)
{
    for (int rows : { 0, 1, 2 }) {
        for (RevealPlacement placement : { RevealPlacement::Match, RevealPlacement::CommandBlock }) {
            CHECK(landingRowFor(rows, placement) == 0);
            // With no landing row to add, the offset is the distance to the target.
            CHECK(viewOffsetToReveal(100, rows, 40, placement) == 60);
        }
    }
    CHECK(landingRowFor(3, RevealPlacement::Match) == 1);
    CHECK(landingRowFor(3, RevealPlacement::CommandBlock) == 0);
}

UBIQUITTY_TEST_MAIN()
