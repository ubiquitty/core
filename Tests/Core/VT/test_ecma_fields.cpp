// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"
#include "VT100Fixture.h"

#include <cstring>

TEST(testTabStopRemovePreservesOtherStopsAndCursor)
{
    Ubiquitty::Terminal terminal;
    Ubiquitty::hardReset(terminal);
    Ubiquitty::resize(terminal, 3, 24);
    feed(terminal, "\033[9 d\tX");
    CHECK_EQ(cell(terminal, 0, 16)->codePoint, static_cast<uint32_t>('X'));
    CHECK(!terminal.tabStops[8]);
    CHECK(terminal.tabStops[16]);
    feed(terminal, "\033[1;4H\033H\033[24GZ\033[4 d");
    CHECK(!terminal.tabStops[3]);
    CHECK_EQ(terminal.screens.active->cursorX, 23);
    CHECK(terminal.screens.active->wrapPending);
    feed(terminal, "\033[ d\033[0 d\033[999 d\033[16  d\033[?16 d");
    CHECK(terminal.tabStops[16]);
    feed(terminal, "\033[16 d\033c");
    CHECK(terminal.tabStops[8]);
    CHECK(terminal.tabStops[16]);
}

TEST(testQualifiedGuardedAreasSurviveErasure)
{
    for (const char* erase : { "\r\033[K", "\r\033[6X", "\r\033[J" }) {
        Ubiquitty::Terminal terminal;
        Ubiquitty::hardReset(terminal);
        Ubiquitty::resize(terminal, 2, 6);
        feed(terminal, "\033[1;1o\033[1mAB\033[0m\033[0oCD");
        feed(terminal, erase);
        CHECK_EQ(cell(terminal, 0, 0)->codePoint, static_cast<uint32_t>('A'));
        CHECK_EQ(cell(terminal, 0, 1)->codePoint, static_cast<uint32_t>('B'));
        CHECK(Ubiquitty::cellPen(*cell(terminal, 0, 0)).attributes.contains(Ubiquitty::VT::Attribute::Bold));
        CHECK_EQ(cell(terminal, 0, 2)->codePoint, 0u);
        CHECK_EQ(cell(terminal, 0, 3)->codePoint, 0u);
    }
}

TEST(testQualifiedAreaDefaultAndUnguardedSelectors)
{
    for (const char* unguard : { "\033[o", "\033[0o", "\033[1;0o", "\033[3o", "\033[1;3;0o" }) {
        Ubiquitty::Terminal terminal;
        Ubiquitty::hardReset(terminal);
        feed(terminal, "\033[1oA");
        feed(terminal, unguard);
        feed(terminal, "B\r\033[K");
        CHECK_EQ(cell(terminal, 0, 0)->codePoint, static_cast<uint32_t>('A'));
        CHECK_EQ(cell(terminal, 0, 1)->codePoint, 0u);
    }
}

TEST(testQualifiedAreaProtectedAndUnguardedSelectorStillProtectsErasure)
{
    for (const char* erase : { "\r\033[K", "\r\033[6X", "\r\033[J" }) {
        Ubiquitty::Terminal terminal;
        Ubiquitty::hardReset(terminal);
        Ubiquitty::resize(terminal, 2, 6);
        feed(terminal, "\033[1oAB\033[8oCD");
        feed(terminal, erase);
        CHECK_EQ(cell(terminal, 0, 0)->codePoint, static_cast<uint32_t>('A'));
        CHECK_EQ(cell(terminal, 0, 1)->codePoint, static_cast<uint32_t>('B'));
        CHECK_EQ(cell(terminal, 0, 2)->codePoint, static_cast<uint32_t>('C'));
        CHECK_EQ(cell(terminal, 0, 3)->codePoint, static_cast<uint32_t>('D'));
        CHECK_EQ(cell(terminal, 0, 4)->codePoint, 0u);
    }
}

TEST(testErasureModeAllErasesGuardedAreas)
{
    for (const char* erase : { "\r\033[K", "\r\033[6X", "\033[2J", "\033[H\033[J" }) {
        Ubiquitty::Terminal terminal;
        Ubiquitty::hardReset(terminal);
        Ubiquitty::resize(terminal, 2, 6);
        feed(terminal, "a\033VB\033Wc\033[6h");
        feed(terminal, erase);
        for (int column = 0; column < 6; ++column)
            CHECK_EQ(cell(terminal, 0, column)->codePoint, 0u);
    }
}

TEST(testErasureModeResetKeepsGuardedAreasAgain)
{
    Ubiquitty::Terminal terminal;
    Ubiquitty::hardReset(terminal);
    Ubiquitty::resize(terminal, 2, 6);
    feed(terminal, "a\033VB\033Wc\033[6h\033[6l\033[2J");
    CHECK_EQ(cell(terminal, 0, 0)->codePoint, 0u);
    CHECK_EQ(cell(terminal, 0, 1)->codePoint, static_cast<uint32_t>('B'));
    CHECK_EQ(cell(terminal, 0, 2)->codePoint, 0u);
}

TEST(testSoftResetReturnsErasureModeToProtect)
{
    Ubiquitty::Terminal terminal;
    Ubiquitty::hardReset(terminal);
    Ubiquitty::resize(terminal, 2, 6);
    feed(terminal, "\033[6h\033[!p\033VB\033W\r\033[2K");
    CHECK_EQ(cell(terminal, 0, 0)->codePoint, static_cast<uint32_t>('B'));
}

