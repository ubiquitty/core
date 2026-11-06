// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_multicell_pitch — the run invariant machinery driven at widths the
// wide-character producer never writes.
//
// The write path's maintenance — the orphan scrub, the range widening, the
// window spill — derives a run's extent from its continuation chain and judges
// it against the anchor's claim. Only the two-column producer exists, so the
// chains here are hand-built: cells given Attribute::Wide and
// Attribute::WideContinuation directly, at pitches three and five, driven
// through the same functions the editing controls call. That proves the
// machinery is sized by the chain rather than by the wide pair, before any
// wider producer lands to depend on it.
//
// What stays untested here is a claim above two. claimedRunColumns answers
// wideRunColumns for the one anchor mark that exists, so the claim-mismatch
// arm above two is unreachable until a producer carries its own claim — the
// case that proves it belongs to that producer's tests.

#include "Harness.h"

#include "Core/CellPen.h"
#include "Core/Screen/WidePair.h"
#include "Core/TextSizeTable.h"

#include <array>

using namespace Ubiquitty;

namespace {

Cell blankCell()
{
    Cell cell {};
    cell.pen.foregroundIndex = colorDefault;
    cell.pen.backgroundIndex = colorDefault;
    cell.pen.underlineIndex = colorDefault;
    return cell;
}

enum class RunRole {
    Anchor,
    Continuation,
};

Cell runCell(char32_t codePoint, RunRole role)
{
    Cell cell = blankCell();
    cell.codePoint = static_cast<std::uint32_t>(codePoint);
    Pen pen = cellPen(cell);
    pen.attributes.add(role == RunRole::Anchor ? VT::Attribute::Wide : VT::Attribute::WideContinuation);
    cellSetPen(cell, pen);
    return cell;
}

bool isBlank(const Cell& cell)
{
    const VT::AttributeSet attributes = cellPen(cell).attributes;
    return !attributes.contains(VT::Attribute::Wide) && !attributes.contains(VT::Attribute::WideContinuation)
        && cell.codePoint == 0;
}

// A row holding one hand-built run of `pitch` columns starting at `at`.
template<std::size_t Columns>
std::array<Cell, Columns> rowWithRun(int at, int pitch)
{
    std::array<Cell, Columns> row;
    row.fill(blankCell());
    row[static_cast<std::size_t>(at)] = runCell(U'東', RunRole::Anchor);
    for (int i = 1; i < pitch; ++i)
        row[static_cast<std::size_t>(at + i)] = runCell(0, RunRole::Continuation);
    return row;
}

} // namespace

TEST(theScrubKeepsTheClaimedPairAndBlanksTheExcessChain)
{
    // A five-cell chain behind a wide-character anchor: the claim is two, so
    // the pair stays and the three excess continuations blank — the scrub is
    // walking the chain's real length, not stepping one fixed column.
    auto row = rowWithRun<10>(2, 5);
    int firstTouched = 10, lastTouched = -1;
    Screen::scrubMulticellOrphans(row.data(), 10, blankCell(), nullptr, Unicode::Width::AmbiguousWidth::Narrow, firstTouched, lastTouched);
    CHECK(cellPen(row[2]).attributes.contains(VT::Attribute::Wide));
    CHECK(cellPen(row[3]).attributes.contains(VT::Attribute::WideContinuation));
    CHECK(isBlank(row[4]));
    CHECK(isBlank(row[5]));
    CHECK(isBlank(row[6]));
    CHECK_EQ(firstTouched, 4);
    CHECK_EQ(lastTouched, 6);
}

TEST(theScrubBlanksAHeadlessChainWhole)
{
    // Three continuations with no anchor behind them — the shape a shift
    // leaves when the anchor scrolls away. Every fragment cell blanks,
    // however long the chain.
    auto row = rowWithRun<10>(2, 5);
    row[2] = blankCell();
    int firstTouched = 10, lastTouched = -1;
    Screen::scrubMulticellOrphans(row.data(), 10, blankCell(), nullptr, Unicode::Width::AmbiguousWidth::Narrow, firstTouched, lastTouched);
    for (int i = 3; i <= 6; ++i)
        CHECK(isBlank(row[static_cast<std::size_t>(i)]));
    CHECK_EQ(firstTouched, 3);
    CHECK_EQ(lastTouched, 6);
}

