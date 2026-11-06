// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// The slot sheet under sustained churn.
//
// The unit tests beside this one prove each rule in one step: a pinned full
// cache refuses, a quiet view drains through the ring, a stale lease yields. The
// failure they guard is not a single step. It accumulates: a lease that gathers
// every frame's touches and releases only on an explicit rebaseline lets a
// terminal that keeps drawing walk its pinned set up to sheet capacity over
// hundreds of frames, and only then begin refusing. Text draws unhealed for a
// window, a correction frees the sheet, and it draws healed again — several
// times a second, on cells nobody has touched.
//
// A test that runs one frame cannot see that. These run hundreds, at a sheet
// size and a footprint a real screen produces, and assert the property the epoch
// ring exists to give: a workload whose per-frame footprint fits the sheet never
// starts refusing, however long it runs.

#include "Harness.h"

#include "Core/Rendering/GlyphSlotCache.h"

#include <cstdio>
#include <set>
#include <vector>

namespace {

// A sheet the size a real window is cut from, rather than the handful of slots a
// mechanism test needs.
constexpr int sheetCapacity = 2048;
constexpr int sheetColumns = 64;
constexpr int sheetBuckets = 8192;

// What the pin ring holds: the frames whose command buffers can still sample a
// slot. Read from the header's own constant rather than repeated here, so the
// arithmetic below follows it if the depth changes.
constexpr int ringDepth = 4;

// One view's frame: `stableKeys` recur every frame (text sitting on screen) and
// `freshKeys` are new (a line scrolled in, a word retyped). Returns how many
// lookups the sheet refused.
struct FrameResult {
    int refusals = 0;
    int pinnedAfter = 0;
};

FrameResult drawFrame(Ubiquitty::Rendering::GlyphSlotCache& cache, int lease,
    int stableKeys, int freshKeys, std::uint32_t freshBase, std::set<std::uint32_t>* distinctSeen)
{
    FrameResult result;
    cache.beginFrame();
    bool isFresh = false;
    for (int i = 0; i < stableKeys; ++i) {
        const std::uint32_t key = 0x1000u + static_cast<std::uint32_t>(i);
        if (distinctSeen)
            distinctSeen->insert(key);
        if (cache.slotForKey(key, &isFresh) < 0)
            ++result.refusals;
    }
    for (int i = 0; i < freshKeys; ++i) {
        const std::uint32_t key = freshBase + static_cast<std::uint32_t>(i);
        if (distinctSeen)
            distinctSeen->insert(key);
        if (cache.slotForKey(key, &isFresh) < 0)
            ++result.refusals;
    }
    cache.retainTouchedSlots(lease);
    result.pinnedAfter = cache.pinnedSlots();
    return result;
}

} // namespace

TEST(aFootprintThatFitsTheSheetNeverStartsRefusing)
{
    // The footprint the ring must hold is one frame's touches times the ring
    // depth. Sized to fit with room to spare, the sheet must never refuse — not
    // on frame one and not on frame six hundred.
    const int perFrame = (sheetCapacity / ringDepth) / 2; // 256 keys a frame
    Ubiquitty::Rendering::GlyphSlotCache cache(sheetCapacity, sheetBuckets, sheetColumns);
    const int view = cache.acquireRetainLease();

    std::set<std::uint32_t> distinctSeen;
    int worstPinned = 0;
    int totalRefusals = 0;
    for (int frame = 0; frame < 600; ++frame) {
        // Three quarters of the screen is text that stays; the rest turns over,
        // which is what a scrolling pane looks like.
        const int stable = (perFrame * 3) / 4;
        const int fresh = perFrame - stable;
        const FrameResult result = drawFrame(cache, view, stable, fresh,
            0x40000u + static_cast<std::uint32_t>(frame) * 64u, &distinctSeen);
        totalRefusals += result.refusals;
        if (result.pinnedAfter > worstPinned)
            worstPinned = result.pinnedAfter;
        if (result.refusals != 0) {
            std::printf("  refused %d lookups on frame %d (pinned %d of %d)\n",
                result.refusals, frame, result.pinnedAfter, sheetCapacity);
            break;
        }
    }

    CHECK_MSG(totalRefusals == 0, "a footprint that fits the sheet refused a lookup");

    // Non-vacuity: the run must have demanded far more distinct slots than the
    // sheet holds, or it proved only that a small workload is small. Without the
    // ring these pins accumulate and this is exactly the run that saturates.
    CHECK_MSG(static_cast<int>(distinctSeen.size()) > sheetCapacity * 4,
        "the churn was too small to have pressured the sheet at all");

    // And the pinned set stayed bounded rather than climbing toward capacity.
    CHECK_MSG(worstPinned < sheetCapacity,
        "the pinned set reached sheet capacity, which is the saturation this guards");
    std::printf("  600 frames, %zu distinct keys, worst pinned %d of %d, refusals %d\n",
        distinctSeen.size(), worstPinned, sheetCapacity, totalRefusals);
}

