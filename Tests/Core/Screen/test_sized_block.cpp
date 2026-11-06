// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_sized_block — the multi-row block writer and the band scrub, driven
// directly the way test_multicell_pitch drives the horizontal machinery: no
// producer exists, so the blocks here are written by the primitive under
// test and damaged by hand.
//
// The property the scrub holds is kitty's own rule for a split block: erase,
// never reinterpret. A chain claiming band k is backed only by a chain
// claiming band k-1 with the same spec id and span directly above; a chain
// nothing backs blanks whole, and a top-down sweep is the cascade — blanking
// row k's fragment is what unbacks row k+1's, and the sweep reaches it next.

#include "Harness.h"
#include "TerminalFixture.h"

#include "Core/Accessibility/TextProjection.h"
#include "Core/CellPen.h"
#include "Core/Screen/Addressing.h"
#include "Core/Screen/Scrollback.h"
#include "Core/Screen/SizedBlock.h"
#include "Core/TerminalOperations.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

Pen plainPen()
{
    Pen pen {};
    pen.foregroundIndex = colorDefault;
    pen.backgroundIndex = colorDefault;
    pen.underlineIndex = colorDefault;
    return pen;
}

struct BlockGrid {
    CellGrid grid;
    BlockGrid(int rows, int columns) { grid.init(rows, columns); }

    const Cell& at(int row, int column) { return *grid.cellAt(Row(row), Column(column)); }
};

} // namespace

TEST(theWriterStampsEveryRowWithItsBandAndTheSharedID)
{
    BlockGrid fx(6, 16);
    Screen::writeSizedBlock(fx.grid, 1, 3, /*columns=*/6, /*rows=*/3, U'東', plainPen(), 5, blankCell());

    for (int band = 0; band < 3; ++band) {
        const Cell& anchor = fx.at(1 + band, 3);
        CHECK(cellPen(anchor).attributes.contains(VT::Attribute::Wide));
        CHECK_EQ(anchor.codePoint, static_cast<std::uint32_t>(U'東'));
        CHECK_EQ(static_cast<int>(cellPen(anchor).extendedAttributes.textSizeSpecID()), 5);
        CHECK_EQ(static_cast<int>(penTextBand(&cellPen(anchor))), band);
        for (int i = 1; i < 6; ++i) {
            const Cell& continuation = fx.at(1 + band, 3 + i);
            CHECK(cellPen(continuation).attributes.contains(VT::Attribute::WideContinuation));
            CHECK_EQ(static_cast<int>(cellPen(continuation).extendedAttributes.textSizeSpecID()), 5);
            CHECK_EQ(static_cast<int>(penTextBand(&cellPen(continuation))), band);
        }
    }
}

TEST(aBlockThatDoesNotFitWritesNothing)
{
    BlockGrid fx(4, 10);
    // Too tall and too wide, each alone: no row of either block is written.
    Screen::writeSizedBlock(fx.grid, 2, 0, 4, 3, U'東', plainPen(), 1, blankCell());
    Screen::writeSizedBlock(fx.grid, 0, 7, 4, 2, U'東', plainPen(), 1, blankCell());
    for (int row = 0; row < 4; ++row)
        for (int column = 0; column < 10; ++column)
            CHECK(!cellPen(fx.at(row, column)).attributes.contains(VT::Attribute::Wide));
}

TEST(theWriterBreaksAnOverlappedNeighbourWholeOnEachRow)
{
    BlockGrid fx(4, 16);
    // A pitch-4 run at columns 1..4 of row 1; the block's window [3..8]
    // overlaps its tail. The run's head outside the window blanks whole.
    std::array<Cell, 4> neighbour;
    neighbour.fill(blankCell());
    Pen pen = plainPen();
    pen.attributes.add(VT::Attribute::Wide);
    cellSetPen(neighbour[0], pen);
    neighbour[0].codePoint = static_cast<std::uint32_t>(U'東');
    Pen continuationPen = plainPen();
    continuationPen.attributes.add(VT::Attribute::WideContinuation);
    for (std::size_t i = 1; i < 4; ++i)
        cellSetPen(neighbour[i], continuationPen);
    for (int i = 0; i < 4; ++i)
        fx.grid.setCell(Row(1), Column(1 + i), neighbour[static_cast<std::size_t>(i)]);

    Screen::writeSizedBlock(fx.grid, 0, 3, 6, 2, U'東', plainPen(), 2, blankCell());

    CHECK(!cellPen(fx.at(1, 1)).attributes.contains(VT::Attribute::Wide));
    CHECK(!cellPen(fx.at(1, 2)).attributes.contains(VT::Attribute::WideContinuation));
    CHECK(cellPen(fx.at(1, 3)).attributes.contains(VT::Attribute::Wide));
}

