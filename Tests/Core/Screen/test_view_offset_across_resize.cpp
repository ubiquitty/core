// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_view_offset_across_resize.cpp — where a view lands after a resize.
//
// A height change moves rows across the boundary between the screen and the
// newest end of history and renumbers nothing older, so a scrolled-back view
// moves by exactly the rows that crossed. A taller screen that pulls the
// reader's rows back onto the live screen lands the view on the tail, where the
// newest rows are; a view left one row back would keep them below the edge.
// A rewrap or an evicting push renumbers the history, and the proportional
// carry keeps the reader's place.

#include "Harness.h"

#include "Core/Screen/Scrollback.h"

using Ubiquitty::Screen::HistoryFacts;
using Ubiquitty::Screen::viewOffsetAcrossReflow;
using Ubiquitty::Screen::viewOffsetAcrossResize;

namespace {

HistoryFacts history(int retainedRows, long long appendedRows)
{
    HistoryFacts facts;
    facts.retainedRows = retainedRows;
    facts.appendedRows = appendedRows;
    return facts;
}

} // namespace

// A taller screen that pulls at least the reader's rows back out of history
// shows all of them live, and the view is at the tail.
TEST(testAPullThatReachesTheReaderLandsOnTheTail)
{
    CHECK(viewOffsetAcrossResize(3, history(200, 200), history(188, 200)) == 0);
    CHECK_MSG(viewOffsetAcrossResize(12, history(200, 200), history(188, 200)) == 0,
        "a pull of exactly the offset puts the reader's top row at the screen's top");
}

// A pull shorter than the offset moves the view by the rows that crossed, so
// the same rows stay in view, still in history.
TEST(testAPullShortOfTheReaderHoldsTheirRows)
{
    CHECK(viewOffsetAcrossResize(40, history(200, 200), history(188, 200)) == 28);
}

// Pulls compose: a drag that grows a row at a time lands where one resize by
// the whole height would, and reaches the tail on the step that reaches the
// reader.
TEST(testPullsComposeAcrossADrag)
{
    int offset = 3;
    int retained = 200;
    for (int step = 0; step < 5; ++step) {
        offset = viewOffsetAcrossResize(offset, history(retained, 200), history(retained - 1, 200));
        --retained;
    }
    CHECK(offset == 0);
}

// A shorter screen pushes rows into history and the view moves back by them,
// keeping the reader's rows rather than a proportion of the history.
TEST(testAPushMovesTheViewBackByTheRowsItAppended)
{
    CHECK(viewOffsetAcrossResize(100, history(200, 200), history(210, 210)) == 110);
    CHECK(viewOffsetAcrossReflow(100, 200, 210) == 105);
}

// The live tail stays the live tail, whichever way the screen moved.
TEST(testTheTailStaysTheTail)
{
    CHECK(viewOffsetAcrossResize(0, history(200, 200), history(188, 200)) == 0);
    CHECK(viewOffsetAcrossResize(0, history(200, 200), history(210, 210)) == 0);
    CHECK(viewOffsetAcrossResize(0, history(200, 300), history(120, 420)) == 0);
}

// A rewrap renumbers the history — the counts move by different amounts — and
// the reader's place is carried proportionally, parked readers staying parked.
TEST(testARewrapCarriesTheReadersPlaceProportionally)
{
    CHECK(viewOffsetAcrossResize(100, history(200, 200), history(120, 320)) == viewOffsetAcrossReflow(100, 200, 120));
    CHECK(viewOffsetAcrossResize(3, history(9000, 9000), history(100, 9100)) == 1);
}

// A push into a full history evicts what it appends: the retained count holds
// while rows were appended, which the counts cannot tell from a rewrap, and the
// proportional carry leaves the offset where it was.
TEST(testAnEvictingPushFallsBackToTheProportionalCarry)
{
    CHECK(viewOffsetAcrossResize(100, history(1000, 5000), history(1000, 5010)) == 100);
}

// Nothing crossed: the view does not move.
TEST(testAResizeThatMovedNoHistoryMovesNoView)
{
    CHECK(viewOffsetAcrossResize(42, history(200, 200), history(200, 200)) == 42);
}

// The offset never points past the history it lands in.
TEST(testTheOffsetClampsToTheNewHistory)
{
    CHECK(viewOffsetAcrossResize(300, history(300, 300), history(310, 310)) == 310);
    CHECK(viewOffsetAcrossResize(25, history(100, 100), history(0, 100)) == 0);
}

UBIQUITTY_TEST_MAIN()
