// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_scrollback_fold.cpp — every reader of history answers the same over
// folded blocks as over raw ones.
//
// Two terminals are fed one stream: coloured text, wide characters, combining
// marks, links, DECDWL rows, soft-wrapped lines, prompt marks and blank rows.
// One folds its cold history (Screen::foldColdScrollback). Then the visible
// cells at every view offset, the row marks and attributes, a copy, a search,
// a reflow and the history plane's joined lines are compared between the two.

#include "Harness.h"
#include "SearchFixture.h"
#include "TerminalFixture.h"

#include "Core/LineStore.h"
#include "Core/Protocol/Sync/HistoryPlane.h"
#include "Core/Screen/Addressing.h"
#include "Core/Screen/HistoryLines.h"
#include "Core/Screen/ScrollSearch.h"
#include "Core/Screen/Scrollback.h"
#include "Core/Selection/TerminalSelection.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>

using namespace Ubiquitty;
using UbiquittyTest::feed;

namespace {

constexpr int rows = 24;
constexpr int columns = 80;

std::string historyStream(int lineCount)
{
    std::string bytes;
    for (int i = 0; i < lineCount; ++i) {
        const std::string name = "L" + std::to_string(i);
        switch (i % 10) {
        case 0:
            bytes += "\x1b[31;1mred bold " + name + "\x1b[0m\r\n";
            break;
        case 1:
            bytes += name + " \xe6\xbc\xa2\xe5\xad\x97\xe3\x83\x86\xe3\x82\xad\xe3\x82\xb9\xe3\x83\x88 wide\r\n";
            break;
        case 2:
            bytes += name + " e\xcc\x81 combining caf\xc3\xa9\r\n";
            break;
        case 3:
            bytes += "\x1b]8;;https://example.invalid/" + std::to_string(i) + "\x1b\\" + name + " link\x1b]8;;\x1b\\\r\n";
            break;
        case 4:
            bytes += "\x1b#6" + name + " double width\r\n";
            break;
        case 5:
            bytes += name + std::string(150, 'x') + "\r\n";
            break;
        case 6:
            bytes += "\x1b]133;A\x1b\\" + name + " prompt\r\n";
            break;
        case 7:
            bytes += "\r\n";
            break;
        case 8:
            bytes += "\x1b[38;2;" + std::to_string(i % 256) + ";" + std::to_string((i * 3) % 256) + ";" + std::to_string((i * 7) % 256) + "m" + name
                + "\x1b[48;5;" + std::to_string(i % 256) + "m bg\x1b[0m\r\n";
            break;
        default:
            bytes += name + " \xf0\x9f\x98\x80 emoji\r\n";
            break;
        }
    }
    return bytes;
}

std::unique_ptr<Terminal> terminalFed(const std::string& bytes)
{
    auto t = std::make_unique<Terminal>();
    setScrollbackLimit(t.get(), 4000);
    resize(*t, rows, columns);
    feed(*t, bytes);
    return t;
}

void foldAllCold(Terminal& t)
{
    while (Screen::foldColdScrollback(t, 1)) {
    }
}

bool sameHistory(const Terminal& a, const Terminal& b)
{
    if (Screen::scrollbackCount(&a) != Screen::scrollbackCount(&b))
        return false;
    for (int index = 0; index < Screen::scrollbackCount(&a); ++index) {
        const Screen::ScrollLine* x = Screen::scrollbackLine(&a, index);
        const Screen::ScrollLine* y = Screen::scrollbackLine(&b, index);
        if (!x || !y || x->length != y->length || x->wrapped != y->wrapped || x->lineAttribute != y->lineAttribute || x->mark != y->mark || x->needsShaping != y->needsShaping)
            return false;
        if (x->length > 0 && std::memcmp(x->cells, y->cells, static_cast<std::size_t>(x->length) * sizeof(Cell)) != 0)
            return false;
    }
    return true;
}

bool sameVisible(const Terminal& a, const Terminal& b, int viewOffset)
{
    for (int row = 0; row < rows; ++row) {
        for (int column = 0; column < columns; ++column) {
            const Cell* x = Screen::visibleCell(&a, viewOffset, row, column);
            const Cell* y = Screen::visibleCell(&b, viewOffset, row, column);
            if (std::memcmp(x, y, sizeof(Cell)) != 0)
                return false;
        }
    }
    return true;
}

// Both terminals over the same stream, one with its cold history folded.
struct Pair {
    std::unique_ptr<Terminal> raw;
    std::unique_ptr<Terminal> folded;
};

Pair foldedPair()
{
    const std::string bytes = historyStream(2500);
    Pair pair { terminalFed(bytes), terminalFed(bytes) };
    foldAllCold(*pair.folded);
    return pair;
}

} // namespace

