// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Modes/Focus.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"
#include "Harness.h"
#include "TerminalFixture.h"

using UbiquittyTest::drain;
using UbiquittyTest::feed;

#include <cstdio>
#include <cstring>

// DEC private mode 1004. The terminal answers the host with CSI I on focus and
// CSI O on blur, but only while the mode is set — an application that never
// enabled it must not receive the bytes, since they would arrive as input.

TEST(testNoReportWhenModeOff)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);

    char buffer[64];
    (void)drain(t, buffer); // clear anything from reset

    Ubiquitty::Modes::focusChanged(&t, Ubiquitty::Modes::FocusState::Focused);
    Ubiquitty::Modes::focusChanged(&t, Ubiquitty::Modes::FocusState::Unfocused);

    CHECK(drain(t, buffer) == 0); // silent, as the host never asked
}

TEST(testReportsFocusAndBlurWhenEnabled)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);

    char buffer[64];
    feed(t, "\033[?1004h");
    (void)drain(t, buffer);

    Ubiquitty::Modes::focusChanged(&t, Ubiquitty::Modes::FocusState::Focused);
    int n = drain(t, buffer);
    CHECK(n == 3 && std::memcmp(buffer, "\033[I", 3) == 0);

    Ubiquitty::Modes::focusChanged(&t, Ubiquitty::Modes::FocusState::Unfocused);
    n = drain(t, buffer);
    CHECK(n == 3 && std::memcmp(buffer, "\033[O", 3) == 0);
}

// A bell only warrants a user-visible notification when the window is unfocused;
// a focused user already saw whatever rang it.
TEST(testBellNotificationTracksFocus)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);

    Ubiquitty::Modes::focusChanged(&t, Ubiquitty::Modes::FocusState::Focused);
    CHECK(!Ubiquitty::Modes::bellWantsNotification(&t));

    Ubiquitty::Modes::focusChanged(&t, Ubiquitty::Modes::FocusState::Unfocused);
    CHECK(Ubiquitty::Modes::bellWantsNotification(&t));

    CHECK(!Ubiquitty::Modes::bellWantsNotification(nullptr));
}

UBIQUITTY_TEST_MAIN()
