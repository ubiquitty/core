// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// A paint session that stops before the end of the grid has not painted the
// screen, and must not be recorded as though it had.
//
// beginPaint consumes the full-redraw request at construction, and the
// destructor decides whether the shadow now describes the screen. A caller that
// runs out of frame budget and drops the session mid-walk leaves every row it
// never reached unoffered: those rows carry clean dirty hints, so once the
// shadow is called valid the next walk skips them and their pixels stay stale
// until something else writes to them.

#include "Harness.h"
#include "TerminalFixture.h"

#include "Core/CellGrid.h"
#include "Core/PaintSession.h"
#include "Core/TerminalOperations.h"

#include <vector>

using UbiquittyTest::feed;

namespace {

// Drains the whole grid the way a frontend with budget to spare does.
int drainEveryRun(Ubiquitty::Terminal& terminal)
{
    int runs = 0;
    Ubiquitty::PaintSession session = terminal.grid->beginPaint();
    Ubiquitty::PaintRun run;
    while (session.nextDirtyRun(&run)) {
        session.ackRun(run);
        ++runs;
    }
    return runs;
}

// Takes `limit` runs and drops the session, the way a frontend that runs out of
// frame budget does.
void drainAtMost(Ubiquitty::Terminal& terminal, int limit)
{
    Ubiquitty::PaintSession session = terminal.grid->beginPaint();
    Ubiquitty::PaintRun run;
    for (int taken = 0; taken < limit && session.nextDirtyRun(&run); ++taken)
        session.ackRun(run);
}

// Every cell the walk offers, gathered in one session — a session drains the
// whole grid, so asking row by row would answer for the first row and report
// every later one as clean.
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

std::vector<bool> offeredColumns(Ubiquitty::Terminal& terminal, int row)
{
    return offeredGrid(terminal)[static_cast<std::size_t>(row)];
}

bool anyOffered(const std::vector<bool>& offered)
{
    for (const bool isOffered : offered) {
        if (isOffered)
            return true;
    }
    return false;
}

} // namespace

TEST(anAbandonedFullRedrawStillOwesThePaint)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 8, 40);
    feed(t, "first line\r\nsecond line\r\nthird line");

    // Painted: the shadow matches the screen and the dirty hints are clear.
    (void)drainEveryRun(t);
    CHECK(!anyOffered(offeredColumns(t, 2)));

    // The font-size change: same cells, every glyph re-laid.
    t.grid->requestFullRedraw();

    // The frontend gets one run out and drops the session.
    drainAtMost(t, 1);

    // The redraw is still owed. Without it the last row was never offered and
    // never will be, because its hint is clean and the shadow claims to match.
    CHECK(t.grid->hasUnpaintedWork());
    CHECK(anyOffered(offeredColumns(t, 2)));
}

TEST(anAbandonedFullRedrawOffersEveryRowItNeverReached)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 6, 32);
    feed(t, "alpha\r\nbravo\r\ncharlie\r\ndelta");
    (void)drainEveryRun(t);

    t.grid->requestFullRedraw();
    drainAtMost(t, 2);

    // Each written row owes its full width, whether or not the abandoned walk
    // happened to reach it.
    const std::vector<std::vector<bool>> offered = offeredGrid(t);
    for (int row = 0; row < 4; ++row) {
        for (int column = 0; column < t.columns; ++column)
            CHECK(offered[static_cast<std::size_t>(row)][static_cast<std::size_t>(column)]);
    }
}

TEST(aCompletedFullRedrawIsDischarged)
{
    // The other side of the guard: a walk that ran to the end has honoured the
    // request, so it must not fire again and repaint the screen every frame.
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 6, 32);
    feed(t, "content");
    (void)drainEveryRun(t);

    t.grid->requestFullRedraw();
    (void)drainEveryRun(t);

    CHECK(!anyOffered(offeredColumns(t, 0)));
    CHECK(!t.grid->hasUnpaintedWork());
}

TEST(anAbandonedIncrementalWalkKeepsOwingItsRows)
{
    // No full-redraw request in play: the rows the walk never reached are dirty
    // in their own right, and an early stop must not clear that.
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 6, 32);
    feed(t, "one\r\ntwo\r\nthree\r\nfour");

    drainAtMost(t, 1);

    CHECK(t.grid->hasUnpaintedWork());
    CHECK(anyOffered(offeredColumns(t, 3)));
}

UBIQUITTY_TEST_MAIN()