TEST(theScrubErasesAChainWhoseBackingRowIsGone)
{
    // A 3-row block, then its top row hand-blanked — the middle row's chain
    // claims band 1 with nothing above it, and the sweep erases it; the
    // erased middle then unbacks the bottom row, which the sweep reaches
    // next. The block ends as absence, kitty's rule, never a wrong slice.
    std::array<std::array<Cell, 12>, 3> rows;
    for (auto& row : rows)
        row.fill(blankCell());
    CellGrid grid;
    grid.init(3, 12);
    Screen::writeSizedBlock(grid, 0, 2, 6, 3, U'東', plainPen(), 3, blankCell());
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 12; ++c)
            rows[static_cast<std::size_t>(r)][static_cast<std::size_t>(c)] = *grid.cellAt(Row(r), Column(c));

    // The damage: the top row erased whole, as ED above the block would.
    rows[0].fill(blankCell());

    for (int r = 0; r < 3; ++r) {
        int firstTouched = 12, lastTouched = -1;
        Screen::scrubUnbackedBands(r > 0 ? rows[static_cast<std::size_t>(r - 1)].data() : nullptr,
            rows[static_cast<std::size_t>(r)].data(), 12, blankCell(), firstTouched, lastTouched);
    }
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 12; ++c)
            CHECK(!cellPen(rows[static_cast<std::size_t>(r)][static_cast<std::size_t>(c)]).attributes.contains(VT::Attribute::Wide));
}

TEST(theScrubKeepsAnIntactBlockAndAnAdjacentStack)
{
    // Two 2-row blocks stacked touching, same spec: bands run 0,1,0,1. The
    // band sequence is what distinguishes the stack from one 4-row block,
    // and the scrub keeps all four rows — band 0 needs no backing, band 1
    // is backed in both blocks.
    std::array<std::array<Cell, 12>, 4> rows;
    for (auto& row : rows)
        row.fill(blankCell());
    CellGrid grid;
    grid.init(4, 12);
    Screen::writeSizedBlock(grid, 0, 2, 4, 2, U'東', plainPen(), 3, blankCell());
    Screen::writeSizedBlock(grid, 2, 2, 4, 2, U'東', plainPen(), 3, blankCell());
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 12; ++c)
            rows[static_cast<std::size_t>(r)][static_cast<std::size_t>(c)] = *grid.cellAt(Row(r), Column(c));

    for (int r = 0; r < 4; ++r) {
        int firstTouched = 12, lastTouched = -1;
        Screen::scrubUnbackedBands(r > 0 ? rows[static_cast<std::size_t>(r - 1)].data() : nullptr,
            rows[static_cast<std::size_t>(r)].data(), 12, blankCell(), firstTouched, lastTouched);
    }
    for (int r = 0; r < 4; ++r)
        CHECK(cellPen(rows[static_cast<std::size_t>(r)][2]).attributes.contains(VT::Attribute::Wide));
}

TEST(theScrubErasesAShiftedChainNotAWrongCrop)
{
    // The middle row of a 3-row block shifted one column right, as ICH at
    // that row would: its span no longer matches its backing, so it blanks
    // — and the bottom row follows in the sweep, unbacked by the blank.
    std::array<std::array<Cell, 12>, 3> rows;
    for (auto& row : rows)
        row.fill(blankCell());
    CellGrid grid;
    grid.init(3, 12);
    Screen::writeSizedBlock(grid, 0, 2, 4, 3, U'東', plainPen(), 3, blankCell());
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 12; ++c)
            rows[static_cast<std::size_t>(r)][static_cast<std::size_t>(c)] = *grid.cellAt(Row(r), Column(c));

    auto& middle = rows[1];
    for (int c = 11; c >= 3; --c)
        middle[static_cast<std::size_t>(c)] = middle[static_cast<std::size_t>(c - 1)];
    middle[2] = blankCell();

    for (int r = 0; r < 3; ++r) {
        int firstTouched = 12, lastTouched = -1;
        Screen::scrubUnbackedBands(r > 0 ? rows[static_cast<std::size_t>(r - 1)].data() : nullptr,
            rows[static_cast<std::size_t>(r)].data(), 12, blankCell(), firstTouched, lastTouched);
    }
    // The top row stands (band 0); the shifted middle and the bottom erased.
    CHECK(cellPen(rows[0][2]).attributes.contains(VT::Attribute::Wide));
    for (int c = 0; c < 12; ++c) {
        CHECK(!cellPen(rows[1][static_cast<std::size_t>(c)]).attributes.contains(VT::Attribute::Wide));
        CHECK(!cellPen(rows[2][static_cast<std::size_t>(c)]).attributes.contains(VT::Attribute::Wide));
    }
}

