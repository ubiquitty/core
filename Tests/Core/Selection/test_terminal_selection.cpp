// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"
#include "SearchFixture.h"

// test_terminal_selection.cpp — the Session facade binding the selection
// primitives to a Terminal (Core/Selection/TerminalSelection.cpp): gesture
// begin/drag through the click-count modes, the copy walk, select-all, the
// viewport spans, and clear. The primitives have their own tests
// (test_selection*); this is the binding every frontend calls instead of
// re-implementing the callback plumbing, and no instrumented test linked it.

#include "Core/CellColor.h"
#include "Core/CellPen.h"
#include "Core/OSC/TextSizing.h"
#include "Core/Screen/Addressing.h"
#include "Core/Screen/LineIdentity.h"
#include "Core/Screen/MarkValues.h"
#include "Core/Screen/ScrollSearch.h"
#include "Core/Screen/Scrollback.h"
#include "Core/Selection/TerminalSelection.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"
#include "PAL/TerminalClients.h"
#include "TerminalFixture.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace Ubiquitty;
using UbiquittyTest::feed;

namespace {

Selection::Session freshSession()
{
    Selection::Session session;
    Selection::init(session);
    return session;
}

std::string dragCopy(Terminal& t, int fromRow, int fromColumn, int toRow, int toColumn)
{
    Selection::Session session = freshSession();
    Selection::beginGesture(session, t, fromRow, fromColumn, /*clickCount*/ 1, 0, {});
    Selection::dragGesture(session, t, toRow, toColumn, 0);
    return Selection::selectedText(session, t);
}

// Select All, then the two things a frontend reads back: what the highlight
// paints for the visible rows, and the text a copy carries.
struct SelectAllOutcome {
    int spanCount;
    Rendering::SelectionRowSpan firstSpan;
    std::string text;
};

SelectAllOutcome selectAllOutcome(Terminal& t)
{
    Selection::Session session = freshSession();
    Selection::selectAllText(session, t);
    Rendering::SelectionRowSpan spans[8] {};
    const int count = Selection::viewportSpans(session, t, 0, spans);
    return { count, spans[0], Selection::selectedText(session, t) };
}

} // namespace

TEST(aResizeDropsARectangleButNotAFlowingSelection)
{
    // A frontend resizing outside the facade samples the epoch first. A
    // flowing selection's ends are positions the rewrap carries, so the drop
    // leaves it; a rectangle's columns mean nothing at another width.
    Terminal t;
    hardReset(t);
    resize(t, 6, 20);
    feed(t, "hello world");

    SelectionState flowing;
    Selection::init(&flowing);
    Selection::begin(&flowing, t.rows, t.columns, 0, 0, SelectionMode::Character);
    Selection::update(&flowing, t.rows, t.columns, 0, 4);
    SelectionState block;
    Selection::init(&block);
    Selection::begin(&block, t.rows, t.columns, 0, 0, SelectionMode::Rectangle);
    Selection::update(&block, t.rows, t.columns, 0, 4);

    // A resize that changes nothing renumbers nothing, and both stay.
    std::uint32_t epochBefore = t.absoluteRowEpoch;
    resize(t, 6, 20);
    CHECK(!Selection::dropSelectionAcrossResize(flowing, epochBefore, t));
    CHECK(!Selection::dropSelectionAcrossResize(block, epochBefore, t));

    epochBefore = t.absoluteRowEpoch;
    resize(t, 6, 12);
    CHECK_MSG(!Selection::dropSelectionAcrossResize(flowing, epochBefore, t), "a flowing selection was dropped by a rewrap that carries it");
    CHECK(Selection::hasSelection(&flowing));
    CHECK_MSG(Selection::dropSelectionAcrossResize(block, epochBefore, t), "a rectangle survived the rewrap that took its columns' meaning");
    CHECK(!Selection::hasSelection(&block));
}

TEST(aSelectionKeepsItsTextThroughNarrowingAndWideningResizes)
{
    Terminal t;
    hardReset(t);
    resize(t, 6, 20);
    feed(t, "pick-me\r\nnot me\r\nnor me");

    Selection::Session session = freshSession();
    Selection::beginGesture(session, t, 0, 0, /*clickCount*/ 1, 0, {});
    Selection::dragGesture(session, t, 0, 6, 0);
    CHECK(Selection::selectedText(session, t) == "pick-me");

    // The rewrap splits the line over two rows and joins it again; the ends
    // name characters, so the copy is the same text at every width.
    for (const int columns : { 8, 5, 20, 3, 17 }) {
        resize(t, 6, columns);
        CHECK_MSG(Selection::hasSelection(session, t), "a resize dropped a selection it can carry");
        const std::string copied = Selection::selectedText(session, t);
        CHECK_MSG(copied == "pick-me", ("at " + std::to_string(columns) + " columns the copy is '" + copied + "'").c_str());
    }
}

TEST(aSelectionInHistoryKeepsItsTextAcrossOutputAndRewraps)
{
    Terminal t;
    hardReset(t);
    setScrollbackLimit(&t, 400);
    resize(t, 4, 30);
    for (int line = 0; line < 30; ++line)
        feed(t, "line " + std::to_string(line) + " of the history\r\n");
    const int history = Screen::scrollbackCount(&t);
    REQUIRE(history > 10);

    // Line 12 is in history: the viewport scrolled back to it.
    Selection::Session session = freshSession();
    const int viewOffset = history - 12;
    Selection::beginGesture(session, t, 0, 0, /*clickCount*/ 1, viewOffset, {});
    Selection::dragGesture(session, t, 0, 6, viewOffset);
    REQUIRE(Selection::selectedText(session, t) == "line 12");

    feed(t, "more\r\nand more\r\n");
    CHECK(Selection::selectedText(session, t) == "line 12");
    for (const int columns : { 9, 40, 12, 30 }) {
        resize(t, 4, columns);
        CHECK(Selection::selectedText(session, t) == "line 12");
    }
}

TEST(aSelectionWhoseLineIsEvictedIsDropped)
{
    Terminal t;
    hardReset(t);
    setScrollbackLimit(&t, 20);
    resize(t, 4, 30);
    for (int line = 0; line < 10; ++line)
        feed(t, "line " + std::to_string(line) + "\r\n");
    Selection::Session session = freshSession();
    Selection::beginGesture(session, t, 0, 0, /*clickCount*/ 1, Screen::scrollbackCount(&t), {});
    Selection::dragGesture(session, t, 0, 5, Screen::scrollbackCount(&t));
    REQUIRE(Selection::selectedText(session, t) == "line 0");

    // Output at a full history evicts it; the selection does not slide onto
    // the lines after it.
    for (int line = 10; line < 60; ++line)
        feed(t, "line " + std::to_string(line) + "\r\n");
    CHECK(!Selection::hasSelection(session, t));
    CHECK(Selection::selectedText(session, t).empty());
}

TEST(aSelectionOnAnErasedLineIsDroppedAndOneOverwrittenIsKept)
{
    Terminal t;
    hardReset(t);
    resize(t, 6, 20);
    feed(t, "keep me\r\nerase me\r\n");

    Selection::Session kept = freshSession();
    Selection::beginGesture(kept, t, 0, 0, 1, 0, {});
    Selection::dragGesture(kept, t, 0, 6, 0);
    Selection::Session erased = freshSession();
    Selection::beginGesture(erased, t, 1, 0, 1, 0, {});
    Selection::dragGesture(erased, t, 1, 7, 0);
    REQUIRE(Selection::selectedText(erased, t) == "erase me");

    // Printing over a line without erasing keeps its selection on the new
    // text; an erase takes the line's characters, and the selection with them.
    feed(t, "\x1b[1;1HKEEP");
    feed(t, "\x1b[2;1H\x1b[2K");
    CHECK(Selection::hasSelection(kept, t));
    CHECK(Selection::selectedText(kept, t) == "KEEP me");
    CHECK(!Selection::hasSelection(erased, t));
}

TEST(aSelectionFollowsInPlaceMovesAndIsDroppedOffTheBand)
{
    Terminal t;
    hardReset(t);
    resize(t, 8, 20);
    feed(t, "one\r\ntwo\r\nthree\r\nfour\r\nfive\r\nsix");

    Selection::Session inside = freshSession();
    Selection::beginGesture(inside, t, 3, 0, 1, 0, {});
    Selection::dragGesture(inside, t, 3, 3, 0);
    REQUIRE(Selection::selectedText(inside, t) == "four");
    Selection::Session atTop = freshSession();
    Selection::beginGesture(atTop, t, 1, 0, 1, 0, {});
    Selection::dragGesture(atTop, t, 1, 2, 0);
    REQUIRE(Selection::selectedText(atTop, t) == "two");

    // An IL above the selected line moves it down; it follows.
    feed(t, "\x1b[1;1H\x1b[L");
    CHECK(Selection::selectedText(inside, t) == "four");
    // A region scroll over rows 3-7 carries "two" (now on row 3) off the band's
    // top; "four" moves up a row within it.
    feed(t, "\x1b[3;7r\x1b[7;1H\n\x1b[r");
    CHECK(Selection::selectedText(inside, t) == "four");
    CHECK(!Selection::hasSelection(atTop, t));
}

TEST(aRectangleSurvivesOutputButNotARewrap)
{
    Terminal t;
    hardReset(t);
    setScrollbackLimit(&t, 100);
    resize(t, 4, 20);
    feed(t, "abcdef\r\nghijkl\r\n");

    Selection::Session session = freshSession();
    Selection::beginGesture(session, t, 0, 1, 1, 0, Selection::GestureStyle { .isRectangular = true });
    Selection::dragGesture(session, t, 1, 2, 0);
    REQUIRE(Selection::hasSelection(session, t));
    const std::string before = Selection::selectedText(session, t);
    REQUIRE(!before.empty());

    feed(t, "x\r\ny\r\nz\r\n");
    CHECK(Selection::selectedText(session, t) == before);
    resize(t, 4, 10);
    CHECK(!Selection::hasSelection(session, t));
}

// A copy reads a cell's base code point directly and asks for the rest only
// when the cell says it has more. The two ways a cell can carry more are the
// combining buffer packed beside the base and, past its capacity, a sequence in
// the cluster pool; both must reach the clipboard whole.
TEST(aCopyKeepsEveryCodePointOfACluster)
{
    Terminal t;
    hardReset(t);
    resize(t, 4, 20);

    // "e" with a combining acute, then a ZWJ sequence long enough to leave the
    // cell's inline buffer and land in the pool.
    const std::string combining = "e\xCC\x81";
    const std::string family = "\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA7\xE2\x80\x8D\xF0\x9F\x91\xA6";
    feed(t, combining + "x" + family);

    Selection::Session session;
    Selection::selectAllText(session, t);
    const std::string copied = Selection::selectedText(session, t);

    CHECK_MSG(copied.find(combining) != std::string::npos, "the combining mark survives the copy");
    CHECK_MSG(copied.find(family) != std::string::npos, "the pooled cluster survives the copy");

    // The control: a cell with a single code point copies as that one code
    // point and gains nothing, so the case above is not passing because every
    // cell now emits a sequence.
    CHECK_MSG(copied.find("x") != std::string::npos, "a plain cell still copies");
    const std::size_t plainCount = [&] {
        std::size_t n = 0;
        for (const char c : copied)
            if (c == 'x')
                n++;
        return n;
    }();
    CHECK_MSG(plainCount == 1, "a plain cell copies once, not once per code point of its neighbours");
}

TEST(aSizedBlockCopiesAsTheOneGlyphItDraws)
{
    // A block draws one glyph over its rows, and every covered row carries the
    // code point so a renderer can crop that row's slice. Copying the block
    // must still yield what the reader sees: one 'A', not one per row.
    Terminal block;
    hardReset(block);
    resize(block, 8, 20);
    block.drawsSizedText = true;
    block.drawsProportionalText = true;
    feed(block, "\x1b[H\x1b]66;s=3;A\x07");

    // The control is the same glyph printed at ordinary size on one row, so
    // the claim is "a block copies like the character it draws, on the one
    // line the reader sees" rather than a guess about how the walk spells
    // the rows under it: the block's lower bands repeat its top and add no
    // line of their own.
    Terminal plain;
    hardReset(plain);
    resize(plain, 8, 20);
    feed(plain, "A\r\nnext");
    feed(block, "\x1b[4;1Hnext");

    CHECK(dragCopy(block, 0, 0, 2, 2) == dragCopy(plain, 0, 0, 0, 2));
    CHECK(dragCopy(block, 0, 0, 3, 3) == dragCopy(plain, 0, 0, 1, 3));
    CHECK(dragCopy(block, 0, 0, 3, 3) == "A\nnext");
}

namespace {

// A sized-text terminal holding `content` from the home cell.
struct SizedScreen {
    Terminal terminal;

    explicit SizedScreen(const std::string& content, int rows = 8, int columns = 24)
    {
        hardReset(terminal);
        resize(terminal, rows, columns);
        terminal.drawsSizedText = true;
        terminal.drawsProportionalText = true;
        feed(terminal, "\x1b[H" + content);
    }
};

// The pieces the highlight paints on one visible row, left to right.
std::vector<std::pair<int, int>> highlightedPiecesOnRow(Selection::Session& session, Terminal& terminal, int visibleRow)
{
    std::vector<Rendering::SelectionRowSpan> spans(static_cast<std::size_t>(Selection::viewportSpanCapacity(terminal.rows, terminal.columns)));
    const int count = Selection::viewportSpans(session, terminal, 0, spans);
    std::vector<std::pair<int, int>> pieces;
    for (int index = 0; index < count; ++index) {
        if (spans[static_cast<std::size_t>(index)].visibleRow == visibleRow)
            pieces.emplace_back(spans[static_cast<std::size_t>(index)].column0, spans[static_cast<std::size_t>(index)].column1);
    }
    return pieces;
}

using Pieces = std::vector<std::pair<int, int>>;

Selection::Session draggedSelection(Terminal& terminal, int fromRow, int fromColumn, int toRow, int toColumn)
{
    Selection::Session session = freshSession();
    Selection::beginGesture(session, terminal, fromRow, fromColumn, /*clickCount*/ 1, 0, {});
    Selection::dragGesture(session, terminal, toRow, toColumn, 0);
    return session;
}

} // namespace

TEST(aSelectionEndingOnABlocksTopBandLightsTheBlocksLowerBandToo)
{
    // Kitty highlights the whole block a selection reaches; the selection's own
    // rows end at the top band, so the band below is added as a piece of its own.
    SizedScreen screen("Ax\x1b]66;s=2;X\x07");
    Selection::Session session = draggedSelection(screen.terminal, 0, 0, 0, 2);
    CHECK(highlightedPiecesOnRow(session, screen.terminal, 0) == Pieces({ { 0, 3 } }));
    CHECK(highlightedPiecesOnRow(session, screen.terminal, 1) == Pieces({ { 2, 3 } }));
    CHECK(highlightedPiecesOnRow(session, screen.terminal, 2).empty());
    CHECK(Selection::selectedText(session, screen.terminal) == "AxX");
}

TEST(aSelectionStartingOnABlocksLowerBandLightsTheTopBandToo)
{
    // "yz" stands right of the block on its top band row. The drag starts on the
    // lower band, so the top band is lit for the block alone: the text beside
    // the block on that row is not between the drag's ends and is not pulled in.
    SizedScreen screen("Ax\x1b]66;s=2;X\x07yz\r\n\n\nline");
    Selection::Session session = draggedSelection(screen.terminal, 1, 3, 3, 2);
    CHECK(highlightedPiecesOnRow(session, screen.terminal, 0) == Pieces({ { 2, 3 } }));
    // The drag starts at the block's anchor column, as a wide glyph's does, so
    // its lower band is the start of the row's own span.
    CHECK(highlightedPiecesOnRow(session, screen.terminal, 1) == Pieces({ { 2, 23 } }));
    CHECK(highlightedPiecesOnRow(session, screen.terminal, 2) == Pieces({ { 0, 23 } }));
    CHECK(highlightedPiecesOnRow(session, screen.terminal, 3) == Pieces({ { 0, 2 } }));
    // The block is copied once, at the lower band the selection begins on.
    const std::string copied = Selection::selectedText(session, screen.terminal);
    CHECK_MSG(copied.substr(0, 1) == "X", "the block's text leads the copy");
    CHECK_MSG(std::count(copied.begin(), copied.end(), 'X') == 1, "the block is copied once");
    CHECK_MSG(copied.find('y') == std::string::npos, "the text beside the block's top band is not copied");
}

TEST(aSelectionPastABlocksRowReachesItsTopBandAcrossAGap)
{
    // The drag starts on the first row to the right of the block and runs down
    // through the row below, which holds the block's lower band. The block is
    // lit on both its rows; the cells between it and the drag's start are not.
    SizedScreen screen("\x1b]66;s=2;X\x07  hello\r\n\r\nworld");
    Selection::Session session = draggedSelection(screen.terminal, 0, 6, 2, 3);
    CHECK(highlightedPiecesOnRow(session, screen.terminal, 0) == Pieces({ { 0, 1 }, { 6, 23 } }));
    CHECK(highlightedPiecesOnRow(session, screen.terminal, 1) == Pieces({ { 0, 23 } }));
    const std::string copied = Selection::selectedText(session, screen.terminal);
    CHECK_MSG(std::count(copied.begin(), copied.end(), 'X') == 1, "the block is copied once");
}

TEST(anExplicitWidthBlockIsLitAcrossAllItsClaimedColumns)
{
    // s=2 with w=3 claims six columns by two rows around a glyph two wide.
    SizedScreen screen("ab\x1b]66;s=2:w=3;Q\x07z");
    Selection::Session session = draggedSelection(screen.terminal, 0, 0, 0, 3);
    CHECK(highlightedPiecesOnRow(session, screen.terminal, 0) == Pieces({ { 0, 7 } }));
    CHECK(highlightedPiecesOnRow(session, screen.terminal, 1) == Pieces({ { 2, 7 } }));
    CHECK(Selection::selectedText(session, screen.terminal) == "abQ");
}

TEST(aTallBlockIsLitOnEveryBandRowTheSelectionDoesNotReach)
{
    SizedScreen screen("\x1b]66;s=4;W\x07", 10, 24);
    Selection::Session session = draggedSelection(screen.terminal, 1, 2, 1, 3);
    for (int row = 0; row < 4; ++row)
        CHECK_MSG(!highlightedPiecesOnRow(session, screen.terminal, row).empty(), "every band row is lit");
    CHECK(highlightedPiecesOnRow(session, screen.terminal, 4).empty());
    CHECK(Selection::selectedText(session, screen.terminal) == "W");
}

TEST(aSelectionThatMissesABlockLeavesItUnlit)
{
    SizedScreen screen("hello  \x1b]66;s=2;X\x07\r\nworld");
    Selection::Session session = draggedSelection(screen.terminal, 0, 0, 0, 3);
    CHECK(highlightedPiecesOnRow(session, screen.terminal, 0) == Pieces({ { 0, 3 } }));
    CHECK(highlightedPiecesOnRow(session, screen.terminal, 1).empty());
}

TEST(aProportionalRunBesideABlockKeepsItsLetterCells)
{
    // A run's cells hold several letters; touching one does not select the rest
    // of the run, whereas the block beside it is taken whole. A guard on the
    // run's own rule: the block rule leaves a run alone, so this holds with or
    // without it.
    const std::string content = "\x1b]66;s=2;X\x07\x1b]66;p=1:c=6;Overview\x07";
    SizedScreen screen(content);
    Selection::Session session = draggedSelection(screen.terminal, 0, 0, 0, 2);
    CHECK(highlightedPiecesOnRow(session, screen.terminal, 0) == Pieces({ { 0, 2 } }));
    CHECK(highlightedPiecesOnRow(session, screen.terminal, 1) == Pieces({ { 0, 1 } }));

    // A drag that starts inside the run touches the block not at all.
    SizedScreen insideTheRun(content);
    Selection::Session inside = draggedSelection(insideTheRun.terminal, 0, 2, 0, 4);
    CHECK(highlightedPiecesOnRow(inside, insideTheRun.terminal, 0) == Pieces({ { 2, 2 } }));
    CHECK(highlightedPiecesOnRow(inside, insideTheRun.terminal, 1).empty());
}

