// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"
#include "TerminalFixture.h"

using UbiquittyTest::cell;
using UbiquittyTest::feed;

// Cursor motion against the margins: CUF/CUB/CNL/CPL/CBT/CHA (ECMA-48 clause 8.3), IND/RI,
// DECSC/DECRC, and reverse wraparound (DEC private mode 45), each crossed with DECSTBM and
// DECSLRM. https://vt100.net/docs/ and
// https://invisible-island.net/xterm/ctlseqs/ctlseqs.html for the DEC private modes.
//
// Entry point for the shared test binary; returns the failure count.

#include "Core/CellGrid.h"
#include "Core/TerminalOperations.h"
#include <cstdio>
#include <cstring>

int runCursorConformanceTests();

/// A 25x80 page: every case below reasons in absolute rows/columns of that geometry.
static void setUp(Ubiquitty::Terminal& t)
{
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 25, 80);
}

/// ---- CUF / CUB: the margin binds only a cursor that starts inside it ----

static void testCUFBegunRightOfMarginRunsToScreenEdge()
{
    Ubiquitty::Terminal t;
    setUp(t);
    feed(t, "\033[?69h\033[5;10s");
    feed(t, "\033[3;12H"); // column 12 — right of the right margin (10)
    feed(t, "\033[80C");
    CHECK(t.screens.active->cursorX == 79);
}

static void testCUFBegunInsideStopsAtRightMargin()
{
    Ubiquitty::Terminal t;
    setUp(t);
    feed(t, "\033[?69h\033[5;10s");
    feed(t, "\033[3;7H");
    feed(t, "\033[80C");
    CHECK(t.screens.active->cursorX == 9);
}

static void testCUBBegunLeftOfMarginRunsToScreenEdge()
{
    Ubiquitty::Terminal t;
    setUp(t);
    feed(t, "\033[?69h\033[5;10s");
    feed(t, "\033[3;4H"); // column 4 — left of the left margin (5)
    feed(t, "\033[99D");
    CHECK(t.screens.active->cursorX == 0);
}

/// BS shares cursorBackward with CUB, so driving CUB pins the BS path too.
static void testCUBOneLeftOfMarginMovesOneCell()
{
    Ubiquitty::Terminal t;
    setUp(t);
    feed(t, "\033[?69h\033[5;10s");
    feed(t, "\033[1;4H");
    feed(t, "\033[D");
    CHECK(t.screens.active->cursorX == 2);
    CHECK(t.screens.active->cursorY == 0);
}

static void testCUBBegunInsideStopsAtLeftMargin()
{
    Ubiquitty::Terminal t;
    setUp(t);
    feed(t, "\033[?69h\033[5;10s");
    feed(t, "\033[3;7H");
    feed(t, "\033[99D");
    CHECK(t.screens.active->cursorX == 4);
}

// ---- Reverse wraparound (DEC private 45) ----

/// Mode 45 acts only with DECAWM also set; either mode alone leaves the cursor put.
static void testReverseWrapRequiresDECAWM()
{
    Ubiquitty::Terminal t;
    setUp(t);
    feed(t, "\033[?7l\033[?45h"); // reverse wrap on, autowrap OFF
    feed(t, "\033[3;1H");
    feed(t, "\033[D");
    CHECK(t.screens.active->cursorX == 0);
    CHECK(t.screens.active->cursorY == 2);

    feed(t, "\033[?7h\033[?45l"); // autowrap on, reverse wrap OFF
    feed(t, "\033[3;1H");
    feed(t, "\033[D");
    CHECK(t.screens.active->cursorX == 0);
    CHECK(t.screens.active->cursorY == 2);
}

static void testReverseWrapToPreviousRow()
{
    Ubiquitty::Terminal t;
    setUp(t);
    feed(t, "\033[?7h\033[?1045h");
    feed(t, "\033[3;1H");
    feed(t, "\033[D");
    CHECK(t.screens.active->cursorX == 79);
    CHECK(t.screens.active->cursorY == 1);
}

/// Off the TOP margin the wrap lands on the bottom margin, not above the region.
static void testReverseWrapOffTopMarginLandsOnBottomMargin()
{
    Ubiquitty::Terminal t;
    setUp(t);
    feed(t, "\033[?7h\033[?1045h");
    feed(t, "\033[2;5r"); // DECSTBM rows 2..5
    feed(t, "\033[2;1H");
    feed(t, "\033[D");
    CHECK(t.screens.active->cursorX == 79);
    CHECK(t.screens.active->cursorY == 4);
}