TEST(theScrubReadsTheBandSequenceNotMerePresence)
{
    // A band-2 chain whose upstairs neighbour is a band-0 top row of the
    // same spec and span: present, same id, same columns — and still not a
    // backing, because the sequence skips band 1. Presence alone would keep
    // the chain and draw the wrong slice; the order check erases it. Beside
    // it, the same shape with a DIFFERENT id above: erased for the id.
    std::array<std::array<Cell, 14>, 2> rows;
    for (auto& row : rows)
        row.fill(blankCell());
    CellGrid grid;
    grid.init(2, 14);
    // Left: top row band 0 id 3; bottom chain hand-stamped band 2 id 3.
    Screen::writeSizedBlock(grid, 0, 1, 4, 2, U'東', plainPen(), 3, blankCell());
    // Right: top row band 0 id 4; bottom chain band 1 id 3.
    Screen::writeSizedBlock(grid, 0, 8, 4, 2, U'東', plainPen(), 4, blankCell());
    for (int r = 0; r < 2; ++r)
        for (int c = 0; c < 14; ++c)
            rows[static_cast<std::size_t>(r)][static_cast<std::size_t>(c)] = *grid.cellAt(Row(r), Column(c));
    for (int c = 1; c < 5; ++c) {
        Pen pen = cellPen(rows[1][static_cast<std::size_t>(c)]);
        penSetTextBand(&pen, 2);
        cellSetPen(rows[1][static_cast<std::size_t>(c)], pen);
    }
    for (int c = 8; c < 12; ++c) {
        Pen pen = cellPen(rows[1][static_cast<std::size_t>(c)]);
        pen.extendedAttributes.setTextSizeSpecID(3);
        cellSetPen(rows[1][static_cast<std::size_t>(c)], pen);
    }

    int firstTouched = 14, lastTouched = -1;
    Screen::scrubUnbackedBands(rows[0].data(), rows[1].data(), 14, blankCell(), firstTouched, lastTouched);
    for (int c = 1; c < 5; ++c)
        CHECK(!cellPen(rows[1][static_cast<std::size_t>(c)]).attributes.contains(VT::Attribute::Wide));
    for (int c = 8; c < 12; ++c)
        CHECK(!cellPen(rows[1][static_cast<std::size_t>(c)]).attributes.contains(VT::Attribute::Wide));
}

TEST(theCompletenessScrubErasesABottomTruncatedBlockWhole)
{
    // A 3-row block with its bottom row erased: every remaining band is
    // backed from above — the upward scrub keeps them — and the block still
    // must go whole, because a top fragment draws half a glyph. The
    // bottom-up sweep is the cascade: the middle fails first (band 1 of a
    // scale-3 spec with nothing below), and its blanking cuts the top short.
    TextSizeTable table;
    TextSizeSpec spec;
    spec.scale = 3;
    spec.widthCells = 2;
    const unsigned specID = table.intern(spec);

    std::array<std::array<Cell, 12>, 3> rows;
    for (auto& row : rows)
        row.fill(blankCell());
    CellGrid grid;
    grid.init(3, 12);
    Screen::writeSizedBlock(grid, 0, 2, 6, 3, U'東', plainPen(), specID, blankCell());
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 12; ++c)
            rows[static_cast<std::size_t>(r)][static_cast<std::size_t>(c)] = *grid.cellAt(Row(r), Column(c));
    rows[2].fill(blankCell());

    // The upward pass alone keeps rows 0 and 1 — the fragment the
    // completeness pass exists to catch.
    for (int r = 0; r < 3; ++r) {
        int firstTouched = 12, lastTouched = -1;
        Screen::scrubUnbackedBands(r > 0 ? rows[static_cast<std::size_t>(r - 1)].data() : nullptr,
            rows[static_cast<std::size_t>(r)].data(), 12, blankCell(), firstTouched, lastTouched);
    }
    CHECK(cellPen(rows[0][2]).attributes.contains(VT::Attribute::Wide));
    CHECK(cellPen(rows[1][2]).attributes.contains(VT::Attribute::Wide));

    for (int r = 2; r >= 0; --r) {
        int firstTouched = 12, lastTouched = -1;
        Screen::scrubShortenedBlocks(rows[static_cast<std::size_t>(r)].data(),
            r + 1 < 3 ? rows[static_cast<std::size_t>(r + 1)].data() : nullptr, 12, &table,
            blankCell(), firstTouched, lastTouched);
    }
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 12; ++c)
            CHECK(!cellPen(rows[static_cast<std::size_t>(r)][static_cast<std::size_t>(c)]).attributes.contains(VT::Attribute::Wide));
}

