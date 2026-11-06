// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_logical_position.cpp — a position names characters, not rows
// (Core/Screen/LogicalPosition.h): it resolves to the same text through
// rewraps, eviction, in-place moves and pull-backs, and to an explicit failure
// once the text is gone, never to other text.

#include "Harness.h"
#include "ScopedNothrowAllocationRefusal.h"
#include "TerminalFixture.h"

#include "Core/CellGrid.h"
#include "Core/LineStore.h"
#include "Core/Screen/HistoryLines.h"
#include "Core/Screen/LineIdentity.h"
#include "Core/Screen/LogicalPosition.h"
#include "Core/Screen/ScrollLine.h"
#include "Core/Screen/Scrollback.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <new>
#include <optional>
#include <random>
#include <string>
#include <vector>

void* operator new(std::size_t size, const std::nothrow_t&) noexcept
{
    if (UbiquittyTest::ScopedNothrowAllocationRefusal::refuses(size))
        return nullptr;
    return ::operator new(size);
}

void operator delete(void* pointer, const std::nothrow_t&) noexcept { ::operator delete(pointer); }

using Ubiquitty::Screen::LogicalPosition;
using Ubiquitty::Screen::PositionFailure;
using UbiquittyTest::feed;

namespace {

std::unique_ptr<Ubiquitty::Terminal> terminalOf(int rows, int columns, int scrollbackLines = 0)
{
    auto t = std::make_unique<Ubiquitty::Terminal>();
    if (scrollbackLines > 0)
        Ubiquitty::setScrollbackLimit(t.get(), scrollbackLines);
    Ubiquitty::resize(*t, rows, columns);
    return t;
}

// A token that names one line, followed by filler to `length` characters.
std::string tokenLine(int index, int length)
{
    char token[16];
    std::snprintf(token, sizeof(token), "<T%05d>", index);
    std::string line = token;
    while (static_cast<int>(line.size()) < length)
        line.push_back(static_cast<char>('a' + line.size() % 26));
    line.resize(static_cast<std::size_t>(std::max<int>(length, static_cast<int>(std::string(token).size()))));
    return line;
}

std::string tokenOf(int index)
{
    char token[16];
    std::snprintf(token, sizeof(token), "<T%05d>", index);
    return token;
}

int historyRowsOf(const Ubiquitty::Terminal& t)
{
    return t.screens.isUsingAlternateScreen ? 0 : Ubiquitty::Screen::scrollbackCount(&t);
}

// The cells of an absolute row on the buffer shown.
struct RowRead {
    const Ubiquitty::Cell* cells = nullptr;
    int length = 0;
    bool isWrapped = false;
};

RowRead readRow(const Ubiquitty::Terminal& t, int absoluteRow)
{
    const int history = historyRowsOf(t);
    if (absoluteRow < history) {
        const Ubiquitty::Screen::ScrollLine* line = Ubiquitty::Screen::scrollbackLine(&t, absoluteRow);
        return line ? RowRead { line->cells, line->length, line->wrapped } : RowRead {};
    }
    const int row = absoluteRow - history;
    if (row < 0 || row >= t.rows)
        return {};
    const Ubiquitty::RowView view = t.grid->rowCells(Ubiquitty::Row(row));
    return { view.data, view.count, t.grid->rowMeta(row).has(Ubiquitty::RowFlag::Wrapped) };
}

// `count` characters from a place, continuing across soft wraps.
std::string textAt(const Ubiquitty::Terminal& t, Ubiquitty::Screen::PhysicalPosition place, int count)
{
    std::string text;
    int row = place.absoluteRow;
    int column = place.column;
    while (static_cast<int>(text.size()) < count) {
        const RowRead read = readRow(t, row);
        if (!read.cells)
            break;
        for (; column < read.length && static_cast<int>(text.size()) < count; ++column) {
            const char32_t codePoint = read.cells[column].codePoint;
            if (codePoint == 0 && Ubiquitty::Screen::isUnwrittenCell(read.cells[column]))
                continue;
            text.push_back(codePoint >= 0x20 && codePoint < 0x7F ? static_cast<char>(codePoint) : '?');
        }
        if (!read.isWrapped)
            break;
        ++row;
        column = 0;
    }
    return text;
}

// The absolute row and column a token is shown at, searching every row.
std::optional<Ubiquitty::Screen::PhysicalPosition> find(const Ubiquitty::Terminal& t, const std::string& token)
{
    const int total = historyRowsOf(t) + t.rows;
    for (int row = 0; row < total; ++row) {
        const RowRead read = readRow(t, row);
        for (int column = 0; column < read.length; ++column) {
            if (textAt(t, { row, column }, static_cast<int>(token.size())) == token)
                return Ubiquitty::Screen::PhysicalPosition { row, column };
        }
    }
    return std::nullopt;
}

LogicalPosition captureToken(Ubiquitty::Terminal& t, const std::string& token)
{
    const auto place = find(t, token);
    CHECK_MSG(place.has_value(), token.c_str());
    if (!place)
        return {};
    const auto position = Ubiquitty::Screen::capturePosition(t, place->absoluteRow, place->column);
    CHECK_MSG(position.has_value(), token.c_str());
    return position.value_or(LogicalPosition {});
}

bool resolvesTo(const Ubiquitty::Terminal& t, const LogicalPosition& position, const std::string& token)
{
    const auto place = Ubiquitty::Screen::resolvePosition(t, position);
    if (!place) {
        std::fprintf(stderr, "  %s failed to resolve (%d)\n", token.c_str(), static_cast<int>(place.error()));
        return false;
    }
    const std::string text = textAt(t, *place, static_cast<int>(token.size()));
    if (text != token)
        std::fprintf(stderr, "  %s resolved to row %d column %d reading '%s'\n", token.c_str(), place->absoluteRow, place->column, text.c_str());
    return text == token;
}

std::optional<PositionFailure> failureOf(const Ubiquitty::Terminal& t, const LogicalPosition& position)
{
    const auto place = Ubiquitty::Screen::resolvePosition(t, position);
    if (place)
        return std::nullopt;
    return place.error();
}

// Whether a column band's edges cross a token on any of its screen rows.
bool cutsAToken(const Ubiquitty::Terminal& t, int top, int bottom, int left, int right)
{
    for (int row = top; row <= bottom; ++row) {
        const RowRead read = readRow(t, historyRowsOf(t) + row);
        for (int column = 0; column + 1 < read.length; ++column) {
            if (read.cells[column].codePoint != '<' || read.cells[column + 1].codePoint != 'T')
                continue;
            const int last = column + 7;
            if ((column < left && last >= left) || (column <= right && last > right))
                return true;
        }
    }
    return false;
}

void feedTokenLines(Ubiquitty::Terminal& t, int first, int count, int length)
{
    for (int index = first; index < first + count; ++index)
        feed(t, tokenLine(index, length) + "\r\n");
}

} // namespace

