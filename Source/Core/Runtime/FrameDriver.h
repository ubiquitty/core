// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Idle<->active presentation-pacing policy, shared by the macOS MTKView drive and
// the Linux GTK frame clock. A terminal renders CONTINUOUSLY while host output is
// flowing, then drops to ON-DEMAND drive after a short idle timeout. Owns only the
// policy (last-activity timestamp, idle decision, active flag); each platform
// translates active() and the edge returns into its own present/timer calls.
//
// Threading: all calls are made on the platform render/main thread. The seq_cst
// atomics keep stamp-then-edge-test and pause-then-recheck a single total order
// even if a future backend stamps from another thread.

#include "Base/SwiftInterop.h"

#include <atomic>
#include <cstdint>

namespace Ubiquitty::Runtime {

/// What drives the next frame: damage, a timer, or nothing.
enum class FrameMode : uint8_t {
    /// Frames are asked for one at a time, by whatever changed.
    OnDemand UBIQUITTY_SWIFT_NAME(onDemand),
    /// Frames are driven on a clock, for as long as output flows.
    Continuous UBIQUITTY_SWIFT_NAME(continuous)
};

/// Whether the driver recommends its drive mode from the screen-change rate.
enum class AdaptiveDrive : uint8_t {
    /// recommendedMode is always Continuous; the byte-driven policy alone applies.
    Off UBIQUITTY_SWIFT_NAME(off),
    /// recommendedMode follows the drain cadence and the rendered net change.
    On UBIQUITTY_SWIFT_NAME(on)
};

class FrameDriver {
public:
    // After this much output silence, drop to on-demand drive.
    static constexpr double defaultIdleTimeoutSeconds = 0.1;

    // A cell-writing drain within this many frame-intervals of the previous
    // extends the keeping-pace streak; a wider gap resets it.
    static constexpr double adaptiveFastIntervalFactor = 1.5;
    // Streak length at which recommendedMode returns Continuous.
    static constexpr int adaptiveEscalateStreak = 3;
    // With no drain for this many frame-intervals, recommendedMode returns
    // OnDemand regardless of the streak.
    static constexpr double adaptiveStaleIntervalFactor = 3.0;
    // A rendered net change confirms escalation for this many frame-intervals.
    // Renders on the on-demand path happen once per delivery, tens of
    // milliseconds apart, so this window is wider than the drain one.
    static constexpr double adaptiveNetChangeIntervalFactor = 30.0;

    // Default-constructible so it can be a plain ObjC++ ivar.
    FrameDriver()
        : FrameDriver(defaultIdleTimeoutSeconds)
    {
    }

    explicit FrameDriver(double idleTimeoutSeconds)
        : m_idleTimeout(idleTimeoutSeconds > 0.0 ? idleTimeoutSeconds : 0.0)
    {
    }

    // Stamp host-output activity at monotonic time `now` (same clock domain as
    // idleExpired). Returns true on the IDLE->ACTIVE edge so the platform resumes
    // continuous drive exactly once; false when already active.
    bool noteActivity(double now)
    {
        m_lastActivity.store(now, std::memory_order_seq_cst);
        return !m_active.load(std::memory_order_seq_cst);
    }

    // True once output has been idle for at least the idle timeout as of `now`.
    bool idleExpired(double now) const { return now - m_lastActivity.load(std::memory_order_seq_cst) >= m_idleTimeout; }

    // Order is required: publish active=true before unpausing the link and
    // active=false after flipping paused, so an edge test never observes
    // paused-but-still-flagged-idle.
    void setMode(FrameMode frameMode) { m_active.store(frameMode == FrameMode::Continuous, std::memory_order_seq_cst); }

    bool active() const { return m_active.load(std::memory_order_seq_cst); }

    double idleTimeoutSeconds() const { return m_idleTimeout; }

    // Adaptive drive (opt-in). Two stamps from two threads. noteContentDrain,
    // from the ingest thread on every drain that wrote a cell, is the
    // escalation clock: a flood's main-thread deliveries coalesce far apart and
    // would never keep pace. noteNetChange, from the render when a frame
    // captured a row differing from the shadow baseline, confirms the writes
    // changed pixels: an identical rewrite (a mouse-tracking app redrawing on
    // motion) drains fast and never confirms. recommendedMode is Continuous only
    // while both hold. Off, it is always Continuous and none of this is read.

    /// Enables the rate recommendation. Off, recommendedMode is always Continuous.
    ///
    /// - Parameter drive: On to recommend a rate, Off to hold Continuous.
    void setAdaptiveDrive(AdaptiveDrive drive) { m_isAdaptiveDrive = drive == AdaptiveDrive::On; }

    /// Whether adaptive drive is enabled.
    ///
    /// - Returns: True while the rate recommendation is enabled.
    [[nodiscard]] bool isAdaptiveDrive() const { return m_isAdaptiveDrive; }

