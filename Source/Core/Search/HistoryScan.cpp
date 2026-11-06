// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// HistoryScan — see HistoryScan.h. Everything but `scanHistoryLine` runs
// without the model lock: it reads the snapshot, the blocks the reader gate
// holds for it, and its own storage.

#include "Core/Search/HistoryScan.h"

#include "Core/Screen/HistoryLines.h"
#include "Core/Screen/ScrollLine.h"
#include "Core/Screen/Scrollback.h"
#include "Core/Terminal.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace Ubiquitty::Search {

namespace {

// Converts the matches in a line's text into hits on its written cells and
// hands them on. A match over nothing but a gap's bytes names no written cell
// and is not a hit, nor is a zero-length one.
struct LineMatches {
    std::uint64_t line = 0;
    std::span<const std::uint32_t> cells;
    std::span<const std::uint8_t> isWritten;
    HistoryHitSink sink = nullptr;
    void* context = nullptr;
    bool shouldContinue = true;
};

bool visitMatch(void* context, std::size_t start, std::size_t length)
{
    auto& matches = *static_cast<LineMatches*>(context);
    if (length == 0 || start + length > matches.cells.size())
        return true;
    const std::size_t last = start + length - 1;
    const std::uint32_t startCell = matches.cells[start];
    const std::uint32_t endCell = matches.cells[last] + (matches.isWritten[last] ? 1 : 0);
    if (endCell <= startCell)
        return true;
    matches.shouldContinue = matches.sink(matches.context, HistoryHit { .line = matches.line, .startCell = startCell, .endCell = endCell });
    return matches.shouldContinue;
}

// The cells of a row up to its last written one: the blank tail past it is
// not the line's text, as the history plane and a position both read it.
[[nodiscard]] int writtenRowLength(const Cell* cells, int length)
{
    while (length > 0 && Screen::isUnwrittenCell(cells[length - 1]))
        --length;
    return length;
}

} // namespace

void HistoryScan::begin(const LineStore::Snapshot& snapshot, const Screen::ScrollSearch::CompiledRegex& compiled, Unicode::NormalizationForm form)
{
    m_snapshot = &snapshot;
    m_compiled = compiled;
    m_form = form;
    m_region = snapshot.regions.size();
    m_rowCount = 0;
    m_line = snapshot.headLine;
    m_hasSeenRow = false;
    m_isStopped = false;
    m_deferred.clear();
}

void HistoryScan::readRow(const Cell* cells, int length, RowText& row)
{
    row.bytes.clear();
    row.cells.clear();
    row.isWritten.clear();
    row.writtenCells = 0;
    row.hasStoreCell = false;
    if (!cells)
        return;
    const int written = writtenRowLength(cells, length);
    for (int column = 0; column < written; ++column) {
        const Cell& cell = cells[column];
        const auto text = Screen::ScrollSearch::cellSearchText(nullptr, &cell, m_form, m_cellScratch);
        if (!text) {
            // Only the terminal holds this cell's text: the line waits for
            // the model lock.
            row.hasStoreCell = true;
            return;
        }
        const bool isWritten = !Screen::isUnwrittenCell(cell);
        for (const char byte : *text) {
            row.bytes.push_back(byte);
            row.cells.push_back(row.writtenCells);
            row.isWritten.push_back(isWritten ? 1 : 0);
        }
        if (isWritten)
            ++row.writtenCells;
    }
}

void HistoryScan::takeRow(const Cell* cells, const LineStore::RowFacts& facts)
{
    // The newest stored row belongs to the line still open on the screen when
    // it wraps into it; otherwise it ends the newest stored line. Walking
    // back, an unwrapped row ends the line before the one being assembled.
    const bool isNewest = !m_hasSeenRow;
    m_hasSeenRow = true;
    if (isNewest) {
        m_line = facts.isWrapped ? m_snapshot->headLine : m_snapshot->headLine - 1;
    } else if (!facts.isWrapped) {
        finishLine(LineStart::Stored);
        --m_line;
    }
    // The open line is the screen's to search, with its stored rows.
    if (m_line == m_snapshot->headLine)
        return;
    if (m_rowCount == m_rows.size())
        m_rows.emplace_back();
    readRow(cells, facts.length, m_rows[m_rowCount++]);
}