TEST(testErasureModeIsStillUnrecognizedByDECRQM)
{
    Ubiquitty::Terminal terminal;
    Ubiquitty::hardReset(terminal);
    feed(terminal, "\033[6h\033[6$p");
    char reply[64];
    drain(terminal, reply);
    CHECK(std::strstr(reply, "\033[6;0$y") != nullptr);
}

TEST(testQualifiedAreaAfterTheLastColumnBeginsOnTheNextRow)
{
    Ubiquitty::Terminal terminal;
    Ubiquitty::hardReset(terminal);
    Ubiquitty::resize(terminal, 3, 4);
    feed(terminal, "\033[0oABCD\033[0oEFGH\033[1;2H\033[2O");
    // The second area begins where the next character goes, not on D.
    CHECK_EQ(cell(terminal, 0, 3)->codePoint, 0u);
    CHECK_EQ(cell(terminal, 1, 0)->codePoint, static_cast<uint32_t>('E'));
}

TEST(testEraseInAreaErasesFromPositionToTheAreaEnd)
{
    Ubiquitty::Terminal terminal;
    Ubiquitty::hardReset(terminal);
    Ubiquitty::resize(terminal, 3, 6);
    feed(terminal, "\033[0oABCDEF\033[0oGHIJKL\033[1;3H\033[0O");
    // The first area runs from A through F: EA 0 keeps AB, erases CDEF, and leaves GHIJKL.
    CHECK_EQ(cell(terminal, 0, 1)->codePoint, static_cast<uint32_t>('B'));
    CHECK_EQ(cell(terminal, 0, 2)->codePoint, 0u);
    CHECK_EQ(cell(terminal, 0, 5)->codePoint, 0u);
    CHECK_EQ(cell(terminal, 1, 0)->codePoint, static_cast<uint32_t>('G'));
    CHECK_EQ(terminal.screens.active->cursorX, 2);
    CHECK_EQ(terminal.screens.active->cursorY, 0);
}

TEST(testEraseInAreaErasesFromTheAreaStartThroughThePosition)
{
    Ubiquitty::Terminal terminal;
    Ubiquitty::hardReset(terminal);
    Ubiquitty::resize(terminal, 3, 6);
    feed(terminal, "\033[0oABCDEF\033[0oGHIJKL\033[2;3H\033[1O");
    CHECK_EQ(cell(terminal, 0, 5)->codePoint, static_cast<uint32_t>('F'));
    CHECK_EQ(cell(terminal, 1, 0)->codePoint, 0u);
    CHECK_EQ(cell(terminal, 1, 2)->codePoint, 0u);
    CHECK_EQ(cell(terminal, 1, 3)->codePoint, static_cast<uint32_t>('J'));
}

TEST(testEraseInAreaErasesTheWholeAreaAcrossRows)
{
    Ubiquitty::Terminal terminal;
    Ubiquitty::hardReset(terminal);
    Ubiquitty::resize(terminal, 4, 4);
    feed(terminal, "AB\033[2;3H\033[0oCDEFGHIJ\033[3;1H\033[2O");
    CHECK_EQ(cell(terminal, 0, 0)->codePoint, static_cast<uint32_t>('A'));
    CHECK_EQ(cell(terminal, 1, 1)->codePoint, 0u);
    CHECK_EQ(cell(terminal, 1, 2)->codePoint, 0u);
    CHECK_EQ(cell(terminal, 2, 0)->codePoint, 0u);
    CHECK_EQ(cell(terminal, 3, 3)->codePoint, 0u);
}

TEST(testEraseInAreaDoesNothingWithoutAnAreaAtOrBeforeThePosition)
{
    Ubiquitty::Terminal terminal;
    Ubiquitty::hardReset(terminal);
    Ubiquitty::resize(terminal, 2, 6);
    feed(terminal, "ABCDEF\033[1;3H\033[2O");
    CHECK_EQ(cell(terminal, 0, 2)->codePoint, static_cast<uint32_t>('C'));
    feed(terminal, "\033[1;4H\033[0o\033[1;2H\033[2O");
    CHECK_EQ(cell(terminal, 0, 5)->codePoint, static_cast<uint32_t>('F'));
}

TEST(testEraseInAreaKeepsAProtectedAreaUntilErasureModeIsSet)
{
    Ubiquitty::Terminal terminal;
    Ubiquitty::hardReset(terminal);
    Ubiquitty::resize(terminal, 2, 5);
    feed(terminal, "\033[1oABCDE\033[1;3H\033[2O");
    CHECK_EQ(cell(terminal, 0, 0)->codePoint, static_cast<uint32_t>('A'));
    CHECK_EQ(cell(terminal, 0, 4)->codePoint, static_cast<uint32_t>('E'));
    feed(terminal, "\033[6h\033[2O");
    CHECK_EQ(cell(terminal, 0, 0)->codePoint, 0u);
    CHECK_EQ(cell(terminal, 0, 4)->codePoint, 0u);
}

TEST(testEraseInAreaSparesGuardedCellsInAnUnprotectedArea)
{
    Ubiquitty::Terminal terminal;
    Ubiquitty::hardReset(terminal);
    Ubiquitty::resize(terminal, 2, 5);
    feed(terminal, "\033[0oA\033VB\033WC\033[1;1H\033[2O");
    CHECK_EQ(cell(terminal, 0, 0)->codePoint, 0u);
    CHECK_EQ(cell(terminal, 0, 1)->codePoint, static_cast<uint32_t>('B'));
    CHECK_EQ(cell(terminal, 0, 2)->codePoint, 0u);
}

