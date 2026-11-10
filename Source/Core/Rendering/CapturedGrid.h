// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include "Base/Compiler.h"
#include "Base/SwiftInterop.h"
#include "Core/Cell.h"
#include "Core/Pen.h"
#include "Core/Rendering/FrameCapture.h"
#include "Core/RowMeta.h"
#include "Core/Screen/HistoryFacts.h"

#include <cstddef>
#include <cstdint>
#include <span>

// A captured frame's grid as view state reads it: the cells, wraps and line
// attributes of the rows the frame shows, numbered as a selection numbers
// rows. What a selection resolves over this, it resolves without the model;
// the capture is immutable, so the answer is the frame's whatever the parser
// has done since.

namespace Ubiquitty::Rendering {

/// A read-only view of one capture's grid, in a selection's row numbering.
///
/// A selection numbers the primary screen's rows from the oldest retained
/// history row and the alternate screen's from its top, so `firstRow()` is the
/// history above the frame on the primary and the frame's own top on the
/// alternate. A row outside the frame reads as a stored-empty, unwrapped,
/// single-width row: the frame holds nothing else.
///
/// Borrows the capture, which must outlive it.
class UBIQUITTY_NONESCAPABLE CapturedGrid {
public:
    /// Reads `capture`'s grid.
    ///
    /// - Parameter capture: The capture being drawn.
    explicit CapturedGrid(const FrameCapture& capture UBIQUITTY_LIFETIMEBOUND) noexcept
        : m_frame(*capture.m_frame)
        , m_cells(m_frame.storedCells())
        , m_rows(m_frame.selectionRows())
    {
        const auto& inputs = m_frame.inputs();
        m_rowCount = inputs.rows;
        m_columnCount = inputs.columns;
        const std::size_t cellCount = static_cast<std::size_t>(m_rowCount > 0 ? m_rowCount : 0) * static_cast<std::size_t>(m_columnCount > 0 ? m_columnCount : 0);
        m_isReadable = m_rowCount > 0 && m_columnCount > 0 && m_cells.size() == cellCount
            && m_rows.size() == static_cast<std::size_t>(m_rowCount) && m_frame.richTextQuads().empty() && !hasRowNeedingTheTerminal();
    }

    /// Visible rows.
    [[nodiscard]] int rows() const noexcept { return m_rowCount; }

    /// Grid columns.
    [[nodiscard]] int columns() const noexcept { return m_columnCount; }

    /// The frame's view offset, as it resolved it.
    [[nodiscard]] int viewOffset() const noexcept { return m_frame.viewOffset(); }

    /// How the captured terminal numbered its absolute rows.
    [[nodiscard]] const Screen::RowNumbering& numbering() const noexcept { return m_frame.rowNumbering(); }

    /// The rows above the screen a selection numbers: the history on the
    /// primary screen, none on the alternate.
    [[nodiscard]] int historyRows() const noexcept
    {
        return numbering().isAlternateScreen ? 0 : numbering().historyRows;
    }

    /// The selection row the frame's top visible row is.
    [[nodiscard]] int firstRow() const noexcept { return historyRows() - viewOffset(); }

    /// How many rows a selection can name: the history and the screen.
    [[nodiscard]] int totalRows() const noexcept { return historyRows() + m_rowCount; }

    /// Whether a selection over this frame is resolved here.
    ///
    /// False where the frame holds what only the terminal can read: a rich
    /// text block, a sized glyph or proportional run, a row drawn in another
    /// order than it is stored, or rows captured from a source rather than a
    /// terminal. The capture's own resolved selection stands for such a frame.
    [[nodiscard]] bool isReadable() const noexcept { return m_isReadable; }

    /// The cells a selection row stores; empty outside the frame.
    ///
    /// - Parameter row: A selection row.
    [[nodiscard]] std::span<const Cell> storedRow(int row) const noexcept
    {
        const int visibleRow = row - firstRow();
        if (visibleRow < 0 || visibleRow >= m_rowCount || !m_isReadable)
            return {};
        const std::size_t first = static_cast<std::size_t>(visibleRow) * static_cast<std::size_t>(m_columnCount);
        const auto length = static_cast<std::size_t>(m_rows[static_cast<std::size_t>(visibleRow)].storedLength);
        return m_cells.subspan(first, length < static_cast<std::size_t>(m_columnCount) ? length : static_cast<std::size_t>(m_columnCount));
    }

    /// Whether a selection row soft-wraps into the next one; false outside the frame.
    ///
    /// - Parameter row: A selection row.
    [[nodiscard]] bool isRowWrapped(int row) const noexcept
    {
        const CapturedSelectionRow* facts = rowFacts(row);
        return facts && facts->isWrapped;
    }

    /// A selection row's DEC line attribute; single width outside the frame.
    ///
    /// - Parameter row: A selection row.
    [[nodiscard]] std::uint8_t lineAttribute(int row) const noexcept
    {
        const CapturedSelectionRow* facts = rowFacts(row);
        return facts ? facts->lineAttribute : lineAttributeNormal;
    }

private:
    [[nodiscard]] const CapturedSelectionRow* rowFacts(int row) const noexcept
    {
        const int visibleRow = row - firstRow();
        if (visibleRow < 0 || visibleRow >= m_rowCount || m_rows.size() != static_cast<std::size_t>(m_rowCount))
            return nullptr;
        return &m_rows[static_cast<std::size_t>(visibleRow)];
    }

    // Whether any row holds what only the terminal resolves for a selection.
    [[nodiscard]] bool hasRowNeedingTheTerminal() const noexcept
    {
        for (int visibleRow = 0; visibleRow < m_rowCount; ++visibleRow) {
            const CapturedSelectionRow& facts = m_rows[static_cast<std::size_t>(visibleRow)];
            if (facts.needsShaping)
                return true;
            const std::size_t first = static_cast<std::size_t>(visibleRow) * static_cast<std::size_t>(m_columnCount);
            const auto length = static_cast<std::size_t>(facts.storedLength < m_columnCount ? facts.storedLength : m_columnCount);
            for (const Cell& cell : m_cells.subspan(first, length)) {
                if (cell.pen.extendedAttributes.textSizeSpecID() != 0 || penRichTextCovered(&cell.pen))
                    return true;
            }
        }
        return false;
    }

    const TerminalFrameInputs& m_frame;
    std::span<const Cell> m_cells;
    std::span<const CapturedSelectionRow> m_rows;
    int m_rowCount = 0;
    int m_columnCount = 0;
    bool m_isReadable = false;
};

} // namespace Ubiquitty::Rendering
