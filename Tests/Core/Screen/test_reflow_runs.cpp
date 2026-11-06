// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// A resize moves a run whole or drops it whole, at any width.
//
// The rewrap groups an anchor with its continuation chain and appends the run
// as one span, so no wrap point can land inside it. The two-column pair is the
// only run a producer writes; the chains here are hand-built at pitch five —
// cells given Attribute::Wide and Attribute::WideContinuation directly — so
// the grouping is proven sized by the chain before any wider producer lands.
//
// The narrow case is the policy the drop guard pins: a run wider than the new
// grid has no row it fits on, and it drops whole rather than tearing, because
// a torn chain renders a glyph over columns the grid no longer gives it.

#include "Harness.h"
#include "TerminalFixture.h"

#include "Core/App/WelcomeScreen.h"
#include "Core/CellPen.h"
#include "Core/TerminalOperations.h"

using namespace Ubiquitty;
using UbiquittyTest::cell;

namespace {

enum class RunRole {
    Anchor,
    Continuation,
};

Cell runCell(char32_t codePoint, RunRole role)
{
    Cell cell {};
    cell.pen.foregroundIndex = colorDefault;
    cell.pen.backgroundIndex = colorDefault;
    cell.pen.underlineIndex = colorDefault;
    cell.codePoint = static_cast<std::uint32_t>(codePoint);
    Pen pen = cellPen(cell);
    pen.attributes.add(role == RunRole::Anchor ? VT::Attribute::Wide : VT::Attribute::WideContinuation);
    cellSetPen(cell, pen);
    return cell;
}

// Writes a pitch-`width` run into the live grid at `row`, `column`, plus a
// marker cell after it so content length reaches past the run.
void plantRun(Terminal& t, int row, int column, int width)
{
    t.grid->setCell(Row(row), Column(column), runCell(U'東', RunRole::Anchor));
    for (int i = 1; i < width; ++i)
        t.grid->setCell(Row(row), Column(column + i), runCell(0, RunRole::Continuation));
    Cell marker {};
    marker.pen.foregroundIndex = colorDefault;
    marker.pen.backgroundIndex = colorDefault;
    marker.pen.underlineIndex = colorDefault;
    marker.codePoint = static_cast<std::uint32_t>(U'x');
    t.grid->setCell(Row(row), Column(column + width), marker);
}

// The run covering `row` in the visible grid: anchor column and chain length,
// or {-1, 0} when the row holds none.
struct FoundRun {
    int anchorColumn = -1;
    int chainColumns = 0;
};

FoundRun findRun(const Terminal& t, int row)
{
    FoundRun found;
    for (int c = 0; c < t.columns; ++c) {
        const Cell* here = cell(t, row, c);
        if (!here)
            continue;
        if (cellPen(*here).attributes.contains(VT::Attribute::Wide)) {
            found.anchorColumn = c;
            found.chainColumns = 1;
            while (c + found.chainColumns < t.columns) {
                const Cell* next = cell(t, row, c + found.chainColumns);
                if (!next || !cellPen(*next).attributes.contains(VT::Attribute::WideContinuation))
                    break;
                ++found.chainColumns;
            }
            return found;
        }
    }
    return found;
}

int totalRunCells(const Terminal& t)
{
    int total = 0;
    for (int r = 0; r < t.rows; ++r)
        for (int c = 0; c < t.columns; ++c) {
            const Cell* here = cell(t, r, c);
            if (!here)
                continue;
            const VT::AttributeSet attributes = cellPen(*here).attributes;
            if (attributes.contains(VT::Attribute::Wide) || attributes.contains(VT::Attribute::WideContinuation))
                ++total;
        }
    return total;
}

} // namespace