static void testReverseWrapFromLeftMargin()
{
    Ubiquitty::Terminal t;
    setUp(t);
    feed(t, "\033[?7h\033[?1045h\033[?69h\033[5;10s");
    feed(t, "\033[3;5H");
    feed(t, "\033[D");
    CHECK(t.screens.active->cursorX == 9);
    CHECK(t.screens.active->cursorY == 1);
}

/// Starting outside the margins, the wrap still lands on the right margin of the
/// previous row.
static void testReverseWrapFromScreenEdgeToRightMargin()
{
    Ubiquitty::Terminal t;
    setUp(t);
    feed(t, "\033[?7h\033[?1045h\033[?69h\033[5;10s");
    feed(t, "\033[3;1H");
    feed(t, "\033[D");
    CHECK(t.screens.active->cursorX == 9);
    CHECK(t.screens.active->cursorY == 1);
}

/// A multi-cell CUB wraps and keeps going.
static void testReverseWrapMultiCell()
{
    Ubiquitty::Terminal t;
    setUp(t);
    feed(t, "\033[?7h");
    feed(t, "\033[1;79Habcd"); // 'c' wraps to row 2; cursor ends at (3,2)
    feed(t, "\033[?45h");
    feed(t, "\033[4D");
    CHECK(t.screens.active->cursorX == 78);
    CHECK(t.screens.active->cursorY == 0);
}

/// With reverse wrap on, the pending-wrap latch absorbs the whole step.
static void testReverseWrapConsumesPendingWrapLatch()
{
    Ubiquitty::Terminal t;
    setUp(t);
    feed(t, "\033[?7h\033[?45h");
    feed(t, "\033[1;79Hab"); // 'b' at column 80 leaves the wrap latched
    CHECK(t.screens.active->cursorX == 79);
    feed(t, "\033[D");
    CHECK(t.screens.active->cursorX == 79);
    CHECK(t.screens.active->cursorY == 0);
    feed(t, "X");
    CHECK(cell(t, 0, 78)->codePoint == 'a');
    CHECK(cell(t, 0, 79)->codePoint == 'X');
}

/// Without reverse wrap the latch is just dropped and the cursor moves one cell left.
static void testPendingWrapLatchWithoutReverseWrapMovesLeft()
{
    Ubiquitty::Terminal t;
    setUp(t);
    feed(t, "\033[?7h\033[?45l");
    feed(t, "\033[1;79Hab");
    feed(t, "\033[D");
    CHECK(t.screens.active->cursorX == 78);
    feed(t, "X");
    CHECK(cell(t, 0, 78)->codePoint == 'X');
    CHECK(cell(t, 0, 79)->codePoint == 'b');
}

/// ---- CNL / CPL ----

static void testCNLBegunBelowRegionRunsToLastRow()
{
    Ubiquitty::Terminal t;
    setUp(t);
    feed(t, "\033[4;5r\033[?69h\033[5;10s");
    feed(t, "\033[6;7H");
    feed(t, "\033[25E");
    CHECK(t.screens.active->cursorY == 24);
    CHECK(t.screens.active->cursorX == 4); // left margin, not column 1
}

static void testCNLBegunInsideStopsAtBottomMargin()
{
    Ubiquitty::Terminal t;
    setUp(t);
    feed(t, "\033[2;4r\033[?69h\033[5;10s");
    feed(t, "\033[3;7H");
    feed(t, "\033[99E");
    CHECK(t.screens.active->cursorY == 3);
    CHECK(t.screens.active->cursorX == 4);
}

static void testCPLBegunAboveRegionRunsToFirstRow()
{
    Ubiquitty::Terminal t;
    setUp(t);
    feed(t, "\033[4;5r\033[?69h\033[5;10s");
    feed(t, "\033[3;7H");
    feed(t, "\033[25F");
    CHECK(t.screens.active->cursorY == 0);
    CHECK(t.screens.active->cursorX == 4);
}

static void testCPLBegunInsideStopsAtTopMargin()
{
    Ubiquitty::Terminal t;
    setUp(t);
    feed(t, "\033[2;4r\033[?69h\033[5;10s");
    feed(t, "\033[3;7H");
    feed(t, "\033[99F");
    CHECK(t.screens.active->cursorY == 1);
    CHECK(t.screens.active->cursorX == 4);
}

/// Left of the left margin, CNL's carriage return lands on column 1.
static void testCNLLeftOfLeftMarginReturnsToColumnOne()
{
    Ubiquitty::Terminal t;
    setUp(t);
    feed(t, "\033[?69h\033[5;10s");
    feed(t, "\033[3;2H");
    feed(t, "\033[E");
    CHECK(t.screens.active->cursorX == 0);
    CHECK(t.screens.active->cursorY == 3);
}

