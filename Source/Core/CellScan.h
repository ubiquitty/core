// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/Cell.h"

#include <cstdint>
#include <cstring>

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#define UBIQUITTY_CELL_SCAN_NEON 1
#endif

// Scanning a run of cells against one pattern — the shape behind an idempotent
// repaint and an already-blank fill. A per-cell memcmp cannot hold the pattern
// across the scan; a vector form can, and the hot case runs to the end without
// a mismatch.

namespace Ubiquitty {

/// The first index in `[lo, hi]` whose cell differs from `pattern`.
///
/// - Parameters:
///   - cells: The row to scan.
///   - lo: First index, inclusive.
///   - hi: Last index, inclusive. A `hi` below `lo` scans nothing.
///   - pattern: The cell every index is compared against.
/// - Returns: The first differing index, or `hi + 1` when every cell matches.
inline int firstDifferingCell(const Cell* cells, int lo, int hi, const Cell& pattern)
{
    if (!cells || lo > hi)
        return lo;

#if UBIQUITTY_CELL_SCAN_NEON
    // A Cell is 32 bytes, so two of them fill four 128-bit registers and the
    // pattern occupies two that stay live for the whole scan. The reduce is
    // over both cells at once: a hit is rare, so pay the second reduce only
    // then rather than per pair.
    static_assert(sizeof(Cell) == 32, "the paired NEON step assumes a 32-byte Cell");
    const uint8_t* p = reinterpret_cast<const uint8_t*>(&pattern);
    const uint8x16_t p0 = vld1q_u8(p);
    const uint8x16_t p1 = vld1q_u8(p + 16);
    int index = lo;
    for (; index + 1 <= hi; index += 2) {
        const uint8_t* c = reinterpret_cast<const uint8_t*>(&cells[index]);
        const uint8x16_t a0 = veorq_u8(vld1q_u8(c), p0);
        const uint8x16_t a1 = veorq_u8(vld1q_u8(c + 16), p1);
        const uint8x16_t b0 = veorq_u8(vld1q_u8(c + 32), p0);
        const uint8x16_t b1 = veorq_u8(vld1q_u8(c + 48), p1);
        const uint8x16_t firstCell = vorrq_u8(a0, a1);
        if (vmaxvq_u8(vorrq_u8(firstCell, vorrq_u8(b0, b1))) != 0)
            return vmaxvq_u8(firstCell) != 0 ? index : index + 1;
    }
    for (; index <= hi; ++index) {
        if (std::memcmp(&cells[index], &pattern, sizeof(Cell)) != 0)
            return index;
    }
    return index;
#else
    for (int index = lo; index <= hi; ++index) {
        if (std::memcmp(&cells[index], &pattern, sizeof(Cell)) != 0)
            return index;
    }
    return hi + 1;
#endif
}

/// The last index in `[lo, hi]` whose cell differs from `pattern`.
///
/// Where a row's content ends, asked from the end. A scroll inside horizontal
/// margins needs the last occupied column of a column band, and the row's live
/// width cannot answer: it spans the whole row, so a neighbouring pane's text
/// holds it at the full width and the search starts at the band's far edge with
/// the answer far below it.
///
/// - Parameters:
///   - cells: The row to scan.
///   - lo: First index, inclusive.
///   - hi: Last index, inclusive. A `hi` below `lo` scans nothing.
///   - pattern: The cell every index is compared against.
/// - Returns: The last differing index, or `lo - 1` when every cell matches.
inline int lastDifferingCell(const Cell* cells, int lo, int hi, const Cell& pattern)
{
    if (!cells || hi < lo)
        return lo - 1;

#if UBIQUITTY_CELL_SCAN_NEON
    static_assert(sizeof(Cell) == 32, "the paired NEON step assumes a 32-byte Cell");
    const uint8_t* p = reinterpret_cast<const uint8_t*>(&pattern);
    const uint8x16_t p0 = vld1q_u8(p);
    const uint8x16_t p1 = vld1q_u8(p + 16);
    int index = hi;
    // Two cells to a step, walking down. The pair is reduced together and the
    // upper one is settled only when the pair says something is there.
    for (; index - 1 >= lo; index -= 2) {
        const uint8_t* c = reinterpret_cast<const uint8_t*>(&cells[index - 1]);
        const uint8x16_t lower0 = veorq_u8(vld1q_u8(c), p0);
        const uint8x16_t lower1 = veorq_u8(vld1q_u8(c + 16), p1);
        const uint8x16_t upper0 = veorq_u8(vld1q_u8(c + 32), p0);
        const uint8x16_t upper1 = veorq_u8(vld1q_u8(c + 48), p1);
        const uint8x16_t upper = vorrq_u8(upper0, upper1);
        if (vmaxvq_u8(vorrq_u8(upper, vorrq_u8(lower0, lower1))) != 0)
            return vmaxvq_u8(upper) != 0 ? index : index - 1;
    }
    for (; index >= lo; --index) {
        if (std::memcmp(&cells[index], &pattern, sizeof(Cell)) != 0)
            return index;
    }
    return lo - 1;
#else
    for (int index = hi; index >= lo; --index) {
        if (std::memcmp(&cells[index], &pattern, sizeof(Cell)) != 0)
            return index;
    }
    return lo - 1;
#endif
}

/// The first index in `[lo, hi]` where two rows differ.
///
/// The damage walk's question: where does the live row stop matching the shadow
/// it was last painted from. A row that matches whole is the common answer on a
/// frame that changed one line, so the scan is built to reject fast rather than
/// to locate cheaply.
///
/// - Parameters:
///   - left: One row.
///   - right: The other, at least `hi + 1` cells long.
///   - lo: First index, inclusive.
///   - hi: Last index, inclusive. A `hi` below `lo` scans nothing.
/// - Returns: The first differing index, or `hi + 1` when the rows agree.
inline int firstDifferingCellBetween(const Cell* left, const Cell* right, int lo, int hi)
{
    if (!left || !right || lo > hi)
        return lo;

#if UBIQUITTY_CELL_SCAN_NEON
    // Two cells a step, as firstDifferingCell does, and for the same reason: a
    // Cell is 32 bytes, so a pair fills four registers each side and one reduce
    // answers for both. The second reduce runs only where the pair differs,
    // which is once per row rather than once per cell.
    static_assert(sizeof(Cell) == 32, "the paired NEON step assumes a 32-byte Cell");
    int index = lo;
    for (; index + 1 <= hi; index += 2) {
        const uint8_t* l = reinterpret_cast<const uint8_t*>(&left[index]);
        const uint8_t* r = reinterpret_cast<const uint8_t*>(&right[index]);
        const uint8x16_t a0 = veorq_u8(vld1q_u8(l), vld1q_u8(r));
        const uint8x16_t a1 = veorq_u8(vld1q_u8(l + 16), vld1q_u8(r + 16));
        const uint8x16_t b0 = veorq_u8(vld1q_u8(l + 32), vld1q_u8(r + 32));
        const uint8x16_t b1 = veorq_u8(vld1q_u8(l + 48), vld1q_u8(r + 48));
        const uint8x16_t firstCell = vorrq_u8(a0, a1);
        if (vmaxvq_u8(vorrq_u8(firstCell, vorrq_u8(b0, b1))) != 0)
            return vmaxvq_u8(firstCell) != 0 ? index : index + 1;
    }
    for (; index <= hi; ++index) {
        if (std::memcmp(&left[index], &right[index], sizeof(Cell)) != 0)
            return index;
    }
    return index;
#else
    for (int index = lo; index <= hi; ++index) {
        if (std::memcmp(&left[index], &right[index], sizeof(Cell)) != 0)
            return index;
    }
    return hi + 1;
#endif
}

} // namespace Ubiquitty
