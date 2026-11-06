// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// The in-place row shift stores what the read-shift-store one stored.
//
// ICH / DCH / DECIC / DECDC read the row whole into a buffer, shifted it there
// and stored the window back, which is two row-length copies for a window
// usually shorter than the row. CellGrid::shiftRowCells moves the row where it
// lies. The claim is that the two leave the same grid, so it is held against
// the buffer path itself, written below through the public API it used.
//
// The hints are part of the comparison, not decoration. `hasBlinkHint` may be
// stale-true and must never be stale-false — a viewer retracts it against a
// whole-grid scan, and a shift that let a Blink cell arrive unannounced is the
// stale-false that stops a cursor blinking. The in-place path raises hints for
// the blank alone, on the argument that a shift introduces no other cell the
// row did not already hold; comparing the hint is what tests that argument.
//
// Rows carrying multicell content are not in scope: the caller keeps the buffer
// path for those, because a pair split by the window's edge needs the scrub.

#include "Assertions.h"
#include "Harness.h"
#include "TerminalFixture.h"

#include "Core/TerminalOperations.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using UbiquittyTest::feed;

namespace {

constexpr int gridRows = 6;
constexpr int gridColumns = 24;

// The read-shift-store the in-place path replaces, through the public grid API
// it used. Kept as that path was, minus the multicell scrub the caller now
// reaches only for rows this one never sees.
void referenceShiftRow(Ubiquitty::Terminal& t, int row, int from, int rightBound, int n,
    const Ubiquitty::Cell& blank, Ubiquitty::RowShiftDirection direction)
{
    if (n > rightBound - from)
        n = rightBound - from;
    if (n <= 0)
        return;
    std::vector<Ubiquitty::Cell> rowBuffer(static_cast<std::size_t>(Ubiquitty::maxColumns));
    t.grid->readRow(Ubiquitty::Row(row), rowBuffer.data());
    const std::size_t moved = static_cast<std::size_t>(rightBound - from - n);
    if (direction == Ubiquitty::RowShiftDirection::InsertRight) {
        std::memmove(&rowBuffer[static_cast<std::size_t>(from + n)], &rowBuffer[static_cast<std::size_t>(from)],
            moved * sizeof(Ubiquitty::Cell));
        for (int i = 0; i < n; i++)
            rowBuffer[static_cast<std::size_t>(from + i)] = blank;
    } else {
        std::memmove(&rowBuffer[static_cast<std::size_t>(from)], &rowBuffer[static_cast<std::size_t>(from + n)],
            moved * sizeof(Ubiquitty::Cell));
        for (int i = rightBound - n; i < rightBound; i++)
            rowBuffer[static_cast<std::size_t>(i)] = blank;
    }
    const Ubiquitty::RowView span { &rowBuffer[static_cast<std::size_t>(from)], rightBound - from };
    t.grid->setRowSpan(Ubiquitty::Row(row), Ubiquitty::Column(from), span);
}

std::string gridFingerprint(Ubiquitty::Terminal& t)
{
    std::string out;
    char field[96];
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
            meta.has(Ubiquitty::RowFlag::NeedsShaping) ? 1 : 0,
            meta.has(Ubiquitty::RowFlag::HasMulticell) ? 1 : 0);
        out += field;
    }
    std::snprintf(field, sizeof(field), "blinkHint=%d sizedHint=%d\n", t.grid->hasBlinkHint() ? 1 : 0,
        t.grid->hasSizedCellHint() ? 1 : 0);
    out += field;
    return out;
}

// Plain text, a blink cell and a cell with a background, which is a blank that
// carries content for the live-width rule. No wide characters: a row holding
// one keeps the buffer path.
std::vector<Ubiquitty::Cell> buildPalette()
{
    Ubiquitty::Terminal source;
    Ubiquitty::hardReset(source);
    Ubiquitty::resize(source, 4, gridColumns);
    feed(source, "\x1b[1;1Hab");
    feed(source, "\x1b[1;4H\x1b[5mB\x1b[0m");
    feed(source, "\x1b[1;6H\x1b[41m \x1b[0m");
    feed(source, "\x1b[1;8H\x1b[5;41m \x1b[0m"); // a blank that blinks: an erase under a blinking pen

    std::vector<Ubiquitty::Cell> palette;
    for (int column : { 0, 1, 3, 5, 7 }) {
        const Ubiquitty::Cell* cell = source.grid->cellAt(Ubiquitty::Row(0), Ubiquitty::Column(column));
        palette.push_back(cell ? *cell : Ubiquitty::Cell {});
    }
    palette.push_back(Ubiquitty::Cell {});
    return palette;
}