TEST(testEraseInAreaIgnoresUndefinedSelectors)
{
    Ubiquitty::Terminal terminal;
    Ubiquitty::hardReset(terminal);
    Ubiquitty::resize(terminal, 2, 5);
    feed(terminal, "\033[0oABC\033[1;1H\033[3O\033[99O");
    CHECK_EQ(cell(terminal, 0, 0)->codePoint, static_cast<uint32_t>('A'));
}

TEST(testQualifiedAreasFollowScrolledRows)
{
    Ubiquitty::Terminal terminal;
    Ubiquitty::hardReset(terminal);
    Ubiquitty::resize(terminal, 3, 4);
    feed(terminal, "\033[2;1H\033[0oWXYZ\033[3;1H\033[0oQRST\033[1S");
    // Scrolling up moved the areas from rows 2 and 3 to rows 1 and 2.
    CHECK_EQ(cell(terminal, 0, 0)->codePoint, static_cast<uint32_t>('W'));
    feed(terminal, "\033[1;1H\033[2O");
    CHECK_EQ(cell(terminal, 0, 0)->codePoint, 0u);
    CHECK_EQ(cell(terminal, 0, 3)->codePoint, 0u);
    CHECK_EQ(cell(terminal, 1, 0)->codePoint, static_cast<uint32_t>('Q'));
}

TEST(testQualifiedAreaScrolledOffThePageEndsItsErasure)
{
    Ubiquitty::Terminal terminal;
    Ubiquitty::hardReset(terminal);
    Ubiquitty::resize(terminal, 2, 4);
    feed(terminal, "\033[0oAB\r\nCD\r\nEF\033[1;1H\033[2O");
    CHECK_EQ(cell(terminal, 0, 0)->codePoint, static_cast<uint32_t>('C'));
}

TEST(testQualifiedAreasFollowInsertedLines)
{
    Ubiquitty::Terminal terminal;
    Ubiquitty::hardReset(terminal);
    Ubiquitty::resize(terminal, 4, 4);
    feed(terminal, "\033[2;1H\033[0oWXYZ\033[1;1H\033[1L\033[3;1H\033[2O");
    CHECK_EQ(cell(terminal, 2, 0)->codePoint, 0u);
    CHECK_EQ(cell(terminal, 2, 3)->codePoint, 0u);
}

TEST(testClearingThePageForgetsQualifiedAreas)
{
    Ubiquitty::Terminal terminal;
    Ubiquitty::hardReset(terminal);
    Ubiquitty::resize(terminal, 2, 4);
    feed(terminal, "\033[0o\033[2J\033[1;1HABCD\033[1;1H\033[2O");
    CHECK_EQ(cell(terminal, 0, 0)->codePoint, static_cast<uint32_t>('A'));
}

TEST(testLineHomeAppliesToReturnsAndLineEditing)
{
    for (const char* control : { "\r", "\033E", "\033[L", "\033[M", "\033[E", "\033[F", "\033[20h\n" }) {
        Ubiquitty::Terminal terminal;
        Ubiquitty::hardReset(terminal);
        Ubiquitty::resize(terminal, 4, 8);
        feed(terminal, "\033[3 U\033[2;6H");
        feed(terminal, control);
        CHECK_EQ(terminal.screens.active->cursorX, 2);
        feed(terminal, "X");
        CHECK_EQ(cell(terminal, terminal.screens.active->cursorY, 2)->codePoint, static_cast<uint32_t>('X'));
    }
}

TEST(testLineHomePersistsAcrossWritesAndWraps)
{
    Ubiquitty::Terminal terminal;
    Ubiquitty::hardReset(terminal);
    Ubiquitty::resize(terminal, 3, 6);
    feed(terminal, "\033[3 UABCDEF");
    feed(terminal, "G");
    CHECK_EQ(cell(terminal, 1, 2)->codePoint, static_cast<uint32_t>('G'));
    CHECK_EQ(terminal.screens.active->cursorX, 3);
    feed(terminal, "\033[ U\033[0 U\033[999 U\r");
    CHECK_EQ(terminal.screens.active->cursorX, 2);
    feed(terminal, "\033c\r");
    CHECK_EQ(terminal.screens.active->cursorX, 0);
}

TEST(testLineHomeRespectsWidthAndLeftMargin)
{
    Ubiquitty::Terminal terminal;
    Ubiquitty::hardReset(terminal);
    Ubiquitty::resize(terminal, 3, 12);
    feed(terminal, "\033[5 U\033[?69h\033[7;11s\033[8G\r");
    CHECK_EQ(terminal.screens.active->cursorX, 6);
    feed(terminal, "\033[2G\r");
    CHECK_EQ(terminal.screens.active->cursorX, 4);
    feed(terminal, "\033[?69l\033[12 U\033#6\r");
    CHECK_EQ(terminal.screens.active->cursorX, 5);
    Ubiquitty::resize(terminal, 3, 4);
    feed(terminal, "\r");
    CHECK(terminal.screens.active->cursorX < 4);
}

static std::string rowText(Ubiquitty::Terminal& terminal, int row)
{
    std::string text;
    for (int column = 0; column < terminal.columns; ++column) {
        const uint32_t codePoint = cell(terminal, row, column)->codePoint;
        text.push_back(codePoint == 0 ? '_' : static_cast<char>(codePoint));
    }
    return text;
}

#define CHECK_ROW(terminal, row, expected)                   \
    do {                                                     \
        const std::string rowText_ = rowText(terminal, row); \
        CHECK_CSTR_EQ(rowText_.c_str(), expected);           \
    } while (false)