TEST(theCompletenessScrubKeepsWholeBlocksAndUnresolvableIDs)
{
    // An intact scale-2 block passes both directions untouched, and a chain
    // whose id resolves to no spec is kept — a stale id renders unsized, and
    // erasing it would turn that soft degradation into data loss.
    TextSizeTable table;
    TextSizeSpec spec;
    spec.scale = 2;
    spec.widthCells = 2;
    const unsigned specID = table.intern(spec);

    std::array<std::array<Cell, 12>, 2> rows;
    for (auto& row : rows)
        row.fill(blankCell());
    CellGrid grid;
    grid.init(2, 12);
    Screen::writeSizedBlock(grid, 0, 1, 4, 2, U'東', plainPen(), specID, blankCell());
    for (int r = 0; r < 2; ++r)
        for (int c = 0; c < 12; ++c)
            rows[static_cast<std::size_t>(r)][static_cast<std::size_t>(c)] = *grid.cellAt(Row(r), Column(c));
    // A second chain stamped with a never-minted id.
    for (int c = 7; c < 11; ++c) {
        Cell cell = blankCell();
        Pen pen = cellPen(cell);
        pen.attributes.add(c == 7 ? VT::Attribute::Wide : VT::Attribute::WideContinuation);
        pen.extendedAttributes.setTextSizeSpecID(9);
        cellSetPen(cell, pen);
        rows[0][static_cast<std::size_t>(c)] = cell;
    }

    for (int r = 1; r >= 0; --r) {
        int firstTouched = 12, lastTouched = -1;
        Screen::scrubShortenedBlocks(rows[static_cast<std::size_t>(r)].data(),
            r + 1 < 2 ? rows[static_cast<std::size_t>(r + 1)].data() : nullptr, 12, &table,
            blankCell(), firstTouched, lastTouched);
    }
    CHECK(cellPen(rows[0][1]).attributes.contains(VT::Attribute::Wide));
    CHECK(cellPen(rows[1][1]).attributes.contains(VT::Attribute::Wide));
    CHECK(cellPen(rows[0][7]).attributes.contains(VT::Attribute::Wide));
}

TEST(theCompletenessScrubDemandsTheNextBandNotAnyLaterOne)
{
    // A scale-3 block whose middle row was deleted, the rows below shifted
    // up: band 0 sits directly over band 2. The below-chain is present, same
    // spec, same span — and still not the continuation, because the sequence
    // skips band 1. Accepting any later band would keep band 0 and draw
    // slices 0 and 2 of a three-slice glyph.
    TextSizeTable table;
    TextSizeSpec spec;
    spec.scale = 3;
    spec.widthCells = 2;
    const unsigned specID = table.intern(spec);

    std::array<std::array<Cell, 12>, 2> rows;
    for (auto& row : rows)
        row.fill(blankCell());
    CellGrid grid;
    grid.init(3, 12);
    Screen::writeSizedBlock(grid, 0, 2, 6, 3, U'東', plainPen(), specID, blankCell());
    for (int c = 0; c < 12; ++c) {
        rows[0][static_cast<std::size_t>(c)] = *grid.cellAt(Row(0), Column(c));
        rows[1][static_cast<std::size_t>(c)] = *grid.cellAt(Row(2), Column(c));
    }

    int firstTouched = 12, lastTouched = -1;
    Screen::scrubShortenedBlocks(rows[0].data(), rows[1].data(), 12, &table,
        blankCell(), firstTouched, lastTouched);
    for (int c = 2; c < 8; ++c)
        CHECK(!cellPen(rows[0][static_cast<std::size_t>(c)]).attributes.contains(VT::Attribute::Wide));
}

namespace {

// A terminal with one interned spec, and the id it minted.
struct SizedTerminal {
    Terminal t;
    unsigned specID = 0;
    TextSizeSpec spec;

    SizedTerminal(int rows, int columns, std::uint8_t scale, std::uint8_t width)
    {
        hardReset(t);
        resize(t, rows, columns);
        spec.scale = scale;
        spec.widthCells = width;
        specID = t.textSizeTable.intern(spec);
    }
};

} // namespace

TEST(placementWritesTheBlockAndAdvancesPastIt)
{
    SizedTerminal fx(8, 20, /*scale=*/2, /*width=*/2);
    fx.t.screens.active->cursorX = 3;
    fx.t.screens.active->cursorY = 1;
    CHECK(Screen::placeSizedGlyph(fx.t, U'東', fx.spec, fx.specID));
    // Four columns (scale 2 × width 2) by two rows, from the cursor.
    for (int band = 0; band < 2; ++band) {
        const Cell* anchor = UbiquittyTest::cell(fx.t, 1 + band, 3);
        CHECK(anchor && cellPen(*anchor).attributes.contains(VT::Attribute::Wide));
        CHECK_EQ(static_cast<int>(penTextBand(&cellPen(*anchor))), band);
    }
    CHECK_EQ(fx.t.screens.active->cursorX, 7);
    CHECK_EQ(fx.t.screens.active->cursorY, 1);
}

