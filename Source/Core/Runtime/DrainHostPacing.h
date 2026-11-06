// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/Runtime/TerminalRuntime.h"
#include "PAL/PAL.h"

#include <atomic>
#include <mutex>

// Source/Core/Runtime/DrainHostPacing.h — the measured pacing every drain host
// carries: the unlock-side donation to a waiting render snapshot, and the
// sync-frame publish throttle. One copy every frontend's host consumes rather
// than re-deriving the tuning: every constant here was swept, and two copies of
// a swept number diverge the first time one is re-tuned.

namespace Ubiquitty::Runtime {

// Bound on the unlock-side donation to a waiting render snapshot: long enough
// to cover the woken main thread's wake latency, short enough that a stalled
// main thread cannot wedge the drain.
inline constexpr std::uint32_t renderLockDonationMicros = 150;

// The load-only probe band in front of the donation's yield tail: the
// renderer usually acquires within a few microseconds on another core, so
// this covers the common wake with ZERO syscalls; only the pathological
// same-core tail still yields (the one case the scheduler hint helps).
inline constexpr std::uint32_t renderLockDonationProbeMicros = 20;

// Floor on the sync-frame publish interval, below which publishing every ESU
// boundary outruns any display. Microseconds, because the throttle's clock is
// the wrapping monotonic-micros counter and an unsigned delta is wrap-safe
// where a floating-point timestamp is not.
inline constexpr std::uint32_t syncPublishMinIntervalFloorMicros = 1'000'000 / 240;

/// A drain host carrying the pacing a live pane needs.
///
/// `lockModel`/`unlockModel` wrap the pane's model lock with the fairness
/// doorway: `std::mutex` does no handoff, and the flood paths re-acquire
/// within nanoseconds of unlocking, so a parked main thread could starve for
/// whole drain passes, long enough to drop frames. When the renderer has
/// announced a wait, the unlock pauses until the renderer takes the lock,
/// probing load-only first and yielding only past the probe band. The flag
/// is false on every non-contended unlock, so the steady-state cost is one
/// relaxed load.
///
/// `onSyncFrameReadyLocked` throttles the publish hook to the display's
/// cadence: the interval arrives from the frontend on display-rate edges, and
/// a burst completing many DECSET-2026 frames publishes the newest at most
/// once per interval — the only frame still capturable.
///
/// The hook is a function pointer and a context rather than any closure type,
/// like the runtime's raw-byte tap: the call happens under the model lock on
/// the writer thread, and a shape that can allocate or retain has no place
/// there.
class PacedDrainHost : public DrainHost {
public:
    /// The publish hook's shape: called under the model lock at a completed
    /// sync frame, at most once per publish interval. The reason rides along so
    /// a hook can capture a boundary and merely announce a settle.
    using PublishHook = void (*)(void* context, SyncFrameReason reason);

    /// Prepares a host over a pane's lock.
    ///
    /// - Parameters:
    ///   - modelLock: The pane's model lock, owned by the caller.
    ///   - isRenderWaiting: The renderer's announced wait, owned by the
    ///     caller; null disables the donation doorway.
    PacedDrainHost(std::mutex& modelLock, std::atomic<bool>* isRenderWaiting)
        : m_modelLock(&modelLock)
        , m_isRenderWaiting(isRenderWaiting)
    {
    }

    /// Points the host at another pane's lock.
    ///
    /// The tmux render adapter borrows a Session-owned Terminal, and every
    /// bracket — render, input, deliver — has to take the one canonical lock
    /// that Session owns rather than this pane's.
    ///
    /// - Important: Call only while no loop is draining through this host;
    ///   the pointer is read unsynchronized on the writer thread.
    ///
    /// - Parameter modelLock: The lock every bracket takes, owned by the
    ///   caller and outliving this host's use of it.
    void retargetModelLock(std::mutex& modelLock) { m_modelLock = &modelLock; }