TEST(testCarriageReturnAndNextLineMoveToTheLineLimitWhenSimdIsOne)
{
    for (const char* control : { "\r", "\033E", "\xC2\x85" }) {
        Ubiquitty::Terminal terminal;
        Ubiquitty::hardReset(terminal);
        Ubiquitty::resize(terminal, 4, 8);
        feed(terminal, "\033[9l\033[1^\033[5 V\033[2;3H");
        const int row = terminal.screens.active->cursorY;
        feed(terminal, control);
        CHECK_EQ(terminal.screens.active->cursorX, 4);
        CHECK_EQ(terminal.screens.active->cursorY, std::string(control) == "\r" ? row : row + 1);
        feed(terminal, "X");
        CHECK_EQ(cell(terminal, terminal.screens.active->cursorY, 4)->codePoint, static_cast<uint32_t>('X'));
    }
}

TEST(testLineFeedUnderNewLineModeMovesToTheLineLimitWhenSimdIsOne)
{
    for (const char* control : { "\n", "\v", "\f" }) {
        Ubiquitty::Terminal terminal;
        Ubiquitty::hardReset(terminal);
        Ubiquitty::resize(terminal, 4, 8);
        feed(terminal, "\033[1^\033[5 V\033[20h\033[2;3H");
        feed(terminal, control);
        CHECK_EQ(terminal.screens.active->cursorY, 2);
        CHECK_EQ(terminal.screens.active->cursorX, 4);
        feed(terminal, "\033[0^\033[2;3H");
        feed(terminal, control);
        CHECK_EQ(terminal.screens.active->cursorX, 0);
    }
}

TEST(testSimdZeroReturnsToTheLineHomeAndTheLimitSurvivesIt)
{
    Ubiquitty::Terminal terminal;
    Ubiquitty::hardReset(terminal);
    Ubiquitty::resize(terminal, 3, 8);
    feed(terminal, "\033[3 U\033[6 V\033[1^\r");
    CHECK_EQ(terminal.screens.active->cursorX, 5);
    feed(terminal, "\033[0^\r");
    CHECK_EQ(terminal.screens.active->cursorX, 2);
    feed(terminal, "\033[^\r");
    CHECK_EQ(terminal.screens.active->cursorX, 2);
    feed(terminal, "\033[1^\r");
    CHECK_EQ(terminal.screens.active->cursorX, 5);
}

TEST(testLineLimitDefaultsToTheEdgeAndStaysInsideTheMargins)
{
    Ubiquitty::Terminal terminal;
    Ubiquitty::hardReset(terminal);
    Ubiquitty::resize(terminal, 3, 12);
    feed(terminal, "\033[1^\r");
    CHECK_EQ(terminal.screens.active->cursorX, 11);
    feed(terminal, "\033[?69h\033[3;8s\033[5G\r");
    CHECK_EQ(terminal.screens.active->cursorX, 7);
    feed(terminal, "\033[10 V\033[5G\r");
    CHECK_EQ(terminal.screens.active->cursorX, 7);
    feed(terminal, "\033[11G\r");
    CHECK_EQ(terminal.screens.active->cursorX, 9);
    feed(terminal, "\033[?69l\033#6\033[12 V\r");
    CHECK_EQ(terminal.screens.active->cursorX, 5);
}

TEST(testLineLimitAndSimdIgnoreWhatTheyCannotTake)
{
    Ubiquitty::Terminal terminal;
    Ubiquitty::hardReset(terminal);
    Ubiquitty::resize(terminal, 3, 8);
    feed(terminal, "\033[1^\033[5 V\033[ V\033[0 V\033[99 V\033[2;3 V\033[?3 V\033[3  V\r");
    CHECK_EQ(terminal.screens.active->cursorX, 4);
    feed(terminal, "\033[2^\033[9^\033[1;0^\033[?0^\033[0!^\033[0 ^\r");
    CHECK_EQ(terminal.screens.active->cursorX, 4);
    feed(terminal, "\033[^\r");
    CHECK_EQ(terminal.screens.active->cursorX, 0);
}

TEST(testResetToInitialStateClearsTheLineLimitAndTheDirection)
{
    Ubiquitty::Terminal terminal;
    Ubiquitty::hardReset(terminal);
    Ubiquitty::resize(terminal, 3, 8);
    feed(terminal, "\033[1^\033[5 V\033c\r");
    CHECK_EQ(terminal.screens.active->cursorX, 0);
    feed(terminal, "\033[1^\r");
    CHECK_EQ(terminal.screens.active->cursorX, 7);
}

TEST(testSelectPresentationDirectionsMovesToTheFirstPositionForPs2OneAndTwo)
{
    for (const char* sequence : { "\033[0;1 S", "\033[0;2 S", "\033[3;1 S", "\033[7;2 S", "\033[;2 S" }) {
        Ubiquitty::Terminal terminal;
        Ubiquitty::hardReset(terminal);
        Ubiquitty::resize(terminal, 3, 6);
        feed(terminal, "AB\033[3;4H");
        feed(terminal, sequence);
        CHECK_EQ(terminal.screens.active->cursorX, 0);
        CHECK_EQ(terminal.screens.active->cursorY, 0);
        feed(terminal, "CD");
        CHECK_ROW(terminal, 0, "CD____");
    }
}

TEST(testSelectPresentationDirectionsLeavesThePositionWhenItIsNotAskedToUpdate)
{
    for (const char* sequence : { "\033[ S", "\033[0 S", "\033[0;0 S", "\033[1;3 S", "\033[7;7 S", "\033[99;1 S", "\033[0;1;2 S", "\033[?0;1 S", "\033[0;1  S" }) {
        Ubiquitty::Terminal terminal;
        Ubiquitty::hardReset(terminal);
        Ubiquitty::resize(terminal, 3, 6);
        feed(terminal, "AB\033[3;4H");
        feed(terminal, sequence);
        CHECK_EQ(terminal.screens.active->cursorX, 3);
        CHECK_EQ(terminal.screens.active->cursorY, 2);
    }
}

