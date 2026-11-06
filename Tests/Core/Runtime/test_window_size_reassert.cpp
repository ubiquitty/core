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

// A program taking the terminal is re-sent its size at once, then every
// quarter second, and four times in all across its first second however often
// the caller asks.
TEST(testATakeoverReSendsFourTimesInItsFirstSecond)
{
    WindowSizeReassert reassert;
    reassert.noteSample(program(200), 10'000);
    CHECK_MSG(reassert.shouldSendNow(10'000), "the sample that sees the takeover re-sends at once");
    CHECK_MSG(!reassert.shouldSendNow(10'001), "and not again before the spacing");
    CHECK_MSG(reassert.microsUntilDue(10'001) == windowSizeReassertSpacingMicros - 1, "the caller's wait lands on the next re-send");
    const int later = sendsBetween(reassert, 10'002, 10'000 + windowSizeReassertWindowMicros + 500'000, 1'000);
    CHECK_MSG(later == 3, "three more re-sends, a quarter second apart, before the window closes");
    CHECK_MSG(!reassert.microsUntilDue(10'000 + windowSizeReassertWindowMicros).has_value(), "a closed window asks the caller to wake for nothing");
}

// The same program sampled again is not a takeover: re-opening the window on
// every sample would keep a program that is already running redrawing forever.
TEST(testTheSameGroupSampledAgainOpensNothing)
{
    WindowSizeReassert reassert;
    reassert.noteSample(program(200), 0);
    (void)sendsBetween(reassert, 0, windowSizeReassertWindowMicros + 1, 1'000);
    reassert.noteSample(program(200), windowSizeReassertWindowMicros + 10);
    CHECK(!reassert.shouldSendNow(windowSizeReassertWindowMicros + 10));
}

// A shell reads the size itself as it draws its prompt, and bash redraws a
// multi-line prompt on every SIGWINCH, so the shell taking the terminal back
// opens no window; the next program still does.
TEST(testTheShellTakingTheTerminalBackOpensNothing)
{
    WindowSizeReassert reassert;
    reassert.noteSample(shell(100), 0);
    CHECK_MSG(!reassert.shouldSendNow(0), "the shell at launch is not re-sent its size");
    reassert.noteSample(program(200), 10);
    CHECK(reassert.shouldSendNow(10));
    reassert.noteSample(shell(100), 20);
    CHECK_MSG(!reassert.shouldSendNow(20 + windowSizeReassertSpacingMicros), "the shell getting the terminal back closes the program's window");
    CHECK(!reassert.microsUntilDue(20 + windowSizeReassertSpacingMicros).has_value());
    reassert.noteSample(program(300), 30);
    CHECK_MSG(reassert.shouldSendNow(30), "the next program opens a window of its own");
}

// A fork that has not exec'd runs the shell's code and reads as the shell's;
// the job it becomes keeps the group, and is the takeover. A job read again
// opens nothing more.
TEST(testAGroupReadAsTheShellsThenAsAJobIsATakeover)
{
    WindowSizeReassert reassert;
    reassert.noteSample(shell(100), 0);
    reassert.noteSample(shell(200), 10);
    CHECK_MSG(!reassert.shouldSendNow(10), "the fork before its exec opens nothing");
    reassert.noteSample(program(200), 20);
    CHECK_MSG(reassert.shouldSendNow(20), "the job it became opens a window");
    CHECK(!reassert.shouldSendNow(21));
    reassert.noteSample(program(200), 22);
    CHECK_MSG(!reassert.microsUntilDue(20 + windowSizeReassertWindowMicros).has_value(), "and the same job read again does not extend its window");
}

// No resolved group is no takeover: a sample that could not read the
// foreground says nothing about who holds the terminal.
TEST(testAnUnresolvedGroupOpensNothing)
{
    WindowSizeReassert reassert;
    reassert.noteSample(program(0), 0);
    CHECK(!reassert.shouldSendNow(0));
    CHECK(reassert.foregroundProcessGroup() == 0);
}

// The runtime's clock is 32-bit microseconds and wraps every seventy minutes;
// a window opened just before the wrap still closes a second later.
TEST(testAWindowOpenedAcrossTheClockWrapStillCloses)
{
    WindowSizeReassert reassert;
    const std::uint32_t beforeWrap = 0xFFFFFFFFu - 100'000;
    reassert.noteSample(program(200), beforeWrap);
    CHECK(reassert.shouldSendNow(beforeWrap));
    const std::uint32_t afterWrap = beforeWrap + windowSizeReassertSpacingMicros;
    CHECK_MSG(reassert.shouldSendNow(afterWrap), "the spacing is measured across the wrap");
    CHECK_MSG(!reassert.microsUntilDue(beforeWrap + windowSizeReassertWindowMicros).has_value(), "and so is the window");
}

UBIQUITTY_TEST_MAIN()
