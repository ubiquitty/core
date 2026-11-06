// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Small conformance holes with visible consequences: a wrap that fires from a
// homed cursor, a history erase that rewrites the live screen's metadata, and
// a command-block scan whose cost multiplies by the buffer.

#include "Harness.h"
#include "TerminalFixture.h"

#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"

#include <string>

using UbiquittyTest::cell;
using UbiquittyTest::feed;

namespace {

Ubiquitty::Terminal& freshTerminal(Ubiquitty::Terminal& t, int rows = 8, int columns = 20)
{
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, rows, columns);
    return t;
}

// Arms the deferred wrap: the cursor sits ON the last column having written it,
// and the next printable would wrap to the following row.
void armWrap(Ubiquitty::Terminal& t)
{
    feed(t, "\x1b[1;1H");
    feed(t, std::string(static_cast<std::size_t>(t.columns), 'w'));
    CHECK(t.screens.active->wrapPending);
}

} // namespace

TEST(aScrollRegionHomeDisarmsThePendingWrap)
{
    // DECSTBM homes the cursor. A wrap armed by the previous cell firing from
    // the HOMED position writes the next character one row down at column 0 —
    // a stray character an application never printed there.
    Ubiquitty::Terminal t;
    freshTerminal(t);
    armWrap(t);

    feed(t, "\x1b[2;6r");
    CHECK(!t.screens.active->wrapPending);

    feed(t, "X");
    CHECK(cell(t, 0, 0)->codePoint == 'X');
    feed(t, "\x1b[r");
}

TEST(aMarginHomeDisarmsThePendingWrap)
{
    // DECSLRM under DECLRMM homes the cursor the same way.
    Ubiquitty::Terminal t;
    freshTerminal(t);
    feed(t, "\x1b[?69h");
    armWrap(t);

    feed(t, "\x1b[3;15s");
    CHECK(!t.screens.active->wrapPending);

    feed(t, "Y");
    CHECK(cell(t, 0, 0)->codePoint == 'Y');
    feed(t, "\x1b[s\x1b[?69l");
}

TEST(erasingSavedLinesLeavesTheLiveScreenAlone)
{
    // ED 3 erases scrollback. The visible rows' soft-wrap joins and line
    // attributes are content the erase never touched: a reflow after a
    // clear-history must still rejoin the wrapped line, and a double-width
    // banner on screen keeps its width.
    Ubiquitty::Terminal t;
    freshTerminal(t, 6, 10);

    // Scrollback to erase, pushed through the top FIRST — a scroll after the
    // metadata is written would carry the flags to other rows and the
    // assertions would read the wrong ones.
    feed(t, "\x1b[6;1H\n\n\n");

    // A soft-wrapped line: eleven characters in a ten-column grid.
    feed(t, "\x1b[1;1H");
    feed(t, "abcdefghijk");
    CHECK(t.screens.active->lineWrapped[0]);

    // A double-width banner two rows down.
    feed(t, "\x1b[3;1H\x1b#6BANNER");
    CHECK(t.screens.active->lineAttribute[2] != 0);

    feed(t, "\x1b[3J");

    CHECK_MSG(t.screens.active->lineWrapped[0], "ED 3 broke a live soft-wrap join");
    CHECK_MSG(t.screens.active->lineAttribute[2] != 0, "ED 3 flattened a live double-width banner");
    CHECK(t.scrollbackCount == 0);
}

UBIQUITTY_TEST_MAIN()
