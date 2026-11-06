// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// HistoryScan — a search of the history a LineStore snapshot captured, run
// without the model lock.
//
// The snapshot is the block directory at one moment; this walks it from the
// newest block to the oldest, holding the reader gate for one block at a time,
// and turns each logical line into the text the find field reads before
// matching it. A line that spans two blocks keeps its newer rows' text while
// the older block completes it, so no block is read twice and none is held
// while another is read.
//
// A match is reported by line number and the written cells it covers, the
// count a logical position uses, so it becomes a position under the model lock
// without another walk of the line. A line whose cells name the terminal's
// cluster pool or run-text store cannot be read without the terminal; the scan
// defers it to `scanHistoryLine`, which the caller runs under the lock.

#include "Core/LineStore.h"
#include "Core/Screen/ScrollSearch.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace Ubiquitty {
struct Terminal;
}

namespace Ubiquitty::Search {

/// One match in a history line: written cells `[startCell, endCell)` of line
/// `line`, counted as a logical position counts them.
struct HistoryHit {
    std::uint64_t line = 0;
    std::uint32_t startCell = 0;
    std::uint32_t endCell = 0;

    friend constexpr bool operator==(const HistoryHit&, const HistoryHit&) = default;
};

/// Receives the hits of one line, in the line's order.
///
/// - Returns: Whether the scan goes on.
using HistoryHitSink = bool (*)(void* context, const HistoryHit& hit);

/// Reports a completely scanned line after its hits, including a line with none.
using HistoryLineSink = void (*)(void* context, std::uint64_t line);

/// What a scan step left to do.
enum class HistoryScanState : std::uint8_t {
    /// Blocks remain; call `step` again.
    Scanning,
    /// The next block's row facts were not copied with the snapshot: the
    /// caller copies them under the model lock (`LineStore::captureRegionFacts`
    /// for `pendingRegion`) and steps again.
    NeedsFacts,
    /// Every line was scanned, or the sink asked to stop.
    Complete,
    /// The store changed under the snapshot, or released a block the scan had
    /// not reached: the caller snapshots again and starts over.
    Stale,
};

/// A history scan in progress.
///
/// Owned by one thread at a time, which also owns the snapshot it reads. Its
/// storage is reused across scans, so a steady caller allocates nothing once
/// it has met its longest line.
class HistoryScan {
public:
    /// Starts a scan of a snapshot, newest line first.
    ///
    /// - Parameters:
    ///   - snapshot: The capture to read; outlives the scan, unchanged but for
    ///     the facts the caller copies into it.
    ///   - compiled: The pattern.
    ///   - form: The form text is compared in, as
    ///     ``Screen::ScrollSearch::matchingNormalizationForm`` answers.
    void begin(const LineStore::Snapshot& snapshot, const Screen::ScrollSearch::CompiledRegex& compiled, Unicode::NormalizationForm form);

    /// Scans blocks until `blockBudget` of them are done, the history is, or
    /// the scan needs the caller.
    ///
    /// - Parameters:
    ///   - blockBudget: The most blocks this step reads.
    ///   - sink: Receives each line's hits, newest line first.
    ///   - context: Handed to `sink`.
    ///   - lineSink: Receives completed lines, or null when only hits are needed.
    /// - Returns: What is left to do.
    [[nodiscard]] HistoryScanState step(int blockBudget, HistoryHitSink sink, void* context, HistoryLineSink lineSink = nullptr);

    /// The region whose facts `NeedsFacts` asks the caller to copy.
    [[nodiscard]] std::size_t pendingRegion() const { return m_region > 0 ? m_region - 1 : 0; }

    /// Ends the scan at the region `NeedsFacts` named, whose rows eviction
    /// took since the snapshot, as every older region's: the line being
    /// assembled lost its start with them, and nothing older remains.
    void finishAtEvictedFront();

    /// The lines whose cells name the terminal's stores, newest first; scanned
    /// under the model lock by ``scanHistoryLine``.
    [[nodiscard]] std::span<const std::uint64_t> deferredLines() const { return m_deferred; }

private:
    /// One row's search text: its bytes, and per byte the written cell of the
    /// row it came from, the next written cell for a gap's byte.
    struct RowText {
        std::string bytes;
        std::vector<std::uint32_t> cells;
        std::vector<std::uint8_t> isWritten;
        std::uint32_t writtenCells = 0;
        bool hasStoreCell = false;
    };

    /// Whether a line's first row is still stored.
    enum class LineStart : std::uint8_t {
        Stored,
        Evicted,
    };

    /// A decoded row's cells, in the scan's own storage.
    struct RowSpan {
        const Cell* cells = nullptr;
        LineStore::RowFacts facts;
    };

    /// Reads one row's cells into its search text.
    ///
    /// - Parameters:
    ///   - cells: The row's cells, or null for a row that stored none.
    ///   - length: How many cells the row stored.
    ///   - row: Receives the text.
    void readRow(const Cell* cells, int length, RowText& row);
    /// Takes the next older row, ending the line being assembled when the row
    /// ends an older one.
    ///
    /// - Parameters:
    ///   - cells: The row's cells, or null for a row that stored none.
    ///   - facts: The row's length and wrap.
    void takeRow(const Cell* cells, const LineStore::RowFacts& facts);
    /// Ends the line the held rows form, matching it unless it is the open
    /// line, a line the front cut, or a deferred one.
    ///
    /// - Parameter start: Whether the line's first row is stored.
    void finishLine(LineStart start);
    /// Matches the assembled line and hands its hits to the sink.
    ///
    /// - Returns: Whether the sink asked to go on.
    [[nodiscard]] bool emitHits();

    const LineStore::Snapshot* m_snapshot = nullptr;
    Screen::ScrollSearch::CompiledRegex m_compiled;
    Unicode::NormalizationForm m_form = Unicode::NormalizationForm::NFC;
    Screen::ScrollSearch::CellTextScratch m_cellScratch;
    // One past the region a step reads: regions are read from the last down,
    // and zero means every one was read.
    std::size_t m_region = 0;
    // The rows of the line being assembled, newest first, and spare rows.
    std::vector<RowText> m_rows;
    std::size_t m_rowCount = 0;
    std::uint64_t m_line = 0;
    bool m_hasSeenRow = false;
    bool m_isStopped = false;
    // The assembled line.
    std::string m_text;
    std::vector<std::uint32_t> m_cells;
    std::vector<std::uint8_t> m_isWritten;
    std::vector<Cell> m_decoded;
    std::vector<RowSpan> m_decodedRows;
    std::vector<std::uint64_t> m_deferred;
    HistoryHitSink m_sink = nullptr;
    HistoryLineSink m_lineSink = nullptr;
    void* m_sinkContext = nullptr;
};

/// Scans one history line with the terminal, for a line the off-lock scan
/// deferred. The caller holds the model lock.
///
/// - Parameters:
///   - terminal: The terminal whose history holds the line.
///   - line: The line's number.
///   - compiled: The pattern.
///   - form: The form text is compared in.
///   - sink: Receives the line's hits.
///   - context: Handed to `sink`.
///   - lineSink: Receives the completed line, or null when only hits are needed.
/// - Returns: Whether the sink asked to go on; true when the line is gone.
bool scanHistoryLine(const Terminal& terminal, std::uint64_t line, const Screen::ScrollSearch::CompiledRegex& compiled, Unicode::NormalizationForm form, HistoryHitSink sink, void* context, HistoryLineSink lineSink = nullptr);

} // namespace Ubiquitty::Search
