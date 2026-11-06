// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Screen/Rect.cpp — The DEC rectangular-area operations — DECFRA, DECERA, DECSERA, DECCRA,
// DECRARA, DECCARA — and DECAC's recolouring of both whole screens.
//
// These are the PRIMITIVES; the CSI handlers that parse their parameters live in
// VT/Prefixed.cpp, so the feature is split across two modules by dispatch shape
// rather than by domain.
//
// Cold relative to the per-byte parser loop, which stays in VT.cpp.

#include "Core/CellGrid.h"
#include "Core/CellPen.h"
#include "Core/Screen/CellBlank.h"
#include "Core/Screen/LineIdentity.h"
#include "Core/Screen/Operations.h"
#include "Core/Screen/SizedBlock.h"
#include "Core/Screen/WidePair.h"

#include <cstdlib>
#include <cstring>

namespace Ubiquitty::Screen {

// A rectangle carries its own left and right edge, so unlike a linear erase it
// can cut a wide pair and leave the columns beyond the cut untouched — and
// DECCRA can WRITE a half whose partner was never copied, an orphan that
// arrives rather than one left behind. Either way a lead without its
// continuation paints a double-width glyph over a neighbour's column, and a
// continuation without its lead is a hole no cursor motion reaches.
//
// The linear controls hold this invariant already: Erase.cpp widens its range to
// whole pairs before writing, Edit.cpp scrubs after shifting. A rectangle cannot
// widen — that would erase columns outside the region the caller asked for, and
// for DECFRA it would fill them with the fill character. So it scrubs, on every
// row the operation touched.
//
// It does not consult Attribute::Protected, which DECSERA otherwise honours, and
// that is safe only because a pair cannot be half-protected. Both halves are
// written from one pen, and every writer of the bit targets a pen rather than a
// cell — DECSCA arms the active screen's pen, and DECCARA/DECRARA
// carry masks confined to Bold, Underline, Blink, Reverse and Hidden. Widen either of
// those masks to reach Protected and this scrub can blank a cell DECSERA
// promised to leave alone.
static void scrubRectangleOrphans(Ubiquitty::Terminal& t, int top, int bot)
{
    if (!t.grid)
        return;
    Ubiquitty::CellScratchBuffer<maxColumns> rowStorage;
    Ubiquitty::Cell* rowBuffer = rowStorage.data();
    Ubiquitty::Cell blank;
    Ubiquitty::Screen::eraseCell(&blank, t);
    for (int row = top; row <= bot; ++row) {
        if (row < 0 || row >= t.rows)
            continue;
        // Only a Wide lead or a WideContinuation gives the scrub a branch to reach, and
        // RowFlag::HasMulticell is latched by every write of one and never stale-false. Without it,
        // reading the row whole is the entire cost of proving there is nothing to scrub.
        if (!t.grid->rowMeta(row).has(Ubiquitty::RowFlag::HasMulticell))
            continue;
        t.grid->readRow(Ubiquitty::Row(row), rowBuffer);
        // Inverted bounds mean "nothing touched yet"; scrubMulticellOrphans only
        // widens them, so an untouched row still reads inverted and is skipped
        // rather than written back whole.
        int firstTouched = t.columns;
        int lastTouched = -1;
        Ubiquitty::Screen::scrubMulticellOrphans(rowBuffer, t.columns, blank, &t.textSizeTable, t.ambiguousWidth, firstTouched, lastTouched);
        if (lastTouched < firstTouched)
            continue;
        Ubiquitty::RowView span = { &rowBuffer[firstTouched], lastTouched - firstTouched + 1 };
        t.grid->setRowSpan(Ubiquitty::Row(row), Ubiquitty::Column(firstTouched), span);
    }
}

// DECFRA — fill rectangle with character `ch` using the current pen.
// DECFRA ignores DECSACE — only DECCARA / DECRARA consult the stream-vs-
// rectangle setting (VT420 spec). Build one row of identical cells, then write
// the whole span per row with setRowSpan.
static void rectFillTemplate(Ubiquitty::Terminal& t, const RectArea& r, const Ubiquitty::Cell& tmpl)
{
    if (!t.grid)
        return;
    const int width = r.right - r.left + 1;
    if (width <= 0)
        return;
    Ubiquitty::CellScratchBuffer<maxColumns> rowStorage;
    Ubiquitty::Cell* rowBuffer = rowStorage.data();
    int n = width;
    if (n > maxColumns)
        n = maxColumns;
    for (int c = 0; c < n; ++c) {
        rowStorage.constructAt(static_cast<std::size_t>(c), tmpl);
    }
    Ubiquitty::RowView view = { rowBuffer, n };
    for (int row = r.top; row <= r.bot; ++row) {
        t.grid->setRowSpan(Ubiquitty::Row(row), Ubiquitty::Column(r.left), view);
    }
    scrubRectangleOrphans(t, r.top, r.bot);
    scrubBlockDamage(t, r.top, r.bot);
}

void rectFill(Ubiquitty::Terminal& t, const RectArea& r, uint32_t ch)
{
    Ubiquitty::Screen::State* s = t.screens.active;
    Ubiquitty::Cell cell = {};
    cell.codePoint = ch;
    cellSetPenFromScreen(cell, s);
    rectFillTemplate(t, r, cell);
}

// DECERA — erase rectangle (cells become space with the current background).
// DECERA ignores DECSACE — see rectFill note.
void rectErase(Ubiquitty::Terminal& t, const RectArea& r)
{
    Ubiquitty::Screen::State* s = t.screens.active;
    Ubiquitty::Cell blank = {};
    blank.codePoint = 0;
    // DECERA is an ERASE (background color only, no rendition attributes — the
    // BCE erased state), else it leaks the current SGR onto the blanks.
    Pen erased = s->pen;
    erased.attributes = {};
    penSetGuarded(&erased, false);
    erased.semanticRole = SemanticRole::Output;
    Ubiquitty::cellSetPen(blank, erased);
    // Selective erase is the DECSED/SEL Ps=2 path; plain DECERA wipes regardless
    // of Attribute::Protected.
    if (hasActivePins(t)) [[unlikely]]
        notePositionsRewritten(t, r.top, r.bot);
    rectFillTemplate(t, r, blank);
}

// DECSERA — Selective Erase Rectangular Area. Erases like DECERA but SKIPS
// DECSCA-protected cells. Ignores DECSACE (always a true block, VT420).
// The erased positions keep their rendition.
void rectSelectiveErase(Ubiquitty::Terminal& t, const RectArea& r)
{
    if (!t.grid)
        return;
    if (hasActivePins(t)) [[unlikely]]
        notePositionsRewritten(t, r.top, r.bot);
    for (int row = r.top; row <= r.bot; row++) {
        int from = r.left;
        int to = r.right;
        expandRangeToWholeRuns(t.grid.get(), row, t.columns, from, to);
        for (int column = from; column <= to; column++) {
            const Ubiquitty::Cell* cell = t.grid->cellAt(Ubiquitty::Row(row), Ubiquitty::Column(column));
            if (cell && Ubiquitty::cellPen(*cell).attributes.contains(Ubiquitty::VT::Attribute::Protected))
                continue; // DECSCA-protected — left intact
            if (cell)
                t.grid->setCell(Ubiquitty::Row(row), Ubiquitty::Column(column), selectivelyErasedCell(*cell));
        }
    }
    scrubRectangleOrphans(t, r.top, r.bot);
    scrubBlockDamage(t, r.top, r.bot);
}

// DECCRA — copy rectangle (source top,left,bot,right → dest dt,dl). Page
// parameters ignored (one page). Overlapping src/dst must behave as if copied
// via a temporary first; honoured with a row-major buffer snapshot.
void rectCopy(Ubiquitty::Terminal& t, const RectArea& src, int dt, int dl)
{
    if (!t.grid)
        return;
    int h = src.bot - src.top + 1;
    int w = src.right - src.left + 1;
    if (h <= 0 || w <= 0)
        return;
    if (h > maxRows)
        h = maxRows;
    if (w > maxColumns)
        w = maxColumns;
    // Snapshot the source first so overlapping src/dst writes work; sized to the
    // actual rect and heap-owned.
    size_t bytes = static_cast<size_t>(h) * static_cast<size_t>(w) * sizeof(Ubiquitty::Cell);
    Ubiquitty::Cell* buffer = static_cast<Ubiquitty::Cell*>(std::malloc(bytes));
    if (!buffer)
        return;
    for (int r = 0; r < h; r++) {
        for (int c = 0; c < w; c++) {
            const Ubiquitty::Cell* codePoint = t.grid->cellAt(Ubiquitty::Row(src.top + r), Ubiquitty::Column(src.left + c));
            buffer[r * w + c] = codePoint ? *codePoint : Ubiquitty::Cell {};
        }
    }
    // Write into the destination, clamped to grid bounds.
    for (int r = 0; r < h; r++) {
        int dstRow = dt - 1 + r;
        if (dstRow < 0 || dstRow >= t.rows)
            continue;
        for (int c = 0; c < w; c++) {
            int dstColumn = dl - 1 + c;
            if (dstColumn < 0 || dstColumn >= t.columns)
                continue;
            t.grid->setCell(Ubiquitty::Row(dstRow), Ubiquitty::Column(dstColumn), buffer[r * w + c]);
        }
    }
    std::free(buffer);
    scrubRectangleOrphans(t, dt - 1, dt - 1 + h - 1);
    scrubBlockDamage(t, dt - 1, dt - 1 + h - 1);
}

// DECSACE extent walker. For sace == 2 (rectangle): iterate the
// RectArea as a block. For sace == 1 (stream, the default): iterate
// reading-order — top row from r.left to columns-1, middle rows from 0
// to columns-1, bottom row from 0 to r.right. Single-row stream extents
// stay confined to [r.left, r.right].
template<class F> static void walkSaceExtent(Ubiquitty::Terminal& t, const RectArea& r, F fn)
{
    if (t.modes.sace == 2 || r.top == r.bot) {
        for (int row = r.top; row <= r.bot; row++)
            for (int column = r.left; column <= r.right; column++)
                fn(row, column);
        return;
    }
    int columns = t.columns;
    for (int c = r.left; c < columns; c++)
        fn(r.top, c);
    for (int row = r.top + 1; row < r.bot; row++)
        for (int c = 0; c < columns; c++)
            fn(row, c);
    for (int c = 0; c <= r.right; c++)
        fn(r.bot, c);
}

// The underline bit and the underline shape describe one rendition: a cell the
// bit turns on draws a single underline, and one it turns off draws none.
static void keepUnderlineShapeWithBit(Pen& pen)
{
    const bool isUnderlined = pen.attributes.contains(Ubiquitty::VT::Attribute::Underline);
    if (!isUnderlined)
        penSetUnderlineShape(&pen, Ubiquitty::VT::underlineNone);
    else if (penUnderlineShape(&pen) == Ubiquitty::VT::underlineNone)
        penSetUnderlineShape(&pen, Ubiquitty::VT::underlineSingle);
}

// Applies `change` to every cell of the DECSACE extent, blank cells included:
// the extent names character positions, not the characters written so far.
template<class Change> static void changeRenditionOverExtent(Ubiquitty::Terminal& t, const RectArea& r, Change change)
{
    if (!t.grid)
        return;
    auto changeCell = [&](Ubiquitty::Cell& cell) {
        Pen pen = cellPen(cell);
        change(pen);
        keepUnderlineShapeWithBit(pen);
        cellSetPen(cell, pen);
    };
    if (t.modes.sace == 2 || r.top == r.bot) {
        const int width = r.right - r.left + 1;
        if (width <= 0)
            return;
        Ubiquitty::CellScratchBuffer<maxColumns> rowStorage;
        Ubiquitty::Cell* rowBuffer = rowStorage.data();
        const int n = width > maxColumns ? maxColumns : width;
        for (int row = r.top; row <= r.bot; ++row) {
            for (int c = 0; c < n; ++c) {
                const Ubiquitty::Cell* source = t.grid->cellAt(Ubiquitty::Row(row), Ubiquitty::Column(r.left + c));
                rowStorage.constructAt(static_cast<std::size_t>(c), source ? *source : Ubiquitty::Cell {});
                changeCell(rowBuffer[c]);
            }
            Ubiquitty::RowView view = { rowBuffer, n };
            t.grid->setRowSpan(Ubiquitty::Row(row), Ubiquitty::Column(r.left), view);
        }
        return;
    }
    walkSaceExtent(t, r, [&](int row, int column) {
        const Ubiquitty::Cell* source = t.grid->cellAt(Ubiquitty::Row(row), Ubiquitty::Column(column));
        if (!source)
            return;
        Ubiquitty::Cell changed = *source;
        changeCell(changed);
        t.grid->setCell(Ubiquitty::Row(row), Ubiquitty::Column(column), changed);
    });
}

// DECRARA — reverse the selected rendition attributes over the DECSACE extent.
void rectReverseAttributes(Ubiquitty::Terminal& t, const RectArea& r, Ubiquitty::VT::AttributeSet toggleMask)
{
    changeRenditionOverExtent(t, r, [&](Pen& pen) { pen.attributes.toggle(toggleMask); });
}

// DECCARA — clear clearMask, then set setMask, over the DECSACE extent.
void rectChangeAttributes(Ubiquitty::Terminal& t, const RectArea& r, uint16_t setMask, uint16_t clearMask)
{
    changeRenditionOverExtent(t, r, [&](Pen& pen) {
        pen.attributes = Ubiquitty::VT::AttributeSet::fromRaw((pen.attributes.toRaw() & ~clearMask) | setMask);
    });
}

// DECAC item 1 — the normal text colours are the default ones, so a position
// in the default colours anywhere on either screen takes them. A deferred
// scroll run holds blanks in the old colours, so it is committed first.
void applyNormalTextColors(Ubiquitty::Terminal& t, const Ubiquitty::ColorAssignment& assigned)
{
    if (!t.grid || !Ubiquitty::isColorAssigned(assigned))
        return;
    scrollLaneFlush(t);
    const int columns = t.grid->columns();
    if (columns <= 0 || columns > maxColumns)
        return;
    Ubiquitty::CellScratchBuffer<maxColumns> rowStorage;
    Ubiquitty::Cell* rowBuffer = rowStorage.data();
    for (const Ubiquitty::ScreenMode screen : { Ubiquitty::ScreenMode::Primary, Ubiquitty::ScreenMode::Alternate }) {
        for (int row = 0; row < t.grid->rows(); ++row) {
            t.grid->readRow(screen, Ubiquitty::Row(row), rowBuffer);
            for (int column = 0; column < columns; ++column) {
                Pen pen = cellPen(rowBuffer[column]);
                Ubiquitty::penApplyNormalTextColors(&pen, assigned);
                cellSetPen(rowBuffer[column], pen);
            }
            t.grid->setRowSpan(screen, Ubiquitty::Row(row), Ubiquitty::Column(0), Ubiquitty::RowView { rowBuffer, columns });
        }
    }
}

} // namespace Ubiquitty::Screen
