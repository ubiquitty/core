// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// One pane's bounded wait for a handover snapshot. The owner supplies the
// actual terminal and a fresh publication class under its model lock. This
// controller does not read a stream, publish a frame, or mutate terminal state.
// Ready requires the owner to keep the pane frozen through snapshot capture;
// it does not establish whole-host, descriptor, or pending-input ownership.

#include <cstddef>
#include <cstdint>
#include <optional>

namespace Ubiquitty {
struct Terminal;
}

namespace Ubiquitty::Runtime {

/// The publication classification owned by `FrameCoherence`.
enum class FrameClass : std::uint8_t;

/// The read policy while a pane approaches its handover boundary.
enum class QuiescePhase : std::uint8_t {
    Natural,
    ReadsFrozen,
    Ready,
    Failed,
};

/// The reason a pane cannot supply a handover boundary within its allowance.
enum class QuiesceFailure : std::uint8_t {
    None,
    Deadline,
    ByteBudget,
    ClockRegressed,
    ReadContract,
};

/// Explicit policy bounds; neither deadline is extended by another observation.
struct QuiesceTuning {
    std::uint64_t naturalMicroseconds;
    std::uint64_t frozenMicroseconds;
    std::size_t maxGroundBytes;
};

/// Bounds natural completion and one-byte parsing without manufacturing ground.
///
/// A single serial owner uses this object for one pane and one attempt. Normal
/// reads stop at `ReadsFrozen`. Only a leased one-byte read can then complete a
/// stream-delimited blocker. Other work must finish through its existing owner.
/// `Ready` and `Failed` are terminal states; a retry needs a new controller.
class HandoverQuiesce {
public:
    /// Starts an attempt with an absolute monotonic origin and explicit bounds.
    ///
    /// - Parameters:
    ///   - tuning: Natural duration, subsequent frozen duration, and byte budget.
    ///   - nowMicroseconds: The monotonic instant this attempt starts.
    HandoverQuiesce(QuiesceTuning tuning, std::uint64_t nowMicroseconds);

    /// Observes the pane after all currently leased read bytes were parsed.
    ///
    /// The optional class must describe a frame of this exact terminal state,
    /// published during this observation, not a remembered earlier frame. Only
    /// `Boundary` and `Settled` qualify. An unread stream is not made quiet by
    /// freezing it; the caller must preserve the coherence rule's real inputs.
    /// This method never ages synchronized output or drains pending callbacks.
    ///
    /// - Parameters:
    ///   - terminal: The same locked pane throughout this attempt.
    ///   - published: The fresh publication class, or no publication.
    ///   - nowMicroseconds: The current monotonic clock, never decreasing.
    void notePass(const Terminal& terminal, std::optional<FrameClass> published, std::uint64_t nowMicroseconds);

    /// Leases at most one byte to finish a stream-delimited terminal blocker.
    ///
    /// A nonzero result requires exactly one `completeGroundRead` call before
    /// another observation or lease, even when the stream would block. No read
    /// is leased for callback-only work or parser ground alone. An opaque sink
    /// or active integration prevents reads even alongside a stream blocker.
    ///
    /// - Parameters:
    ///   - terminal: The same locked pane throughout this attempt.
    ///   - nowMicroseconds: The current monotonic clock, never decreasing.
    /// - Returns: One for the maximum read size, or zero when no read is allowed.
    [[nodiscard]] std::size_t takeGroundReadAllowance(const Terminal& terminal, std::uint64_t nowMicroseconds);

    /// Completes the outstanding lease with the actual number of bytes read.
    ///
    /// The owner parses any returned byte before its next `notePass`. Zero is
    /// a would-block or closed read and spends no bytes; the deadline still
    /// advances. An unsolicited completion or a count above one fails closed.
    ///
    /// - Parameters:
    ///   - bytesRead: Zero or one from the leased read.
    ///   - nowMicroseconds: The current monotonic clock, never decreasing.
    void completeGroundRead(std::size_t bytesRead, std::uint64_t nowMicroseconds);

    /// Returns the current read policy or final outcome.
    ///
    /// - Returns: The most recently observed phase.
    [[nodiscard]] QuiescePhase phase() const { return m_phase; }

    /// Returns why the attempt failed.
    ///
    /// - Returns: None until a failure, then the first failure reason.
    [[nodiscard]] QuiesceFailure failure() const { return m_failure; }

    /// Returns the exact number of bytes consumed by completed ground reads.
    ///
    /// - Returns: A count no greater than the configured byte budget.
    [[nodiscard]] std::size_t groundBytesRead() const { return m_groundBytesRead; }

private:
    /// Advances deadlines without adding durations to a potentially large clock.
    ///
    /// - Parameter nowMicroseconds: The next monotonic observation.
    /// - Returns: Whether the attempt remains open.
    [[nodiscard]] bool advanceClock(std::uint64_t nowMicroseconds);

    /// Records the first failure and withdraws an outstanding read lease.
    ///
    /// - Parameter failure: The refusal that closes this attempt.
    void fail(QuiesceFailure failure);

    const QuiesceTuning m_tuning;
    const std::uint64_t m_startedMicroseconds;
    std::uint64_t m_lastMicroseconds;
    std::size_t m_groundBytesRead = 0;
    QuiescePhase m_phase = QuiescePhase::Natural;
    QuiesceFailure m_failure = QuiesceFailure::None;
    bool m_hasReadLease = false;
};

} // namespace Ubiquitty::Runtime