void HistoryScan::finishLine(LineStart start)
{
    const std::size_t rowCount = std::exchange(m_rowCount, 0);
    if (rowCount == 0 || start == LineStart::Evicted || m_isStopped)
        return;
    const auto rows = std::span(m_rows).first(rowCount);
    if (std::ranges::any_of(rows, [](const RowText& row) { return row.hasStoreCell; })) {
        m_deferred.push_back(m_line);
        return;
    }
    // The rows were taken newest first; the line reads oldest first, its
    // cells counted across them.
    m_text.clear();
    m_cells.clear();
    m_isWritten.clear();
    std::uint32_t before = 0;
    for (auto row = rows.rbegin(); row != rows.rend(); ++row) {
        m_text.append(row->bytes);
        for (const std::uint32_t cell : row->cells)
            m_cells.push_back(before + cell);
        m_isWritten.insert(m_isWritten.end(), row->isWritten.begin(), row->isWritten.end());
        before += row->writtenCells;
    }
    m_isStopped = !emitHits();
}

bool HistoryScan::emitHits()
{
    LineMatches matches { .line = m_line, .cells = m_cells, .isWritten = m_isWritten, .sink = m_sink, .context = m_sinkContext, .shouldContinue = true };
    Screen::ScrollSearch::forEachLineMatch(m_compiled, m_text, visitMatch, &matches);
    if (matches.shouldContinue && m_lineSink)
        m_lineSink(m_sinkContext, m_line);
    return matches.shouldContinue;
}

void HistoryScan::finishAtEvictedFront()
{
    m_region = 0;
    finishLine(LineStart::Evicted);
    m_isStopped = true;
}

