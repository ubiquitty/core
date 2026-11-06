// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_block_edit_wiring — the editing controls reach the block scrubs.
//
// The scrub primitives have their own suite; this one proves the WIRING: a
// sized block planted in a live terminal, damaged through the wire by each
// control family, ends as absence — never a fragment, never a wrong slice.
// The blocks are planted directly (the OSC 66 producer is the arc's last
// commit), with their spec interned so the damage pass has a table to ask.

#include "Harness.h"
#include "TerminalFixture.h"

#include "Core/CellPen.h"
#include "Core/Screen/SizedBlock.h"
#include "Core/TerminalOperations.h"

using UbiquittyTest::feed;
using namespace Ubiquitty;

namespace {

Cell plainBlank()
{
    Cell cell {};
    cell.pen.foregroundIndex = colorDefault;
    cell.pen.backgroundIndex = colorDefault;
    cell.pen.underlineIndex = colorDefault;
    return cell;
}

Pen plainPen()
{
    Pen pen {};
    pen.foregroundIndex = colorDefault;
    pen.backgroundIndex = colorDefault;
    pen.underlineIndex = colorDefault;
    return pen;
}

// Interns a scale-3, width-2 spec and writes its six-column block at
// (row, column) of the live grid.
unsigned plantBlock(Terminal& t, int row, int column)
{
    TextSizeSpec spec;
    spec.scale = 3;
    spec.widthCells = 2;
    const unsigned specID = t.textSizeTable.intern(spec);
    Screen::writeSizedBlock(*t.grid, row, column, 6, 3, U'東', plainPen(), specID, plainBlank());
    return specID;
}

int sizedCellCount(const Terminal& t)
{
    int total = 0;
    for (int r = 0; r < t.rows; ++r)
        for (int c = 0; c < t.columns; ++c) {
            const Cell* cell = UbiquittyTest::cell(t, r, c);
            if (cell && cellPen(*cell).extendedAttributes.textSizeSpecID() != 0)
                ++total;
        }
    return total;
}

} // namespace

TEST(deleteLineThroughABlockErasesItWhole)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    plantBlock(t, 2, 4);
    CHECK_EQ(sizedCellCount(t), 18);
    feed(t, "\x1b[3;1H\x1b[M"); // DL on the block's middle row
    CHECK_EQ(sizedCellCount(t), 0);
}

TEST(insertLineAboveABlockSlidesItWholeAndStanding)
{
    // Full-width IL rotates rows whole: the block's three rows stay adjacent
    // one row down, still a valid block — moved, never torn, never erased.
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    plantBlock(t, 2, 4);
    feed(t, "\x1b[2;1H\x1b[L");
    CHECK_EQ(sizedCellCount(t), 18);
    const Cell* anchor = UbiquittyTest::cell(t, 3, 4);
    CHECK(anchor && cellPen(*anchor).attributes.contains(VT::Attribute::Wide));
    CHECK_EQ(static_cast<int>(penTextBand(&cellPen(*anchor))), 0);
}

TEST(eraseLineThroughABlockRowErasesItWhole)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    plantBlock(t, 1, 4);
    feed(t, "\x1b[2;1H\x1b[2K"); // EL whole-line on the middle row
    CHECK_EQ(sizedCellCount(t), 0);
}

TEST(characterDeleteInABlockRowErasesItWhole)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    plantBlock(t, 1, 4);
    feed(t, "\x1b[2;2H\x1b[2P"); // DCH shifts the middle row's chain left
    CHECK_EQ(sizedCellCount(t), 0);
}

TEST(aRectangleEraseOverOneRowErasesTheBlockWhole)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    plantBlock(t, 1, 4);
    feed(t, "\x1b[2;4;2;10$z"); // DECERA covering the middle row's chain
    CHECK_EQ(sizedCellCount(t), 0);
}

TEST(anEditBesideABlockLeavesItStanding)
{
    // The control the guard exists for: damage nowhere near the block, the
    // block stands whole — the scrubs judge, they do not blanket-erase.
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    plantBlock(t, 1, 4);
    feed(t, "\x1b[10;1Hplain text\x1b[10;1H\x1b[2P");
    feed(t, "\x1b[12;1H\x1b[2K");
    CHECK_EQ(sizedCellCount(t), 18);
}

TEST(scrollRightPastTheEdgeErasesTheBlockItTruncates)
{
    // SL/SR move each column independently. A block whose columns all stay
    // inside the shifted band moves whole; one against the grid's edge loses
    // the columns pushed past it, and what remains is a chain shorter than
    // its own claim — which goes, rather than standing as a truncated glyph.
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 8, 10);
    plantBlock(t, 1, 4); // six columns: 4..9, hard against the right edge
    CHECK_EQ(sizedCellCount(t), 18);
    feed(t, "\x1b[2 A"); // SR 2 — columns 8 and 9 have nowhere to go
    CHECK_EQ(sizedCellCount(t), 0);
}

TEST(scrollLeftPastTheEdgeErasesTheBlockItTruncates)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 8, 10);
    plantBlock(t, 1, 1); // six columns: 1..6, hard against the left edge
    feed(t, "\x1b[3 @"); // SL 3 — the anchor and two continuations fall off
    CHECK_EQ(sizedCellCount(t), 0);
}

TEST(aScrollThatMovesABlockWholeKeepsIt)
{
    // The other half of the same rule, and the guard against a blanket
    // erase: every column of the block stays inside the band, so it moves
    // intact and the scrub leaves it alone.
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    plantBlock(t, 1, 4); // default 80 columns: 4..9 with room to spare
    feed(t, "\x1b[2 A");
    CHECK_EQ(sizedCellCount(t), 18);
    const Cell* moved = UbiquittyTest::cell(t, 1, 6);
    CHECK(moved && cellPen(*moved).attributes.contains(VT::Attribute::Wide));
}

TEST(aScrollBesideNoRunLeavesThePlainTextItMoved)
{
    // The guard: the scrub judges, it does not blanket-erase what a shift
    // legitimately moved.
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\x1b[5;1Hplain");
    feed(t, "\x1b[2 A");
    const Cell* moved = UbiquittyTest::cell(t, 4, 2);
    CHECK(moved && moved->codePoint == 'p');
}

UBIQUITTY_TEST_MAIN()
