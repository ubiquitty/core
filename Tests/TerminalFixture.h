// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

//
// Tests/TerminalFixture.h — the shared vocabulary for driving a Terminal.
//
// Harness.h supplies the assertions; this supplies the four operations every
// terminal test needs: put bytes in, take bytes out, read a cell, read a row.
// File-local copies of these drift into different spellings of the same
// thing, and a reader has to check each one before trusting it. One
// definition means one thing to know.
//
// Include it beside Harness.h:
//
//     #include "Harness.h"
//     #include "TerminalFixture.h"
//

#include "Core/CellGrid.h"
#include "Core/TerminalOperations.h"

#include <cstdio>
#include <cstring>
#include <span>
#include <string>
#include <string_view>

namespace UbiquittyTest {

/// Feeds bytes to the terminal exactly as the PTY read loop would.
///
/// - Parameters:
///   - terminal: The terminal to drive.
///   - bytes: One read's worth of input; a control sequence may span calls.
inline void feed(Ubiquitty::Terminal& terminal, std::string_view bytes)
{
    Ubiquitty::processBytes(&terminal, bytes);
}

/// Feeds raw bytes that are not text.
///
/// A span rather than a pointer and a length: the call sites pass fixed arrays,
/// which deduce their own extent, and the pointer form needed a cast at each one
/// to get from `unsigned char` to the `char` the parser takes.
///
/// - Parameters:
///   - terminal: The terminal to drive.
///   - bytes: Raw input, forwarded unchanged.
inline void feed(Ubiquitty::Terminal& terminal, std::span<const unsigned char> bytes)
{
    Ubiquitty::processBytes(&terminal, std::string_view(reinterpret_cast<const char*>(bytes.data()), static_cast<std::size_t>(bytes.size())));
}

/// Drains pending replies into `outputBuffer` and NUL-terminates them.
///
/// - Parameters:
///   - terminal: The terminal holding the pending replies.
///   - outputBuffer: Receives the replies; one byte is reserved for the
///     terminator, so a buffer of size `n` takes `n - 1` bytes of reply.
/// - Returns: The number of bytes written, not counting the terminator.
inline int drain(Ubiquitty::Terminal& terminal, std::span<char> outputBuffer)
{
    const int written = Ubiquitty::drainOutput(&terminal, { outputBuffer.data(), static_cast<std::size_t>(static_cast<int>(outputBuffer.size()) - 1) });
    outputBuffer[static_cast<std::size_t>(written)] = '\0';
    return written;
}

/// Returns the visible cell at a row and column.
///
/// Reads through CellGrid because Screen owns no cell array: the visible grid
/// lives entirely behind `CellGrid::cellAt`.
///
/// - Parameters:
///   - terminal: The terminal to read from.
///   - row: Zero-based row, counted from the top of the visible grid.
///   - column: Zero-based column, counted from the left margin.
/// - Returns: The cell, or `nullptr` when the position is outside the grid.
inline const Ubiquitty::Cell* cell(const Ubiquitty::Terminal& terminal, int row, int column)
{
    return terminal.grid->cellAt(Ubiquitty::Row(row), Ubiquitty::Column(column));
}

/// Reports the first orphaned half of a wide pair in the visible grid, or an
/// empty string when every pair is whole.
///
/// A wide character occupies two columns: `Attribute::Wide` at the lead and
/// `Attribute::WideContinuation` at the column after it. `writeWideCell` asserts
/// that shape when the pair is written and nothing asserts it again, so any
/// operation that can land on one half alone — an editing control, an overwrite,
/// a reflow — can leave a grid the write path would have rejected.
///
/// One property stands in for an expected grid per case, which means a caller
/// does not have to know the correct outcome of its operation, only that the
/// operation did not saw a pair in half. Every row is walked, not the edited
/// one: an operation that scrolls or rewraps moves pairs between rows, and an
/// orphan created that way is the same defect.
///
/// The attributes are read straight from the cells rather than through
/// `Screen::multicellRunAt`, and routing this through the query would make it
/// unable to fail. The query derives a run by walking left to a lead and right
/// over continuations, so it is self-consistent for any cell contents and
/// reports an orphan as no run at all — indistinguishable from a plain cell,
/// which is the distinction this sweep exists to make. A check derived from the
/// same source as the thing it checks cannot disagree with it.
///
/// That is also why there is no clause here asserting a run's anchor and span.
/// `MulticellRun` carries both, but `multicellRunAt` computes them by the same
/// leftward-and-rightward walk this sweep performs: `span.columns` is the number
/// of adjacent continuations and `span.rows` is the literal 1. Comparing them
/// against adjacency compares adjacency with itself, and the clause would pass
/// for every grid including the torn ones.
///
/// It becomes a real clause when a producer records a span the grid does not
/// already state — a kitty-sized cluster claiming rows, or an anchor naming an
/// extent wider than its continuations. Until then the assertion to want is the
/// one this makes, and an anchor-span clause added early would be a second
/// passing line reporting the coverage of the first.
///
/// - Parameter terminal: The terminal whose visible grid is checked.
/// - Returns: A description naming the offending position, or an empty string
///   when every pair is whole.
inline std::string firstOrphanedWideHalf(const Ubiquitty::Terminal& terminal)
{
    for (int row = 0; row < terminal.rows; ++row) {
        for (int column = 0; column < terminal.columns; ++column) {
            const Ubiquitty::Cell* here = cell(terminal, row, column);
            if (!here)
                continue;
            const auto attributes = here->pen.attributes;
            char message[128];

            if (attributes.contains(Ubiquitty::VT::Attribute::Wide)) {
                const Ubiquitty::Cell* next = column + 1 < terminal.columns
                    ? cell(terminal, row, column + 1)
                    : nullptr;
                if (!next || !next->pen.attributes.contains(Ubiquitty::VT::Attribute::WideContinuation)) {
                    std::snprintf(message, sizeof(message),
                        "lead at (%d,%d) has no continuation to its right", row, column);
                    return message;
                }
            }

            if (attributes.contains(Ubiquitty::VT::Attribute::WideContinuation)) {
                const Ubiquitty::Cell* previous = column > 0 ? cell(terminal, row, column - 1) : nullptr;
                if (!previous || !previous->pen.attributes.contains(Ubiquitty::VT::Attribute::Wide)) {
                    std::snprintf(message, sizeof(message),
                        "continuation at (%d,%d) has no lead to its left", row, column);
                    return message;
                }
            }
        }
    }
    return {};
}

/// Counts the cells in the visible grid carrying either half of a wide pair.
///
/// A sweep asserting that no pair is torn passes trivially on a grid holding no
/// pairs, so a caller that writes its own wide content asserts this is nonzero
/// to show the sweep had something to look at.
///
/// - Parameter terminal: The terminal whose visible grid is counted.
/// - Returns: The number of cells carrying `Attribute::Wide` or
///   `Attribute::WideContinuation`, counting both halves of a pair.
inline int wideHalfCount(const Ubiquitty::Terminal& terminal)
{
    int count = 0;
    for (int row = 0; row < terminal.rows; ++row) {
        for (int column = 0; column < terminal.columns; ++column) {
            const Ubiquitty::Cell* here = cell(terminal, row, column);
            if (!here)
                continue;
            const auto attributes = here->pen.attributes;
            if (attributes.contains(Ubiquitty::VT::Attribute::Wide) || attributes.contains(Ubiquitty::VT::Attribute::WideContinuation))
                ++count;
        }
    }
    return count;
}

} // namespace UbiquittyTest
