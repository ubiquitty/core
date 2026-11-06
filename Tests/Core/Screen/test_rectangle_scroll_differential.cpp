// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// The bulk rectangle scroll moves what a per-cell one moves.
//
// CellGrid::scrollRectangleCells settles per row, from one sequential read of
// the source span, the bookkeeping a per-cell write repeats for every column:
// the live width, the shaping and multicell row flags, the blink and sized-text
// hints. That is an equivalence claim about a hot path, so it is held against
// a per-cell loop rather than against a description of one — the reference
// below is that loop, written through the public grid API a caller uses.
//
// The grids are painted cell by cell from a seeded sequence rather than by
// feeding text, because the claim turns on where a row's live width sits
// relative to the moved span: the two implementations differ only when the
// last occupied column of the span lands exactly on the destination's width,
// and a corpus of written lines reaches that boundary only by accident.

#include "Assertions.h"
#include "Harness.h"
#include "TerminalFixture.h"

#include "Core/Screen/CellBlank.h"
#include "Core/Screen/Operations.h"
#include "Core/Screen/SizedBlock.h"
#include "Core/Screen/WidePair.h"
#include "Core/TerminalOperations.h"
#include "Core/Unicode/Joining.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using UbiquittyTest::feed;

namespace {

constexpr int gridRows = 10;
constexpr int gridColumns = 20;

// The per-cell rectangle move, through the public grid API, as the reference
// for the bulk path. Kept as a loop rather than reduced to what it computes, so
// a divergence indicts the bulk code and not a paraphrase of the reference.
void referenceScrollRectangle(Ubiquitty::Terminal& t, int top, int bot, int left, int right, int n,
    const Ubiquitty::Cell& blank, Ubiquitty::RectangleScrollDirection direction)
{
    const int height = bot - top + 1;
    if (n > height)
        n = height;
    if (direction == Ubiquitty::RectangleScrollDirection::Up) {
        for (int row = top; row + n <= bot; row++) {
            for (int column = left; column <= right; column++) {
                const Ubiquitty::Cell* source = t.grid->cellAt(Ubiquitty::Row(row + n), Ubiquitty::Column(column));
                t.grid->setCell(Ubiquitty::Row(row), Ubiquitty::Column(column), source ? *source : blank);
            }
        }
        for (int row = bot - n + 1; row <= bot; row++) {
            for (int column = left; column <= right; column++)
                t.grid->setCell(Ubiquitty::Row(row), Ubiquitty::Column(column), blank);
        }
        return;
    }
    for (int row = bot; row - n >= top; row--) {
        for (int column = left; column <= right; column++) {
            const Ubiquitty::Cell* source = t.grid->cellAt(Ubiquitty::Row(row - n), Ubiquitty::Column(column));
            t.grid->setCell(Ubiquitty::Row(row), Ubiquitty::Column(column), source ? *source : blank);
        }
    }
    for (int row = top; row < top + n; row++) {
        for (int column = left; column <= right; column++)
            t.grid->setCell(Ubiquitty::Row(row), Ubiquitty::Column(column), blank);
    }
}

// The per-cell horizontal band move SL/SR used before the row-wise one, through
// the public grid API it used then. Kept as the loop was, for the same reason
// the rectangle reference above is.
void referenceScrollHorizontal(Ubiquitty::Terminal& t, int top, int bot, int left, int right, int n,
    const Ubiquitty::Cell& blank, Ubiquitty::Screen::HorizontalScrollDirection direction)
{
    if (n > right - left + 1)
        n = right - left + 1;
    for (int row = top; row <= bot; row++) {
        if (direction == Ubiquitty::Screen::HorizontalScrollDirection::Right) {
            for (int column = right; column >= left + n; column--) {
                const Ubiquitty::Cell* source = t.grid->cellAt(Ubiquitty::Row(row), Ubiquitty::Column(column - n));
                t.grid->setCell(Ubiquitty::Row(row), Ubiquitty::Column(column), source ? *source : blank);
            }
            for (int column = left; column < left + n; column++)
                t.grid->setCell(Ubiquitty::Row(row), Ubiquitty::Column(column), blank);
        } else {
            for (int column = left; column + n <= right; column++) {
                const Ubiquitty::Cell* source = t.grid->cellAt(Ubiquitty::Row(row), Ubiquitty::Column(column + n));
                t.grid->setCell(Ubiquitty::Row(row), Ubiquitty::Column(column), source ? *source : blank);
            }
            for (int column = right - n + 1; column <= right; column++)
                t.grid->setCell(Ubiquitty::Row(row), Ubiquitty::Column(column), blank);
        }
    }
    // The two repair passes that follow the move loop, so the
    // reference is the whole operation and not the half of it that moves cells:
    // a band shift splits a wide pair across either edge, and the claim under
    // test is about the move, not about whether the tear is repaired.
    Ubiquitty::CellScratchBuffer<Ubiquitty::maxColumns> rowStorage;
    Ubiquitty::Cell* rowBuffer = rowStorage.data();
    for (int row = top; row <= bot; row++) {
        if (!t.grid->rowMeta(row).has(Ubiquitty::RowFlag::HasMulticell))
            continue;
        t.grid->readRow(Ubiquitty::Row(row), rowBuffer);
        int firstTouched = t.columns;
        int lastTouched = -1;
        Ubiquitty::Screen::scrubMulticellOrphans(rowBuffer, t.columns, blank, &t.textSizeTable, t.ambiguousWidth, firstTouched, lastTouched);
        if (lastTouched < firstTouched)
            continue;
        Ubiquitty::RowView span = { &rowBuffer[firstTouched], lastTouched - firstTouched + 1 };
        t.grid->setRowSpan(Ubiquitty::Row(row), Ubiquitty::Column(firstTouched), span);
    }
    Ubiquitty::Screen::scrubBlockDamage(t, top, bot);
}

// Everything a reader can observe of a row: its cells, and the live width that
// decides how much of them any reader is handed.
/// Which fields a fingerprint carries.
enum class ShapingField {
    /// The row's shaping flag is part of the comparison.
    Included,
    /// It is held apart and compared by its own rule.
    Excluded,
};

std::string gridFingerprint(Ubiquitty::Terminal& t, ShapingField shapingField = ShapingField::Included)
{
    std::string out;
    char field[80];
    for (int row = 0; row < t.rows; ++row) {
        for (int column = 0; column < t.columns; ++column) {
            const Ubiquitty::Cell* cell = t.grid->cellAt(Ubiquitty::Row(row), Ubiquitty::Column(column));
            if (!cell) {
                out += "<null>|";
                continue;
            }
            std::snprintf(field, sizeof(field), "%u/%u/%u|", static_cast<unsigned>(cell->codePoint),
                static_cast<unsigned>(Ubiquitty::cellPen(*cell).attributes.toRaw()),
                static_cast<unsigned>(Ubiquitty::cellPen(*cell).extendedAttributes.textSizeSpecID()));
            out += field;
        }
        const Ubiquitty::RowMeta& meta = t.grid->rowMeta(row);
        std::snprintf(field, sizeof(field), " width=%d shaping=%d multicell=%d\n",
            t.grid->scrollCaptureMetadata(row).contentWidth,
            shapingField == ShapingField::Included && meta.has(Ubiquitty::RowFlag::NeedsShaping) ? 1 : 0,
            meta.has(Ubiquitty::RowFlag::HasMulticell) ? 1 : 0);
        out += field;
    }
    // The grid-wide hints belong to what a reader observes: a surface arms its blink clock and the
    // sized-text flatten off these, and the scroll decides whether to look for either by asking them.
    std::snprintf(field, sizeof(field), "blinkHint=%d sizedHint=%d\n", t.grid->hasBlinkHint() ? 1 : 0,
        t.grid->hasSizedCellHint() ? 1 : 0);
    out += field;
    return out;
}

// Whether any cell of a row would set the shaping gate, read from the cells
// rather than from the flag that is supposed to describe them.
bool anyCellShapes(Ubiquitty::Terminal& t, int row)
{
    for (int column = 0; column < t.columns; ++column) {
        const Ubiquitty::Cell* cell = t.grid->cellAt(Ubiquitty::Row(row), Ubiquitty::Column(column));
        if (cell && Ubiquitty::Unicode::mayNeedShaping(cell->codePoint))
            return true;
    }
    return false;
}

// A palette the parser built, so every cell carries the attribute shape real
// content has: a wide lead and its continuation, a blink cell, a coloured
// blank, plain text. Read once from a terminal that wrote them.
std::vector<Ubiquitty::Cell> buildPalette()
{
    Ubiquitty::Terminal source;
    Ubiquitty::hardReset(source);
    Ubiquitty::resize(source, 4, gridColumns);
    feed(source, "\x1b[1;1Hab");
    feed(source, "\x1b[1;4H\xE6\x9D\xB1"); // U+6771: a wide lead at 4, its continuation at 5
    feed(source, "\x1b[1;7H\x1b[5mB\x1b[0m"); // blink
    feed(source, "\x1b[1;9H\x1b[41m \x1b[0m"); // a blank carrying a background
    feed(source, "\x1b[1;11H\xD9\x85"); // U+0645 ARABIC LETTER MEEM: a cursive base, so its row shapes

    std::vector<Ubiquitty::Cell> palette;
    for (int column : { 0, 3, 4, 6, 8, 10 }) {
        const Ubiquitty::Cell* cell = source.grid->cellAt(Ubiquitty::Row(0), Ubiquitty::Column(column));
        palette.push_back(cell ? *cell : Ubiquitty::Cell {});
    }
    palette.push_back(Ubiquitty::Cell {}); // the default blank
    return palette;
}

// xorshift32: a deterministic sequence, so a failure reproduces from the seed.
struct Sequence {
    std::uint32_t state;
    std::uint32_t next()
    {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return state;
    }
    int upTo(int bound) { return bound <= 0 ? 0 : static_cast<int>(next() % static_cast<std::uint32_t>(bound)); }
};

// Paint both grids identically: a random live width per row, filled from the
// palette, with the tail past that width left blank. Writing the widths across
// the whole range is what walks the live-width boundary through the span.
void paintBoth(Ubiquitty::Terminal& bulk, Ubiquitty::Terminal& reference,
    const std::vector<Ubiquitty::Cell>& palette, Sequence& sequence)
{
    for (int row = 0; row < gridRows; ++row) {
        const int width = sequence.upTo(gridColumns + 1);
        for (int column = 0; column < width; ++column) {
            const Ubiquitty::Cell& cell = palette[static_cast<std::size_t>(sequence.upTo(static_cast<int>(palette.size())))];
            bulk.grid->setCell(Ubiquitty::Row(row), Ubiquitty::Column(column), cell);
            reference.grid->setCell(Ubiquitty::Row(row), Ubiquitty::Column(column), cell);
        }
    }
}

} // namespace

