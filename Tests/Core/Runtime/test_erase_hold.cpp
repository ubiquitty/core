// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Runtime/EraseHold.h"
#include "Harness.h"

#include <atomic>

// test_erase_hold.cpp — pins Runtime::EraseHold, the hold a whole-screen erase
// puts on presentation: an erase arms it, a presenter releases it once the
// redraw has settled or the watchdog has run out, a pause in the bytes settles
// it whether or not anyone asked, and an arm and a release cannot cross.
// Each interleaving is stepped by hand through the calls the runtime makes, so
// a release made between a presenter's read and an erase's arm is pinned
// exactly. Bare libc++; no platform code.

using Ubiquitty::Runtime::EraseHold;
using Ubiquitty::Runtime::eraseHoldMaxMicros;
using Ubiquitty::Runtime::eraseHoldSettleMicros;

namespace {

// Odd, as an armed start is: a start of 0 means released, so the hold stamps
// `now | 1`, and an odd clock keeps every span below exact.
constexpr std::uint32_t firstEraseMicros = 1'000'001;

} // namespace

// Before any erase nothing holds, and the model's initial generation is no
// erase.
TEST(testNothingHoldsBeforeAnErase)
{
    EraseHold hold;
    std::atomic<std::uint32_t> lastBytes { firstEraseMicros };
    CHECK(!hold.shouldWithhold(firstEraseMicros, lastBytes));
    hold.arm(0, firstEraseMicros);
    CHECK_MSG(!hold.shouldWithhold(firstEraseMicros, lastBytes), "generation 0 is the model before any erase");
    CHECK(hold.microsRemaining(firstEraseMicros, lastBytes) == 0);
}

// An erase holds the present while its redraw's bytes keep arriving, and a
// pause as long as the settle releases it; the same erase seen again does not
// arm it a second time.
TEST(testAnEraseHoldsUntilItsRedrawSettles)
{
    EraseHold hold;
    std::atomic<std::uint32_t> lastBytes { firstEraseMicros };
    hold.arm(1, firstEraseMicros);
    CHECK(hold.shouldWithhold(firstEraseMicros + 1000, lastBytes));
    CHECK(hold.microsRemaining(firstEraseMicros + 1000, lastBytes) == eraseHoldSettleMicros - 1000);
    lastBytes.store(firstEraseMicros + 4000);
    CHECK_MSG(hold.shouldWithhold(firstEraseMicros + 9000, lastBytes), "bytes still arriving keep the hold");
    const std::uint32_t settledMicros = firstEraseMicros + 4000 + eraseHoldSettleMicros;
    CHECK_MSG(!hold.shouldWithhold(settledMicros, lastBytes), "a pause as long as the settle releases it");
    hold.arm(1, settledMicros + 1000);
    CHECK_MSG(!hold.shouldWithhold(settledMicros + 1000, lastBytes), "the same erase seen again does not re-arm it");
}

// A presenter that found the hold settled must not release an erase that
// arrived between its read and its release: the release is made against the
// hold it read, the erase has moved it, and the presenter decides again with
// the bytes that erase brought.
TEST(testAReleaseAnEraseCrossedReleasesNothing)
{
    EraseHold hold;
    std::atomic<std::uint32_t> lastBytes { firstEraseMicros };
    hold.arm(1, firstEraseMicros);
    const std::uint32_t quietMicros = firstEraseMicros + eraseHoldSettleMicros;
    EraseHold::Reading reading = hold.read();
    CHECK_MSG(!EraseHold::isHolding(reading, quietMicros, lastBytes.load()), "the presenter finds the first redraw settled");
    // The drain stamps the next erase's bytes and arms it before the presenter
    // releases what it read.
    hold.stampBytes(lastBytes, quietMicros);
    hold.arm(2, quietMicros);
    CHECK_MSG(!hold.release(reading), "the release made against the settled hold finds it moved");
    CHECK_MSG(reading.generation() == 2, "and reads the hold as the second erase left it");
    CHECK_MSG(EraseHold::isHolding(reading, quietMicros + 1000, lastBytes.load()), "which holds the present for that redraw");
    CHECK_MSG(hold.shouldWithhold(quietMicros + 1000, lastBytes), "as the presenter's next ask does");
}

