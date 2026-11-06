// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_terminal_clock.cpp — a terminal reads its own clock.
//
// Core reads time while it parses: an OSC 133 command's C and D marks time the
// command. Read from a process-wide clock, two terminals cannot be given
// different instants, and a replay or a replica parsing the same bytes later
// reads the wrong ones. So a terminal reads time through terminalNow, which
// prefers the terminal's own TerminalClock; two terminals with two clocks time
// their commands each by their own.

#include "Harness.h"
#include "TerminalFixture.h"

#include "Core/CellGrid.h"
#include "Core/Screen/MarkValues.h"
#include "Core/TerminalOperations.h"
#include "PAL/TerminalClients.h"

#include <cstdint>
#include <memory>

using UbiquittyTest::feed;

namespace {

class SetClock final : public Ubiquitty::TerminalClock {
public:
    explicit SetClock(std::uint32_t start)
        : ticks(start)
    {
    }
    Ubiquitty::Ticks now() override { return Ubiquitty::Ticks(ticks); }
    std::uint32_t ticks;
};

struct ClockedTerminal {
    std::unique_ptr<Ubiquitty::Terminal> terminal = std::make_unique<Ubiquitty::Terminal>();
    SetClock clock;

    explicit ClockedTerminal(std::uint32_t start)
        : clock(start)
    {
        Ubiquitty::resize(*terminal, 10, 40);
        terminal->tickClock = &clock;
    }

    ~ClockedTerminal() { terminal->tickClock = nullptr; }

    std::uint32_t lastCommandDuration() const
    {
        return terminal->commandDurationTicksRing[(terminal->commandCompletedGeneration + 7) % 8];
    }
};

} // namespace

TEST(terminalNowReadsTheTerminalsOwnClock)
{
    ClockedTerminal left(100);
    ClockedTerminal right(5000);
    CHECK(Ubiquitty::terminalNow(*left.terminal).value_or(Ubiquitty::Ticks(0)).count == 100);
    CHECK(Ubiquitty::terminalNow(*right.terminal).value_or(Ubiquitty::Ticks(0)).count == 5000);
}

TEST(twoTerminalsTimeTheirCommandsByTheirOwnClocks)
{
    ClockedTerminal left(100);
    ClockedTerminal right(5000);
    feed(*left.terminal, "\x1b]133;C\x07");
    feed(*right.terminal, "\x1b]133;C\x07");
    left.clock.ticks = 160;
    right.clock.ticks = 5090;
    feed(*left.terminal, "\x1b]133;D;0\x07");
    feed(*right.terminal, "\x1b]133;D;0\x07");
    CHECK(left.lastCommandDuration() == 60);
    CHECK(right.lastCommandDuration() == 90);
}

// RIS wipes the program's state, not the frontend's wiring.
TEST(aHardResetKeepsTheClock)
{
    ClockedTerminal left(100);
    feed(*left.terminal, "\033c\x1b]133;C\x07");
    left.clock.ticks = 130;
    feed(*left.terminal, "\x1b]133;D;0\x07");
    CHECK(left.lastCommandDuration() == 30);
}

UBIQUITTY_TEST_MAIN()