/// An omitted Pn defaults to 1.
static void testCNLCPLWithoutMargins()
{
    Ubiquitty::Terminal t;
    setUp(t);
    feed(t, "\033[3;5H\033[E");
    CHECK(t.screens.active->cursorX == 0);
    CHECK(t.screens.active->cursorY == 3);
    feed(t, "\033[3;5H\033[2F");
    CHECK(t.screens.active->cursorX == 0);
    CHECK(t.screens.active->cursorY == 0);
}

/// ---- CBT / CHA ----

static void testCBTIgnoresLeftMargin()
{
    Ubiquitty::Terminal t;
    setUp(t);
    feed(t, "\033[?69h\033[5;30s");
    feed(t, "\033[9;7H");
    feed(t, "\033[2Z");
    CHECK(t.screens.active->cursorX == 0);
    CHECK(t.screens.active->cursorY == 8);
}

/// In origin mode the column is relative to the left margin.
static void testCHARespectsOriginMode()
{
    Ubiquitty::Terminal t;
    setUp(t);
    feed(t, "\033[6;11r\033[?69h\033[5;10s");
    feed(t, "\033[?6h"); // DECOM
    feed(t, "\033[1;2H"); // origin-relative → absolute row 6, column 6
    feed(t, "\033[1G"); // CHA 1 → the left margin
    CHECK(t.screens.active->cursorX == 4);
    CHECK(t.screens.active->cursorY == 5);
    feed(t, "X");
    CHECK(cell(t, 5, 4)->codePoint == 'X');
}

/// Without origin mode CHA is absolute and may land outside the margins.
static void testCHAIgnoresMarginsWithoutOriginMode()
{
    Ubiquitty::Terminal t;
    setUp(t);
    feed(t, "\033[?69h\033[5;10s");
    feed(t, "\033[3;5H\033[1G");
    CHECK(t.screens.active->cursorX == 0);
    CHECK(t.screens.active->cursorY == 2);
}

// ---- DECSC / DECRC ----

/// DECSC's saved set is origin mode, the DECSCA protection attribute and the
/// last-column (pending-wrap) flag. DECAWM is not in it, so DECRC must not put
/// it back.
static void testDECRCDoesNotRestoreDECAWM()
{
    Ubiquitty::Terminal t;
    setUp(t);
    feed(t, "\033[?7h");
    feed(t, "\0337");
    feed(t, "\033[?7l");
    feed(t, "\0338");
    CHECK(t.modes.autowrap == false);
    // Behaviourally: writing past the last column must not wrap.
    feed(t, "\033[1;79Habcd");
    CHECK(t.screens.active->cursorY == 0);
}

static void testDECRCRestoresPendingWrapLatch()
{
    Ubiquitty::Terminal t;
    setUp(t);
    feed(t, "\033[?7h");
    feed(t, "\033[1;80Ha"); // latch armed at the last column
    feed(t, "\0337");
    feed(t, "\033[5;5H");
    feed(t, "\0338");
    CHECK(t.screens.active->cursorX == 79);
    CHECK(t.screens.active->cursorY == 0);
    feed(t, "b"); // the restored latch wraps this one
    CHECK(cell(t, 1, 0)->codePoint == 'b');
}

// ---- Line-feed family vs. left/right margins ----

/// Shared fixture: 6 rows x 10 columns, DECSTBM 2..5, DECSLRM 3..8, an 'x'
/// sentinel at row 5 column 4 (inside the margin rectangle).
static void marginScrollFixture(Ubiquitty::Terminal& t)
{
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 6, 10);
    feed(t, "\033[5;4Hx");
    feed(t, "\033[2;5r\033[?69h\033[3;8s");
}

static void testLinefeedOutsideLeftRightMarginsDoesNotScroll()
{
    Ubiquitty::Terminal t;
    marginScrollFixture(t);
    feed(t, "\033[5;9H"); // bottom margin row, right of the right margin
    feed(t, "\n");
    CHECK(t.screens.active->cursorY == 4);
    CHECK(t.screens.active->cursorX == 8);
    CHECK(cell(t, 4, 3)->codePoint == 'x');

    feed(t, "\033[5;1H"); // bottom margin row, left of the left margin
    feed(t, "\n");
    CHECK(t.screens.active->cursorY == 4);
    CHECK(cell(t, 4, 3)->codePoint == 'x');

    feed(t, "\033[6;9H"); // below the region, at the last screen row
    feed(t, "\n");
    CHECK(t.screens.active->cursorY == 5);
    CHECK(cell(t, 4, 3)->codePoint == 'x');
}