TEST(aHistoryPositionRefusesAnUntrackedNameAndCanRetry)
{
    auto t = terminalOf(4, 40);
    feedTokenLines(*t, 0, 20, 20);
    const auto address = Ubiquitty::Screen::historyAddress(*t);
    REQUIRE(address.oldest < address.head);
    REQUIRE(!t->lineIdentity);
    const auto refusals = UbiquittyTest::ScopedNothrowAllocationRefusal::refusalCount();
    {
        UbiquittyTest::ScopedNothrowAllocationRefusal refuse(sizeof(Ubiquitty::Screen::LineIdentity));
        const auto position = Ubiquitty::Screen::historyPosition(*t, address.oldest, 0);
        REQUIRE(!position);
        CHECK(position.error() == PositionFailure::Destroyed);
        CHECK(!t->lineIdentity);
    }
    CHECK(UbiquittyTest::ScopedNothrowAllocationRefusal::refusalCount() == refusals + 1);
    const auto position = Ubiquitty::Screen::historyPosition(*t, address.oldest, 0);
    REQUIRE(position);
    const auto place = Ubiquitty::Screen::resolvePosition(*t, *position);
    REQUIRE(place);
    const auto text = textAt(*t, *place, 8);
    Ubiquitty::resize(*t, 4, 13);
    CHECK(resolvesTo(*t, *position, text));
    feed(*t, "\x1b"
             "c");
    CHECK(failureOf(*t, *position).has_value());
}

TEST(aHistoryPositionKeepsItsTextAcrossNarrowAndWideRewraps)
{
    auto t = terminalOf(10, 40);
    feedTokenLines(*t, 0, 60, 70);
    const LogicalPosition early = captureToken(*t, tokenOf(5));
    const LogicalPosition late = captureToken(*t, tokenOf(40));
    for (const int columns : { 23, 71, 40, 17, 120, 40 }) {
        Ubiquitty::resize(*t, 10, columns);
        CHECK(resolvesTo(*t, early, tokenOf(5)));
        CHECK(resolvesTo(*t, late, tokenOf(40)));
    }
}

// A place in the middle of a wrapped line: the cell offset counts written
// cells, so it names the same character at every width.
TEST(aPlaceInsideAWrappedLineKeepsItsCharacterAcrossRewraps)
{
    auto t = terminalOf(8, 30);
    feed(*t, "head\r\n");
    std::string line;
    for (int index = 0; index < 20; ++index)
        line += "w" + std::to_string(1000 + index) + " ";
    feed(*t, line + "\r\n");
    feedTokenLines(*t, 0, 3, 10);
    const LogicalPosition onScreen = captureToken(*t, "w1013");
    feedTokenLines(*t, 3, 20, 10);
    const LogicalPosition inHistory = captureToken(*t, "w1007");
    for (const int columns : { 11, 47, 30, 63, 19 }) {
        Ubiquitty::resize(*t, 8, columns);
        CHECK(resolvesTo(*t, onScreen, "w1013"));
        CHECK(resolvesTo(*t, inHistory, "w1007"));
    }
}

// The gaps a tab or a cursor move leave are not counted: a rewrap that
// strands one at a row's end drops it on the next rewrap, and a place after
// the gap keeps its character through both.
TEST(aPlaceAfterAGapKeepsItsCharacterAcrossRewraps)
{
    auto t = terminalOf(6, 40);
    feed(*t, "abc\tdef\t<T00001>xyz\x1b[4C<T00002>\r\n");
    const LogicalPosition afterTab = captureToken(*t, tokenOf(1));
    const LogicalPosition afterMove = captureToken(*t, tokenOf(2));
    for (const int columns : { 6, 40, 9, 17, 5, 40 }) {
        Ubiquitty::resize(*t, 6, columns);
        CHECK(resolvesTo(*t, afterTab, tokenOf(1)));
        CHECK(resolvesTo(*t, afterMove, tokenOf(2)));
    }
}

