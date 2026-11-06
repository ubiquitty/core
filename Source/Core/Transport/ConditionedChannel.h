// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
// A Channel that sits between a session and a real carrier and impairs what passes, so a test can
// ask what a session does on a network it cannot otherwise arrange: a slow link, a stalled one, one
// that cuts, one that moves to a different path mid-session.
//
// It decorates any Channel — subprocess, local socket, QUIC — because it works at the seam they all
// present, which is what makes one harness cover every carrier the product ships.

#include "Core/Transport/Channel.h"
#include "Core/Transport/LinkConditions.h"

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace Ubiquitty::Transport {

/// A clock and a timer, so the conditioner can be driven by wall time or by a test.
///
/// Only the seam lives here. A unit test supplies a manual one and steps it, because a conditioner
/// asserted against `sleep` measures the machine's load as much as the code; a harness driving a real
/// daemon supplies one on the monotonic clock, and that one owns a thread — which is why it belongs
/// to the harness and not to Core, where no translation unit starts one.
class LinkScheduler {
public:
    virtual ~LinkScheduler() = default;

    /// Milliseconds on an arbitrary monotonic origin.
    [[nodiscard]] virtual std::uint64_t nowMilliseconds() const = 0;

    /// Runs `work` once, at or after `deadlineMilliseconds`.
    ///
    /// Work scheduled for a deadline already passed runs at the next opportunity rather than being
    /// dropped, so a conditioner set to zero latency still delivers.
    virtual void scheduleAt(std::uint64_t deadlineMilliseconds, std::function<void()> work) = 0;
};

/// Applies a `LinkConditions` to everything crossing one carrier, in both directions.
///
/// Wraps the real channel and stands in its place: the session starts, sends and stops this, and
/// sees the observer callbacks this releases. What the session cannot tell is that anything is in
/// the way, which is the point — the code under test is the shipping code.
///
/// **Release order is monotonic per direction.** Each chunk's release time is clamped to be no
/// earlier than the previous chunk's in that direction, so latency and jitter delay the stream and
/// never permute it. A reliable stream that arrived out of order would desynchronise the frame
/// decoder and read as a protocol bug, and no carrier the product uses can deliver one.
///
/// **Loss is a datagram-only impairment.** See ``LinkConditions::datagramLossPercent``.
class ConditionedChannel final : public Channel, private ChannelObserver {
public:
    /// Takes ownership of the carrier it impairs.
    ///
    /// - Parameters:
    ///   - inner: The real carrier. Never null.
    ///   - scheduler: Borrowed, and must outlive this channel.
    ///   - conditions: What the link is doing at the start; changeable later.
    ConditionedChannel(std::unique_ptr<Channel> inner, LinkScheduler* scheduler,
        LinkConditions conditions = {});
    ~ConditionedChannel() override;

    // ---- the conditioner's own controls (any thread) ----

    /// Replaces what the link is doing.
    ///
    /// Applies to what has not been released yet as well as to what arrives next: turning a blackout
    /// off delivers what it held, which is a link coming back rather than a fresh connection.
    void setConditions(LinkConditions conditions);

    [[nodiscard]] LinkConditions conditions() const;

    /// Holds both directions without closing, until ``endBlackout``.
    ///
    /// Shorthand for setting ``LinkConditions::blackout``; named because it is the interesting one:
    /// the peers still believe they are connected, so nothing but a liveness deadline notices.
    void beginBlackout();

    /// Passes what the blackout held, then resumes normal conditioning.
    void endBlackout();

    /// Ends the link the way a reset does: the observer sees `onClose` and nothing after it.
    ///
    /// Distinct from a blackout in exactly the way that matters — a cut is reported, a blackout is
    /// not — and distinct from ``stop`` in that the session did not ask for it.
    ///
    /// - Parameter reason: What the session reports as the drop's cause.
    void cut(const std::string& reason);

    /// Whether the carrier under this one is still open.
    [[nodiscard]] bool innerIsOpen() const;

    /// What the conditioner discarded, for a test that asserts loss actually happened.
    [[nodiscard]] std::uint64_t datagramsDropped() const;

    // ---- Channel ----

    bool start(ChannelObserver*) override;
    void send(std::span<const std::uint8_t>) override;
    void stop() override;
    void suspendReading() override;
    void resumeReading() override;
    [[nodiscard]] std::int64_t pendingOutboundBytes() const override;
    [[nodiscard]] std::int64_t outboundCapacityBytes() const override;
    [[nodiscard]] Origin origin() const override;
    [[nodiscard]] Health health() const override;
    [[nodiscard]] std::string description() const override;
    bool queryStats(TransportStats&) const override;

private:
    // ---- ChannelObserver, from the inner carrier's I/O thread ----
    void onConnect() override;
    void onData(std::span<const std::uint8_t>) override;
    void onDatagram(std::span<const std::uint8_t>) override;
    void onClose(const std::string& reason) override;

    /// One direction's pending traffic and its release clock.
    ///
    /// `nextReleaseMilliseconds` is what enforces the monotonic-order invariant: it only ever moves
    /// forward, so a chunk drawn a small jitter cannot be released before one drawn a large one.
    struct Direction {
        std::vector<std::function<void()>> held; // queued during a blackout, in arrival order
        std::uint64_t nextReleaseMilliseconds = 0;
    };

    /// Everything a scheduled release touches, owned by a `shared_ptr`.
    ///
    /// Separated from the channel because a release outlives it: work is queued against a deadline
    /// and the session may destroy the channel first. The scheduled lambda holds a `weak_ptr`, so a
    /// deadline reached after the channel is gone finds nothing to deliver to instead of a dangling
    /// `this`. `stopped` is what a destructor or a `stop()` sets to silence releases already queued.
    struct State {
        std::mutex mutex;
        ChannelObserver* observer = nullptr;
        LinkConditions conditions;
        Direction inbound;
        Direction outbound;
        Channel* inner = nullptr; // borrowed; the channel owns it and clears this before dropping it
        std::uint64_t datagramsDropped = 0;
        // Per-channel rather than global: two conditioned carriers in one harness must not perturb
        // each other's draws, or neither run reproduces on its own.
        std::uint32_t lossRandomState = 0x9e3779b9u;
        std::uint32_t jitterRandomState = 0x2545f491u;
        // A scheduled release belongs to the generation that admitted it. Stopping or a failed
        // start advances the generation, so work already held by a scheduler cannot become live
        // against a later observer.
        std::uint64_t generation = 1;
        std::condition_variable operationsDone;
        int operationsInFlight = 0;
        bool hasStarted = false;
        bool stopped = false;
        bool innerClosed = false;

        /// Whether callbacks may still reach the session. Caller holds `mutex`.
        [[nodiscard]] bool deliveringLocked() const { return observer != nullptr && !stopped; }
    };

    class OperationLease;

    /// Computes a chunk's release time and advances the direction's clock past it.
    ///
    /// Caller holds `State::mutex`. `jitterRandomState` is advanced, so the draw is reproducible
    /// per channel rather than shared across every conditioner in the process.
    [[nodiscard]] static std::uint64_t reserveRelease(const LinkConditions&, Direction&,
        std::uint32_t& jitterRandomState, std::uint64_t nowMilliseconds, std::size_t byteCount);

    /// Schedules `work` for its release, or holds it when the link is blacked out.
    ///
    /// Caller holds `State::mutex`; the lock is not held when `work` runs.
    static void releaseOrHold(const std::shared_ptr<State>&, LinkScheduler*, Direction&,
        std::size_t byteCount, std::function<void()> work);

    std::shared_ptr<State> m_state;
    std::unique_ptr<Channel> m_inner;
    LinkScheduler* m_scheduler = nullptr;
};

} // namespace Ubiquitty::Transport