TEST(aBlockThatWouldCrossTheRightEdgeWrapsWhole)
{
    SizedTerminal fx(8, 20, 2, 2);
    fx.t.screens.active->cursorX = 18; // two columns left, the block needs four
    fx.t.screens.active->cursorY = 0;
    CHECK(Screen::placeSizedGlyph(fx.t, U'東', fx.spec, fx.specID));
    // The whole block moved down rather than splitting across the margin.
    const Cell* anchor = UbiquittyTest::cell(fx.t, 1, 0);
    CHECK(anchor && cellPen(*anchor).attributes.contains(VT::Attribute::Wide));
    CHECK(!cellPen(*UbiquittyTest::cell(fx.t, 0, 18)).attributes.contains(VT::Attribute::Wide));
    CHECK_EQ(fx.t.screens.active->cursorY, 1);
    CHECK_EQ(fx.t.screens.active->cursorX, 4);
}

TEST(aBlockWiderThanTheWritableBandIsRefusedWhole)
{
    // Scale 7 × width 2 is fourteen columns; the screen has ten. Nothing is
    // written and the cursor does not move — the caller emits it unsized.
    SizedTerminal fx(8, 10, 7, 2);
    fx.t.screens.active->cursorX = 0;
    fx.t.screens.active->cursorY = 0;
    CHECK(!Screen::placeSizedGlyph(fx.t, U'東', fx.spec, fx.specID));
    CHECK_EQ(fx.t.screens.active->cursorX, 0);
    for (int c = 0; c < 10; ++c)
        CHECK(!cellPen(*UbiquittyTest::cell(fx.t, 0, c)).attributes.contains(VT::Attribute::Wide));
}

TEST(aBlockThatWouldStraddleTheScrollRegionIsRefused)
{
    // DECSTBM 1..4 (0-based 0..3) with the cursor on row 3: a three-row block
    // would reach row 5, outside the region — and the region's next scroll
    // would cut it, which the scrubs can only answer by erasing. Refused at
    // the source instead.
    SizedTerminal fx(8, 20, 3, 2);
    UbiquittyTest::feed(fx.t, "\x1b[1;4r");
    fx.t.screens.active->cursorX = 0;
    fx.t.screens.active->cursorY = 3;
    CHECK(!Screen::placeSizedGlyph(fx.t, U'東', fx.spec, fx.specID));
    CHECK_EQ(fx.t.screens.active->cursorY, 3);
    for (int r = 3; r < 8; ++r)
        CHECK(!cellPen(*UbiquittyTest::cell(fx.t, r, 0)).attributes.contains(VT::Attribute::Wide));
}

TEST(theSpecsOwnWidthOverridesTheGlyphsAndZeroTakesTheGlyphs)
{
    // width=1 on a double-width glyph: the spec's word is what the block
    // takes, so scale 2 × width 1 is two columns, not four.
    SizedTerminal narrow(4, 20, 2, 1);
    narrow.t.screens.active->cursorX = 0;
    CHECK(Screen::placeSizedGlyph(narrow.t, U'東', narrow.spec, narrow.specID));
    CHECK_EQ(narrow.t.screens.active->cursorX, 2);

    // width=0 leaves the terminal to compute it: the CJK glyph is two wide,
    // so scale 2 gives four columns.
    SizedTerminal computed(4, 20, 2, 0);
    computed.t.screens.active->cursorX = 0;
    CHECK(Screen::placeSizedGlyph(computed.t, U'東', computed.spec, computed.specID));
    CHECK_EQ(computed.t.screens.active->cursorX, 4);
}

TEST(aClientThatDrawsNoBandsFlattensWhatItReceives)
{
    // The host sizes; the client's renderer crops no bands. Left as they
    // arrive, the block's cells would draw the whole glyph on every covered
    // row — a column of stacked duplicates. Flattened, the client shows the
    // run as it would have printed it.
    SizedTerminal host(6, 20, /*scale=*/3, /*width=*/2);
    host.t.screens.active->cursorX = 2;
    host.t.screens.active->cursorY = 1;
    CHECK(Screen::placeSizedGlyph(host.t, U'東', host.spec, host.specID));

    Screen::flattenSizedCells(host.t);

    // One plain glyph where the anchor sat, nothing else of the block.
    const Cell* kept = UbiquittyTest::cell(host.t, 1, 2);
    CHECK(kept && kept->codePoint == static_cast<std::uint32_t>(U'東'));
    CHECK(!cellPen(*kept).attributes.contains(VT::Attribute::Wide));
    CHECK_EQ(static_cast<int>(cellPen(*kept).extendedAttributes.textSizeSpecID()), 0);
    for (int row = 1; row <= 3; ++row)
        for (int column = 2; column < 8; ++column) {
            if (row == 1 && column == 2)
                continue;
            const Cell* c = UbiquittyTest::cell(host.t, row, column);
            CHECK(c && c->codePoint == 0);
            CHECK_EQ(static_cast<int>(cellPen(*c).extendedAttributes.textSizeSpecID()), 0);
        }
    // The ids nothing carries are free by the table's own rule.
    CHECK_EQ(host.t.textSizeTable.liveCount(), 0);
}

