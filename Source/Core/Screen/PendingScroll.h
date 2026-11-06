// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/StrongIndex.h"

namespace Ubiquitty::Screen {

// PendingScroll: PAL must blit the BB region [topRow..botRow] by
// shiftRows. Positive shift = content moves up (top of region exposes
// blanks at bottom). CellGrid has already shifted cells_; m_bbShadow
// shifts on AckScroll so it stays in sync with BB pixels.
struct PendingScroll {
    Row topRow;
    Row botRow;
    int shiftRows; // > 0 up; < 0 down
};

// An inclusive row range; firstRow > lastRow means the range is empty.
struct ScrollExposedRows {
    int firstRow;
    int lastRow;
    [[nodiscard]] bool isEmpty() const { return firstRow > lastRow; }
};

// The rows within [topRow..botRow] left NEWLY EXPOSED (blank) by a shift of
// `shiftRows`: content moving up (shiftRows > 0) exposes the bottom |shift| rows;
// content moving DOWN (shiftRows < 0) exposes the top |shift| rows; |shift| at or
// beyond the region height exposes the whole region. Pure arithmetic, so it is
// host-tested (test_pending_scroll) and shared by the snapshot capture, which must
// mark these rows dirty: the macOS scroll blit clears the whole back-texture and
// repaints the exposed band only through the per-row dirty mask, but the
// incremental dirty runs can drop those rows on scroll coalescing / a snapshot
// re-encode — leaving the cleared band unpainted ("missing lines" at the top/
// top-left). Marking the exposed band here makes its repaint independent of the
// dirty-run path.
inline ScrollExposedRows scrollExposedRows(int topRow, int botRow, int shiftRows)
{
    if (shiftRows == 0 || topRow > botRow)
        return { topRow, topRow - 1 }; // empty
    const int magnitude = shiftRows > 0 ? shiftRows : -shiftRows;
    const int regionHeight = botRow - topRow + 1;
    if (magnitude >= regionHeight)
        return { topRow, botRow }; // whole region exposed (no in-place shift survives)
    if (shiftRows > 0)
        return { botRow - magnitude + 1, botRow }; // content up -> blanks at the bottom
    return { topRow, topRow + magnitude - 1 }; // content down -> blanks at the top
}

} // namespace Ubiquitty::Screen