// A place inside a gap stays inside it.
TEST(aPlaceInAGapStaysInTheGap)
{
    auto t = terminalOf(6, 40);
    feed(*t, "abc\tdef\r\n");
    const auto position = Ubiquitty::Screen::capturePosition(*t, 0, 5);
    REQUIRE(position.has_value());
    CHECK_EQ(position->cell, 3u);
    CHECK_EQ(position->voidCells, 2u);
    const auto resolved = Ubiquitty::Screen::resolvePosition(*t, *position);
    REQUIRE(resolved.has_value());
    CHECK_EQ(resolved->column, 5);
}

TEST(aScreenPositionFollowsItsLineIntoHistoryAndIsEvictedWithIt)
{
    auto t = terminalOf(6, 40, 30);
    feedTokenLines(*t, 0, 3, 20);
    const LogicalPosition pinned = captureToken(*t, tokenOf(1));
    CHECK(pinned.line.isPinned());
    feedTokenLines(*t, 3, 10, 20);
    CHECK(Ubiquitty::Screen::historyAddress(*t).head > 1u);
    CHECK(resolvesTo(*t, pinned, tokenOf(1)));
    feedTokenLines(*t, 13, 60, 20);
    CHECK(failureOf(*t, pinned) == PositionFailure::Evicted);
}

TEST(aHistoryPositionIsEvictedWithItsLine)
{
    auto t = terminalOf(6, 40, 30);
    feedTokenLines(*t, 0, 20, 20);
    const LogicalPosition early = captureToken(*t, tokenOf(2));
    CHECK(!early.line.isPinned());
    feedTokenLines(*t, 20, 40, 20);
    CHECK(failureOf(*t, early) == PositionFailure::Evicted);
}

TEST(anEraseInvalidatesAPinnedLineButOverwritingDoesNot)
{
    auto t = terminalOf(6, 40);
    feedTokenLines(*t, 0, 4, 20);
    const LogicalPosition overwritten = captureToken(*t, tokenOf(1));
    const LogicalPosition erased = captureToken(*t, tokenOf(2));
    const LogicalPosition untouched = captureToken(*t, tokenOf(3));
    // Row 1 overwritten in place without an erase.
    feed(*t, "\x1b[2;1H<T00001>XYZ");
    CHECK(resolvesTo(*t, overwritten, tokenOf(1)));
    // Row 2 erased with EL.
    feed(*t, "\x1b[3;5H\x1b[K");
    CHECK(failureOf(*t, erased) == PositionFailure::Erased);
    CHECK(resolvesTo(*t, untouched, tokenOf(3)));
    // ECH and ED below the cursor erase too.
    const LogicalPosition byCharacters = captureToken(*t, tokenOf(3));
    feed(*t, "\x1b[4;3H\x1b[2X");
    CHECK(failureOf(*t, byCharacters) == PositionFailure::Erased);
    const LogicalPosition byDisplay = captureToken(*t, tokenOf(0));
    feed(*t, "\x1b[2J");
    CHECK(failureOf(*t, byDisplay) == PositionFailure::Erased);
}

TEST(aPositionFollowsARegionScrollAndIsDestroyedOffTheBand)
{
    auto t = terminalOf(10, 40);
    feedTokenLines(*t, 0, 9, 20);
    const LogicalPosition inside = captureToken(*t, tokenOf(5));
    const LogicalPosition atTop = captureToken(*t, tokenOf(2));
    const LogicalPosition outside = captureToken(*t, tokenOf(8));
    // Band rows 3..8 (1-based), scroll up by one: line 2 leaves the band.
    feed(*t, "\x1b[3;8r\x1b[8;1H\n\x1b[r");
    CHECK(resolvesTo(*t, inside, tokenOf(5)));
    CHECK(resolvesTo(*t, outside, tokenOf(8)));
    CHECK(failureOf(*t, atTop) == PositionFailure::Destroyed);
}

TEST(insertAndDeleteLineAboveAPinnedLineFollowItIntoHistory)
{
    auto t = terminalOf(10, 40, 200);
    feedTokenLines(*t, 0, 9, 20);
    const LogicalPosition pinned = captureToken(*t, tokenOf(6));
    feed(*t, "\x1b[3;1H\x1b[2L");
    CHECK(resolvesTo(*t, pinned, tokenOf(6)));
    feed(*t, "\x1b[2;1H\x1b[3M");
    CHECK(resolvesTo(*t, pinned, tokenOf(6)));
    feed(*t, "\x1b[10;1H");
    feedTokenLines(*t, 100, 30, 20);
    CHECK(resolvesTo(*t, pinned, tokenOf(6)));
    // A history capture of a line the deletion renamed resolves to that line.
    const LogicalPosition later = captureToken(*t, tokenOf(110));
    CHECK(resolvesTo(*t, later, tokenOf(110)));
    CHECK(resolvesTo(*t, pinned, tokenOf(6)));
}

