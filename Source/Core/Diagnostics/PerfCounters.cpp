// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Portable heatmap counter model: a byte-per-cell grid, a decay step, and a
// count -> packed-RGB gradient. See PerfCounters.h.

#include "Core/Diagnostics/PerfCounters.h"

#if UBIQUITTY_PERF_DIAG

namespace Ubiquitty {
namespace Diagnostics {
namespace PerfCounters {

namespace {

uint8_t gPaintCount[MaxRows][MaxColumns];
// Saturating rather than wrapping: a wrapped count that lands on zero is the
// same reading as a complete map, which is the thing this exists to distinguish.
uint64_t unmeasuredPaintCount = 0;

void clampExtent(int& rows, int& columns)
{
    if (rows < 0)
        rows = 0;
    if (columns < 0)
        columns = 0;
    if (rows > MaxRows)
        rows = MaxRows;
    if (columns > MaxColumns)
        columns = MaxColumns;
}

// Linear-interpolate from a to b over a 0..segmentMaximum span.
uint16_t lerp16(uint16_t a, uint16_t b, int t, int segmentMaximum)
{
    int delta = static_cast<int>(b) - static_cast<int>(a);
    int v = static_cast<int>(a) + (delta * t) / segmentMaximum;
    if (v < 0)
        v = 0;
    if (v > 0xFFFF)
        v = 0xFFFF;
    return static_cast<uint16_t>(v);
}

// Gradient is authored against 16-bit channels; pack to 8-bit-per-channel RGB
// by taking the high byte.
uint32_t pack(uint16_t r16, uint16_t g16, uint16_t b16)
{
    uint32_t r = (r16 >> 8) & 0xFF;
    uint32_t g = (g16 >> 8) & 0xFF;
    uint32_t b = (b16 >> 8) & 0xFF;
    return (r << 16) | (g << 8) | b;
}

} // namespace

void noteCellPaint(int row, int column)
{
    if (row < 0 || row >= MaxRows || column < 0 || column >= MaxColumns) {
        if (unmeasuredPaintCount != UINT64_MAX)
            unmeasuredPaintCount++;
        return;
    }
    uint32_t v = gPaintCount[row][column] + static_cast<uint32_t>(PaintBump);
    gPaintCount[row][column] = static_cast<uint8_t>(v > 255U ? 255U : v);
}

uint8_t snapshotCell(int row, int column)
{
    if (row < 0 || row >= MaxRows)
        return 0;
    if (column < 0 || column >= MaxColumns)
        return 0;
    return gPaintCount[row][column];
}

void decay(int gridRows, int gridColumns)
{
    clampExtent(gridRows, gridColumns);
    for (int r = 0; r < gridRows; r++) {
        for (int c = 0; c < gridColumns; c++) {
            uint32_t v = gPaintCount[r][c];
            v = (v * 9) / 10;
            gPaintCount[r][c] = static_cast<uint8_t>(v);
        }
    }
}

void zeroOutsideGrid(int gridRows, int gridColumns)
{
    clampExtent(gridRows, gridColumns);
    for (int r = 0; r < MaxRows; r++) {
        for (int c = 0; c < MaxColumns; c++) {
            if (r >= gridRows || c >= gridColumns)
                gPaintCount[r][c] = 0;
        }
    }
}

uint64_t unmeasuredPaints() { return unmeasuredPaintCount; }

void reset()
{
    unmeasuredPaintCount = 0;
    for (int r = 0; r < MaxRows; r++) {
        for (int c = 0; c < MaxColumns; c++)
            gPaintCount[r][c] = 0;
    }
}

// Gradient walk:
// 0           near-black background
// 1..31       deep-blue -> cyan
// 32..63      cyan -> green
// 64..127     green -> yellow
// 128..191    yellow -> red
// 192..255    red -> white (heavy overpaint)
//
uint32_t colorFor(uint8_t count)
{
    if (count == 0)
        return pack(0x1800, 0x1800, 0x1800);
    if (count < 32) {
        int t = count - 1;
        return pack(0, lerp16(0x1000, 0xC000, t, 30), 0xFFFF);
    }
    if (count < 64) {
        int t = count - 32;
        return pack(0, 0xFFFF, lerp16(0xFFFF, 0, t, 31));
    }
    if (count < 128) {
        int t = count - 64;
        return pack(lerp16(0, 0xFFFF, t, 63), 0xFFFF, 0);
    }
    if (count < 192) {
        int t = count - 128;
        return pack(0xFFFF, lerp16(0xFFFF, 0, t, 63), 0);
    }
    int t = count - 192;
    return pack(0xFFFF, lerp16(0, 0xE000, t, 63), lerp16(0, 0xE000, t, 63));
}

} // namespace PerfCounters
} // namespace Diagnostics
} // namespace Ubiquitty

#endif // UBIQUITTY_PERF_DIAG