TEST(foldedHistoryReadsAsRawHistory)
{
    const Pair pair = foldedPair();
    const Terminal& raw = *pair.raw;
    const Terminal& folded = *pair.folded;
    CHECK(Screen::scrollbackCount(&raw) > 2000);
    CHECK(folded.lineStore->foldedBlockCount() >= 3);
    CHECK(folded.lineStore->foldedBytes() > 0);
    CHECK(folded.lineStore->arenaBytes() < raw.lineStore->arenaBytes());
    CHECK(sameHistory(raw, folded));
    const int history = Screen::scrollbackCount(&raw);
    for (const int viewOffset : { 0, 1, 23, 100, 257, 400, 1000, history })
        CHECK_MSG(sameVisible(raw, folded, viewOffset), "visibleCell differs between raw and folded history");
    bool sameFacts = true;
    for (int row = 0; row < history + rows; ++row) {
        sameFacts = sameFacts && Screen::rowMark(&raw, row) == Screen::rowMark(&folded, row)
            && Screen::rowLineAttribute(&raw, row) == Screen::rowLineAttribute(&folded, row)
            && Screen::absoluteRowText(raw, row, columns) == Screen::absoluteRowText(folded, row, columns);
    }
    CHECK(sameFacts);
}

TEST(selectionCopiesTheSameTextOverFoldedHistory)
{
    const Pair pair = foldedPair();
    Selection::Session rawSession;
    Selection::init(rawSession);
    Selection::Session foldedSession;
    Selection::init(foldedSession);
    Selection::selectAllText(rawSession, *pair.raw);
    Selection::selectAllText(foldedSession, *pair.folded);
    const std::string all = Selection::selectedText(rawSession, *pair.raw);
    CHECK(!all.empty());
    CHECK(all == Selection::selectedText(foldedSession, *pair.folded));

    // A drag while scrolled into folded history.
    const int viewOffset = 900;
    Selection::init(rawSession);
    Selection::init(foldedSession);
    Selection::beginGesture(rawSession, *pair.raw, 2, 3, 1, viewOffset, {});
    Selection::dragGesture(rawSession, *pair.raw, 15, 40, viewOffset);
    Selection::beginGesture(foldedSession, *pair.folded, 2, 3, 1, viewOffset, {});
    Selection::dragGesture(foldedSession, *pair.folded, 15, 40, viewOffset);
    const std::string dragged = Selection::selectedText(rawSession, *pair.raw);
    CHECK(!dragged.empty());
    CHECK(dragged == Selection::selectedText(foldedSession, *pair.folded));
}

TEST(searchFindsTheSameMatchesOverFoldedHistory)
{
    const Pair pair = foldedPair();
    UbiquittyTest::SearchFixture rawSearch(*pair.raw);
    UbiquittyTest::SearchFixture foldedSearch(*pair.folded);
    const int rawCount = rawSearch.find("link|caf\xc3\xa9|L1[0-9]3");
    const int foldedCount = foldedSearch.find("link|caf\xc3\xa9|L1[0-9]3");
    CHECK(rawCount > 100);
    CHECK_EQ(rawCount, foldedCount);
    const auto rawHits = rawSearch.hits();
    const auto foldedHits = foldedSearch.hits();
    REQUIRE(rawHits.size() == static_cast<std::size_t>(rawCount));
    REQUIRE(rawHits.size() == foldedHits.size());
    for (std::size_t index = 0; index < rawHits.size(); ++index) {
        const auto raw = Screen::resolvePosition(*pair.raw, rawHits[index].start);
        const auto folded = Screen::resolvePosition(*pair.folded, foldedHits[index].start);
        CHECK(raw && folded && *raw == *folded);
        CHECK_EQ(rawHits[index].endCell - rawHits[index].start.cell, foldedHits[index].endCell - foldedHits[index].start.cell);
    }
}

TEST(reflowRebuildsTheSameHistoryFromFoldedBlocks)
{
    const Pair pair = foldedPair();
    resize(*pair.raw, rows, 60);
    resize(*pair.folded, rows, 60);
    CHECK(sameHistory(*pair.raw, *pair.folded));
    bool sameScreen = true;
    for (int row = 0; row < rows; ++row) {
        for (int column = 0; column < 60; ++column)
            sameScreen = sameScreen && std::memcmp(Screen::visibleCell(pair.raw.get(), 0, row, column), Screen::visibleCell(pair.folded.get(), 0, row, column), sizeof(Cell)) == 0;
    }
    CHECK(sameScreen);
    // The rebuilt history folds again and reads the same.
    foldAllCold(*pair.folded);
    CHECK(pair.folded->lineStore->foldedBlockCount() >= 3);
    CHECK(sameHistory(*pair.raw, *pair.folded));
    resize(*pair.raw, 30, 100);
    resize(*pair.folded, 30, 100);
    CHECK(sameHistory(*pair.raw, *pair.folded));
}

TEST(theHistoryPlaneJoinsTheSameLinesOverFoldedHistory)
{
    const Pair pair = foldedPair();
    const Screen::HistoryAddress address = Screen::historyAddress(*pair.raw);
    CHECK(address.head > address.oldest);
    bool sameLines = true;
    int joined = 0;
    for (std::uint64_t number = address.oldest; sameLines && number < address.head; ++number) {
        const auto x = Protocol::Sync::joinedLine(*pair.raw, number);
        const auto y = Protocol::Sync::joinedLine(*pair.folded, number);
        if (!x && !y)
            continue;
        sameLines = x && y && x->lineAttribute == y->lineAttribute && x->mark == y->mark && x->flags == y->flags && x->cells.size() == y->cells.size()
            && (x->cells.empty() || std::memcmp(x->cells.data(), y->cells.data(), x->cells.size() * sizeof(Cell)) == 0);
        ++joined;
    }
    CHECK(sameLines);
    CHECK(joined > 1000);
}

UBIQUITTY_TEST_MAIN()
