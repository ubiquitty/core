// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Screen/Addressing.h"
#include "Harness.h"
#include "TerminalFixture.h"

using UbiquittyTest::feed;

// test_grid_integration — end-to-end behavioral net for the grid/scrollback/
// reflow pipeline, driven entirely through the PUBLIC Core API
// (processBytes / resize / visibleCell / absoluteCell / rowMark /
// rowLineAttribute / scrollbackCount). Storage-agnostic on purpose: it pins
// observable behavior so the Tier-2 storage rewrite (LineStore, copy-free
// scroll-off, reflow-over-store) cannot regress what the user sees —
// content, DECDWL/DECDHL geometry attributes, OSC-133 marks, soft-wrap joins —
// across scroll-into-history, width reflow, and height resize round-trips.

#include "Core/CellGrid.h"
#include "Core/TerminalOperations.h"

#include <cstdio>
#include <cstring>

using Ubiquitty::processBytes;
using Ubiquitty::resize;
using Ubiquitty::Screen::absoluteCell;
using Ubiquitty::Screen::rowLineAttribute;
using Ubiquitty::Screen::rowMark;
using Ubiquitty::Screen::scrollbackCount;
using Ubiquitty::Screen::visibleCell;

// Absolute-row cell char via absoluteCell (history+live unified space).
static uint32_t absCh(Ubiquitty::Terminal& t, int absoluteRow, int column)
{
    const Ubiquitty::Cell* c = absoluteCell(&t, absoluteRow, column);
    return c ? c->codePoint : 0;
}

// ---- content survives scroll into history -------------------------------
TEST(testScrollIntoHistoryPreservesContent)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    resize(t, 4, 20);
    // 10 distinct hard-broken lines; rows=4 so 6 age into history.
    for (int i = 0; i < 10; ++i) {
        char line[32];
        std::snprintf(line, sizeof(line), "L%d-XYZ\r\n", i);
        feed(t, line);
    }
    const int hist = scrollbackCount(&t);
    CHECK(hist >= 6);
    // Oldest absolute row 0 must be "L0-XYZ".
    CHECK(absCh(t, 0, 0) == 'L');
    CHECK(absCh(t, 0, 1) == '0');
    // Row for L5 (absolute 5) intact.
    CHECK(absCh(t, 5, 0) == 'L');
    CHECK(absCh(t, 5, 1) == '5');
}

// ---- DECDWL/DECDHL survive into history ---------------------------------
TEST(testDECDWLSurvivesIntoHistory)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    resize(t, 4, 20);
    // Row 0: DECDWL (ESC#6) banner, then push it into history.
    feed(t, "\x1b#6BANNER\r\n");
    for (int i = 0; i < 8; ++i)
        feed(t, "x\r\n");
    const int hist = scrollbackCount(&t);
    CHECK(hist >= 1);
    // The banner aged to absolute row 0; its line attribute must read DOUBLE_WIDTH
    // from the history record, not the live row.
    CHECK(rowLineAttribute(&t, 0) == Ubiquitty::lineAttributeDoubleWidth);
    CHECK(absCh(t, 0, 0) == 'B');
}

// ---- OSC-133 prompt mark rides its row into history ---------------------
TEST(testOSC133MarkRidesIntoHistory)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    resize(t, 4, 20);
    feed(t, "\x1b]133;A\x07prompt\r\n");
    for (int i = 0; i < 8; ++i)
        feed(t, "y\r\n");
    CHECK(scrollbackCount(&t) >= 1);
    CHECK(rowMark(&t, 0) & Ubiquitty::Screen::osc133MarkPromptStart);
}

// ---- width reflow: wrapped line resplits, content preserved -------------
TEST(testWidthReflowRoundtripPreservesContent)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    resize(t, 6, 20);
    // One 30-column logical line auto-wraps at 20; narrow to 10 then back to 20.
    feed(t, "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123\r\n");
    resize(t, 6, 10);
    resize(t, 6, 20);
    // After the round trip the logical content must still read ABC...3 across
    // the top visible rows (offset 0). Concatenate row 0 + row 1 of the live
    // screen up to the original 30 chars.
    char got[40];
    int n = 0;
    for (int r = 0; r < 6 && n < 30; ++r)
        for (int c = 0; c < 20 && n < 30; ++c) {
            const Ubiquitty::Cell* cell = visibleCell(&t, 0, r, c);
            if (cell && cell->codePoint)
                got[n++] = (char)cell->codePoint;
        }
    got[n] = 0;
    CHECK(std::strncmp(got, "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123", 30) == 0);
}

