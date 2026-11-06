// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Rendering/StatusRow.h"
#include "Core/Terminal.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <memory>

namespace Ubiquitty::Rendering {

StatusRowCells ensureStatusRow(Ubiquitty::Terminal& t)
{
    if (!t.statusRow) [[unlikely]]
        t.statusRow = std::make_unique<StatusRowStorage>();
    return t.statusRow->cells;
}

std::span<const Cell> statusRow(const Ubiquitty::Terminal& t)
{
    if (!t.statusRow)
        return {};
    return t.statusRow->cells;
}

const Cell& statusCellAt(const Ubiquitty::Terminal& t, int column)
{
    // A negative column wraps above the bound, so one compare covers both ends.
    if (!t.statusRow || static_cast<unsigned>(column) >= static_cast<unsigned>(Ubiquitty::maxColumns))
        return defaultBlankCell;
    return t.statusRow->cells[static_cast<std::size_t>(column)];
}

void releaseStatusRow(Ubiquitty::Terminal& t) { t.statusRow.reset(); }

bool fillStatusRow(const Ubiquitty::Terminal* t, int rows, int columns, int viewOffset, StatusRowCells output UBIQUITTY_NOESCAPE)
{
    if (!t || rows <= 0 || columns <= 0 || viewOffset != 0)
        return false;
    if (output.size() < static_cast<std::size_t>(columns))
        return false;
    if (t->statusLineType == 2) {
        // An unwritten cell is the canonical blank, not the array's zero state.
        // Resolved once: this runs per column, per frame.
        const std::span<const Cell> row = statusRow(*t);
        if (row.empty()) {
            std::fill_n(output.begin(), static_cast<std::size_t>(columns), defaultBlankCell);
            return true;
        }
        for (int c = 0; c < columns; ++c) {
            const Cell& src = row[static_cast<std::size_t>(c)];
            output[static_cast<std::size_t>(c)] = (src.codePoint != 0) ? src : defaultBlankCell;
        }
        return true;
    }
    if (t->statusLineType == 1) {
        std::fill_n(output.begin(), static_cast<std::size_t>(columns), defaultBlankCell);
        // Terminal-drawn indicator: 1-based cursor line/column plus the grid
        // size, from the active screen's cursor.
        int cursorLine = 1;
        int cursorColumn = 1;
        if (t->screens.active) {
            cursorLine = t->screens.active->cursorY + 1;
            cursorColumn = t->screens.active->cursorX + 1;
        }
        std::array<char, 80> indicator {};
        int length = std::snprintf(indicator.data(), indicator.size(), "Line %d, Column %d  (%dx%d)", cursorLine, cursorColumn, columns, rows);
        if (length < 0)
            length = 0;
        // Truncated to the visible width rather than wrapped: the status line
        // is one row by definition.
        length = std::min(length, columns);
        for (int c = 0; c < length; ++c) {
            Cell cell = defaultBlankCell;
            cell.codePoint = static_cast<uint32_t>(static_cast<unsigned char>(indicator[c]));
            output[static_cast<std::size_t>(c)] = cell;
        }
        return true;
    }
    return false;
}

} // namespace Ubiquitty::Rendering