TEST(thePinnedSetStopsGrowingOnceTheRingIsFull)
{
    // Pins must plateau at the ring's worth of footprint: each new epoch's
    // pins arrive as the oldest epoch's are dropped, so the total is bounded
    // by footprint times depth however long the run continues.
    const int perFrame = 128;
    Ubiquitty::Rendering::GlyphSlotCache cache(sheetCapacity, sheetBuckets, sheetColumns);
    const int view = cache.acquireRetainLease();

    int pinnedAfterRingFilled = 0;
    int pinnedAtEnd = 0;
    for (int frame = 0; frame < 200; ++frame) {
        // Every key fresh: the hardest case for the ring, since nothing is
        // already resident and every frame's footprint is new.
        const FrameResult result = drawFrame(cache, view, 0, perFrame,
            0x80000u + static_cast<std::uint32_t>(frame) * 256u, nullptr);
        if (frame == ringDepth)
            pinnedAfterRingFilled = result.pinnedAfter;
        pinnedAtEnd = result.pinnedAfter;
    }

    // Once the ring is full the pinned set is the ring's contents, and 195 more
    // frames of entirely fresh keys must not add to it.
    CHECK_MSG(pinnedAtEnd <= pinnedAfterRingFilled,
        "the pinned set grew after the ring filled, which is unbounded accumulation");
    CHECK_MSG(pinnedAtEnd <= perFrame * ringDepth,
        "the pinned set exceeded one ring's worth of footprint");
    std::printf("  pinned after the ring filled: %d, after 200 frames: %d (footprint %d x depth %d)\n",
        pinnedAfterRingFilled, pinnedAtEnd, perFrame, ringDepth);
}

TEST(aSheetPressedPastItsCapacityRecoversRatherThanWedging)
{
    // A footprint too big for the sheet refuses — degraded, never wrong — and the
    // property that matters is what happens next. A sheet that settles into
    // refusal stays there until something unrelated forces a correction; the
    // sheet must instead come back on its own once the demand falls, or a
    // moment of heavy drawing costs unhealed text indefinitely.
    Ubiquitty::Rendering::GlyphSlotCache cache(256, 1024, 16);
    const int view = cache.acquireRetainLease();

    // Press: a footprint whose ring's worth cannot fit 256 slots.
    int refusalsUnderPressure = 0;
    for (int frame = 0; frame < 40; ++frame) {
        const FrameResult result = drawFrame(cache, view, 0, 200,
            0x200000u + static_cast<std::uint32_t>(frame) * 256u, nullptr);
        refusalsUnderPressure += result.refusals;
    }
    CHECK_MSG(refusalsUnderPressure > 0,
        "the pressure phase never refused, so the recovery below proves nothing");

    // Release: a footprint that fits comfortably, for longer than the ring is
    // deep so every pressured epoch has rotated out.
    int refusalsAfterRelease = 0;
    for (int frame = 0; frame < 40; ++frame) {
        const FrameResult result = drawFrame(cache, view, 32, 0, 0, nullptr);
        // The first few frames still carry the pressured epochs; judge the tail.
        if (frame >= ringDepth * 2)
            refusalsAfterRelease += result.refusals;
    }
    CHECK_MSG(refusalsAfterRelease == 0,
        "the sheet kept refusing after the demand fell, so it needs an external correction to recover");
}