TEST(aWideGlyphBesideABlockIsNotPartOfTheBlock)
{
    // The glyph's continuation cell follows the block's own continuation, and
    // carries no spec: the block's run ends where the glyph begins.
    SizedScreen screen("\x1b]66;s=2;X\x07\xe7\x95\x8c");
    Selection::Session onTheGlyph = draggedSelection(screen.terminal, 0, 2, 0, 3);
    CHECK(highlightedPiecesOnRow(onTheGlyph, screen.terminal, 0) == Pieces({ { 2, 3 } }));
    CHECK_MSG(highlightedPiecesOnRow(onTheGlyph, screen.terminal, 1).empty(), "the block below is not lit by the glyph beside it");
    Selection::Session onTheBlock = draggedSelection(screen.terminal, 0, 0, 0, 1);
    CHECK(highlightedPiecesOnRow(onTheBlock, screen.terminal, 0) == Pieces({ { 0, 1 } }));
    CHECK(highlightedPiecesOnRow(onTheBlock, screen.terminal, 1) == Pieces({ { 0, 1 } }));

    // And on the left, the glyph's cells are not the block's.
    SizedScreen before("\xe7\x95\x8c\x1b]66;s=2;X\x07");
    Selection::Session fromTheGlyph = draggedSelection(before.terminal, 0, 0, 0, 2);
    CHECK(highlightedPiecesOnRow(fromTheGlyph, before.terminal, 0) == Pieces({ { 0, 3 } }));
    CHECK(highlightedPiecesOnRow(fromTheGlyph, before.terminal, 1) == Pieces({ { 2, 3 } }));
}

TEST(aColourGlyphBlockIsLitWholeLikeAnyOther)
{
    SizedScreen screen("\x1b]66;s=2;\xf0\x9f\x98\x80\x07"
                       "ab");
    Selection::Session session = draggedSelection(screen.terminal, 0, 0, 0, 1);
    CHECK(highlightedPiecesOnRow(session, screen.terminal, 0) == Pieces({ { 0, 3 } }));
    CHECK(highlightedPiecesOnRow(session, screen.terminal, 1) == Pieces({ { 0, 3 } }));
}

TEST(aBlockOnADoubleWidthRowIsLitOnTheRowBelowItToo)
{
    // The block's logical columns are the row's, as the drag's are: a double
    // width row lights the same columns, drawn twice as wide.
    SizedScreen screen("\x1b#6\x1b]66;s=2;X\x07yz");
    Selection::Session session = draggedSelection(screen.terminal, 0, 0, 0, 3);
    CHECK(highlightedPiecesOnRow(session, screen.terminal, 0) == Pieces({ { 0, 1 } }));
    CHECK(highlightedPiecesOnRow(session, screen.terminal, 1) == Pieces({ { 0, 1 } }));

    SizedScreen below("\x1b#6\r\n\x1b#6\x1b[H\x1b]66;s=2;X\x07yz");
    Selection::Session fromBelow = draggedSelection(below.terminal, 1, 0, 1, 3);
    CHECK(highlightedPiecesOnRow(fromBelow, below.terminal, 0) == Pieces({ { 0, 1 } }));
    CHECK(highlightedPiecesOnRow(fromBelow, below.terminal, 1) == Pieces({ { 0, 1 } }));
}

TEST(aBlockOnADoubleHeightPairIsLitOnceOnEachHalf)
{
    // Both halves of the pair show the same glyphs, and the pair's mirrored span
    // already covers the block: no piece goes over it, so the highlight is not
    // laid on itself.
    SizedScreen screen("\x1b#3\r\n\x1b#4\x1b[H\x1b]66;s=2;X\x07yz");
    Selection::Session session = draggedSelection(screen.terminal, 0, 0, 0, 3);
    CHECK(highlightedPiecesOnRow(session, screen.terminal, 0) == Pieces({ { 0, 1 } }));
    CHECK(highlightedPiecesOnRow(session, screen.terminal, 1) == Pieces({ { 0, 1 } }));
}

TEST(theEndsOfASelectionAreItsOwnAndNotABlocksPieces)
{
    // A frontend draws its handles on the selection's ends. The spans are the
    // highlight, and a sized block adds pieces to them on rows the selection
    // does not reach: the first and last span are then not the ends.
    SizedScreen lower("Ax\x1b]66;s=2;X\x07yz\r\n\n\nline");
    Selection::Session fromTheLowerBand = draggedSelection(lower.terminal, 1, 3, 3, 2);
    std::vector<Rendering::SelectionRowSpan> spans(static_cast<std::size_t>(Selection::viewportSpanCapacity(lower.terminal.rows, lower.terminal.columns)));
    Selection::ViewportEnds ends;
    const int count = Selection::viewportSpansAndEnds(fromTheLowerBand, lower.terminal, 0, spans, ends);
    REQUIRE(count > 0);
    CHECK_MSG(spans[0].visibleRow == 0, "the block's top band is the first span");
    CHECK(ends.isVisible);
    CHECK_MSG(ends.startRow == 1 && ends.startColumn == 2, "the selection starts on the block's lower band");
    CHECK_MSG(ends.endRow == 3 && ends.endColumn == 2, "and ends where the drag did");

    SizedScreen top("Ax\x1b]66;s=2;X\x07");
    Selection::Session toTheTopBand = draggedSelection(top.terminal, 0, 0, 0, 2);
    const int topCount = Selection::viewportSpansAndEnds(toTheTopBand, top.terminal, 0, spans, ends);
    REQUIRE(topCount > 0);
    CHECK_MSG(spans[static_cast<std::size_t>(topCount) - 1].visibleRow == 1, "the block's lower band is the last span");
    CHECK(ends.isVisible);
    CHECK_MSG(ends.startRow == 0 && ends.startColumn == 0, "the selection starts at the drag");
    CHECK_MSG(ends.endRow == 0 && ends.endColumn == 3, "and ends on the top band, whole block");

    // The count matches the spans-only call.
    CHECK_EQ(Selection::viewportSpans(toTheTopBand, top.terminal, 0, spans), topCount);

    Selection::Session none = freshSession();
    CHECK_EQ(Selection::viewportSpansAndEnds(none, top.terminal, 0, spans, ends), 0);
    CHECK(!ends.isVisible);
}

TEST(aRowOfAdjacentBlocksIsOneHighlightPiece)
{
    // Eleven seven-row blocks side by side on an 8 by 80 screen: the drag lights
    // row 0, and every block is added on the six rows below it. One span a row
    // is enough, so a buffer of two a row never runs out.
    std::string banner;
    for (int block = 0; block < 11; ++block)
        banner += "\x1b]66;s=7;X\x07";
    SizedScreen screen(banner, 8, 80);
    Selection::Session session = draggedSelection(screen.terminal, 0, 0, 0, 79);
    std::vector<Rendering::SelectionRowSpan> spans(static_cast<std::size_t>(2 * screen.terminal.rows));
    const int count = Selection::viewportSpans(session, screen.terminal, 0, spans);
    CHECK_EQ(count, 7);
    for (int row = 0; row < 7; ++row) {
        CHECK_MSG(highlightedPiecesOnRow(session, screen.terminal, row) == Pieces({ { 0, 76 } }), "every band row is lit across all eleven blocks");
    }
    CHECK(highlightedPiecesOnRow(session, screen.terminal, 7).empty());
}

TEST(aRowOfSeparatedBlocksFitsTheCapacityCoreAsksFor)
{
    // Twenty-six blocks with a blank between each: the pieces cannot merge, and
    // the buffer Core sizes for the grid holds all of them.
    std::string banner;
    for (int block = 0; block < 26; ++block)
        banner += "\x1b]66;s=2;X\x07 ";
    SizedScreen screen(banner, 8, 80);
    Selection::Session session = draggedSelection(screen.terminal, 0, 0, 0, 79);
    const Pieces lit = highlightedPiecesOnRow(session, screen.terminal, 1);
    REQUIRE(lit.size() == 26);
    for (int block = 0; block < 26; ++block)
        CHECK(lit[static_cast<std::size_t>(block)] == std::make_pair(3 * block, 3 * block + 1));
    CHECK(Selection::viewportSpanCapacity(screen.terminal.rows, screen.terminal.columns) > 26 + 8);
}

TEST(aBufferTooSmallForTheBlocksHoldsWhatFitsAndNoMore)
{
    std::string banner;
    for (int block = 0; block < 26; ++block)
        banner += "\x1b]66;s=2;X\x07 ";
    SizedScreen screen(banner, 8, 80);
    Selection::Session session = draggedSelection(screen.terminal, 0, 0, 0, 79);
    // A sentinel past the span handed over: nothing is written beyond it.
    std::vector<Rendering::SelectionRowSpan> spans(10, Rendering::SelectionRowSpan { 99, 99, 99, 99, 99 });
    const int count = Selection::viewportSpans(session, screen.terminal, 0, std::span<Rendering::SelectionRowSpan>(spans.data(), 6));
    CHECK(count <= 6);
    CHECK_MSG(count >= 1, "the drag's own span goes in first");
    for (std::size_t index = 6; index < spans.size(); ++index)
        CHECK_MSG(spans[index].visibleRow == 99, "nothing is written past the buffer");
}

TEST(theSpanCapacityIsZeroForAGridWithNoCells)
{
    CHECK_EQ(Selection::viewportSpanCapacity(0, 80), 0);
    CHECK_EQ(Selection::viewportSpanCapacity(24, 0), 0);
    CHECK(Selection::viewportSpanCapacity(24, 80) >= 24);
}

TEST(testCharacterDragSelectsARun)
{
    Terminal t;
    hardReset(t);
    resize(t, 6, 20);
    feed(t, "hello world\r\nsecond line");

    Selection::Session session = freshSession();
    CHECK(!Selection::hasSelection(session));

    Selection::beginGesture(session, t, 0, 0, /*clickCount*/ 1, 0, {});
    Selection::dragGesture(session, t, 0, 4, 0);
    CHECK(Selection::hasSelection(session));
    CHECK(Selection::selectedText(session, t) == "hello");

    // A drag across rows joins through the row break.
    Selection::dragGesture(session, t, 1, 5, 0);
    const std::string acrossRows = Selection::selectedText(session, t);
    CHECK(acrossRows.find("hello world") != std::string::npos);
    CHECK(acrossRows.find("second") != std::string::npos);
}

// A reflowing resize rewraps the rows a selection was made on, pushing the
// target line down so its old row holds different text: the selection's ends
// are positions, and the copy after the resize is the text selected before.
TEST(testAReflowingResizeDoesNotStrandTheSelectionOnOtherRows)
{
    Terminal t;
    hardReset(t);
    resize(t, 6, 20);
    // A full-width logical line that rewraps to two rows when the grid narrows,
    // pushing the target line down so its old row holds different text.
    feed(t, "01234567890123456789\r\nTARGET");

    Selection::Session session = freshSession();
    Selection::beginGesture(session, t, 1, 0, /*clickCount*/ 1, 0, {});
    Selection::dragGesture(session, t, 1, 5, 0);
    const std::string before = Selection::selectedText(session, t);
    REQUIRE(before == "TARGET");

    resize(t, 6, 10); // reflow: the wide line wraps and TARGET moves down a row
    const std::string after = Selection::selectedText(session, t);
    CHECK_MSG(after == before, "a reflowing resize must carry the selection to its own text");
}

TEST(testWordAndLineClicks)
{
    Terminal t;
    hardReset(t);
    resize(t, 6, 20);
    feed(t, "hello world");

    // Double click lands mid-word and takes the word.
    Selection::Session session = freshSession();
    Selection::beginGesture(session, t, 0, 2, /*clickCount*/ 2, 0, {});
    CHECK(Selection::selectedText(session, t) == "hello");

    // Triple click takes the line.
    Selection::Session lineSession = freshSession();
    Selection::beginGesture(lineSession, t, 0, 2, /*clickCount*/ 3, 0, {});
    CHECK(Selection::selectedText(lineSession, t).find("hello world") != std::string::npos);
}

TEST(testClearAndSelectAll)
{
    Terminal t;
    hardReset(t);
    resize(t, 6, 20);
    feed(t, "alpha\r\nbeta");

    Selection::Session session = freshSession();
    // Clearing an empty selection reports nothing to clear.
    CHECK(!Selection::clearSelection(session));

    // Explicit clearing also closes a deferred single-click gesture. Otherwise
    // a later mouse move could resurrect a selection after keyboard input had
    // already canceled it.
    Selection::beginGesture(session, t, 0, 0, 1, 0, {});
    CHECK(session.gesture.isOpen);
    CHECK(!Selection::clearSelection(session));
    CHECK(!session.gesture.isOpen);

    Selection::selectAllText(session, t);
    CHECK(Selection::hasSelection(session));
    const std::string all = Selection::selectedText(session, t);
    CHECK(all.find("alpha") != std::string::npos);
    CHECK(all.find("beta") != std::string::npos);

    CHECK(Selection::clearSelection(session));
    CHECK(!Selection::hasSelection(session));
    CHECK(Selection::selectedText(session, t).empty());
}

// A full-screen program on the primary screen -- Claude Code is one -- leaves
// its first row blank as a rule. Select All takes that row: it is a bare
// newline, the way a blank row anywhere else in the selection is. It was the
// one row the highlight left out, and only while the buffer held no history: a
// character selection from (0, 0) meets the top-edge void rule, which starts a
// drag after the empty row it began on, while with one row of scrollback the
// visible top row is an interior row of the selection and paints whole. That
// is the "sometimes" in the report.
TEST(selectAllTakesABlankTopRowHoweverItCameToBeBlank)
{
    // Never written: the row holds the blank sentinel.
    {
        Terminal t;
        hardReset(t);
        resize(t, 4, 20);
        feed(t, "\r\nalpha\r\nbeta");
        const SelectAllOutcome all = selectAllOutcome(t);
        CHECK_EQ(all.spanCount, 4);
        CHECK_EQ(all.firstSpan.visibleRow, 0);
        CHECK_EQ(all.firstSpan.column0, 0);
        CHECK_EQ(all.firstSpan.column1, 19);
        CHECK_MSG(all.text == "\nalpha\nbeta\n", "the copy carries the never-written top row as a leading newline");
    }

    // Erased: the program wrote the row and cleared it, the way a redraw does.
    {
        Terminal t;
        hardReset(t);
        resize(t, 4, 20);
        feed(t, "stale\r\nalpha\r\nbeta\x1b[H\x1b[2K");
        const SelectAllOutcome all = selectAllOutcome(t);
        CHECK_EQ(all.spanCount, 4);
        CHECK_EQ(all.firstSpan.visibleRow, 0);
        CHECK_EQ(all.firstSpan.column0, 0);
        CHECK_EQ(all.firstSpan.column1, 19);
        CHECK_MSG(all.text == "\nalpha\nbeta\n", "the copy carries the erased top row as a leading newline");
    }

    // The control: a row of typed spaces is a printed row to the void rule and
    // was never dropped, so the cases above are measured against a row the
    // walk already agreed on.
    {
        Terminal t;
        hardReset(t);
        resize(t, 4, 20);
        feed(t, "     \r\nalpha\r\nbeta");
        const SelectAllOutcome all = selectAllOutcome(t);
        CHECK_EQ(all.spanCount, 4);
        CHECK_EQ(all.firstSpan.visibleRow, 0);
        CHECK_EQ(all.firstSpan.column0, 0);
        CHECK_EQ(all.firstSpan.column1, 19);
        CHECK_MSG(all.text == "\nalpha\nbeta\n", "a row of spaces copies as a bare newline");
    }

    // With history the same blank visible top row is an interior row and
    // paints whole, and the copy carries the history row before it.
    {
        Terminal t;
        hardReset(t);
        resize(t, 4, 20);
        feed(t, "first\r\n\r\nalpha\r\nbeta\r\n");
        CHECK_EQ(Screen::scrollbackCount(&t), 1);
        const SelectAllOutcome all = selectAllOutcome(t);
        CHECK_EQ(all.spanCount, 4);
        CHECK_EQ(all.firstSpan.visibleRow, 0);
        CHECK_EQ(all.firstSpan.column0, 0);
        CHECK_EQ(all.firstSpan.column1, 19);
        CHECK_MSG(all.text == "first\n\nalpha\nbeta\n", "the copy runs from the history row through the blank visible top row");
    }
}

TEST(aShiftClickAfterSelectAllTrimsToTheCellAndKeepsTheBlankTopRow)
{
    // Select All is a character selection: the Shift-click ends it at the cell
    // clicked, not at the end of that row, and the blank top row it began on
    // stays painted through the trim.
    Terminal t;
    hardReset(t);
    resize(t, 4, 20);
    feed(t, "\r\nalpha\r\nbeta");
    Selection::Session session = freshSession();
    Selection::selectAllText(session, t);

    Selection::GestureStyle trim;
    trim.mode = Selection::GestureMode::Extend;
    Selection::beginGesture(session, t, /*visibleRow*/ 2, /*column*/ 2, /*clickCount*/ 1, 0, trim);
    Selection::endGesture(session);

    CHECK_MSG(Selection::selectedText(session, t) == "\nalpha\nbet", "the trim ends at the cell clicked");
    Rendering::SelectionRowSpan spans[8] {};
    CHECK_EQ(Selection::viewportSpans(session, t, 0, spans), 3);
    CHECK_EQ(spans[0].visibleRow, 0);
    CHECK_EQ(spans[0].column0, 0);
    CHECK_EQ(spans[0].column1, 19);
    CHECK_EQ(spans[2].visibleRow, 2);
    CHECK_EQ(spans[2].column1, 2);
}

TEST(aPressAfterSelectAllDragsByThePointersRuleAgain)
{
    // The fact Select All records is the command's, not the row's: the next
    // press on the blank top row sits in its void, and the drag starts below.
    Terminal t;
    hardReset(t);
    resize(t, 4, 20);
    feed(t, "\r\nalpha\r\nbeta");
    Selection::Session session = freshSession();
    Selection::selectAllText(session, t);

    Selection::beginGesture(session, t, /*visibleRow*/ 0, /*column*/ 3, /*clickCount*/ 1, 0, {});
    Selection::dragGesture(session, t, /*visibleRow*/ 1, /*column*/ 2, 0);
    Selection::endGesture(session);

    Rendering::SelectionRowSpan spans[8] {};
    CHECK_EQ(Selection::viewportSpans(session, t, 0, spans), 1);
    CHECK_EQ(spans[0].visibleRow, 1);
    CHECK_EQ(spans[0].column0, 0);
    CHECK_EQ(spans[0].column1, 2);
}