HistoryScanState HistoryScan::step(int blockBudget, HistoryHitSink sink, void* context, HistoryLineSink lineSink)
{
    if (!m_snapshot || !sink)
        return HistoryScanState::Complete;
    m_sink = sink;
    m_lineSink = lineSink;
    m_sinkContext = context;
    const LineStore::Snapshot& snapshot = *m_snapshot;
    while (!m_isStopped && m_region > 0 && blockBudget > 0) {
        const std::size_t index = m_region - 1;
        const LineStore::SnapshotRegion& region = snapshot.regions[index];
        if (!region.isFolded && !region.hasFacts)
            return HistoryScanState::NeedsFacts;
        const auto factsOf = [&](std::uint64_t ordinal) -> const LineStore::RowFacts& {
            return snapshot.facts[region.factsOffset + static_cast<std::size_t>(ordinal - region.firstOrdinal)];
        };
        if (region.sequence == 0) {
            // Blank rows outside every block: their facts are the snapshot's.
            for (std::uint64_t ordinal = region.endOrdinal; ordinal-- > region.firstOrdinal;)
                takeRow(nullptr, factsOf(ordinal));
            m_region = index;
            continue;
        }
        if (region.isFolded) {
            std::size_t cellCount = 0;
            for (std::uint64_t ordinal = region.firstOrdinal; ordinal < region.endOrdinal; ++ordinal)
                cellCount += region.entries[static_cast<std::size_t>(ordinal - region.entryOrdinal)].facts.length;
            if (m_decoded.size() < cellCount)
                m_decoded.resize(cellCount);
            std::vector<RowSpan>& decoded = m_decodedRows;
            decoded.clear();
            {
                const LineStore::RegionRead read(snapshot, region);
                if (!read.isValid())
                    return HistoryScanState::Stale;
                const bool isDecoded = LineStore::decodeFoldedRegion(region, m_decoded, region.firstOrdinal, region.endOrdinal,
                    [&decoded](std::uint64_t, const LineStore::RowFacts& facts, const Cell* cells) { decoded.push_back({ cells, facts }); });
                if (!isDecoded)
                    return HistoryScanState::Stale;
            }
            // The decode is the scan's own copy: the block may go now.
            for (auto row = decoded.rbegin(); row != decoded.rend(); ++row)
                takeRow(row->cells, row->facts);
        } else {
            const LineStore::RegionRead read(snapshot, region);
            if (!read.isValid())
                return HistoryScanState::Stale;
            // A raw block's rows lie back to back from its first stored cell.
            std::size_t total = 0;
            for (std::uint64_t ordinal = region.firstOrdinal; ordinal < region.endOrdinal; ++ordinal)
                total += factsOf(ordinal).length;
            std::size_t end = total;
            for (std::uint64_t ordinal = region.endOrdinal; ordinal-- > region.firstOrdinal;) {
                const LineStore::RowFacts& facts = factsOf(ordinal);
                end -= facts.length;
                if (facts.isCompact && facts.length && region.text) {
                    // A compact row's cells are unwritten; its bytes sit at
                    // the same offset in the text slab and expand into the
                    // scan's own scratch.
                    if (m_decoded.size() < facts.length)
                        m_decoded.resize(facts.length);
                    LineStore::expandCompactRow(region.text + end, m_decoded.data(), facts.length);
                    takeRow(m_decoded.data(), facts);
                    continue;
                }
                takeRow(facts.length && region.cells ? region.cells + end : nullptr, facts);
            }
        }
        m_region = index;
        --blockBudget;
    }
    if (m_isStopped)
        return HistoryScanState::Complete;
    if (m_region > 0)
        return HistoryScanState::Scanning;
    // The oldest line is whole only when the front row begins it; otherwise
    // its first rows were evicted, and the cells its matches count from.
    finishLine(snapshot.front.continuity == LineStore::LineContinuity::StartsLine ? LineStart::Stored : LineStart::Evicted);
    return HistoryScanState::Complete;
}

bool scanHistoryLine(const Terminal& terminal, std::uint64_t line, const Screen::ScrollSearch::CompiledRegex& compiled, Unicode::NormalizationForm form, HistoryHitSink sink, void* context, HistoryLineSink lineSink)
{
    const auto rows = Screen::logicalLineRows(terminal, line);
    if (!rows || !sink)
        return true;
    Screen::ScrollSearch::CellTextScratch scratch;
    std::string text;
    std::vector<std::uint32_t> cells;
    std::vector<std::uint8_t> isWritten;
    std::uint32_t before = 0;
    for (int index = rows->firstIndex; index < rows->firstIndex + rows->rowCount; ++index) {
        const Screen::ScrollLine* row = Screen::scrollbackLine(&terminal, index);
        if (!row)
            return true;
        const int written = writtenRowLength(row->cells, row->length);
        for (int column = 0; column < written; ++column) {
            const Cell& cell = row->cells[column];
            const bool isCellWritten = !Screen::isUnwrittenCell(cell);
            const std::string_view cellText = Screen::ScrollSearch::cellSearchText(&terminal, &cell, form, scratch).value_or(std::string_view {});
            for (const char byte : cellText) {
                text.push_back(byte);
                cells.push_back(before);
                isWritten.push_back(isCellWritten ? 1 : 0);
            }
            if (isCellWritten)
                ++before;
        }
    }
    LineMatches matches { .line = line, .cells = cells, .isWritten = isWritten, .sink = sink, .context = context, .shouldContinue = true };
    Screen::ScrollSearch::forEachLineMatch(compiled, text, visitMatch, &matches);
    if (matches.shouldContinue && lineSink)
        lineSink(context, line);
    return matches.shouldContinue;
}

} // namespace Ubiquitty::Search
