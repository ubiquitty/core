// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Base/Types.h"
#include "Core/RichText/Document.h"
#include "Core/Selection/Selection.h"
#include "Core/Unicode/UTF8Encoder.h"
#include <algorithm>
#include <array>
#include <span>
#include <string>

// Row-range → UTF-8 text policy. The single policy both frontends route through
// when serializing a selection to clipboard text, so they can't diverge. It
// suppresses the trailing newline on a soft-wrapped row and handles rectangular
// blocks. Pure — the caller supplies the grid through callbacks:
//   * cellAt     — code point at (row, column), 0 for a void cell (rowSpan's rule).
//   * cellTextAt — code point PLUS the wide-continuation flag (the authority for
//                  skipping the continuation half).
//   * rowWrapped — 1 when `row` soft-wraps into row+1; may be null (no suppression).
// Rows resolve through Selection::rowSpan (the one source of the void rule), so
// every mode clips identically to what is painted.

namespace Ubiquitty::Selection {

// One grid cell as the text walk sees it: its code point and whether another
// cell already carries that glyph, which makes this one contribute no bytes —
// the trailing half of a wide pair, or a sized block's row below its top.
struct TextCell {
    std::uint32_t codePoint;
    bool isContinuation;
    // Only the first `codePointCount` entries are defined. Zero-initialising the
    // array costs 64 bytes of stores for every cell of a selection — 1.28 GB over
    // a 20M-cell copy — for entries no consumer reads: every walk bounds its loop
    // by the count.
    std::array<std::uint32_t, 16> codePoints;
    int codePointCount { 0 };
    // A proportional run's cell: the letters whose ink begins in it, read from
    // the run's text in place of `codePoints`, which cannot hold a cell that
    // owns more than sixteen. Empty for every other cell.
    std::span<const std::uint32_t> runCodePoints {};
    // Whether `runCodePoints` is already exactly what the selection holds of
    // the cell — a scaled run's selected letters, gathered from every band
    // row — so no caret at the selection's ends cuts it again.
    bool isUncut = false;
};

// Reads (row, column) for the emission walk. `row` is in the same coordinate
// space as the bounds passed to appendSelectionText (absolute buffer rows on
// both frontends).
using TextCellAt = TextCell (*)(void* context, int row, int column);

// Whether a soft-wrapped row's newline is suppressed when joining rows.
//   Join — drop the '\n' after a soft-wrapped row so a reflowed logical line
//          copies back as a single line (the terminal copy convention).
//   Keep — always emit a '\n' between rows (rectangular block copy, where each
//          grid row is its own physical line regardless of wrap state).
enum class SoftWrapNewline {
    Join,
    Keep,
};

/// The code points of one cell's text a selection covers: `[first, end)`.
struct PackedCellCut {
    int first;
    int end;
};

/// Which of a cell's code points a selection covers.
///
/// The whole cell, except at a character selection's two edge cells, where a
/// caret standing inside a packed proportional cell leaves out the letters
/// before it (the first cell) or after it (the last). A selection wholly
/// inside one cell cuts both ends of that cell. Every walk that turns a
/// selection into text asks here, so the plain, HTML and RTF flavours of one
/// copy cannot carry different letters.
///
/// The row's resolved span says whether an edge is still where the bounds put
/// it. The per-row void rule moves an edge off the cell its offset counts
/// into, and an offset applied there would name the wrong letters, so a moved
/// edge takes its cell whole.
///
/// - Parameters:
///   - b: The resolved bounds.
///   - mode: The selection's mode; only a character selection cuts.
///   - span: The row's span, as ``rowSpan`` resolved it.
///   - row: The row being walked.
///   - column: The cell being walked.
///   - count: How many code points the cell holds.
/// - Returns: The covered range, clamped to the cell's text.
inline PackedCellCut packedCellCut(
    const Ubiquitty::SelectionBounds* b, Ubiquitty::SelectionMode mode, const RowSpan& span, int row, int column, int count)
{
    PackedCellCut cut { 0, count };
    if (!b || mode != Ubiquitty::SelectionMode::Character)
        return cut;
    if (b->subColumn0 > 0 && row == b->row0 && column == b->column0 && span.column0 == b->column0)
        cut.first = std::min(b->subColumn0, count);
    if (b->subColumn1 > 0 && row == b->row1 && column == b->column1 && span.column1 == b->column1)
        cut.end = std::min(b->subColumn1, count);
    return cut;
}

/// Whether a proportional run's spaces at the end of a row are its text.
///
/// Keep on a row that soft-wraps into the next selected row: a run broken at
/// a space keeps the space as the last letter of the piece before the wrap,
/// and a copy that joins the rows carries it between the words.
enum class RunSpaces {
    Trim,
    Keep,
};

/// Whether a copy keeps the run spaces closing `row`: the row soft-wraps into
/// a row the selection also covers, and the copy joins the two.
inline RunSpaces runSpacesClosingRow(const Ubiquitty::SelectionBounds* b, int row, SoftWrapNewline newline,
    Ubiquitty::SelectionRowWrappedCallback rowWrapped, void* wrappedContext)
{
    const bool isJoined = row < b->row1 && newline == SoftWrapNewline::Join && rowWrapped != nullptr && rowWrapped(wrappedContext, row) != 0;
    return isJoined ? RunSpaces::Keep : RunSpaces::Trim;
}

/// Finds the final selected column carrying nonblank text.
///
/// A proportional cell may begin with a space and carry a complete word in
/// its remaining cluster. The scalar-only fast path cannot trim that cell.
///
/// - Parameters:
///   - row: The selected row.
///   - firstColumn: The selection's first column.
///   - lastColumn: The last stored column inside the selection.
///   - cellAt: Reads a cell's leading code point.
///   - cellContext: The leading-code-point reader's context.
///   - textAt: Reads a cell's complete text and continuation flag.
///   - textContext: The full-text reader's context.
///   - runSpaces: Whether a run cell holding only spaces is text.
/// - Returns: The final nonblank column, or one before `firstColumn`.
inline int lastNonBlankSelectionColumn(int row, int firstColumn, int lastColumn,
    SelectionCellAt cellAt, void* cellContext, TextCellAt textAt, void* textContext, RunSpaces runSpaces = RunSpaces::Trim)
{
    for (int column = lastColumn; column >= firstColumn; --column) {
        const std::uint32_t codePoint = cellAt(cellContext, row, column);
        if (!codePoint) {
            continue;
        }
        if (codePoint != ' ') {
            return column;
        }
        const auto cell = textAt(textContext, row, column);
        if (cell.isContinuation) {
            continue;
        }
        for (int index = 0; index < cell.codePointCount && index < static_cast<int>(cell.codePoints.size()); ++index) {
            const std::uint32_t member = cell.codePoints[static_cast<std::size_t>(index)];
            if (member != 0 && member != ' ') {
                return column;
            }
        }
        for (const std::uint32_t member : cell.runCodePoints) {
            if (member != 0 && (member != ' ' || runSpaces == RunSpaces::Keep)) {
                return column;
            }
        }
    }
    return firstColumn - 1;
}

// Append the text of bounds `b` (inclusive rows [row0,row1]) to `out`. Per row,
// rowSpan yields the selected span; trailing blanks are trimmed and each
// non-continuation cell is UTF-8 encoded. Rows join with '\n', except a
// soft-wrapped row's newline is dropped when `newline` is Join and rowWrapped
// reports the join. `rowWrapped` may be null (newline between every row).
inline void appendSelectionText(
    std::string& out, const Ubiquitty::SelectionBounds* b, Ubiquitty::SelectionMode mode, int columns, Ubiquitty::SelectionCellAt cellAt, void* cellContext, TextCellAt cellTextAt, void* textContext, SoftWrapNewline newline, Ubiquitty::SelectionRowWrappedCallback rowWrapped, void* wrappedContext, Ubiquitty::SelectionRowExtentAt rowExtent = nullptr, void* extentContext = nullptr)
{
    if (!b || columns <= 0)
        return;
    // The newline after a row is written once the next row proves to be a
    // line of its own — it emits a letter, or it is blank. A row whose
    // selected cells are all continuations repeats the row above it (the
    // lower bands of sized blocks, the bottom half of a double-height pair
    // under its selected top) and adds neither a letter nor a line: the
    // reader sees one line where the grid holds two.
    bool isNewlineOwed = false;
    // Bytes are staged and appended a block at a time: appending to the
    // string per byte is a capacity check and a call on every character.
    std::array<char, 512> staged;
    std::size_t stagedLength = 0;
    const auto flush = [&] {
        out.append(staged.data(), stagedLength);
        stagedLength = 0;
    };
    const auto stage = [&](std::uint32_t codePoint) {
        if (stagedLength > staged.size() - 4)
            flush();
        Unicode::appendUTF8(staged.data(), &stagedLength, codePoint);
    };
    for (int row = b->row0; row <= b->row1; ++row) {
        const RowSpan span = rowSpan(b, mode, row, columns, cellAt, cellContext, /*attributesAt*/ nullptr, /*attributeContext*/ nullptr);
        if (span.selected) {
            // Trim trailing blanks (void / space) within the selected span so a
            // copied line ends at its last printed glyph, not the grid edge.
            // Start at the row's own end rather than the screen's: a row that
            // stores nothing past its last cell answers the scan in a step
            // where the screen edge makes it walk the whole unstored tail.
            int scanFrom = span.column1;
            if (rowExtent) {
                const int extent = rowExtent(extentContext, row);
                if (extent - 1 < scanFrom)
                    scanFrom = extent - 1;
            }
            const int lastNonBlank = lastNonBlankSelectionColumn(row, span.column0, scanFrom,
                cellAt, cellContext, cellTextAt, textContext, runSpacesClosingRow(b, row, newline, rowWrapped, wrappedContext));
            if (lastNonBlank < span.column0 && isNewlineOwed) {
                stage('\n');
                isNewlineOwed = false;
            }
            // Only a character selection's first and last rows can cut a cell,
            // and only where the bounds carry a sub-cell offset there.
            const bool mayCutCells = mode == Ubiquitty::SelectionMode::Character
                && ((row == b->row0 && b->subColumn0 > 0) || (row == b->row1 && b->subColumn1 > 0));
            for (int column = span.column0; column <= lastNonBlank; ++column) {
                const TextCell cell = cellTextAt(textContext, row, column);
                if (cell.isContinuation)
                    continue; // continuation half — the lead already emitted it
                if (isNewlineOwed) {
                    stage('\n');
                    isNewlineOwed = false;
                }
                if (!cell.runCodePoints.empty()) {
                    const int runCount = static_cast<int>(cell.runCodePoints.size());
                    const PackedCellCut cut = cell.isUncut ? PackedCellCut { 0, runCount } : packedCellCut(b, mode, span, row, column, runCount);
                    for (int i = cut.first; i < cut.end; ++i)
                        stage(RichText::copiedCodePoint(cell.runCodePoints[static_cast<std::size_t>(i)]));
                } else if (cell.codePointCount > 0) {
                    const PackedCellCut cut = mayCutCells ? packedCellCut(b, mode, span, row, column, cell.codePointCount)
                                                          : PackedCellCut { 0, cell.codePointCount };
                    for (int i = cut.first; i < cut.end && i < static_cast<int>(cell.codePoints.size()); ++i)
                        stage(cell.codePoints[static_cast<std::size_t>(i)]);
                } else {
                    stage(cell.codePoint != 0 ? cell.codePoint : static_cast<std::uint32_t>(' '));
                }
            }
        } else if (isNewlineOwed) {
            stage('\n');
            isNewlineOwed = false;
        }
        // Newline between rows, unless this row soft-wraps into the next and the
        // caller asked to join wrapped rows (the continuation has no real break).
        if (row < b->row1) {
            const bool wrapped = newline == SoftWrapNewline::Join && rowWrapped != nullptr && rowWrapped(wrappedContext, row) != 0;
            if (!wrapped)
                isNewlineOwed = true;
        }
    }
    flush();
}

/// Returns the text of a selection as a fresh string.
///
/// Wraps ``appendSelectionText`` for a caller with no buffer to append to; the
/// parameters carry the same meaning, including the one-sided `rowExtent`
/// contract.
///
/// - Parameters:
///   - b: The selection's inclusive row bounds.
///   - mode: Character, word or rectangular selection.
///   - columns: The grid width the bounds were taken against.
///   - cellAt: Reads a cell's code point.
///   - cellContext: Passed to `cellAt`.
///   - cellTextAt: Reads a cell's full cluster.
///   - textContext: Passed to `cellTextAt`.
///   - newline: Whether a soft-wrapped row's newline is dropped.
///   - rowWrapped: Reports a soft-wrap join, or null for a newline between every row.
///   - wrappedContext: Passed to `rowWrapped`.
///   - rowExtent: Reports a row's stored width, or null to scan from the screen edge.
///   - extentContext: Passed to `rowExtent`.
/// - Returns: The selected text.
inline std::string selectionText(const Ubiquitty::SelectionBounds* b, Ubiquitty::SelectionMode mode, int columns, Ubiquitty::SelectionCellAt cellAt, void* cellContext, TextCellAt cellTextAt, void* textContext, SoftWrapNewline newline, Ubiquitty::SelectionRowWrappedCallback rowWrapped, void* wrappedContext, Ubiquitty::SelectionRowExtentAt rowExtent = nullptr, void* extentContext = nullptr)
{
    std::string out;
    appendSelectionText(out, b, mode, columns, cellAt, cellContext, cellTextAt, textContext, newline, rowWrapped, wrappedContext, rowExtent, extentContext);
    return out;
}

} // namespace Ubiquitty::Selection
