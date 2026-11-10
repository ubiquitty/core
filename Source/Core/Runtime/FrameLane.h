// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Base/SwiftInterop.h"
#include "Base/ThreadSafety.h"
#include "Core/Rendering/FrameCapture.h"
#include "Core/Selection/SelectionOverCapture.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <optional>
#include <utility>

// Source/Core/Runtime/FrameLane.h — one view's frames of a pane: the captures
// the owner writes and the view takes, and the view's hold on the one it
// presents.

namespace Ubiquitty::Runtime {

/// The lock-free triple-buffer exchange behind the session's boundary captures.
///
/// The ingest thread is the single producer and the Swift frame drive is the
/// single consumer. Three slots let the producer fill a free capture while one
/// is being read and another is pending. Publishing a completed capture
/// retires any older pending capture, preserving newest-wins without writing
/// storage the consumer can see.
class SyncFrameSlot {
public:
    static constexpr int slotCount = 3;

    /// The buffer the next boundary capture writes.
    ///
    /// Claims free storage before the capture starts. Slots named by
    /// `unavailableMask` remain untouched even when their exchange state is
    /// free, allowing an externally retained frame to outlive a drain.
    ///
    /// - Returns: The claimed slot, or -1 when no reusable slot exists.
    [[nodiscard]] int captureTarget(std::uint8_t unavailableMask = 0)
    {
        assert(m_writingIndex < 0);
        for (int index = 0; index < slotCount; ++index) {
            if (unavailableMask & (1u << static_cast<unsigned>(index))) {
                continue;
            }
            State expected = State::Free;
            if (m_states[index].compare_exchange_strong(
                    expected, State::Writing, std::memory_order_acquire, std::memory_order_relaxed)) {
                m_writingIndex = index;
                return index;
            }
        }
        return -1;
    }

    /// Records a completed capture into `target`.
    ///
    /// - Parameter target: The buffer written, from ``captureTarget``.
    void captured(int target)
    {
        assert(target == m_writingIndex);
        assert(m_states[target].load(std::memory_order_relaxed) == State::Writing);

        for (int index = 0; index < slotCount; ++index) {
            if (index == target)
                continue;
            State expected = State::Ready;
            if (m_states[index].compare_exchange_strong(
                    expected, State::Free, std::memory_order_acq_rel, std::memory_order_relaxed)) {
                m_overwriteCount.fetch_add(1, std::memory_order_relaxed);
            }
        }
        m_writingIndex = -1;
        m_states[target].store(State::Ready, std::memory_order_release);
    }

    /// Whether a published capture is still waiting for the consumer.
    ///
    /// Asked by the producer on its own thread, before it decides whether a
    /// publish needs to copy the grid: a capture nobody has taken describes a
    /// moment the grid has already left, and a consumer that prefers the slot
    /// to the model would present it and then owe nothing.
    ///
    /// - Returns: Whether any slot holds a capture that has not been drained.
    [[nodiscard]] bool hasReadyCapture() const
    {
        for (int index = 0; index < slotCount; ++index) {
            if (m_states[index].load(std::memory_order_acquire) == State::Ready)
                return true;
        }
        return false;
    }

    /// Releases a claimed target after a capture fails.
    ///
    /// An older ready frame remains publishable because capture always writes
    /// a distinct free slot.
    ///
    /// - Parameter target: The buffer claimed by ``captureTarget``.
    void cancelCapture(int target)
    {
        assert(target == m_writingIndex);
        assert(m_states[target].load(std::memory_order_relaxed) == State::Writing);
        m_writingIndex = -1;
        m_states[target].store(State::Free, std::memory_order_release);
    }

    /// Hands the pending frame to the drive.
    ///
    /// - Returns: The drained buffer's index, or -1 when none waits.
    [[nodiscard]] int drain()
    {
        const int previous = m_readingIndex;
        m_readingIndex = -1;
        if (previous >= 0)
            m_states[previous].store(State::Free, std::memory_order_release);

        for (int index = 0; index < slotCount; ++index) {
            State expected = State::Ready;
            if (!m_states[index].compare_exchange_strong(
                    expected, State::Reading, std::memory_order_acquire, std::memory_order_relaxed)) {
                continue;
            }
            m_readingIndex = index;
            m_promoteCount.fetch_add(1, std::memory_order_relaxed);
            return index;
        }
        return -1;
    }

    /// Boundary frames retired before the drive promoted them.
    [[nodiscard]] std::uint64_t overwriteCount() const { return m_overwriteCount.load(std::memory_order_relaxed); }