TEST(aHistoryPositionPulledOntoTheScreenFollowsEditsThere)
{
    auto t = terminalOf(6, 40, 200);
    feedTokenLines(*t, 0, 20, 20);
    const LogicalPosition pulled = captureToken(*t, tokenOf(13));
    const LogicalPosition kept = captureToken(*t, tokenOf(11));
    CHECK(!pulled.line.isPinned());
    // A taller screen pulls the newest history lines back onto it.
    Ubiquitty::resize(*t, 12, 40);
    CHECK(resolvesTo(*t, pulled, tokenOf(13)));
    CHECK(resolvesTo(*t, kept, tokenOf(11)));
    const auto place = Ubiquitty::Screen::resolvePosition(*t, pulled);
    REQUIRE(place.has_value());
    CHECK(place->absoluteRow >= historyRowsOf(*t));
    // Erased while on the screen, it stays erased once pushed back.
    feed(*t, "\x1b[" + std::to_string(place->absoluteRow - historyRowsOf(*t) + 1) + ";1H\x1b[2K");
    Ubiquitty::resize(*t, 6, 40);
    CHECK(failureOf(*t, pulled) == PositionFailure::Erased);
    CHECK(resolvesTo(*t, kept, tokenOf(11)));
}

TEST(anInsertedLineDoesNotInheritTheNameOfALineCarriedOnScreen)
{
    auto t = terminalOf(6, 40, 200);
    feedTokenLines(*t, 0, 20, 20);
    const LogicalPosition carried = captureToken(*t, tokenOf(14));
    Ubiquitty::resize(*t, 12, 40);
    const auto place = Ubiquitty::Screen::resolvePosition(*t, carried);
    REQUIRE(place.has_value());
    // An IL above the carried line gives its number to the inserted line.
    feed(*t, "\x1b[1;1H\x1b[L<T00500>");
    feed(*t, "\x1b[12;1H\r\n");
    feedTokenLines(*t, 600, 20, 20);
    CHECK(resolvesTo(*t, carried, tokenOf(14)));
    const LogicalPosition inserted = captureToken(*t, tokenOf(500));
    CHECK(resolvesTo(*t, inserted, tokenOf(500)));
    CHECK(inserted.line != carried.line);
}

TEST(aWideningRewrapCarriesHistoryLinesOntoTheScreen)
{
    auto t = terminalOf(6, 20, 200);
    feedTokenLines(*t, 0, 12, 35);
    // Line 8's two rows are both stored; line 9 straddles the seam.
    const LogicalPosition recent = captureToken(*t, tokenOf(8));
    CHECK(!recent.line.isPinned());
    Ubiquitty::resize(*t, 6, 80);
    CHECK(resolvesTo(*t, recent, tokenOf(8)));
    const auto place = Ubiquitty::Screen::resolvePosition(*t, recent);
    REQUIRE(place.has_value());
    REQUIRE(place->absoluteRow >= historyRowsOf(*t));
    feed(*t, "\x1b[" + std::to_string(place->absoluteRow - historyRowsOf(*t) + 1) + ";1H\x1b[2K");
    CHECK(failureOf(*t, recent) == PositionFailure::Erased);
}

TEST(theAlternateBufferNamesItsOwnLines)
{
    auto t = terminalOf(8, 40);
    feedTokenLines(*t, 0, 4, 20);
    const LogicalPosition primary = captureToken(*t, tokenOf(2));
    feed(*t, "\x1b[?1049h");
    feedTokenLines(*t, 100, 4, 20);
    const LogicalPosition alternate = captureToken(*t, tokenOf(102));
    CHECK(alternate.buffer == Ubiquitty::Screen::Buffer::Alternate);
    CHECK(failureOf(*t, primary) == PositionFailure::OtherBuffer);
    CHECK(resolvesTo(*t, alternate, tokenOf(102)));
    // A full-screen scroll on the alternate screen moves its lines in place.
    feed(*t, "\x1b[8;1H\n\n");
    CHECK(resolvesTo(*t, alternate, tokenOf(102)));
    feed(*t, "\x1b[?1049l");
    CHECK(resolvesTo(*t, primary, tokenOf(2)));
    CHECK(failureOf(*t, alternate) == PositionFailure::OtherBuffer);
    // Entering again clears the alternate screen, erasing what it held.
    feed(*t, "\x1b[?1049h");
    CHECK(failureOf(*t, alternate) == PositionFailure::Erased);
}

TEST(anAlternateRowKeepsItsPlaceThroughErasesAndFollowsOnlyMoves)
{
    auto t = terminalOf(6, 10);
    feed(*t, "\x1b[?1049h");
    // Row 0 wraps into row 1, so row 1 continues a line.
    feed(*t, "0123456789abcdefghij\r\nrow 2\r\nrow 3\r\nrow 4\r\nrow 5");
    const auto continuation = Ubiquitty::Screen::captureAlternateRow(*t, 1);
    const auto lower = Ubiquitty::Screen::captureAlternateRow(*t, 4);
    REQUIRE(continuation.has_value());
    REQUIRE(lower.has_value());
    const auto rowOf = [&t](const LogicalPosition& position) { return Ubiquitty::Screen::resolveAlternateRow(*t, position, 0); };

    // A scroll of the whole screen takes the line's first row off the top; the
    // row that continued it is still there, a row higher.
    feed(*t, "\x1b[6;1H\n");
    CHECK(rowOf(*continuation) == 0);
    CHECK(rowOf(*lower) == 3);

    // Erases and rewrites, down to a clear of the screen, leave every row.
    feed(*t, "\x1b[1;1H\x1b[2Kother\x1b[4;3H\x1b[K\x1b[4;1H\x1b[2P\x1b[H\x1b[2J");
    CHECK(rowOf(*continuation) == 0);
    CHECK(rowOf(*lower) == 3);

    // A region scroll carries a row with it, and off its top, away.
    feed(*t, "\x1b[2;4r\x1b[4;1H\n\n");
    CHECK(rowOf(*lower) == 1);
    CHECK(rowOf(*continuation) == 0);
    feed(*t, "\n\x1b[r");
    const auto carriedOff = rowOf(*lower);
    CHECK(!carriedOff && carriedOff.error() == PositionFailure::Destroyed);

    // The primary screen names no alternate row.
    feed(*t, "\x1b[?1049l");
    const auto hidden = rowOf(*continuation);
    CHECK(!hidden && hidden.error() == PositionFailure::OtherBuffer);
    const auto onPrimary = Ubiquitty::Screen::captureAlternateRow(*t, 0);
    CHECK(!onPrimary && onPrimary.error() == PositionFailure::OtherBuffer);
}