// Claude Code composes each redraw inside a DECSET 2026 block. A Select All
// made while a block is open, followed by the redraw that closes it, still
// covers every row and copies the redrawn text: synchronised output withholds
// frames and renumbers nothing, so the selection's epoch stands.
TEST(aSelectAllInsideASynchronisedOutputBlockSurvivesTheRedraw)
{
    Terminal t;
    hardReset(t);
    resize(t, 4, 20);
    feed(t, "\r\nalpha\r\nbeta");
    feed(t, "\x1b[?2026h");

    Selection::Session session = freshSession();
    Selection::selectAllText(session, t);
    feed(t, "\x1b[H\x1b[2K\x1b[B\x1b[2Kgamma\x1b[B\r\x1b[2Kdelta\x1b[?2026l");

    CHECK_MSG(Selection::hasSelection(session, t), "the redraw inside the block kept the selection");
    Rendering::SelectionRowSpan spans[8] {};
    CHECK_EQ(Selection::viewportSpans(session, t, 0, spans), 4);
    CHECK_EQ(spans[0].visibleRow, 0);
    CHECK_EQ(spans[0].column0, 0);
    CHECK_EQ(spans[0].column1, 19);
    CHECK_MSG(Selection::selectedText(session, t) == "\ngamma\ndelta\n", "the copy carries the redrawn rows behind the blank top row");
}

TEST(testRectangularSelectionKeepsRowBreaks)
{
    Terminal t;
    hardReset(t);
    resize(t, 6, 20);
    feed(t, "abcdef\r\nghijkl\r\nmnopqr");

    Selection::Session session = freshSession();
    Selection::GestureStyle style;
    style.isRectangular = true;
    Selection::beginGesture(session, t, 0, 1, 1, 0, style);
    Selection::dragGesture(session, t, 2, 3, 0);
    CHECK(Selection::hasSelection(session));

    // The exact column block, byte for byte: a LINEAR drag over the same
    // endpoints contains every one of these rows as a substring too (it
    // carries the row remainders with them), so only exact equality can tell
    // the two shapes apart. This is the assertion that catches a style
    // dropped between the press and the deferred open on the first drag —
    // a single click opens nothing, so the drag is where the rectangle
    // either takes effect or silently degrades to a stream.
    CHECK(Selection::isRectangular(&session.state));
    const std::string block = Selection::selectedText(session, t);
    CHECK(block == "bcd\nhij\nnop");
}

TEST(testViewportSpans)
{
    Terminal t;
    hardReset(t);
    resize(t, 6, 20);
    feed(t, "alpha\r\nbeta\r\ngamma");

    Selection::Session session = freshSession();

    // No selection: no spans, and the buffer is untouched.
    Rendering::SelectionRowSpan spans[8];
    CHECK_EQ(Selection::viewportSpans(session, t, 0, spans), 0);

    Selection::beginGesture(session, t, 0, 0, 1, 0, {});
    Selection::dragGesture(session, t, 2, 4, 0);
    const int count = Selection::viewportSpans(session, t, 0, spans);
    CHECK_EQ(count, 3); // one span per selected row
    for (int index = 0; index < count; ++index) {
        CHECK(spans[index].visibleRow >= 0 && spans[index].visibleRow < t.rows);
        CHECK(spans[index].column0 >= 0 && spans[index].column1 < t.columns);
    }
}

TEST(testCharacterSelectionOverVisibleRows)
{
    Terminal t;
    hardReset(t);
    resize(t, 4, 20);

    // No history: the two row spaces coincide, so this leg cannot tell a
    // conversion from its absence. It is the control for the one below.
    SelectionState fresh = Selection::characterSelectionOverVisibleRows(t, SelectionBounds { 0, 2, 1, 5 });
    CHECK(fresh.active);
    CHECK(fresh.mode == SelectionMode::Character);
    CHECK_EQ(fresh.anchorRow, 0);
    CHECK_EQ(fresh.anchorColumn, 2);
    CHECK_EQ(fresh.headRow, 1);
    CHECK_EQ(fresh.headColumn, 5);

    // Scroll rows off the top so the viewport no longer starts at absolute
    // row zero. Now an unconverted row names a row of history instead of the
    // row on screen, and the offset is what separates the two.
    feed(t, "one\r\ntwo\r\nthree\r\nfour\r\nfive\r\nsix\r\nseven\r\n");
    const int history = Screen::scrollbackCount(&t);
    CHECK(history > 0);
    SelectionState scrolled = Selection::characterSelectionOverVisibleRows(t, SelectionBounds { 0, 0, 1, 3 });
    CHECK_EQ(scrolled.anchorRow, history);
    CHECK_EQ(scrolled.headRow, history + 1);

    // The alternate screen has no history of its own, and the primary's
    // survives underneath it. A conversion that reads the raw scrollback count
    // pushes the rectangle that many rows below the viewport — every row of
    // the screen the caller was pointing at.
    feed(t, "\033[?1049h");
    CHECK(t.screens.isUsingAlternateScreen);
    SelectionState alternate = Selection::characterSelectionOverVisibleRows(t, SelectionBounds { 0, 0, 1, 3 });
    CHECK_EQ(alternate.anchorRow, 0);
    CHECK_EQ(alternate.headRow, 1);
}

// A triple click takes the logical line, not the visual row it landed on.
//
// A soft-wrapped line occupies several rows, and selecting the row alone hands
// back a fragment of what the user pointed at. For the case that motivates
// this -- a wrapped URL -- a fragment is worse than an empty selection, because
// it still looks like a URL and pastes as one.
//
// 20 columns and a 46-character line, so the line occupies three rows and the
// middle row is neither end: clicking there proves the walk goes both ways
// rather than only forward from the click.
TEST(aTripleClickTakesTheWholeSoftWrappedLine)
{
    Terminal t;
    hardReset(t);
    resize(t, 6, 20);
    feed(t, "https://example.com/a/very/long/path/that/wraps");

    Selection::Session session = freshSession();
    Selection::beginGesture(session, t, /*visibleRow*/ 1, /*column*/ 3, /*clickCount*/ 3, 0, {});
    const std::string selected = Selection::selectedText(session, t);

    CHECK_MSG(selected.find("https://example.com") != std::string::npos,
        "the selection reaches back to the line's first row");
    CHECK_MSG(selected.find("that/wraps") != std::string::npos,
        "and forward to its last");
}

TEST(aTripleClickDragTakesWholeSoftWrappedLines)
{
    Terminal t;
    hardReset(t);
    resize(t, 8, 8);
    feed(t, "UPPER-AAAAA\r\nMIDDLE-BBBBB\r\nLOWER-CCCCCC");

    Selection::Session session = freshSession();
    Selection::beginGesture(session, t, /*visibleRow*/ 3, /*column*/ 2, /*clickCount*/ 3, 0, {});

    SelectionBounds bounds;
    CHECK(Selection::getBounds(&session.state, t.rows, t.columns, nullptr, nullptr, &bounds));
    CHECK_EQ(bounds.row0, 2);
    CHECK_EQ(bounds.row1, 3);

    // The pointer only has to enter the target logical line. Its continuation
    // rows and every row of the triple-clicked line remain selected.
    Selection::dragGesture(session, t, /*visibleRow*/ 4, /*column*/ 0, 0);
    CHECK(Selection::getBounds(&session.state, t.rows, t.columns, nullptr, nullptr, &bounds));
    CHECK_EQ(bounds.row0, 2);
    CHECK_EQ(bounds.row1, 5);

    // Reversing direction uses the far edge of the anchor run, rather than
    // collapsing that run to the physical row where the press landed.
    Selection::dragGesture(session, t, /*visibleRow*/ 1, /*column*/ 0, 0);
    CHECK(Selection::getBounds(&session.state, t.rows, t.columns, nullptr, nullptr, &bounds));
    CHECK_EQ(bounds.row0, 0);
    CHECK_EQ(bounds.row1, 3);

    Selection::dragGesture(session, t, /*visibleRow*/ 2, /*column*/ 0, 0);
    CHECK(Selection::getBounds(&session.state, t.rows, t.columns, nullptr, nullptr, &bounds));
    CHECK_EQ(bounds.row0, 2);
    CHECK_EQ(bounds.row1, 3);
}

TEST(anOpenCharacterGestureFollowsOutputUnderAStationaryPointer)
{
    Terminal t;
    hardReset(t);
    resize(t, 4, 12);
    feed(t, "anchor\r\nmiddle\r\npointer");

    Selection::Session session = freshSession();
    Selection::beginGesture(session, t, /*visibleRow*/ 0, /*column*/ 0, /*clickCount*/ 1, 0, {});
    Selection::dragGesture(session, t, /*visibleRow*/ 2, /*column*/ 6, 0);

    // Two line feeds move the selected content up one visible row. The mouse
    // has not moved: its retained row now points at "new", so the open gesture
    // grows to that row while its anchor remains on "anchor" in history.
    feed(t, "\r\nnew\r\nmore");
    const Selection::OutputUpdate update = Selection::updateAfterOutput(
        session, t, 0, Input::OutputSnapPolicy::HoldViewedRows);

    CHECK(update.viewport.hold == Input::ViewportHold::Follow);
    CHECK_EQ(update.viewport.viewOffset, 0);
    CHECK(update.didChangeSelection);
    const std::string selected = Selection::selectedText(session, t);
    CHECK_MSG(selected.find("anchor") != std::string::npos, "the press remains attached to its buffer character");
    CHECK_MSG(selected.find("new") != std::string::npos, "the head extends to the stationary pointer");
    CHECK_MSG(selected.find("more") == std::string::npos, "the head stops at the pointer row");
}

TEST(anOpenCaretGestureFollowsOutputUnderAStationaryPointer)
{
    Terminal t;
    hardReset(t);
    resize(t, 4, 12);
    feed(t, "anchor\r\nmiddle\r\npointer");

    Selection::Session session = freshSession();
    Selection::beginGesture(session, t, /*visibleRow*/ 0, /*column*/ 0, /*clickCount*/ 1, 0, {});
    Selection::configureCharacterCaretGesture(session, t, Selection::CaretResolution { .absoluteRow = 0, .caret = 0, .slot = 0 });
    Selection::dragCharacterCaretGesture(session, t, Selection::CaretResolution { .absoluteRow = 2, .caret = 7, .slot = 7, .caretAfter = 7 }, 0);

    feed(t, "\r\nnew\r\nmore");
    const Selection::OutputUpdate update = Selection::updateAfterOutput(
        session, t, 0, Input::OutputSnapPolicy::HoldViewedRows);
    CHECK(update.didChangeSelection);

    const std::string selected = Selection::selectedText(session, t);
    CHECK_MSG(selected.find("anchor") != std::string::npos, "the caret press remains attached to its character");
    CHECK_MSG(selected.find("new") != std::string::npos, "the caret head follows the stationary pointer");
    CHECK_MSG(selected.find("more") == std::string::npos, "the half-open selection stops at the pointer row");
}

TEST(anOpenGestureTracksCharactersAcrossScrollbackEviction)
{
    Terminal t;
    hardReset(t);
    t.scrollbackLimit = 3;
    resize(t, 3, 12);
    Screen::dropScrollback(&t);
    feed(t, "\033[Hzero\r\nanchor\r\npointer\r\nbase");
    CHECK_EQ(Screen::scrollbackCount(&t), 1);

    Selection::Session session = freshSession();
    Selection::beginGesture(session, t, /*visibleRow*/ 0, /*column*/ 0, /*clickCount*/ 1, 0, {});
    Selection::dragGesture(session, t, /*visibleRow*/ 1, /*column*/ 6, 0);

    // The third append fills the ring and evicts "zero". Every surviving
    // absolute row is renumbered by one even though the retained count only
    // grew by two.
    feed(t, "\r\none\r\ntwo\r\nthree");
    CHECK_EQ(Screen::scrollbackCount(&t), 3);
    const Selection::OutputUpdate update = Selection::updateAfterOutput(
        session, t, 0, Input::OutputSnapPolicy::HoldViewedRows);
    CHECK(update.didChangeSelection);

    const std::string selected = Selection::selectedText(session, t);
    CHECK_MSG(selected.find("anchor") != std::string::npos, "front eviction rebases the surviving press character");
    CHECK_MSG(selected.find("two") != std::string::npos, "the head still reaches the stationary pointer");
    CHECK_MSG(selected.find("three") == std::string::npos, "the head does not drift to the live cursor row");
}

TEST(anOutputDrivenWordDragStillTakesWholeWords)
{
    Terminal t;
    hardReset(t);
    resize(t, 4, 12);
    feed(t, "alpha one\r\nbravo two\r\ncharlie x");

    Selection::Session session = freshSession();
    Selection::beginGesture(session, t, /*visibleRow*/ 0, /*column*/ 2, /*clickCount*/ 2, 0, {});
    Selection::dragGesture(session, t, /*visibleRow*/ 1, /*column*/ 2, 0);

    feed(t, "\r\ndelta y\r\necho z");
    const Selection::OutputUpdate update = Selection::updateAfterOutput(
        session, t, 0, Input::OutputSnapPolicy::HoldViewedRows);
    CHECK(update.didChangeSelection);

    const std::string selected = Selection::selectedText(session, t);
    CHECK_MSG(selected.find("charlie") != std::string::npos, "the moved head expands to the whole word under the pointer");
    CHECK_MSG(selected.find("delta") == std::string::npos, "word expansion does not cross into the next row");
}

TEST(anOutputDrivenLineDragStillTakesWholeWrappedRuns)
{
    Terminal t;
    hardReset(t);
    resize(t, 6, 8);
    feed(t, "top\r\nmiddle\r\nTARGET-AAAAA");

    Selection::Session session = freshSession();
    Selection::beginGesture(session, t, /*visibleRow*/ 0, /*column*/ 1, /*clickCount*/ 3, 0, {});
    Selection::dragGesture(session, t, /*visibleRow*/ 3, /*column*/ 1, 0);

    feed(t, "\r\nNEWLINE-BBBB\r\nlast");
    const Selection::OutputUpdate update = Selection::updateAfterOutput(
        session, t, 0, Input::OutputSnapPolicy::HoldViewedRows);
    CHECK(update.didChangeSelection);

    const std::string selected = Selection::selectedText(session, t);
    CHECK_MSG(selected.find("NEWLINE-BBBB") != std::string::npos,
        "the target's complete wrapped run is selected");
    CHECK_MSG(selected.find("last") == std::string::npos,
        "the selection stops at the end of the wrapped run under the pointer");
}

TEST(anOutputDrivenRectangularDragKeepsItsColumnBlock)
{
    Terminal t;
    hardReset(t);
    resize(t, 4, 12);
    feed(t, "anchor\r\nmiddle\r\npointer");

    Selection::Session session = freshSession();
    Selection::GestureStyle style;
    style.isRectangular = true;
    Selection::beginGesture(session, t, /*visibleRow*/ 0, /*column*/ 0, /*clickCount*/ 1, 0, style);
    Selection::dragGesture(session, t, /*visibleRow*/ 2, /*column*/ 5, 0);
    CHECK(Selection::isRectangular(&session.state));

    // The retained style rides on `session.gestureStyle`, read fresh on every
    // replay in `updateAfterOutput` -- this is the field a copy taken only at
    // `beginGesture` time could drop silently, since a rectangular selection
    // and a linear one over the same endpoints share every row as a substring
    // and only the block shape tells them apart.
    feed(t, "\r\nnew\r\nmore");
    const Selection::OutputUpdate update = Selection::updateAfterOutput(
        session, t, 0, Input::OutputSnapPolicy::HoldViewedRows);
    CHECK(update.didChangeSelection);

    CHECK(Selection::isRectangular(&session.state));
    const std::string selected = Selection::selectedText(session, t);
    CHECK_MSG(selected.find("anchor") != std::string::npos, "the anchor row's block remains, from history");
    CHECK_MSG(selected.find("new") != std::string::npos, "the moved head's row is in the block");
    CHECK_MSG(selected.find("more") == std::string::npos, "the block does not reach past the retained row");
}

TEST(aReleasedGestureDoesNotGrowWithOutput)
{
    Terminal t;
    hardReset(t);
    resize(t, 4, 12);
    feed(t, "anchor\r\nmiddle\r\npointer");

    Selection::Session session = freshSession();
    Selection::beginGesture(session, t, 0, 0, 1, 0, {});
    Selection::dragGesture(session, t, 2, 6, 0);
    CHECK(Selection::endGesture(session));

    feed(t, "\r\nnew\r\nmore");
    const Selection::OutputUpdate update = Selection::updateAfterOutput(
        session, t, 0, Input::OutputSnapPolicy::HoldViewedRows);
    CHECK(!update.didChangeSelection);
    CHECK_MSG(Selection::selectedText(session, t).find("new") == std::string::npos,
        "a standing selection remains attached to its final characters");
}

TEST(aSelectionStaysOnItsTextWhenAFrameReproducesItself)
{
    Terminal t;
    hardReset(t);
    resize(t, 4, 12);
    feed(t, "alpha\r\nbravo\r\ncharlie\r\ndelta");

    Selection::Session session = freshSession();
    Selection::beginGesture(session, t, /*visibleRow*/ 1, /*column*/ 0, /*clickCount*/ 1, 0, {});
    Selection::dragGesture(session, t, /*visibleRow*/ 1, /*column*/ 4, 0);
    CHECK(Selection::endGesture(session));
    CHECK_MSG(Selection::selectedText(session, t) == "bravo", "the press and drag took the second row");
    Selection::OutputUpdate update = Selection::updateAfterOutput(session, t, 0, Input::OutputSnapPolicy::HoldViewedRows);
    CHECK(!update.didChangeSelection);
    CHECK_MSG(session.hasScreenObservation, "a standing selection is observed with its screen digest");
    const std::uint64_t digestBefore = session.observedScreenDigest;

    // A renderer's tick: the line feed on the bottom row scrolls "alpha" into
    // history, then the cursor climbs and paints the same four rows back,
    // erasing each row before it rewrites it.
    feed(t, "\r\n\x1b[3A\x1b[2Kalpha\r\n\x1b[2Kbravo\r\n\x1b[2Kcharlie\r\n\x1b[2Kdelta");
    CHECK_EQ(Screen::scrollbackCount(&t), 1);
    update = Selection::updateAfterOutput(session, t, 0, Input::OutputSnapPolicy::HoldViewedRows);
    CHECK_MSG(session.observedScreenDigest == digestBefore, "a repainted screen digests as the screen it repainted");
    CHECK(update.didChangeSelection);
    CHECK_MSG(Selection::selectedText(session, t) == "bravo", "a frame that repainted itself left the selection on its text");

    // Genuine output: the screen changes, and the anchors keep their rows,
    // which is the text they were on.
    feed(t, "\r\nnew");
    CHECK_EQ(Screen::scrollbackCount(&t), 2);
    update = Selection::updateAfterOutput(session, t, 0, Input::OutputSnapPolicy::HoldViewedRows);
    CHECK(!update.didChangeSelection);
    CHECK_MSG(Selection::selectedText(session, t) == "bravo", "genuine output left the anchors on their rows");
}

TEST(aSelectionInHistoryStaysOnItsTextWhenAFrameReproducesItself)
{
    Terminal t;
    hardReset(t);
    resize(t, 4, 12);
    feed(t, "alpha\r\nbravo\r\ncharlie\r\ndelta");

    Selection::Session session = freshSession();
    Selection::beginGesture(session, t, /*visibleRow*/ 1, /*column*/ 0, /*clickCount*/ 1, 0, {});
    Selection::dragGesture(session, t, /*visibleRow*/ 1, /*column*/ 4, 0);
    CHECK(Selection::endGesture(session));
    CHECK_MSG(Selection::selectedText(session, t) == "bravo", "the press and drag took the second row");

    // Output carries the selected row off the screen. A history row keeps the
    // absolute number it had on the screen, so the selection is on the same
    // characters from a row below the viewport.
    feed(t, "\r\nnew1\r\nnew2\r\nnew3\r\nnew4");
    (void)Selection::updateAfterOutput(session, t, 0, Input::OutputSnapPolicy::HoldViewedRows);
    CHECK_MSG(Selection::selectedText(session, t) == "bravo",
        "output that carried the selection into history left it on its text");

    // The same renderer's tick as the test above: one row scrolls and the
    // screen paints itself back, so it digests as the screen it repainted.
    // The rows of that screen are a row deeper in the buffer; "bravo", which
    // the repaint never touched, is where it was.
    feed(t, "\r\n\x1b[3A\x1b[2Knew1\r\n\x1b[2Knew2\r\n\x1b[2Knew3\r\n\x1b[2Knew4");
    (void)Selection::updateAfterOutput(session, t, 0, Input::OutputSnapPolicy::HoldViewedRows);
    CHECK_MSG(Selection::selectedText(session, t) == "bravo",
        "a repainted frame walked a selection standing in history off its text");
}

