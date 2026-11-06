// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Modes/SyncOutput.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"
#include "Harness.h"
#include "TerminalFixture.h"

using UbiquittyTest::feed;

#include <cstdio>
#include <cstring>

// DEC 2026 synchronised output. The block is age-bounded, so the frontend can
// expire it and an application that never clears the mode cannot freeze the
// display. Tick 0 is the uninitialised sentinel.

// Tick 0 is the "uninitialised" sentinel, so a first touch at tick 0 must store 1
// rather than leave the block looking un-started forever.
TEST(test_syncOutput_tick_zero_sentinel)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[?2026h");
    CHECK(Ubiquitty::Modes::SyncOutput::isActive(&t));

    Ubiquitty::Modes::SyncOutput::touch(&t, Ubiquitty::Ticks(0));
    CHECK(t.modes.syncOutputTouchTicks == 1);

    // A later touch does not re-stamp; the block ages from its FIRST touch.
    Ubiquitty::Modes::SyncOutput::touch(&t, Ubiquitty::Ticks(500));
    CHECK(t.modes.syncOutputTouchTicks == 1);

    Ubiquitty::Modes::SyncOutput::expire(&t);
    CHECK(!Ubiquitty::Modes::SyncOutput::isActive(&t));
}

UBIQUITTY_TEST_MAIN()
