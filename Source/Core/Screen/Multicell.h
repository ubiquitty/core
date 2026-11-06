// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Which run a cell belongs to, asked position-first.
//
// A cell that draws larger than one cell is stored as a leading cell plus the
// cells it covers, and a reader that has a position needs the run rather than
// the cell: to paint it, to widen a selection to it, or to assert that no
// editing control left half of one behind. Asking "what covers this position"
// keeps that question answerable without knowing which mechanism produced the
// run — a DEC double-width line and a kitty-sized cluster differ in how they are
// set, not in what covers a cell.

#include "Forward.h"

namespace Ubiquitty::Screen {

/// The extent a run claims, measured in cells.
struct MulticellSpan {
    /// Columns the run occupies, at least 1.
    int columns = 1;
    /// Rows the run occupies, at least 1.
    int rows = 1;
};

/// The run covering a position.
struct MulticellRun {
    /// The run's leading cell, or -1 when no run covers the position.
    int anchorRow = -1;
    /// The leading cell's column, or -1.
    int anchorColumn = -1;
    /// What the anchor claims from there.
    MulticellSpan span;

    /// - Returns: True when a run covers the position this was resolved for.
    [[nodiscard]] constexpr bool isValid() const { return anchorRow >= 0 && anchorColumn >= 0; }

    /// - Returns: True when the position asked about is the run's leading cell.
    [[nodiscard]] constexpr bool isAnchor(int row, int column) const
    {
        return isValid() && row == anchorRow && column == anchorColumn;
    }

    /// - Returns: True when the run's claim reaches `row`, `column`.
    [[nodiscard]] constexpr bool covers(int row, int column) const
    {
        return isValid() && row >= anchorRow && row < anchorRow + span.rows && column >= anchorColumn
            && column < anchorColumn + span.columns;
    }
};

/// The run covering a visible cell.
///
/// A point query walks left from `column` to find the leading cell, so it costs
/// the run's width. **A reader crossing a whole row must track the last anchor it
/// passed instead of calling this per cell** — the walk has already gone by the
/// anchor, so remembering it is constant work where asking again is quadratic in
/// the run width, and the sizing protocol admits runs far wider than the two
/// columns a CJK pair takes. ``RowFlag::HasMulticell`` says whether a row needs
/// the tracking loop at all; almost none do, and the tracked loop is slower than
/// the plain one.
///
/// - Parameters:
///   - t: The terminal to read. Null yields an invalid run.
///   - row: A visible row, in [0, rows).
///   - column: A visible column, in [0, columns).
/// - Returns: The covering run, or an invalid run when the cell stands alone.
[[nodiscard]] MulticellRun multicellRunAt(const Ubiquitty::Terminal* t, int row, int column);

} // namespace Ubiquitty::Screen
