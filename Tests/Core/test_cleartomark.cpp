// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Screen/Addressing.h"
#include "Core/Screen/Marks.h"
#include "Harness.h"
#include "TerminalFixture.h"

using UbiquittyTest::feed;

// test_cleartomark.cpp — pins Ubiquitty::clearToMarkOrFull, the one-call
// Cmd-L policy (clearToPreviousMark, else clearBufferPreserveInput, with the
// frontend viewOffset snapped to the live tail). Verifies it matches the inline
// decision the macOS snapshot harness made and gives a non-mark fallback. Bare
// libc++; no platform code.

#include "Core/TerminalOperations.h"

#include "Core/CellGrid.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using Ubiquitty::Screen::ClearToMarkOutcome;

static char cellCh(Ubiquitty::Terminal& t, int row, int column)
{
    const Ubiquitty::Cell* c = Ubiquitty::Screen::visibleCell(&t, 0, row, column);
    return c ? static_cast<char>(c->codePoint) : '?';
}

static bool rowIs(Ubiquitty::Terminal& t, int row, const char* s)
{
    for (int i = 0; s[i]; ++i)
        if (cellCh(t, row, i) != s[i])
            return false;
    return true;
}

// A clean 3-command session ending at a bare prompt (mirrors test_osc133_marks).
static void feedThreeCommands(Ubiquitty::Terminal& t)
{
    feed(t, "\033]133;D;0\007\033]133;A\007$ echo one\r\n\033]133;C\007one\r\n");
    feed(t, "\033]133;D;0\007\033]133;A\007$ echo two\r\n\033]133;C\007two\r\n");
    feed(t, "\033]133;D;0\007\033]133;A\007$ false\r\n\033]133;C\007");
    feed(t, "\033]133;D;1\007\033]133;A\007$ ");
}

// Clear-to-mark branch: with an executed prompt above, clearToMarkOrFull reports
// ClearedToMark, eats the bottom-most command, and snaps viewOffset to 0.
TEST(testClearsToMark)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 12, 40);
    feedThreeCommands(t);

    int viewOffset = 5; // pretend the user had scrolled back
    const std::uint32_t epochBefore = t.absoluteRowEpoch;
    const ClearToMarkOutcome outcome = Ubiquitty::Screen::clearToMarkOrFull(&t, &viewOffset);
    CHECK(outcome == ClearToMarkOutcome::ClearedToMark);
    CHECK(viewOffset == 0); // snapped to the live tail
    // Same result clearToPreviousMark produced in test_cmdL_eats_last_command.
    CHECK(rowIs(t, 0, "$ echo one"));
    CHECK(rowIs(t, 2, "$ echo two"));
    CHECK(rowIs(t, 4, "$ ")); // the bare prompt rode up into "$ false"'s slot
    CHECK(Ubiquitty::Screen::scrollbackCount(&t) == 0);
    CHECK(t.absoluteRowEpoch != epochBefore);
    std::printf("testClearsToMark done\n");
}

// No-mark fallback: with no executed prompt above the cursor, clearToMarkOrFull
// reports FullClear and preserves the half-typed input row (clearBufferPreserveInput).
TEST(testFallsBackToFullClear)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 8, 40);
    // A bare prompt with a half-typed command, but no executed command above it.
    feed(t, "\033]133;A\007$ half-typed");

    int viewOffset = 3;
    const ClearToMarkOutcome outcome = Ubiquitty::Screen::clearToMarkOrFull(&t, &viewOffset);
    CHECK(outcome == ClearToMarkOutcome::FullClear);
    CHECK(viewOffset == 0);
    // The input row survives the full clear and rides to the top.
    CHECK(rowIs(t, 0, "$ half-typed"));
    CHECK(Ubiquitty::Screen::scrollbackCount(&t) == 0);
    std::printf("testFallsBackToFullClear done\n");
}

// Repeated calls eat one command per call, then fall back to FullClear once the
// last executed prompt is gone — the live frontend's "keep pressing Cmd-L" loop.
TEST(testRepeatedThenFullClear)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 12, 40);
    feedThreeCommands(t);
    CHECK(Ubiquitty::Screen::clearToMarkOrFull(&t) == ClearToMarkOutcome::ClearedToMark); // eats $ false
    CHECK(Ubiquitty::Screen::clearToMarkOrFull(&t) == ClearToMarkOutcome::ClearedToMark); // eats $ echo two
    CHECK(Ubiquitty::Screen::clearToMarkOrFull(&t) == ClearToMarkOutcome::ClearedToMark); // eats $ echo one
    CHECK(rowIs(t, 0, "$ "));
    // Nothing executed left above the bare prompt → full clear.
    CHECK(Ubiquitty::Screen::clearToMarkOrFull(&t) == ClearToMarkOutcome::FullClear);
    CHECK(rowIs(t, 0, "$ "));
    std::printf("testRepeatedThenFullClear done\n");
}