// Indices into the palette above.
[[maybe_unused]] constexpr std::size_t blinkingCell = 2;
constexpr std::size_t colouredBlank = 3;
constexpr std::size_t blinkingBlank = 4;
constexpr std::size_t defaultBlank = 5;

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

// Whether the painted grid may already contain a blinking cell. Excluding one
// brings the grid to the shift with the blink hint down, and only then can a
// blinking blank arriving with the shift be what raises it — the case that
// tests the in-place path's claim about hints.
enum class BlinkInPaint : std::uint8_t {
    Excluded,
    Included
};

void paintBoth(Ubiquitty::Terminal& inPlace, Ubiquitty::Terminal& reference,
    const std::vector<Ubiquitty::Cell>& palette, Sequence& sequence, BlinkInPaint blinkInPaint)
{
    static constexpr std::size_t quietEntries[] = { 0, 1, colouredBlank, defaultBlank };
    for (int row = 0; row < gridRows; ++row) {
        const int width = sequence.upTo(gridColumns + 1);
        for (int column = 0; column < width; ++column) {
            const std::size_t index = blinkInPaint == BlinkInPaint::Excluded
                ? quietEntries[static_cast<std::size_t>(sequence.upTo(4))]
                : static_cast<std::size_t>(sequence.upTo(static_cast<int>(palette.size())));
            const Ubiquitty::Cell& cell = palette[index];
            inPlace.grid->setCell(Ubiquitty::Row(row), Ubiquitty::Column(column), cell);
            reference.grid->setCell(Ubiquitty::Row(row), Ubiquitty::Column(column), cell);
        }
    }
}

} // namespace

TEST(inPlaceRowShiftMatchesTheBufferShiftItReplaced)
{
    const std::vector<Ubiquitty::Cell> palette = buildPalette();
    Sequence sequence { 0x85EBCA6Bu };
    int cases = 0;
    int shiftsThatMovedContent = 0;

    for (int iteration = 0; iteration < 3000; ++iteration) {
        Ubiquitty::Terminal inPlace;
        Ubiquitty::Terminal reference;
        Ubiquitty::hardReset(inPlace);
        Ubiquitty::hardReset(reference);
        Ubiquitty::resize(inPlace, gridRows, gridColumns);
        Ubiquitty::resize(reference, gridRows, gridColumns);
        const BlinkInPaint blinkInPaint = (iteration & 1) == 0 ? BlinkInPaint::Excluded : BlinkInPaint::Included;
        paintBoth(inPlace, reference, palette, sequence, blinkInPaint);

        const std::string painted = gridFingerprint(inPlace);
        CHECK_STR_EQ(painted, gridFingerprint(reference));

        const int row = sequence.upTo(gridRows);
        const int from = sequence.upTo(gridColumns);
        const int rightBound = from + 1 + sequence.upTo(gridColumns - from);
        const int n = 1 + sequence.upTo(rightBound - from);
        const Ubiquitty::RowShiftDirection direction = (sequence.next() & 1u) != 0
            ? Ubiquitty::RowShiftDirection::InsertRight
            : Ubiquitty::RowShiftDirection::DeleteLeft;
        // On the quiet grids the blank is the blinking one often enough to make
        // the hint's arrival the shift's own doing.
        const std::uint32_t choice = sequence.next() & 3u;
        const Ubiquitty::Cell blank = blinkInPaint == BlinkInPaint::Excluded && choice != 0 ? palette[blinkingBlank]
            : choice == 0                                                                   ? palette[colouredBlank]
                                                                                            : Ubiquitty::Cell {};

        inPlace.grid->shiftRowCells(row, from, rightBound, n, blank, direction);
        referenceShiftRow(reference, row, from, rightBound, n, blank, direction);

        const std::string shifted = gridFingerprint(inPlace);
        CHECK_STR_EQ(shifted, gridFingerprint(reference));
        if (shifted != painted)
            ++shiftsThatMovedContent;
        ++cases;
    }

    // Guards against a corpus that compares nothing: the loop must run, and the
    // shifts must actually change the grids they are checked on.
    CHECK_EQ(cases, 3000);
    CHECK(shiftsThatMovedContent > 2000);
}

UBIQUITTY_TEST_MAIN()