    /// The frame interval recommendedMode measures against, from the resolved
    /// panel rate. A non-positive value is ignored.
    ///
    /// - Parameter seconds: The interval between two frames at the panel rate.
    void setFrameIntervalSeconds(double seconds)
    {
        if (seconds > 0.0)
            m_frameInterval.store(seconds, std::memory_order_seq_cst);
    }

    /// The frame interval recommendedMode measures against, in seconds.
    ///
    /// - Returns: The interval in seconds.
    [[nodiscard]] double frameIntervalSeconds() const { return m_frameInterval.load(std::memory_order_seq_cst); }

    /// Stamps a cell-writing drain at monotonic `now` (recommendedMode's clock
    /// domain). Ingest thread. The first drain after construction or a reset
    /// does not extend the streak: there is no prior interval to measure against.
    ///
    /// - Parameters:
    ///   - now: The monotonic time of the drain, in the recommendation clock domain.
    ///   - isCapacityLimited: Whether the byte budget ended the drain before it caught up.
    void noteContentDrain(double now, bool isCapacityLimited = false) // bool-ok: publishes the drain’s byte-budget exhaustion flag.
    {
        m_isCapacityLimited.store(isCapacityLimited, std::memory_order_seq_cst);
        const double last = m_lastDrainTime.load(std::memory_order_seq_cst);
        m_lastDrainTime.store(now, std::memory_order_seq_cst);
        if (last > 0.0 && now - last <= frameIntervalSeconds() * adaptiveFastIntervalFactor) {
            const int streak = m_fastDrainStreak.load(std::memory_order_seq_cst);
            if (streak < adaptiveEscalateStreak)
                m_fastDrainStreak.store(streak + 1, std::memory_order_seq_cst);
        } else {
            m_fastDrainStreak.store(0, std::memory_order_seq_cst);
        }
    }

    /// Stamps a rendered frame that changed pixels at monotonic `now`. Render thread.
    ///
    /// - Parameter now: The monotonic time the frame was rendered.
    void noteNetChange(double now) { m_lastNetChangeTime.store(now, std::memory_order_seq_cst); }

    /// The drain streak, for a debug readout.
    ///
    /// - Returns: Consecutive fast drains, saturating at adaptiveEscalateStreak.
    [[nodiscard]] int fastDrainStreak() const { return m_fastDrainStreak.load(std::memory_order_seq_cst); }

    /// The drive mode warranted as of `now`. Continuous when adaptive drive is
    /// off or the frame interval is unset; otherwise Continuous only while the
    /// drain streak is full, the last drain is within adaptiveStaleIntervalFactor
    /// frame-intervals, and a net change was rendered within
    /// adaptiveNetChangeIntervalFactor frame-intervals; else OnDemand.
    ///
    /// - Parameter now: The monotonic time to judge against, in the drain stamps' clock domain.
    /// - Returns: Continuous or OnDemand.
    [[nodiscard]] FrameMode recommendedMode(double now) const
    {
        if (!m_isAdaptiveDrive)
            return FrameMode::Continuous;
        // Rate not sourced yet: stay Continuous rather than yield on an unknown rate.
        const double frameInterval = frameIntervalSeconds();
        if (frameInterval <= 0.0)
            return FrameMode::Continuous;
        const bool isCapacityLimited = m_isCapacityLimited.load(std::memory_order_seq_cst);
        const bool isEscalated = isCapacityLimited || m_fastDrainStreak.load(std::memory_order_seq_cst) >= adaptiveEscalateStreak;
        // A full byte budget proves backlog even when parsing it exceeds a frame.
        const double recentInterval = isCapacityLimited ? m_idleTimeout : frameInterval * adaptiveStaleIntervalFactor;
        const bool isDrainRecent = now - m_lastDrainTime.load(std::memory_order_seq_cst) <= recentInterval;
        const bool isNetChangeRecent = now - m_lastNetChangeTime.load(std::memory_order_seq_cst) <= frameInterval * adaptiveNetChangeIntervalFactor;
        return (isEscalated && isDrainRecent && isNetChangeRecent) ? FrameMode::Continuous : FrameMode::OnDemand;
    }

private:
    std::atomic<double> m_lastActivity { 0.0 };
    std::atomic<bool> m_active { false };
    double m_idleTimeout;
    // The per-change atomics match m_lastActivity's seq_cst.
    bool m_isAdaptiveDrive { false };
    // 0 until sourced from the resolved screen rate; recommendedMode stays
    // Continuous until then.
    std::atomic<double> m_frameInterval { 0.0 };
    std::atomic<bool> m_isCapacityLimited { false };
    std::atomic<double> m_lastDrainTime { 0.0 };
    std::atomic<int> m_fastDrainStreak { 0 };
    std::atomic<double> m_lastNetChangeTime { 0.0 };
};

} // namespace Ubiquitty::Runtime
