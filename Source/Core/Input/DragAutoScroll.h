// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>

namespace Ubiquitty::Input {

/// What a selection drag held outside the grid asks of the view.
struct DragAutoScrollStep {
    /// Rows to move the view by: positive walks up into history, negative back
    /// down toward the tail. Zero when the pointer is over the grid, which is
    /// also the answer that says the drag needs no help.
    int lines = 0;
    /// Whether the pointer is outside the grid at all. Distinct from a zero
    /// step only in principle — they agree — and named separately because the
    /// caller's question is "keep the timer running?", not "how far".
    bool isOutsideGrid = false;
};

/// How far a drag held past an edge moves the view, per tick.
///
/// A drag-select that runs off the top or bottom of the window has to keep
/// selecting, and the rows it wants are not on screen. Every frontend that
/// supports the gesture needs the same arithmetic and the same edge cases —
/// the step is proportional to how far outside the pointer is, so flicking
/// well past the edge scrolls faster than easing over it — so the arithmetic
/// is here rather than in each of them.
///
/// The row must be UNCLAMPED: a mapping that pins the pointer to the first or
/// last row cannot tell "at the edge" from "past it", and a drag that leaves
/// the window then reads as a drag resting on its last row. That is the whole
/// reason this takes a signed row rather than a cell position.
///
/// - Parameters:
///   - signedRow: The row under the pointer, negative above the grid and at or
///     beyond `rows` below it.
///   - rows: The grid's visible row count.
/// - Returns: The step, zero when the pointer is over the grid or the grid has
///   no rows.
[[nodiscard]] constexpr DragAutoScrollStep dragAutoScrollStep(int signedRow, int rows)
{
    DragAutoScrollStep step;
    if (rows <= 0)
        return step;
    if (signedRow < 0) {
        step.lines = -signedRow;
        step.isOutsideGrid = true;
    } else if (signedRow >= rows) {
        step.lines = rows - 1 - signedRow;
        step.isOutsideGrid = true;
    }
    return step;
}

} // namespace Ubiquitty::Input