TEST(anOpenGesturePressedInHistoryKeepsItsAnchorWhenAFrameReproducesItself)
{
    Terminal t;
    hardReset(t);
    resize(t, 4, 12);
    feed(t, "one\r\ntwo\r\nthree\r\nfour\r\nfive\r\nsix\r\nseven\r\neight");
    CHECK_EQ(Screen::scrollbackCount(&t), 4);

    // Scrolled back two rows, the top of the view is "three" -- absolute row
    // 2, a row of history. The press lands there and the drag reaches "five".
    Selection::Session session = freshSession();
    const int viewOffset = 2;
    Selection::beginGesture(session, t, /*visibleRow*/ 0, /*column*/ 0, /*clickCount*/ 1, viewOffset, {});
    Selection::dragGesture(session, t, /*visibleRow*/ 2, /*column*/ 3, viewOffset);
    CHECK_EQ(session.gestureAnchorAbsoluteRow, 2);
    CHECK_MSG(Selection::selectedText(session, t).rfind("three", 0) == 0, "the drag starts at the pressed characters");

    feed(t, "\r\n\x1b[3A\x1b[2Kfive\r\n\x1b[2Ksix\r\n\x1b[2Kseven\r\n\x1b[2Keight");
    (void)Selection::updateAfterOutput(session, t, viewOffset, Input::OutputSnapPolicy::HoldViewedRows);
    CHECK_MSG(Selection::selectedText(session, t).rfind("three", 0) == 0,
        "a repainted frame moved the press off the character it went down on");
}

TEST(aBufferClearCancelsEveryOpenClickGesture)
{
    for (const int clickCount : { 1, 2, 3 }) {
        Terminal t;
        hardReset(t);
        resize(t, 4, 12);
        feed(t, "anchor\r\nmiddle\r\npointer");

        Selection::Session session = freshSession();
        Selection::beginGesture(session, t, /*visibleRow*/ 0, /*column*/ 2, clickCount, 0, {});
        if (clickCount == 1) {
            Selection::dragGesture(session, t, /*visibleRow*/ 2, /*column*/ 6, 0);
        }
        REQUIRE(Selection::hasSelection(session));

        // Cmd-K preserves at most the input row and replaces the absolute-row
        // coordinate space. The old press cannot be mapped truthfully, so the
        // next Core observation cancels the gesture rather than attaching it
        // to a new character under the same window coordinate.
        clearBufferPreserveInput(&t);
        feed(t, "replacement\r\noutput");
        (void)Selection::updateAfterOutput(
            session, t, 0, Input::OutputSnapPolicy::HoldViewedRows);

        CHECK(!Selection::hasSelection(session));
        CHECK(!session.gesture.isOpen);
    }
}

TEST(aHardResetCancelsEveryOpenClickGesture)
{
    for (const int clickCount : { 1, 2, 3 }) {
        Terminal t;
        hardReset(t);
        resize(t, 4, 12);
        feed(t, "anchor\r\nmiddle\r\npointer");

        Selection::Session session = freshSession();
        Selection::beginGesture(session, t, /*visibleRow*/ 0, /*column*/ 2, clickCount, 0, {});
        if (clickCount == 1) {
            Selection::dragGesture(session, t, /*visibleRow*/ 2, /*column*/ 6, 0);
        }
        REQUIRE(Selection::hasSelection(session));

        // RIS reconstructs the entire Core terminal in place. Its replacement
        // epoch makes the same cancellation work even though the pointer and
        // Selection::Session themselves outlive that reconstruction.
        feed(t, "\033c");
        (void)Selection::updateAfterOutput(
            session, t, 0, Input::OutputSnapPolicy::HoldViewedRows);

        CHECK(!Selection::hasSelection(session));
        CHECK(!session.gesture.isOpen);
    }
}

// A hard-wrapped line is still one row, which is the truthful answer.
//
// tmux emits each visual row as its own write, so the terminal is never told
// the rows are continuous and nothing marks them wrapped. Joining them anyway
// would guess, and a wrong guess concatenates two unrelated lines into one
// selection. Two feeds separated by a newline are exactly that shape.
TEST(aTripleClickOnAnUnmarkedRowTakesThatRowAlone)
{
    Terminal t;
    hardReset(t);
    resize(t, 6, 20);
    feed(t, "first line here\r\nsecond line here\r\n");

    Selection::Session session = freshSession();
    Selection::beginGesture(session, t, /*visibleRow*/ 1, /*column*/ 2, /*clickCount*/ 3, 0, {});
    const std::string selected = Selection::selectedText(session, t);

    CHECK_MSG(selected.find("second line") != std::string::npos, "the clicked row is selected");
    CHECK_MSG(selected.find("first line") == std::string::npos,
        "and the row above it, which no flag joins, is not");
}

TEST(aLongProportionalRunCopiesItsWholeText)
{
    // A run longer than sixteen code points chunks its text across its cells,
    // and the copy must read every chunk once: the sentence whole, and not
    // once per cell that carries a piece of it.
    Terminal t;
    hardReset(t);
    resize(t, 4, 40);
    t.drawsSizedText = true;
    t.drawsProportionalText = true;
    const std::string sentence = "The quick brown fox jumps over the lazy dog";
    feed(t, "\x1b[H\x1b]66;p=1:c=6;" + sentence + "\x07");

    Selection::Session session;
    Selection::selectAllText(session, t);
    const std::string copied = Selection::selectedText(session, t);
    CHECK_MSG(copied.find(sentence) != std::string::npos, "the whole sentence survives the copy");
    CHECK_MSG(copied.find("The") == copied.rfind("The"), "and it is copied once");

    // The control: a run that fits one chunk copies as before.
    Terminal control;
    hardReset(control);
    resize(control, 4, 40);
    control.drawsSizedText = true;
    control.drawsProportionalText = true;
    feed(control, "\x1b[H\x1b]66;p=1:c=6;Overview\x07");
    Selection::Session controlSession;
    Selection::selectAllText(controlSession, control);
    const std::string controlCopy = Selection::selectedText(controlSession, control);
    CHECK_MSG(controlCopy.find("Overview") != std::string::npos && controlCopy.find("OverviewO") == std::string::npos,
        "a one-chunk run copies once");
}

namespace {

/// A shaper that sets every letter 12 px wide in 8 px cells, so a run's
/// cells hold its text where the ink is and some cells hold none.
struct WideLetterMeasurer final : Ubiquitty::TextMeasureClient {
    int shapeProportionalRun(std::span<const std::uint32_t> codePoints, Ubiquitty::ProportionalRunStyle, std::span<Ubiquitty::ProportionalShapedGlyph> glyphsOut) override
    {
        if (glyphsOut.size() < codePoints.size())
            return 0;
        for (std::size_t i = 0; i < codePoints.size(); ++i) {
            glyphsOut[i] = Ubiquitty::ProportionalShapedGlyph {};
            glyphsOut[i].glyph = static_cast<std::uint32_t>(i + 1);
            glyphsOut[i].advanceInPixels = 12.0f;
            glyphsOut[i].cluster = static_cast<std::uint16_t>(i);
        }
        return static_cast<int>(codePoints.size());
    }
    float proportionalCellAdvanceInPixels() override { return 8.0f; }
};

/// A shaper that sets every letter 4 px wide in 8 px cells, so two letters'
/// ink begins in each cell of a run and a cell is selectable in halves.
struct TwoLettersPerCellMeasurer final : Ubiquitty::TextMeasureClient {
    int shapeProportionalRun(std::span<const std::uint32_t> codePoints, Ubiquitty::ProportionalRunStyle, std::span<Ubiquitty::ProportionalShapedGlyph> glyphsOut) override
    {
        if (glyphsOut.size() < codePoints.size())
            return 0;
        for (std::size_t index = 0; index < codePoints.size(); ++index) {
            glyphsOut[index] = Ubiquitty::ProportionalShapedGlyph {};
            glyphsOut[index].glyph = codePoints[index];
            glyphsOut[index].advanceInPixels = 4.0f;
            glyphsOut[index].cluster = static_cast<std::uint16_t>(index);
        }
        return static_cast<int>(codePoints.size());
    }
    float proportionalCellAdvanceInPixels() override { return 8.0f; }
};

/// Packs sixteen source characters into each grid cell.
struct PackedLetterMeasurer final : Ubiquitty::TextMeasureClient {
    int shapeProportionalRun(std::span<const std::uint32_t> codePoints, Ubiquitty::ProportionalRunStyle, std::span<Ubiquitty::ProportionalShapedGlyph> glyphsOut) override
    {
        if (glyphsOut.size() < codePoints.size()) {
            return 0;
        }
        for (std::size_t index = 0; index < codePoints.size(); ++index) {
            glyphsOut[index] = {};
            glyphsOut[index].glyph = codePoints[index];
            glyphsOut[index].advanceInPixels = 0.5f;
            glyphsOut[index].cluster = static_cast<std::uint16_t>(index);
        }
        return static_cast<int>(codePoints.size());
    }
    float proportionalCellAdvanceInPixels() override { return 8.0f; }
};

} // namespace

TEST(centeredProportionalCopiesKeepNonblankTextInsideSpaceLeadingClusters)
{
    for (const int columns : { 48, 1024 }) {
        for (const std::string& suffix : { std::string(15, ' ') + "W", std::string(16, ' ') }) {
            Terminal t;
            hardReset(t);
            resize(t, 4, columns);
            t.drawsSizedText = true;
            t.drawsProportionalText = true;
            PackedLetterMeasurer measurer;
            t.textMeasureClient = Ubiquitty::CheckedPtr<Ubiquitty::TextMeasureClient>(&measurer);
            const std::string prefix(columns == 48 ? 16 : 1264, 'A');
            const std::string text = prefix + suffix;
            const std::string content = "\x1b]66;p=1:c=" + std::to_string(columns) + ":h=2;" + text + "\x07";
            feed(t, content);
            Selection::Session session = freshSession();
            Selection::selectAllText(session, t);
            const std::string expected = suffix.back() == 'W' ? text : prefix;
            const std::string plain = Selection::selectedText(session, t);
            const CellPalette palette = CellPalette::fromTerminalPalette(t.palette);
            const Selection::StyledFont font { "monospace", 16.0 };
            const auto styled = Selection::styledClipboard(session, t, Selection::StyleColorMode::Full, palette, font);
            CHECK_MSG(plain.starts_with(expected), "the complete nonblank cluster survives trailing-blank trim");
            CHECK_MSG(plain.size() == expected.size() || (plain.size() > expected.size() && plain[expected.size()] == '\n'), "an all-space trailing cluster is trimmed");
            CHECK(styled.plain == plain);
            if (suffix.back() == 'W') {
                CHECK(styled.html.find(suffix) != std::string::npos);
                CHECK(styled.rtf.find(suffix) != std::string::npos);
            }
            t.textMeasureClient = Ubiquitty::CheckedPtr<Ubiquitty::TextMeasureClient>(nullptr);
        }
    }
}

TEST(proportionalCopyEndpointsPreserveOnlyTheSelectedSpaceLeadingCluster)
{
    Terminal t;
    hardReset(t);
    resize(t, 3, 8);
    t.drawsSizedText = true;
    t.drawsProportionalText = true;
    // Sixteen letters a cell, and from the layout rather than from storage: the
    // run's text rides its anchor, so which cell a letter is copied from is the
    // measurer's answer.
    PackedLetterMeasurer measurer;
    t.textMeasureClient = Ubiquitty::CheckedPtr<Ubiquitty::TextMeasureClient>(&measurer);
    feed(t, "\x1b]66;p=1:c=2;AAAAAAAAAAAAAAAA j W\x07");
    CHECK(dragCopy(t, 0, 0, 0, 0) == std::string(16, 'A'));
    CHECK(dragCopy(t, 0, 1, 0, 1) == " j W");
    Selection::Session session = freshSession();
    Selection::beginGesture(session, t, 0, 1, 1, 0, {});
    Selection::dragGesture(session, t, 0, 1, 0);
    const CellPalette palette = CellPalette::fromTerminalPalette(t.palette);
    const Selection::StyledFont font { "monospace", 16.0 };
    const auto styled = Selection::styledClipboard(session, t, Selection::StyleColorMode::Full, palette, font);
    CHECK(styled.plain == " j W");
    CHECK(styled.html.find(" j W") != std::string::npos);
    t.textMeasureClient = Ubiquitty::CheckedPtr<Ubiquitty::TextMeasureClient>(nullptr);
}

// Letters narrower than a cell: "Hello from Ubiquitty" in a run twenty cells
// wide, at half a pixel a letter, puts the whole text in the first cell — the
// space before "from" and its f begin in one cell, as they do at any face's
// width. A word press on the f takes "from", the word the letter under the
// pointer is in, and the span it leaves is one the copy and the fill cut at
// the same letters.
TEST(aWordPressInsideARunTakesTheWordUnderThePointer)
{
    Terminal t;
    hardReset(t);
    resize(t, 3, 40);
    t.drawsSizedText = true;
    t.drawsProportionalText = true;
    PackedLetterMeasurer measurer;
    t.textMeasureClient = Ubiquitty::CheckedPtr<Ubiquitty::TextMeasureClient>(&measurer);
    feed(t, "\x1b]66;p=1:c=20;Hello from Ubiquitty\x07");

    // Half a pixel a letter: the f is the seventh letter, 3.0..3.5 pixels into
    // an eight pixel cell; the U the twelfth, 5.5..6.0.
    Selection::Session session = freshSession();
    Selection::GestureStyle style;
    CHECK(Selection::beginGesture(session, t, 0, 0, /*clickCount*/ 2, 0, style, /*xInCells*/ 3.2 / 8.0));
    std::string plain = Selection::selectedText(session, t);
    CHECK_MSG(plain == "from", ("the word under the f, whole: '" + plain + "'").c_str());
    Rendering::SelectionRowSpan spans[3] {};
    CHECK_EQ(Selection::viewportSpans(session, t, 0, spans), 1);
    CHECK_MSG(spans[0].column0 == 0 && spans[0].column1 == 0, "one cell holds every letter");
    CHECK_MSG(spans[0].subColumn0 == 6 && spans[0].subColumn1 == 10, "the fill's cut names the f and the m, not the space before");

    CHECK(Selection::beginGesture(session, t, 0, 0, 2, 0, style, 5.7 / 8.0));
    plain = Selection::selectedText(session, t);
    CHECK_MSG(plain == "Ubiquitty", ("the last word, to the text's end and no further: '" + plain + "'").c_str());

    CHECK(Selection::beginGesture(session, t, 0, 0, 2, 0, style, 0.1 / 8.0));
    plain = Selection::selectedText(session, t);
    CHECK_MSG(plain == "Hello", ("the first word, from the text's start: '" + plain + "'").c_str());

    // The space between two words is its own selection, as in plain cells.
    CHECK(Selection::beginGesture(session, t, 0, 0, 2, 0, style, 2.6 / 8.0));
    plain = Selection::selectedText(session, t);
    CHECK_MSG(plain == " ", ("the space alone: '" + plain + "'").c_str());

    // Without the pointer's place along the row the press is on its cell.
    CHECK(Selection::beginGesture(session, t, 0, 0, 2, 0, style));
    plain = Selection::selectedText(session, t);
    CHECK_MSG(plain == "Hello from Ubiquitty", ("the cell's letters, whole: '" + plain + "'").c_str());
    t.textMeasureClient = Ubiquitty::CheckedPtr<Ubiquitty::TextMeasureClient>(nullptr);
}

// A run's claim runs past its text, and the cells after the last letter are
// the row's void: a drag through them stops at the last letter, and takes the
// rest of the line only once it has gone the void's distance past the text —
// the rule a drag past a plain row's last glyph follows.
TEST(aDragPastARunsLastLetterStopsAtItUntilTheVoidSnaps)
{
    Terminal t;
    hardReset(t);
    resize(t, 3, 40);
    t.drawsSizedText = true;
    t.drawsProportionalText = true;
    PackedLetterMeasurer measurer;
    t.textMeasureClient = Ubiquitty::CheckedPtr<Ubiquitty::TextMeasureClient>(&measurer);
    // Half a pixel a letter: the text fills the first cell's first pixels
    // and the claim's other nineteen cells are its unused tail.
    feed(t, "\x1b]66;p=1:c=20;Hello\x07");
    const auto resolve = [&t](double xInCells) {
        return Selection::resolveSelectionCaret(t, 0, 0, static_cast<int>(xInCells + 0.5), xInCells);
    };
    Selection::Session session = freshSession();
    Selection::beginGesture(session, t, 0, 0, /*clickCount*/ 1, 0, {});
    Selection::configureCharacterCaretGesture(session, t, resolve(0.1 / 8.0));
    Rendering::SelectionRowSpan spans[3] {};

    // Three cells into the tail: the text, and nothing of the tail.
    CHECK(Selection::dragCharacterCaretGesture(session, t, resolve(3.0), 0));
    CHECK_STR_EQ(Selection::selectedText(session, t), "Hello");
    CHECK_EQ(Selection::viewportSpans(session, t, 0, spans), 1);
    CHECK_MSG(spans[0].column0 == 0 && spans[0].column1 == 0,
        ("the highlight stops at the last letter's cell: " + std::to_string(spans[0].column0) + ".." + std::to_string(spans[0].column1)).c_str());

    // The void's distance past the text: the rest of the line.
    CHECK(Selection::dragCharacterCaretGesture(session, t, resolve(8.0), 0));
    CHECK_EQ(Selection::viewportSpans(session, t, 0, spans), 1);
    CHECK_MSG(spans[0].column0 == 0 && spans[0].column1 == t.columns - 1,
        ("far enough out, the line is taken whole: " + std::to_string(spans[0].column0) + ".." + std::to_string(spans[0].column1)).c_str());
    CHECK_STR_EQ(Selection::selectedText(session, t), "Hello");
    t.textMeasureClient = Ubiquitty::CheckedPtr<Ubiquitty::TextMeasureClient>(nullptr);
}

// A centred run's first letter sits cells away from the anchor, and a press
// on it is a press on that cell: the selection starts there, not at the
// anchor with the slack between lit as if selected.
TEST(aPressOnACentredRunsFirstLetterStartsTheSelectionAtItsCell)
{
    Terminal t;
    hardReset(t);
    resize(t, 3, 40);
    t.drawsSizedText = true;
    t.drawsProportionalText = true;
    PackedLetterMeasurer measurer;
    t.textMeasureClient = Ubiquitty::CheckedPtr<Ubiquitty::TextMeasureClient>(&measurer);
    // Half a pixel a letter: "Hello" is 2.5 px, centred in twenty 8 px cells
    // it begins 78.75 px in, cell 9.
    feed(t, "\x1b]66;p=1:c=20:h=2;Hello\x07");
    const auto resolve = [&t](double xInCells) {
        return Selection::resolveSelectionCaret(t, 0, 0, static_cast<int>(xInCells + 0.5), xInCells);
    };
    const Selection::CaretResolution onTheLetter = resolve(78.9 / 8.0);
    CHECK_EQ(onTheLetter.caret, 9);
    CHECK_EQ(onTheLetter.subCaret, 0);
    Selection::Session session = freshSession();
    Selection::beginGesture(session, t, 0, 9, /*clickCount*/ 1, 0, {});
    Selection::configureCharacterCaretGesture(session, t, onTheLetter);
    CHECK(Selection::dragCharacterCaretGesture(session, t, resolve(79.6 / 8.0), 0));
    CHECK_STR_EQ(Selection::selectedText(session, t), "He");
    Rendering::SelectionRowSpan spans[3] {};
    CHECK_EQ(Selection::viewportSpans(session, t, 0, spans), 1);
    CHECK_MSG(spans[0].column0 == 9 && spans[0].column1 == 9, "the highlight is on the letters' cell alone");
    t.textMeasureClient = Ubiquitty::CheckedPtr<Ubiquitty::TextMeasureClient>(nullptr);
}