// A hold nobody released, because nobody asked while the bytes paused, is
// settled by the pause when the next bytes arrive. An erase among them starts a
// hold of its own: the spent one is not its deadline.
TEST(testAnEraseAfterAPauseNobodySawStartsItsOwnHold)
{
    EraseHold hold;
    std::atomic<std::uint32_t> lastBytes { firstEraseMicros };
    hold.arm(1, firstEraseMicros);
    const std::uint32_t laterMicros = firstEraseMicros + 1'000'000;
    hold.stampBytes(lastBytes, laterMicros);
    CHECK_MSG(hold.read().startMicros() == 0, "the pause released the hold nobody asked about");
    hold.arm(2, laterMicros);
    CHECK_MSG(hold.shouldWithhold(laterMicros + 1000, lastBytes), "the later erase is held");
    CHECK(hold.microsRemaining(laterMicros + 1000, lastBytes) > 0);
    lastBytes.store(laterMicros + eraseHoldMaxMicros - 2000);
    CHECK_MSG(hold.shouldWithhold(laterMicros + eraseHoldMaxMicros - 1000, lastBytes), "its watchdog runs from its own start");
    CHECK_MSG(!hold.shouldWithhold(laterMicros + eraseHoldMaxMicros, lastBytes), "and runs out one watchdog after it");
}

// Erases on every refresh inside a live hold keep its start, so a stream that
// never pauses is held for one watchdog and then presented.
TEST(testErasesInsideALiveHoldKeepItsStart)
{
    EraseHold hold;
    std::atomic<std::uint32_t> lastBytes { firstEraseMicros };
    hold.arm(1, firstEraseMicros);
    std::uint32_t generation = 1;
    for (std::uint32_t at = firstEraseMicros + 16'000; at < firstEraseMicros + eraseHoldMaxMicros; at += 16'000) {
        lastBytes.store(at);
        hold.arm(++generation, at);
        CHECK_MSG(hold.shouldWithhold(at + 1000, lastBytes), "each erase inside the watchdog is held");
    }
    lastBytes.store(firstEraseMicros + eraseHoldMaxMicros - 1000);
    CHECK_MSG(!hold.shouldWithhold(firstEraseMicros + eraseHoldMaxMicros, lastBytes), "the watchdog runs from the first erase");
}

// Bytes that never pause leave the hold standing however long its watchdog has
// run, and an erase among them joins it: a stream that erases faster than the
// settle is presented one watchdog after its first erase, asked or not, and the
// erase after that presentation starts a fresh hold.
TEST(testAnEraseAmongUnpausedBytesJoinsTheStandingHold)
{
    EraseHold hold;
    std::atomic<std::uint32_t> lastBytes { firstEraseMicros };
    hold.arm(1, firstEraseMicros);
    std::uint32_t generation = 1;
    std::uint32_t at = firstEraseMicros;
    while (at < firstEraseMicros + eraseHoldMaxMicros + 2000) {
        at += 4000;
        hold.stampBytes(lastBytes, at);
        hold.arm(++generation, at);
    }
    CHECK_MSG(hold.read().startMicros() == firstEraseMicros, "every erase joined the first one's hold");
    CHECK_MSG(hold.microsRemaining(at, lastBytes) == 0, "whose watchdog has run out");
    CHECK_MSG(!hold.shouldWithhold(at, lastBytes), "so the stream is presented");
    at += 4000;
    hold.stampBytes(lastBytes, at);
    hold.arm(++generation, at);
    CHECK_MSG(hold.shouldWithhold(at + 1000, lastBytes), "and the erase after the presentation is held afresh");
}

// A byte stamped after the presenter read its clock is a byte just now, not
// one seventy minutes old, across the clock's wrap too.
TEST(testAByteStampedAfterTheReadClockIsRecent)
{
    EraseHold hold;
    const std::uint32_t nearWrapMicros = 0xFFFF'F001u;
    std::atomic<std::uint32_t> lastBytes { nearWrapMicros + 500 };
    hold.arm(1, nearWrapMicros);
    CHECK_MSG(hold.shouldWithhold(nearWrapMicros + 100, lastBytes), "a stamp after now reads as now");
    const std::uint32_t wrappedMicros = nearWrapMicros + 8000;
    lastBytes.store(wrappedMicros - 1000);
    CHECK_MSG(hold.shouldWithhold(wrappedMicros, lastBytes), "the hold spans the wrap");
}

UBIQUITTY_TEST_MAIN()
