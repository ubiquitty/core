// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// The single-cell ASCII placement, at every edge it declines to handle.
//
// One printable byte between two escapes cannot be batched, so it takes a
// placement of its own that answers the questions emitCharacter would answer
// and skips the frame emitCharacter needs to answer them. Every clause of its
// guard stands in for a branch, and a clause that admits a case it should have
// refused writes the wrong cell rather than failing — the suite would stay
// green and a glyph would land in the wrong place.
//
// So each case here drives one clause across its boundary and asserts the cell
// on both sides. Where the fast placement declines, the general path is what
// answers, and the answer has to be the same one it always gave.

#include "Harness.h"
#include "VT100Fixture.h"

#include "Core/CellGrid.h"
#include "Core/Terminal.h"

#include <string>

namespace {

std::string rowText(Ubiquitty::Terminal& t, int row, int count)
{
    std::string text;
    for (int column = 0; column < count; ++column) {
        const Ubiquitty::Cell* c = cell(t, row, column);
        const bool isPrintable = c && c->codePoint >= 0x20 && c->codePoint < 0x7F;
        text.push_back(isPrintable ? static_cast<char>(c->codePoint) : '.');
    }
    return text;
}

} // namespace

// One printable at a time. A bare string is a RUN, which the batched writer
// takes — the placement under test only ever sees a lone byte, so the tests
// have to hand it one.
static void feedSingly(Ubiquitty::Terminal& t, const char* text)
{
    for (const char* p = text; *p; ++p) {
        const char one[2] = { *p, '\0' };
        feed(t, "\033[m");
        feed(t, one);
    }
}

TEST(testPlainCellAtAndPastTheRightMargin)
{
    // The column before the margin is the fast placement's; the margin column
    // itself arms the deferred wrap and belongs to the general path. Both have
    // to land, and the wrap after them has to behave.
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 4, 8);
    feedSingly(t, "abcdefgh");
    CHECK(rowText(t, 0, 8) == "abcdefgh");
    CHECK(t.screens.active->wrapPending);
    CHECK(t.screens.active->cursorX == 7);
    // The next byte commits the wrap rather than overwriting the margin cell.
    feedSingly(t, "i");
    CHECK(rowText(t, 0, 8) == "abcdefgh");
    CHECK(rowText(t, 1, 8) == "i.......");
    CHECK(t.screens.active->cursorY == 1);
}

TEST(testPlainCellUnderDECSLRMMargins)
{
    // effectiveRight moves with the margins, so the column the fast placement
    // declines moves with them too.
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 4, 20);
    feed(t, "\033[?69h\033[5;12s"); // columns 5..12, one-based
    feed(t, "\033[1;5H");
    feedSingly(t, "abcdefgh"); // exactly fills 4..11 (zero-based)
    CHECK(rowText(t, 0, 20) == "....abcdefgh........");
    CHECK(t.screens.active->wrapPending);
    feedSingly(t, "i");
    CHECK(rowText(t, 1, 20) == "....i...............");
}

TEST(testPlainCellDeclinesADesignatedSet)
{
    // The byte is its own code point only while GL holds US-ASCII; a designated
    // set has to translate it.
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 4, 20);
    feed(t, "\033(0");
    feed(t, "\033[mq\033[mq"); // one glyph between escapes: the single-cell shape
    CHECK(cell(t, 0, 0)->codePoint == 0x2500);
    CHECK(cell(t, 0, 1)->codePoint == 0x2500);
    feed(t, "\033(B\033[mq");
    CHECK(cell(t, 0, 2)->codePoint == 'q');
}

TEST(testPlainCellDeclinesInsertMode)
{
    // IRM opens a cell before the glyph lands, pushing the tail right.
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 4, 8);
    feedSingly(t, "abcd");
    feed(t, "\033[1;1H\033[4h"); // home, insert mode
    feed(t, "\033[mX");
    CHECK(rowText(t, 0, 8) == "Xabcd...");
    feed(t, "\033[4l");
}

TEST(testPlainCellDeclinesAnOpenCluster)
{
    // A combining mark leaves a cluster open; the ASCII byte after it starts a
    // fresh cell rather than extending, and the mark stays on its base.
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 4, 8);
    feed(t, "e\xcc\x81"); // e + U+0301
    feed(t, "\033[mx");
    CHECK(cell(t, 0, 0)->codePoint == 'e');
    CHECK(cell(t, 0, 0)->combiningLength() == 1);
    CHECK(cell(t, 0, 1)->codePoint == 'x');
}

TEST(testPlainCellOverwritesAWidePair)
{
    // Landing on either half of a wide pair orphans the other, which the
    // placement has to blank.
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 4, 8);
    feed(t, "\xe4\xb8\xad"); // U+4E2D, wide
    CHECK(cell(t, 0, 0)->codePoint == 0x4E2D);
    feed(t, "\033[1;1H\033[mA");
    CHECK(cell(t, 0, 0)->codePoint == 'A');
    CHECK(cell(t, 0, 1)->codePoint == 0 || cell(t, 0, 1)->codePoint == ' ');
}

TEST(testPlainCellCarriesTheREPPredecessor)
{
    // The single-cell placement records the graphic REP repeats, so a REP after
    // one repeats that glyph rather than whatever came before it.
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 4, 12);
    feed(t, "\033[mZ\033[4b");
    CHECK(rowText(t, 0, 12) == "ZZZZZ.......");
}

TEST(testPlainCellFillsARowOneByteAtATime)
{
    // The placement's own lane: every column but the last is its, and the row
    // has to read the same as one written in a single run.
    Ubiquitty::Terminal singly;
    Ubiquitty::hardReset(singly);
    Ubiquitty::resize(singly, 4, 12);
    feedSingly(singly, "hello world!");

    Ubiquitty::Terminal batched;
    Ubiquitty::hardReset(batched);
    Ubiquitty::resize(batched, 4, 12);
    feed(batched, "hello world!");

    CHECK(rowText(singly, 0, 12) == "hello world!");
    CHECK(rowText(singly, 0, 12) == rowText(batched, 0, 12));
    CHECK(singly.screens.active->cursorX == batched.screens.active->cursorX);
    CHECK(singly.screens.active->cursorY == batched.screens.active->cursorY);
    CHECK(singly.screens.active->wrapPending == batched.screens.active->wrapPending);
}

UBIQUITTY_TEST_MAIN()