TEST(flatteningLeavesAGridWithoutSizedCellsAlone)
{
    // The guard: every terminal that never meets a sizing peer pays one
    // comparison, and a plain grid is not walked or rewritten.
    Terminal t;
    hardReset(t);
    resize(t, 4, 10);
    UbiquittyTest::feed(t, "plain text");
    Screen::flattenSizedCells(t);
    const Cell* first = UbiquittyTest::cell(t, 0, 0);
    CHECK(first && first->codePoint == 'p');
    CHECK_EQ(t.textSizeTable.liveCount(), 0);
}

TEST(flatteningReachesTheScreenTheTerminalIsNotShowing)
{
    // A block on the primary screen, the terminal then on the alternate: the
    // table empties either way, so an id left behind on the screen nobody is
    // showing resolves to nothing the moment the alternate exit brings those
    // cells back — a block with no specification, and no pass that reaches it
    // again.
    SizedTerminal fx(6, 20, /*scale=*/2, /*width=*/2);
    fx.t.screens.active->cursorX = 1;
    fx.t.screens.active->cursorY = 0;
    CHECK(Screen::placeSizedGlyph(fx.t, U'東', fx.spec, fx.specID));
    CHECK(fx.t.grid->cellAt(ScreenMode::Primary, Row(0), Column(1)) != nullptr);

    // Switch to the alternate screen; the primary's cells freeze as they are.
    fx.t.grid->setAlternateActive(ScreenMode::Alternate);
    fx.t.screens.activate(Modes::ScreenBuffer::Alternate);

    Screen::flattenSizedCells(fx.t);

    // The frozen primary carries no sizing either.
    for (int row = 0; row < fx.t.rows; ++row)
        for (int column = 0; column < fx.t.columns; ++column) {
            const Cell* cell = fx.t.grid->cellAt(ScreenMode::Primary, Row(row), Column(column));
            CHECK(cell && cellPen(*cell).extendedAttributes.textSizeSpecID() == 0);
        }
    // And the glyph survives as ordinary text where its anchor sat.
    const Cell* kept = fx.t.grid->cellAt(ScreenMode::Primary, Row(0), Column(1));
    CHECK(kept && kept->codePoint == static_cast<std::uint32_t>(U'東'));
    CHECK_EQ(fx.t.textSizeTable.liveCount(), 0);
}

namespace {

int sizedCellCount(const Terminal& t, ScreenMode mode)
{
    int count = 0;
    for (int row = 0; row < t.rows; ++row)
        for (int column = 0; column < t.columns; ++column) {
            const Cell* cell = t.grid->cellAt(mode, Row(row), Column(column));
            if (cell && cellPen(*cell).extendedAttributes.textSizeSpecID() != 0)
                ++count;
        }
    return count;
}

} // namespace

TEST(aShrinkThatAgesTheBlocksTopRowsErasesWhatStaysBehind)
{
    // A height shrink scrolls the top rows into history and shifts the
    // survivors up. A block straddling that boundary loses its first bands to
    // the scrollback and keeps the rest — bands claiming a backing that is no
    // longer on the screen, which would draw the glyph's lower slice at the
    // top of the terminal.
    SizedTerminal fx(6, 20, /*scale=*/3, /*width=*/2);
    fx.t.screens.active->cursorX = 0;
    fx.t.screens.active->cursorY = 0;
    CHECK(Screen::placeSizedGlyph(fx.t, U'東', fx.spec, fx.specID));
    CHECK_EQ(sizedCellCount(fx.t, ScreenMode::Primary), 18);
    // Content below the block, so the shrink has rows to push.
    processBytes(&fx.t, "\x1b[6;1Hbottom");

    resize(fx.t, 4, 20);

    // Whatever the shrink kept of the block, none of it still claims a size.
    CHECK_EQ(sizedCellCount(fx.t, ScreenMode::Primary), 0);
}

TEST(aResizeThatMovesABlockWholeKeepsIt)
{
    // The control the erasure needs: a resize that leaves every row of the
    // block on the screen together must not touch it, or the scrub would be
    // erasing blocks for the mere fact of a resize.
    SizedTerminal fx(6, 20, /*scale=*/3, /*width=*/2);
    fx.t.screens.active->cursorX = 0;
    fx.t.screens.active->cursorY = 0;
    CHECK(Screen::placeSizedGlyph(fx.t, U'東', fx.spec, fx.specID));

    resize(fx.t, 8, 20);

    CHECK_EQ(sizedCellCount(fx.t, ScreenMode::Primary), 18);
    for (int band = 0; band < 3; ++band) {
        const Cell* anchor = fx.t.grid->cellAt(ScreenMode::Primary, Row(band), Column(0));
        CHECK(anchor && cellPen(*anchor).attributes.contains(VT::Attribute::Wide));
        CHECK_EQ(static_cast<int>(penTextBand(&cellPen(*anchor))), band);
    }
}

