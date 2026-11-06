// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_cell_canvas — the off-screen cell surface terminal chrome is drawn into.
//
// Every primitive here clips, and clipping is where a drawing API is wrong in
// ways that look right: a rect one cell too wide draws over a border, a wide
// glyph split at the edge leaves an orphan the grid's own write path rejects,
// and a composite off the top writes above row 0 into whatever follows.
//
// So the negatives come first, and each is reached from one positive fixture by
// changing a single field. A case that draws nothing for two reasons at once
// cannot say which clip held.

#include "Harness.h"

#include "Core/Pen.h"
#include "Core/Rendering/CellCanvas.h"
#include "Core/VT/Constants.h"

#include <string>
#include <vector>

using Ubiquitty::Cell;
using Ubiquitty::Pen;
using Ubiquitty::Rendering::BoxStyle;
using Ubiquitty::Rendering::CellCanvas;
using Ubiquitty::Rendering::composite;
using Ubiquitty::Rendering::drawBox;
using Ubiquitty::Rendering::drawText;
using Ubiquitty::Rendering::fill;
using Ubiquitty::Rendering::GridRect;

namespace {

Pen inkPen()
{
    Pen pen {};
    pen.foregroundIndex = 7;
    pen.backgroundIndex = 4;
    return pen;
}

std::uint32_t glyphAt(const CellCanvas& canvas, int row, int column)
{
    const Cell* cell = canvas.at(row, column);
    return cell ? cell->codePoint : 0xFFFFFFFFu;
}

} // namespace

TEST(cellCanvasClipsEveryPrimitive)
{
    // The positive fixture every negative below subtracts from.
    CellCanvas canvas;
    canvas.resizeAndBlank(6, 12);
    CHECK_EQ(canvas.rows, 6);
    CHECK_EQ(canvas.columns, 12);
    CHECK_MSG(canvas.at(0, 0) != nullptr, "the canvas has cells");
    CHECK_MSG(canvas.at(6, 0) == nullptr, "and reports its own bounds rather than trusting the caller");
    CHECK_MSG(canvas.at(0, 12) == nullptr, "on both axes");
    CHECK_MSG(canvas.at(-1, 0) == nullptr, "including negatives, which an unsigned index would wrap");

    // fill, clipped on the far edges. Nothing outside is touched and nothing
    // faults — a rect hanging off the canvas is a caller placing a popover near
    // an edge, not a bug.
    fill(canvas, GridRect { 4, 9, 99, 99 }, inkPen());
    CHECK_EQ(static_cast<int>(canvas.at(5, 11)->pen.backgroundIndex), 4);
    CHECK_EQ(static_cast<int>(canvas.at(3, 9)->pen.backgroundIndex), 0);

    // A rect entirely outside writes nothing at all.
    CellCanvas untouched;
    untouched.resizeAndBlank(6, 12);
    fill(untouched, GridRect { 20, 20, 4, 4 }, inkPen());
    for (int row = 0; row < untouched.rows; ++row)
        for (int column = 0; column < untouched.columns; ++column)
            CHECK_EQ(static_cast<int>(untouched.at(row, column)->pen.backgroundIndex), 0);
}