TEST(testSelectPresentationDirectionsClearsAPendingWrap)
{
    Ubiquitty::Terminal terminal;
    Ubiquitty::hardReset(terminal);
    Ubiquitty::resize(terminal, 3, 4);
    feed(terminal, "ABCD");
    CHECK(terminal.screens.active->wrapPending);
    feed(terminal, "\033[0;2 SX");
    CHECK_ROW(terminal, 0, "XBCD");
    CHECK_EQ(terminal.screens.active->cursorX, 1);
}

TEST(testQuadFlushesAStringToTheLineLimitAndCentresIt)
{
    Ubiquitty::Terminal terminal;
    Ubiquitty::hardReset(terminal);
    Ubiquitty::resize(terminal, 4, 5);
    feed(terminal, "\033[5 VAB\033[4 H");
    CHECK_ROW(terminal, 0, "___AB");
    CHECK_EQ(terminal.screens.active->cursorX, 4);
    CHECK(terminal.screens.active->wrapPending);
    feed(terminal, "\033[2;1fABC\033[2 H");
    CHECK_ROW(terminal, 1, "_ABC_");
    CHECK_EQ(terminal.screens.active->cursorX, 4);
    CHECK(!terminal.screens.active->wrapPending);
    feed(terminal, "\033[3;1fAB\033[2 H");
    CHECK_ROW(terminal, 2, "_AB__");
    feed(terminal, "\033[4;1fABCDE\033[4 H");
    CHECK_ROW(terminal, 3, "ABCDE");
    CHECK(terminal.screens.active->wrapPending);
}

TEST(testQuadFlushesAStringToTheLineHomeFromWhereItBegan)
{
    Ubiquitty::Terminal terminal;
    Ubiquitty::hardReset(terminal);
    Ubiquitty::resize(terminal, 3, 8);
    feed(terminal, "\033[3 U\033[1;5fAB\033[0 H");
    CHECK_ROW(terminal, 0, "__AB____");
    CHECK_EQ(terminal.screens.active->cursorX, 4);
    feed(terminal, "\033[2;6fAB\033[ H");
    CHECK_ROW(terminal, 1, "__AB____");
}

TEST(testQuadStartsTheNextStringWhereTheLastOneEnded)
{
    Ubiquitty::Terminal terminal;
    Ubiquitty::hardReset(terminal);
    Ubiquitty::resize(terminal, 2, 8);
    feed(terminal, "\033[8 VAB\033[0 HCD\033[4 H");
    CHECK_ROW(terminal, 0, "AB____CD");
    feed(terminal, "\033[2;1fEF\033[2 H");
    CHECK_ROW(terminal, 1, "___EF___");
}

TEST(testQuadAfterAQuadAtTheEdgeStartsTheNextStringPastIt)
{
    Ubiquitty::Terminal terminal;
    Ubiquitty::hardReset(terminal);
    Ubiquitty::resize(terminal, 3, 6);
    feed(terminal, "\033[6 V\033[1;5fAB\033[4 H");
    CHECK_ROW(terminal, 0, "____AB");
    feed(terminal, "\033[0 H");
    CHECK_ROW(terminal, 0, "____AB");
    CHECK_EQ(terminal.screens.active->cursorX, 5);
    feed(terminal, "\033[2;1fCD\033[4 H\033[2 H");
    CHECK_ROW(terminal, 1, "____CD");
}

TEST(testQuadActsOnTheWrappedPartOfAStringOnly)
{
    Ubiquitty::Terminal terminal;
    Ubiquitty::hardReset(terminal);
    Ubiquitty::resize(terminal, 3, 6);
    feed(terminal, "\033[6 V\033[1;5fABCDEFG\033[4 H");
    CHECK_ROW(terminal, 0, "____AB");
    CHECK_ROW(terminal, 1, "_CDEFG");
    CHECK_EQ(terminal.screens.active->cursorY, 1);
    CHECK_EQ(terminal.screens.active->cursorX, 5);
}

TEST(testQuadStringsBeginAtEveryFormator)
{
    struct Formator {
        const char* sequence;
        int row;
        int column;
    };
    for (const auto& formator : { Formator { "\n", 2, 3 }, Formator { "\v", 2, 3 }, Formator { "\f", 2, 3 }, Formator { "\033E", 2, 0 }, Formator { "\033M", 0, 3 }, Formator { "\033[1;3f", 0, 2 }, Formator { "\033[1d", 0, 3 }, Formator { "\033[e", 2, 3 }, Formator { "\033[k", 0, 3 } }) {
        Ubiquitty::Terminal terminal;
        Ubiquitty::hardReset(terminal);
        Ubiquitty::resize(terminal, 3, 6);
        feed(terminal, "\033[1;1Hxxxxxx\033[2;1Hxxxxxx\033[3;1Hxxxxxx\033[6 V\033[2;4H");
        feed(terminal, formator.sequence);
        CHECK_EQ(terminal.screens.active->cursorY, formator.row);
        CHECK_EQ(terminal.screens.active->cursorX, formator.column);
        feed(terminal, "A\033[4 H");
        std::string expected = "xxxxxx";
        expected[static_cast<std::size_t>(formator.column)] = '_';
        expected[5] = 'A';
        CHECK_ROW(terminal, formator.row, expected.c_str());
    }
}