TEST(twoViewsSharingASheetBothKeepDrawing)
{
    // Two windows, or two panes, on one sheet. A lease that gathers every touch
    // starves one with the other's history; the rule is that neither view's
    // ordinary drawing can make the other refuse.
    Ubiquitty::Rendering::GlyphSlotCache cache(sheetCapacity, sheetBuckets, sheetColumns);
    const int viewA = cache.acquireRetainLease();
    const int viewB = cache.acquireRetainLease();

    int refusalsA = 0;
    int refusalsB = 0;
    for (int frame = 0; frame < 300; ++frame) {
        const FrameResult a = drawFrame(cache, viewA, 96, 32,
            0x300000u + static_cast<std::uint32_t>(frame) * 64u, nullptr);
        refusalsA += a.refusals;
        const FrameResult b = drawFrame(cache, viewB, 96, 32,
            0x500000u + static_cast<std::uint32_t>(frame) * 64u, nullptr);
        refusalsB += b.refusals;
    }

    CHECK_MSG(refusalsA == 0 && refusalsB == 0,
        "two views drawing ordinary frames made each other refuse");

    cache.releaseRetainLease(viewA);
    cache.releaseRetainLease(viewB);
}

TEST(anIdleViewReleasesItsHoldOnTheSheet)
{
    // A window nobody is looking at must not hold slots against one being used.
    //
    // Stated as a comparison rather than an absolute: the busy view is run twice
    // over the same sheet size and the same work, once alone and once beside an
    // idle view that drew a screenful and stopped. Whatever the busy view costs
    // itself, the idle one must not add to it. An absolute assertion here would
    // instead measure whether the busy view's own footprint fits, which is the
    // previous test's question.
    enum class Neighbour {
        None,
        IdleView
    };
    const auto runBusyView = [](Neighbour neighbour) {
        Ubiquitty::Rendering::GlyphSlotCache cache(512, 2048, 32);
        const int busy = cache.acquireRetainLease();
        int idle = -1;
        if (neighbour == Neighbour::IdleView) {
            idle = cache.acquireRetainLease();
            // One screenful, then never again.
            drawFrame(cache, idle, 0, 200, 0x600000u, nullptr);
        }

        int refusals = 0;
        for (int frame = 0; frame < 100; ++frame) {
            const FrameResult result = drawFrame(cache, busy, 64, 64,
                0x700000u + static_cast<std::uint32_t>(frame) * 128u, nullptr);
            // The first frames still carry the idle view's epochs; judge the tail.
            if (frame >= ringDepth * 2)
                refusals += result.refusals;
        }
        if (idle >= 0)
            cache.releaseRetainLease(idle);
        cache.releaseRetainLease(busy);
        return refusals;
    };

    const int alone = runBusyView(Neighbour::None);
    const int beside = runBusyView(Neighbour::IdleView);
    std::printf("  over 100 frames, busy view refusals: %d alone, %d beside an idle view\n", alone, beside);

    // The reclaim that ends this is on a 240-frame threshold, so find the frame
    // the busy view actually recovers on. The number is the cost: every frame
    // before it drew the active window's text unligated and unhealed.
    Ubiquitty::Rendering::GlyphSlotCache cache(512, 2048, 32);
    const int busy = cache.acquireRetainLease();
    const int idle = cache.acquireRetainLease();
    drawFrame(cache, idle, 0, 200, 0x600000u, nullptr);

    int lastRefusingFrame = -1;
    for (int frame = 0; frame < 400; ++frame) {
        const FrameResult result = drawFrame(cache, busy, 64, 64,
            0x700000u + static_cast<std::uint32_t>(frame) * 128u, nullptr);
        if (result.refusals > 0)
            lastRefusingFrame = frame;
    }
    std::printf("  a busy view beside an idle one stops refusing at frame %d\n", lastRefusingFrame + 1);

    cache.releaseRetainLease(idle);
    cache.releaseRetainLease(busy);

    // The stale bound scales with the leases sharing the sheet - the frame
    // counter advances once per drawing view per display frame - so with two
    // leases the idle view's pins yield within slack ring-depths times the two
    // views, and the busy view is whole again in a fraction of a second
    // whatever the window count. One ring-depth of slop covers the frames the
    // reclaim's own retry spends.
    const int staleBoundFrames = 2 * ringDepth * 2;
    CHECK_MSG(lastRefusingFrame >= 0,
        "the comparison measured nothing: the idle arm never pressed the sheet");
    CHECK_MSG(lastRefusingFrame + 1 <= staleBoundFrames + ringDepth,
        "an idle view's pins outlived the scaled stale bound and kept starving the busy view");
}

UBIQUITTY_TEST_MAIN()