    /// Boundary frames promoted toward the screen.
    [[nodiscard]] std::uint64_t promoteCount() const { return m_promoteCount.load(std::memory_order_relaxed); }

private:
    /// Names each slot's atomic ownership state.
    enum class State : std::uint8_t {
        Free,
        Writing,
        Ready,
        Reading,
    };

    static_assert(std::atomic<State>::is_always_lock_free);
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);

    std::array<std::atomic<State>, slotCount> m_states { State::Free, State::Free, State::Free };
    // Each side alone owns its index; the atomics above carry cross-thread
    // ownership and make these bookkeeping values unobservable to the peer.
    int m_writingIndex = -1;
    int m_readingIndex = -1;
    std::atomic<std::uint64_t> m_overwriteCount { 0 };
    std::atomic<std::uint64_t> m_promoteCount { 0 };
};

/// A view's hold on the capture it presents: one reference, released when the
/// view takes its next frame or drops the hold. Move-only.
///
/// The C++ frontends' lease. Swift holds a capture through its own retain and
/// release, since ``Rendering::FrameCapture`` is a shared reference type there.
class PresentedCapture {
public:
    /// An empty hold.
    PresentedCapture() = default;

    /// Takes over the reference a take returned.
    ///
    /// - Parameter retained: A capture with a reference the caller owns, or
    ///   null for an empty hold.
    /// - Returns: The hold.
    [[nodiscard]] static PresentedCapture adopt(Rendering::FrameCapture* retained) noexcept
    {
        PresentedCapture held;
        held.m_capture = retained;
        return held;
    }

    /// Moves the hold, leaving `other` empty.
    ///
    /// - Parameter other: The hold to move.
    PresentedCapture(PresentedCapture&& other) noexcept
        : m_capture(std::exchange(other.m_capture, nullptr))
    {
    }

    /// Releases the capture held, then moves `other`'s hold in.
    ///
    /// - Parameter other: The hold to move.
    /// - Returns: This hold.
    PresentedCapture& operator=(PresentedCapture&& other) noexcept
    {
        if (this != &other)
            releaseFrameCapture(std::exchange(m_capture, std::exchange(other.m_capture, nullptr)));
        return *this;
    }

    PresentedCapture(const PresentedCapture&) = delete;
    PresentedCapture& operator=(const PresentedCapture&) = delete;

    /// Releases the capture held.
    ~PresentedCapture() { releaseFrameCapture(m_capture); }

    /// Whether a capture is held.
    [[nodiscard]] explicit operator bool() const noexcept { return m_capture; }

    /// Returns the capture held.
    ///
    /// - Returns: The capture, immutable while it is held.
    [[nodiscard]] const Rendering::FrameCapture& capture() const noexcept
    {
        assert(m_capture);
        return *m_capture;
    }

    /// Returns the geometry registration the capture was taken under.
    ///
    /// - Returns: The registration's generation.
    [[nodiscard]] std::uint64_t viewGeneration() const noexcept { return capture().viewGeneration(); }

private:
    Rendering::FrameCapture* m_capture = nullptr;
};

/// One view's frames of a pane: the boundary captures the owner writes and the
/// view takes, exchanged through a ``SyncFrameSlot``, each with the view's
/// selection resolved beside it.
///
/// The owner claims a capture no view holds, writes it under the model lock
/// and publishes it; the view takes the newest one without a lock. A capture
/// and its selection are immutable from publication until the last view that
/// took it lets it go.
class FrameLane {
public:
    /// The most views one pane presents at once, each through a lane of its own.
    static constexpr int maxViewsPerPane = 4;

    /// A capture the owner writes, with the selection resolved for it.
    struct Target {
        /// The slot claimed.
        int index = -1;
        /// The capture to write.
        Rendering::FrameCapture* capture = nullptr;
        /// The view's selection, resolved with the capture.
        Selection::CapturedSelection* selection = nullptr;
    };

    /// An unarmed lane.
    FrameLane() = default;

    /// Drops the lane's own reference to each capture; a view that still
    /// holds one keeps it.
    ~FrameLane()
    {
        for (Rendering::FrameCapture* capture : m_captures)
            releaseFrameCapture(capture);
    }

    FrameLane(const FrameLane&) = delete;
    FrameLane& operator=(const FrameLane&) = delete;

    /// Makes the lane's captures when it has none yet.
    ///
    /// - Returns: Whether every capture exists.
    bool arm()
    {
        if (!m_captures[0]) {
            for (Rendering::FrameCapture*& capture : m_captures)
                capture = Rendering::FrameCapture::make();
        }
        return std::ranges::all_of(m_captures, [](const Rendering::FrameCapture* capture) { return capture; });
    }