TEST(theResizeJudgesTheBlockOnTheScreenNobodyIsShowing)
{
    // The alternate is shifted and resized while the primary is on display,
    // and the switch back passes through no control that would judge it. A
    // block cut short by that resize has to be erased here or never.
    SizedTerminal fx(6, 20, /*scale=*/3, /*width=*/2);
    fx.t.grid->setAlternateActive(ScreenMode::Alternate);
    fx.t.screens.activate(Modes::ScreenBuffer::Alternate);
    fx.t.screens.active->cursorX = 0;
    fx.t.screens.active->cursorY = 3;
    CHECK(Screen::placeSizedGlyph(fx.t, U'東', fx.spec, fx.specID));
    CHECK_EQ(sizedCellCount(fx.t, ScreenMode::Alternate), 18);

    fx.t.grid->setAlternateActive(ScreenMode::Primary);
    fx.t.screens.activate(Modes::ScreenBuffer::Primary);
    resize(fx.t, 4, 20);

    CHECK_EQ(sizedCellCount(fx.t, ScreenMode::Alternate), 0);
}

TEST(theClearThatKeepsTheInputRowDoesNotKeepABandOfIt)
{
    // Cmd-K mid-typing preserves one row of the screen and drops everything
    // else, history included. That row can be any row of a block — here a
    // middle band — and re-blitting it alone puts a chain claiming a backing
    // at the top of an otherwise empty screen. The ids the erasure releases
    // have to come back too, or a table that survives clear after clear
    // reaches its ceiling and refuses the next size the shell asks for.
    SizedTerminal fx(6, 20, /*scale=*/3, /*width=*/2);
    fx.t.screens.active->cursorX = 0;
    fx.t.screens.active->cursorY = 0;
    CHECK(Screen::placeSizedGlyph(fx.t, U'東', fx.spec, fx.specID));
    CHECK_EQ(fx.t.textSizeTable.liveCount(), 1);
    // The cursor sits on the block's middle band, past the block's columns.
    fx.t.screens.active->cursorY = 1;
    fx.t.screens.active->cursorX = 8;

    clearBufferPreserveInput(&fx.t);

    CHECK_EQ(sizedCellCount(fx.t, ScreenMode::Primary), 0);
    CHECK_EQ(fx.t.textSizeTable.liveCount(), 0);
}

TEST(theRowTextWalksReadABlocksLowerBandsAsBlank)
{
    // Every row a block covers carries the glyph so a renderer can crop that
    // row's slice. The walks that turn rows into text — command capture, the
    // accessibility projection, search, the print sink — must read only the
    // band that owns it, or one glyph becomes one per row everywhere at once.
    SizedTerminal fx(6, 20, /*scale=*/3, /*width=*/1);
    fx.t.drawsSizedText = true;
    fx.t.drawsProportionalText = true;
    processBytes(&fx.t, "\x1b[H\x1b]66;s=3;A\x07");
    CHECK(sizedCellCount(fx.t, ScreenMode::Primary) > 0);

    // The block's top row reads as the character; the rows under it read
    // blank, the same answer they would give for a wide pair's trailing half.
    const std::string top = Screen::absoluteRowText(fx.t, fx.t.scrollbackCount + 0, fx.t.columns);
    const std::string second = Screen::absoluteRowText(fx.t, fx.t.scrollbackCount + 1, fx.t.columns);
    const std::string third = Screen::absoluteRowText(fx.t, fx.t.scrollbackCount + 2, fx.t.columns);
    CHECK(top.find('A') != std::string::npos);
    CHECK(second.find('A') == std::string::npos);
    CHECK(third.find('A') == std::string::npos);

    // And the accessibility projection, which reads the visible screen through
    // its own walk, says the same thing.
    const Accessibility::TextProjection projection = Accessibility::projectVisibleText(fx.t, 0);
    int glyphCount = 0;
    for (std::size_t i = 0; i < projection.text.size(); ++i)
        if (projection.text[i] == 'A')
            ++glyphCount;
    CHECK_EQ(glyphCount, 1);
}

namespace {

/// Every spec id a cell on the primary screen or in the scrollback names, with
/// the spec it resolves to now.
std::vector<std::pair<unsigned, TextSizeSpec>> primaryAndHistorySpecs(const Terminal& t)
{
    std::vector<std::pair<unsigned, TextSizeSpec>> named;
    const auto collect = [&](const Cell* cells, int count) {
        for (int column = 0; column < count; ++column) {
            const unsigned specID = cellPen(cells[column]).extendedAttributes.textSizeSpecID();
            if (const TextSizeSpec* spec = specID != 0 ? t.textSizeTable.lookup(specID) : nullptr)
                named.emplace_back(specID, *spec);
        }
    };
    for (int row = 0; row < t.rows; ++row) {
        const RowView view = t.grid->rowCells(ScreenMode::Primary, Row(row));
        if (view.data)
            collect(view.data, view.count);
    }
    for (int line = 0; line < Screen::scrollbackCount(&t); ++line) {
        if (const Screen::ScrollLine* stored = Screen::scrollbackLine(&t, line))
            collect(stored->cells, stored->length);
    }
    return named;
}

} // namespace

