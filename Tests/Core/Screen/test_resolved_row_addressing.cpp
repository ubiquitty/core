// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// A resolved row reads the cells the per-cell addressing reads.
//
// visibleCell re-derives the history depth and re-resolves the ring slot for
// one cell, so a consumer walking a whole screen pays that per column. The
// answer is resolveVisibleRow plus cellInResolvedRow, and the claim that makes
// the substitution safe is equivalence — including where the two could most
// easily part: columns past a scrollback line's length, rows with no cells at
// all, and view offsets clamped past the end of the history.
//
// Held over the whole address space rather than at sampled points, because the
// blank convention is exactly what a sampled corpus reaches by accident.

#include "Assertions.h"
#include "Harness.h"
#include "TerminalFixture.h"

#include "Core/Screen/Addressing.h"
#include "Core/TerminalOperations.h"

#include <cstring>
#include <memory>

using UbiquittyTest::feed;

namespace {

constexpr int gridRows = 6;
constexpr int gridColumns = 24;

// Lines of unlike lengths, so the scrollback carries rows whose length falls
// short of the width — the columns where a resolved row and a per-cell read
// could disagree. Wide characters, a link and an empty line are in the mix
// because each writes a cell whose bytes a blank does not have.
std::unique_ptr<Ubiquitty::Terminal> makePopulatedTerminal()
{
    auto t = std::make_unique<Ubiquitty::Terminal>();
    Ubiquitty::hardReset(*t);
    Ubiquitty::resize(*t, gridRows, gridColumns);
    feed(*t, "\x1b]8;id=1;https://example.invalid/one\x07");
    feed(*t, "a link that wraps across more than one row of this narrow grid\r\n");
    feed(*t, "\x1b]8;;\x07");
    feed(*t, "short\r\n");
    feed(*t, "\r\n");
    feed(*t, "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E wide leads and continuations\r\n");
    for (int line = 0; line < 12; ++line)
        feed(*t, "filler line to push the rows above into the scrollback\r\n");
    feed(*t, "\x1b[38;5;82mcoloured tail\x1b[0m");
    return t;
}

} // namespace

TEST(aResolvedRowReadsWhatPerCellAddressingReads)
{
    auto t = makePopulatedTerminal();
    const int history = Ubiquitty::Screen::scrollbackCount(t.get());
    CHECK_MSG(history > 0, "the corpus has to reach the scrollback for the length convention to be exercised");

    int comparisons = 0;
    int cellsPastRowLength = 0;
    int nonBlankCells = 0;
    // Past the history on purpose: an offset the view clamps is an address both
    // paths have to resolve the same way.
    for (int viewOffset = 0; viewOffset <= history + 2; ++viewOffset) {
        for (int row = 0; row < gridRows; ++row) {
            const Ubiquitty::Screen::VisibleRow resolved = Ubiquitty::Screen::resolveVisibleRow(t.get(), viewOffset, row);
            for (int column = 0; column < gridColumns; ++column) {
                const Ubiquitty::Cell* perCell = Ubiquitty::Screen::visibleCell(t.get(), viewOffset, row, column);
                const Ubiquitty::Cell* fromRow = Ubiquitty::Screen::cellInResolvedRow(resolved, column, gridColumns);
                CHECK(perCell != nullptr && fromRow != nullptr);
                CHECK_MSG(std::memcmp(perCell, fromRow, sizeof(Ubiquitty::Cell)) == 0,
                    "a resolved row handed back a different cell than the per-cell address did");
                if (column >= resolved.length)
                    ++cellsPastRowLength;
                if (std::memcmp(perCell, &Ubiquitty::defaultBlankCell, sizeof(Ubiquitty::Cell)) != 0)
                    ++nonBlankCells;
                ++comparisons;
            }
        }
    }

    // Guards against a corpus that compares nothing: the walk must run, must
    // reach columns past a row's length, and must find content — a screen of
    // blanks agrees with anything.
    CHECK_EQ(comparisons, (history + 3) * gridRows * gridColumns);
    CHECK_MSG(cellsPastRowLength > 0, "no column fell past a row's length, so the blank convention went unread");
    CHECK_MSG(nonBlankCells > 0, "every cell compared was blank, so the walk proved nothing");

    t.reset();
}

TEST(aResolvedRowIsBlankWhereThereIsNoRow)
{
    auto t = makePopulatedTerminal();
    // A row with no cells at all: every column of it is blank, which is what
    // visibleCell answers for a row it cannot resolve.
    const Ubiquitty::Screen::VisibleRow empty { nullptr, 0, false };
    for (int column = 0; column < gridColumns; ++column) {
        const Ubiquitty::Cell* cell = Ubiquitty::Screen::cellInResolvedRow(empty, column, gridColumns);
        CHECK(cell != nullptr);
        CHECK_MSG(std::memcmp(cell, &Ubiquitty::defaultBlankCell, sizeof(Ubiquitty::Cell)) == 0,
            "a row with no cells handed back something other than a blank");
    }
    // And a column outside the terminal, which the per-cell path blanks too.
    const Ubiquitty::Screen::VisibleRow row = Ubiquitty::Screen::resolveVisibleRow(t.get(), 0, 0);
    for (int column : { -1, gridColumns, gridColumns + 7 }) {
        const Ubiquitty::Cell* fromRow = Ubiquitty::Screen::cellInResolvedRow(row, column, gridColumns);
        const Ubiquitty::Cell* perCell = Ubiquitty::Screen::visibleCell(t.get(), 0, 0, column);
        CHECK_MSG(std::memcmp(fromRow, perCell, sizeof(Ubiquitty::Cell)) == 0,
            "a column outside the terminal read differently through a resolved row");
    }
    t.reset();
}

UBIQUITTY_TEST_MAIN()