// The flowed flag follows the cells: after a rewrap every row holding a run
// carries it and no other row does, wherever the rows landed. The flag is a
// row's, and a rewrap moves cells between rows; a run landing on a row whose
// flag was never restated draws as one glyph across its claim.
TEST(aRewrapLeavesTheFlowedFlagOnEveryRowHoldingARunAndNoOther)
{
    Terminal t;
    hardReset(t);
    resize(t, 24, 80);
    t.drawsSizedText = true;
    t.drawsProportionalText = true;
    UbiquittyTest::feed(t, App::Welcome::screenContent(App::Welcome::Pane {}));
    for (const int width : { 64, 100, 72 }) {
        resize(t, 24, width);
        for (int r = 0; r < t.rows; ++r) {
            bool hasRun = false;
            for (int c = 0; c < t.columns && !hasRun; ++c) {
                const Cell* here = cell(t, r, c);
                hasRun = here && isProportionalRunAnchor(&t, *here);
            }
            const bool isFlowed = t.grid->rowMeta(r).has(RowFlag::Flowed);
            CHECK_MSG(isFlowed == hasRun,
                ("width " + std::to_string(width) + " row " + std::to_string(r) + ": flowed " + std::to_string(isFlowed) + " holds a run " + std::to_string(hasRun)).c_str());
        }
    }
}

// A doubled line draws each cell two columns wide, so a run on it that fits
// the grid's column count may still not fit the row: it is clamped to the
// row's own capacity, half the grid, rather than drawn past the right edge.
TEST(aRunOnADoubledRowClampsToHalfTheGrid)
{
    Terminal t;
    hardReset(t);
    resize(t, 4, 80);
    t.drawsSizedText = true;
    t.drawsProportionalText = true;
    UbiquittyTest::feed(t, "\x1b#6\x1b]66;p=1:c=40:h=2;Hello\x1b\\\r\n");
    resize(t, 4, 64);
    const Cell* anchor = cell(t, 0, 0);
    REQUIRE(anchor && isProportionalRunAnchor(&t, *anchor));
    const TextSizeSpec* spec = t.textSizeTable.lookup(cellPen(*anchor).extendedAttributes.textSizeSpecID());
    REQUIRE(spec);
    CHECK_EQ(static_cast<int>(spec->runWidthCells), 32);
    CHECK_EQ(findRun(t, 0).chainColumns, 32);
    CHECK_EQ(static_cast<int>(spec->horizontalAlign), 2);
}

TEST(aNarrowingWrapMovesTheWholeChainToTheNextRow)
{
    // A pitch-five run at columns 10..14 of a 20-column row, with content
    // after it. Narrowing to 12 leaves no room for the run on its row, so
    // the whole chain wraps: five adjacent cells, anchor first, on one row.
    Terminal t;
    hardReset(t);
    resize(t, 4, 20);
    UbiquittyTest::feed(t, "leading-10");
    plantRun(t, 0, 10, 5);

    resize(t, t.rows, 12);

    bool foundWhole = false;
    for (int r = 0; r < t.rows; ++r) {
        const FoundRun run = findRun(t, r);
        if (run.anchorColumn >= 0) {
            CHECK_EQ(run.chainColumns, 5);
            foundWhole = true;
            break;
        }
    }
    CHECK(foundWhole);
    CHECK_EQ(totalRunCells(t), 5);
}

TEST(aRunWiderThanTheNewGridDropsWholeNotTorn)
{
    // Narrowing to four columns leaves no row a pitch-five run fits on: the
    // run drops whole. Zero run cells survive — a lone anchor or a chain
    // fragment would render a glyph the grid cannot back.
    Terminal t;
    hardReset(t);
    resize(t, 4, 20);
    plantRun(t, 0, 0, 5);

    resize(t, t.rows, 4);

    CHECK_EQ(totalRunCells(t), 0);
}

TEST(aWideningPutsTheChainBackOnOneRowIntact)
{
    // Round trip: narrow enough to wrap the run, then widen past the original
    // width. The chain survives both rewraps at its full pitch.
    Terminal t;
    hardReset(t);
    resize(t, 4, 20);
    UbiquittyTest::feed(t, "abcdefgh");
    plantRun(t, 0, 8, 5);

    resize(t, t.rows, 10);
    resize(t, t.rows, 24);

    bool foundWhole = false;
    for (int r = 0; r < t.rows; ++r) {
        const FoundRun run = findRun(t, r);
        if (run.anchorColumn >= 0) {
            CHECK_EQ(run.chainColumns, 5);
            foundWhole = true;
            break;
        }
    }
    CHECK(foundWhole);
    CHECK_EQ(totalRunCells(t), 5);
}

UBIQUITTY_TEST_MAIN()