    /// Takes the newest published capture, retiring the one taken before.
    ///
    /// The view's side.
    ///
    /// - Parameter minimumGeneration: The view's newest geometry
    ///   registration; a capture made under an older one carries the old
    ///   placement and is retired instead.
    /// - Returns: The capture, with a reference the caller owns, or null when
    ///   none was published since the last take.
    [[nodiscard]] Rendering::FrameCapture* takeReady(std::uint64_t minimumGeneration = 0) noexcept UBIQUITTY_NONBLOCKING UBIQUITTY_RETURNS_RETAINED
    {
        const int drained = m_slot.drain();
        if (drained < 0)
            return nullptr;
        Rendering::FrameCapture* capture = m_captures[static_cast<std::size_t>(drained)];
        if (capture->viewGeneration() < minimumGeneration)
            return nullptr;
        capture->swiftReferences.fetch_add(1, std::memory_order_relaxed);
        return capture;
    }

    /// Retires the published capture and the one taken before, so neither is
    /// presented.
    ///
    /// - Returns: Whether a published capture was waiting.
    bool retireReady() noexcept { return m_slot.drain() >= 0; }

    /// Whether a published capture is waiting for the view.
    ///
    /// - Returns: Whether one is.
    [[nodiscard]] bool hasReadyCapture() const noexcept { return m_slot.hasReadyCapture(); }

    /// Returns the selection resolved with one of this lane's captures.
    ///
    /// - Parameter capture: A capture the caller holds.
    /// - Returns: Its selection, or null for a capture of another lane.
    [[nodiscard]] const Selection::CapturedSelection* selectionFor(const Rendering::FrameCapture& capture) const noexcept UBIQUITTY_NONBLOCKING
    {
        for (std::size_t index = 0; index < m_captures.size(); ++index) {
            if (m_captures[index] == &capture)
                return &m_selections[index];
        }
        return nullptr;
    }

    /// Claims a capture to write: one the exchange holds free and no view
    /// still holds.
    ///
    /// The owner's side, under the model lock.
    ///
    /// - Returns: The capture and its selection, or nothing when every free
    ///   one is still held.
    [[nodiscard]] std::optional<Target> claimTarget() noexcept
    {
        std::uint8_t unavailableMask = 0;
        for (int candidate = 0; candidate < SyncFrameSlot::slotCount; ++candidate) {
            const int index = m_slot.captureTarget(unavailableMask);
            if (index < 0)
                return std::nullopt;
            // A free slot may still have a view's reference from an earlier
            // take. The state transition's acquire observes that take before
            // any bytes are written into the capture.
            Rendering::FrameCapture* capture = m_captures[static_cast<std::size_t>(index)];
            if (capture->swiftReferences.load(std::memory_order_acquire) == 1)
                return Target { .index = index, .capture = capture, .selection = &m_selections[static_cast<std::size_t>(index)] };
            unavailableMask |= static_cast<std::uint8_t>(1u << static_cast<unsigned>(index));
            m_slot.cancelCapture(index);
        }
        return std::nullopt;
    }

    /// Publishes a written capture, retiring an older one no view took.
    ///
    /// - Parameter target: The capture written, from ``claimTarget``.
    void publish(const Target& target) noexcept { m_slot.captured(target.index); }

    /// Gives back a claimed capture that was not written.
    ///
    /// - Parameter target: The capture claimed, from ``claimTarget``.
    void cancel(const Target& target) noexcept { m_slot.cancelCapture(target.index); }

    /// Published captures retired before a view took them.
    ///
    /// - Returns: The count.
    [[nodiscard]] std::uint64_t overwriteCount() const noexcept { return m_slot.overwriteCount(); }

    /// Published captures a view took.
    ///
    /// - Returns: The count.
    [[nodiscard]] std::uint64_t promoteCount() const noexcept { return m_slot.promoteCount(); }

private:
    // One capture the view reads, one published, and one the owner writes
    // without waiting for either.
    std::array<Rendering::FrameCapture*, SyncFrameSlot::slotCount> m_captures {};
    SyncFrameSlot m_slot;
    // Each capture's selection, written with it under the model lock and read
    // by the view only while it holds that capture, which no write reuses.
    std::array<Selection::CapturedSelection, SyncFrameSlot::slotCount> m_selections {};
};

} // namespace Ubiquitty::Runtime
