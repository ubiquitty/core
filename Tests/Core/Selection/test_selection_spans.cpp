// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_selection_spans.cpp — absolute selection rows to viewport rows.
//
// A selection is anchored in absolute buffer rows that survive scrolling; a
// renderer paints viewport rows that do not. Written per frontend, that mapping
// drifts on exactly the cases a person does not reach by hand: a selection
// scrolled partly out of view, one entirely above the viewport, and the
// alternate screen where history is zero rather than the primary's surviving
// count.
//
// The cases here pin the mapping, the clamp at both viewport edges, and what
// Core's void rules actually do — which is EXPAND an empty row to a whole line,
// not drop it. That one is worth a case of its own: the natural assumption runs
// the other way, and a collector written to it would filter rows Core selected
// on purpose.

#include "Harness.h"

#include "Core/Selection/SelectionSpans.h"

using namespace Ubiquitty;
using namespace Ubiquitty::Selection;
using Ubiquitty::Rendering::SelectionRowSpan;

namespace {

// A grid whose every cell holds a letter, so no row is void by the trailing-
// blank rule and each case tests the mapping rather than the content.
uint32_t filledCellAt(void*, int, int)
{
    return u'x';
}

// A grid with no printed cell anywhere, which is what reaches Core's
// empty-row rule: `lastPrintedColumn` answers -1 and the row selects whole.
uint32_t blankCellAt(void*, int, int)
{
    return u' ';
}

uint16_t noAttributesAt(void*, int, int)
{
    return 0;
}

SelectionBounds boundsOver(int firstRow, int lastRow, int firstColumn, int lastColumn)
{
    SelectionBounds bounds {};
    bounds.row0 = firstRow;
    bounds.column0 = firstColumn;
    bounds.row1 = lastRow;
    bounds.column1 = lastColumn;
    return bounds;
}

} // namespace

// The unscrolled case: absolute and visible rows coincide, so a span's row is
// the row it was selected on.
TEST(testAnUnscrolledSelectionMapsRowForRow)
{
    SelectionRowSpan spans[8] {};
    const Viewport viewport { 8, 20, 0 };
    const int count = collectViewportSpans(
        boundsOver(2, 4, 0, 19), SelectionMode::Character, viewport,
        filledCellAt, nullptr, noAttributesAt, nullptr, spans);

    CHECK(count == 3);
    CHECK(spans[0].visibleRow == 2);
    CHECK(spans[1].visibleRow == 3);
    CHECK(spans[2].visibleRow == 4);
}

// Scrolled back: the viewport starts at absolute row 100, so a selection on
// absolute 102 draws on visible row 2. Getting this wrong shifts every
// highlight by the scrollback depth, which is invisible until someone scrolls.
TEST(testAScrolledViewportOffsetsEveryRow)
{
    SelectionRowSpan spans[8] {};
    const Viewport viewport { 8, 20, 100 };
    const int count = collectViewportSpans(
        boundsOver(102, 103, 0, 19), SelectionMode::Character, viewport,
        filledCellAt, nullptr, noAttributesAt, nullptr, spans);

    CHECK(count == 2);
    CHECK(spans[0].visibleRow == 2);
    CHECK(spans[1].visibleRow == 3);
}

// A selection running from above the viewport to below it yields exactly the
// viewport's rows and no more. The walk is over the intersection, so a
// selection spanning a deep scrollback costs the viewport rather than the
// buffer.
TEST(testASelectionLargerThanTheViewportClampsToIt)
{
    SelectionRowSpan spans[8] {};
    const Viewport viewport { 4, 20, 50 };
    const int count = collectViewportSpans(
        boundsOver(0, 10000, 0, 19), SelectionMode::Character, viewport,
        filledCellAt, nullptr, noAttributesAt, nullptr, spans);

    CHECK(count == 4);
    CHECK(spans[0].visibleRow == 0);
    CHECK(spans[3].visibleRow == 3);
}

// Entirely above the viewport: scrolled away, so nothing draws. The empty
// answer is the correct one, not a refusal to compute.
TEST(testASelectionAboveTheViewportEmitsNothing)
{
    SelectionRowSpan spans[8] {};
    const Viewport viewport { 8, 20, 100 };
    const int count = collectViewportSpans(
        boundsOver(10, 20, 0, 19), SelectionMode::Character, viewport,
        filledCellAt, nullptr, noAttributesAt, nullptr, spans);

    CHECK(count == 0);
}

// Entirely below it, which a viewport scrolled up past the selection produces.
TEST(testASelectionBelowTheViewportEmitsNothing)
{
    SelectionRowSpan spans[8] {};
    const Viewport viewport { 8, 20, 0 };
    const int count = collectViewportSpans(
        boundsOver(100, 120, 0, 19), SelectionMode::Character, viewport,
        filledCellAt, nullptr, noAttributesAt, nullptr, spans);

    CHECK(count == 0);
}

// Core's void rules EXPAND rather than drop: an empty row is a bare newline and
// selects as a whole line. Pinned because the natural assumption is the
// opposite — that a blank row contributes nothing — and a collector written to
// that assumption drops every empty line inside a multi-line selection, which
// reads as a rendering gap rather than as a filter.
TEST(testAnEmptyRowSelectsAsAWholeLine)
{
    SelectionRowSpan spans[8] {};
    const Viewport viewport { 8, 20, 0 };
    const int count = collectViewportSpans(
        boundsOver(1, 3, 0, 19), SelectionMode::Character, viewport,
        blankCellAt, nullptr, noAttributesAt, nullptr, spans);

    CHECK(count == 3);
    CHECK(spans[0].visibleRow == 1);
    CHECK(spans[0].column0 == 0);
    CHECK(spans[0].column1 == 19);
}