// Sixteen letters' ink in one cell: a caret between two of them is the only
// thing that can name a single letter there, and every flavour of the copy has
// to cut at the same place.
TEST(aCaretInsideAPackedCellCopiesOneLetterInEveryFlavour)
{
    Terminal t;
    hardReset(t);
    resize(t, 3, 8);
    t.drawsSizedText = true;
    t.drawsProportionalText = true;
    PackedLetterMeasurer measurer;
    t.textMeasureClient = Ubiquitty::CheckedPtr<Ubiquitty::TextMeasureClient>(&measurer);
    feed(t, "\x1b]66;p=1:c=1;ABCDEFGHIJKLMNOP\x07");

    // Half a pixel a letter in an eight pixel cell: a point on `B` is between
    // the boundaries half a pixel and a pixel in, which no cell boundary can
    // name; a press on it and a drag that stays on it select `B` alone.
    const Selection::CaretResolution start = Selection::resolveSelectionCaret(t, 0, 0, /*caret*/ 0, 0.6 / 8.0);
    CHECK_EQ(start.caret, 0);
    CHECK_MSG(start.subCaret == 1 && start.subCaretAfter == 2, "the letter under the point is the second of the cell");
    const Selection::CaretResolution end = Selection::resolveSelectionCaret(t, 0, 0, /*caret*/ 0, 0.9 / 8.0);
    CHECK_EQ(end.caret, 0);
    CHECK_EQ(end.subCaret, 1);
    CHECK_EQ(end.subCaretAfter, 2);

    Selection::Session session = freshSession();
    Selection::beginGesture(session, t, 0, 0, /*clickCount*/ 1, 0, {});
    Selection::configureCharacterCaretGesture(session, t, start);
    CHECK(Selection::dragCharacterCaretGesture(session, t, end, 0));
    const std::string plain = Selection::selectedText(session, t);
    CHECK_MSG(plain == "B", ("one letter of sixteen in a cell: " + plain).c_str());

    const CellPalette palette = CellPalette::fromTerminalPalette(t.palette);
    const Selection::StyledFont font { "monospace", 16.0 };
    const auto styled = Selection::styledClipboard(session, t, Selection::StyleColorMode::Full, palette, font);
    CHECK_MSG(styled.plain == "B", ("the styled walk cuts where the plain one does: " + styled.plain).c_str());
    CHECK(styled.html.find(">B<") != std::string::npos);

    // The grid is still addressed in cells: the highlight names the one cell
    // the letters sit in, and the cut lives on the span beside it.
    Rendering::SelectionRowSpan spans[8] {};
    CHECK_EQ(Selection::viewportSpans(session, t, 0, spans), 1);
    CHECK_EQ(spans[0].column0, 0);
    CHECK_EQ(spans[0].column1, 0);
    t.textMeasureClient = Ubiquitty::CheckedPtr<Ubiquitty::TextMeasureClient>(nullptr);
}

namespace {

// One caret gesture between two points along row 0, in cells from the grid's
// left edge, as `routePointer` drives it.
Selection::Session caretDrag(Terminal& t, double fromXInCells, double toXInCells)
{
    Selection::Session session = freshSession();
    const auto resolve = [&t](double xInCells) {
        return Selection::resolveSelectionCaret(t, 0, 0, static_cast<int>(xInCells + 0.5), xInCells);
    };
    Selection::beginGesture(session, t, 0, static_cast<int>(fromXInCells), /*clickCount*/ 1, 0, {});
    Selection::configureCharacterCaretGesture(session, t, resolve(fromXInCells));
    Selection::dragCharacterCaretGesture(session, t, resolve(toXInCells), 0);
    return session;
}

} // namespace

// The offsets survive only as long as the caret drag that wrote them: every
// other way of placing an endpoint puts the selection back on whole cells,
// which is what keeps the addition invisible to every gesture but this one.
TEST(aCellGestureAfterASubCellDragGoesBackToWholeCells)
{
    Terminal t;
    hardReset(t);
    resize(t, 4, 20);
    t.drawsSizedText = true;
    t.drawsProportionalText = true;
    TwoLettersPerCellMeasurer measurer;
    t.textMeasureClient = Ubiquitty::CheckedPtr<Ubiquitty::TextMeasureClient>(&measurer);
    // Four 4 px letters in two 8 px cells: `ab` in cell 0, `cd` in cell 1.
    feed(t, "\x1b[H\x1b]66;p=1:c=2;abcd\x07");

    Selection::Session session = caretDrag(t, 0.5, 1.0);
    CHECK_MSG(Selection::selectedText(session, t) == "bc", "the drag takes the letters it was pressed on and reached");
    // A cell drag moves the head without a caret, and the cut goes with it:
    // the cells the new head names are the whole of what copies.
    Selection::dragGesture(session, t, 0, 1, 0);
    CHECK_MSG(Selection::selectedText(session, t) == "abcd", "a head placed by cells takes its cells whole");

    // So does a word expansion: a double click after a sub-cell drag places
    // both endpoints afresh, and the cut goes with them.
    Selection::Session word = caretDrag(t, 0.5, 1.0);
    CHECK(Selection::selectedText(word, t) == "bc");
    Selection::beginGesture(word, t, 0, 1, /*clickCount*/ 2, 0, {});
    CHECK_MSG(Selection::selectedText(word, t) == "abcd", "a double click after a sub-cell drag takes the word whole");

    // And Select All, which places both endpoints over the whole buffer.
    Selection::Session all = caretDrag(t, 0.5, 1.0);
    CHECK(Selection::selectedText(all, t) == "bc");
    Selection::selectAllText(all, t);
    // The whole buffer, so the blank rows below the run join on as newlines.
    CHECK_MSG(Selection::selectedText(all, t).starts_with("abcd"), "Select All after a sub-cell drag takes the run whole");
    t.textMeasureClient = Ubiquitty::CheckedPtr<Ubiquitty::TextMeasureClient>(nullptr);
}

TEST(aLaidOutRunCopiesWholeAndDragsByTheCellsItsInkIsIn)
{
    Terminal t;
    hardReset(t);
    resize(t, 4, 40);
    t.drawsSizedText = true;
    t.drawsProportionalText = true;
    WideLetterMeasurer measurer;
    t.textMeasureClient = Ubiquitty::CheckedPtr<Ubiquitty::TextMeasureClient>(&measurer);
    // "abcdef" at 12 px lands in cells 0,1,3,4,6,7 of nine; cells 2, 5 and
    // 8 carry no text. The whole run copies whole, once, with no blanks
    // from the empty cells inside it.
    feed(t, "\x1b[H\x1b]66;p=1:c=9;abcdef\x07");
    Selection::Session session;
    Selection::selectAllText(session, t);
    const std::string copied = Selection::selectedText(session, t);
    CHECK_MSG(copied.find("abcdef") != std::string::npos, "the run copies whole");
    CHECK_MSG(copied.find("ab c") == std::string::npos, "and an empty cell inside it adds no space");
    // A drag over the whole run yields it whole.
    CHECK(dragCopy(t, 0, 0, 0, 8).find("abcdef") != std::string::npos);
    // A drag inside the run takes the letters whose ink begins in the cells
    // under it: cells 3 and 4 hold c and d. The empty cell 2 between b and c
    // belongs to b's run of cells and adds nothing.
    const std::string inside = dragCopy(t, 0, 3, 0, 4);
    CHECK_MSG(inside.find("cd") != std::string::npos && inside.find("b") == std::string::npos && inside.find("e") == std::string::npos,
        "a drag inside a run selects by the cells the letters begin in");
    // An endpoint on an empty cell snaps to the letters before it: cell 2
    // is b's tail, so a drag from 2 to 3 yields b and c.
    const std::string fromEmpty = dragCopy(t, 0, 2, 0, 3);
    CHECK_MSG(fromEmpty.find("bc") != std::string::npos && fromEmpty.find("a") == std::string::npos,
        "an empty cell inside the run belongs to the letter before it");
    // A double-click anywhere in the word takes the word: the empty cells
    // inside the run are not word breaks.
    Selection::Session word = freshSession();
    Selection::beginGesture(word, t, 0, 4, /*clickCount*/ 2, 0, {});
    CHECK_MSG(Selection::selectedText(word, t) == "abcdef", "a double-click inside a laid-out run takes the whole word");
    t.textMeasureClient = Ubiquitty::CheckedPtr<Ubiquitty::TextMeasureClient>(nullptr);
}

namespace {

/// A shaper that sets every letter one 8 px cell wide, so each cell of a run
/// holds one letter.
struct LetterPerCellMeasurer final : Ubiquitty::TextMeasureClient {
    int shapeProportionalRun(std::span<const std::uint32_t> codePoints, Ubiquitty::ProportionalRunStyle, std::span<Ubiquitty::ProportionalShapedGlyph> glyphsOut) override
    {
        if (glyphsOut.size() < codePoints.size())
            return 0;
        for (std::size_t index = 0; index < codePoints.size(); ++index) {
            glyphsOut[index] = Ubiquitty::ProportionalShapedGlyph {};
            glyphsOut[index].glyph = codePoints[index];
            glyphsOut[index].advanceInPixels = 8.0f;
            glyphsOut[index].cluster = static_cast<std::uint16_t>(index);
        }
        return static_cast<int>(codePoints.size());
    }
    float proportionalCellAdvanceInPixels() override { return 8.0f; }
};

// A sized-text terminal measuring with `measurer`, holding `content` from the
// home cell.
struct MeasuredScreen {
    Terminal terminal;

    MeasuredScreen(Ubiquitty::TextMeasureClient& measurer, const std::string& content, int rows = 6, int columns = 24)
    {
        hardReset(terminal);
        resize(terminal, rows, columns);
        terminal.drawsSizedText = true;
        terminal.drawsProportionalText = true;
        terminal.textMeasureClient = Ubiquitty::CheckedPtr<Ubiquitty::TextMeasureClient>(&measurer);
        feed(terminal, "\x1b[H" + content);
    }
    ~MeasuredScreen() { terminal.textMeasureClient = Ubiquitty::CheckedPtr<Ubiquitty::TextMeasureClient>(nullptr); }
};

} // namespace

// Hello at c=5:s=2 is five letters, each a glyph two rows tall over its cell's
// two columns: H at 0-1, e at 2-3, l at 4-5 and 6-7, o at 8-9. A selection
// holds a letter wherever it holds one of the letter's columns, on either row
// of the run, as kitty selects a scaled character; the letters it holds are
// lit on both rows and copied once.
TEST(aScaledRunSelectsTheLettersADragOnItsTopBandHolds)
{
    LetterPerCellMeasurer measurer;
    MeasuredScreen screen(measurer, "\x1b]66;p=1:c=5:s=2;Hello\x07yz");
    Selection::Session session = draggedSelection(screen.terminal, 0, 2, 0, 5);
    CHECK(highlightedPiecesOnRow(session, screen.terminal, 0) == Pieces({ { 2, 5 } }));
    CHECK(highlightedPiecesOnRow(session, screen.terminal, 1) == Pieces({ { 2, 5 } }));
    CHECK_STR_EQ(Selection::selectedText(session, screen.terminal), "el");
}

TEST(aScaledRunSelectsTheLettersADragOnItsBottomBandHolds)
{
    LetterPerCellMeasurer measurer;
    MeasuredScreen screen(measurer, "\x1b]66;p=1:c=5:s=2;Hello\x07yz");
    Selection::Session session = draggedSelection(screen.terminal, 1, 2, 1, 5);
    CHECK(highlightedPiecesOnRow(session, screen.terminal, 0) == Pieces({ { 2, 5 } }));
    CHECK(highlightedPiecesOnRow(session, screen.terminal, 1) == Pieces({ { 2, 5 } }));
    CHECK_STR_EQ(Selection::selectedText(session, screen.terminal), "el");
}

TEST(aScaledRunSelectsTheLettersADragOnAMiddleBandHolds)
{
    // At s=3 each letter is three columns: e at 3-5, l at 6-8. A drag on the
    // middle row from column 4 to 7 holds both, and all three rows light them.
    LetterPerCellMeasurer measurer;
    MeasuredScreen screen(measurer, "\x1b]66;p=1:c=5:s=3;Hello\x07");
    Selection::Session session = draggedSelection(screen.terminal, 1, 4, 1, 7);
    for (int row = 0; row < 3; ++row)
        CHECK(highlightedPiecesOnRow(session, screen.terminal, row) == Pieces({ { 3, 8 } }));
    CHECK_STR_EQ(Selection::selectedText(session, screen.terminal), "el");
}

TEST(aDragAcrossAScaledRunsEdgeHoldsTheLettersItReaches)
{
    // From past the run on its bottom row back to the second column of the
    // second l's cell: that l and the o, lit on both rows, and the bottom
    // row's void after the run with them, as a drag from a void takes it.
    LetterPerCellMeasurer measurer;
    MeasuredScreen screen(measurer, "\x1b]66;p=1:c=5:s=2;Hello\x07yz");
    Selection::Session session = draggedSelection(screen.terminal, 1, 15, 1, 7);
    CHECK(highlightedPiecesOnRow(session, screen.terminal, 0) == Pieces({ { 6, 9 } }));
    CHECK(highlightedPiecesOnRow(session, screen.terminal, 1) == Pieces({ { 6, 9 }, { 10, 23 } }));
    CHECK_STR_EQ(Selection::selectedText(session, screen.terminal), "lo");
}

TEST(aSelectionThroughBothBandsCopiesEachLetterOnceInTheTextsOrder)
{
    // From the second l on the top row to the e on the bottom row: the top row
    // holds the l, the o and yz after the run, the bottom row the H and the e.
    // The copy carries the held letters once, in order, where the selection
    // first reaches the run, and the bottom row adds no line of its own.
    LetterPerCellMeasurer measurer;
    MeasuredScreen screen(measurer, "\x1b]66;p=1:c=5:s=2;Hello\x07yz");
    Selection::Session session = draggedSelection(screen.terminal, 0, 6, 1, 3);
    CHECK(highlightedPiecesOnRow(session, screen.terminal, 0) == Pieces({ { 0, 3 }, { 6, 9 }, { 10, 23 } }));
    CHECK(highlightedPiecesOnRow(session, screen.terminal, 1) == Pieces({ { 0, 3 }, { 6, 9 } }));
    CHECK_STR_EQ(Selection::selectedText(session, screen.terminal), "Heloyz");
}

TEST(aWordPressOnAScaledRunsLowerBandTakesTheWordUnderThePointer)
{
    // Two letters a cell at c=3:s=2: "Hi" at 0-1, " y" at 2-3, "ou" at 4-5. A
    // double press on the bottom row over the o takes "you".
    TwoLettersPerCellMeasurer measurer;
    MeasuredScreen screen(measurer, "\x1b]66;p=1:c=3:s=2;Hi you\x07");
    Selection::Session session = freshSession();
    Selection::GestureStyle style;
    CHECK(Selection::beginGesture(session, screen.terminal, 1, 4, /*clickCount*/ 2, 0, style, /*xInCells*/ 4.5));
    CHECK_STR_EQ(Selection::selectedText(session, screen.terminal), "you");
    CHECK(highlightedPiecesOnRow(session, screen.terminal, 0) == Pieces({ { 2, 5 } }));
    CHECK(highlightedPiecesOnRow(session, screen.terminal, 1) == Pieces({ { 2, 5 } }));
    // A triple press takes the line, the run's text whole.
    CHECK(Selection::beginGesture(session, screen.terminal, 1, 4, 3, 0, style, 4.5));
    CHECK_STR_EQ(Selection::selectedText(session, screen.terminal), "Hi you");
}

TEST(aCaretOnAScaledRunsLowerBandStandsAtALetterBoundaryOnThatRow)
{
    // Two letters a cell, each cell two columns: 3.25 columns along is 13 px
    // into the run's own 8 px cells, on the y, so the caret before it is the
    // second cell's one letter in, the one after it the third cell's, both on
    // the row the pointer is on.
    TwoLettersPerCellMeasurer measurer;
    MeasuredScreen screen(measurer, "\x1b]66;p=1:c=3:s=2;Hi you\x07");
    const Selection::CaretResolution caret = Selection::resolveSelectionCaret(screen.terminal, 0, 1, 3, 3.25);
    CHECK_EQ(caret.absoluteRow, 1);
    CHECK_EQ(caret.caret, 2);
    CHECK_EQ(caret.subCaret, 1);
    CHECK_EQ(caret.caretAfter, 4);
    CHECK_EQ(caret.subCaretAfter, 0);
}

// A selection running through the run's top band from the row above and
// ending inside its bottom band, between two letters of a packed cell, holds
// every letter: the top band's span reaches them all, so each is lit on both
// band rows and copied once, wherever the drag began. The caret's cut on the
// bottom band names only the letters that band's own span takes.
TEST(aSelectionThroughAScaledRunsTopBandLightsEveryLetterOnItsBottomBand)
{
    // Two letters a cell at c=3:s=2: "He" at 0-1, "ll" at 2-3, "o" at 4-5.
    TwoLettersPerCellMeasurer measurer;
    MeasuredScreen screen(measurer, "abcdefghijk\r\n\x1b]66;p=1:c=3:s=2;Hello\x07|");
    const auto caretAt = [&](int visibleRow, double xInCells) {
        return Selection::resolveSelectionCaret(screen.terminal, 0, visibleRow, static_cast<int>(xInCells + 0.5), xInCells);
    };
    for (const bool isFromAbove : { true, false }) {
        // On the bottom band, 2.25 columns along is the first l's ink.
        const Selection::CaretResolution above = caretAt(0, 5.0);
        const Selection::CaretResolution below = caretAt(2, 2.25);
        const Selection::CaretResolution& start = isFromAbove ? above : below;
        const Selection::CaretResolution& end = isFromAbove ? below : above;
        Selection::Session session = freshSession();
        Selection::beginGesture(session, screen.terminal, isFromAbove ? 0 : 2, isFromAbove ? 5 : 2, /*clickCount*/ 1, 0, {});
        Selection::configureCharacterCaretGesture(session, screen.terminal, start);
        CHECK(Selection::dragCharacterCaretGesture(session, screen.terminal, end, 0));
        const Pieces lower = highlightedPiecesOnRow(session, screen.terminal, 2);
        std::string described;
        for (const auto& [first, last] : lower)
            described += std::to_string(first) + "-" + std::to_string(last) + " ";
        CHECK_MSG(lower == Pieces({ { 0, 5 } }), ("every letter on the bottom band: " + described).c_str());
        CHECK(highlightedPiecesOnRow(session, screen.terminal, 1).front() == std::pair(0, 5));
        const std::string copied = Selection::selectedText(session, screen.terminal);
        CHECK_MSG(copied.find("Hello") != std::string::npos, copied.c_str());
    }
}

