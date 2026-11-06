// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_sync_frame_slot — the lock-free triple-buffer exchange behind the
// session's boundary captures, held apart from the ingest thread that drives it.
//
// The property a torn frame would violate is the one under test: the capture
// target is never the buffer the drive holds, however boundaries and drains
// interleave. The frame-coherence of each individual capture is
// TerminalFrameInputs' contract and the boundary's timing is DrainHost's;
// this file holds the composition rule that lets a caller keep a drained
// frame while the ingest thread keeps capturing.

#include "Harness.h"

#include "Core/Runtime/TerminalSession.h"

#include <atomic>
#include <thread>

using Ubiquitty::Runtime::SyncFrameSlot;

TEST(aBoundaryThenADrainHandsOverTheCapturedBuffer)
{
    SyncFrameSlot slot;
    const int target = slot.captureTarget();
    CHECK(target >= 0 && target < SyncFrameSlot::slotCount);
    slot.captured(target);
    CHECK(slot.drain() == target);
    CHECK(slot.overwriteCount() == 0);
    CHECK(slot.promoteCount() == 1);
    // Nothing new: the next drain reports empty rather than re-handing a frame.
    CHECK(slot.drain() == -1);
}

TEST(boundariesBetweenDrainsCoalesceToTheNewest)
{
    SyncFrameSlot slot;
    const int first = slot.captureTarget();
    slot.captured(first);
    // The second capture fills a free slot. Only once complete does it retire
    // the older pending frame, so a failed capture cannot erase a good one.
    const int second = slot.captureTarget();
    CHECK(second != first);
    slot.captured(second);
    CHECK(slot.overwriteCount() == 1);
    CHECK(slot.drain() == second);
    CHECK(slot.promoteCount() == 1);
}

TEST(theCaptureTargetIsNeverTheBufferTheDriveHolds)
{
    SyncFrameSlot slot;
    // Steady state: capture, drain, capture, drain — the target must alternate
    // away from the drive's buffer every time, or the ingest thread rewrites a
    // frame mid-read.
    int held = -1;
    for (int cycle = 0; cycle < 8; ++cycle) {
        const int target = slot.captureTarget();
        CHECK_MSG(target != held,
            "the boundary capture must not write the buffer the drive holds — "
            "that is the torn frame the slot exists to prevent");
        slot.captured(target);
        held = slot.drain();
        CHECK(held == target);
    }
    CHECK(slot.promoteCount() == 8);
    CHECK(slot.overwriteCount() == 0);
}

TEST(aFloodOfBoundariesWithARareDrainStaysInTheFreeBuffer)
{
    SyncFrameSlot slot;
    // The drive holds a frame and stops draining; boundaries keep landing.
    const int first = slot.captureTarget();
    slot.captured(first);
    const int held = slot.drain();
    for (int boundary = 0; boundary < 5; ++boundary) {
        const int target = slot.captureTarget();
        CHECK(target != held);
        slot.captured(target);
    }
    // Five pending captures: each completed successor retired its predecessor.
    CHECK(slot.overwriteCount() == 4);
    const int drained = slot.drain();
    CHECK(drained != held);
    CHECK(slot.promoteCount() == 2);
}

TEST(aFailedCaptureLeavesTheOlderReadyFrameAvailable)
{
    SyncFrameSlot slot;
    const int first = slot.captureTarget();
    slot.captured(first);

    const int failed = slot.captureTarget();
    CHECK(failed != first);
    slot.cancelCapture(failed);

    CHECK(slot.drain() == first);
    CHECK(slot.overwriteCount() == 0);
}

TEST(theProducerAndConsumerExchangeFramesWithoutTearing)
{
    SyncFrameSlot slot;
    std::array<int, SyncFrameSlot::slotCount> payload {};
    std::atomic<bool> producerDone { false };
    std::atomic<bool> sawFailure { false };
    constexpr int frameCount = 100'000;

    std::thread producer([&] {
        for (int frame = 1; frame <= frameCount; ++frame) {
            const int target = slot.captureTarget();
            if (target < 0) {
                sawFailure.store(true, std::memory_order_relaxed);
                break;
            }
            payload[target] = frame;
            slot.captured(target);
        }
        producerDone.store(true, std::memory_order_release);
    });

    int lastFrame = 0;
    while (true) {
        int drained = slot.drain();
        if (drained < 0) {
            if (!producerDone.load(std::memory_order_acquire)) {
                std::this_thread::yield();
                continue;
            }
            // The first scan may have overlapped the producer's final Writing
            // state. The acquire above proves publication is now complete.
            drained = slot.drain();
            if (drained < 0)
                break;
        }
        const int frame = payload[drained];
        if (frame <= lastFrame)
            sawFailure.store(true, std::memory_order_relaxed);
        lastFrame = frame;
    }
    producer.join();

    CHECK(!sawFailure.load(std::memory_order_relaxed));
    CHECK(lastFrame == frameCount);
    CHECK_EQ(slot.overwriteCount() + slot.promoteCount(), static_cast<std::uint64_t>(frameCount));
}

UBIQUITTY_TEST_MAIN()
