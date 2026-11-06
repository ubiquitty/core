// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// A resize never leaves half a wide character behind.
//
// Reflow is the other operation that can land on one half of a pair alone, and
// it reaches further than an editing control does: narrowing rewraps a logical
// line across more physical rows, so every cell after the wrap point changes
// both row and column. A pair straddling the new right margin is the case —
// there is no column for the continuation on the row the lead ends up on.
//
// Same invariant as the editing-control sweep, asserted after a width change:
// every continuation has a lead immediately to its left, every lead a
// continuation immediately to its right. Stating it as a property rather than an
// expected grid is what lets one sweep cover widths the wrap arithmetic treats
// differently without encoding that arithmetic.
//
// The widths are chosen so a pair lands on the margin rather than clear of it:
// with pairs written every third column, an odd target width puts a lead in the
// last column of a rewrapped row, which is where a splitter splits.

#include "Harness.h"
#include "TerminalFixture.h"

#include "Core/TerminalOperations.h"

#include <cstdio>
#include <string>

using UbiquittyTest::feed;
using UbiquittyTest::firstOrphanedWideHalf;
using UbiquittyTest::wideHalfCount;

namespace {

// A CJK code point is width 2 on every width table the Core ships, so it lands
// as a lead plus a continuation without depending on emoji presentation rules.
constexpr const char* wideCharacter = "\xE6\x9D\xB1"; // U+6771

/// Fills the screen with wide pairs separated by single narrow columns.
///
/// The narrow separator is what shifts pair alignment relative to any margin: a
/// row of nothing but pairs aligns every pair to an even column and a resize to
/// an even width never splits one.
void fillWithPairs(Ubiquitty::Terminal& terminal)
{
    feed(terminal, "\x1b[H\x1b[2J");
    for (int row = 0; row < terminal.rows; ++row) {
        char place[32];
        std::snprintf(place, sizeof(place), "\x1b[%d;1H", row + 1);
        feed(terminal, place);
        for (int column = 0; column + 2 < terminal.columns; column += 3) {
            feed(terminal, wideCharacter);
            feed(terminal, "-");
        }
    }
}

} // namespace

TEST(narrowingLeavesNoOrphanedWideHalf)
{
    int checked = 0;
    for (int columns = 79; columns >= 10; --columns) {
        Ubiquitty::Terminal terminal;
        Ubiquitty::hardReset(terminal);
        fillWithPairs(terminal);
        Ubiquitty::resize(terminal, terminal.rows, columns);

        // The sweep passes trivially on a grid with no pairs left to tear.
        CHECK(wideHalfCount(terminal) > 0);
        const std::string orphan = firstOrphanedWideHalf(terminal);
        ++checked;
        if (!orphan.empty()) {
            char message[192];
            std::snprintf(message, sizeof(message), "narrowing 80 -> %d: %s", columns, orphan.c_str());
            CHECK_MSG(false, message);
        }
    }
    CHECK(checked == 70);
}

TEST(wideningLeavesNoOrphanedWideHalf)
{
    // Widening rejoins rows a previous wrap split, so a pair the narrow width
    // pushed onto the next row has to come back whole rather than as two halves
    // on either side of the join.
    for (int columns = 81; columns <= 120; ++columns) {
        Ubiquitty::Terminal terminal;
        Ubiquitty::hardReset(terminal);
        fillWithPairs(terminal);
        Ubiquitty::resize(terminal, terminal.rows, columns);

        // The sweep passes trivially on a grid with no pairs left to tear.
        CHECK(wideHalfCount(terminal) > 0);
        const std::string orphan = firstOrphanedWideHalf(terminal);
        char message[192];
        std::snprintf(message, sizeof(message), "widening 80 -> %d: %s", columns, orphan.c_str());
        CHECK_MSG(orphan.empty(), message);
    }
}

TEST(narrowThenWideRoundTripLeavesNoOrphanedWideHalf)
{
    // Two resizes in a row is where a splitter's damage becomes visible even
    // when each step looks whole on its own: the second reflow reads what the
    // first wrote, so an orphan the first created survives into a width where
    // the pair would have fitted.
    for (int narrowColumns = 40; narrowColumns >= 11; --narrowColumns) {
        Ubiquitty::Terminal terminal;
        Ubiquitty::hardReset(terminal);
        fillWithPairs(terminal);
        Ubiquitty::resize(terminal, terminal.rows, narrowColumns);
        Ubiquitty::resize(terminal, terminal.rows, 80);

        // The sweep passes trivially on a grid with no pairs left to tear.
        CHECK(wideHalfCount(terminal) > 0);
        const std::string orphan = firstOrphanedWideHalf(terminal);
        char message[192];
        std::snprintf(message, sizeof(message), "80 -> %d -> 80: %s", narrowColumns, orphan.c_str());
        CHECK_MSG(orphan.empty(), message);
    }
}

TEST(shrinkingRowsLeavesNoOrphanedWideHalf)
{
    // A row-count change scrolls surviving content into history rather than
    // rewrapping it, which is a different path through the same grid — and a
    // pair split across the history boundary is an orphan on both sides.
    for (int rows = 24; rows >= 2; --rows) {
        Ubiquitty::Terminal terminal;
        Ubiquitty::hardReset(terminal);
        fillWithPairs(terminal);
        Ubiquitty::resize(terminal, rows, terminal.columns);

        // The sweep passes trivially on a grid with no pairs left to tear.
        CHECK(wideHalfCount(terminal) > 0);
        const std::string orphan = firstOrphanedWideHalf(terminal);
        char message[192];
        std::snprintf(message, sizeof(message), "24 -> %d rows: %s", rows, orphan.c_str());
        CHECK_MSG(orphan.empty(), message);
    }
}

UBIQUITTY_TEST_MAIN()
