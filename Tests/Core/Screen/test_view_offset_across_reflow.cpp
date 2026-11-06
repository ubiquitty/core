// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_view_offset_across_reflow.cpp — a scrolled-back view survives a rewrap.
//
// A reflow renumbers the scrollback: widening merges wrapped rows and the
// count drops, narrowing splits them and it grows. The offset is rows back
// from the live tail, so holding it constant across the count change points
// the reader somewhere else — and feeding the count change to the tail-growth
// compensation drives the offset to zero, which is the reported jump. These
// cases pin the proportional carry: the reader's place as a fraction of the
// history, the floor that keeps a parked reader in history, and the tail that
// stays the tail.

#include "Harness.h"

#include "Core/Screen/Scrollback.h"

using Ubiquitty::Screen::viewOffsetAcrossReflow;

// The reader's place is a fraction of the history, so halving the history
// halves the offset.
TEST(testWideningMergesRowsAndTheOffsetScalesDown)
{
    CHECK(viewOffsetAcrossReflow(1000, 2000, 1000) == 500);
    CHECK(viewOffsetAcrossReflow(600, 2000, 500) == 150);
}

// Narrowing splits rows; the offset grows with the history.
TEST(testNarrowingSplitsRowsAndTheOffsetScalesUp)
{
    CHECK(viewOffsetAcrossReflow(500, 1000, 2000) == 1000);
    CHECK(viewOffsetAcrossReflow(10, 100, 250) == 25);
}

// The tail is not a place in history: an offset of zero stays zero, whatever
// the counts did.
TEST(testTheLiveTailStaysTheLiveTail)
{
    CHECK(viewOffsetAcrossReflow(0, 2000, 100) == 0);
    CHECK(viewOffsetAcrossReflow(0, 100, 2000) == 0);
}

// A parked reader stays in history: a merge large enough to round the offset
// to zero lands on row one instead, because zero means the tail and the
// reader was not there.
TEST(testASmallOffsetSurvivesALargeMerge)
{
    CHECK(viewOffsetAcrossReflow(1, 10000, 10) == 1);
    CHECK(viewOffsetAcrossReflow(3, 9000, 100) == 1);
}

// The carry clamps into the new history rather than pointing past it.
TEST(testTheOffsetClampsToTheNewHistory)
{
    CHECK(viewOffsetAcrossReflow(2000, 2000, 50) == 50);
}

// Degenerate histories answer the tail: there is nothing to be scrolled into.
TEST(testAnEmptyHistoryAnswersTheTail)
{
    CHECK(viewOffsetAcrossReflow(25, 0, 100) == 0);
    CHECK(viewOffsetAcrossReflow(25, 100, 0) == 0);
}

// The product of a deep offset and a deep history overflows int; the carry
// computes through 64 bits and lands exactly.
TEST(testDeepHistoriesDoNotOverflow)
{
    CHECK(viewOffsetAcrossReflow(1000000, 2000000, 1000000) == 500000);
}

UBIQUITTY_TEST_MAIN()
