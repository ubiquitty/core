// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_sync_output_hold.cpp — the per-frame synchronised-output decision.
//
// DECSET 2026 exists so an application can compose a frame without the terminal
// showing it half-drawn. A frontend that repaints whenever bytes arrive, rather
// than asking, renders every intermediate state — which is how a benchmark that
// should print only its results ends up printing everything it wrote.
//
// The counterpart matters as much: an application that sets the mode and never
// clears it must not be able to freeze the display.

#include "Harness.h"
#include <memory>

#include "Core/Modes/SyncOutput.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"

#include <cstring>

namespace {

using namespace Ubiquitty;

std::unique_ptr<Terminal> makeTerminal()
{
    auto t = std::make_unique<Terminal>();
    Ubiquitty::resize(*t, 24, 80);
    return t;
}

void feed(Terminal& t, const char* bytes)
{
    Ubiquitty::processBytes(&t, bytes);
}

TEST(testNoBlockMeansPresentEveryFrame)
{
    auto t = makeTerminal();
    CHECK_MSG(!Modes::SyncOutput::shouldWithholdFrame(*t, Ticks(100)), "nothing is withheld outside a block");
    CHECK_MSG(!Modes::SyncOutput::shouldWithholdFrame(*t, Ticks(100000)), "still nothing, however late");
    t.reset();
}

TEST(testAnOpenBlockWithholdsFrames)
{
    auto t = makeTerminal();
    feed(*t, "\033[?2026h"); // BSU
    CHECK_MSG(Modes::SyncOutput::isActive(t.get()), "the mode is set");
    CHECK_MSG(Modes::SyncOutput::shouldWithholdFrame(*t, Ticks(10)), "the frame is withheld");
    CHECK_MSG(Modes::SyncOutput::shouldWithholdFrame(*t, Ticks(20)), "and the next one");

    feed(*t, "\033[?2026l"); // ESU
    CHECK_MSG(!Modes::SyncOutput::shouldWithholdFrame(*t, Ticks(30)), "clearing the mode presents again");
    t.reset();
}

// The watchdog. An application that crashes mid-frame leaves 2026 set forever;
// without this the display never updates again.
// The ask-back delay is the budget still unspent, never zero while a block
// is held: a frontend sleeps on the answer and comes back to find the hold
// expired by the same call that measured it. Before the stamp the whole
// budget is ahead; after, what the clock has not yet eaten.
TEST(testTheAskBackDelayIsTheUnspentBudget)
{
    auto t = makeTerminal();
    CHECK_MSG(Modes::SyncOutput::ticksUntilHoldExpires(*t, Ticks(100)) == 0, "no block, nothing to come back for");
    feed(*t, "\033[?2026h");
    CHECK_MSG(Modes::SyncOutput::ticksUntilHoldExpires(*t, Ticks(100)) == Modes::SyncOutput::maxHoldTicks + 1, "unstamped: the whole budget and the tick that expires it");
    CHECK(Modes::SyncOutput::shouldWithholdFrame(*t, Ticks(100))); // stamps
    CHECK_MSG(Modes::SyncOutput::ticksUntilHoldExpires(*t, Ticks(130)) == Modes::SyncOutput::maxHoldTicks - 30 + 1, "thirty ticks in, thirty-one to go");
    CHECK_MSG(Modes::SyncOutput::ticksUntilHoldExpires(*t, Ticks(100 + Modes::SyncOutput::maxHoldTicks + 5)) == 1, "past the budget the answer is still a sleep, not a spin");
    CHECK_MSG(!Modes::SyncOutput::shouldWithholdFrame(*t, Ticks(100 + Modes::SyncOutput::maxHoldTicks + 5)), "and that call expires it");
    CHECK_MSG(Modes::SyncOutput::ticksUntilHoldExpires(*t, Ticks(200)) == 0, "expired: nothing to come back for");
    t.reset();
}

TEST(testAStuckBlockExpires)
{
    auto t = makeTerminal();
    feed(*t, "\033[?2026h");

    CHECK_MSG(Modes::SyncOutput::shouldWithholdFrame(*t, Ticks(1000)), "the block starts holding");
    CHECK_MSG(Modes::SyncOutput::shouldWithholdFrame(*t, Ticks(1000 + Modes::SyncOutput::maxHoldTicks)), "still within budget");
    CHECK_MSG(!Modes::SyncOutput::shouldWithholdFrame(*t, Ticks(1000 + Modes::SyncOutput::maxHoldTicks + 1)), "past the budget it expires");
    CHECK_MSG(!Modes::SyncOutput::isActive(t.get()), "and the mode is dropped, not merely ignored");

    // Once expired it stays presented — a crashed application cannot re-hold it
    // without setting the mode again.
    CHECK(!Modes::SyncOutput::shouldWithholdFrame(*t, Ticks(1000 + Modes::SyncOutput::maxHoldTicks + 2)));
    t.reset();
}

// The tick counter wraps. Treating a wrap as a huge elapsed time would expire
// every block the moment it happened.
TEST(testTickWraparoundDoesNotExpireALiveBlock)
{
    auto t = makeTerminal();
    feed(*t, "\033[?2026h");
    CHECK(Modes::SyncOutput::shouldWithholdFrame(*t, Ticks(0xFFFFFF00u)));
    CHECK_MSG(Modes::SyncOutput::shouldWithholdFrame(*t, Ticks(0x00000010u)), "a wrapped tick reads as fresh, not ancient");
    CHECK(Modes::SyncOutput::isActive(t.get()));
    t.reset();
}

} // namespace

UBIQUITTY_TEST_MAIN()
