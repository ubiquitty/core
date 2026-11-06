// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// HistoryLines — the address a history line keeps while the screen moves
// under it (Docs/sync.md §6.6).
//
// A history line is addressed by `(epoch, number)`. The number counts
// completed logical lines, not stored rows: a reflow re-wraps every line into a
// different count of rows, so a row count cannot name the same line across a
// resize, while a count of completed lines can.
//
// The invariants:
//
// - A row stored with `wrapped == false` completes one logical line; the line
//   takes the number `head` held before it completed, and `head` advances.
// - A line is below `head` only once its last row has left the screen. A line
//   whose earlier rows are in history and whose last row is still on screen
//   (it straddles the seam) is the open line: it will take the number `head`,
//   and the rows it has in history so far are `openLineRows`.
// - A reflow never renumbers a line. It moves `head` only as far as it moves
//   the seam, and it may advance `oldest` by whole lines, because the ring's
//   capacity is in rows and a narrower width spends more of them.
// - Eviction advances `oldest` by whole lines, and never the epoch.
// - A taller screen that pulls rows back retreats `head` by whole lines; a
//   later shrink pushes the same lines out under the same numbers.
// - `retireHistory` alone advances the epoch, and runs on the destructive
//   verbs only: ED 3, RIS, the clear-buffer verbs, the clear commands that
//   drop history, `dropScrollbackBefore`, `dropRowsBefore` and the clear to
//   the previous mark. Eviction, reflow and pull-back never call it. A verb
//   in an epoch where no line was numbered opens none: there is nothing a
//   client could hold, so a repeated clear at an empty prompt costs nothing.
// - `retireHistory` never rewinds a number. The lines that survive a partial
//   clear are renumbered from the highest number the old epoch reached, which
//   becomes the new epoch's `clearedThrough`; RIS carries that number across
//   the reconstruction. Numbers are therefore monotonic across epochs, and a
//   number below `clearedThrough` names nothing in the current epoch.

#include "Core/LineStore.h"

#include <cstdint>
#include <optional>

namespace Ubiquitty {
struct Cell;
struct Terminal;
}

namespace Ubiquitty::Screen {

struct ScrollLine;

/// Where the history of a terminal stands.
struct HistoryAddress {
    /// The epoch every number below is counted in.
    std::uint32_t epoch = 0;
    /// The oldest line whose first row is still retained.
    std::uint64_t oldest = 0;
    /// The number the next completed line takes; lines in `[oldest, head)` are
    /// history.
    std::uint64_t head = 0;
    /// The first number of this epoch. A line below it was cleared.
    std::uint64_t clearedThrough = 0;
};

[[nodiscard]] HistoryAddress historyAddress(const Ubiquitty::Terminal&);

/// The stored rows of line `number`, or nothing when it is not history: below
/// `oldest`, at or above `head`, or in a cleared epoch.
[[nodiscard]] std::optional<Ubiquitty::LineStore::LogicalLineRows> logicalLineRows(const Ubiquitty::Terminal&, std::uint64_t number);

/// The stored rows of the line `head` numbers when it completes: the first rows
/// of a line that straddles the seam, which have left the screen while its
/// last is still on it. Nothing when no line straddles, or its first rows were
/// evicted.
[[nodiscard]] std::optional<Ubiquitty::LineStore::LogicalLineRows> openLineRows(const Ubiquitty::Terminal&);

/// Whether a cell is one the program never wrote and nothing painted: what a
/// row's tail holds past its content, and what the Core leaves in a margin
/// column a wide character did not fit.
///
/// - Parameter cell: The cell.
/// - Returns: Whether the cell carries no glyph, no attribute and no colour.
[[nodiscard]] bool isUnwrittenCell(const Ubiquitty::Cell& cell);

/// The cells of a stored row that a logical line joins: its length less the
/// cells at its end the program never wrote, which include the pad a wide
/// character leaves at a right margin it did not fit. A line joined this way
/// is the same at every width.
///
/// - Parameter row: The row.
/// - Returns: The cells to join.
[[nodiscard]] int writtenLength(const ScrollLine& row);

/// Returns whether any line has been numbered since the current epoch opened.
///
/// A clear in an epoch where none was has nothing to purge, so it opens no
/// epoch.
[[nodiscard]] bool hasNumberedHistoryInEpoch(const Ubiquitty::Terminal&);

/// Opens a new history epoch after a destructive clear has run, unless no line
/// was numbered in the current one. The lines that survived the clear are
/// renumbered from the old epoch's highest number.
void retireHistory(Ubiquitty::Terminal&);

} // namespace Ubiquitty::Screen