TEST(theCensusKeepsTheIDsTheInactiveScreenAndReflowedHistoryName)
{
    // A full table on the alternate screen reclaims only what no cell anywhere
    // names. The primary is frozen behind the alternate and returns with its
    // cells, and its history, rewrapped by a resize, is drawn when the view
    // scrolls back. A census that read the active screen alone would free
    // their ids and mint a different geometry into them.
    Terminal t;
    hardReset(t);
    resize(t, 6, 40);
    t.drawsSizedText = true;
    t.drawsProportionalText = true;
    // Eight distinct runs, one per line: three age into history, five stay.
    for (int cells = 1; cells <= 8; ++cells) {
        char sequence[48];
        std::snprintf(sequence, sizeof(sequence), "\x1b]66;p=1:c=%d;r\x07\r\n", cells);
        processBytes(&t, sequence);
    }
    CHECK_MSG(Screen::scrollbackCount(&t) > 0, "premise: runs reached the scrollback");
    resize(t, 6, 30);
    const std::vector<std::pair<unsigned, TextSizeSpec>> before = primaryAndHistorySpecs(t);
    CHECK_MSG(!before.empty(), "premise: the primary screen and its history name specs");

    // On the alternate, seven distinct runs over one field fill the table, and
    // only the last of them stays named.
    processBytes(&t, "\x1b[?1049h");
    for (int cells = 9; cells <= 15; ++cells) {
        char sequence[48];
        std::snprintf(sequence, sizeof(sequence), "\x1b[H\x1b]66;p=1:c=%d;a\x07", cells);
        processBytes(&t, sequence);
    }
    CHECK_EQ(t.textSizeTable.liveCount(), 15);

    processBytes(&t, "\x1b[3;1H\x1b]66;p=1:c=16;new\x07");
    const Cell* anchor = t.grid->cellAt(ScreenMode::Alternate, Row(2), Column(0));
    const unsigned newID = anchor ? cellPen(*anchor).extendedAttributes.textSizeSpecID() : 0;
    CHECK_MSG(newID != 0, "the census found the overwritten alternate specs and still refused");
    for (const auto& [specID, spec] : before) {
        CHECK_MSG(specID != newID, "the new spec minted into a slot the frozen primary or its history names");
        const TextSizeSpec* now = t.textSizeTable.lookup(specID);
        CHECK_MSG(now && *now == spec, "a primary or history cell resolves to a different specification after the census");
    }

    // Back on the primary, its cells draw the sizes they were written with.
    processBytes(&t, "\x1b[?1049l");
    CHECK(primaryAndHistorySpecs(t) == before);
}

TEST(cancelAfterOSC66DoesNotRestoreAnEarlierGraphic)
{
    for (const std::string_view spec : { "s=2", "p=1:c=4" }) {
        for (const bool hasPayloadLineFeed : { false, true }) {
            Terminal t;
            hardReset(t);
            resize(t, 8, 8);
            t.drawsSizedText = true;
            t.drawsProportionalText = true;
            const auto prefix = std::string("X\0", 2) + "\033]66;" + std::string(spec) + ";" + (hasPayloadLineFeed ? "\nZ" : "Z");
            processBytes(&t, prefix);
            processBytes(&t, "\a");
            CHECK_EQ(t.grid->cellAt(Row(0), Column(0))->codePoint, 'X');
            CHECK(t.grid->hasSizedCellHint());
            const int x = t.screens.active->cursorX;
            const int y = t.screens.active->cursorY;
            std::array<Cell, 64> before;
            for (int row = 0; row < 8; ++row) {
                for (int column = 0; column < 8; ++column)
                    before[row * 8 + column] = *t.grid->cellAt(Row(row), Column(column));
            }
            processBytes(&t, "\033T");
            CHECK_EQ(t.grid->cellAt(Row(0), Column(0))->codePoint, 'X');
            CHECK_EQ(t.screens.active->cursorX, x);
            CHECK_EQ(t.screens.active->cursorY, y);
            for (int row = 0; row < 8; ++row) {
                for (int column = 0; column < 8; ++column)
                    CHECK(std::memcmp(t.grid->cellAt(Row(row), Column(column)), &before[row * 8 + column], sizeof(Cell)) == 0);
            }
        }
    }
}

UBIQUITTY_TEST_MAIN()
