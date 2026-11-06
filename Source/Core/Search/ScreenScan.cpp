// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Search/ScreenScan.h"

#include "Core/CellGrid.h"
#include "Core/LineStore.h"
#include "Core/RowMeta.h"
#include "Core/Screen/Addressing.h"
#include "Core/Screen/HistoryLines.h"
#include "Core/Screen/ScrollLine.h"
#include "Core/Screen/Scrollback.h"
#include "Core/Terminal.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <limits>

namespace Ubiquitty::Search {

namespace {

constexpr std::size_t cellBudget = 8192;

Screen::LogicalPosition scopedPosition(const Terminal& terminal, int row, const std::optional<Scope>& scope)
{
    const auto position = Screen::capturePosition(terminal, row, 0);
    if (!position)
        return {};
    if (!scope)
        return *position;
    const auto first = Screen::resolvePosition(terminal, scope->first);
    const auto last = Screen::resolvePosition(terminal, scope->last);
    if (!first || !last || first->absoluteRow > last->absoluteRow)
        return {};
    if (position->line == scope->first.line || position->line == scope->last.line
        || (row >= first->absoluteRow && row <= last->absoluteRow))
        return *position;
    return {};
}

} // namespace

ScreenStamp ScreenStamp::read(const Terminal& terminal)
{
    const bool isPrimary = !terminal.screens.isUsingAlternateScreen;
    const LineStore* store = isPrimary ? terminal.lineStore.get() : nullptr;
    return { .content = terminal.grid ? terminal.grid->contentGeneration() : 0,
        .historyHead = store ? store->headLineNumber() : 0,
        .historyFront = store ? store->frontOrdinal() : 0,
        .historyEnd = store ? store->nextOrdinal() : 0,
        .layout = terminal.absoluteRowEpoch,
        .historyEpoch = terminal.historyEpoch,
        .rows = terminal.rows,
        .columns = terminal.columns,
        .buffer = isPrimary ? Screen::Buffer::Primary : Screen::Buffer::Alternate };
}

void ScreenScan::begin(const Terminal& terminal, const std::optional<Scope>& scope, const std::optional<Screen::LogicalPosition>& newest)
{
    m_stamp = ScreenStamp::read(terminal);
    m_locatedHit.reset();
    m_locatedOrdinal.reset();
    m_prefixNext = 0;
    m_prefixEnd = 0;
    m_prefixPosition = {};
    m_prefixRows.clear();
    m_liveRows.clear();
    m_cells.clear();
    m_textRefs.clear();
    m_storedText.clear();
    if (++m_captureGeneration == 0) {
        m_storeEntries.clear();
        ++m_captureGeneration;
    }
    m_lineStarts.clear();
    m_hits.clear();
    m_results.clear();
    m_isStarted = false;
    m_isLineStarted = false;
    m_newestRow.reset();
    m_skippedCount = 0;
    m_skippedInBoundaryLine = 0;
    m_replayRange.reset();
    m_replayCounts.clear();
    m_replayBeforeLine = 0;
    if (!terminal.grid)
        return;
    const int history = m_stamp.buffer == Screen::Buffer::Primary ? Screen::scrollbackCount(&terminal) : 0;
    int firstRow = history;
    if (history && terminal.lineStore) {
        if (const auto open = terminal.lineStore->openLine()) {
            m_prefixNext = terminal.lineStore->frontOrdinal() + static_cast<std::uint64_t>(open->firstIndex);
            m_prefixEnd = terminal.lineStore->nextOrdinal();
            m_prefixPosition = scopedPosition(terminal, open->firstIndex, scope);
            firstRow = open->firstIndex;
        }
    }
    for (int row = 0; row < terminal.rows; ++row) {
        const RowView view = terminal.grid->rowCells(Ubiquitty::Row(row));
        const int length = view.data ? std::min(terminal.grid->rowContentWidth(Ubiquitty::Row(row)), view.count) : 0;
        Screen::LogicalPosition position;
        if ((row == 0 && m_prefixNext == m_prefixEnd) || (row > 0 && !m_liveRows.back().isWrapped))
            position = scopedPosition(terminal, history + row, scope);
        Row copied = copyRow(terminal, view.data, length, position);
        copied.isWrapped = terminal.grid->rowMeta(row).has(RowFlag::Wrapped);
        m_liveRows.push_back(copied);
    }
    if (!m_liveRows.empty())
        m_liveRows.back().isWrapped = false;
    if (newest) {
        const auto place = Screen::resolvePosition(terminal, *newest);
        m_newestRow = place ? static_cast<std::int64_t>(place->absoluteRow) - firstRow : -1;
        m_newestCell = newest->cell;
    }
}

void ScreenScan::replay(std::uint64_t first, std::uint64_t end)
{
    assert(m_isStarted && !m_line && first <= end && end - first <= maxStoredHits);
    m_replayCounts.clear();
    for (const LineResult& result : m_results)
        m_replayCounts.push_back(result.count);
    m_replayRange = ReplayRange { first, end };
    m_locatedHit.reset();
    m_locatedOrdinal.reset();
    m_replayBeforeLine = 0;
    m_newestRow.reset();
    m_skippedCount = 0;
    m_skippedInBoundaryLine = 0;
    m_hits.clear();
    m_results.clear();
    m_line = m_lineStarts.size();
    m_isLineStarted = false;
}

void ScreenScan::locate(const Hit& hit)
{
    replay(0, 0);
    m_locatedHit = hit;
}

ScreenScan::Row ScreenScan::copyRow(const Terminal& terminal, const Cell* cells, int length, Screen::LogicalPosition position)
{
    if (!cells)
        length = 0;
    while (length > 0 && Screen::isUnwrittenCell(cells[length - 1]))
        --length;
    Row row { .firstCell = m_cells.size(), .length = length, .position = position };
    for (int column = 0; column < length; ++column) {
        const Cell& cell = cells[column];
        TextRef ref;
        ref.isSkipped = isTextContinuation(cell) || isProportionalRunContinuation(&terminal, cell);
        if (!ref.isSkipped && (cell.clusterID() || cell.pen.extendedAttributes.textSizeSpecID())) {
            const auto run = runTextCodePoints(&terminal, cell);
            const std::uint32_t key = cell.clusterID() | (run.empty() ? 0u : 1u << 16);
            const auto found = cell.clusterID() ? m_storeEntries.find(key) : m_storeEntries.end();
            if (found != m_storeEntries.end() && found->second.generation == m_captureGeneration)
                ref = found->second.text;
            else {
                std::array<std::uint32_t, 16> cluster;
                const int count = run.empty() ? cellClusterCodePoints(&terminal, &cell, cluster.data(), static_cast<int>(cluster.size())) : 0;
                const auto text = run.empty() ? std::span<const std::uint32_t>(cluster.data(), static_cast<std::size_t>(std::max(count, 0))) : run;
                ref.offset = static_cast<std::uint32_t>(m_storedText.size());
                ref.length = static_cast<std::uint32_t>(text.size());
                ref.isStored = true;
                m_storedText.insert(m_storedText.end(), text.begin(), text.end());
                if (cell.clusterID())
                    m_storeEntries.insert_or_assign(key, StoredEntry { .text = ref, .generation = m_captureGeneration });
            }
        }
        m_cells.push_back(cell);
        m_textRefs.push_back(ref);
    }
    return row;
}

bool ScreenScan::capturePrefix(const Terminal& terminal)
{
    if (isCaptured())
        return true;
    if (!terminal.lineStore || terminal.absoluteRowEpoch != m_stamp.layout
        || terminal.screens.isUsingAlternateScreen || terminal.lineStore->frontOrdinal() > m_prefixNext
        || terminal.lineStore->nextOrdinal() < m_prefixEnd)
        return false;
    std::size_t copied = 0;
    while (m_prefixNext < m_prefixEnd && copied < cellBudget) {
        const int index = static_cast<int>(m_prefixNext - terminal.lineStore->frontOrdinal());
        const Screen::ScrollLine* line = Screen::scrollbackLine(&terminal, index);
        Row row = copyRow(terminal, line ? line->cells : nullptr, line ? line->length : 0,
            m_prefixRows.empty() ? m_prefixPosition : Screen::LogicalPosition {});
        row.isWrapped = true;
        m_prefixRows.push_back(row);
        copied += static_cast<std::size_t>(std::max(row.length, 1));
        ++m_prefixNext;
    }
    return true;
}

const ScreenScan::Row& ScreenScan::rowAt(std::size_t index) const
{
    return index < m_prefixRows.size() ? m_prefixRows[index] : m_liveRows[index - m_prefixRows.size()];
}

bool ScreenScan::visitMatch(void* context, std::size_t start, std::size_t length)
{
    auto& scan = *static_cast<ScreenScan*>(context);
    LineResult& result = scan.m_results.back();
    if (!result.position.line.value || !length || start + length > scan.m_text.size())
        return true;
    const std::size_t last = start + length - 1;
    const std::uint32_t firstCell = scan.m_writtenCells[start];
    const std::uint32_t endCell = scan.m_writtenCells[last] + scan.m_isWritten[last];
    if (endCell <= firstCell)
        return true;
    ++result.count;
    auto position = result.position;
    position.cell += firstCell;
    position.voidCells = scan.m_gaps[start];
    const Hit hit { .start = position, .endCell = result.position.cell + endCell };
    if (scan.m_replayRange) {
        const auto count = scan.m_replayCounts[scan.m_results.size() - 1];
        assert(result.count <= count);
        const std::uint64_t ordinal = scan.m_replayBeforeLine + static_cast<std::uint64_t>(count - result.count);
        if (scan.m_locatedHit && scan.m_locatedHit->start == hit.start && scan.m_locatedHit->endCell == hit.endCell)
            scan.m_locatedOrdinal = ordinal;
        if (ordinal < scan.m_replayRange->first) {
            ++scan.m_skippedCount;
            if (scan.m_replayRange->first < scan.m_replayBeforeLine + static_cast<std::uint64_t>(count))
                ++scan.m_skippedInBoundaryLine;
            return true;
        }
        if (ordinal >= scan.m_replayRange->end)
            return true;
    } else if (scan.m_newestRow) {
        const auto firstRow = static_cast<std::int64_t>(scan.m_lineStarts[scan.m_line - 1]);
        const auto endRow = static_cast<std::int64_t>(scan.m_line < scan.m_lineStarts.size() ? scan.m_lineStarts[scan.m_line] : scan.m_prefixRows.size() + scan.m_liveRows.size());
        if (firstRow > *scan.m_newestRow || (endRow > *scan.m_newestRow && result.position.cell + firstCell > scan.m_newestCell)) {
            ++scan.m_skippedCount;
            if (firstRow <= *scan.m_newestRow)
                ++scan.m_skippedInBoundaryLine;
            return true;
        }
    }
    ++scan.m_lineCandidateCount;
    const std::size_t room = maxStoredHits - result.firstHit;
    if (!room)
        return true;
    if (result.hitCount < room) {
        scan.m_hits.push_back(hit);
        ++result.hitCount;
    } else
        scan.m_hits[result.firstHit + (static_cast<std::size_t>(scan.m_lineCandidateCount) - 1) % room] = hit;
    return true;
}

bool ScreenScan::step(const Screen::ScrollSearch::CompiledRegex& compiled, Unicode::NormalizationForm form)
{
    if (!isCaptured())
        return true;
    const std::size_t rowCount = m_prefixRows.size() + m_liveRows.size();
    if (!m_isStarted) {
        for (std::size_t row = 0; row < rowCount; ++row) {
            if (!row || !rowAt(row - 1).isWrapped)
                m_lineStarts.push_back(row);
        }
        m_line = m_lineStarts.size();
        m_isStarted = true;
    }
    std::size_t converted = 0;
    while (m_line && converted < cellBudget) {
        if (!m_isLineStarted) {
            m_row = m_lineStarts[m_line - 1];
            m_column = 0;
            m_written = 0;
            m_voidCells = 0;
            m_text.clear();
            m_writtenCells.clear();
            m_gaps.clear();
            m_isWritten.clear();
            m_lineCandidateCount = 0;
            m_results.push_back({ .position = rowAt(m_row).position, .firstHit = m_hits.size() });
            m_isLineStarted = true;
        }
        const std::size_t end = m_line < m_lineStarts.size() ? m_lineStarts[m_line] : rowCount;
        while (m_row < end && converted < cellBudget) {
            const Row& row = rowAt(m_row);
            for (; m_column < row.length && converted < cellBudget; ++m_column, ++converted) {
                const std::size_t index = row.firstCell + static_cast<std::size_t>(m_column);
                const Cell& cell = m_cells[index];
                const TextRef& ref = m_textRefs[index];
                const bool isWritten = !Screen::isUnwrittenCell(cell);
                if (isWritten)
                    m_voidCells = 0;
                const auto text = ref.isSkipped ? std::string_view {} : ref.isStored ? Screen::ScrollSearch::codePointSearchText(std::span(m_storedText).subspan(ref.offset, ref.length), form, m_scratch)
                                                                                     : Screen::ScrollSearch::cellSearchText(nullptr, &cell, form, m_scratch).value_or(std::string_view {});
                m_text.append(text);
                m_writtenCells.insert(m_writtenCells.end(), text.size(), m_written);
                m_gaps.insert(m_gaps.end(), text.size(), m_voidCells);
                m_isWritten.insert(m_isWritten.end(), text.size(), isWritten ? 1 : 0);
                if (isWritten)
                    ++m_written;
                else if (m_voidCells < std::numeric_limits<std::uint16_t>::max())
                    ++m_voidCells;
            }
            if (m_column == row.length) {
                ++m_row;
                m_column = 0;
                m_voidCells = 0;
                ++converted;
            }
        }
        if (m_row != end)
            return true;
        Screen::ScrollSearch::forEachLineMatch(compiled, m_text, visitMatch, this);
        const LineResult& result = m_results.back();
        auto first = m_hits.begin() + static_cast<std::ptrdiff_t>(result.firstHit);
        if (result.hitCount && m_lineCandidateCount > static_cast<std::int64_t>(result.hitCount))
            std::rotate(first, first + static_cast<std::ptrdiff_t>(static_cast<std::size_t>(m_lineCandidateCount) % result.hitCount), m_hits.end());
        std::reverse(first, m_hits.end());
        m_replayBeforeLine += static_cast<std::uint64_t>(result.count);
        --m_line;
        m_isLineStarted = false;
    }
    return m_line != 0;
}

} // namespace Ubiquitty::Search