namespace {

/// A shaper with a text face's spread of advances in 8 px cells: H 6.5, e
/// 5.5, l 2.2 and o 5.6, so "Hello" packs into three cells and a run of six
/// leaves three cells of tail after its last letter.
struct TextFaceMeasurer final : Ubiquitty::TextMeasureClient {
    /// The l's advance; at 1 px the two l and the o begin in one cell.
    float lAdvance = 2.2f;
    int shapeProportionalRun(std::span<const std::uint32_t> codePoints, Ubiquitty::ProportionalRunStyle, std::span<Ubiquitty::ProportionalShapedGlyph> glyphsOut) override
    {
        if (glyphsOut.size() < codePoints.size())
            return 0;
        for (std::size_t index = 0; index < codePoints.size(); ++index) {
            const std::uint32_t codePoint = codePoints[index];
            glyphsOut[index] = Ubiquitty::ProportionalShapedGlyph {};
            glyphsOut[index].glyph = codePoint;
            glyphsOut[index].advanceInPixels = codePoint == 'H' ? 6.5f : codePoint == 'e' ? 5.5f
                : codePoint == 'l'                                                        ? lAdvance
                                                                                          : 5.6f;
            glyphsOut[index].cluster = static_cast<std::uint16_t>(index);
        }
        return static_cast<int>(codePoints.size());
    }
    float proportionalCellAdvanceInPixels() override { return 8.0f; }
};

} // namespace

// The verifier's screen: a scale-2 run after a sized X, and a scale-3 run on
// the rows below, both claiming six cells for text that fills three. A drag
// from the scale-2 run's bottom band, on its l, down into the scale-3 run's
// middle band holds the first run's l, l and o from its bottom band, and
// every letter of the second from its top band; each held letter is lit on
// every band of its run, the last letter with the claim's tail after it, so
// a band row whose own span stopped short of the tail lights what the others
// do, and a renderer's layout putting the o's ink a cell from the measurer's
// still lights it.
TEST(aDragAcrossTwoScaledRunsLightsEveryHeldLetterOnEveryBand)
{
    TextFaceMeasurer measurer;
    MeasuredScreen screen(measurer, "Ax\x1b]66;s=2;X\x1b\\yz \x1b]66;p=1:c=6:s=2;Hello\x1b\\|\r\n\r\n\x1b]66;p=1:c=6:s=3;Hello\x1b\\|", 8, 24);
    Selection::Session session = draggedSelection(screen.terminal, 1, 9, 3, 5);
    // The first run's cells are two columns from column 7: l and l begin in
    // its second cell, 9-10, and o in its third, 11-12, with the tail to 18.
    CHECK(highlightedPiecesOnRow(session, screen.terminal, 0) == Pieces({ { 9, 18 } }));
    CHECK(highlightedPiecesOnRow(session, screen.terminal, 1) == Pieces({ { 9, 18 }, { 19, 23 } }));
    // The second run is lit whole on every band.
    for (int row = 2; row < 5; ++row)
        CHECK(highlightedPiecesOnRow(session, screen.terminal, row).front() == std::pair(0, 17));
    const std::string copied = Selection::selectedText(session, screen.terminal);
    CHECK_MSG(copied.find("llo") != std::string::npos && copied.find("Hello") != std::string::npos, copied.c_str());
}

namespace {

// One drag of the matrix below: where it starts and ends, on the run's band
// rows, and what went wrong with it.
struct MatrixDrag {
    int fromRow;
    int fromColumn;
    int toRow;
    int toColumn;
};

std::string describeDrag(int scale, const MatrixDrag& drag)
{
    return "s=" + std::to_string(scale) + " (" + std::to_string(drag.fromRow) + "," + std::to_string(drag.fromColumn) + ")->(" + std::to_string(drag.toRow) + ","
        + std::to_string(drag.toColumn) + ")";
}

} // namespace

// Kitty's contract for scaled text, over every drag a pointer can make on a
// scaled proportional run: from each band row to each band row, forward and
// backward, from every column of the claim and from the plain cells either
// side of it. "Hello" at c=6 in a text face's spread of advances fills three
// of its six cells, H and e in the first, the two l in the second, o in the
// third, and the claim's tail after it is no letter's; with a narrow l the o
// begins in the second cell and its ink runs into the third, which is the
// o's as far as the text reaches. Every drag holds exactly the letters whose
// columns its span reaches on some band row, and never lights the tail where
// its span does not reach it; each held letter is lit by the same pieces,
// cut the same way, on every band row of the run, so no letter lights on
// only some of its bands and every band's fill has the same left and right;
// and the copy carries each held letter once.
TEST(theSelectionMatrixOverAScaledRunHoldsTheSameLettersOnEveryBand)
{
    TextFaceMeasurer measurer;
    int failedChecks = 0;
    std::string firstFailure;
    for (const float lAdvance : { 2.2f, 1.0f }) {
        measurer.lAdvance = lAdvance;
        for (const int scale : { 2, 3 }) {
            MeasuredScreen screen(measurer, "ab\x1b]66;p=1:c=6:s=" + std::to_string(scale) + ";Hello\x1b\\|cd", 6, 28);
            Terminal& terminal = screen.terminal;
            const int anchor = 2;
            const int claim = 6 * scale;
            const int lastClaimed = anchor + claim - 1;
            const Cell* anchorCell = UbiquittyTest::cell(terminal, 0, anchor);
            REQUIRE(anchorCell && isProportionalRunAnchor(&terminal, *anchorCell));
            const std::span<const std::uint32_t> text = runTextCodePoints(&terminal, *anchorCell);
            std::vector<std::uint16_t> owners(text.size());
            REQUIRE(OSC::TextSizing::proportionalRunCellOwners(terminal, text, 6, 0, OSC::TextSizing::runStyleOf(cellPen(*anchorCell)), owners));
            // The cells the text reaches, by the face's own advances: the last
            // letter's ink ends in the last of them.
            std::vector<Ubiquitty::ProportionalShapedGlyph> shaped(text.size());
            REQUIRE(measurer.shapeProportionalRun(text, {}, shaped) == static_cast<int>(text.size()));
            float textWidth = 0.0f;
            for (const Ubiquitty::ProportionalShapedGlyph& glyph : shaped)
                textWidth += glyph.advanceInPixels;
            const int textCells = static_cast<int>(std::ceil(textWidth / measurer.proportionalCellAdvanceInPixels() - 1e-4f));
            REQUIRE(textCells > owners.back());
            // Each letter's columns: its cell to the column before the next cell
            // a letter begins in, and the last letters' through the last cell
            // the text reaches; the claim's tail after it is no letter's.
            std::vector<std::pair<int, int>> letterColumns;
            for (std::size_t letter = 0; letter < owners.size(); ++letter) {
                const auto next = std::ranges::upper_bound(owners, owners[letter]);
                const int nextCell = next == owners.end() ? textCells : static_cast<int>(*next);
                letterColumns.emplace_back(anchor + owners[letter] * scale, anchor + nextCell * scale - 1);
            }
            const int tailFirst = anchor + textCells * scale;
            const auto record = [&](bool isFailure, const std::string& what) {
                if (!isFailure)
                    return;
                if (failedChecks++ < 6)
                    firstFailure += " | " + what;
            };
            for (int fromRow = 0; fromRow < scale; ++fromRow) {
                for (int toRow = 0; toRow < scale; ++toRow) {
                    for (int fromColumn = 0; fromColumn <= lastClaimed + 2; ++fromColumn) {
                        for (int toColumn = 0; toColumn <= lastClaimed + 2; ++toColumn) {
                            const MatrixDrag drag { fromRow, fromColumn, toRow, toColumn };
                            // A press that does not move is a click, and selects nothing.
                            if (fromRow == toRow && fromColumn == toColumn)
                                continue;
                            Selection::Session session = draggedSelection(terminal, fromRow, fromColumn, toRow, toColumn);
                            // The span the drag gives each band row, in reading order.
                            const bool isForward = std::pair(fromRow, fromColumn) <= std::pair(toRow, toColumn);
                            const auto [startRow, startColumn] = isForward ? std::pair(fromRow, fromColumn) : std::pair(toRow, toColumn);
                            const auto [endRow, endColumn] = isForward ? std::pair(toRow, toColumn) : std::pair(fromRow, fromColumn);
                            std::vector<bool> isHeld(text.size(), false);
                            bool isTailHeld = false;
                            for (int row = startRow; row <= endRow; ++row) {
                                const int first = row == startRow ? startColumn : 0;
                                const int last = row == endRow ? endColumn : terminal.columns - 1;
                                for (std::size_t letter = 0; letter < text.size(); ++letter)
                                    isHeld[letter] = isHeld[letter] || (letterColumns[letter].second >= first && letterColumns[letter].first <= last);
                                isTailHeld = isTailHeld || (last >= tailFirst && first <= lastClaimed);
                            }
                            // The run's columns each band row lights, piece by piece.
                            std::vector<Rendering::SelectionRowSpan> spans(static_cast<std::size_t>(4 * terminal.rows));
                            const int count = Selection::viewportSpans(session, terminal, 0, spans);
                            std::vector<std::vector<std::tuple<int, int, int, int, int, int>>> claimPieces(static_cast<std::size_t>(scale));
                            for (int index = 0; index < count; ++index) {
                                const Rendering::SelectionRowSpan& span = spans[static_cast<std::size_t>(index)];
                                if (span.visibleRow < 0 || span.visibleRow >= scale || span.column1 < anchor || span.column0 > lastClaimed)
                                    continue;
                                claimPieces[static_cast<std::size_t>(span.visibleRow)].emplace_back(
                                    span.column0, span.column1, span.subColumn0, span.subColumn1, span.firstLetter, span.endLetter);
                            }
                            for (int band = 1; band < scale; ++band)
                                record(claimPieces[static_cast<std::size_t>(band)] != claimPieces[0], describeDrag(scale, drag) + ": band " + std::to_string(band) + " lit unlike the top band");
                            // Which letters the pieces light, by the letters' columns,
                            // taking the cuts into a packed cell as the copy does.
                            std::vector<bool> isLit(text.size(), false);
                            for (const auto& [first, last, startCut, endCut, firstNamed, endNamed] : claimPieces[0]) {
                                for (std::size_t letter = 0; letter < text.size(); ++letter) {
                                    if (letterColumns[letter].second < first || letterColumns[letter].first > last)
                                        continue;
                                    const int cellColumn = letterColumns[letter].first;
                                    const int stretchStart = static_cast<int>(std::ranges::lower_bound(owners, owners[letter]) - owners.begin());
                                    const int intoCell = static_cast<int>(letter) - stretchStart;
                                    if (startCut > 0 && cellColumn == first && intoCell < startCut)
                                        continue;
                                    if (endCut > 0 && cellColumn + scale - 1 == last && intoCell >= endCut)
                                        continue;
                                    isLit[letter] = true;
                                }
                            }
                            record(isLit != isHeld, describeDrag(scale, drag) + ": the lit letters are not the held ones");
                            // The pieces name the letters they light, so a renderer
                            // whose layout puts a letter a cell from the measurer's
                            // lights the held letters all the same: every held
                            // letter named once, and nothing else.
                            std::vector<int> timesNamed(text.size(), 0);
                            bool isEveryPieceNamed = true;
                            for (const auto& [first, last, startCut, endCut, firstNamed, endNamed] : claimPieces[0]) {
                                isEveryPieceNamed = isEveryPieceNamed && firstNamed >= 0;
                                for (int letter = std::max(firstNamed, 0); letter < std::min(endNamed, static_cast<int>(text.size())); ++letter)
                                    ++timesNamed[static_cast<std::size_t>(letter)];
                            }
                            bool areNamedTheHeld = isEveryPieceNamed;
                            for (std::size_t letter = 0; letter < text.size(); ++letter)
                                areNamedTheHeld = areNamedTheHeld && timesNamed[letter] == (isHeld[letter] ? 1 : 0);
                            record(!areNamedTheHeld, describeDrag(scale, drag) + ": the pieces do not name the held letters");
                            // The tail lights only where a span reached it; where the
                            // drag ended in a band row's void the row's own rule
                            // may stop it at the last letter.
                            bool isTailLit = false;
                            for (const auto& [first, last, startCut, endCut, firstNamed, endNamed] : claimPieces[0])
                                isTailLit = isTailLit || last >= tailFirst;
                            record(isTailLit && !isTailHeld, describeDrag(scale, drag) + ": the tail lit where no span reached it");
                            // The copy carries each held letter once.
                            const std::string copied = Selection::selectedText(session, terminal);
                            std::string held;
                            for (std::size_t letter = 0; letter < text.size(); ++letter) {
                                if (isHeld[letter])
                                    held.push_back(static_cast<char>(text[letter]));
                            }
                            std::string runLettersCopied;
                            for (const char letter : copied) {
                                if (letter == 'H' || letter == 'e' || letter == 'l' || letter == 'o')
                                    runLettersCopied.push_back(letter);
                            }
                            record(runLettersCopied != held, describeDrag(scale, drag) + ": copied \"" + copied + "\" for \"" + held + "\"");
                        }
                    }
                }
            }
        }
    }
    const std::string summary = std::to_string(failedChecks) + " failing check(s); the first:" + firstFailure;
    CHECK_MSG(failedChecks == 0, summary.c_str());
}

// Drag F of the verifier's sheet and its mirror: on the middle band of the
// scale-3 "Hello" alone, from the run's third column to a column past its
// letters' cells and back. With the narrow l the o begins in the run's second
// cell at 14 of 8 px per cell and its ink runs to 19.6, into the third; the
// drag holds all five letters, and the o's columns, and so the highlight,
// run through the third cell, where its ink is, and no further. Every band
// row's pieces name the five letters, and the copy is the word.
TEST(dragFOnAScaledRunsMiddleBandLightsTheLastLetterThroughItsInk)
{
    TextFaceMeasurer measurer;
    measurer.lAdvance = 1.0f;
    MeasuredScreen screen(measurer, "ab\x1b]66;p=1:c=6:s=3;Hello\x1b\\|cd", 6, 28);
    const int anchor = 2;
    // The text reaches three cells: the last column with ink is the ninth of
    // the claim.
    const int textLast = anchor + 3 * 3 - 1;
    for (const bool isBackward : { false, true }) {
        Selection::Session session = isBackward ? draggedSelection(screen.terminal, 1, anchor + 10, 1, anchor + 2) : draggedSelection(screen.terminal, 1, anchor + 2, 1, anchor + 10);
        std::vector<Rendering::SelectionRowSpan> spans(static_cast<std::size_t>(4 * screen.terminal.rows));
        const int count = Selection::viewportSpans(session, screen.terminal, 0, spans);
        for (int band = 0; band < 3; ++band) {
            int firstNamed = -1;
            int endNamed = -1;
            int lastLit = -1;
            for (int index = 0; index < count; ++index) {
                const Rendering::SelectionRowSpan& span = spans[static_cast<std::size_t>(index)];
                if (span.visibleRow != band || span.column1 < anchor || span.column0 > anchor + 17)
                    continue;
                lastLit = std::max(lastLit, span.column1);
                if (span.endLetter <= span.firstLetter)
                    continue;
                firstNamed = firstNamed < 0 ? span.firstLetter : std::min(firstNamed, span.firstLetter);
                endNamed = std::max(endNamed, span.endLetter);
            }
            const std::string what = std::string(isBackward ? "backward" : "forward") + ", band " + std::to_string(band) + ": named " + std::to_string(firstNamed)
                + " to " + std::to_string(endNamed) + ", lit through column " + std::to_string(lastLit);
            CHECK_MSG(firstNamed == 0 && endNamed == 5, what.c_str());
            CHECK_MSG(lastLit == textLast, what.c_str());
        }
        const std::string copied = Selection::selectedText(session, screen.terminal);
        CHECK_MSG(copied == "Hello", copied.c_str());
    }
}

UBIQUITTY_TEST_MAIN()

TEST(aHeldViewportStaysOnItsRowsOnceHistoryIsFull)
{
    Terminal t;
    hardReset(t);
    t.scrollbackLimit = 4;
    resize(t, 3, 12);
    Screen::dropScrollback(&t);
    feed(t, "\033[Hzero\r\none\r\ntwo\r\nthree\r\nfour\r\nfive");
    CHECK_EQ(Screen::scrollbackCount(&t), 3);

    // Scrolled back one row: the top visible row is "three", absolute row 3.
    Selection::Session session = freshSession();
    int viewOffset = 1;
    const int viewedAbsoluteRow = Screen::scrollbackCount(&t) - viewOffset;
    CHECK_EQ(viewedAbsoluteRow, 2);
    // The drain that put the view here judged these facts; the next one is
    // measured from them.
    (void)Selection::updateAfterOutput(session, t, viewOffset, Input::OutputSnapPolicy::HoldViewedRows);

    // Two more lines: history reaches its limit of 4 on the first and evicts
    // "zero" on the second. The rows still in history are renumbered, and the
    // held view must keep showing the same content -- the row that was under
    // the eye, not the row that now carries its old number.
    feed(t, "\r\nsix\r\nseven");
    const Selection::OutputUpdate update = Selection::updateAfterOutput(
        session, t, viewOffset, Input::OutputSnapPolicy::HoldViewedRows);
    CHECK(update.viewport.hold == Input::ViewportHold::Hold);
    viewOffset = static_cast<int>(update.viewport.viewOffset);
    // Appended 2, evicted 1: the count grew by 1, the content moved by 2.
    CHECK_EQ(Screen::scrollbackCount(&t), 4);
    CHECK_MSG(viewOffset == 3, "the held offset moves by the rows appended (2), not by the rows retained (1)");
    CHECK_EQ(Screen::scrollbackCount(&t) - viewOffset, viewedAbsoluteRow - 1);
}

TEST(thePressThatActivatesTheWindowOpensNothingAndKeepsTheSelection)
{
    Terminal t;
    hardReset(t);
    resize(t, 4, 12);
    feed(t, "anchor\r\nmiddle\r\npointer");

    Selection::Session session = freshSession();
    Selection::beginGesture(session, t, 0, 0, 1, 0, {});
    Selection::dragGesture(session, t, 2, 6, 0);
    CHECK(Selection::endGesture(session));
    const std::string standing = Selection::selectedText(session, t);
    REQUIRE(!standing.empty());

    // The click that brought the window forward: Core declines the gesture,
    // the selection stands, and a drag that follows it moves nothing.
    Selection::GestureStyle activating;
    activating.isWindowActivating = true;
    CHECK(!Selection::beginGesture(session, t, 1, 1, 1, 0, activating));
    CHECK(!Selection::isGestureOpen(session));
    CHECK(!Selection::dragGesture(session, t, 2, 2, 0));
    CHECK(Selection::selectedText(session, t) == standing);

    // The same press reported to a mouse-tracking application keeps it too;
    // an ordinary reported press drops it, as a keystroke would.
    CHECK(!Selection::clearSelectionForReportedPress(session, activating));
    CHECK(Selection::hasSelection(session));
    CHECK(Selection::clearSelectionForReportedPress(session, {}));
    CHECK(!Selection::hasSelection(session));

    // The click after the activating one opens as any click does.
    Selection::beginGesture(session, t, 0, 0, 1, 0, {});
    CHECK(Selection::isGestureOpen(session));
}