TEST(bulkRectangleScrollMatchesThePerCellMoveItReplaced)
{
    const std::vector<Ubiquitty::Cell> palette = buildPalette();
    Sequence sequence { 0x9E3779B9u };
    int cases = 0;
    int rectanglesThatMovedContent = 0;

    for (int iteration = 0; iteration < 3000; ++iteration) {
        Ubiquitty::Terminal bulk;
        Ubiquitty::Terminal reference;
        Ubiquitty::hardReset(bulk);
        Ubiquitty::hardReset(reference);
        Ubiquitty::resize(bulk, gridRows, gridColumns);
        Ubiquitty::resize(reference, gridRows, gridColumns);
        paintBoth(bulk, reference, palette, sequence);

        const std::string painted = gridFingerprint(bulk);
        CHECK_STR_EQ(painted, gridFingerprint(reference));

        const int top = sequence.upTo(gridRows - 1);
        const int bot = top + 1 + sequence.upTo(gridRows - top - 1);
        const int left = sequence.upTo(gridColumns);
        const int right = left + sequence.upTo(gridColumns - left);
        const int n = 1 + sequence.upTo(bot - top + 1);
        const Ubiquitty::RectangleScrollDirection direction = (sequence.next() & 1u) != 0
            ? Ubiquitty::RectangleScrollDirection::Up
            : Ubiquitty::RectangleScrollDirection::Down;
        const Ubiquitty::Cell blank = (sequence.next() & 3u) == 0 ? palette[4] : Ubiquitty::Cell {};

        bulk.grid->scrollRectangleCells(top, bot, left, right, n, blank, direction);
        referenceScrollRectangle(reference, top, bot, left, right, n, blank, direction);

        const std::string moved = gridFingerprint(bulk);
        CHECK_STR_EQ(moved, gridFingerprint(reference));
        if (moved != painted)
            ++rectanglesThatMovedContent;
        ++cases;
    }

    // Guards against a corpus that compares nothing: the loop must run, and the
    // scrolls must actually change the grids they are checked on.
    CHECK_EQ(cases, 3000);
    CHECK(rectanglesThatMovedContent > 2000);
}