TEST(theRangeWideningWalksToTheRunsOwnEdges)
{
    // The widening's contract is exercised through a real grid by the erase
    // controls; here the walk itself is pinned at pitch five through the grid
    // the fixture cannot hold, so the assertion names the chain arithmetic.
    // A range touching any chain cell takes the whole run: from mid-chain
    // left to the anchor, to mid-chain right to the last continuation.
    CellGrid grid;
    grid.init(1, 12);
    auto row = rowWithRun<12>(3, 5);
    for (int i = 0; i < 12; ++i)
        grid.setCell(Row(0), Column(i), row[static_cast<std::size_t>(i)]);
    int from = 5, to = 5;
    Screen::expandRangeToWholeRuns(&grid, 0, 12, from, to);
    CHECK_EQ(from, 3);
    CHECK_EQ(to, 7);
}

TEST(theWindowSpillBlanksBothFragmentsOfAnOverlappedRun)
{
    // A pair written into the middle of a seven-column run: the head (anchor
    // plus two continuations) and the tail (two continuations) both stand
    // outside the pair's window and both blank whole. The fragments are sized
    // past one step on each side, so a breaker that walks a fixed distance —
    // the shape being replaced — leaves a far cell standing and fails here.
    CellGrid grid;
    grid.init(1, 14);
    const Cell blank = blankCell();
    auto row = rowWithRun<14>(2, 7);
    for (int i = 0; i < 14; ++i)
        grid.setCell(Row(0), Column(i), row[static_cast<std::size_t>(i)]);

    Cell lead = runCell(U'東', RunRole::Anchor);
    Cell cont = runCell(0, RunRole::Continuation);
    grid.writeWideCell(Row(0), Column(5), lead, cont, blank);

    // The head: anchor at 2, continuations at 3 and 4 — outside the window [5,6].
    CHECK(isBlank(*grid.cellAt(Row(0), Column(2))));
    CHECK(isBlank(*grid.cellAt(Row(0), Column(3))));
    CHECK(isBlank(*grid.cellAt(Row(0), Column(4))));
    // The written pair.
    CHECK(cellPen(*grid.cellAt(Row(0), Column(5))).attributes.contains(VT::Attribute::Wide));
    CHECK(cellPen(*grid.cellAt(Row(0), Column(6))).attributes.contains(VT::Attribute::WideContinuation));
    // The tail: continuations at 7 and 8, chained out of the window, blank.
    CHECK(isBlank(*grid.cellAt(Row(0), Column(7))));
    CHECK(isBlank(*grid.cellAt(Row(0), Column(8))));
}

TEST(theSpillLeavesANonRunNeighbourStanding)
{
    // The window's right neighbour is ordinary text, not a chain cell: the
    // breaker must not blank it. The fixed-offset form blanked the cell one
    // past a lead unconditionally, which took an innocent neighbour with a
    // torn pair; the chain walk blanks only cells that chain out of the
    // window.
    CellGrid grid;
    grid.init(1, 12);
    const Cell blank = blankCell();
    Cell text = blankCell();
    text.codePoint = static_cast<std::uint32_t>(U'x');
    grid.setCell(Row(0), Column(7), text);

    Cell lead = runCell(U'東', RunRole::Anchor);
    Cell cont = runCell(0, RunRole::Continuation);
    grid.writeWideCell(Row(0), Column(5), lead, cont, blank);

    CHECK_EQ(grid.cellAt(Row(0), Column(7))->codePoint, static_cast<std::uint32_t>(U'x'));
}