TEST(aHeldViewportIsNotYankedByTheRowsAResizeRewraps)
{
    Terminal t;
    hardReset(t);
    resize(t, 3, 40);
    Screen::dropScrollback(&t);
    feed(t, "\033[Hone one one one one one one one one one\r\ntwo\r\nthree\r\nfour\r\nfive");
    CHECK(Screen::scrollbackCount(&t) > 0);

    Selection::Session session = freshSession();
    const int viewOffset = 1;
    (void)Selection::updateAfterOutput(session, t, viewOffset, Input::OutputSnapPolicy::HoldViewedRows);
    const long long appendedBefore = Screen::scrollbackTotalAppended(&t);

    // A narrower grid rewraps the long line into more rows. Whatever the
    // rewrap does to the appended count, those rows are the resize's own
    // business (the frontend carries the offset across a reflow itself):
    // the next output transition must not read them as growth.
    resize(t, 3, 12);
    const Selection::OutputUpdate update = Selection::updateAfterOutput(
        session, t, viewOffset, Input::OutputSnapPolicy::HoldViewedRows);
    CHECK(update.viewport.hold == Input::ViewportHold::Hold);
    CHECK_MSG(update.viewport.viewOffset == viewOffset,
        "a resize's rows counted as output growth and moved a held view");
    (void)appendedBefore;

    // Ordinary output after the resize is judged from the resize's facts.
    feed(t, "\r\nsix");
    const Selection::OutputUpdate grown = Selection::updateAfterOutput(
        session, t, viewOffset, Input::OutputSnapPolicy::HoldViewedRows);
    CHECK_EQ(grown.viewport.viewOffset, viewOffset + 1);
}

// --- Content that moves in place ---------------------------------------------
//
// A full-screen scroll on the primary screen appends to history, and a
// selection follows it by construction: its rows keep their absolute numbers
// while the history count grows. A region scroll, IL/DL, a margined pane and
// the alternate screen move rows without touching history, so the terminal
// moves the pins the selection's ends name, and the selection follows them at
// its next output judgment.

namespace {

std::string selectRow(Selection::Session& session, Terminal& t, int row, int fromColumn, int toColumn)
{
    Selection::beginGesture(session, t, row, fromColumn, /*clickCount*/ 1, 0, {});
    Selection::dragGesture(session, t, row, toColumn, 0);
    Selection::endGesture(session);
    return Selection::selectedText(session, t);
}

// One drain's judgment, as every frontend asks it after output.
bool judge(Selection::Session& session, Terminal& t)
{
    return Selection::updateAfterOutput(session, t, 0).didChangeSelection;
}

} // namespace

TEST(aSelectionOnTheAlternateScreenRidesUpWithItsContent)
{
    Terminal t;
    hardReset(t);
    resize(t, 5, 20);
    feed(t, "\033[?1049h"); // the alternate screen keeps no history
    feed(t, "alpha\r\nbeta\r\ngamma\r\ndelta\r\nepsilon");
    Selection::Session session = freshSession();
    CHECK(selectRow(session, t, 2, 0, 4) == "gamma");

    // A line feed at the bottom scrolls every row up one in place.
    feed(t, "\r\nzeta");
    CHECK_MSG(judge(session, t), "the move is a repaint");
    CHECK_MSG(Selection::selectedText(session, t) == "gamma", "the selection followed its content to row 1");

    // Scrolled off the top, the content is gone and so is the selection.
    feed(t, "\r\none\r\ntwo");
    judge(session, t);
    CHECK_MSG(!Selection::hasSelection(session), "content scrolled off the alternate screen takes the selection with it");
}

TEST(aSelectionInsideAScrollRegionFollowsItAndOutsideStandsStill)
{
    Terminal t;
    hardReset(t);
    resize(t, 6, 20);
    feed(t, "head\r\nrow1\r\nrow2\r\nrow3\r\nrow4\r\nfoot");
    feed(t, "\033[2;5r"); // DECSTBM: rows 1..4 (zero-based) scroll, head and foot stand
    feed(t, "\033[5;1H"); // the cursor at the region's bottom

    Selection::Session inside = freshSession();
    CHECK(selectRow(inside, t, 3, 0, 3) == "row3");
    Selection::Session outside = freshSession();
    CHECK(selectRow(outside, t, 5, 0, 3) == "foot");

    feed(t, "\r\nrow5"); // the region scrolls up one; the footer does not move
    CHECK(judge(inside, t));
    CHECK_MSG(Selection::selectedText(inside, t) == "row3", "a selection inside the region followed the scroll");
    CHECK_MSG(!judge(outside, t), "a selection outside the region has nothing to follow");
    CHECK_MSG(Selection::selectedText(outside, t) == "foot", "and stays on its content");

    // A selection spanning the region's edge shears: the top moved, the bottom did not.
    Selection::Session spanning = freshSession();
    Selection::beginGesture(spanning, t, 3, 0, 1, 0, {});
    Selection::dragGesture(spanning, t, 5, 3, 0);
    Selection::endGesture(spanning);
    CHECK(Selection::hasSelection(spanning));
    feed(t, "\r\nrow6");
    judge(spanning, t);
    CHECK_MSG(!Selection::hasSelection(spanning), "a selection across the region's edge drops rather than shear");
}

TEST(insertAndDeleteLineMoveTheRowsBelowTheCursor)
{
    Terminal t;
    hardReset(t);
    resize(t, 6, 20);
    feed(t, "a\r\nb\r\nc\r\nd\r\ne\r\nf");
    Selection::Session session = freshSession();
    CHECK(selectRow(session, t, 3, 0, 0) == "d");

    feed(t, "\033[2;1H\033[L"); // IL at row 1: b..e move down one
    CHECK(judge(session, t));
    CHECK_MSG(Selection::selectedText(session, t) == "d", "the selection moved down with its row");

    feed(t, "\033[2;1H\033[M"); // DL at row 1: the inserted blank goes, rows come back up
    CHECK(judge(session, t));
    CHECK_MSG(Selection::selectedText(session, t) == "d", "and back up again");

    Selection::Session above = freshSession();
    CHECK(selectRow(above, t, 0, 0, 0) == "a");
    feed(t, "\033[3;1H\033[M");
    CHECK_MSG(!judge(above, t), "a row above the edit stands still");
    CHECK(Selection::selectedText(above, t) == "a");
}

TEST(aSelectionInAMarginedPaneFollowsThePanesScroll)
{
    Terminal t;
    hardReset(t);
    resize(t, 5, 20);
    feed(t, "left-one  right-one\r\nleft-two  right-two\r\nleft-thr  right-thr\r\nleft-fou  right-fou\r\nleft-fiv  right-fiv");
    feed(t, "\033[?69h\033[11;20s"); // DECSLRM: columns 10..19 are the pane
    feed(t, "\033[5;11H"); // the cursor at the pane's bottom

    Selection::Session pane = freshSession();
    CHECK(selectRow(pane, t, 2, 10, 18) == "right-thr");
    Selection::Session other = freshSession();
    CHECK(selectRow(other, t, 2, 0, 7) == "left-thr");

    feed(t, "\r\n"); // the pane scrolls up one; the left column stands
    feed(t, "\033[5;11Hright-six");
    CHECK(judge(pane, t));
    CHECK_MSG(Selection::selectedText(pane, t) == "right-thr", "the pane's selection followed the pane's scroll");
    judge(other, t);
    CHECK_MSG(Selection::selectedText(other, t) == "left-thr", "the other column's selection has nothing to follow");
}

TEST(anOpenGestureFollowsAnInPlaceScroll)
{
    Terminal t;
    hardReset(t);
    resize(t, 5, 20);
    feed(t, "\033[?1049h");
    feed(t, "alpha\r\nbeta\r\ngamma\r\ndelta\r\nepsilon");
    Selection::Session session = freshSession();
    Selection::beginGesture(session, t, 1, 0, 1, 0, {});
    Selection::dragGesture(session, t, 2, 4, 0);
    CHECK(Selection::selectedText(session, t) == "beta\ngamma");

    feed(t, "\r\nzeta"); // the whole screen moves up one under the held button
    judge(session, t);
    CHECK_MSG(Selection::selectedText(session, t).rfind("beta", 0) == 0, "the held anchor moved with its content");
    Selection::dragGesture(session, t, 2, 4, 0);
    CHECK_MSG(Selection::selectedText(session, t) == "beta\ngamma\ndelta", "and the drag extends from the moved anchor");
}

// --- Views of one terminal ---------------------------------------------------
//
// Two windows on one session, or a surface adopting another's terminal, each
// hold a selection of their own. An in-place move is applied to the names of
// the lines those selections stand on, not handed to whichever view asks
// first, so each view follows it at its own judgment, however late.

namespace {

// Scrolls the band whose last row is `bottomRow` once per line, writes each
// new line on that row, and has `view` select it.
//
// - Returns: The most line names the terminal held at once.
std::size_t selectEveryLineScrolledThrough(Selection::Session& view, Terminal& t, int bottomRow, int lines)
{
    std::size_t mostNames = 0;
    const std::string bottom = "\033[" + std::to_string(bottomRow + 1) + ";1H\n";
    for (int line = 0; line < lines; ++line) {
        feed(t, bottom + "line " + std::to_string(line));
        if (selectRow(view, t, bottomRow, 0, 3) != "line") {
            CHECK_MSG(false, ("line " + std::to_string(line) + " could not be selected").c_str());
            break;
        }
        mostNames = std::max(mostNames, t.lineIdentity ? t.lineIdentity->primary.pins.size() : 0);
    }
    return mostNames;
}

} // namespace

TEST(everyViewOfATerminalFollowsARegionScrollWhicheverDrainsFirst)
{
    Terminal t;
    hardReset(t);
    resize(t, 10, 20);
    feed(t, "head\r\nrow1\r\nrow2\r\nrow3\r\nrow4\r\nrow5\r\nrow6\r\nrow7\r\nrow8\r\nfoot");
    feed(t, "\033[2;9r"); // DECSTBM: rows 1-8 scroll, head and foot stand

    Selection::Session first = freshSession();
    REQUIRE(selectRow(first, t, 7, 0, 3) == "row7");
    Selection::Session second = freshSession();
    REQUIRE(selectRow(second, t, 8, 0, 3) == "row8");

    feed(t, "\033[S"); // SU: the region moves up a row
    // A view with nothing selected asks first, as a surface opened for a moment does.
    Selection::Session passing = freshSession();
    CHECK(!Selection::hasSelection(passing, t));
    CHECK_MSG(judge(first, t), "the first view's drain follows the scroll");
    feed(t, "\033[2S");
    CHECK(judge(first, t));
    CHECK_MSG(Selection::selectedText(first, t) == "row7", "the view that drained followed both scrolls");

    // The second view asks only now, three rows later.
    const std::optional<SelectionBounds> moved = Selection::selectedBounds(second, t);
    REQUIRE(moved.has_value());
    CHECK_MSG(moved->row0 == 5 && moved->row1 == 5, "the view that did not drain moved up with its text");
    CHECK_MSG(Selection::selectedText(second, t) == "row8", "and copies the text it selected");
}

// Nothing is kept per move for a view to catch up on, so a view that never
// asks costs the terminal nothing however much moves: the table of line names
// is bounded by the screen however many lines other views select. A view whose
// line is still on the screen keeps its name through the table's turnover.
TEST(aViewThatNeverAsksKeepsItsSelectionWhileTheLineNamesStayBounded)
{
    Terminal t;
    hardReset(t);
    resize(t, 8, 20);
    feed(t, "\033[2;1Hkeeper");
    Selection::Session idle = freshSession();
    REQUIRE(selectRow(idle, t, 1, 0, 5) == "keeper");

    // An IL above the idle view's line, then a band below it that scrolls
    // through more lines than the table holds names for, each selected by
    // another view.
    feed(t, "\033[1;1H\033[L");
    feed(t, "\033[4;8r"); // the band: rows 3-7
    Selection::Session busy = freshSession();
    const std::size_t mostNames = selectEveryLineScrolledThrough(busy, t, 7, static_cast<int>(Screen::BufferPins::maxPins) + 64);
    CHECK_MSG(mostNames <= Screen::BufferPins::maxPins, "the line names stay within their table");

    CHECK_MSG(judge(idle, t), "the idle view's first judgment follows the IL it missed");
    const std::optional<SelectionBounds> moved = Selection::selectedBounds(idle, t);
    REQUIRE(moved.has_value());
    CHECK(moved->row0 == 2 && moved->row1 == 2);
    CHECK_MSG(Selection::selectedText(idle, t) == "keeper", "and its selection is still on its text");
}

// A view that does not ask while its line leaves the band, and while the table
// of names turns over, finds its selection gone at its next judgment: the move
// took the text it selected, and a name the table gave up names nothing rather
// than another line. Moves a selection cannot follow drop it, as two bands in
// one drain do, rather than leave it over text it did not select.
TEST(aViewThatFallsBehindLosesItsSelectionRatherThanLandingOnOtherText)
{
    Terminal t;
    hardReset(t);
    resize(t, 8, 20);
    feed(t, "\033[4;8r\033[5;1Hselected"); // the band: rows 3-7; the text on row 4
    Selection::Session idle = freshSession();
    REQUIRE(selectRow(idle, t, 4, 0, 7) == "selected");

    Selection::Session busy = freshSession();
    const std::size_t mostNames = selectEveryLineScrolledThrough(busy, t, 7, static_cast<int>(Screen::BufferPins::maxPins) + 64);
    CHECK(mostNames <= Screen::BufferPins::maxPins);

    CHECK_MSG(!Selection::hasSelection(idle, t), "the line the idle view selected scrolled off the band");
    CHECK_MSG(Selection::selectedText(idle, t).empty(), "and nothing else stands in its place");
}

// --- The alternate screen ----------------------------------------------------
//
// A full-screen program repaints the alternate screen in place, erasing and
// rewriting rows as it pleases, and keeps no history. A selection made there,
// with Shift while the program reports the mouse, covers screen cells: it keeps
// them through every redraw and follows only an in-place move of its rows. It
// goes when the program leaves the alternate screen or the window is resized.

namespace {

// Clears the screen and paints one frame by cursor address, row `r` reading
// "<prefix> r".
void paintFrame(Terminal& t, std::string_view prefix)
{
    feed(t, "\033[H\033[2J");
    for (int row = 0; row < t.rows; ++row)
        feed(t, "\033[" + std::to_string(row + 1) + ";1H" + std::string(prefix) + " " + std::to_string(row));
}

// Enters the alternate screen with mouse reporting on, as vim, htop and less
// do, and paints the program's first frame.
void enterFullScreenProgram(Terminal& t)
{
    feed(t, "\033[?1049h\033[?1000h");
    paintFrame(t, "first frame row");
}

// Whether the selection covers exactly these cells, as the highlight reads them.
bool covers(Selection::Session& session, Terminal& t, int row0, int column0, int row1, int column1)
{
    const std::optional<SelectionBounds> bounds = Selection::selectedBounds(session, t);
    return bounds && bounds->row0 == row0 && bounds->column0 == column0 && bounds->row1 == row1 && bounds->column1 == column1;
}

} // namespace

TEST(aSelectionOnTheAlternateScreenKeepsItsCellsThroughEveryRedraw)
{
    Terminal t;
    hardReset(t);
    resize(t, 8, 30);
    enterFullScreenProgram(t);
    Selection::Session session = freshSession();
    Selection::beginGesture(session, t, 2, 6, 1, 0, {});
    Selection::dragGesture(session, t, 4, 10, 0);
    Selection::endGesture(session);
    REQUIRE(covers(session, t, 2, 6, 4, 10));

    // Rows the selection does not reach: a status line and a title.
    feed(t, "\033[8;1H\033[2Kstatus 1\033[1;1H\033[2Ktitle 1");
    CHECK_MSG(!judge(session, t), "nothing the selection covers moved");
    CHECK_MSG(covers(session, t, 2, 6, 4, 10), "a redraw of other rows leaves the selection alone");

    // The rows holding the selection's ends erased whole and written again, as
    // a status pane is.
    feed(t, "\033[3;1H\033[2Krewritten row 2\033[5;1H\033[2Krewritten row 4");
    CHECK(!judge(session, t));
    CHECK_MSG(covers(session, t, 2, 6, 4, 10), "an erased and rewritten row keeps the selection on its cells");

    // EL from the cursor, ECH, ICH and DCH inside the selected rows.
    feed(t, "\033[3;9H\033[K\033[5;7H\033[3X\033[4;2H\033[2@\033[4;4H\033[3P");
    CHECK(!judge(session, t));
    CHECK_MSG(covers(session, t, 2, 6, 4, 10), "erases and character edits inside the selection keep it");

    // The whole screen cleared and painted again with other text.
    paintFrame(t, "other frame row");
    CHECK(!judge(session, t));
    CHECK_MSG(covers(session, t, 2, 6, 4, 10), "a full repaint keeps the selection on its cells");
    CHECK_MSG(Selection::selectedText(session, t) == "frame row 2\nother frame row 3\nother frame", "and a copy reads what those cells hold now");

    // An open drag keeps its press through a repaint and extends from it.
    Selection::Session held = freshSession();
    Selection::beginGesture(held, t, 1, 0, 1, 0, {});
    Selection::dragGesture(held, t, 2, 4, 0);
    paintFrame(t, "third frame row");
    judge(held, t);
    REQUIRE_MSG(Selection::hasSelection(held, t), "an open drag outlived the repaint");
    Selection::dragGesture(held, t, 3, 4, 0);
    CHECK_MSG(covers(held, t, 1, 0, 3, 4), "and extends from where it was pressed");
}

TEST(aSelectionOnTheAlternateScreenRidesARegionScrollAndKeepsItsCellsAfter)
{
    Terminal t;
    hardReset(t);
    resize(t, 8, 30);
    enterFullScreenProgram(t);
    Selection::Session session = freshSession();
    Selection::beginGesture(session, t, 3, 0, 1, 0, {});
    Selection::dragGesture(session, t, 4, 7, 0);
    Selection::endGesture(session);
    REQUIRE(covers(session, t, 3, 0, 4, 7));

    // A scroll region below the selection moves nothing it covers.
    feed(t, "\033[6;8r\033[8;1H\nnew row 7\033[r");
    CHECK(!judge(session, t));
    CHECK_MSG(covers(session, t, 3, 0, 4, 7), "a scroll below the selection leaves it standing");

    // A scroll region holding the selection carries it up a row with its rows,
    // and the program repaints the row the scroll revealed and the rows it
    // moved, as an editor scrolling one line does.
    feed(t, "\033[2;7r\033[7;1H\n\033[r\033[7;1H\033[2Krevealed row\033[3;1H\033[2Kmoved row 2\033[4;1H\033[2Kmoved row 3");
    CHECK_MSG(judge(session, t), "the selection moved");
    CHECK_MSG(covers(session, t, 2, 0, 3, 7), "it rides the region scroll and keeps those cells through the repaint");

    // Carried off the region's top, the rows are gone, and the selection too.
    feed(t, "\033[2;7r\033[7;1H\n\n\033[r");
    judge(session, t);
    CHECK_MSG(!Selection::hasSelection(session, t), "a selection carried off the region goes with its rows");
}

TEST(aSelectionOnAWrappedAlternateRowRidesAScrollThatTakesTheLinesFirstRow)
{
    Terminal t;
    hardReset(t);
    resize(t, 6, 10);
    feed(t, "\033[?1049h");
    // Row 0 wraps into row 1: one line of two rows, as less prints a long one.
    feed(t, "0123456789abcdefghij\r\nrow 2\r\nrow 3\r\nrow 4\r\nrow 5");
    Selection::Session session = freshSession();
    REQUIRE(selectRow(session, t, 1, 0, 4) == "abcde");

    // A line feed at the bottom scrolls the line's first row off the screen;
    // the selected row is still there, a row higher.
    feed(t, "\r\nrow 6");
    CHECK(judge(session, t));
    CHECK_MSG(covers(session, t, 0, 0, 0, 4), "the selection follows its row, not the line it began");
}

