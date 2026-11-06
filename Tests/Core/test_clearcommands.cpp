// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Screen/Addressing.h"
#include "Core/Screen/ClearBuffer.h"
#include "Harness.h"
#include "TerminalFixture.h"

#include "Core/CellGrid.h"
#include "Core/TerminalOperations.h"

#include <string>

using Ubiquitty::Screen::ClearCommand;
using UbiquittyTest::feed;

// test_clearcommands.cpp — pins Screen::runClearCommand, the one mapping from
// the command a user invoked to the call that performs it. Every frontend
// dispatches through it, so what each command destroys, what it sends the
// process, and whether it asks the viewport back to the tail are pinned here
// rather than once per arm. Bare libc++; no platform code.

namespace {

// What a frontend would observe of one command: the bytes it was asked to
// forward, and whether it was asked to return the view to the tail.
struct ClearObservation {
    std::string sent;
    bool returnsViewToTail = false;
};

ClearObservation run(Ubiquitty::Terminal& t, ClearCommand command)
{
    ClearObservation observed;
    observed.returnsViewToTail = Ubiquitty::Screen::runClearCommand(
        command, &t, nullptr, [&observed](std::span<const char> bytes) { observed.sent.append(bytes.data(), bytes.size()); });
    return observed;
}

// Three screenfuls, so history exists and the cursor sits below the top row.
void feedHistory(Ubiquitty::Terminal& t)
{
    for (int line = 0; line < 40; ++line)
        feed(t, "history line\r\n");
}

char cellCh(Ubiquitty::Terminal& t, int row, int column)
{
    const Ubiquitty::Cell* c = Ubiquitty::Screen::visibleCell(&t, 0, row, column);
    return c ? static_cast<char>(c->codePoint) : '?';
}

// Whether any visible row still carries ink. A cleared cell reads as 0 or a
// space depending on how it was cleared, and neither is content.
bool anyVisibleRowHasInk(Ubiquitty::Terminal& t)
{
    for (int row = 0; row < t.rows; ++row) {
        for (int column = 0; column < t.columns; ++column) {
            const char ch = cellCh(t, row, column);
            if (ch != 0 && ch != ' ')
                return true;
        }
    }
    return false;
}

}

// Cmd-K: everything goes — the screen and the history behind it — and the shell
// is asked to repaint, because its line editor still believes its prompt sits
// where the screen no longer shows it. Bound to Clear Screen instead, Cmd-K
// would destroy nothing and send no byte.
TEST(testBufferClearsEverythingAndAsksForARedraw)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 12, 40);
    feedHistory(t);
    CHECK(t.scrollbackCount > 0);

    const ClearObservation observed = run(t, ClearCommand::Buffer);

    CHECK(t.scrollbackCount == 0);
    CHECK(observed.sent == "\x0c");
    CHECK(observed.returnsViewToTail);
    // The preserved input row is lifted to the top, so the screen above it is
    // gone rather than scrolled away. Nothing was typed here, so the preserved
    // row is empty too and the whole grid comes up clean.
    CHECK(t.screens.active->cursorY == 0);
    CHECK(!anyVisibleRowHasInk(t));
}

// Opt-Cmd-K: the history the screen is not showing goes, and nothing else. No
// byte, because the screen did not move and the shell is still right about
// where its prompt sits.
TEST(testScrollbackClearsHistoryAndLeavesTheScreen)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 12, 40);
    feedHistory(t);
    CHECK(t.scrollbackCount > 0);
    const int cursorRow = t.screens.active->cursorY;
    const char topLeft = cellCh(t, 0, 0);

    const ClearObservation observed = run(t, ClearCommand::Scrollback);

    CHECK(t.scrollbackCount == 0);
    CHECK(observed.sent.empty());
    CHECK(t.screens.active->cursorY == cursorRow);
    CHECK(cellCh(t, 0, 0) == topLeft);
}

// Clear Screen destroys nothing: the rows above the boundary move into history,
// which is why history GROWS where the other two empty it.
TEST(testScreenScrollsRatherThanDestroying)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 12, 40);
    feedHistory(t);
    const int historyBefore = t.scrollbackCount;

    const ClearObservation observed = run(t, ClearCommand::Screen);

    CHECK(t.scrollbackCount >= historyBefore);
    CHECK(observed.sent.empty());
    CHECK(observed.returnsViewToTail);
}

// A program that owns the layout gets the redraw request instead of an edit:
// the terminal cannot tell which of its rows are finished output, so it stops
// guessing. Nothing is destroyed, and the viewport is not moved.
TEST(testProgramOwningTheLayoutIsHandedTheRedraw)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 12, 40);
    feedHistory(t);
    const int historyBefore = t.scrollbackCount;
    feed(t, "\033[?1049h"); // alternate screen

    const ClearObservation observed = run(t, ClearCommand::Buffer);

    CHECK(observed.sent == "\x0c");
    CHECK(!observed.returnsViewToTail);
    CHECK(t.scrollbackCount == historyBefore);
}

// A null terminal is answered, not crashed on: a frontend may invoke a clear
// between sessions.
TEST(testNoTerminalIsANoOp)
{
    bool sent = false;
    const bool returnsViewToTail = Ubiquitty::Screen::runClearCommand(
        ClearCommand::Buffer, nullptr, nullptr, [&sent](std::span<const char>) { sent = true; });
    CHECK(!returnsViewToTail);
    CHECK(!sent);
}

UBIQUITTY_TEST_MAIN()
