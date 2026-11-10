// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <cstdint>
#include <limits>

namespace Ubiquitty::Screen {

/// The retained history as one output transition sees it.
///
/// Published beside every drain so a viewport policy can run without the
/// terminal. A held viewport moves by the growth of `appendedRows`, never of
/// `retainedRows`: at the history limit every appended row evicts one, so the
/// retained count stands still while the content still shifts a row per line.
struct HistoryFacts {
    /// Rows the view can scroll back over, zero on the alternate screen.
    int retainedRows = 0;
    /// Rows ever scrolled into history, monotonic through eviction and clears.
    std::int64_t appendedRows = 0;
    /// The coordinate space the rows are numbered in; a resize or a clear
    /// changes it, and rows counted under another epoch are not comparable.
    std::uint32_t absoluteRowEpoch = 0;
};

/// How a terminal numbered its absolute rows at one moment.
///
/// What a row index recorded at that moment needs to be read at another:
/// absolute rows count from the oldest retained history row, so eviction
/// renumbers every row while the text the rows hold stays put.
struct RowNumbering {
    /// `Screen::scrollbackCount`: the absolute row of the screen's top row, on
    /// either screen.
    int historyRows = 0;
    /// Rows ever scrolled into history, as ``HistoryFacts/appendedRows``.
    std::int64_t appendedRows = 0;
    /// As ``HistoryFacts/absoluteRowEpoch``.
    std::uint32_t absoluteRowEpoch = 0;
    /// Whether the alternate screen was the one shown.
    bool isAlternateScreen = false;

    friend bool operator==(const RowNumbering&, const RowNumbering&) = default;
};

/// Reads a row numbered under one numbering under another.
///
/// On the primary screen a row moves up by every row evicted between the two
/// moments: the rows appended less the growth of the history. On the alternate
/// screen the screen's rows are the rows, so a row keeps its place on the
/// screen as the history above it changes. Numberings under different epochs
/// or screens share no rows, and nothing translates between them.
///
/// - Parameters:
///   - absoluteRow: The row, numbered under `from`.
///   - from: The numbering the row was recorded under.
///   - to: The numbering to read it under.
///   - translated: Receives the row under `to`, which may lie outside the rows
///     `to` holds.
/// - Returns: Whether the two numberings share rows.
[[nodiscard]] constexpr bool translateAbsoluteRow(int absoluteRow, const RowNumbering& from, const RowNumbering& to, int& translated)
{
    if (from.absoluteRowEpoch != to.absoluteRowEpoch || from.isAlternateScreen != to.isAlternateScreen)
        return false;
    const std::int64_t historyGrowth = static_cast<std::int64_t>(to.historyRows) - from.historyRows;
    const std::int64_t evicted = from.isAlternateScreen ? -historyGrowth : (to.appendedRows - from.appendedRows) - historyGrowth;
    const std::int64_t row = static_cast<std::int64_t>(absoluteRow) - evicted;
    if (row < std::numeric_limits<int>::min() || row > std::numeric_limits<int>::max())
        return false;
    translated = static_cast<int>(row);
    return true;
}

} // namespace Ubiquitty::Screen