TEST(aPaneScrollSplitsAnAlternateRowByColumn)
{
    auto t = terminalOf(5, 20);
    feed(*t, "\x1b[?1049h");
    feed(*t, "left-one  right-one\r\nleft-two  right-two\r\nleft-thr  right-thr\r\nleft-fou  right-fou\r\nleft-fiv  right-fiv");
    const auto row = Ubiquitty::Screen::captureAlternateRow(*t, 2);
    REQUIRE(row.has_value());
    // DECSLRM makes columns 10..19 a pane, and the pane scrolls up one.
    feed(*t, "\x1b[?69h\x1b[11;20s\x1b[5;11H\r\n");
    CHECK_MSG(Ubiquitty::Screen::resolveAlternateRow(*t, *row, 12) == 1, "the pane's cells moved up with the pane");
    CHECK_MSG(Ubiquitty::Screen::resolveAlternateRow(*t, *row, 3) == 2, "the cells left of the pane stand");
}

TEST(clearingHistoryEvictsItsPositionsAndKeepsTheSurvivorsNames)
{
    auto t = terminalOf(6, 40, 500);
    feedTokenLines(*t, 0, 50, 20);
    const LogicalPosition cleared = captureToken(*t, tokenOf(3));
    const LogicalPosition survivor = captureToken(*t, tokenOf(30));
    const LogicalPosition onScreen = captureToken(*t, tokenOf(47));
    Ubiquitty::Screen::dropScrollbackBefore(t.get(), 20);
    CHECK(failureOf(*t, cleared) == PositionFailure::Evicted);
    CHECK(resolvesTo(*t, survivor, tokenOf(30)));
    CHECK(resolvesTo(*t, onScreen, tokenOf(47)));
    // Lines completing after the clear take names no position already holds.
    feedTokenLines(*t, 50, 30, 20);
    CHECK(resolvesTo(*t, survivor, tokenOf(30)));
    CHECK(resolvesTo(*t, onScreen, tokenOf(47)));
    CHECK(failureOf(*t, cleared) == PositionFailure::Evicted);
    feed(*t, "\x1b[3J");
    CHECK(failureOf(*t, survivor) == PositionFailure::Evicted);
}

TEST(aHardResetLeavesNoPositionNamingNewText)
{
    auto t = terminalOf(6, 40, 500);
    feedTokenLines(*t, 0, 30, 20);
    const LogicalPosition history = captureToken(*t, tokenOf(10));
    const LogicalPosition screen = captureToken(*t, tokenOf(28));
    feed(*t, "\x1b"
             "c");
    feedTokenLines(*t, 0, 30, 20);
    CHECK(failureOf(*t, history).has_value());
    CHECK(failureOf(*t, screen).has_value());
}

// The far end of a whole-line selection stands in the void past the line's
// text, and stays at the line's end through a narrowing rewrap.
TEST(aPlaceInTheVoidStaysAtItsLineEnd)
{
    auto t = terminalOf(6, 60);
    feed(*t, "short line\r\n<T00001>\r\n");
    const auto place = find(*t, "short line");
    REQUIRE(place.has_value());
    const auto end = Ubiquitty::Screen::capturePosition(*t, place->absoluteRow, 59);
    REQUIRE(end.has_value());
    CHECK_EQ(end->cell, 10u);
    CHECK_EQ(end->voidCells, 49u);
    Ubiquitty::resize(*t, 6, 25);
    const auto resolved = Ubiquitty::Screen::resolvePosition(*t, *end);
    REQUIRE(resolved.has_value());
    CHECK_EQ(resolved->absoluteRow, place->absoluteRow);
    CHECK_EQ(resolved->column, 24);
}

// A line a wrap joins to the one above keeps its characters, so a position
// on it still names them, before and after a rewrap.
TEST(aLineAWrapJoinsKeepsItsText)
{
    auto t = terminalOf(6, 10);
    feed(*t, "abc\r\nZ<T00001>\r\n");
    const LogicalPosition below = captureToken(*t, tokenOf(1));
    // Fill the line above to its margin; the next character wraps onto the
    // token's row, over the Z, and joins the two rows into one line.
    feed(*t, "\x1b[1;1H0123456789X");
    CHECK(resolvesTo(*t, below, tokenOf(1)));
    Ubiquitty::resize(*t, 6, 23);
    CHECK(resolvesTo(*t, below, tokenOf(1)));
    Ubiquitty::resize(*t, 6, 7);
    CHECK(resolvesTo(*t, below, tokenOf(1)));
}

