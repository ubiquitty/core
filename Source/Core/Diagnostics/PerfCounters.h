// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
// Portable data model behind the paint-frequency heatmap. Color is returned as
// a packed 24-bit RGB value (0x00RRGGBB) — no platform color struct crosses this
// seam. Everything sits behind UBIQUITTY_PERF_DIAG (compiles to nothing when off).

#include "Base/Types.h"
#include "Core/Diagnostics/PerfDiag.h"

#if UBIQUITTY_PERF_DIAG

namespace Ubiquitty::Diagnostics::PerfCounters {

// Counter grid size. Not the core's cap, which is 1024 x 1024
// (Core/GridBounds.h) — this covers the first 80 rows and 240 columns and a
// terminal can be larger than that in both directions.
//
// A cell outside it is not measured, and an unmeasured cell reads on the heatmap
// exactly like one that was never painted: black, which the legend calls cheap.
// `unmeasuredPaints()` is what separates the two, and a reader who does not
// consult it is looking at a partial map with no marking to say so.
//
// Sizing to the core cap would cost 1 MiB here and twenty times the frontend's
// shadow buffer, in a build that already exists only to be measured — worth
// doing if the blind region ever matters, and a decision for whoever owns the
// heatmap rather than a silent default.
enum {
    MaxRows = 80,
    MaxColumns = 240
};

// Per-paint counter increment (~32 overpaint levels before saturating at 255).
enum {
    PaintBump = 8
};

// Bump the per-cell paint counter, saturating at 255.
//
// A row or column outside the counter grid is counted as unmeasured rather than
// dropped, because the heatmap cannot otherwise tell "never painted" from "never
// looked at".
void noteCellPaint(int row, int column);

/// Paints that fell outside the counter grid since the last `reset`.
///
/// Paints rather than cells: one out-of-grid cell painted twice counts twice,
/// because the question this answers is how much of the heatmap's evidence was
/// discarded, not how much of the grid is uncovered.
///
/// Non-zero means the map is partial — the terminal is taller or wider than
/// `MaxRows` x `MaxColumns`, and everything past that edge is black for want of
/// a counter rather than for want of painting.
///
/// - Returns: The count, saturating rather than wrapping.
[[nodiscard]] uint64_t unmeasuredPaints();

// Read a cell's current counter. Out-of-range row/column returns 0.
uint8_t snapshotCell(int row, int column);

// Decay every counter inside the active grid by 10% (v -> v*9/10). Cells outside
// the active grid are left untouched (frontend zeroes those via zeroOutsideGrid);
// extents are clamped to [0, Max*].
void decay(int gridRows, int gridColumns);

// Zero counters that fell outside the active grid so a shrink stops
// bleeding decayed colors into the visible area.
void zeroOutsideGrid(int gridRows, int gridColumns);

// Clear every counter.
void reset();

// Map a 0..255 counter to a packed 0x00RRGGBB color: 0 is near-black, then
// blue -> cyan -> green -> yellow -> red -> white.
uint32_t colorFor(uint8_t count);

} // namespace Ubiquitty::Diagnostics::PerfCounters

#endif // UBIQUITTY_PERF_DIAG
