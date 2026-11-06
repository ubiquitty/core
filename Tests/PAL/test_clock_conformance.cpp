// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_clock_conformance.cpp — every PAL's clock must run at the rate PAL.h
// promises.
//
// PAL::monotonicTicks is 60 Hz and PAL::monotonicMicros is 1 MHz. Both are
// uint32_t, so a frontend that returns milliseconds from either compiles, links,
// and runs — and every age-bounded policy in Core then expires at the wrong
// time. Milliseconds read as ticks cut synchronised-output blocks at 60 ms
// instead of a second and blink the cursor sixteen times too fast. Nothing
// fails; the terminal behaves slightly wrong, everywhere, quietly.
//
// The type system cannot distinguish a tick from a millisecond, so a test has to.
// This one links against whichever PAL the build supplies, which makes it a
// conformance check any frontend can be pointed at rather than a test of one.
//
// Bounds are loose because what is asserted is the UNIT, not the scheduler's
// precision. A CI machine under load may sleep long; it will not sleep sixteen
// times short.

#include "Harness.h"

#include "Base/Types.h"
#include "PAL/PAL.h"

#include <chrono>
#include <thread>

namespace {

constexpr int sleepMilliseconds = 250;

TEST(testTicksRunAtSixtyHertz)
{
    const Ubiquitty::Ticks before = Ubiquitty::PAL::monotonicTicks();
    std::this_thread::sleep_for(std::chrono::milliseconds(sleepMilliseconds));
    const std::uint32_t elapsed = Ubiquitty::PAL::monotonicTicks().since(before);

    // 250 ms at 60 Hz is 15 ticks. Milliseconds would read ~250; seconds ~0.
    CHECK_MSG(elapsed >= 8, "the clock advances — a constant would freeze every watchdog");
    CHECK_MSG(elapsed <= 60, "and in 60 Hz ticks, not milliseconds");
}

TEST(testMicrosRunAtOneMegahertz)
{
    const std::uint32_t before = Ubiquitty::PAL::monotonicMicros();
    std::this_thread::sleep_for(std::chrono::milliseconds(sleepMilliseconds));
    const std::uint32_t elapsed = Ubiquitty::PAL::monotonicMicros() - before;

    // 250 ms is 250,000 us. Milliseconds would read ~250.
    CHECK_MSG(elapsed >= 200000, "micros are microseconds");
    CHECK_MSG(elapsed <= 2000000, "and not nanoseconds");
}

// Both counters are uint32_t and wrap — micros every ~71 minutes. A delta must
// stay correct across a wrap, which unsigned arithmetic gives for free but a
// signed or floating intermediate silently loses.
TEST(testDeltasSurviveWraparound)
{
    const Ubiquitty::Ticks nearMax { 0xFFFFFFF0u };
    const Ubiquitty::Ticks justPast { 0x0000000Fu };
    CHECK_MSG(justPast.since(nearMax) == 0x1Fu,
        "since() is exact across the wrap, which hand-subtracting only gets right by luck");
}

// The named conversions are the point of the type: a backend holding
// milliseconds or a timespec gets the right answer without re-deriving 60/1000,
// the step where a hand-written conversion goes wrong.
TEST(testNamedConversionsCarryTheRatio)
{
    CHECK_MSG(Ubiquitty::Ticks::fromMilliseconds(1000).count == 60, "a second is 60 ticks");
    CHECK_MSG(Ubiquitty::Ticks::fromMilliseconds(0).count == 0, "and nothing is nothing");
    CHECK_MSG(Ubiquitty::Ticks::fromNanoseconds(1000000000ull).count == 60, "a second of nanoseconds too");
    CHECK_MSG(Ubiquitty::Ticks::fromTimespec(1, 0).count == 60, "and of a timespec");

    // Rounds to nearest rather than truncating, so a measurement straddling a
    // tick boundary does not always land short.
    CHECK_MSG(Ubiquitty::Ticks::fromTimespec(0, 16666667ull).count == 1, "one tick's worth is one tick");
}

// The type must cost nothing: it is passed per frame on the render path.
TEST(testTheTypeIsFree)
{
    static_assert(sizeof(Ubiquitty::Ticks) == sizeof(std::uint32_t));
    constexpr Ubiquitty::Ticks compileTime { 42 };
    static_assert(compileTime.count == 42, "usable in constant expressions");
    static_assert(Ubiquitty::Ticks::fromMilliseconds(1000).count == 60);
    CHECK(true);
}

// The two counters must describe the same clock: a frontend that backed them
// with different sources would let a policy budgeted in ticks disagree with a
// measurement taken in micros.
TEST(testBothCountersTrackOneClock)
{
    const Ubiquitty::Ticks ticksBefore = Ubiquitty::PAL::monotonicTicks();
    const std::uint32_t microsBefore = Ubiquitty::PAL::monotonicMicros();
    std::this_thread::sleep_for(std::chrono::milliseconds(sleepMilliseconds));
    const double ticksElapsed = Ubiquitty::PAL::monotonicTicks().since(ticksBefore);
    const double microsElapsed = Ubiquitty::PAL::monotonicMicros() - microsBefore;

    // ticks/60 and micros/1e6 are both seconds; they should agree within the
    // tick quantum, which is a sixtieth of a second.
    const double ticksSeconds = ticksElapsed / 60.0;
    const double microsSeconds = microsElapsed / 1000000.0;
    const double difference = ticksSeconds > microsSeconds ? ticksSeconds - microsSeconds : microsSeconds - ticksSeconds;
    CHECK_MSG(difference < 0.05, "the two counters measure the same elapsed time");
}

} // namespace

UBIQUITTY_TEST_MAIN()