// The viewOffset argument is optional: a null pointer must be a safe no-op.
TEST(testNullViewOffsetOk)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 8, 40);
    feed(t, "\033]133;A\007$ x");
    const ClearToMarkOutcome outcome = Ubiquitty::Screen::clearToMarkOrFull(&t); // default nullptr
    CHECK(outcome == ClearToMarkOutcome::FullClear);
    std::printf("testNullViewOffsetOk done\n");
}

// Re-anchoring after a clear pulls history back onto the screen and slides the
// live rows down under it. The width a row reports is what the next scrollback
// capture copies, so a row that claims more than it fills makes that capture
// copy blanks.
//
// What the rows say is already held by the cases above. This holds what they
// claim about themselves: past the live width a row is blank, so a reader that
// copies only that far loses nothing.
TEST(aReAnchoredRowClaimsNoMoreWidthThanItHasContent)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 12, 40);

    // Rows of unlike lengths under marks and wrapped joins, so the width the
    // shift carries varies and the per-row metadata has something to lose.
    for (int block = 0; block < 6; ++block) {
        feed(t, "\x1b]133;A\x07"); // prompt mark
        feed(t, "prompt> command\r\n");
        feed(t, "\x1b]133;C\x07");
        for (int line = 0; line < 3; ++line) {
            const int width = 1 + ((block * 3 + line) * 7) % 39;
            feed(t, std::string(static_cast<std::size_t>(width), 'a' + ((block + line) % 26)).c_str());
            feed(t, "\r\n");
        }
        feed(t, "\x1b]133;D;0\x07");
    }
    feed(t, "\x1b]133;A\x07prompt> ");

    const int rows = t.rows;
    const int columns = t.columns;
    CHECK(Ubiquitty::Screen::scrollbackCount(&t) > 0);

    int viewOffset = 0;
    const ClearToMarkOutcome outcome = Ubiquitty::Screen::clearToMarkOrFull(&t, &viewOffset);
    CHECK_MSG(outcome == ClearToMarkOutcome::ClearedToMark,
        "the corpus has to reach a mark for the re-anchor to run");

    // And the live width bounds the content rather than restating the row: past
    // it every cell is blank, which is what a scrollback capture relies on when
    // it copies only that far.
    int rowsWithContent = 0;
    for (int row = 0; row < rows; ++row) {
        const int width = t.grid->rowContentWidth(Ubiquitty::Row(row));
        for (int column = width; column < columns; ++column) {
            const Ubiquitty::Cell* cell = Ubiquitty::Screen::visibleCell(&t, 0, row, column);
            CHECK_MSG(cell && std::memcmp(cell, &Ubiquitty::defaultBlankCell, sizeof(Ubiquitty::Cell)) == 0,
                "a cell past the row's live width is not blank");
        }
        if (width <= 0)
            continue;
        // Tight, not merely safe: the last column the width covers carries
        // content. A row carried at the full width satisfies the loop above by
        // covering everything and says nothing about where its content ends.
        const Ubiquitty::Cell* last = Ubiquitty::Screen::visibleCell(&t, 0, row, width - 1);
        CHECK_MSG(last && std::memcmp(last, &Ubiquitty::defaultBlankCell, sizeof(Ubiquitty::Cell)) != 0,
            "a row claims a width its content does not reach");
        ++rowsWithContent;
    }
    CHECK_MSG(rowsWithContent > 0, "no row carried content, so the width claim was never tested");
}

TEST(programOwnedLayoutsKeepTheirPictureAndViewOffset)
{
    for (const auto* ownership : { "\x1b[?1049h", "\x1b[2;7r", "\x1b[?69h\x1b[2;30s" }) {
        Ubiquitty::Terminal terminal;
        Ubiquitty::hardReset(terminal);
        Ubiquitty::resize(terminal, 8, 40);
        feedThreeCommands(terminal);
        feed(terminal, ownership);
        feed(terminal, "\x1b[Hprogram chrome");
        const auto epoch = terminal.absoluteRowEpoch;
        const int history = terminal.scrollbackCount;
        std::vector<Ubiquitty::Cell> before(8 * 40);
        for (int row = 0; row < 8; ++row)
            terminal.grid->readRow(Ubiquitty::Row(row), before.data() + row * 40);
        int viewOffset = 3;
        CHECK(Ubiquitty::Screen::clearToMarkOrFull(&terminal, &viewOffset) == ClearToMarkOutcome::HandedOff);
        CHECK(viewOffset == 3);
        CHECK(terminal.scrollbackCount == history && terminal.absoluteRowEpoch == epoch);
        std::vector<Ubiquitty::Cell> after(8 * 40);
        for (int row = 0; row < 8; ++row)
            terminal.grid->readRow(Ubiquitty::Row(row), after.data() + row * 40);
        CHECK(std::memcmp(before.data(), after.data(), before.size() * sizeof(Ubiquitty::Cell)) == 0);
    }
}

UBIQUITTY_TEST_MAIN()
