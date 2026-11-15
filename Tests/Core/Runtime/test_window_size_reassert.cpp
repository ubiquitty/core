// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Runtime/WindowSizeReassert.h"
#include "Harness.h"

// test_window_size_reassert.cpp — pins Runtime::WindowSizeReassert: which
// process samples open the window after a takeover, how often it re-sends
// inside it, and when it closes. Every frontend's runtime runs this one
// schedule, so it is pinned here rather than once per arm. Bare libc++; no
// platform code.

using Ubiquitty::Runtime::ForegroundSample;
using Ubiquitty::Runtime::WindowSizeReassert;
using Ubiquitty::Runtime::windowSizeReassertSpacingMicros;
using Ubiquitty::Runtime::windowSizeReassertWindowMicros;

namespace {

constexpr ForegroundSample program(std::uint32_t group) { return { .processGroup = group, .isShell = false }; }
constexpr ForegroundSample shell(std::uint32_t group) { return { .processGroup = group, .isShell = true }; }

// Every re-send the schedule allows from `start` to `end`, asked at each
// microsecond step of `step`.
int sendsBetween(WindowSizeReassert& reassert, std::uint32_t start, std::uint32_t end, std::uint32_t step)
{
    int sends = 0;
    for (std::uint32_t now = start; now < end; now += step) {
        if (reassert.shouldSendNow(now))
            ++sends;
    }
    return sends;
}

} // namespace

// A foreground launch with stable geometry leaves Vim's intro untouched.
TEST(testATakeoverWithoutASizeChangeSendsNothing)
{
    WindowSizeReassert reassert;
    reassert.noteSample(shell(100), 0);
    reassert.noteSample(program(200), 10'000);
    CHECK(sendsBetween(reassert, 10'000, 1'500'000, 1'000) == 0);
    CHECK(!reassert.microsUntilDue(10'001).has_value());
}

TEST(testAStartupSizeChangeRetriesAfterItsOrdinaryDelivery)
{
    WindowSizeReassert reassert;
    reassert.noteSample(program(200), 10'000);
    reassert.noteSizeChange(20'000);
    CHECK(!reassert.shouldSendNow(20'000));
    CHECK(reassert.microsUntilDue(20'000) == windowSizeReassertSpacingMicros);
    CHECK(sendsBetween(reassert, 20'001, 1'500'000, 1'000) == 3);
    CHECK(!reassert.microsUntilDue(1'500'000).has_value());
}

// Sampling the takeover can follow the tab bar's resize. The missed-handler
// race still needs a retry even though the process sample came second.
TEST(testASizeChangeBeforeTheTakeoverSampleStillRetries)
{
    WindowSizeReassert reassert;
    reassert.noteSample(shell(100), 0);
    reassert.noteSizeChange(10'000);
    CHECK(!reassert.microsUntilDue(20'000).has_value());
    reassert.noteSample(program(200), 20'000);
    CHECK(reassert.shouldSendNow(260'000));
    CHECK(reassert.foregroundProcessGroup() == 200);
}

TEST(testAnOldSizeChangeDoesNotArmTheNextLaunch)
{
    WindowSizeReassert reassert;
    reassert.noteSizeChange(1);
    reassert.noteSample(program(200), 1 + windowSizeReassertWindowMicros);
    CHECK(!reassert.microsUntilDue(1 + windowSizeReassertWindowMicros).has_value());
}

TEST(testALateStartupSizeChangeGetsItsRetryWindow)
{
    WindowSizeReassert reassert;
    reassert.noteSample(program(200), 1);
    reassert.noteSizeChange(990'000);
    CHECK(reassert.shouldSendNow(1'240'000));
    reassert.noteSample(program(200), 1'250'000);
    CHECK(sendsBetween(reassert, 1'240'001, 2'500'000, 1'000) == 2);
    CHECK(!reassert.microsUntilDue(2'500'000).has_value());
}

TEST(testSteadyStateResizingDoesNotArmRetries)
{
    WindowSizeReassert reassert;
    reassert.noteSample(program(200), 1);
    reassert.noteSizeChange(2'000'000);
    CHECK(sendsBetween(reassert, 2'000'000, 3'500'000, 1'000) == 0);
}

TEST(testTheShellTakingTheTerminalBackStopsRetries)
{
    WindowSizeReassert reassert;
    reassert.noteSample(program(200), 10);
    reassert.noteSizeChange(20);
    CHECK(reassert.shouldSendNow(20 + windowSizeReassertSpacingMicros));
    reassert.noteSample(shell(100), 30 + windowSizeReassertSpacingMicros);
    CHECK(!reassert.microsUntilDue(20 + 2 * windowSizeReassertSpacingMicros).has_value());
}

TEST(testAGroupReadAsTheShellsThenAsAJobIsATakeover)
{
    WindowSizeReassert reassert;
    reassert.noteSample(shell(100), 0);
    reassert.noteSample(shell(200), 10);
    reassert.noteSizeChange(20);
    CHECK(!reassert.shouldSendNow(20));
    reassert.noteSample(program(200), 30);
    CHECK(reassert.shouldSendNow(20 + windowSizeReassertSpacingMicros));
    reassert.noteSample(program(200), 40 + windowSizeReassertSpacingMicros);
    CHECK(!reassert.microsUntilDue(20 + windowSizeReassertWindowMicros).has_value());
}

TEST(testAnUnresolvedGroupOpensNothing)
{
    WindowSizeReassert reassert;
    reassert.noteSizeChange(0);
    reassert.noteSample(program(0), 0);
    CHECK(!reassert.shouldSendNow(windowSizeReassertSpacingMicros));
    CHECK(reassert.foregroundProcessGroup() == 0);
}

TEST(testAWindowOpenedAcrossTheClockWrapStillCloses)
{
    WindowSizeReassert reassert;
    const std::uint32_t beforeWrap = 0xFFFFFFFFu - 100'000;
    reassert.noteSample(program(200), beforeWrap);
    reassert.noteSizeChange(beforeWrap);
    const std::uint32_t afterWrap = beforeWrap + windowSizeReassertSpacingMicros;
    CHECK(reassert.shouldSendNow(afterWrap));
    CHECK(!reassert.microsUntilDue(beforeWrap + windowSizeReassertWindowMicros).has_value());
}

UBIQUITTY_TEST_MAIN()