// A place past a line's written cells stands at the line's end: a request past
// a line that has since grown shorter resolves there rather than failing.
TEST(aPlacePastAShortenedLineResolvesAtItsEnd)
{
    auto t = terminalOf(6, 40);
    feed(*t, "0123456789012345678901234567890123456789");
    const auto position = Ubiquitty::Screen::capturePosition(*t, 0, 30);
    REQUIRE(position.has_value());
    feed(*t, "\x1b[1;6H\x1b[K");
    const auto resolved = Ubiquitty::Screen::resolvePosition(*t, *position, Ubiquitty::Screen::RevisionCheck::IgnoreErases);
    REQUIRE(resolved.has_value());
    CHECK_EQ(resolved->absoluteRow, 0);
    CHECK_EQ(resolved->column, 5);
    CHECK(failureOf(*t, *position) == PositionFailure::Erased);
}

// A pane a multiplexer scrolls with left and right margins moves a column band
// inside rows that stay put: a position in the band follows it, one beside it
// stays, and one the band carries off the pane's top names nothing.
TEST(aPositionInAMarginedPaneFollowsThePanesScroll)
{
    auto t = terminalOf(6, 20);
    feed(*t, "left-one  right-one\r\nleft-two  right-two\r\nleft-thr  right-thr\r\nleft-fou  right-fou\r\nleft-fiv  right-fiv");
    const LogicalPosition inPane = captureToken(*t, "right-thr");
    const LogicalPosition beside = captureToken(*t, "left-thr");
    const LogicalPosition atTop = captureToken(*t, "right-one");
    // The pane is rows 1-5, columns 11-20; a linefeed on its last row
    // scrolls it.
    feed(*t, "\x1b[?69h\x1b[11;20s\x1b[1;5r\x1b[5;11H\n");
    feed(*t, "\x1b[5;11Hright-six");
    CHECK(resolvesTo(*t, inPane, "right-thr"));
    CHECK(resolvesTo(*t, beside, "left-thr"));
    CHECK(failureOf(*t, atTop) == PositionFailure::Destroyed);
    feed(*t, "\x1b[5;11H\n");
    CHECK(resolvesTo(*t, inPane, "right-thr"));
    CHECK(resolvesTo(*t, beside, "left-thr"));
    // The next scroll carries it off the pane's top.
    feed(*t, "\x1b[5;11H\n");
    CHECK(failureOf(*t, inPane) == PositionFailure::Destroyed);
    CHECK(resolvesTo(*t, beside, "left-thr"));
}

// The pin table is bounded: once full, the oldest pin whose line left the
// screen is given up, and a position on it names nothing rather than another
// line.
TEST(aFullPinTableGivesUpTheOldestDepartedPin)
{
    auto t = terminalOf(4, 40, 5000);
    const int captures = static_cast<int>(Ubiquitty::Screen::BufferPins::maxPins) + 40;
    std::vector<LogicalPosition> held;
    for (int index = 0; index < captures; ++index) {
        feed(*t, tokenLine(index, 20) + "\r\n");
        // The line just written is the screen's second row from the bottom.
        const auto position = Ubiquitty::Screen::capturePosition(*t, historyRowsOf(*t) + t->rows - 2, 0);
        REQUIRE(position.has_value());
        CHECK(position->line.isPinned());
        held.push_back(*position);
    }
    CHECK(t->lineIdentity->primary.pins.size() <= Ubiquitty::Screen::BufferPins::maxPins);
    CHECK(failureOf(*t, held.front()) == PositionFailure::Destroyed);
    CHECK(resolvesTo(*t, held.back(), tokenOf(captures - 1)));
    CHECK(resolvesTo(*t, held[held.size() - 100], tokenOf(captures - 100)));
}

