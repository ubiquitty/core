// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Screen/Addressing.h"
#include "Core/Terminal.h"
#include "Harness.h"

// test_commandregion.cpp — OSC 133 command-region resolution
// (Ubiquitty::resolveCommandRegion, Core/Screen/CommandBlocks.cpp). Maps a clicked
// absolute row to {promptRow, outputStart, commandLastRow} off the terminal's
// OSC 133 marks. Scripts each scenario's mark layout on a real Ubiquitty::Terminal via
// addRowMark, then resolves — the rule reads Ubiquitty::Terminal marks directly (no provider),
// so it lives in Core beside the mark model, not up in the Selection module.

#include "Core/TerminalOperations.h"
#include "Core/VT/Constants.h"

#include <cstdint>
#include <cstdio>
#include <vector>

// Build a fresh Terminal, script per-absolute-row OSC 133 marks, and resolve the
// command under `absoluteRow`. A hardReset Ubiquitty::Terminal has no scrollback, so absolute
// row r == screen row r; the default height (24) covers every scripted layout.
static Ubiquitty::Screen::CommandRegionRows resolveOn(const std::vector<uint8_t>& marks, int totalRows, int absoluteRow)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    for (int r = 0; r < static_cast<int>(marks.size()); ++r) {
        if (marks[r])
            Ubiquitty::Screen::addRowMark(&t, r, marks[r]);
    }
    return Ubiquitty::Screen::resolveCommandRegion(&t, totalRows, absoluteRow);
}

// A scripted 3-command session, one command per CMD_EXECUTED prompt row, each
// with an OUTPUT_START a row below:
//   row 0: $ echo one  (CMD_EXECUTED)   row 1: one  (OUTPUT_START)
//   row 2: $ echo two  (CMD_EXECUTED)   row 3: two  (OUTPUT_START)
//   row 4: $ false     (CMD_EXECUTED)   row 5: <err>(OUTPUT_START)
//   row 6: $           (bare prompt — unmarked)
static std::vector<uint8_t> threeCommands()
{
    std::vector<uint8_t> m(7, 0);
    m[0] = Ubiquitty::Screen::osc133MarkCmdExecuted;
    m[1] = Ubiquitty::Screen::osc133MarkOutputStart;
    m[2] = Ubiquitty::Screen::osc133MarkCmdExecuted;
    m[3] = Ubiquitty::Screen::osc133MarkOutputStart;
    m[4] = Ubiquitty::Screen::osc133MarkCmdExecuted;
    m[5] = Ubiquitty::Screen::osc133MarkOutputStart;
    return m;
}

// Clicking a command's prompt line resolves its own command (the search is
// inclusive of the clicked row), not the previous one.
TEST(testClickOnPromptRow)
{
    auto r = resolveOn(threeCommands(), 7, /*absoluteRow*/ 2); // "$ echo two"
    CHECK(r.found);
    CHECK(r.promptRow == 2);
    CHECK(r.outputStart == 3);
    CHECK(r.commandLastRow == 2); // prompt row up to the row above output
}

// Clicking a command's OUTPUT row resolves the command above it (most recent
// CMD_EXECUTED at or before the click).
TEST(testClickOnOutputRow)
{
    auto r = resolveOn(threeCommands(), 7, /*absoluteRow*/ 3); // "two"
    CHECK(r.found);
    CHECK(r.promptRow == 2);
    CHECK(r.outputStart == 3);
    CHECK(r.commandLastRow == 2);
}

TEST(testClickLastCommand)
{
    auto r = resolveOn(threeCommands(), 7, /*absoluteRow*/ 5); // "$ false" output
    CHECK(r.found);
    CHECK(r.promptRow == 4);
    CHECK(r.outputStart == 5);
    CHECK(r.commandLastRow == 4);
}

// The bare prompt below the last command is unmarked, so it resolves the last
// executed command (CMD_EXECUTED at row 4).
TEST(testClickBarePromptUsesLastCommand)
{
    auto r = resolveOn(threeCommands(), 7, /*absoluteRow*/ 6); // bare "$ "
    CHECK(r.found);
    CHECK(r.promptRow == 4);
    CHECK(r.outputStart == 5);
    CHECK(r.commandLastRow == 4);
}

