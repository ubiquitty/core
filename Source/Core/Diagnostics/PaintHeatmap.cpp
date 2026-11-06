// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// The paint heatmap's policy over the PerfCounters grid. See PaintHeatmap.h.

#include "Core/Diagnostics/PaintHeatmap.h"

#include "Core/Cell.h"
#include "Core/Diagnostics/PerfCounters.h"
#include "Core/Diagnostics/PerfDiag.h"

#include <algorithm>
#include <cstring>

namespace Ubiquitty::Diagnostics {

bool isPerfDiagnosticsBuild()
{
    return UBIQUITTY_PERF_DIAG != 0;
}

namespace {

#if UBIQUITTY_PERF_DIAG
/// The key the shadow keeps for a cell it has not seen.
constexpr std::uint64_t unseenKey = 0;
#endif

/// One step of a 64-bit finaliser (splitmix64's), so two cells differing in
/// one bit land far apart.
std::uint64_t mixed(std::uint64_t value)
{
    value ^= value >> 30;
    value *= 0xBF58476D1CE4E5B9ull;
    value ^= value >> 27;
    value *= 0x94D049BB133111EBull;
    value ^= value >> 31;
    return value;
}

} // namespace

std::uint64_t PaintHeatShadow::contentKey(const Cell& cell)
{
    static_assert(sizeof(Cell) % sizeof(std::uint64_t) == 0, "the key reads the cell as whole words");
    std::uint64_t words[sizeof(Cell) / sizeof(std::uint64_t)];
    std::memcpy(words, &cell, sizeof(Cell));
    std::uint64_t key = 0x9E3779B97F4A7C15ull;
    for (const std::uint64_t word : words)
        key = mixed(key ^ word);
    return key | 1u;
}

void PaintHeatShadow::enable()
{
    m_keys.clear();
    m_rows = 0;
    m_columns = 0;
    m_isEnabled = true;
    m_hasBaseline = false;
#if UBIQUITTY_PERF_DIAG
    PerfCounters::reset();
#endif
}

void PaintHeatShadow::disable()
{
    m_isEnabled = false;
    m_hasBaseline = false;
    m_rows = 0;
    m_columns = 0;
    std::vector<std::uint64_t>().swap(m_keys);
}

void PaintHeatShadow::noteCapturedGrid(const Cell* cells, int rows, int columns)
{
#if UBIQUITTY_PERF_DIAG
    if (!m_isEnabled || !cells || rows <= 0 || columns <= 0)
        return;
    if (rows != m_rows || columns != m_columns) {
        std::vector<std::uint64_t> resized(static_cast<std::size_t>(rows) * static_cast<std::size_t>(columns), unseenKey);
        const int sharedRows = std::min(rows, m_rows);
        const int sharedColumns = std::min(columns, m_columns);
        for (int row = 0; row < sharedRows; ++row) {
            const auto* from = m_keys.data() + static_cast<std::size_t>(row) * static_cast<std::size_t>(m_columns);
            std::copy(from, from + sharedColumns, resized.data() + static_cast<std::size_t>(row) * static_cast<std::size_t>(columns));
        }
        m_keys = std::move(resized);
        m_rows = rows;
        m_columns = columns;
    }
    const bool isBaseline = !m_hasBaseline;
    m_hasBaseline = true;
    for (int row = 0; row < rows; ++row) {
        for (int column = 0; column < columns; ++column) {
            const std::size_t index = static_cast<std::size_t>(row) * static_cast<std::size_t>(columns) + static_cast<std::size_t>(column);
            const std::uint64_t key = contentKey(cells[index]);
            if (m_keys[index] == key)
                continue;
            m_keys[index] = key;
            if (!isBaseline)
                PerfCounters::noteCellPaint(row, column);
        }
    }
#else
    (void)cells;
    (void)rows;
    (void)columns;
#endif
}

namespace PaintHeatmap {

void noteWindowOpened()
{
#if UBIQUITTY_PERF_DIAG
    PerfCounters::reset();
#endif
}

PaintHeatmapExtent refreshWindow(int liveRows, int liveColumns)
{
    PaintHeatmapExtent extent;
#if UBIQUITTY_PERF_DIAG
    extent.rows = std::clamp(liveRows, 0, static_cast<int>(PerfCounters::MaxRows));
    extent.columns = std::clamp(liveColumns, 0, static_cast<int>(PerfCounters::MaxColumns));
    PerfCounters::decay(extent.rows, extent.columns);
    PerfCounters::zeroOutsideGrid(extent.rows, extent.columns);
    extent.widthInPoints = extent.columns * cellSizeInPoints();
    extent.heightInPoints = extent.rows * cellSizeInPoints();
#else
    (void)liveRows;
    (void)liveColumns;
#endif
    return extent;
}

std::uint32_t cellColor(int row, int column)
{
#if UBIQUITTY_PERF_DIAG
    return PerfCounters::colorFor(PerfCounters::snapshotCell(row, column));
#else
    (void)row;
    (void)column;
    return 0;
#endif
}

std::uint8_t cellCount(int row, int column)
{
#if UBIQUITTY_PERF_DIAG
    return PerfCounters::snapshotCell(row, column);
#else
    (void)row;
    (void)column;
    return 0;
#endif
}

} // namespace PaintHeatmap

} // namespace Ubiquitty::Diagnostics
