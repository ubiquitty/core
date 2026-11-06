// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Modes/Scroll.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"
#include "Harness.h"
#include "TerminalFixture.h"

using UbiquittyTest::feed;

#include <cstdio>
#include <cstring>
#include <type_traits>

// The host modes that decide what the frontend's scrollback view does. All are
// questions the frontend asks before moving the view, never state the parser
// mutates on its own.

// The conditional-mode wrapper has to cost what the bare bool cost: modes are
// read on the per-byte parse path.
static_assert(sizeof(Ubiquitty::Modes::AlternateScrollMode) == sizeof(bool));
static_assert(std::is_trivially_copyable_v<Ubiquitty::Modes::AlternateScrollMode>);

// The requested bit and the effective answer diverge, which is the reason the
// type exists. Asking for the effective value needs a Modes::ScreenBuffer, so a caller
// that has none cannot get a wrong answer by accident.
static_assert([] {
    Ubiquitty::Modes::AlternateScrollMode m {};
    m = true;
    return m.asRequested() && !m.isEffective(Ubiquitty::Modes::ScreenBuffer::Primary)
        && m.isEffective(Ubiquitty::Modes::ScreenBuffer::Alternate);
}());

// DEC 1010 (output) and 1011 (keypress) are independent: an application may want
// keystrokes to snap the view without output doing so.
TEST(testSnapTriggersAreIndependent)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);

    feed(t, "\033[?1010h\033[?1011l");
    CHECK(Ubiquitty::Modes::shouldSnapToTail(&t, Ubiquitty::Modes::SnapTrigger::Output));
    CHECK(!Ubiquitty::Modes::shouldSnapToTail(&t, Ubiquitty::Modes::SnapTrigger::Keypress));

    feed(t, "\033[?1010l\033[?1011h");
    CHECK(!Ubiquitty::Modes::shouldSnapToTail(&t, Ubiquitty::Modes::SnapTrigger::Output));
    CHECK(Ubiquitty::Modes::shouldSnapToTail(&t, Ubiquitty::Modes::SnapTrigger::Keypress));

    CHECK(!Ubiquitty::Modes::shouldSnapToTail(nullptr, Ubiquitty::Modes::SnapTrigger::Output));
}

// DEC 1007 only applies on the alternate screen. On the primary screen the wheel
// scrolls history, so translating it into cursor keys there would steal the
// user's scrollback.
TEST(testAlternateScrollRequiresAlternateScreen)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);

    feed(t, "\033[?1007h");
    CHECK(!Ubiquitty::Modes::alternateScrollActive(&t)); // mode set, but primary screen

    feed(t, "\033[?1049h"); // enter the alternate screen
    CHECK(Ubiquitty::Modes::alternateScrollActive(&t));

    feed(t, "\033[?1007l");
    CHECK(!Ubiquitty::Modes::alternateScrollActive(&t)); // alternate screen, mode off

    feed(t, "\033[?1049l");
    CHECK(!Ubiquitty::Modes::alternateScrollActive(&t));
}

// The emitted keys depend on DECCKM, which is why the caller passes the live
// mode rather than the function reading a default.
TEST(testAlternateScrollKeysFollowCursorKeyMode)
{
    char buffer[16];

    int n = Ubiquitty::Modes::alternateScrollKeys(Ubiquitty::Modes::CursorKeyMode::Normal,
        Ubiquitty::Modes::ScrollDirection::Up, buffer);
    CHECK(n > 0);
    CHECK(buffer[0] == '\033' && buffer[1] == '[');

    const int normalLength = n;
    n = Ubiquitty::Modes::alternateScrollKeys(Ubiquitty::Modes::CursorKeyMode::Application,
        Ubiquitty::Modes::ScrollDirection::Up, buffer);
    CHECK(n == normalLength);
    CHECK(buffer[0] == '\033' && buffer[1] == 'O'); // SS3, not CSI

    // Down differs from Up, so a caller cannot conflate the two.
    char down[16];
    const int d = Ubiquitty::Modes::alternateScrollKeys(Ubiquitty::Modes::CursorKeyMode::Normal,
        Ubiquitty::Modes::ScrollDirection::Down, down);
    CHECK(d > 0);
    n = Ubiquitty::Modes::alternateScrollKeys(Ubiquitty::Modes::CursorKeyMode::Normal,
        Ubiquitty::Modes::ScrollDirection::Up, buffer);
    CHECK(std::memcmp(buffer, down, static_cast<size_t>(n < d ? n : d)) != 0);
}

UBIQUITTY_TEST_MAIN()
