// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_status_row.cpp — the DECSSDT status-row assembly.
//
// The status line's cells were assembled by one frontend and invisible to the
// rest; these cases pin the Core derivation both arms now consume, so the
// indicator's format and the host-writable row's blank normalisation cannot
// fork per backend again.

#include "Harness.h"
#include <cstring>
#include <memory>
#include <span>
#include <string>

#include "Core/Rendering/StatusRow.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"

namespace {

using namespace Ubiquitty;
using namespace Ubiquitty::Rendering;

std::unique_ptr<Terminal> makeTerminal(int rows = 6, int columns = 40)
{
    auto t = std::make_unique<Terminal>();
    Ubiquitty::resize(*t, rows, columns);
    return t;
}

std::string rowText(std::span<const Cell> cells)
{
    std::string text;
    for (const Cell& cell : cells) {
        if (cell.codePoint == 0)
            break;
        text.push_back(static_cast<char>(cell.codePoint));
    }
    return text;
}

} // namespace

// Type 0 is no status line, and the fill says so rather than clearing the
// caller's buffer to blanks a consumer might then draw.
TEST(testNoStatusLineFillsNothing)
{
    auto t = makeTerminal();
    std::vector<Cell> cells(static_cast<std::size_t>(t->columns));
    CHECK_MSG(!fillStatusRow(t.get(), t->rows, t->columns, 0, cells), "type 0 reports inactive");
}

// The indicator formats the active screen's cursor 1-based with the grid
// size — the VT220 default a host that never writes the row still shows.
TEST(testIndicatorFormatsCursorAndGrid)
{
    auto t = makeTerminal(6, 40);
    Ubiquitty::processBytes(t.get(), "\x1b[1$~");
    Ubiquitty::processBytes(t.get(), "\x1b[3;5H");
    std::vector<Cell> cells(static_cast<std::size_t>(t->columns));
    CHECK_MSG(fillStatusRow(t.get(), t->rows, t->columns, 0, cells), "type 1 reports active");
    CHECK_MSG(rowText(cells) == "Line 3, Column 5  (40x6)", "the indicator carries cursor and grid");
}

// The host-writable row copies what the application wrote and normalises the
// unwritten remainder to the canonical blank, so trailing columns paint as
// the default background byte-for-byte.
TEST(testHostWritableCopiesAndNormalises)
{
    auto t = makeTerminal();
    Ubiquitty::processBytes(t.get(), "\x1b[2$~");
    Ubiquitty::processBytes(t.get(), "\x1b[1$}");
    Ubiquitty::processBytes(t.get(), "OK");
    Ubiquitty::processBytes(t.get(), "\x1b[0$}");
    std::vector<Cell> cells(static_cast<std::size_t>(t->columns));
    CHECK_MSG(fillStatusRow(t.get(), t->rows, t->columns, 0, cells), "type 2 reports active");
    CHECK_MSG(rowText(cells) == "OK", "the written cells copy through");
    CHECK_MSG(std::memcmp(&cells[2], &defaultBlankCell, sizeof(Cell)) == 0, "an unwritten cell is the canonical blank");
}

// A scrolled-back frame shows history; the status line belongs to the tail,
// so a nonzero view offset deactivates the row.
TEST(testScrolledBackViewDeactivates)
{
    auto t = makeTerminal();
    Ubiquitty::processBytes(t.get(), "\x1b[2$~");
    std::vector<Cell> cells(static_cast<std::size_t>(t->columns));
    CHECK_MSG(!fillStatusRow(t.get(), t->rows, t->columns, 1, cells), "a scrolled view reports inactive");
}

// The indicator truncates to the visible width rather than wrapping: the
// status line is one row by definition.
TEST(testIndicatorTruncatesToWidth)
{
    auto t = makeTerminal(6, 12);
    Ubiquitty::processBytes(t.get(), "\x1b[1$~");
    std::vector<Cell> cells(static_cast<std::size_t>(t->columns));
    CHECK_MSG(fillStatusRow(t.get(), t->rows, t->columns, 0, cells), "type 1 reports active on a narrow grid");
    CHECK_MSG(rowText(cells).size() == 12, "the indicator stops at the last column");
}

// An undersized destination is refused whole: painting a partial status line
// over live grid content would show two frames' worth of the screen at once.
TEST(testUndersizedOutputRefused)
{
    auto t = makeTerminal();
    Ubiquitty::processBytes(t.get(), "\x1b[2$~");
    std::vector<Cell> cells(static_cast<std::size_t>(t->columns) - 1);
    CHECK_MSG(!fillStatusRow(t.get(), t->rows, t->columns, 0, cells), "a short buffer is refused");
}

UBIQUITTY_TEST_MAIN()