    /// Installs the publish hook the throttle guards.
    ///
    /// - Parameters:
    ///   - hook: The hook, or null to publish nothing.
    ///   - context: Handed back to the hook verbatim.
    void setPublishHook(PublishHook hook, void* context)
    {
        m_publishHook = hook;
        m_publishContext = context;
    }

    /// Sets the minimum interval between sync-frame publishes.
    ///
    /// Written by the frontend on display-rate edges, read by the writer
    /// thread per completed frame.
    ///
    /// - Parameter seconds: The interval; clamped to the floor.
    void setPublishMinInterval(double seconds)
    {
        const double micros = seconds * 1'000'000.0;
        m_publishMinIntervalMicros.store(
            micros > syncPublishMinIntervalFloorMicros ? static_cast<std::uint32_t>(micros) : syncPublishMinIntervalFloorMicros,
            std::memory_order_relaxed);
    }

    /// Returns the minimum interval between sync-frame publishes, the cadence
    /// the coherence rule paces this pane's frames to.
    ///
    /// - Returns: Microseconds, never below the floor.
    [[nodiscard]] std::uint32_t frameIntervalMicroseconds() const override { return m_publishMinIntervalMicros.load(std::memory_order_relaxed); }

    /// Enables timed drain passes when the owner has armed a presented capture.
    ///
    /// The ordinary publish throttle remains active without a local renderer.
    ///
    /// - Parameter presentation: Presented only when a local capture is armed
    ///   and its surface can present it.
    void setDrainPassPresentation(PanePresentation presentation)
    {
        m_isDrainPassPresented.store(presentation == PanePresentation::Presented, std::memory_order_relaxed);
    }

    /// Returns the pass budget for an armed, presented capture.
    ///
    /// - Returns: The publish interval, or zero without a presentation consumer.
    [[nodiscard]] std::uint32_t drainPassIntervalMicroseconds() const override
    {
        return m_isDrainPassPresented.load(std::memory_order_relaxed) ? frameIntervalMicroseconds() : 0;
    }

    /// Acquires the pane's model lock for the drain.
    void lockModel() override { m_modelLock->lock(); }

    /// Releases the model lock, donating through the fairness doorway when
    /// the renderer has announced a wait.
    void unlockModel() override
    {
        m_modelLock->unlock();
        if (m_isRenderWaiting && m_isRenderWaiting->load(std::memory_order_acquire)) {
            // Hybrid donation (was an unconditional yield loop: ~75
            // sched_yield syscalls per 150 us window): probe load-only first,
            // yield only past the probe band.
            m_donationCount.fetch_add(1, std::memory_order_relaxed);
            const std::uint32_t start = PAL::monotonicMicros();
            std::uint32_t waited = 0;
            while (m_isRenderWaiting->load(std::memory_order_acquire)) {
                waited = static_cast<std::uint32_t>(PAL::monotonicMicros() - start);
                if (waited >= renderLockDonationMicros) {
                    // The renderer never took it inside the window. A donation
                    // that ends this way bought nothing and spent the whole
                    // band, so the two counts are kept apart: a high ratio here
                    // means the drain is paying for a wake that is not coming.
                    m_donationTimeoutCount.fetch_add(1, std::memory_order_relaxed);
                    break;
                }
                if (waited >= renderLockDonationProbeMicros)
                    PAL::schedulerYield();
            }
            m_donationMicros.fetch_add(waited, std::memory_order_relaxed);
        }
    }