// Randomized: lines carry unique tokens, positions are captured on them, and
// the run mixes output, wraps, in-place moves, margined pane scrolls, erases,
// alternate trips, resizes, clears and resets. A position that resolves must
// land on its token; failing is allowed, landing on other text is not. Nothing
// prints over a held token without erasing it first, since printing over a
// line keeps its positions on purpose.
TEST(aPositionNeverResolvesToOtherText)
{
    // UBQ_POSITION_SEEDS runs more seeds; UBQ_POSITION_TRACE=<seed> prints
    // that seed's pins and resolutions after every step.
    const char* seedsOverride = std::getenv("UBQ_POSITION_SEEDS");
    const std::uint32_t seeds = seedsOverride ? static_cast<std::uint32_t>(std::atoi(seedsOverride)) : 24;
    // Resolutions that landed and were read, so a run that resolved nothing
    // cannot pass.
    long long landed = 0;
    for (std::uint32_t seed = 1; seed <= seeds; ++seed) {
        std::mt19937 random(seed);
        const auto pick = [&random](int low, int high) { return std::uniform_int_distribution<int>(low, high)(random); };
        auto t = terminalOf(pick(4, 12), pick(12, 60), pick(30, 200));
        int nextToken = 0;
        struct Held {
            LogicalPosition position;
            std::string token;
        };
        std::vector<Held> held;
        // What each step did, printed when a resolution lands on other text.
        std::vector<std::string> trace;
        const auto note = [&](std::string what) { trace.push_back(std::move(what)); };
        const auto send = [&](const std::string& bytes) {
            std::string shown;
            for (const char byte : bytes)
                shown += byte == '\x1b' ? std::string("\\e") : byte == '\r' ? std::string("\\r")
                    : byte == '\n'                                          ? std::string("\\n")
                                                                            : std::string(1, byte);
            note("    " + shown);
            feed(*t, bytes);
        };
        for (int step = 0; step < 300; ++step) {
            const int action = pick(0, 16);
            note(std::to_string(step) + ": action " + std::to_string(action) + " at " + std::to_string(t->rows) + "x" + std::to_string(t->columns) + " history " + std::to_string(historyRowsOf(*t)));
            switch (action) {
            case 0:
            case 1:
            case 2:
                // Output starts on a fresh bottom row: printing over a line
                // keeps its positions, so text printed over a held token
                // would be a change the oracle cannot tell from a wrong
                // resolution.
                {
                    // Some lines carry gaps a tab or a cursor move leaves, which a
                    // rewrap can strand at a row's end.
                    std::string line = tokenLine(nextToken++, pick(8, 90));
                    for (int gap = pick(-2, 2); gap > 0; --gap)
                        line.insert(static_cast<std::size_t>(pick(8, static_cast<int>(line.size()))), pick(0, 1) ? "\t" : "\x1b[3C");
                    send("\x1b[" + std::to_string(t->rows) + ";1H\r\n" + line + "\r\n");
                    break;
                }
            case 3:
                send("\x1b[" + std::to_string(pick(1, t->rows)) + ";1H\x1b[" + std::to_string(pick(1, 3)) + "L");
                break;
            case 4:
                send("\x1b[" + std::to_string(pick(1, t->rows)) + ";1H\x1b[" + std::to_string(pick(1, 3)) + "M");
                break;
            case 5: {
                const int top = pick(1, t->rows);
                const int bottom = pick(top, t->rows);
                send("\x1b[" + std::to_string(top) + ";" + std::to_string(bottom) + "r\x1b[" + std::to_string(bottom) + ";1H\n\x1b[r");
                break;
            }
            case 6:
                send("\x1b[" + std::to_string(pick(1, t->rows)) + ";" + std::to_string(pick(1, t->columns)) + "H\x1b[" + std::to_string(pick(0, 2)) + "K");
                break;
            case 7:
                send(pick(0, 3) == 0 ? "\x1b[?1049h" : "\x1b[?1049l");
                break;
            case 8:
                Ubiquitty::resize(*t, t->rows, pick(8, 90));
                break;
            case 9:
                Ubiquitty::resize(*t, pick(3, 16), t->columns);
                break;
            case 10:
                if (pick(0, 9) == 0)
                    send("\x1b[3J");
                else
                    Ubiquitty::Screen::dropScrollbackBefore(t.get(), pick(0, 20));
                break;
            case 11:
                if (pick(0, 19) == 0)
                    send("\x1b"
                         "c");
                break;
            case 13: {
                // A pane with left and right margins scrolled up or down. Its
                // edges never cut a token: the halves of a cut token are two
                // texts, which a single token's place cannot be checked
                // against.
                // A region of one row is no region (DECSTBM wants top above
                // bottom), and neither is a band of one column.
                if (t->rows < 2 || t->columns < 2)
                    break;
                const int top = pick(1, t->rows - 1);
                const int bottom = pick(top + 1, t->rows);
                const int left = pick(1, t->columns - 1);
                const int right = pick(left + 1, t->columns);
                if (cutsAToken(*t, top - 1, bottom - 1, left - 1, right - 1))
                    break;
                const std::string margins = "\x1b[?69h\x1b[" + std::to_string(left) + ";" + std::to_string(right) + "s\x1b[" + std::to_string(top) + ";" + std::to_string(bottom) + "r";
                send(margins + "\x1b[" + std::to_string(pick(0, 1) ? top : bottom) + ";" + std::to_string(left) + "H" + (pick(0, 1) ? "\n" : "\x1bM") + "\x1b[r\x1b[?69l");
                break;
            }
            case 12:
                // Erased first, and no longer than the row, for the same
                // reason.
                send("\x1b[" + std::to_string(pick(1, t->rows)) + ";1H\x1b[2K" + tokenLine(nextToken++, std::min(pick(4, 30), t->columns)));
                break;
            default: {
                // Capture on a token somewhere in the buffer shown.
                const int total = historyRowsOf(*t) + t->rows;
                const int row = pick(0, total - 1);
                const RowRead read = readRow(*t, row);
                for (int column = 0; column + 8 <= read.length; ++column) {
                    const std::string text = textAt(*t, { row, column }, 8);
                    if (text.size() == 8 && text[0] == '<' && text[1] == 'T' && text[7] == '>') {
                        if (const auto position = Ubiquitty::Screen::capturePosition(*t, row, column)) {
                            held.push_back({ *position, text });
                            note("    captured " + text + " at row " + std::to_string(row) + " column " + std::to_string(column) + (position->line.isPinned() ? " pinned" : " derived"));
                        }
                        break;
                    }
                }
                break;
            }
            }
            if (std::getenv("UBQ_POSITION_TRACE") && std::atoi(std::getenv("UBQ_POSITION_TRACE")) == static_cast<int>(seed)) {
                std::fprintf(stderr, "%s\n", trace.back().c_str());
                if (t->lineIdentity && t->lineStore && !t->screens.isUsingAlternateScreen) {
                    // A live derived-name pin must hold the line its name derives.
                    for (const auto& pin : t->lineIdentity->primary.pins) {
                        if (pin.state == Ubiquitty::Screen::PinState::Destroyed || (pin.id & Ubiquitty::Screen::pinnedLineIDBit))
                            continue;
                        const auto number = Ubiquitty::Screen::numberForDerivedName(t->lineIdentity.get(), pin.id);
                        if (!number)
                            continue;
                        const auto rows = Ubiquitty::Screen::logicalLineRows(*t, *number);
                        if (!rows)
                            continue;
                        const std::uint64_t ordinal = t->lineStore->frontOrdinal() + static_cast<std::uint64_t>(rows->firstIndex);
                        if (ordinal != pin.ordinal)
                            std::fprintf(stderr, "    COLLISION pin %llx at %llu, its derived line %llu at %llu\n", static_cast<unsigned long long>(pin.id), static_cast<unsigned long long>(pin.ordinal), static_cast<unsigned long long>(*number), static_cast<unsigned long long>(ordinal));
                    }
                }
                if (t->lineIdentity) {
                    std::string segments;
                    for (const auto& segment : t->lineIdentity->segments)
                        segments += " [" + std::to_string(segment.firstNumber) + " " + std::to_string(segment.bias) + "]";
                    std::fprintf(stderr, "    segments%s minimum %llu head %llu\n", segments.c_str(), static_cast<unsigned long long>(t->lineIdentity->minNumber), static_cast<unsigned long long>(Ubiquitty::Screen::historyAddress(*t).head));
                }
                if (t->lineIdentity) {
                    for (const auto& pin : t->lineIdentity->primary.pins)
                        std::fprintf(stderr, "    pin %llx ordinal %llu offset %u revision %u state %d parent %llx\n", static_cast<unsigned long long>(pin.id), static_cast<unsigned long long>(pin.ordinal), pin.cellOffset, pin.revision, static_cast<int>(pin.state), static_cast<unsigned long long>(pin.parentID));
                }
                const Ubiquitty::LineStore* store = t->lineStore.get();
                if (std::getenv("UBQ_POSITION_ROWS")) {
                    const int history = historyRowsOf(*t);
                    for (int row = std::max(0, history - 4); row < history + t->rows; ++row) {
                        const RowRead read = readRow(*t, row);
                        std::string text;
                        for (int column = 0; column < read.length; ++column)
                            text.push_back(read.cells[column].codePoint >= 0x20 && read.cells[column].codePoint < 0x7F ? static_cast<char>(read.cells[column].codePoint) : '.');
                        std::fprintf(stderr, "    %c%3d |%s|%s\n", row < history ? 'h' : 's', row, text.c_str(), read.isWrapped ? " +" : "");
                    }
                    std::fprintf(stderr, "    cursor %d,%d\n", t->screens.active->cursorY, t->screens.active->cursorX);
                }
                std::fprintf(stderr, "    store front %llu next %llu count %d\n", store ? static_cast<unsigned long long>(store->frontOrdinal()) : 0ull, store ? static_cast<unsigned long long>(store->nextOrdinal()) : 0ull, store ? store->count() : 0);
                for (const Held& entry : held) {
                    const auto place = Ubiquitty::Screen::resolvePosition(*t, entry.position);
                    const auto number = Ubiquitty::Screen::lineNumberOf(*t, entry.position.line);
                    std::fprintf(stderr, "    %s name %llx number %lld cell %u:", entry.token.c_str(), static_cast<unsigned long long>(entry.position.line.value), number ? static_cast<long long>(*number) : -1ll, entry.position.cell);
                    if (place)
                        std::fprintf(stderr, " -> row %d column %d '%s'\n", place->absoluteRow, place->column, textAt(*t, *place, 8).c_str());
                    else
                        std::fprintf(stderr, " -> failure %d\n", static_cast<int>(place.error()));
                }
            }
            for (const Held& entry : held) {
                const auto place = Ubiquitty::Screen::resolvePosition(*t, entry.position);
                if (!place)
                    continue;
                const std::string text = textAt(*t, *place, static_cast<int>(entry.token.size()));
                ++landed;
                if (text != entry.token) {
                    for (std::size_t index = trace.size() > 40 ? trace.size() - 40 : 0; index < trace.size(); ++index)
                        std::fprintf(stderr, "  %s\n", trace[index].c_str());
                    std::fprintf(stderr, "seed %u step %d: %s resolved to '%s'\n", seed, step, entry.token.c_str(), text.c_str());
                    CHECK(text == entry.token);
                    return;
                }
            }
        }
    }
    CHECK_MSG(landed > 0, "the run resolved positions it could read");
}

// Liveness: through output, rewraps and pull-backs alone, every position on
// a line still held resolves.
TEST(outputAndResizesAloneKeepEveryRetainedPosition)
{
    auto t = terminalOf(8, 40, 2000);
    std::vector<std::pair<LogicalPosition, std::string>> held;
    std::mt19937 random(7);
    const auto pick = [&random](int low, int high) { return std::uniform_int_distribution<int>(low, high)(random); };
    int nextToken = 0;
    for (int step = 0; step < 200; ++step) {
        feed(*t, tokenLine(nextToken++, pick(8, 120)) + "\r\n");
        if (step % 7 == 0)
            Ubiquitty::resize(*t, pick(4, 20), pick(10, 100));
        if (step % 5 == 0) {
            const int index = pick(std::max(0, nextToken - 20), nextToken - 1);
            if (find(*t, tokenOf(index)))
                held.push_back({ captureToken(*t, tokenOf(index)), tokenOf(index) });
        }
        for (const auto& [position, token] : held)
            CHECK(resolvesTo(*t, position, token));
    }
}

UBIQUITTY_TEST_MAIN()