TEST(aSizedAnchorsClaimComesFromTheTableAndErasesATruncatedChain)
{
    // A spec of scale 2, width 3 claims six columns. The chain holds four —
    // an edit truncated it — and the whole chain erases: judging it at the
    // wide pair's claim would keep two cells and draw a truncated crop.
    TextSizeTable table;
    TextSizeSpec spec;
    spec.scale = 2;
    spec.widthCells = 3;
    const unsigned specID = table.intern(spec);

    auto row = rowWithRun<12>(2, 4);
    for (int i = 2; i < 6; ++i) {
        Pen pen = cellPen(row[static_cast<std::size_t>(i)]);
        pen.extendedAttributes.setTextSizeSpecID(specID);
        cellSetPen(row[static_cast<std::size_t>(i)], pen);
    }
    int firstTouched = 12, lastTouched = -1;
    Screen::scrubMulticellOrphans(row.data(), 12, blankCell(), &table, Unicode::Width::AmbiguousWidth::Narrow, firstTouched, lastTouched);
    for (int i = 2; i < 6; ++i)
        CHECK(isBlank(row[static_cast<std::size_t>(i)]));

    // The intact six-column chain under the same spec stands whole.
    auto whole = rowWithRun<12>(2, 6);
    for (int i = 2; i < 8; ++i) {
        Pen pen = cellPen(whole[static_cast<std::size_t>(i)]);
        pen.extendedAttributes.setTextSizeSpecID(specID);
        cellSetPen(whole[static_cast<std::size_t>(i)], pen);
    }
    firstTouched = 12;
    lastTouched = -1;
    Screen::scrubMulticellOrphans(whole.data(), 12, blankCell(), &table, Unicode::Width::AmbiguousWidth::Narrow, firstTouched, lastTouched);
    CHECK(cellPen(whole[2]).attributes.contains(VT::Attribute::Wide));
    CHECK(cellPen(whole[7]).attributes.contains(VT::Attribute::WideContinuation));
    CHECK_EQ(lastTouched, -1);
}

TEST(overwritingOneCellOfAWideRunBreaksTheWholeRun)
{
    // The per-glyph write path's breaker, reached when a glyph lands on a
    // cell that belongs to a run. A pair loses its one partner; a longer run
    // loses every cell of the fragment on each side, because a chain cut in
    // the middle leaves continuations no anchor's walk reaches — the same
    // orphan the row-span writers already refuse to leave.
    CellGrid grid;
    grid.init(1, 14);
    auto row = rowWithRun<14>(2, 6); // anchor at 2, continuations 3..7
    for (int i = 0; i < 14; ++i)
        grid.setCell(Row(0), Column(i), row[static_cast<std::size_t>(i)]);

    // A narrow glyph lands mid-chain, at column 5.
    grid.clearWideOverlap(Row(0), Column(5), Column(5), blankCell());

    // Everything left of the window back to and including the anchor, and
    // everything right of it that chained on, is gone.
    for (int c = 2; c < 8; ++c) {
        if (c == 5)
            continue;
        const Cell& cell = *grid.cellAt(Row(0), Column(c));
        CHECK(!cellPen(cell).attributes.contains(VT::Attribute::Wide));
        CHECK(!cellPen(cell).attributes.contains(VT::Attribute::WideContinuation));
    }
}

TEST(overwritingACellOutsideAnyRunTouchesNothing)
{
    // The guard: the breaker is called only when the target belongs to a run,
    // and a window over ordinary text leaves its neighbours alone.
    CellGrid grid;
    grid.init(1, 14);
    auto row = rowWithRun<14>(2, 4);
    for (int i = 0; i < 14; ++i)
        grid.setCell(Row(0), Column(i), row[static_cast<std::size_t>(i)]);
    Cell text = blankCell();
    text.codePoint = static_cast<std::uint32_t>(U'x');
    grid.setCell(Row(0), Column(9), text);

    grid.clearWideOverlap(Row(0), Column(9), Column(9), blankCell());

    CHECK_EQ(grid.cellAt(Row(0), Column(9))->codePoint, static_cast<std::uint32_t>(U'x'));
    CHECK(cellPen(*grid.cellAt(Row(0), Column(2))).attributes.contains(VT::Attribute::Wide));
}

UBIQUITTY_TEST_MAIN()
