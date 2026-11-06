// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
// The paint heatmap's policy: what bumps the counters, and how a window over
// them behaves. PerfCounters holds the counters; every frontend that shows a
// heatmap reads this rather than deciding either half itself.
//
// Declared and defined in every build. A frontend's importer need not share the
// C++ build's UBIQUITTY_PERF_DIAG — the Swift importer sees none of its NDEBUG —
// so nothing here changes shape with the gate: under UBIQUITTY_PERF_DIAG=0 every
// entry is inert, `isPerfDiagnosticsBuild()` says so, and the capture carries no
// heat pass at all.

#include "Base/SwiftInterop.h"

#include <cstdint>
#include <vector>

namespace Ubiquitty {
struct Cell;
}

namespace Ubiquitty::Diagnostics {

/// Whether this build carries the paint heatmap and the drawing-debug panel.
///
/// Defined in a translation unit compiled with Core's own flags, so the answer
/// is the build's rather than whatever the caller's importer assumed.
///
/// - Returns: True when UBIQUITTY_PERF_DIAG compiled the diagnostics in.
[[nodiscard]] bool isPerfDiagnosticsBuild();

/// Where a heatmap window stands after one refresh.
struct UBIQUITTY_ESCAPABLE PaintHeatmapExtent {
    /// The rows drawn: the live grid's, clamped to the counter grid.
    int rows = 0;
    /// The columns drawn: the live grid's, clamped to the counter grid.
    int columns = 0;
    /// The content width the window takes, one square per column.
    double widthInPoints = 0.0;
    /// The content height the window takes, one square per row.
    double heightInPoints = 0.0;
};

/// The content key each cell carried in the last captured frame, so the heat
/// counter bumps only where content changed.
///
/// The renderer redraws every cell of a frame, so counting painted cells shows
/// a uniform pulse; counting cells whose key differs from the last frame's
/// shows where output churns. Keyed on the captured cell rather than the
/// resolved pen: the map answers "did this cell change", and a palette swap or
/// reverse video moves every pen without changing any content.
///
/// Enabling resyncs: the shadow forgets every key and the counters clear, and
/// the first frame after that records its keys without bumping, so it is a
/// baseline rather than a whole-grid flash.
class UBIQUITTY_ESCAPABLE PaintHeatShadow {
public:
    /// Starts watching, from a fresh baseline and cleared counters.
    void enable();

    /// Stops watching and releases the shadow's storage.
    void disable();

    /// Whether captures are being compared.
    [[nodiscard]] bool isEnabled() const { return m_isEnabled; }

    /// Compares a captured frame against the last one and bumps the counter of
    /// every cell whose content differs.
    ///
    /// A frame whose grid changed shape keeps the keys of the cells both
    /// shapes share; a cell the new shape adds has no key yet and bumps on
    /// sight, since it is content the reader has not seen there before.
    ///
    /// - Parameters:
    ///   - cells: The captured cells, row-major, `rows * columns` of them.
    ///   - rows: The captured rows.
    ///   - columns: The captured columns.
    void noteCapturedGrid(const Cell* cells, int rows, int columns);

    /// The key a cell is compared by: a mix of every byte the cell holds, so
    /// a change to its character, cluster, pen or link reads as a change.
    ///
    /// - Parameter cell: The captured cell.
    /// - Returns: The key, never zero, which the shadow keeps for "unseen".
    [[nodiscard]] static std::uint64_t contentKey(const Cell& cell);

private:
    std::vector<std::uint64_t> m_keys;
    int m_rows = 0;
    int m_columns = 0;
    bool m_isEnabled = false;
    bool m_hasBaseline = false;
};

namespace PaintHeatmap {

/// The side of one cell's square in a heatmap window.
[[nodiscard]] constexpr double cellSizeInPoints() { return 6.0; }

/// How often a heatmap window refreshes: decay and redraw.
[[nodiscard]] constexpr double refreshIntervalSeconds() { return 0.05; }

/// Clears the counters for a window that is opening, so the picture reflects
/// activity since it opened rather than history nobody was watching.
void noteWindowOpened();

/// One refresh of a heatmap window over a live grid.
///
/// Decays every counter inside the grid, zeroes the ones a shrink left
/// outside it so they stop bleeding into the visible area, and returns the
/// extent the window takes, which tracks the grid across a DECCOLM switch or a
/// resize.
///
/// - Parameters:
///   - liveRows: The grid's rows now.
///   - liveColumns: The grid's columns now.
/// - Returns: The extent to draw.
[[nodiscard]] PaintHeatmapExtent refreshWindow(int liveRows, int liveColumns);

/// The colour a cell's square is painted, packed 0x00RRGGBB.
///
/// - Parameters:
///   - row: The cell's row.
///   - column: The cell's column.
/// - Returns: The counter's colour on the heat ramp.
[[nodiscard]] std::uint32_t cellColor(int row, int column);

/// A cell's counter, for a reader checking what the map holds.
///
/// - Parameters:
///   - row: The cell's row.
///   - column: The cell's column.
/// - Returns: The counter; zero outside the counter grid.
[[nodiscard]] std::uint8_t cellCount(int row, int column);

} // namespace PaintHeatmap

} // namespace Ubiquitty::Diagnostics