TEST(cellCanvasDrawsTextAndRefusesToHalveAWideGlyph)
{
    CellCanvas canvas;
    canvas.resizeAndBlank(3, 10);

    CHECK_MSG(drawText(canvas, 1, 0, "hi", inkPen()), "text that fits reports so");
    CHECK_EQ(static_cast<int>(glyphAt(canvas, 1, 0)), static_cast<int>('h'));
    CHECK_EQ(static_cast<int>(glyphAt(canvas, 1, 1)), static_cast<int>('i'));
    CHECK_MSG(glyphAt(canvas, 0, 0) == 0, "and does not touch another row");

    // A double-width glyph takes two cells and marks the pair, so the grid's own
    // wide-pair invariant holds on a canvas too.
    {
        CellCanvas wide;
        wide.resizeAndBlank(1, 4);
        CHECK(drawText(wide, 0, 0, "\xE6\xBC\xA2", inkPen())); // U+6F22
        CHECK_EQ(static_cast<int>(glyphAt(wide, 0, 0)), 0x6F22);
        CHECK_MSG(wide.at(0, 0)->pen.attributes.contains(Ubiquitty::VT::Attribute::Wide),
            "the lead is marked wide");
        CHECK_MSG(wide.at(0, 1)->pen.attributes.contains(Ubiquitty::VT::Attribute::WideContinuation),
            "and the cell after it is its continuation");
    }

    // The negative that matters: the same glyph one column further right, where
    // only its second half would fall off. It is dropped whole rather than
    // half-drawn — an orphaned lead is a state the grid's write path refuses,
    // and every later pass would have to tolerate it.
    {
        CellCanvas edge;
        edge.resizeAndBlank(1, 4);
        CHECK_MSG(!drawText(edge, 0, 3, "\xE6\xBC\xA2", inkPen()),
            "a wide glyph straddling the last column reports that it did not fit");
        CHECK_MSG(glyphAt(edge, 0, 3) == 0, "and writes nothing, rather than an orphaned lead");
    }

    // A narrow glyph in that same last column does fit — otherwise the case
    // above would pass for the wrong reason, on a canvas too small.
    {
        CellCanvas edge;
        edge.resizeAndBlank(1, 4);
        CHECK(drawText(edge, 0, 3, "x", inkPen()));
        CHECK_EQ(static_cast<int>(glyphAt(edge, 0, 3)), static_cast<int>('x'));
    }

    // Text does not wrap. Chrome that wrapped would reflow its own border.
    {
        CellCanvas narrow;
        narrow.resizeAndBlank(2, 3);
        CHECK_MSG(!drawText(narrow, 0, 0, "abcdef", inkPen()), "an overlong string reports clipping");
        CHECK_MSG(glyphAt(narrow, 1, 0) == 0, "and never continues onto the next row");
    }

    // A combining mark attaches to its base rather than taking a cell.
    {
        CellCanvas marked;
        marked.resizeAndBlank(1, 4);
        CHECK(drawText(marked, 0, 0, "e\xCC\x81", inkPen())); // e + U+0301
        CHECK_EQ(static_cast<int>(glyphAt(marked, 0, 0)), static_cast<int>('e'));
        CHECK_EQ(static_cast<int>(marked.at(0, 0)->combiningLength()), 1);
        CHECK_MSG(glyphAt(marked, 0, 1) == 0, "the mark did not consume a cell of its own");
    }

    // A leading combining mark has no base. It is dropped rather than floating.
    {
        CellCanvas orphan;
        orphan.resizeAndBlank(1, 4);
        CHECK(drawText(orphan, 0, 0, "\xCC\x81", inkPen()));
        CHECK_MSG(glyphAt(orphan, 0, 0) == 0, "a mark with nothing to attach to draws nothing");
    }
}