// A caller whose buffer is shorter than the selection gets what fits and a
// count saying so, rather than a write past the end.
TEST(testAShortBufferStopsAtItsCapacity)
{
    SelectionRowSpan spans[2] {};
    const Viewport viewport { 8, 20, 0 };
    const int count = collectViewportSpans(
        boundsOver(0, 7, 0, 19), SelectionMode::Character, viewport,
        filledCellAt, nullptr, noAttributesAt, nullptr, ViewportSpans(spans, 2));

    CHECK(count == 2);
    CHECK(spans[0].visibleRow == 0);
    CHECK(spans[1].visibleRow == 1);
}

// A degenerate grid answers empty rather than dividing by it.
TEST(testAnEmptyViewportEmitsNothing)
{
    SelectionRowSpan spans[8] {};
    const int count = collectViewportSpans(
        boundsOver(0, 4, 0, 19), SelectionMode::Character, Viewport { 0, 20, 0 },
        filledCellAt, nullptr, noAttributesAt, nullptr, spans);

    CHECK(count == 0);
}

UBIQUITTY_TEST_MAIN()

namespace {

// Rows 2 and 3 are a double-height pair; every other row is single-height.
uint8_t pairAtRowsTwoAndThree(void*, int row)
{
    if (row == 2)
        return lineAttributeDoubleHeightTop;
    if (row == 3)
        return lineAttributeDoubleHeightBottom;
    return lineAttributeNormal;
}

} // namespace

// Both halves of a double-height pair show one line of glyphs, so a span on
// either half is the pair's: the collector lights the same columns on the
// other half, keeps the top half's columns where the bounds run through the
// bottom half as a middle row, and adds nothing where no reader of line
// attributes is given.
TEST(testASpanOnEitherHalfOfADoubleHeightPairCoversBothHalves)
{
    const Viewport viewport { 8, 20, 0 };

    // Selected on the bottom half alone: the top half takes the same columns.
    SelectionRowSpan fromBottom[8] {};
    int count = collectViewportSpans(
        boundsOver(3, 3, 2, 5), SelectionMode::Character, viewport,
        filledCellAt, nullptr, noAttributesAt, nullptr, fromBottom, pairAtRowsTwoAndThree, nullptr);
    CHECK(count == 2);
    CHECK(fromBottom[0].visibleRow == 2 && fromBottom[0].column0 == 2 && fromBottom[0].column1 == 5);
    CHECK(fromBottom[1].visibleRow == 3 && fromBottom[1].column0 == 2 && fromBottom[1].column1 == 5);

    // Selected from the top half's column 6 down onto the plain row below:
    // the bottom half, a middle row, takes the top's columns rather than the
    // whole row the bounds give it.
    SelectionRowSpan through[8] {};
    count = collectViewportSpans(
        boundsOver(2, 4, 6, 3), SelectionMode::Character, viewport,
        filledCellAt, nullptr, noAttributesAt, nullptr, through, pairAtRowsTwoAndThree, nullptr);
    CHECK(count == 3);
    CHECK(through[0].visibleRow == 2 && through[0].column0 == 6 && through[0].column1 == 19);
    CHECK(through[1].visibleRow == 3 && through[1].column0 == 6 && through[1].column1 == 19);
    CHECK(through[2].visibleRow == 4 && through[2].column0 == 0 && through[2].column1 == 3);

    // Without a reader of line attributes the rows stand alone.
    SelectionRowSpan alone[8] {};
    count = collectViewportSpans(
        boundsOver(3, 3, 2, 5), SelectionMode::Character, viewport,
        filledCellAt, nullptr, noAttributesAt, nullptr, alone);
    CHECK(count == 1);
    CHECK(alone[0].visibleRow == 3);

    // A pair whose top half is above the viewport lights only what is shown.
    SelectionRowSpan scrolled[8] {};
    const Viewport fromRowThree { 8, 20, 3 };
    count = collectViewportSpans(
        boundsOver(3, 3, 2, 5), SelectionMode::Character, fromRowThree,
        filledCellAt, nullptr, noAttributesAt, nullptr, scrolled, pairAtRowsTwoAndThree, nullptr);
    CHECK(count == 1);
    CHECK(scrolled[0].visibleRow == 0 && scrolled[0].column0 == 2 && scrolled[0].column1 == 5);
}

// A character selection's end standing inside a proportional run's cell is a
// cut into that cell's letters, and the span carries it on the bounds' own
// end cells so the fill lights the letters the copy takes. A middle row's
// cells are whole, as is every cell of a selection that is not by character.
TEST(testASpanCarriesTheCutsInsideItsEndCells)
{
    SelectionRowSpan spans[8] {};
    const Viewport viewport { 8, 20, 0 };
    SelectionBounds bounds = boundsOver(2, 4, 3, 7);
    bounds.subColumn0 = 1;
    bounds.subColumn1 = 2;
    int count = collectViewportSpans(bounds, SelectionMode::Character, viewport, filledCellAt, nullptr, noAttributesAt, nullptr, spans);
    CHECK(count == 3);
    CHECK(spans[0].subColumn0 == 1 && spans[0].subColumn1 == 0);
    CHECK(spans[1].subColumn0 == 0 && spans[1].subColumn1 == 0);
    CHECK(spans[2].subColumn0 == 0 && spans[2].subColumn1 == 2);

    count = collectViewportSpans(bounds, SelectionMode::Word, viewport, filledCellAt, nullptr, noAttributesAt, nullptr, spans);
    CHECK(count == 3);
    CHECK(spans[0].subColumn0 == 0 && spans[2].subColumn1 == 0);
}
