// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_clock_pin.cpp — the macOS clock's determinism hook answers the
// environment as it stands at the call.
//
// UBIQUITTY_FAKE_TICKS pins PAL::monotonicTicks so a headless run reproduces a
// blink phase across separate processes, which is what makes the offscreen
// snapshot goldens byte-stable. The hook is only worth the line if it holds
// under the two ways a caller reaches it: a process that touches the clock
// before it sets the variable, and a harness that moves the pin between frames.
// A backend that reads the variable once into a `static` satisfies neither —
// the first call latches whatever the environment held then, and every later
// call answers from that latch. Nothing fails loudly; the pin is ignored for the
// life of the process and the goldens it was protecting drift.
//
// Linked against Source/Platform/Apple/Clock.cpp, the one backend that carries
// the hook. The other PALs have no FAKE_TICKS at all, so this is a test of that
// backend rather than a conformance check every frontend answers — the rate
// conformance any PAL must meet lives in test_clock_conformance.cpp.

#include "Harness.h"

#include "Base/Types.h"
#include "PAL/PAL.h"

#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>

namespace {

// Restores the variable to whatever the process started with, so a case that
// pins the clock cannot decide what a later one reads.
class ScopedFakeTicks {
public:
    ScopedFakeTicks()
    {
        if (const char* value = ::getenv("UBIQUITTY_FAKE_TICKS")) {
            m_hadValue = true;
            m_value = value;
        }
    }

    ~ScopedFakeTicks()
    {
        if (m_hadValue)
            ::setenv("UBIQUITTY_FAKE_TICKS", m_value.c_str(), 1);
        else
            ::unsetenv("UBIQUITTY_FAKE_TICKS");
    }

    ScopedFakeTicks(const ScopedFakeTicks&) = delete;
    ScopedFakeTicks& operator=(const ScopedFakeTicks&) = delete;

private:
    bool m_hadValue = false;
    std::string m_value;
};

// The order is the whole case: read the clock while no pin exists. A backend
// that caches the lookup answers every later call from that first miss, which
// is the state any process that consults the clock during start-up leaves
// behind — the snapshot binary sets the variable inside its own run, well after
// something has asked the time.
TEST(testAPinSetAfterTheFirstReadStillTakes)
{
    const ScopedFakeTicks restore;
    ::unsetenv("UBIQUITTY_FAKE_TICKS");

    // The first read, with no pin in the environment.
    (void)Ubiquitty::PAL::monotonicTicks();

    ::setenv("UBIQUITTY_FAKE_TICKS", "1234", 1);
    CHECK_MSG(Ubiquitty::PAL::monotonicTicks().count == 1234u,
        "a pin set after the clock has already been read is ignored by a cached lookup");
}

// A harness that steps the phase between frames writes the variable repeatedly
// and expects each write to be the next answer.
TEST(testThePinMovesBetweenReads)
{
    const ScopedFakeTicks restore;

    ::setenv("UBIQUITTY_FAKE_TICKS", "7", 1);
    CHECK(Ubiquitty::PAL::monotonicTicks().count == 7u);

    ::setenv("UBIQUITTY_FAKE_TICKS", "32", 1);
    CHECK_MSG(Ubiquitty::PAL::monotonicTicks().count == 32u,
        "the second pin is the second answer, so a harness can step the blink phase");

    ::setenv("UBIQUITTY_FAKE_TICKS", "0", 1);
    CHECK_MSG(Ubiquitty::PAL::monotonicTicks().count == 0u,
        "zero is a pin like any other, not an absent one");
}

// Clearing the variable hands the caller back the real clock rather than the
// last pinned value: the hook is an override, not a latch.
TEST(testClearingThePinRestoresTheRunningClock)
{
    const ScopedFakeTicks restore;

    ::setenv("UBIQUITTY_FAKE_TICKS", "500", 1);
    CHECK(Ubiquitty::PAL::monotonicTicks().count == 500u);

    ::unsetenv("UBIQUITTY_FAKE_TICKS");
    const Ubiquitty::Ticks before = Ubiquitty::PAL::monotonicTicks();
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    const std::uint32_t elapsed = Ubiquitty::PAL::monotonicTicks().since(before);

    // 120 ms at 60 Hz is about 7 ticks. The bound is loose because what is
    // asserted is that the clock is running again, not the scheduler's
    // precision; a frozen clock reports zero and a stale pin reports nothing
    // that moves.
    CHECK_MSG(before.count != 500u, "the cleared pin is not still the answer");
    CHECK_MSG(elapsed >= 1, "the real clock advances once the pin is gone");
}

} // namespace

UBIQUITTY_TEST_MAIN()
