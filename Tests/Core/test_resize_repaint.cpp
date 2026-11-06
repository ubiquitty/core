// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// A resize that changes no dimension still has to repaint.
//
// Changing the font size or family re-lays every glyph. When the window's pixel
// dimensions still divide into the same grid — which is the common case for a
// one-step size change, and the only case for a family change — the frontend
// asks the terminal to resize to the size it already has.
//
// The cell COUNT is unchanged, so the reflow has nothing to move and returns
// early. The cells' PIXELS are not: the shadow the damage walk compares against
// mirrors the bitmap, so after a re-lay it describes glyphs that are no longer
// on screen. A walk that trusts it offers no runs, the frontend repaints
// nothing, and the old type stays up until something else dirties the row.

#include "Harness.h"
#include "TerminalFixture.h"

#include "Core/CellGrid.h"
#include "Core/PaintSession.h"
#include "Core/TerminalOperations.h"

#include <string>
#include <vector>

using UbiquittyTest::cell;
using UbiquittyTest::feed;

namespace {

// Every column the walk offers for `row`, drained as a capture drains it.
std::vector<bool> offeredColumns(Ubiquitty::Terminal& terminal, int row)
{
    std::vector<bool> offered(static_cast<std::size_t>(terminal.columns), false);
    Ubiquitty::PaintSession session = terminal.grid->beginPaint();
    Ubiquitty::PaintRun run;
    while (session.nextDirtyRun(&run)) {
        if (run.row.value() == row) {
            for (int column = run.firstColumn.value(); column <= run.lastColumn.value() && column < terminal.columns; ++column)
                offered[static_cast<std::size_t>(column)] = true;
        }
        session.ackRun(run);
    }
    return offered;
}

int offeredCount(const std::vector<bool>& offered)
{
    int count = 0;
    for (const bool isOffered : offered)
        count += isOffered ? 1 : 0;
    return count;
}

} // namespace

TEST(aResizeToTheSameSizeRepaintsEveryCell)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 6, 24);
    feed(t, "content that must be redrawn");

    // Painted: the shadow now matches, and a second walk offers nothing.
    (void)offeredColumns(t, 0);
    CHECK(offeredCount(offeredColumns(t, 0)) == 0);

    // The font-size change: same rows, same columns, different glyphs.
    Ubiquitty::resize(t, 6, 24);

    // Every column of every occupied row comes back, because every one of them
    // is drawn from a face the shadow has never seen.
    const std::vector<bool> offered = offeredColumns(t, 0);
    CHECK(offeredCount(offered) == t.columns);
}

TEST(aResizeToTheSameSizeKeepsTheContentItRedraws)
{
    // The repaint must not be a clear: the cells are the same cells, and only
    // their pixels are stale.
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 4, 20);
    feed(t, "keep me");
    (void)offeredColumns(t, 0);

    Ubiquitty::resize(t, 4, 20);
    CHECK(cell(t, 0, 0)->codePoint == 'k');
    CHECK(cell(t, 0, 6)->codePoint == 'e');
    CHECK(t.screens.active->cursorX == 7);
}

TEST(aFirstPaintOffersEveryWrittenCell)
{
    // The other half of the same question: a terminal that has just opened and
    // been written to owes its whole first line, because nothing has painted it
    // yet and the shadow describes an empty grid.
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 6, 24);
    feed(t, "user@host:~$ hello");

    const std::vector<bool> offered = offeredColumns(t, 0);
    for (int column = 0; column < 18; ++column)
        CHECK(offered[static_cast<std::size_t>(column)]);
}

UBIQUITTY_TEST_MAIN()
