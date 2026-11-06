// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// A scroll confined to horizontal margins damages the band it moved, not the
// screen around it.
//
// DECSLRM is the tmux case: side-by-side panes set left/right margins, and one
// pane's output scrolls a column band while the other pane's rows sit unchanged
// beside it. The rectangle move bumps every row of the band as it goes, so the
// damage is already recorded row by row, and the cells outside the band's
// columns still match the shadow they were painted from.

#include "Harness.h"
#include "TerminalFixture.h"

#include "Core/CellGrid.h"
#include "Core/PaintSession.h"
#include "Core/Screen/Operations.h"
#include "Core/TerminalOperations.h"

#include <string>
#include <vector>

using UbiquittyTest::feed;

namespace {

// Every cell the walk offers, gathered in one session — a session drains the
// whole grid, so asking row by row would answer for the first row alone.
std::vector<std::vector<bool>> offeredGrid(Ubiquitty::Terminal& terminal)
{
    std::vector<std::vector<bool>> offered(
        static_cast<std::size_t>(terminal.rows),
        std::vector<bool>(static_cast<std::size_t>(terminal.columns), false));
    Ubiquitty::PaintSession session = terminal.grid->beginPaint();
    Ubiquitty::PaintRun run;
    while (session.nextDirtyRun(&run)) {
        const int row = run.row.value();
        if (row >= 0 && row < terminal.rows) {
            for (int column = run.firstColumn.value(); column <= run.lastColumn.value() && column < terminal.columns; ++column)
                offered[static_cast<std::size_t>(row)][static_cast<std::size_t>(column)] = true;
        }
        session.ackRun(run);
    }
    return offered;
}

int offeredCellCount(const std::vector<std::vector<bool>>& offered)
{
    int count = 0;
    for (const std::vector<bool>& row : offered) {
        for (const bool isOffered : row)
            count += isOffered ? 1 : 0;
    }
    return count;
}

// Two panes side by side, as tmux lays them out: margins over the right-hand
// band, a scroll region over the full height, and text already painted in both.
void openTwoPanes(Ubiquitty::Terminal& t)
{
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 24, 80);
    // The left pane's content, written before the margins go up.
    for (int row = 0; row < 24; ++row) {
        feed(t, "\x1b[" + std::to_string(row + 1) + ";1H");
        feed(t, "left pane row");
    }
    // DECLRMM, then the right pane's band.
    feed(t, "\x1b[?69h\x1b[41;80s\x1b[1;24r");
}

} // namespace

TEST(aMarginedScrollLeavesTheRowsOutsideTheRegionAlone)
{
    Ubiquitty::Terminal t;
    openTwoPanes(t);
    // A shorter scroll region, so rows below it belong to nobody the scroll moves.
    feed(t, "\x1b[1;12r\x1b[12;41H");
    (void)offeredGrid(t);

    // One line feed at the bottom of the region: a margined scroll.
    feed(t, "\n");

    const std::vector<std::vector<bool>> offered = offeredGrid(t);
    for (int row = 12; row < 24; ++row) {
        for (int column = 0; column < t.columns; ++column)
            CHECK(!offered[static_cast<std::size_t>(row)][static_cast<std::size_t>(column)]);
    }
}

TEST(aMarginedScrollLeavesTheColumnsOutsideTheBandAlone)
{
    Ubiquitty::Terminal t;
    openTwoPanes(t);
    feed(t, "\x1b[24;41H");
    (void)offeredGrid(t);

    feed(t, "\n");

    // The left pane's columns were never touched by a band that starts at 41.
    const std::vector<std::vector<bool>> offered = offeredGrid(t);
    for (int row = 0; row < 24; ++row) {
        for (int column = 0; column < 40; ++column)
            CHECK(!offered[static_cast<std::size_t>(row)][static_cast<std::size_t>(column)]);
    }
}

TEST(aMarginedScrollStillOffersWhatItMoved)
{
    // The band's own content has to come back, or the pane stops updating —
    // the damage narrowed above must not narrow to nothing.
    Ubiquitty::Terminal t;
    openTwoPanes(t);
    feed(t, "\x1b[1;41H");
    for (int row = 0; row < 24; ++row)
        feed(t, "right\r\n");
    (void)offeredGrid(t);

    feed(t, "scrolled\r\n");

    const std::vector<std::vector<bool>> offered = offeredGrid(t);
    CHECK(offeredCellCount(offered) > 0);
}

TEST(aMarginedScrollKeepsTheContentItMoved)
{
    // Damage is a claim about pixels; the cells themselves must still be right.
    Ubiquitty::Terminal t;
    openTwoPanes(t);
    feed(t, "\x1b[1;41Halpha\r\n");
    feed(t, "bravo\r\n");
    (void)offeredGrid(t);

    // Fill to the bottom of the region so the next feed scrolls the band.
    for (int row = 0; row < 24; ++row)
        feed(t, "filler\r\n");

    // The left pane is untouched by every one of those scrolls.
    CHECK(UbiquittyTest::cell(t, 0, 0)->codePoint == 'l');
    CHECK(UbiquittyTest::cell(t, 23, 0)->codePoint == 'l');
}

TEST(aTmuxSplitOffersFarLessThanAScreenfulPerPaint)
{
    // The narrowing stated as a bound, so a change that widens it again fails
    // here. Two panes taking turns to print, painted after each burst as a
    // frontend paints: a band scroll owes its band, so a paint costs a fraction
    // of the screen. Requesting a full redraw per scroll instead measures
    // exactly one screenful per paint.
    Ubiquitty::Terminal t;
    openTwoPanes(t);

    const int screenful = t.rows * t.columns;
    long long offered = 0;
    int paints = 0;
    for (int burst = 0; burst < 40; ++burst) {
        const int pane = burst % 2;
        const int left = pane == 0 ? 1 : 41;
        const int right = pane == 0 ? 39 : 80;
        feed(t, "\x1b[" + std::to_string(left) + ";" + std::to_string(right) + "s");
        feed(t, "\x1b[1;23r");
        feed(t, "\x1b[23;" + std::to_string(left) + "H");
        for (int line = 0; line < 8; ++line)
            feed(t, "output line " + std::to_string(burst * 8 + line) + "\r\n");

        const std::vector<std::vector<bool>> paintedNow = offeredGrid(t);
        offered += offeredCellCount(paintedNow);
        ++paints;
    }

    // A screenful per paint is the shape of a full redraw; a band's worth is the
    // shape of the truth. The bound sits between them with room to spare rather
    // than pinned to a measurement that would drift.
    const double cellsPerPaint = static_cast<double>(offered) / static_cast<double>(paints);
    CHECK(cellsPerPaint < static_cast<double>(screenful) / 2.0);
}

UBIQUITTY_TEST_MAIN()