TEST(testQuadLeavesWhatItCannotPositionAlone)
{
    for (const char* sequence : { "\033[99 H", "\033[7 H", "\033[0;2 H", "\033[?4 H", "\033[4!H", "\033[4  H", "\033[1 H", "\033[3 H", "\033[5 H", "\033[6 H" }) {
        Ubiquitty::Terminal terminal;
        Ubiquitty::hardReset(terminal);
        Ubiquitty::resize(terminal, 2, 6);
        feed(terminal, "\033[5 VAB");
        feed(terminal, sequence);
        CHECK_ROW(terminal, 0, "AB____");
        CHECK_EQ(terminal.screens.active->cursorX, 2);
    }
}

TEST(testQuadKeepsAStringThatDoesNotFitBetweenTheMargins)
{
    Ubiquitty::Terminal terminal;
    Ubiquitty::hardReset(terminal);
    Ubiquitty::resize(terminal, 2, 8);
    feed(terminal, "\033[3 U\033[6 VABCDEF\033[2 H");
    CHECK_ROW(terminal, 0, "ABCDEF__");
    feed(terminal, "\033[2;1fAB\033[4 H");
    CHECK_ROW(terminal, 1, "____AB__");
}

TEST(testQuadMovesRenditionAndWideCharactersWithTheString)
{
    Ubiquitty::Terminal terminal;
    Ubiquitty::hardReset(terminal);
    Ubiquitty::resize(terminal, 2, 8);
    feed(terminal, "\033[8 V\033[1mA\033[0m\xE4\xB8\x80\033[4 H");
    CHECK_EQ(cell(terminal, 0, 5)->codePoint, static_cast<uint32_t>('A'));
    CHECK(Ubiquitty::cellPen(*cell(terminal, 0, 5)).attributes.contains(Ubiquitty::VT::Attribute::Bold));
    CHECK_EQ(cell(terminal, 0, 6)->codePoint, 0x4E00u);
    CHECK_EQ(cell(terminal, 0, 0)->codePoint, 0u);
    CHECK_EQ(cell(terminal, 0, 1)->codePoint, 0u);
    CHECK_EQ(cell(terminal, 0, 2)->codePoint, 0u);
}

TEST(testAlignedTabStopsAreSharedByHorizontalTabAndCHT)
{
    for (const char* definition : { "\033[9 a", "\033[9 b", "\033[9 `", "\033[9 c" }) {
        Ubiquitty::Terminal terminal;
        Ubiquitty::resize(terminal, 2, 20);
        feed(terminal, "\033[3g");
        feed(terminal, definition);
        feed(terminal, "\033[IX");
        CHECK_EQ(cell(terminal, 0, 8)->codePoint, uint32_t('X'));
        CHECK(!terminal.isAlignedTabActive);
    }
}

TEST(testAlignedTabStringsUseTheirFirstAndLastGlyphEdges)
{
    struct Example {
        const char* definition;
        int start;
    };
    for (const auto& example : { Example { "\033[9 a", 6 }, Example { "\033[9 b", 7 }, Example { "\033[9 `", 7 } }) {
        for (const char* end : { "\r", "\033E", "\t" }) {
            Ubiquitty::Terminal terminal;
            Ubiquitty::resize(terminal, 2, 20);
            feed(terminal, "\033[3g\033[17G\033H\r");
            feed(terminal, example.definition);
            feed(terminal, "\tABC");
            CHECK(terminal.isAlignedTabActive);
            feed(terminal, end);
            CHECK(!terminal.isAlignedTabActive);
            CHECK_EQ(cell(terminal, 0, example.start)->codePoint, uint32_t('A'));
            CHECK_EQ(cell(terminal, 0, example.start + 1)->codePoint, uint32_t('B'));
            CHECK_EQ(cell(terminal, 0, example.start + 2)->codePoint, uint32_t('C'));
        }
    }
}

TEST(testAlignedTabTextRestoresCellsItMovesAwayFrom)
{
    Ubiquitty::Terminal terminal;
    Ubiquitty::resize(terminal, 2, 20);
    feed(terminal, "abcdefghijklmnopqrst\r\033[3g\033[9;46 c\t12");
    CHECK_EQ(cell(terminal, 0, 7)->codePoint, uint32_t('1'));
    CHECK_EQ(cell(terminal, 0, 8)->codePoint, uint32_t('2'));
    feed(terminal, ".34\r");
    CHECK_EQ(cell(terminal, 0, 6)->codePoint, uint32_t('1'));
    CHECK_EQ(cell(terminal, 0, 7)->codePoint, uint32_t('2'));
    CHECK_EQ(cell(terminal, 0, 8)->codePoint, uint32_t('.'));
    CHECK_EQ(cell(terminal, 0, 9)->codePoint, uint32_t('3'));
    CHECK_EQ(cell(terminal, 0, 10)->codePoint, uint32_t('4'));
    CHECK_EQ(cell(terminal, 0, 11)->codePoint, uint32_t('l'));
}

TEST(testCenteredTabTargetUsesItsFirstOccurrenceAndDefaultsToSpace)
{
    for (const char* definition : { "\033[9 c", "\033[9;32 c" }) {
        Ubiquitty::Terminal terminal;
        Ubiquitty::resize(terminal, 2, 20);
        feed(terminal, "\033[3g");
        feed(terminal, definition);
        feed(terminal, "\tAB C D\r");
        CHECK_EQ(cell(terminal, 0, 6)->codePoint, uint32_t('A'));
        CHECK_EQ(cell(terminal, 0, 8)->codePoint, uint32_t(' '));
        CHECK_EQ(cell(terminal, 0, 11)->codePoint, uint32_t('D'));
    }
}