TEST(cellCanvasDrawsBordersThatCollapseCleanly)
{
    CellCanvas canvas;
    canvas.resizeAndBlank(5, 8);
    drawBox(canvas, GridRect { 0, 0, 8, 5 }, BoxStyle::Light, inkPen());

    CHECK_EQ(static_cast<int>(glyphAt(canvas, 0, 0)), 0x250C);
    CHECK_EQ(static_cast<int>(glyphAt(canvas, 0, 7)), 0x2510);
    CHECK_EQ(static_cast<int>(glyphAt(canvas, 4, 0)), 0x2514);
    CHECK_EQ(static_cast<int>(glyphAt(canvas, 4, 7)), 0x2518);
    CHECK_EQ(static_cast<int>(glyphAt(canvas, 0, 3)), 0x2500);
    CHECK_EQ(static_cast<int>(glyphAt(canvas, 2, 0)), 0x2502);
    CHECK_MSG(glyphAt(canvas, 2, 3) == 0, "the interior is left alone, so a border can be drawn over content");

    // Style changes every form together. A caller cannot pair a heavy corner
    // with a light rule, because it never names code points.
    {
        CellCanvas heavy;
        heavy.resizeAndBlank(3, 3);
        drawBox(heavy, GridRect { 0, 0, 3, 3 }, BoxStyle::Heavy, inkPen());
        CHECK_EQ(static_cast<int>(glyphAt(heavy, 0, 0)), 0x250F);
        CHECK_EQ(static_cast<int>(glyphAt(heavy, 0, 1)), 0x2501);
    }
    {
        CellCanvas rounded;
        rounded.resizeAndBlank(3, 3);
        drawBox(rounded, GridRect { 0, 0, 3, 3 }, BoxStyle::Rounded, inkPen());
        CHECK_EQ(static_cast<int>(glyphAt(rounded, 0, 0)), 0x256D);
        CHECK_MSG(glyphAt(rounded, 0, 1) == 0x2500, "rounded uses LIGHT rules — its corners join them");
    }

    // Height 1: both rules land on one row. The degenerate case, and the one a
    // border routine usually draws twice or draws inverted.
    {
        CellCanvas thin;
        thin.resizeAndBlank(3, 5);
        drawBox(thin, GridRect { 1, 0, 5, 1 }, BoxStyle::Light, inkPen());
        CHECK_EQ(static_cast<int>(glyphAt(thin, 1, 2)), 0x2500);
        CHECK_MSG(glyphAt(thin, 0, 2) == 0 && glyphAt(thin, 2, 2) == 0,
            "a one-row border stays on its row");
    }

    // A zero-size rect draws nothing. Half-open bounds are what make this true;
    // an inclusive bound would draw one cell here.
    {
        CellCanvas empty;
        empty.resizeAndBlank(3, 3);
        drawBox(empty, GridRect { 1, 1, 0, 0 }, BoxStyle::Light, inkPen());
        for (int row = 0; row < 3; ++row)
            for (int column = 0; column < 3; ++column)
                CHECK_MSG(glyphAt(empty, row, column) == 0, "a zero-size border is no border");
    }
}

TEST(cellCanvasCompositesClippedOnEverySide)
{
    CellCanvas canvas;
    canvas.resizeAndBlank(2, 2);
    drawText(canvas, 0, 0, "ab", inkPen());
    drawText(canvas, 1, 0, "cd", inkPen());

    const int rows = 4, columns = 6;
    std::vector<Cell> destination(static_cast<std::size_t>(rows) * columns, Cell {});
    const auto glyph = [&](int row, int column) {
        return destination[static_cast<std::size_t>(row) * columns + column].codePoint;
    };

    composite(destination.data(), rows, columns, canvas, 1, 2);
    CHECK_EQ(static_cast<int>(glyph(1, 2)), static_cast<int>('a'));
    CHECK_EQ(static_cast<int>(glyph(2, 3)), static_cast<int>('d'));
    CHECK_MSG(glyph(0, 0) == 0, "and nothing outside the placement moved");

    // Off the top-left: only the overlapping quadrant lands, and nothing is
    // written before the buffer. An unclipped composite would corrupt whatever
    // precedes it, which no assertion inside the grid can see.
    {
        std::vector<Cell> guard(static_cast<std::size_t>(rows) * columns + 2, Cell {});
        guard[0].codePoint = 0xDEAD;
        composite(guard.data() + 1, rows, columns, canvas, -1, -1);
        CHECK_MSG(guard[0].codePoint == 0xDEAD, "nothing was written before the destination");
        CHECK_EQ(static_cast<int>(guard[1].codePoint), static_cast<int>('d'));
    }

    // Entirely outside writes nothing.
    {
        std::vector<Cell> clear(static_cast<std::size_t>(rows) * columns, Cell {});
        composite(clear.data(), rows, columns, canvas, 40, 40);
        for (const Cell& cell : clear)
            CHECK_MSG(cell.codePoint == 0, "a placement off the grid draws nothing");
    }

    // A null destination is a no-op rather than a fault: a frontend with no
    // frame yet is a real state, and faulting on it would be a crash in the
    // path that exists to draw an error message.
    composite(nullptr, rows, columns, canvas, 0, 0);
}

UBIQUITTY_TEST_MAIN()
