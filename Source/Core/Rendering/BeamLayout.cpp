// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Source/Core/Rendering/BeamLayout.cpp — see BeamLayout.h.

#include "Core/Rendering/BeamLayout.h"

#include "Core/Pen.h"
#include "Core/VT/Constants.h"

#include <algorithm>
#include <string>

namespace Ubiquitty::Rendering {

namespace {

using CommandPalette::BeamRow;
using CommandPalette::BeamState;

// The chrome around the list: a border row top and bottom, the query line and
// the rule under it. The status line is counted only when there is a status —
// an always-present row that is usually blank reads as a rendering fault, and
// costs a line of the list on exactly the short grids that can least spare one.
constexpr int chromeRowsWithoutStatus = 4;
constexpr int minBodyRows = 1;
constexpr int minColumns = 24;
// Wide enough to read a command and its shortcut, narrow enough to stay a
// popover rather than a second screen.
constexpr int preferredColumns = 64;

// A packed 0xRRGGBB becomes a truecolor Pen — the same shape an SGR 38;2 cell
// carries, so no new render path is involved.
Pen penFor(std::uint32_t foreground, std::uint32_t background)
{
    Pen pen {};
    pen.foregroundIndex = colorRGB;
    pen.foregroundRed = static_cast<std::uint8_t>((foreground >> 16) & 0xFF);
    pen.foregroundGreen = static_cast<std::uint8_t>((foreground >> 8) & 0xFF);
    pen.foregroundBlue = static_cast<std::uint8_t>(foreground & 0xFF);
    pen.backgroundIndex = colorRGB;
    pen.backgroundRed = static_cast<std::uint8_t>((background >> 16) & 0xFF);
    pen.backgroundGreen = static_cast<std::uint8_t>((background >> 8) & 0xFF);
    pen.backgroundBlue = static_cast<std::uint8_t>(background & 0xFF);
    return pen;
}

// Truncates UTF-8 to a column budget on a code-point boundary, so a clipped
// title ends at a character rather than mid-sequence.
std::string elide(const std::string& text, int columns)
{
    if (columns <= 0)
        return {};
    if (static_cast<int>(text.size()) <= columns)
        return text;
    int cut = columns;
    while (cut > 0 && (static_cast<unsigned char>(text[static_cast<std::size_t>(cut)]) & 0xC0) == 0x80)
        --cut;
    return text.substr(0, static_cast<std::size_t>(cut));
}

} // namespace

BeamGeometry layoutBeam(const BeamState& state, const ChromeStyle& style, int gridRows, int gridColumns)
{
    BeamGeometry geometry;
    if (!state.isOpen || gridRows <= 0 || gridColumns <= 0)
        return geometry;

    // Always a margin, even on a grid narrower than the preferred width: a
    // popover flush to both edges is a band across the screen rather than
    // something floating above it, and the terminal behind it stops reading as
    // context. Four columns is the smallest gap that still reads as one.
    const int columns = std::min(gridColumns - 4, preferredColumns);
    const int chromeRows = chromeRowsWithoutStatus + (state.status.empty() ? 0 : 1);
    if (columns < minColumns || gridRows < chromeRows + minBodyRows)
        return geometry; // too small to be a palette; drawing a stub is worse

    // As many rows as fit, capped by what there is to show. The list never
    // reserves space it will not fill — an empty result set draws a short box
    // rather than a tall one full of nothing.
    // Exactly as many rows as there are, capped by what fits. Not floored at one:
    // a result set of none should draw its status and stop, and a reserved blank
    // line above "no matches" reads as a row that failed to render.
    // Half the grid at most, so the terminal behind stays legible. A list that
    // takes every row it can is not a popover — and the theme drill reaches that
    // the moment it opens, because Core ships more themes than a short grid has
    // rows. The cap yields to the grid on a short one, where half of very little
    // is worse than what fits.
    const int availableBody = gridRows - chromeRows;
    const int roomyBody = std::min(availableBody, std::max(1, gridRows / 2));
    const int bodyRows = std::clamp(static_cast<int>(state.rows.size()), 0, std::max(0, roomyBody));
    const int rows = bodyRows + chromeRows;

    geometry.bodyRows = bodyRows;
    geometry.canvas.resizeAndBlank(rows, columns);
    geometry.originColumn = (gridColumns - columns) / 2;
    // Upper third rather than centred: the palette should not cover the prompt
    // it was summoned from, and a list grows downward.
    geometry.originRow = std::min(gridRows / 6, std::max(0, gridRows - rows));

    const Pen surfacePen = penFor(style.text, style.surface);
    const Pen borderPen = penFor(style.border, style.surface);
    const Pen dimPen = penFor(style.dim, style.surface);
    const Pen accentPen = penFor(style.accent, style.surface);
    const Pen selectedPen = penFor(style.selectionText, style.selection);
    const Pen selectedAccentPen = penFor(style.accent, style.selection);

    fill(geometry.canvas, GridRect { 0, 0, columns, rows }, surfacePen);
    drawBox(geometry.canvas, GridRect { 0, 0, columns, rows }, BoxStyle::Rounded, borderPen);

    // The query line, and inside a drill the breadcrumb that says what is being
    // changed. Without it a value list is four words with no subject — the user
    // types "dark", sees three rows and has no way to tell what they belong to.
    //
    // The caret is a cell of the accent colour rather than a glyph, so it cannot
    // be mistaken for typed text and needs no blink clock.
    const int inner = columns - 4;
    int queryColumn = 2;
    if (!state.drillTitle.empty()) {
        // At most half the line, so a long command title cannot leave the value
        // being typed with nowhere to appear.
        const std::string crumb = elide(state.drillTitle, inner / 2);
        drawText(geometry.canvas, 1, queryColumn, crumb, accentPen);
        queryColumn += static_cast<int>(crumb.size());
        drawText(geometry.canvas, 1, queryColumn, " \u203A ", dimPen);
        queryColumn += 3;
    }
    const std::string prompt = elide(state.query, columns - 2 - queryColumn);
    drawText(geometry.canvas, 1, queryColumn, prompt, surfacePen);
    fill(geometry.canvas, GridRect { 1, queryColumn + static_cast<int>(prompt.size()), 1, 1 }, accentPen);

    // A rule under the query, joined to the border on both sides.
    for (int column = 1; column < columns - 1; ++column)
        if (Cell* cell = geometry.canvas.at(2, column))
            *cell = *geometry.canvas.at(0, column);
    drawText(geometry.canvas, 2, 0, "├", borderPen);
    drawText(geometry.canvas, 2, columns - 1, "┤", borderPen);

    // Rows, from the session's own scroll window so the selection is visible by
    // construction rather than by the layout guessing where it went.
    const int firstRow = std::clamp(state.scrollTop, 0, std::max(0, static_cast<int>(state.rows.size()) - 1));
    for (int line = 0; line < bodyRows; ++line) {
        const int rowIndex = firstRow + line;
        if (rowIndex >= static_cast<int>(state.rows.size()))
            break;
        const BeamRow& row = state.rows[static_cast<std::size_t>(rowIndex)];
        const bool isSelected = rowIndex == state.selectedIndex;
        const int canvasRow = 3 + line;

        if (isSelected)
            fill(geometry.canvas, GridRect { canvasRow, 1, columns - 2, 1 }, selectedPen);

        // A drill indents every row by the width of the check column, so the
        // titles line up whether or not one of them is ticked. Indenting only
        // the ticked row would make the list appear to jitter as the selection
        // moved.
        const int titleColumn = state.drillTitle.empty() ? 2 : 4;
        if (row.isChecked)
            drawText(geometry.canvas, canvasRow, 2, "\u2713", isSelected ? selectedAccentPen : accentPen);
        const int titleWidth = columns - 2 - titleColumn;
        drawText(geometry.canvas, canvasRow, titleColumn, elide(row.title, titleWidth),
            isSelected ? selectedPen : surfacePen);

        // Matched characters take the accent. Indices are code-point offsets, so
        // a highlight lands on the character the matcher matched rather than
        // mid-sequence — and an index past the title is ignored rather than
        // written, because on the wire they are attacker-controlled.
        for (const int matched : row.matchedIndices) {
            const int column = titleColumn + matched;
            if (matched < 0 || column >= columns - 2)
                continue;
            if (Cell* cell = geometry.canvas.at(canvasRow, column))
                cell->pen = isSelected ? selectedAccentPen : accentPen;
        }

        if (!row.detail.empty()) {
            const std::string detail = elide(row.detail, inner / 2);
            const int column = columns - 2 - static_cast<int>(detail.size());
            if (column > titleColumn + static_cast<int>(elide(row.title, titleWidth).size()))
                drawText(geometry.canvas, canvasRow, column, detail, isSelected ? selectedPen : dimPen);
        }
    }

    if (!state.status.empty())
        drawText(geometry.canvas, rows - 2, 2, elide(state.status, inner), dimPen);
    return geometry;
}

} // namespace Ubiquitty::Rendering