TEST(testAlignedTabsPreserveRenditionAndGraphemeWidths)
{
    Ubiquitty::Terminal terminal;
    Ubiquitty::resize(terminal, 2, 20);
    feed(terminal, "\033[3g\033[9 a\tA\033[1mB\xcc\x81\xcc\x82\033[0m\xe4\xb8\xad\r");
    CHECK_EQ(cell(terminal, 0, 6)->codePoint, uint32_t('A'));
    CHECK_EQ(cell(terminal, 0, 7)->codePoint, uint32_t('B'));
    CHECK(Ubiquitty::cellPen(*cell(terminal, 0, 7)).attributes.contains(Ubiquitty::VT::Attribute::Bold));
    CHECK(!Ubiquitty::cellPen(*cell(terminal, 0, 8)).attributes.contains(Ubiquitty::VT::Attribute::Bold));
    CHECK_EQ(cell(terminal, 0, 8)->codePoint, 0x4E2Du);
    CHECK(Ubiquitty::cellPen(*cell(terminal, 0, 9)).attributes.contains(Ubiquitty::VT::Attribute::WideContinuation));
    std::array<uint32_t, 8> points {};
    CHECK_EQ(Ubiquitty::cellClusterCodePoints(&terminal, cell(terminal, 0, 7), points.data(), 8), 3);
    CHECK_EQ(points[1], 0x301u);
    CHECK_EQ(points[2], 0x302u);
}

TEST(testOrdinaryTabSetReplacesAlignmentAndResetClearsIt)
{
    for (const char* set : { "\033H", "\033[W" }) {
        Ubiquitty::Terminal terminal;
        Ubiquitty::resize(terminal, 2, 20);
        feed(terminal, "\033[3g\033[9 a\033[9G");
        feed(terminal, set);
        feed(terminal, "\r\tABC\r");
        CHECK_EQ(cell(terminal, 0, 8)->codePoint, uint32_t('A'));
        CHECK_EQ(cell(terminal, 0, 10)->codePoint, uint32_t('C'));
        feed(terminal, "\033[9 a\tZ\033c");
        CHECK(!terminal.isAlignedTabActive);
        CHECK(terminal.tabAlignments.empty());
        feed(terminal, "\tABC\r");
        CHECK_EQ(cell(terminal, 0, 8)->codePoint, uint32_t('A'));
    }
}

TEST(testAlignedTabClippingDoesNotLeaveWideFragments)
{
    Ubiquitty::Terminal terminal;
    Ubiquitty::resize(terminal, 2, 8);
    feed(terminal, "\033[3g\033[2 a\t\xe4\xb8\xad"
                   "AB\r");
    CHECK_EQ(cell(terminal, 0, 0)->codePoint, uint32_t('A'));
    CHECK_EQ(cell(terminal, 0, 1)->codePoint, uint32_t('B'));
    CHECK(!Ubiquitty::cellPen(*cell(terminal, 0, 0)).attributes.contains(Ubiquitty::VT::Attribute::WideContinuation));
}

TEST(testAlignedTabInsertionShiftsTheOriginalTextOnce)
{
    Ubiquitty::Terminal terminal;
    Ubiquitty::resize(terminal, 2, 20);
    feed(terminal, "abcdefghijklmnopqrst\r\033[3g\033[9 a\033[4h\tABC\r");
    CHECK_EQ(cell(terminal, 0, 6)->codePoint, uint32_t('A'));
    CHECK_EQ(cell(terminal, 0, 8)->codePoint, uint32_t('C'));
    CHECK_EQ(cell(terminal, 0, 9)->codePoint, uint32_t('g'));
    CHECK_EQ(cell(terminal, 0, 19)->codePoint, uint32_t('q'));
}

TEST(testAlignedTabInputIsIndependentOfReadBoundaries)
{
    constexpr std::string_view input = "\033[3g\033[9 a\tA\033[1mB\xcc\x81\xcc\x82\033[0m\xe4\xb8\xad\r";
    for (std::size_t split = 0; split <= input.size(); ++split) {
        Ubiquitty::Terminal terminal;
        Ubiquitty::resize(terminal, 2, 20);
        feed(terminal, input.substr(0, split));
        feed(terminal, input.substr(split));
        CHECK_EQ(cell(terminal, 0, 6)->codePoint, uint32_t('A'));
        CHECK_EQ(cell(terminal, 0, 7)->codePoint, uint32_t('B'));
        CHECK_EQ(cell(terminal, 0, 8)->codePoint, 0x4E2Du);
    }
}

TEST(testAlignedTabTargetUsesTheInvokedCharacterSet)
{
    Ubiquitty::Terminal terminal;
    Ubiquitty::resize(terminal, 2, 20);
    feed(terminal, "\033[3g\033(0\033[9;113 c\tABqCD\r");
    CHECK_EQ(cell(terminal, 0, 6)->codePoint, uint32_t('A'));
    CHECK_EQ(cell(terminal, 0, 8)->codePoint, 0x2500u);
    feed(terminal, "\033[9;0 c\033[9;128 c\033[9;256 c");
    CHECK_EQ(terminal.tabAlignmentTargets[8], 0x2500u);
}