    /// Publishes a completed sync frame through the throttled hook.
    ///
    /// Each kind of call is throttled against its own last call, because the
    /// two kinds bound different costs.
    ///
    /// A boundary copies the grid, so its window bounds how often a frame is
    /// copied; a saturated pass copies for the same reason and shares that
    /// window. A settle copies nothing — it announces, so the render wakes and
    /// the live route captures the grid itself — so its window bounds how often
    /// the render is woken. Sharing one stamp makes each throttle the other: a
    /// settle charged to the copy window strands the boundary behind it, which
    /// is the call that would have copied; and a boundary charged to the wake
    /// window would silence the announcements a reader is waiting on.
    ///
    /// A boundary throttled behind another boundary loses nothing, because the
    /// next capture carries both bodies. A boundary throttled behind a settle
    /// loses the copy the settle never made.
    ///
    /// Every one of those rests on there being a next capture, which a burst's
    /// end is exactly the moment there is not: the stream has stopped, so a
    /// publish withheld here is not deferred but lost, and whatever it carried
    /// stays off the screen until an unrelated edge asks for a frame. So the
    /// burst-end publish is never withheld. It is also the cheap one to let
    /// through -- there is no flood left to pace, which is what the window is
    /// for.
    ///
    /// - Parameter reason: Why the frame was announced ready.
    void onSyncFrameReadyLocked(SyncFrameReason reason) override
    {
        if (!m_publishHook)
            return;
        const std::uint32_t now = PAL::monotonicMicros();
        const bool isBoundary = reason == SyncFrameReason::Boundary;
        // A burst end is counted with the announcements even though it may copy:
        // it copies only to replace a capture nobody took, which is a once-per-
        // burst event at rest, and charging it to the copy window would throttle
        // the next real boundary behind a call the window does not exist to
        // bound.
        const bool isCopy = reason == SyncFrameReason::Boundary || reason == SyncFrameReason::SaturatedPass
            || reason == SyncFrameReason::ContinuingPass;
        if (isBoundary)
            m_boundaryRequestCount.fetch_add(1, std::memory_order_relaxed);
        const std::uint32_t sinceItsOwn = isCopy ? now - m_lastCopyMicros : now - m_lastAnnounceMicros;
        if (reason != SyncFrameReason::BurstEnd
            && sinceItsOwn < m_publishMinIntervalMicros.load(std::memory_order_relaxed)) {
            if (isBoundary)
                m_boundaryThrottledCount.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        m_publishHook(m_publishContext, reason);
        m_lastAnnounceMicros = now;
        if (isCopy)
            m_lastCopyMicros = now;
    }

    /// Boundaries the drain asked to publish, counted before the throttle.
    [[nodiscard]] std::uint64_t boundaryRequestCount() const { return m_boundaryRequestCount.load(std::memory_order_relaxed); }

    /// Boundaries the throttle withheld for arriving inside the publish window.
    [[nodiscard]] std::uint64_t boundaryThrottledCount() const { return m_boundaryThrottledCount.load(std::memory_order_relaxed); }

    /// How many unlocks found a renderer waiting and donated the lock to it.
    [[nodiscard]] std::uint64_t donationCount() const { return m_donationCount.load(std::memory_order_relaxed); }

    /// How many of those ran the whole band without the renderer taking it.
    [[nodiscard]] std::uint64_t donationTimeoutCount() const { return m_donationTimeoutCount.load(std::memory_order_relaxed); }

    /// Microseconds spent donating, summed across every donation.
    [[nodiscard]] std::uint64_t donationMicros() const { return m_donationMicros.load(std::memory_order_relaxed); }

private:
    std::mutex* m_modelLock;
    std::atomic<bool>* m_isRenderWaiting = nullptr;
    std::atomic<std::uint64_t> m_boundaryRequestCount { 0 };
    std::atomic<std::uint64_t> m_boundaryThrottledCount { 0 };
    std::atomic<std::uint64_t> m_donationCount { 0 };
    std::atomic<std::uint64_t> m_donationTimeoutCount { 0 };
    std::atomic<std::uint64_t> m_donationMicros { 0 };
    PublishHook m_publishHook = nullptr;
    void* m_publishContext = nullptr;
    std::atomic<std::uint32_t> m_publishMinIntervalMicros { syncPublishMinIntervalFloorMicros };
    std::atomic<bool> m_isDrainPassPresented { false };
    // Both writer-thread-only; no atomic needed. Separate because a copy and an
    // announcement are throttled against their own kind, not against each other.
    std::uint32_t m_lastCopyMicros = 0;
    std::uint32_t m_lastAnnounceMicros = 0;
};

} // namespace Ubiquitty::Runtime
