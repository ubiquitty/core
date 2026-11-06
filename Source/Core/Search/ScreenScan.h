// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/Cell.h"
#include "Core/Screen/ScrollSearch.h"
#include "Core/Search/Search.h"

#include <cstdint>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

namespace Ubiquitty::Search {

/// The facts that invalidate a captured screen search. Read under the model lock.
struct ScreenStamp {
    std::uint64_t content = 0;
    std::uint64_t historyHead = 0;
    std::uint64_t historyFront = 0;
    std::uint64_t historyEnd = 0;
    std::uint32_t layout = 0;
    std::uint32_t historyEpoch = 0;
    int rows = 0;
    int columns = 0;
    Screen::Buffer buffer = Screen::Buffer::Primary;

    /// Captures the presented buffer's text and layout generations.
    /// - Parameter terminal: The model, locked by the caller.
    /// - Returns: Its current stamp.
    [[nodiscard]] static ScreenStamp read(const Terminal& terminal);
    friend bool operator==(const ScreenStamp&, const ScreenStamp&) = default;
};

/// An owned screen copy, including the stored prefix of its open logical line.
/// Capturing reads cells and side-table text under the model lock; conversion
/// and matching use only this copy. A long stored prefix is copied in slices.
class ScreenScan {
public:
    /// One captured logical line's result, including hits beyond storage capacity.
    struct LineResult {
        Screen::LogicalPosition position;
        std::int64_t count = 0;
        std::size_t firstHit = 0;
        std::size_t hitCount = 0;
    };

    /// Copies the live grid and records the open prefix. Model lock held.
    /// - Parameters:
    ///   - terminal: The model to capture.
    ///   - scope: An optional inclusive range of logical lines.
    ///   - newest: No value keeps the newest hits; a position keeps hits at
    ///     or before that text while still counting every match.
    void begin(const Terminal& terminal, const std::optional<Scope>& scope, const std::optional<Screen::LogicalPosition>& newest = std::nullopt);
    /// Searches a completed owned copy again for a bounded ordinal interval.
    /// The query must match the completed scan. No model access is needed;
    /// subsequent steps use the same conversion budget as the initial scan.
    /// - Parameters:
    ///   - first: The first match to retain in newest-first order.
    ///   - end: One past the last match, at most maxStoredHits past first.
    void replay(std::uint64_t first, std::uint64_t end);
    /// Locates a hit's exact ordinal by replaying the completed owned copy.
    /// Subsequent steps use the initial scan's budget and retain no hits.
    /// - Parameter hit: The captured text to locate.
    void locate(const Hit& hit);
    /// The located hit's newest-first ordinal, available after replay completes.
    [[nodiscard]] std::optional<std::uint64_t> locatedOrdinal() const { return m_locatedOrdinal; }
    /// Copies one bounded part of the stored prefix. Model lock held.
    /// - Parameter terminal: The same model.
    /// - Returns: False when a destructive mutation invalidated the capture.
    [[nodiscard]] bool capturePrefix(const Terminal& terminal);
    /// Whether all cells and side-table text have been copied.
    [[nodiscard]] bool isCaptured() const { return m_prefixNext == m_prefixEnd; }
    /// Converts and searches one bounded batch of copied cells, without a model lock.
    /// - Parameters:
    ///   - compiled: The query's compiled pattern.
    ///   - form: Its normalization form.
    /// - Returns: Whether more copied text remains to scan.
    bool step(const Screen::ScrollSearch::CompiledRegex& compiled, Unicode::NormalizationForm form);
    /// The bounded hit prefix, newest first.
    [[nodiscard]] std::span<const Hit> hits() const { return m_hits; }
    /// Exact counts for each captured line, newest first.
    [[nodiscard]] std::span<const LineResult> lines() const { return m_results; }
    /// The model facts at the live-grid capture.
    [[nodiscard]] const ScreenStamp& stamp() const { return m_stamp; }
    /// Matches newer than the retained position boundary, independent of capacity.
    [[nodiscard]] std::int64_t skippedCount() const { return m_skippedCount; }
    /// Matches newer than the boundary within its own logical line.
    [[nodiscard]] std::int64_t skippedInBoundaryLine() const { return m_skippedInBoundaryLine; }

private:
    /// A half-open interval in newest-first match order.
    struct ReplayRange {
        std::uint64_t first = 0;
        std::uint64_t end = 0;
    };
    /// Locates copied side-table text, or marks an inline or continuation cell.
    struct TextRef {
        std::uint32_t offset = 0;
        std::uint32_t length = 0;
        bool isStored = false;
        bool isSkipped = false;
    };
    /// Reuses a side-table lookup entry across captures without reusing its text.
    struct StoredEntry {
        TextRef text;
        std::uint64_t generation = 0;
    };
    /// Locates copied cells and names a logical line at its first row.
    struct Row {
        std::size_t firstCell = 0;
        int length = 0;
        bool isWrapped = false;
        Screen::LogicalPosition position;
    };

    /// Copies a row and the text named by its cells. Model lock held.
    /// - Parameters:
    ///   - terminal: The model owning the side tables.
    ///   - cells: The row's borrowed cells.
    ///   - length: The filled width.
    ///   - position: The logical line's first position, on its first row.
    /// - Returns: A row borrowing only this scan's storage.
    [[nodiscard]] Row copyRow(const Terminal& terminal, const Cell* cells, int length, Screen::LogicalPosition position);
    /// Returns a captured row in document order, prefix before live grid.
    [[nodiscard]] const Row& rowAt(std::size_t index) const;
    /// Records one match in the line being scanned.
    /// - Parameters:
    ///   - context: The scan.
    ///   - start: The match's byte offset.
    ///   - length: Its byte length.
    /// - Returns: True to count every match.
    static bool visitMatch(void* context, std::size_t start, std::size_t length);

    ScreenStamp m_stamp;
    Screen::LogicalPosition m_prefixPosition;
    std::uint64_t m_prefixNext = 0;
    std::uint64_t m_prefixEnd = 0;
    std::vector<Row> m_prefixRows;
    std::vector<Row> m_liveRows;
    std::vector<Cell> m_cells;
    std::vector<TextRef> m_textRefs;
    std::vector<std::uint32_t> m_storedText;
    std::unordered_map<std::uint32_t, StoredEntry> m_storeEntries;
    std::uint64_t m_captureGeneration = 0;
    std::vector<std::size_t> m_lineStarts;
    std::size_t m_line = 0;
    std::size_t m_row = 0;
    int m_column = 0;
    bool m_isStarted = false;
    bool m_isLineStarted = false;
    std::optional<std::int64_t> m_newestRow;
    std::uint32_t m_newestCell = 0;
    std::int64_t m_skippedCount = 0;
    std::int64_t m_skippedInBoundaryLine = 0;
    std::int64_t m_lineCandidateCount = 0;
    std::optional<ReplayRange> m_replayRange;
    std::optional<Hit> m_locatedHit;
    std::optional<std::uint64_t> m_locatedOrdinal;
    std::vector<std::int64_t> m_replayCounts;
    std::uint64_t m_replayBeforeLine = 0;
    std::uint32_t m_written = 0;
    std::uint16_t m_voidCells = 0;
    std::string m_text;
    std::vector<std::uint32_t> m_writtenCells;
    std::vector<std::uint16_t> m_gaps;
    std::vector<std::uint8_t> m_isWritten;
    Screen::ScrollSearch::CellTextScratch m_scratch;
    std::vector<Hit> m_hits;
    std::vector<LineResult> m_results;
};

} // namespace Ubiquitty::Search