TEST(testAlignedTabStorageIsBoundedAndResizeSettlesTheString)
{
    Ubiquitty::Terminal terminal;
    Ubiquitty::resize(terminal, 2, 20);
    feed(terminal, "\033[3g\033[9 a\t");
    feed(terminal, std::string(2 * Ubiquitty::maxColumns, 'x'));
    CHECK(terminal.didAlignedTabOverflow);
    CHECK_EQ(terminal.alignedTabTerminal->columns, Ubiquitty::maxColumns);
    Ubiquitty::resize(terminal, 3, 24);
    CHECK(!terminal.isAlignedTabActive);
    feed(terminal, "\033[H\tYZ\r");
    CHECK_EQ(cell(terminal, 0, 7)->codePoint, uint32_t('Y'));
    CHECK_EQ(cell(terminal, 0, 8)->codePoint, uint32_t('Z'));
}

TEST(testTabClearAndDECReportRestorationRemoveAlignment)
{
    for (const char* restoration : { "\033[3g\033[9G\033H", "\033[5W\033[9G\033H", "\033P2$t9\033\\" }) {
        Ubiquitty::Terminal terminal;
        Ubiquitty::resize(terminal, 2, 20);
        feed(terminal, "\033[3g\033[9 a");
        feed(terminal, restoration);
        feed(terminal, "\r\tABC\r");
        CHECK_EQ(cell(terminal, 0, 8)->codePoint, uint32_t('A'));
        CHECK_EQ(cell(terminal, 0, 10)->codePoint, uint32_t('C'));
    }
}

TEST(testDECSelectiveErasureIgnoresECMAGuarding)
{
    for (const char* erase : { "\033[?2K", "\033[?2J", "\033[1;1;1;6${" }) {
        Ubiquitty::Terminal terminal;
        Ubiquitty::resize(terminal, 2, 6);
        feed(terminal, "\033[1\"qD\033[0\"q\033VG\033[1\"qB\033W\033[0\"qU");
        feed(terminal, erase);
        CHECK_EQ(cell(terminal, 0, 0)->codePoint, uint32_t('D'));
        CHECK_EQ(cell(terminal, 0, 1)->codePoint, 0u);
        CHECK_EQ(cell(terminal, 0, 2)->codePoint, uint32_t('B'));
        CHECK_EQ(cell(terminal, 0, 3)->codePoint, 0u);
    }
}

TEST(testECMAErasureIgnoresDECProtection)
{
    for (const char* erase : { "\r\033[2K", "\r\033[2J", "\r\033[6X", "\r\033[2N" }) {
        Ubiquitty::Terminal terminal;
        Ubiquitty::resize(terminal, 2, 10);
        feed(terminal, "\033H\033[1\"qD\033[0\"q\033VG\033[1\"qB\033W\033[0\"qU");
        feed(terminal, erase);
        CHECK_EQ(cell(terminal, 0, 0)->codePoint, 0u);
        CHECK_EQ(cell(terminal, 0, 1)->codePoint, uint32_t('G'));
        CHECK_EQ(cell(terminal, 0, 2)->codePoint, uint32_t('B'));
        CHECK_EQ(cell(terminal, 0, 3)->codePoint, 0u);
    }
}

TEST(testSGRResetPreservesBothProtectionProperties)
{
    for (const char* erase : { "\033[?2K", "\033[2K" }) {
        Ubiquitty::Terminal terminal;
        feed(terminal, "\033V\033[1\"q\033[1;31mA\033[0mB\033W\033[0\"qC");
        feed(terminal, erase);
        CHECK_EQ(cell(terminal, 0, 0)->codePoint, uint32_t('A'));
        CHECK_EQ(cell(terminal, 0, 1)->codePoint, uint32_t('B'));
        CHECK_EQ(cell(terminal, 0, 2)->codePoint, 0u);
        CHECK(!Ubiquitty::cellPen(*cell(terminal, 0, 1)).attributes.contains(Ubiquitty::VT::Attribute::Bold));
    }
}

TEST(testSoftResetPreservesProtectionOfExistingCells)
{
    for (const char* erase : { "\033[?2K", "\033[2K" }) {
        Ubiquitty::Terminal terminal;
        feed(terminal, "\033V\033[1\"qA\033[!pB");
        feed(terminal, erase);
        CHECK_EQ(cell(terminal, 0, 0)->codePoint, uint32_t('A'));
        CHECK_EQ(cell(terminal, 0, 1)->codePoint, 0u);
    }
}

TEST(testRectangleCopyPreservesIndependentProtection)
{
    Ubiquitty::Terminal terminal;
    Ubiquitty::resize(terminal, 3, 6);
    feed(terminal, "\033[1\"qD\033[0\"q\033VG\033W\033[1;1;1;2;1;2;1;1$v");
    feed(terminal, "\033[1;1H\033[?2K\033[2;1H\033[2K");
    CHECK_EQ(cell(terminal, 0, 0)->codePoint, uint32_t('D'));
    CHECK_EQ(cell(terminal, 0, 1)->codePoint, 0u);
    CHECK_EQ(cell(terminal, 1, 0)->codePoint, 0u);
    CHECK_EQ(cell(terminal, 1, 1)->codePoint, uint32_t('G'));
}

TEST(testCursorRestoreRetainsIndependentProtection)
{
    for (const char* erase : { "\033[?2K", "\033[2K" }) {
        Ubiquitty::Terminal terminal;
        feed(terminal, "\033V\033[1\"q\0337\033W\033[0\"q\0338A\033W\033[0\"qB");
        feed(terminal, erase);
        CHECK_EQ(cell(terminal, 0, 0)->codePoint, uint32_t('A'));
        CHECK_EQ(cell(terminal, 0, 1)->codePoint, 0u);
    }
}

UBIQUITTY_TEST_MAIN()