// Prompt + output on one row (a command whose ; C landed on the prompt line):
// outputStart == promptRow, commandLastRow == promptRow.
TEST(testZeroOutputCommand)
{
    std::vector<uint8_t> m(3, 0);
    m[0] = Ubiquitty::Screen::osc133MarkCmdExecuted | Ubiquitty::Screen::osc133MarkOutputStart;
    auto r = resolveOn(m, 3, /*absoluteRow*/ 0);
    CHECK(r.found);
    CHECK(r.promptRow == 0);
    CHECK(r.outputStart == 0);
    CHECK(r.commandLastRow == 0);
}

// No OUTPUT_START below the prompt: the prompt row is the output start (no ; C).
TEST(testNoOutputMark)
{
    std::vector<uint8_t> m(4, 0);
    m[1] = Ubiquitty::Screen::osc133MarkCmdExecuted; // prompt at row 1, no ; C anywhere
    auto r = resolveOn(m, 4, /*absoluteRow*/ 2);
    CHECK(r.found);
    CHECK(r.promptRow == 1);
    CHECK(r.outputStart == 1); // fell back to the prompt row
    CHECK(r.commandLastRow == 1);
}

// A block with no ; C followed by a block that has one: the scan stops at the
// next block's prompt, or it adopts that block's output start and the first
// command's "command line" swallows the second command's prompt row. The same
// bound is what keeps building every block from walking the whole buffer.
TEST(testNoOutputMarkStopsAtTheNextBlock)
{
    std::vector<uint8_t> m(6, 0);
    m[1] = Ubiquitty::Screen::osc133MarkCmdExecuted; // block one, never emits ; C
    m[3] = Ubiquitty::Screen::osc133MarkCmdExecuted; // block two
    m[4] = Ubiquitty::Screen::osc133MarkOutputStart; // block two's output
    auto r = resolveOn(m, 6, /*absoluteRow*/ 2);
    CHECK(r.found);
    CHECK(r.promptRow == 1);
    CHECK(r.outputStart == 1); // fell back to its own prompt, not block two's ; C
    CHECK(r.commandLastRow == 1);
}

// A multi-row command line: the ; C is two rows below CMD_EXECUTED, so the
// command line spans both prompt rows.
TEST(testMultilineCommandLine)
{
    std::vector<uint8_t> m(4, 0);
    m[0] = Ubiquitty::Screen::osc133MarkCmdExecuted;
    m[2] = Ubiquitty::Screen::osc133MarkOutputStart;
    auto r = resolveOn(m, 4, /*absoluteRow*/ 1); // click "> echo two"
    CHECK(r.found);
    CHECK(r.promptRow == 0);
    CHECK(r.outputStart == 2);
    CHECK(r.commandLastRow == 1); // both prompt rows 0 and 1
}

// A click above the first command (or in an unmarked buffer) is under no command
// → found == false.
TEST(testNoCommandAbove)
{
    std::vector<uint8_t> shifted(5, 0);
    shifted[2] = Ubiquitty::Screen::osc133MarkCmdExecuted;
    auto r = resolveOn(shifted, 5, /*absoluteRow*/ 1); // no CMD_EXECUTED at/below
    CHECK(!r.found);
    std::vector<uint8_t> blank(5, 0);
    auto b = resolveOn(blank, 5, /*absoluteRow*/ 3); // fully unmarked
    CHECK(!b.found);
}

// Guards: null Terminal / non-positive totalRows → found == false.
TEST(testGuards)
{
    CHECK(!Ubiquitty::Screen::resolveCommandRegion(nullptr, 7, 2).found);
    CHECK(!resolveOn(threeCommands(), /*totalRows*/ 0, 2).found);
}

// A click below the live buffer (absoluteRow >= totalRows) caps to the last row,
// still resolving the last command.
TEST(testClickPastEndCapped)
{
    auto r = resolveOn(threeCommands(), 7, /*absoluteRow*/ 999);
    CHECK(r.found);
    CHECK(r.promptRow == 4); // capped to row 6 → last command at row 4
    CHECK(r.outputStart == 5);
}

UBIQUITTY_TEST_MAIN()