TEST(everyViewOfTheAlternateScreenFollowsARegionScrollWhicheverDrainsFirst)
{
    Terminal t;
    hardReset(t);
    resize(t, 8, 30);
    enterFullScreenProgram(t);
    Selection::Session first = freshSession();
    REQUIRE(selectRow(first, t, 3, 0, 16) == "first frame row 3");
    Selection::Session second = freshSession();
    REQUIRE(selectRow(second, t, 5, 0, 16) == "first frame row 5");

    feed(t, "\033[2;7r\033[S\033[r"); // SU inside rows 1-6, as an editor scrolls its text pane
    Selection::Session passing = freshSession();
    CHECK(!Selection::hasSelection(passing, t));
    CHECK(judge(first, t));
    CHECK_MSG(covers(first, t, 2, 0, 2, 16), "the view that drained rode the scroll");
    CHECK_MSG(covers(second, t, 4, 0, 4, 16), "the view that did not drain rode it too");
    CHECK_MSG(Selection::selectedText(second, t) == "first frame row 5", "and copies the cells it selected");
}

TEST(aSelectionDoesNotCrossBetweenTheScreens)
{
    Terminal t;
    hardReset(t);
    resize(t, 8, 30);
    feed(t, "shell output\r\nmore output");
    Selection::Session primary = freshSession();
    REQUIRE(selectRow(primary, t, 0, 0, 4) == "shell");

    // Entering the alternate screen takes a selection of the shell's rows, and
    // leaving it does not bring one back: its cells were out of sight.
    enterFullScreenProgram(t);
    judge(primary, t);
    CHECK_MSG(!Selection::hasSelection(primary, t), "a selection of the primary screen does not stand over the program's");

    Selection::Session alternate = freshSession();
    REQUIRE(selectRow(alternate, t, 2, 0, 4) == "first");
    feed(t, "\033[?1000l\033[?1049l");
    judge(alternate, t);
    judge(primary, t);
    CHECK_MSG(!Selection::hasSelection(alternate, t), "a selection of the program's screen goes with it");
    CHECK_MSG(!Selection::hasSelection(primary, t), "and the shell's does not come back");
}

TEST(aResizeDropsASelectionOnTheAlternateScreen)
{
    Terminal t;
    hardReset(t);
    resize(t, 8, 30);
    enterFullScreenProgram(t);
    Selection::Session session = freshSession();
    REQUIRE(selectRow(session, t, 2, 0, 10) == "first frame");

    // The program lays its frame out again for the new size, so the cells
    // under the selection no longer hold what was selected; nothing names
    // that text there, so the selection goes, with the resize.
    const std::uint32_t epochBefore = t.absoluteRowEpoch;
    resize(t, 8, 24);
    CHECK_MSG(Selection::dropSelectionAcrossResize(session.state, epochBefore, t), "the frontend drops it with the resize");
    CHECK(!Selection::hasSelection(session));

    Selection::Session unjudged = freshSession();
    REQUIRE(selectRow(unjudged, t, 2, 0, 10) == "first frame");
    resize(t, 6, 24);
    paintFrame(t, "resized frame row");
    judge(unjudged, t);
    CHECK_MSG(!Selection::hasSelection(unjudged, t), "and the next judgment drops one the frontend did not");
}

namespace {

// A scripted three-command session, marked the way a shell integration marks
// one: the executed prompt row, the row its output starts on, and the row after
// the output where the command ended.
//
//   0  $ echo one     A/executed      3  $ echo two      A/executed
//   1  one            C/output start  4  two             C/output start
//   2  (end of one)   D/cmd end       5  (end of two)    D/cmd end
Terminal& scriptedSession(Terminal& t)
{
    hardReset(t);
    resize(t, 8, 20);
    feed(t, "$ echo one\r\none\r\n.\r\n$ echo two\r\ntwo\r\n.");
    Screen::addRowMark(&t, 0, Screen::osc133MarkCmdExecuted);
    Screen::addRowMark(&t, 1, Screen::osc133MarkOutputStart);
    Screen::addRowMark(&t, 2, Screen::osc133MarkCmdEnd);
    Screen::addRowMark(&t, 3, Screen::osc133MarkCmdExecuted);
    Screen::addRowMark(&t, 4, Screen::osc133MarkOutputStart);
    Screen::addRowMark(&t, 5, Screen::osc133MarkCmdEnd);
    return t;
}

} // namespace

TEST(aClickOnAPromptRowCopiesThatCommandsOwnOutput)
{
    Terminal t;
    scriptedSession(t);
    Selection::Session session = freshSession();

    // The click that would be read wrong: the prompt row of the second
    // command sits ABOVE its output, so a search anchored on the click finds
    // the first command's output instead of the second's.
    CHECK(Selection::selectCommandOutputUnderRow(session.state, t, 3));
    CHECK_MSG(Selection::selectedText(session, t) == "two", "a click on the prompt row took the previous command's output");

    // A click on the output row itself answers the same, which is what makes
    // the anchoring invisible to a user rather than a second behaviour.
    CHECK(Selection::selectCommandOutputUnderRow(session.state, t, 4));
    CHECK(Selection::selectedText(session, t) == "two");

    // And the command above still has its own.
    CHECK(Selection::selectCommandOutputUnderRow(session.state, t, 0));
    CHECK(Selection::selectedText(session, t) == "one");
}

TEST(aClickCopiesTheCommandLineWholeRows)
{
    Terminal t;
    scriptedSession(t);
    Selection::Session session = freshSession();

    // The prompt row through the row before output begins, taken as whole
    // rows: what was typed, with the prompt that introduced it.
    CHECK(Selection::selectCommandLineUnderRow(session.state, t, 4));
    CHECK_MSG(Selection::selectedText(session, t) == "$ echo two", "the command line came back as something other than its rows");

    CHECK(Selection::hasCommandUnderRow(t, 4));
}

TEST(aRowUnderNoCommandSelectsNothing)
{
    Terminal t;
    hardReset(t);
    resize(t, 8, 20);
    feed(t, "no shell integration here");
    Selection::Session session = freshSession();

    // Unmarked rows are the common case — most shells are not configured —
    // and the answer has to be "there is nothing here" rather than a span
    // over whatever happens to be on screen.
    CHECK(!Selection::hasCommandUnderRow(t, 0));
    CHECK(!Selection::selectCommandLineUnderRow(session.state, t, 0));
    CHECK(!Selection::selectCommandOutputUnderRow(session.state, t, 0));
    CHECK(!Selection::hasSelection(session));
}

TEST(twoBandsInOneDrainDropTheSelection)
{
    Terminal t;
    hardReset(t);
    resize(t, 6, 20);
    feed(t, "a\r\nb\r\nc\r\nd\r\ne\r\nf");
    Selection::Session session = freshSession();
    CHECK(selectRow(session, t, 4, 0, 0) == "e");
    feed(t, "\033[2;1H\033[L\033[4;1H\033[L"); // IL at row 1, then IL at row 3: two bands
    judge(session, t);
    CHECK_MSG(!Selection::hasSelection(session), "two bands cannot compose to one move, so the selection drops");
}

TEST(visualRTLSelectionsCopyOriginalLogicalText)
{
    Terminal terminal;
    hardReset(terminal);
    resize(terminal, 6, 60);
    feed(terminal, "\x1b[?25lبسم\r\nلا\r\nسلام دنیا  שלום עולם\r\n東京 가나다 オペレーション");
    CHECK(dragCopy(terminal, 2, 11, 2, 19) == "سلام دنیا");
    CHECK(dragCopy(terminal, 2, 19, 2, 11) == "سلام دنیا");
    CHECK(dragCopy(terminal, 2, 0, 2, 8) == "שלום עולם");
    CHECK(dragCopy(terminal, 2, 0, 2, 19) == "سلام دنیا  שלום עולם");
    CHECK(dragCopy(terminal, 1, 0, 1, 1) == "لا");
    CHECK(dragCopy(terminal, 1, 0, 1, 0) == "لا");
    CHECK(dragCopy(terminal, 1, 1, 1, 1) == "لا");
    CHECK(dragCopy(terminal, 0, 0, 0, 2) == "بسم");
}

// A frontend reports the pointer in screen cells, and a double-width row draws
// its logical column c across screen cells 2c and 2c+1, so the gesture lands on
// half the screen column. Reported straight through, a drag over "CDE" on a
// DECDWL row selected and copied the text to its right, and a double-click
// opened on the word twice as far along the row.
TEST(aGestureOnADoubleWidthRowLandsOnTheGlyphUnderThePointer)
{
    Terminal t;
    hardReset(t);
    resize(t, 4, 20);
    feed(t, "\x1b#6ABCDEFGH\r\nABCDEFGH");
    // Screen cells 4 to 9 are the glyphs C, D and E.
    CHECK(dragCopy(t, 0, 4, 0, 9) == "CDE");
    // The plain row below is the control: the same screen cells are E..J.
    CHECK(dragCopy(t, 1, 4, 1, 7) == "EFGH");
    // A screen cell past the doubled row's visible half lands on its last
    // visible glyph rather than on a cell nothing draws.
    CHECK(dragCopy(t, 0, 0, 0, 19) == "ABCDEFGH");

    Terminal words;
    hardReset(words);
    resize(words, 4, 20);
    feed(words, "\x1b#6foo bar");
    Selection::Session session = freshSession();
    // Screen cell 9 is the `b` of `bar`, logical column 4.
    Selection::beginGesture(session, words, 0, 9, /*clickCount*/ 2, 0, {});
    CHECK(Selection::selectedText(session, words) == "bar");

    // The caret halves the same way, and a run's point inside it with it.
    const Selection::CaretResolution caret = Selection::resolveSelectionCaret(t, 0, 0, 4, 4.0);
    CHECK(caret.caret == 2);
    CHECK(Selection::isCellInTextVoid(t, 0, 0, 15, Selection::TextRestriction::ToText) == false);
    CHECK(Selection::isCellInTextVoid(t, 0, 0, 17, Selection::TextRestriction::ToText) == true);
}

// Both halves of a double-height pair carry the same text, and the reader sees
// one line. A selection over the pair copies that line once; the bottom half
// selected on its own still copies its text, since it is the only half shown.
TEST(aDoubleHeightPairCopiesAsOneLine)
{
    Terminal t;
    hardReset(t);
    resize(t, 6, 20);
    feed(t, "\x1b#3Hello\r\n\x1b#4Hello\r\nnext");
    CHECK(dragCopy(t, 0, 0, 1, 9) == "Hello");
    CHECK(dragCopy(t, 0, 0, 2, 7) == "Hello\nnext");
    CHECK(dragCopy(t, 1, 0, 1, 9) == "Hello");
    CHECK(dragCopy(t, 1, 0, 2, 7) == "Hello\nnext");

    Terminal alternate;
    hardReset(alternate);
    resize(alternate, 6, 20);
    feed(alternate, "\x1b[?1049h\x1b#3Hello\r\n\x1b#4Hello\r\nnext");
    CHECK(dragCopy(alternate, 0, 0, 2, 7) == "Hello\nnext");
}

// The styled flavours walk the same rows, so a double-height pair is one line
// there too.
TEST(aDoubleHeightPairCopiesOnceInEveryFlavour)
{
    Terminal t;
    hardReset(t);
    resize(t, 6, 20);
    feed(t, "\x1b#3Hello\r\n\x1b#4Hello\r\nnext");
    Selection::Session session = freshSession();
    Selection::beginGesture(session, t, 0, 0, 1, 0, {});
    Selection::dragGesture(session, t, 2, 7, 0);
    const std::string plain = Selection::selectedText(session, t);
    CHECK(plain == "Hello\nnext");
    const CellPalette palette = CellPalette::fromTerminalPalette(t.palette);
    const Selection::StyledFont font { "monospace", 16.0 };
    const auto styled = Selection::styledClipboard(session, t, Selection::StyleColorMode::Full, palette, font);
    CHECK(styled.plain == plain);
    CHECK(styled.html.find("Hello") != std::string::npos);
    CHECK(styled.html.find("Hello") == styled.html.rfind("Hello"));
}

// A sized block's lower band rows show the same letters as its top row, so a
// selection that covers only a lower band is a selection of those letters.
TEST(aSelectionOverASizedBlocksLowerBandCopiesItsLetters)
{
    Terminal t;
    hardReset(t);
    resize(t, 6, 20);
    t.drawsSizedText = true;
    t.drawsProportionalText = true;
    feed(t, "\x1b]66;s=2;Top\x07");
    CHECK(dragCopy(t, 0, 0, 0, 5) == "Top");
    CHECK_MSG(dragCopy(t, 1, 0, 1, 5) == "Top", dragCopy(t, 1, 0, 1, 5).c_str());
    CHECK_MSG(dragCopy(t, 0, 0, 1, 5) == "Top", dragCopy(t, 0, 0, 1, 5).c_str());

    // A double-click on any band of a scaled word takes the whole word.
    for (int row = 0; row < 2; ++row) {
        Selection::Session session = freshSession();
        Selection::beginGesture(session, t, row, 3, /*clickCount*/ 2, 0, {});
        CHECK_MSG(Selection::selectedText(session, t) == "Top", Selection::selectedText(session, t).c_str());
    }
}

// Both halves of a double-height pair show one line of glyphs, so a selection
// is of the glyphs, not of a half: a gesture on either half selects the
// same columns of the pair, the highlight covers both halves of every selected
// glyph, and the copy is the line once.
TEST(aSelectionOnEitherHalfOfADoubleHeightPairLightsBothHalves)
{
    Terminal t;
    hardReset(t);
    resize(t, 6, 20);
    feed(t, "\x1b#3Hello\r\n\x1b#4Hello\r\nnext");

    // A drag along the bottom half over screen cells 2..7: the glyphs e, l, l.
    Selection::Session bottom = freshSession();
    Selection::beginGesture(bottom, t, 1, 2, 1, 0, {});
    Selection::dragGesture(bottom, t, 1, 7, 0);
    CHECK_MSG(Selection::selectedText(bottom, t) == "ell", Selection::selectedText(bottom, t).c_str());
    Rendering::SelectionRowSpan spans[6] {};
    int count = Selection::viewportSpans(bottom, t, 0, spans);
    CHECK_EQ(count, 2);
    CHECK(spans[0].visibleRow == 0 && spans[0].column0 == 1 && spans[0].column1 == 3);
    CHECK(spans[1].visibleRow == 1 && spans[1].column0 == 1 && spans[1].column1 == 3);

    // A drag from the top half down onto the bottom half is the one line too.
    Selection::Session across = freshSession();
    Selection::beginGesture(across, t, 0, 0, 1, 0, {});
    Selection::dragGesture(across, t, 1, 9, 0);
    CHECK_MSG(Selection::selectedText(across, t) == "Hello", Selection::selectedText(across, t).c_str());
    count = Selection::viewportSpans(across, t, 0, spans);
    CHECK_EQ(count, 2);
    CHECK(spans[0].visibleRow == 0 && spans[0].column0 == 0 && spans[0].column1 == 4);
    CHECK(spans[1].visibleRow == 1 && spans[1].column0 == 0 && spans[1].column1 == 4);

    // Past the pair, the plain row below keeps its own span.
    Selection::Session onward = freshSession();
    Selection::beginGesture(onward, t, 1, 4, 1, 0, {});
    Selection::dragGesture(onward, t, 2, 3, 0);
    CHECK_MSG(Selection::selectedText(onward, t) == "llo\nnext", Selection::selectedText(onward, t).c_str());
    count = Selection::viewportSpans(onward, t, 0, spans);
    CHECK_EQ(count, 3);
    CHECK(spans[0].visibleRow == 0 && spans[0].column0 == 2);
    CHECK(spans[1].visibleRow == 1 && spans[1].column0 == 2);
    CHECK(spans[2].visibleRow == 2 && spans[2].column0 == 0 && spans[2].column1 == 3);

    // The caret path maps the same way: a caret on the bottom half resolves
    // to the pair's row.
    const Selection::CaretResolution caret = Selection::resolveSelectionCaret(t, 0, 1, 4, 4.0);
    CHECK_EQ(caret.absoluteRow, 0);
    CHECK_EQ(caret.caret, 2);
}

// --- The Find hand-off -------------------------------------------------------
//
// Closing a find bar hands the focused match over as the view's selection, so
// the text the user was looking at stays selected and copies as it was found.

TEST(aWrappedSearchMatchHandsOverItsWholeText)
{
    Terminal t;
    hardReset(t);
    setScrollbackLimit(&t, 100);
    resize(t, 6, 10);
    for (int line = 0; line < 12; ++line)
        feed(t, "history " + std::to_string(line) + "\r\n");
    // Ten columns wrap the match over three rows of the screen.
    feed(t, "say needle-in-a-haystack here");
    REQUIRE(Screen::scrollbackCount(&t) > 0);
    {
        UbiquittyTest::SearchFixture search(t);
        REQUIRE(search.find("needle-in-a-haystack") == 1);
        REQUIRE(Screen::ScrollSearch::segmentCount(t) == 3);
        Selection::Session session = freshSession();
        REQUIRE(Selection::selectActiveSearchMatch(session.state, t));
        CHECK_MSG(Selection::selectedText(session, t) == "needle-in-a-haystack", "the hand-off takes every row of the match");
    }

    // Scrolled into history, the match still hands over whole.
    feed(t, "\r\nmore\r\nmore\r\nmore\r\nmore\r\nmore\r\nmore");
    UbiquittyTest::SearchFixture search(t);
    REQUIRE(search.find("needle-in-a-haystack") == 1);
    const Screen::ScrollSearch::Match* active = Screen::ScrollSearch::activeMatch(t);
    REQUIRE(active != nullptr);
    REQUIRE(active->absoluteRow < Screen::scrollbackCount(&t));
    Selection::Session session = freshSession();
    REQUIRE(Selection::selectActiveSearchMatch(session.state, t));
    CHECK_MSG(Selection::selectedText(session, t) == "needle-in-a-haystack", "a match in history hands over every row too");
}

TEST(aSearchMatchOnTheAlternateScreenHandsOverTheTextItMatched)
{
    Terminal t;
    hardReset(t);
    resize(t, 6, 30);
    for (int line = 0; line < 20; ++line)
        feed(t, "shell line " + std::to_string(line) + "\r\n");
    REQUIRE(Screen::scrollbackCount(&t) > t.rows);
    // The match starts on row 3 at column 26 and wraps onto row 4.
    feed(t, "\033[?1049h\033[2;1Hprogram text\033[4;27Hneedle");

    UbiquittyTest::SearchFixture search(t);
    REQUIRE(search.find("needle") == 1);
    Selection::Session session = freshSession();
    REQUIRE(Selection::selectActiveSearchMatch(session.state, t));
    const std::optional<SelectionBounds> bounds = Selection::selectedBounds(session, t);
    REQUIRE(bounds.has_value());
    CHECK_MSG(bounds->row0 == 3 && bounds->column0 == 26 && bounds->row1 == 4 && bounds->column1 == 1,
        "the selection is on the alternate screen's own rows, not the history behind it");
    CHECK(Selection::selectedText(session, t) == "needle");
}

TEST(aMatchInTheHistoryBehindTheAlternateScreenHandsOverNothing)
{
    Terminal t;
    hardReset(t);
    resize(t, 6, 30);
    feed(t, "needle in the shell\r\n");
    for (int line = 0; line < 20; ++line)
        feed(t, "shell line " + std::to_string(line) + "\r\n");
    UbiquittyTest::SearchFixture search(t);
    REQUIRE(search.find("needle") == 1);
    const Screen::ScrollSearch::Match* found = Screen::ScrollSearch::activeMatch(t);
    REQUIRE(found != nullptr);
    REQUIRE(found->absoluteRow < Screen::scrollbackCount(&t));

    // A program takes the screen and fills every row, the one the match's
    // number would name among them, before the search runs again.
    enterFullScreenProgram(t);
    Selection::Session session = freshSession();
    REQUIRE(selectRow(session, t, 2, 0, 10) == "first frame");
    CHECK_MSG(!Selection::selectActiveSearchMatch(session.state, t), "a match behind the program is on no row of its screen");
    CHECK_MSG(Selection::selectedText(session, t) == "first frame", "and the view's own selection is left as it was");
}
