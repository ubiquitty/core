// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// PacedDrainHost::unlockModel donates the model lock through a fairness doorway
// when the renderer has announced a wait. std::mutex is not fair on macOS: a
// producer that is already running reacquires ahead of a waiting reader again
// and again, which is what turns a flood into a stall long enough to drop
// frames.
//
// Nothing exercised the doorway. Every unlockModel under Tests/ belongs to a
// double, and bench_drain drives DrainHostDouble whose locks are no-ops — so
// deleting the donation loop left the suite green.
//
// This test links Source/Platform/POSIX/SystemPAL.cpp (see Tests/CMakeLists.txt)
// because the doorway spins on PAL::monotonicMicros and yields through
// PAL::schedulerYield. A fake clock that advances one microsecond per call would
// turn the donation band into a fixed iteration count and measure nothing.

#include "Core/Runtime/DrainHostPacing.h"
#include "Harness.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <ranges>
#include <vector>

namespace Ubiquitty::PAL {

// A real clock, because the doorway's band is a wall-clock window: a counter
// that advances one microsecond per call would turn the donation into a fixed
// iteration count and this test would assert nothing about time. SystemPAL.cpp
// carries schedulerYield but the clock is per-platform, so it lands here.
uint32_t monotonicMicros()
{
    const auto since = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::microseconds>(since).count() & 0xFFFFFFFFull);
}

} // namespace Ubiquitty::PAL

namespace {

using namespace Ubiquitty::Runtime;

// Median of a small sample, because one unlock can be preempted by anything on
// a loaded machine and a mean carries that straight into the verdict.
double medianMicros(std::vector<double> samples)
{
    if (samples.empty())
        return 0.0;
    std::ranges::sort(samples);
    return samples[samples.size() / 2];
}

// One unlock, timed. The lock is taken and released around nothing: the
// donation happens after the release, so the body under it is irrelevant.
double unlockMicros(DrainHost& host)
{
    host.lockModel();
    const auto start = std::chrono::steady_clock::now();
    host.unlockModel();
    const auto elapsed = std::chrono::steady_clock::now() - start;
    return std::chrono::duration<double, std::micro>(elapsed).count();
}

constexpr int samplesPerArm = 9;

TEST(aPublishThrottleDoesNotEnableTimedDrainsWithoutAPresentedCapture)
{
    std::mutex lock;
    PacedDrainHost host(lock, nullptr);
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

TEST(pacedDrainHostDonatesOnlyWhenTheRendererIsWaiting)
{
    std::mutex modelLock;
    std::atomic<bool> isRenderWaiting { false };
    PacedDrainHost donating(modelLock, &isRenderWaiting);
    // A null flag is the documented way to disable the doorway, so it is the
    // control: same class, same lock, same call, donation switched off.
    PacedDrainHost plain(modelLock, nullptr);

    std::vector<double> quietMicros;
    std::vector<double> waitingMicros;
    std::vector<double> disabledMicros;
    for (int i = 0; i < samplesPerArm; ++i) {
        isRenderWaiting.store(false, std::memory_order_release);
        quietMicros.push_back(unlockMicros(donating));
        // A wait that never clears, so the loop runs to its cap rather than to
        // whatever a real renderer's timing happened to be.
        isRenderWaiting.store(true, std::memory_order_release);
        waitingMicros.push_back(unlockMicros(donating));
        disabledMicros.push_back(unlockMicros(plain));
    }
    isRenderWaiting.store(false, std::memory_order_release);

    const double quiet = medianMicros(quietMicros);
    const double waiting = medianMicros(waitingMicros);
    const double disabled = medianMicros(disabledMicros);

    char message[192];
    std::snprintf(message, sizeof(message),
        "donating %.1fus, quiet %.1fus, doorway-disabled %.1fus, cap %uus",
        waiting, quiet, disabled, renderLockDonationMicros);

    // The donation runs until renderLockDonationMicros, so this is a lower
    // bound: load can only lengthen it, never shorten it below the cap.
    CHECK_MSG(waiting >= renderLockDonationMicros / 2.0, message);

    // Both controls say the flag is what drives it, not the class: the same
    // host returns promptly with no wait announced, and a host built without
    // the doorway returns promptly even while the flag is set.
    CHECK_MSG(quiet < waiting / 2.0, message);
    CHECK_MSG(disabled < waiting / 2.0, message);
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
    std::mutex modelLock;
    PublishTally counts;
    PacedDrainHost host(modelLock, nullptr);
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
    std::mutex modelLock;

    // Two saturated passes back to back: each would copy the grid, so the
    // second sits inside the copy window and is withheld — a flood closes
    // passes far faster than any display can show them, and a copy per pass
    // would put the render's whole-grid cost onto the ingest thread instead
    // of removing it.
    PublishTally backToBack;
    PacedDrainHost host(modelLock, nullptr);
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
    PacedDrainHost settleHost(modelLock, nullptr);
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
    std::mutex modelLock;

    // Back to back, so the second call is inside the publish window by
    // construction: the floor is a 240th of a second and two calls are
    // microseconds apart. The settle is the common case — the runtime raises
    // one for every drained frame that caught up — so this ordering is what
    // an ordinary session does between two synchronized-output frames.
    PublishTally afterSettle;
    PacedDrainHost host(modelLock, nullptr);
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
    PacedDrainHost settleHost(modelLock, nullptr);
    settleHost.setPublishHook(tallyPublish, &settlesOnly);
    settleHost.onSyncFrameReadyLocked(SyncFrameReason::Settle);
    settleHost.onSyncFrameReadyLocked(SyncFrameReason::Settle);

    std::snprintf(message, sizeof(message), "settles admitted %d of 2", settlesOnly.settles);
    CHECK_MSG(settlesOnly.settles == 1, message);
}

TEST(theBurstEndPublishIsNeverWithheld)
{
    std::mutex modelLock;

    // Back to back, so the second call is inside the window by construction.
    // Every other exemption here rests on a next capture carrying what this one
    // would have; a burst's end is the moment there is no next capture, so a
    // withheld publish is not deferred but lost. nvim's showcmd is the case: the
    // key is drawn into the last row, the cell is cleared a moment later inside
    // one window, and the glyph stays on the glass until an unrelated edge asks
    // for a frame.
    PublishTally afterBoundary;
    PacedDrainHost host(modelLock, nullptr);
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
    PacedDrainHost twiceHost(modelLock, nullptr);
    twiceHost.setPublishHook(tallyPublish, &twice);
    twiceHost.onSyncFrameReadyLocked(SyncFrameReason::BurstEnd);
    twiceHost.onSyncFrameReadyLocked(SyncFrameReason::BurstEnd);
    std::snprintf(message, sizeof(message), "burst ends admitted %d of 2", twice.burstEnds);
    CHECK_MSG(twice.burstEnds == 2, message);

    // And it does not spend the copy window: a burst end copies only to replace
    // a capture nobody took, so a boundary right behind one still copies.
    PublishTally thenBoundary;
    PacedDrainHost boundaryHost(modelLock, nullptr);
    boundaryHost.setPublishHook(tallyPublish, &thenBoundary);
    boundaryHost.onSyncFrameReadyLocked(SyncFrameReason::BurstEnd);
    boundaryHost.onSyncFrameReadyLocked(SyncFrameReason::Boundary);
    std::snprintf(message, sizeof(message), "after a burst end: boundaries %d", thenBoundary.boundaries);
    CHECK_MSG(thenBoundary.boundaries == 1, message);
}

} // namespace

UBIQUITTY_TEST_MAIN()