TEST(rowWiseHorizontalScrollMatchesThePerCellMoveItReplaced)
{
    const std::vector<Ubiquitty::Cell> palette = buildPalette();
    Sequence sequence { 0x85EBCA6Bu };
    int cases = 0;
    int bandsThatMovedContent = 0;
    int rowsWhoseShapingWasAnswered = 0;

    for (int iteration = 0; iteration < 3000; ++iteration) {
        Ubiquitty::Terminal rowWise;
        Ubiquitty::Terminal reference;
        Ubiquitty::hardReset(rowWise);
        Ubiquitty::hardReset(reference);
        Ubiquitty::resize(rowWise, gridRows, gridColumns);
        Ubiquitty::resize(reference, gridRows, gridColumns);
        paintBoth(rowWise, reference, palette, sequence);

        const std::string painted = gridFingerprint(rowWise);
        CHECK_STR_EQ(painted, gridFingerprint(reference));

        // The margins SL/SR runs inside, driven through the terminal so the
        // band the row-wise scroll reads is the band the mode selects — a band picked
        // here instead would test the loop against arguments nothing produces.
        const int top = sequence.upTo(gridRows - 1);
        const int bot = top + 1 + sequence.upTo(gridRows - top - 1);
        const int left = sequence.upTo(gridColumns);
        const int right = left + sequence.upTo(gridColumns - left);
        const int n = 1 + sequence.upTo(right - left + 1);
        const Ubiquitty::Screen::HorizontalScrollDirection direction = (sequence.next() & 1u) != 0
            ? Ubiquitty::Screen::HorizontalScrollDirection::Right
            : Ubiquitty::Screen::HorizontalScrollDirection::Left;

        for (Ubiquitty::Terminal* terminal : { &rowWise, &reference }) {
            terminal->modes.leftRightMarginMode = true;
            terminal->screens.active->scrollTop = top;
            terminal->screens.active->scrollBottom = bot;
            terminal->screens.active->scrollLeft = left;
            terminal->screens.active->scrollRight = right;
            // Inside the region, where SL and SR act; outside it they leave
            // the region alone, which leaves nothing to compare.
            terminal->screens.active->cursorY = top;
            terminal->screens.active->cursorX = left;
        }
        Ubiquitty::Cell blank;
        Ubiquitty::Screen::eraseCell(&blank, reference.screens.active);

        Ubiquitty::Screen::scrollRegionHorizontal(rowWise, n, direction);
        referenceScrollHorizontal(reference, top, bot, left, right, n, blank, direction);

        // Every cell, the live width and the multicell flag, exactly. The
        // shaping flag is held apart below: it is sticky-true under a per-cell
        // write, which can only ever set it, and a span covering a whole row is
        // that row's own answer — so the row-wise path can retract a flag the
        // reference has no way to.
        const std::string moved = gridFingerprint(rowWise, ShapingField::Excluded);
        CHECK_STR_EQ(moved, gridFingerprint(reference, ShapingField::Excluded));
        for (int row = 0; row < gridRows; ++row) {
            const bool doesRowWiseShape = rowWise.grid->rowMeta(row).has(Ubiquitty::RowFlag::NeedsShaping);
            const bool doesReferenceShape = reference.grid->rowMeta(row).has(Ubiquitty::RowFlag::NeedsShaping);
            CHECK_MSG(!doesRowWiseShape || doesReferenceShape, "the row-wise path raised a shaping flag the per-cell one did not");
            if (doesRowWiseShape != doesReferenceShape) {
                // The only way the two differ: the row-wise write covered the
                // whole row and nothing in it shapes, so the flag was answered
                // rather than added to.
                CHECK_MSG(!anyCellShapes(rowWise, row), "a shaping flag was dropped on a row that shapes");
                ++rowsWhoseShapingWasAnswered;
            }
        }
        if (moved != painted)
            ++bandsThatMovedContent;
        ++cases;
    }

    CHECK_EQ(cases, 3000);
    CHECK(bandsThatMovedContent > 2000);
    // Reported rather than required: the corpus reaches the whole-row span often,
    // but the count is a property of the sequence and not of the claim.
    std::printf("rows whose shaping flag the row-wise write answered: %d\n", rowsWhoseShapingWasAnswered);
}

UBIQUITTY_TEST_MAIN()