// ---- height shrink then grow: content + count round-trip ----------------
TEST(testHeightResizeRoundtrip)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    resize(t, 10, 20);
    for (int i = 0; i < 6; ++i) {
        char line[32];
        std::snprintf(line, sizeof(line), "row%d\r\n", i);
        feed(t, line);
    }
    // Shrink (pushes top rows into history) then grow back.
    resize(t, 4, 20);
    resize(t, 10, 20);
    // row0..row5 content must still be reachable in absolute space, in order.
    const int total = scrollbackCount(&t) + 10;
    int found = -1;
    for (int a = 0; a < total; ++a)
        if (absCh(t, a, 0) == 'r' && absCh(t, a, 3) == '0') {
            found = a;
            break;
        }
    CHECK(found >= 0);
    if (found >= 0) {
        // The five rows after row0 are row1..row5 in order.
        for (int k = 1; k <= 5; ++k)
            CHECK(absCh(t, found + k, 3) == static_cast<uint32_t>(('0' + k)));
    }
}

// ---- clear (ED 3 / Cmd-K-ish) drops history -----------------------------
TEST(testClearScrollbackDropsHistory)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    resize(t, 4, 20);
    for (int i = 0; i < 8; ++i)
        feed(t, "z\r\n");
    CHECK(scrollbackCount(&t) >= 1);
    feed(t, "\x1b[3J"); // Erase Saved Lines
    CHECK(scrollbackCount(&t) == 0);
}

// ---- Cmd+K (clearBuffer) drops history for good; a later scroll must not
// resurrect it. Zeroing scrollbackCount without clearing the LineStore lets
// the next scrolled line reset the count back to the store's uncleared total.
TEST(testClearBufferDoesNotResurrectHistory)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    resize(t, 4, 20);
    for (int i = 0; i < 12; ++i)
        feed(t, "line\r\n");
    CHECK(scrollbackCount(&t) >= 1);
    Ubiquitty::clearBuffer(&t);
    CHECK(scrollbackCount(&t) == 0);
    feed(t, "new\r\n"); // one line scrolls off post-clear
    CHECK(scrollbackCount(&t) <= 1); // not the resurrected ~8
}

// ---- alt screen does not populate scrollback ----------------------------
TEST(testAlternateScreenNoScrollback)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    resize(t, 4, 20);
    feed(t, "\x1b[?1049h"); // enter alt
    for (int i = 0; i < 8; ++i)
        feed(t, "q\r\n");
    CHECK(scrollbackCount(&t) == 0);
    feed(t, "\x1b[?1049l"); // leave alt
}

// ---- soft-wrap join survives the scrollback boundary --------------------
TEST(testSoftwrapJoinIntoHistory)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    resize(t, 4, 10);
    // 15 columns of content wraps across two physical rows (soft join), then
    // gets pushed into history; a narrow->wide reflow must rejoin it.
    feed(t, "0123456789ABCDE\r\n");
    for (int i = 0; i < 6; ++i)
        feed(t, ".\r\n");
    resize(t, 4, 20); // widen: the wrapped pair should rejoin
    // Find the rejoined line in absolute space and confirm it reads contiguously.
    const int total = scrollbackCount(&t) + 4;
    int found = -1;
    for (int a = 0; a < total; ++a)
        if (absCh(t, a, 0) == '0' && absCh(t, a, 9) == '9') {
            found = a;
            break;
        }
    CHECK(found >= 0);
    if (found >= 0)
        for (int c = 10; c < 15; ++c)
            CHECK(absCh(t, found, c) == static_cast<uint32_t>(("ABCDE"[c - 10])));
}

UBIQUITTY_TEST_MAIN()