/// The same feed from INSIDE the margins still scrolls the rectangle.
static void testLinefeedInsideLeftRightMarginsScrolls()
{
    Ubiquitty::Terminal t;
    marginScrollFixture(t);
    feed(t, "\033[5;4H");
    feed(t, "\n");
    CHECK(cell(t, 3, 3)->codePoint == 'x');
    CHECK(cell(t, 4, 3)->codePoint != 'x');
}

static void testReverseIndexOutsideLeftRightMarginsDoesNotScroll()
{
    Ubiquitty::Terminal t;
    marginScrollFixture(t);
    feed(t, "\033[2;9H"); // top margin row, right of the right margin
    feed(t, "\033M"); // RI
    CHECK(t.screens.active->cursorY == 1);
    CHECK(cell(t, 4, 3)->codePoint == 'x');

    feed(t, "\033[2;1H"); // top margin row, left of the left margin
    feed(t, "\033M");
    CHECK(t.screens.active->cursorY == 1);
    CHECK(cell(t, 4, 3)->codePoint == 'x');
}

static void testReverseIndexInsideLeftRightMarginsScrolls()
{
    Ubiquitty::Terminal t;
    marginScrollFixture(t);
    feed(t, "\033[2;4H");
    feed(t, "\033M");
    CHECK(cell(t, 4, 3)->codePoint != 'x');
    CHECK(cell(t, 5, 3)->codePoint != 'x'); // row 5 is below the region
    CHECK(cell(t, 3, 3)->codePoint == 0 || cell(t, 3, 3)->codePoint == ' ');
}

/// Unlike the line-feed family above, SU and SD are not suppressed by the cursor
/// sitting outside the margins.
static void testSuSdScrollWithCursorOutsideLeftRightMargins()
{
    Ubiquitty::Terminal t;
    marginScrollFixture(t);
    feed(t, "\033[5;9H");
    feed(t, "\033[1S");
    CHECK(cell(t, 3, 3)->codePoint == 'x');

    feed(t, "\033[1T");
    CHECK(cell(t, 4, 3)->codePoint == 'x');
}

int runCursorConformanceTests()
{
    testCUFBegunRightOfMarginRunsToScreenEdge();
    testCUFBegunInsideStopsAtRightMargin();
    testCUBBegunLeftOfMarginRunsToScreenEdge();
    testCUBOneLeftOfMarginMovesOneCell();
    testCUBBegunInsideStopsAtLeftMargin();

    testReverseWrapRequiresDECAWM();
    testReverseWrapToPreviousRow();
    testReverseWrapOffTopMarginLandsOnBottomMargin();
    testReverseWrapFromLeftMargin();
    testReverseWrapFromScreenEdgeToRightMargin();
    testReverseWrapMultiCell();
    testReverseWrapConsumesPendingWrapLatch();
    testPendingWrapLatchWithoutReverseWrapMovesLeft();

    testCNLBegunBelowRegionRunsToLastRow();
    testCNLBegunInsideStopsAtBottomMargin();
    testCPLBegunAboveRegionRunsToFirstRow();
    testCPLBegunInsideStopsAtTopMargin();
    testCNLLeftOfLeftMarginReturnsToColumnOne();
    testCNLCPLWithoutMargins();

    testCBTIgnoresLeftMargin();
    testCHARespectsOriginMode();
    testCHAIgnoresMarginsWithoutOriginMode();

    testDECRCDoesNotRestoreDECAWM();
    testDECRCRestoresPendingWrapLatch();

    testLinefeedOutsideLeftRightMarginsDoesNotScroll();
    testLinefeedInsideLeftRightMarginsScrolls();
    testReverseIndexOutsideLeftRightMarginsDoesNotScroll();
    testReverseIndexInsideLeftRightMarginsScrolls();
    testSuSdScrollWithCursorOutsideLeftRightMargins();

    if (failures)
        printf("test_conformance_cursor: %d FAILED\n", failures);
    else
        printf("test_conformance_cursor: all passed\n");
    return failures;
}

// Defined out when several cluster conformance files are linked into one
// binary; that driver supplies main and calls the run*Tests entry points.
#ifndef UBIQUITTY_CONFORMANCE_TESTS_COMBINED
int main() { return runCursorConformanceTests() == 0 ? 0 : 1; }
#endif
