// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// A margin-bounded line scroll never leaves half a wide character behind.
//
// With DECLRMM margins narrower than the screen, IL and DL route through the
// partial-width rectangle scroll, whose copy loops move a column band and
// cannot see a run straddling either band edge: the half inside the band
// moves, the half outside stays. The scrub pass after the copy is what holds
// the invariant, and these cases place a pair across the margin on the
// destination row, the source row, and the blanked row to reach every shape
// the band cut can take.

#include "Harness.h"
#include "TerminalFixture.h"

#include "Core/TerminalOperations.h"

#include <string>

using UbiquittyTest::feed;
using UbiquittyTest::firstOrphanedWideHalf;

namespace {

// A CJK code point is width 2 on every width table the Core ships.
constexpr const char* wideCharacter = "\xE6\x9D\xB1"; // U+6771

void placePairAcrossLeftMargin(Ubiquitty::Terminal& t, int row1Based)
{
    // The pair lands at 1-based columns 5-6; the left margin at 6 falls
    // between its halves.
    char place[32];
    std::snprintf(place, sizeof(place), "\x1b[%d;5H", row1Based);
    feed(t, place);
    feed(t, wideCharacter);
}

} // namespace

TEST(insertLineInsideMarginsScrubsTheStraddlingPair)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    placePairAcrossLeftMargin(t, 1);
    placePairAcrossLeftMargin(t, 2);
    placePairAcrossLeftMargin(t, 3);
    feed(t, "\x1b[?69h\x1b[6;15s"); // DECLRMM on, margins 6..15
    feed(t, "\x1b[1;8H\x1b[L"); // IL inside the margins
    CHECK_MSG(firstOrphanedWideHalf(t).empty(), firstOrphanedWideHalf(t).c_str());
}

TEST(deleteLineInsideMarginsScrubsTheStraddlingPair)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    placePairAcrossLeftMargin(t, 1);
    placePairAcrossLeftMargin(t, 2);
    placePairAcrossLeftMargin(t, 3);
    feed(t, "\x1b[?69h\x1b[6;15s");
    feed(t, "\x1b[1;8H\x1b[M"); // DL inside the margins
    CHECK_MSG(firstOrphanedWideHalf(t).empty(), firstOrphanedWideHalf(t).c_str());
}

TEST(rightMarginEdgeScrubsTheSameWay)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    // Pair at 1-based 15-16; right margin at 15 falls between its halves.
    feed(t, "\x1b[1;15H");
    feed(t, wideCharacter);
    feed(t, "\x1b[2;15H");
    feed(t, wideCharacter);
    feed(t, "\x1b[?69h\x1b[6;15s");
    feed(t, "\x1b[1;8H\x1b[M");
    CHECK_MSG(firstOrphanedWideHalf(t).empty(), firstOrphanedWideHalf(t).c_str());
}

UBIQUITTY_TEST_MAIN()
