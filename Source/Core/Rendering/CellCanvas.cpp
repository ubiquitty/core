// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Source/Core/Rendering/CellCanvas.cpp — see CellCanvas.h.

#include "Core/Rendering/CellCanvas.h"

#include "Core/Pen.h"
#include "Core/Unicode/UTF8Decoder.h"
#include "Core/Unicode/Width.h"
#include "Core/VT/Constants.h"

namespace Ubiquitty::Rendering {

namespace {

// The six forms a border needs, per style, in one place: a caller cannot pair a
// heavy corner with a light rule because it never names the code points.
struct BoxGlyphs {
    std::uint32_t horizontal;
    std::uint32_t vertical;
    std::uint32_t topLeft;
    std::uint32_t topRight;
    std::uint32_t bottomLeft;
    std::uint32_t bottomRight;
};

constexpr BoxGlyphs glyphsFor(BoxStyle style)
{
    switch (style) {
    case BoxStyle::Light:
        return { 0x2500, 0x2502, 0x250C, 0x2510, 0x2514, 0x2518 };
    case BoxStyle::Heavy:
        return { 0x2501, 0x2503, 0x250F, 0x2513, 0x2517, 0x251B };
    case BoxStyle::Double:
        return { 0x2550, 0x2551, 0x2554, 0x2557, 0x255A, 0x255D };
    case BoxStyle::Rounded:
        // Light rules, rounded corners — the corners are the only difference,
        // and they join a light rule by design.
        return { 0x2500, 0x2502, 0x256D, 0x256E, 0x2570, 0x256F };
    }
    return { 0x2500, 0x2502, 0x250C, 0x2510, 0x2514, 0x2518 };
}

// Writes one code point with its pen, leaving the cluster and link fields clear.
// A canvas has no Terminal behind it, so it can carry neither a pool cluster id
// nor a link id — both index tables that do not exist here, and a cell carrying
// one would resolve against whatever table the destination happens to have.
void putGlyph(Cell& cell, std::uint32_t codePoint, const Pen& pen)
{
    cell.codePoint = codePoint;
    cell.pen = pen;
    cell.combining.fill(0);
    cell.clusterPacked = 0;
    cell.linkIDLo = 0;
    cell.linkIDHi = 0;
}

} // namespace

void CellCanvas::resizeAndBlank(int rowCount, int columnCount)
{
    rows = rowCount > 0 ? rowCount : 0;
    columns = columnCount > 0 ? columnCount : 0;
    cells.assign(static_cast<std::size_t>(rows) * static_cast<std::size_t>(columns), Cell {});
}

Cell* CellCanvas::at(int row, int column)
{
    if (row < 0 || column < 0 || row >= rows || column >= columns)
        return nullptr;
    return &cells[static_cast<std::size_t>(row) * static_cast<std::size_t>(columns) + static_cast<std::size_t>(column)];
}

const Cell* CellCanvas::at(int row, int column) const
{
    if (row < 0 || column < 0 || row >= rows || column >= columns)
        return nullptr;
    return &cells[static_cast<std::size_t>(row) * static_cast<std::size_t>(columns) + static_cast<std::size_t>(column)];
}

void fill(CellCanvas& canvas, GridRect rect, const Pen& pen)
{
    for (int row = rect.row; row < rect.row + rect.height; ++row)
        for (int column = rect.column; column < rect.column + rect.width; ++column)
            if (Cell* cell = canvas.at(row, column))
                putGlyph(*cell, 0, pen);
}

bool drawText(CellCanvas& canvas, int row, int column, std::string_view utf8, const Pen& pen)
{
    if (row < 0 || row >= canvas.rows)
        return utf8.empty();

    Unicode::UTF8Decoder decoder;
    Unicode::utf8Reset(&decoder);

    int pen_column = column;
    Cell* base = nullptr; // the cell a combining mark attaches to

    for (const char raw : utf8) {
        std::uint32_t codePoint = 0;
        const Unicode::UTF8Status status = Unicode::utf8Feed(&decoder, static_cast<std::uint8_t>(raw), &codePoint);
        if (status == Unicode::UTF8Status::Need)
            continue;
        if (status == Unicode::UTF8Status::Invalid)
            codePoint = 0xFFFD;

        // A canvas holds frontend chrome, which belongs to no terminal and so
        // measures ambiguous code points at the default.
        const int width = Unicode::Width::forCodePoint(codePoint, Unicode::Width::AmbiguousWidth::Narrow);
        if (width == 0) {
            // A combining mark belongs to the cell before it. With no base — a
            // string starting with one — it is dropped rather than given a cell
            // of its own, which would render as a mark floating on blank.
            if (base && base->combiningLength() < cellMaxCombining) {
                base->combining[base->combiningLength()] = codePoint;
                base->setCombiningLength(static_cast<std::uint8_t>(base->combiningLength() + 1));
            }
            continue;
        }

        // A wide glyph needs both halves inside the canvas. Drawing the lead
        // alone leaves an orphan the grid's own write path would have refused,
        // and every later pass then has to tolerate it.
        if (pen_column + width > canvas.columns)
            return false;

        Cell* cell = canvas.at(row, pen_column);
        if (!cell)
            return false;
        putGlyph(*cell, codePoint, pen);
        base = cell;

        if (width == 2) {
            cell->pen.attributes.add(VT::Attribute::Wide);
            if (Cell* continuation = canvas.at(row, pen_column + 1)) {
                putGlyph(*continuation, 0, pen);
                continuation->pen.attributes.add(VT::Attribute::WideContinuation);
            }
        }
        pen_column += width;
    }
    return true;
}

void drawBox(CellCanvas& canvas, GridRect rect, BoxStyle style, const Pen& pen)
{
    if (rect.width <= 0 || rect.height <= 0)
        return;
    const BoxGlyphs glyphs = glyphsFor(style);
    const int top = rect.row;
    const int bottom = rect.row + rect.height - 1;
    const int left = rect.column;
    const int right = rect.column + rect.width - 1;

    for (int column = left + 1; column < right; ++column) {
        if (Cell* cell = canvas.at(top, column))
            putGlyph(*cell, glyphs.horizontal, pen);
        // A one-row rect puts both rules on the same line; writing the second
        // would be a redundant store, not a different picture.
        if (bottom != top)
            if (Cell* cell = canvas.at(bottom, column))
                putGlyph(*cell, glyphs.horizontal, pen);
    }
    for (int row = top + 1; row < bottom; ++row) {
        if (Cell* cell = canvas.at(row, left))
            putGlyph(*cell, glyphs.vertical, pen);
        if (right != left)
            if (Cell* cell = canvas.at(row, right))
                putGlyph(*cell, glyphs.vertical, pen);
    }
    // Corners last, so a collapsed rect ends as corners rather than as rules —
    // a one-column box reads as a vertical run with caps, which is what a
    // reader expects from a border that thin.
    if (Cell* cell = canvas.at(top, left))
        putGlyph(*cell, glyphs.topLeft, pen);
    if (Cell* cell = canvas.at(top, right))
        putGlyph(*cell, right == left ? glyphs.topLeft : glyphs.topRight, pen);
    if (Cell* cell = canvas.at(bottom, left))
        putGlyph(*cell, bottom == top ? glyphs.topLeft : glyphs.bottomLeft, pen);
    if (Cell* cell = canvas.at(bottom, right))
        putGlyph(*cell, (bottom == top || right == left) ? glyphs.topLeft : glyphs.bottomRight, pen);
}

bool hasDrawnChrome(ChromeLayers chrome)
{
    for (const ChromePlacement& layer : chrome) {
        if (layer.canvas != nullptr && layer.canvas->rows > 0)
            return true;
    }
    return false;
}

void composite(Cell* destination, int destinationRows, int destinationColumns,
    const CellCanvas& canvas, int atRow, int atColumn)
{
    if (!destination || destinationRows <= 0 || destinationColumns <= 0)
        return;
    for (int row = 0; row < canvas.rows; ++row) {
        const int destinationRow = atRow + row;
        if (destinationRow < 0 || destinationRow >= destinationRows)
            continue;
        for (int column = 0; column < canvas.columns; ++column) {
            const int destinationColumn = atColumn + column;
            if (destinationColumn < 0 || destinationColumn >= destinationColumns)
                continue;
            const Cell* source = canvas.at(row, column);
            if (!source)
                continue;
            destination[static_cast<std::size_t>(destinationRow) * static_cast<std::size_t>(destinationColumns)
                + static_cast<std::size_t>(destinationColumn)]
                = *source;
        }
    }
}

} // namespace Ubiquitty::Rendering
