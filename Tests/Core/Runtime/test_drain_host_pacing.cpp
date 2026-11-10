// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// PacedDrainHost::unlockModel releases the model lock through
// ModelLock::unlockForDrain, which hands the lock to a waiting thread, and
// counts what each handoff cost the drain. Tests/Core/test_model_lock.cpp
// bounds the waits themselves; this file checks the host reaches the handoff
// and keeps its accounts, and the publish throttle.
//
// This test links Source/Platform/POSIX/SystemPAL.cpp (see Tests/CMakeLists.txt)
// because the handoff spins on PAL::monotonicMicros and yields through
// PAL::schedulerYield.

#include "Core/Runtime/DrainHostPacing.h"
#include "Harness.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <thread>

namespace Ubiquitty::PAL {

// A real clock, because the handoff's band is a wall-clock window.
uint32_t monotonicMicros()
{
    const auto since = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::microseconds>(since).count() & 0xFFFFFFFFull);
}

} // namespace Ubiquitty::PAL

namespace {

using namespace Ubiquitty::Runtime;
using Ubiquitty::ModelLock;

TEST(aPublishThrottleDoesNotEnableTimedDrainsWithoutAPresentedCapture)
{
    ModelLock lock;
    PacedDrainHost host(lock);
    CHECK(host.frameIntervalMicroseconds() > 0);
    CHECK_EQ(host.drainPassIntervalMicroseconds(), 0u);
    host.setPublishMinInterval(0.01);
    CHECK_EQ(host.drainPassIntervalMicroseconds(), 0u);
    host.setDrainPassPresentation(PanePresentation::Presented);
    CHECK_EQ(host.drainPassIntervalMicroseconds(), 10'000u);
    host.setDrainPassPresentation(PanePresentation::Hidden);
    CHECK_EQ(host.drainPassIntervalMicroseconds(), 0u);
    CHECK_EQ(host.frameIntervalMicroseconds(), 10'000u);
    host.setDrainPassPresentation(PanePresentation::Presented);
    CHECK_EQ(host.drainPassIntervalMicroseconds(), 10'000u);
}

// What one waiter cost a host whose drain releases the lock and takes it
// straight back, as a flood's drain does, until the waiter holds it.
struct HostHandoff {
    int releaseCount = 0;
    // Releases that counted nothing although a timed-out one had found the
    // waiter counted; the waiter stays counted until it holds the lock.
    int uncountedAfterTimeoutCount = 0;
    bool wasTaken = false;
    bool isHeldByWaiterAfterTaken = false;
    std::uint64_t donationCount = 0;
    std::uint64_t donationTimeoutCount = 0;
    std::uint64_t donationMicros = 0;
};

// Releases one waiter may cost. A waiter the scheduler runs late misses a
// band and stays counted, so a later release hands the lock over; the bound
// only ends a case whose releases never do.
constexpr int releasesPerWaiter = 1000;
// Fresh waiters a case may try before one is handed the lock. A waiter that
// takes the free lock in the instant between a timed-out release and the
// drain's re-take was not handed it, and the case tries the next one.
constexpr int waiterTrials = 8;

// Drives the host's release and the lock's primitives in an order no one
// scope holds, as the drain and a waiter interleave them, which the
// thread-safety analysis does not follow.
HostHandoff handOffToOneWaiter(ModelLock& modelLock) UBIQUITTY_NO_THREAD_SAFETY_ANALYSIS
{
    PacedDrainHost host(modelLock);
    HostHandoff result;
    std::atomic<bool> isReleasing { false };
    host.lockModel();
    std::thread waiter([&] {
        // A plain guard, as the session's members take the lock.
        const std::lock_guard hold(modelLock);
        while (!isReleasing.load(std::memory_order_acquire)) {
        }
    });
    // Long enough for the waiter to find the lock held and block on it.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    bool isDrainHolding = true;
    while (result.releaseCount < releasesPerWaiter) {
        const std::uint64_t donationsBefore = host.donationCount();
        const std::uint64_t timeoutsBefore = host.donationTimeoutCount();
        host.unlockModel();
        isDrainHolding = false;
        ++result.releaseCount;
        const bool didDonate = host.donationCount() != donationsBefore;
        const bool didTimeOut = host.donationTimeoutCount() != timeoutsBefore;
        if (didDonate && !didTimeOut) {
            result.wasTaken = true;
            result.isHeldByWaiterAfterTaken = !modelLock.try_lock();
            if (!result.isHeldByWaiterAfterTaken)
                modelLock.unlock();
            break;
        }
        if (!didDonate && host.donationTimeoutCount())
            ++result.uncountedAfterTimeoutCount;
        // A try, because a waiter that took the free lock after the band ran
        // out holds it until released.
        if (!modelLock.try_lock())
            break;
        isDrainHolding = true;
    }
    if (isDrainHolding)
        modelLock.unlock();
    isReleasing.store(true, std::memory_order_release);
    waiter.join();
    result.donationCount = host.donationCount();
    result.donationTimeoutCount = host.donationTimeoutCount();
    result.donationMicros = host.donationMicros();
    return result;
}

TEST(theHostHandsTheLockToAWaiterAndCountsOnlyThatRelease)
{
    ModelLock modelLock;

    // Nobody waits: the release hands nothing off and counts nothing.
    {
        PacedDrainHost host(modelLock);
        host.lockModel();
        host.unlockModel();
        CHECK_EQ(host.donationCount(), 0u);
    }

    HostHandoff handoff;
    int trialCount = 0;
    while (trialCount < waiterTrials && !handoff.wasTaken) {
        handoff = handOffToOneWaiter(modelLock);
        ++trialCount;
        // Every release that found the waiter counted is counted, and one that
        // timed out spent the whole band.
        CHECK_EQ(handoff.uncountedAfterTimeoutCount, 0);
        CHECK(handoff.donationMicros >= handoff.donationTimeoutCount * Ubiquitty::modelLockHandoffMicroseconds);
    }

    char message[192];
    std::snprintf(message, sizeof(message), "%d waiters; the last cost %d releases: handoffs %llu, timeouts %llu, %lluus",
        trialCount, handoff.releaseCount, static_cast<unsigned long long>(handoff.donationCount),
        static_cast<unsigned long long>(handoff.donationTimeoutCount), static_cast<unsigned long long>(handoff.donationMicros));
    std::printf("%s\n", message);
    CHECK_MSG(handoff.wasTaken, message);
    CHECK_MSG(handoff.isHeldByWaiterAfterTaken, message);
    CHECK_MSG(handoff.donationCount - handoff.donationTimeoutCount == 1, message);
}

// What the publish hook was handed, by reason.
struct PublishTally {
    int boundaries = 0;
    int settles = 0;
    int saturatedPasses = 0;
    int burstEnds = 0;
    int continuingPasses = 0;
};

// A publish hook that only counts, so the assertion is about what reached it.
void tallyPublish(void* context, SyncFrameReason reason)
{
    auto* counts = static_cast<PublishTally*>(context);
    switch (reason) {
    case SyncFrameReason::Boundary:
        ++counts->boundaries;
        break;
    case SyncFrameReason::Settle:
        ++counts->settles;
        break;
    case SyncFrameReason::SaturatedPass:
        ++counts->saturatedPasses;
        break;
    case SyncFrameReason::ContinuingPass:
        ++counts->continuingPasses;
        break;
    case SyncFrameReason::BurstEnd:
        ++counts->burstEnds;
        break;
    }
}

TEST(aContinuingPassSharesTheCaptureBudget)
{
    ModelLock modelLock;
    PublishTally counts;
    PacedDrainHost host(modelLock);
    host.setPublishHook(tallyPublish, &counts);
    host.onSyncFrameReadyLocked(SyncFrameReason::Settle);
    host.onSyncFrameReadyLocked(SyncFrameReason::ContinuingPass);
    host.onSyncFrameReadyLocked(SyncFrameReason::ContinuingPass);
    host.onSyncFrameReadyLocked(SyncFrameReason::SaturatedPass);
    host.onSyncFrameReadyLocked(SyncFrameReason::Boundary);
    CHECK_EQ(counts.settles, 1);
    CHECK_EQ(counts.continuingPasses, 1);
    CHECK_EQ(counts.saturatedPasses, 0);
    CHECK_EQ(counts.boundaries, 0);
}

TEST(aSaturatedPassIsThrottledAsACopyAndReachesTheHookAsItself)
{
    ModelLock modelLock;

    // Two saturated passes back to back: each would copy the grid, so the
    // second sits inside the copy window and is withheld — a flood closes
    // passes far faster than any display can show them, and a copy per pass
    // would put the render's whole-grid cost onto the ingest thread instead
    // of removing it.
    PublishTally backToBack;
    PacedDrainHost host(modelLock);
    host.setPublishHook(tallyPublish, &backToBack);
    host.onSyncFrameReadyLocked(SyncFrameReason::SaturatedPass);
    host.onSyncFrameReadyLocked(SyncFrameReason::SaturatedPass);

    char message[192];
    std::snprintf(message, sizeof(message),
        "saturated passes admitted %d of 2, boundaries %d, settles %d",
        backToBack.saturatedPasses, backToBack.boundaries, backToBack.settles);
    CHECK_MSG(backToBack.saturatedPasses == 1, message);
    // The hook is told what it was handed. A consumer that counts boundaries
    // for its synchronized-output telemetry must not see a flood as a run of
    // DECSET-2026 commits.
    CHECK_MSG(backToBack.boundaries == 0, message);

    // A settle spends the announce window and nothing else, so a saturated
    // pass right behind it still copies: the settle is the common case, and
    // if it could strand the copy the flood's first frame would wait a whole
    // window for a frame the drain had already announced.
    PublishTally afterSettle;
    PacedDrainHost settleHost(modelLock);
    settleHost.setPublishHook(tallyPublish, &afterSettle);
    settleHost.onSyncFrameReadyLocked(SyncFrameReason::Settle);
    settleHost.onSyncFrameReadyLocked(SyncFrameReason::SaturatedPass);

    std::snprintf(message, sizeof(message),
        "after a settle: saturated passes %d, settles %d", afterSettle.saturatedPasses, afterSettle.settles);
    CHECK_MSG(afterSettle.saturatedPasses == 1, message);
    CHECK_MSG(afterSettle.settles == 1, message);
}

TEST(aBoundaryIsNotThrottledBehindASettle)
{
    ModelLock modelLock;

    // Back to back, so the second call is inside the publish window by
    // construction: the floor is a 240th of a second and two calls are
    // microseconds apart. The settle is the common case — the runtime raises
    // one for every drained frame that caught up — so this ordering is what
    // an ordinary session does between two synchronized-output frames.
    PublishTally afterSettle;
    PacedDrainHost host(modelLock);
    host.setPublishHook(tallyPublish, &afterSettle);
    host.onSyncFrameReadyLocked(SyncFrameReason::Settle);
    host.onSyncFrameReadyLocked(SyncFrameReason::Boundary);

    char message[192];
    std::snprintf(message, sizeof(message),
        "boundaries %d, settles %d, floor %uus",
        afterSettle.boundaries, afterSettle.settles, syncPublishMinIntervalFloorMicros);

    // A settle captures nothing but still spends the window. If it can strand
    // the boundary behind it, the frame is gone: the boundary's grid does not
    // survive the next byte, and no later call goes back for it.
    CHECK_MSG(afterSettle.boundaries == 1, message);

    // The control: two settles back to back must still publish once. A
    // throttle that admitted every call would satisfy the assertion above as
    // well as the exemption does, so without this the test cannot tell the
    // boundary being let through from the window never holding anything back.
    PublishTally settlesOnly;
    PacedDrainHost settleHost(modelLock);
    settleHost.setPublishHook(tallyPublish, &settlesOnly);
    settleHost.onSyncFrameReadyLocked(SyncFrameReason::Settle);
    settleHost.onSyncFrameReadyLocked(SyncFrameReason::Settle);

    std::snprintf(message, sizeof(message), "settles admitted %d of 2", settlesOnly.settles);
    CHECK_MSG(settlesOnly.settles == 1, message);
}

TEST(theBurstEndPublishIsNeverWithheld)
{
    ModelLock modelLock;

    // Back to back, so the second call is inside the window by construction.
    // Every other exemption here rests on a next capture carrying what this one
    // would have; a burst's end is the moment there is no next capture, so a
    // withheld publish is not deferred but lost. nvim's showcmd is the case: the
    // key is drawn into the last row, the cell is cleared a moment later inside
    // one window, and the glyph stays on the glass until an unrelated edge asks
    // for a frame.
    PublishTally afterBoundary;
    PacedDrainHost host(modelLock);
    host.setPublishHook(tallyPublish, &afterBoundary);
    host.onSyncFrameReadyLocked(SyncFrameReason::Boundary);
    host.onSyncFrameReadyLocked(SyncFrameReason::BurstEnd);

    char message[192];
    std::snprintf(message, sizeof(message), "boundaries %d, burst ends %d, floor %uus",
        afterBoundary.boundaries, afterBoundary.burstEnds, syncPublishMinIntervalFloorMicros);
    CHECK_MSG(afterBoundary.burstEnds == 1, message);

    // Two of them, for the one-pane case where a stream stops twice inside a
    // window: neither is the flood the window exists to pace.
    PublishTally twice;
    PacedDrainHost twiceHost(modelLock);
    twiceHost.setPublishHook(tallyPublish, &twice);
    twiceHost.onSyncFrameReadyLocked(SyncFrameReason::BurstEnd);
    twiceHost.onSyncFrameReadyLocked(SyncFrameReason::BurstEnd);
    std::snprintf(message, sizeof(message), "burst ends admitted %d of 2", twice.burstEnds);
    CHECK_MSG(twice.burstEnds == 2, message);

    // And it does not spend the copy window: a burst end copies only to replace
    // a capture nobody took, so a boundary right behind one still copies.
    PublishTally thenBoundary;
    PacedDrainHost boundaryHost(modelLock);
    boundaryHost.setPublishHook(tallyPublish, &thenBoundary);
    boundaryHost.onSyncFrameReadyLocked(SyncFrameReason::BurstEnd);
    boundaryHost.onSyncFrameReadyLocked(SyncFrameReason::Boundary);
    std::snprintf(message, sizeof(message), "after a burst end: boundaries %d", thenBoundary.boundaries);
    CHECK_MSG(thenBoundary.boundaries == 1, message);
}

} // namespace

UBIQUITTY_TEST_MAIN()
