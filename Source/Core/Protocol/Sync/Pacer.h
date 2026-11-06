// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

//
// Sync/Pacer.h — one attachment's send window (Docs/sync.md §6.4).
//
// The window bounds the bytes the host has handed the link and the client has
// not acknowledged:
//
//     window = max(floor, 2 × deliveryRate × smoothedRoundTrip)
//
// - **Round trip.** Each update carries `hostTime`; the `Ack` that answers it
//   echoes that time and adds `holdMicros`, how long the client held the
//   `Ack` before sending it. `sample = now − echo − hold`: one clock, no clock
//   sync, and no bias from the client's ack coalescing. Samples smooth as
//   RFC 6298 §2 does, with its gain of 1/8.
// - **Delivery rate.** Bytes acknowledged per interval between acks,
//   max-filtered over a span of time.
// - **Freshness.** A round trip older than the staleness bound drops the
//   window to its floor, so a frame after an idle period is never paced from
//   a stale estimate; while no screen data flows the host probes for a fresh
//   sample.
//
// Outstanding bytes are of two kinds. Stream bytes are counted when they are
// handed to the stream and retired by the cumulative `bytesReceived` an `Ack`
// carries, which bounds what queues in a relay the host cannot see through,
// such as the remote SSH server. Datagram bytes are retired by the `Ack` that names
// their generation or a later one, or by the loss timeout, after which the
// next superset update covers what they carried.
//
// The pacer queues nothing itself: a caller asks `admits` before it encodes,
// and an attachment the window refuses is owed one fresh update when it opens.
//
// **Bulk in flight.** Bulk bytes — transfer chunks and the frames of a run —
// are admitted by the window and also by a narrower one of their own:
//
//     bulkWindow = max(one capped frame, 2 × deliveryRate × smoothedRoundTrip)
//
// The window's floor exists for screen updates, and a floor's worth of bulk
// queued in a relay would sit ahead of every interactive frame after it. With
// bulk held to one capped frame beyond what the link carries in two round
// trips, an interactive frame admitted after it waits behind at most one
// capped frame's time on the wire (Docs/sync.md §8).
//

#include "Core/Protocol/Sync/Envelope.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <vector>

namespace Ubiquitty::Protocol::Sync {

/// The pacing constants.
///
/// The connection owner supplies these constants; `measuredHostPolicy`
/// provides the defaults.
struct PacingPolicy {
    /// The smallest window, in bytes.
    std::uint64_t windowFloorBytes;
    /// How long no screen data may flow before the host probes for a round trip.
    std::uint64_t probeIdleMicroseconds;
    /// How old a round-trip sample may be before the window falls to its floor.
    std::uint64_t roundTripStaleMicroseconds;
    /// The span the delivery rate's max filter covers.
    std::uint64_t rateFilterMicroseconds;
    /// The least time a datagram waits for its `Ack` before it is counted lost;
    /// the wait is the larger of this and twice the smoothed round trip.
    std::uint64_t datagramLossFloorMicroseconds;
    /// The longest interval between two idle probes: the schedule doubles
    /// from `probeIdleMicroseconds` and repeats this step once it reaches it.
    std::uint64_t maxProbeIntervalMicroseconds = 30'000'000;
    /// How long bytes may stay outstanding with no `Ack` or `Echo` heard
    /// before the attachment is lost.
    std::uint64_t lostSilenceMicroseconds = 30'000'000;
};

/// The least bulk the bulk window admits: one capped frame, so a transfer
/// always moves while nothing of it is in flight.
inline constexpr std::uint64_t bulkWindowFloorBytes = maxEnvelopeHeaderBytes + maxFrameBodyBytes;

/// A datagram the client has not acknowledged.
struct DatagramInFlight {
    std::uint32_t handle = 0;
    std::uint64_t generation = 0;
    std::uint64_t bytes = 0;
    std::uint64_t sentMicroseconds = 0;
};

/// One attachment's window, round trip and delivery rate.
class AttachmentPacer {
public:
    /// Creates a pacer.
    ///
    /// - Parameter policy: The pacing constants.
    explicit AttachmentPacer(const PacingPolicy& policy)
        : m_policy(policy)
    {
    }

    /// Records bytes handed to the stream.
    ///
    /// - Parameter bytes: Their count on the wire.
    void noteStreamBytes(std::uint64_t bytes) { m_streamBytesSent += bytes; }

    /// Records bulk bytes handed to the stream, which the bulk window also
    /// bounds until an `Ack` counts past them.
    ///
    /// - Parameter bytes: Their count on the wire.
    void noteBulkStreamBytes(std::uint64_t bytes);

    /// Records a datagram carrying a pane's update.
    ///
    /// - Parameters:
    ///   - handle: The pane's handle.
    ///   - generation: The generation the update brings the pane to.
    ///   - bytes: The datagram's size.
    ///   - now: The host clock.
    void noteDatagram(std::uint32_t handle, std::uint64_t generation, std::uint64_t bytes, std::uint64_t now);

    /// Cancels a queued datagram before the transport takes it. It contributes
    /// no delivered bytes or loss sample.
    ///
    /// - Parameters:
    ///   - handle: The pane's handle.
    ///   - generation: The generation carried by the withdrawn datagram.
    void cancelDatagram(std::uint32_t handle, std::uint64_t generation);

    /// Records that screen data left, which restarts the probe schedule.
    ///
    /// - Parameter now: The host clock.
    void noteScreenData(std::uint64_t now)
    {
        m_lastScreenDataMicroseconds = now;
        m_probeStep = 0;
    }

    /// Records the client's input reaching its program, which restarts the
    /// probe schedule as screen data does.
    ///
    /// - Parameter now: The host clock.
    void noteActivity(std::uint64_t now) { noteScreenData(now); }

    /// Records that the client was heard: an `Ack` or an `Echo`, which is what
    /// the silence rule waits for.
    ///
    /// - Parameter now: The host clock.
    void noteHeard(std::uint64_t now) { m_lastHeardMicroseconds = now; }

    /// When the client was last heard, 0 before it has been.
    [[nodiscard]] std::uint64_t lastHeardMicroseconds() const { return m_lastHeardMicroseconds; }

    /// Whether a probe left that no `Echo` has answered.
    [[nodiscard]] bool isProbeOutstanding() const { return m_outstandingProbe.has_value(); }

    /// Applies an `Ack`'s connection fields.
    ///
    /// - Parameters:
    ///   - now: The host clock.
    ///   - hostTimeEcho: The `hostTime` the client echoed, or 0 when it had none.
    ///   - holdMicroseconds: How long the client held the `Ack`.
    ///   - bytesReceived: The stream bytes the client has received.
    void noteAck(std::uint64_t now, std::uint64_t hostTimeEcho, std::uint64_t holdMicroseconds, std::uint64_t bytesReceived);

    /// Retires the datagrams of a pane at or below an acknowledged generation.
    ///
    /// - Parameters:
    ///   - handle: The pane's handle.
    ///   - generation: The generation the client acknowledged.
    void retireDatagrams(std::uint32_t handle, std::uint64_t generation);

    /// Applies an `Echo`: contact for the silence rule, and a round-trip
    /// sample when it answers the probe outstanding.
    ///
    /// - Parameters:
    ///   - now: The host clock.
    ///   - timeEcho: The time the probe it answers carried.
    ///   - holdMicroseconds: How long the client held the `Echo`.
    /// - Returns: Whether it answered the probe outstanding, so was a sample.
    bool noteEcho(std::uint64_t now, std::uint64_t timeEcho, std::uint64_t holdMicroseconds);

    /// Retires datagrams that waited past the loss timeout.
    ///
    /// - Parameter now: The host clock.
    /// - Returns: The datagrams counted lost.
    [[nodiscard]] std::vector<DatagramInFlight> expireDatagrams(std::uint64_t now);

    /// The window at a time, in bytes.
    ///
    /// - Parameter now: The host clock.
    /// - Returns: The floor while the round trip is unknown or stale.
    [[nodiscard]] std::uint64_t windowBytes(std::uint64_t now) const;

    /// The bytes handed to the link and not yet acknowledged or counted lost.
    [[nodiscard]] std::uint64_t outstandingBytes() const;

    /// The stream bytes the client has acknowledged receiving, in stream
    /// order from the first.
    [[nodiscard]] std::uint64_t acknowledgedStreamBytes() const { return m_streamBytesReceived; }

    /// Records that every stream byte handed over so far owes an `Ack`: the
    /// frame just noted carries more than the host's probes.
    ///
    /// A client acknowledges bytes that carried only probes with its `Echo`
    /// and counts them in its next `Ack` only when something else arrives, so
    /// probe bytes at the end of the stream are answered, not outstanding.
    void noteAckOwedThroughSent() { m_ackOwedStreamEnd = m_streamBytesSent; }

    /// Whether the client owes an `Ack`: a stream byte that is not only a
    /// probe, or a datagram, is not yet acknowledged.
    [[nodiscard]] bool isAckOwed() const { return m_streamBytesReceived < m_ackOwedStreamEnd || m_datagramBytesOutstanding > 0; }

    /// Whether more bytes may leave: they fit inside the window, or nothing
    /// is outstanding, so a frame larger than the window still goes alone.
    ///
    /// - Parameters:
    ///   - bytes: The bytes that would leave.
    ///   - now: The host clock.
    ///   - queuedBytes: Interactive bytes reserved but not handed to the stream.
    /// - Returns: Whether they may.
    [[nodiscard]] bool admits(std::uint64_t bytes, std::uint64_t now, std::uint64_t queuedBytes = 0) const;

    /// The bulk window at a time, in bytes.
    ///
    /// - Parameter now: The host clock.
    /// - Returns: One capped frame while the round trip is unknown or stale.
    [[nodiscard]] std::uint64_t bulkWindowBytes(std::uint64_t now) const;

    /// The bulk bytes handed to the stream that no `Ack` has counted past.
    [[nodiscard]] std::uint64_t bulkOutstandingBytes() const { return m_bulkBytesOutstanding; }

    /// Whether a bulk frame may leave: the window admits it, and it fits the
    /// bulk window or no bulk is outstanding.
    ///
    /// - Parameters:
    ///   - bytes: The frame's bytes on the wire.
    ///   - now: The host clock.
    ///   - queuedBytes: Interactive bytes reserved in the connection window,
    ///     not in the bulk window.
    /// - Returns: Whether it may.
    [[nodiscard]] bool admitsBulk(std::uint64_t bytes, std::uint64_t now, std::uint64_t queuedBytes = 0) const;

    /// Whether the window has room for anything at all.
    ///
    /// - Parameters:
    ///   - now: The host clock.
    ///   - queuedBytes: Interactive bytes reserved but not handed to the stream.
    /// - Returns: Whether it has.
    [[nodiscard]] bool isOpen(std::uint64_t now, std::uint64_t queuedBytes = 0) const { return outstandingBytes() + queuedBytes < windowBytes(now) || outstandingBytes() + queuedBytes == 0; }

    /// Whether the host should probe: no screen data has flowed for the
    /// schedule's current step since the last screen data or probe.
    ///
    /// - Parameter now: The host clock.
    /// - Returns: Whether a probe is due.
    [[nodiscard]] bool isProbeDue(std::uint64_t now) const;

    /// Records a probe sent, which moves the schedule a step on.
    ///
    /// - Parameters:
    ///   - now: The host clock.
    ///   - time: The time the probe carried, which its `Echo` returns.
    void noteProbe(std::uint64_t now, std::uint64_t time);

    /// When the next probe falls due if no screen data leaves before then.
    [[nodiscard]] std::uint64_t nextProbeMicroseconds() const;

    /// The idle schedule's step after a run of probes.
    ///
    /// - Parameter step: How many probes have left since screen data last did.
    /// - Returns: The interval before the next, in microseconds.
    [[nodiscard]] std::uint64_t probeIntervalMicroseconds(std::size_t step) const;

    /// When the oldest datagram in flight is counted lost, or `nullopt` when
    /// none is in flight.
    [[nodiscard]] std::optional<std::uint64_t> nextDatagramExpiryMicroseconds() const;

    /// The smoothed round trip, or `nullopt` before the first sample.
    [[nodiscard]] std::optional<std::uint64_t> smoothedRoundTripMicroseconds() const { return m_smoothedRoundTrip; }

    /// The max-filtered delivery rate, in bytes per second.
    [[nodiscard]] std::uint64_t deliveryRateBytesPerSecond() const;

private:
    /// How long a datagram waits for its `Ack` before it is counted lost.
    [[nodiscard]] std::uint64_t datagramTimeoutMicroseconds() const;

    /// Folds one round-trip sample in.
    ///
    /// - Parameters:
    ///   - now: The host clock.
    ///   - hostTimeEcho: The `hostTime` echoed.
    ///   - holdMicroseconds: How long the client held its answer.
    void noteRoundTrip(std::uint64_t now, std::uint64_t hostTimeEcho, std::uint64_t holdMicroseconds);

    /// One delivery-rate sample.
    struct RateSample {
        std::uint64_t at = 0;
        std::uint64_t bytesPerSecond = 0;
    };

    /// Bulk bytes sent, by the stream offset just past them.
    struct BulkSpan {
        std::uint64_t end = 0;
        std::uint64_t bytes = 0;
    };

    PacingPolicy m_policy;
    std::deque<BulkSpan> m_bulkSpans;
    std::uint64_t m_bulkBytesOutstanding = 0;
    std::uint64_t m_streamBytesSent = 0;
    std::uint64_t m_streamBytesReceived = 0;
    std::uint64_t m_ackOwedStreamEnd = 0;
    std::deque<DatagramInFlight> m_datagrams;
    std::uint64_t m_datagramBytesOutstanding = 0;
    std::uint64_t m_bytesDeliveredSinceAck = 0;
    std::optional<std::uint64_t> m_lastAckMicroseconds;
    std::deque<RateSample> m_rateSamples;
    std::optional<std::uint64_t> m_smoothedRoundTrip;
    std::uint64_t m_lastRoundTripSampleMicroseconds = 0;
    std::uint64_t m_lastScreenDataMicroseconds = 0;
    std::uint64_t m_lastProbeMicroseconds = 0;
    std::uint64_t m_lastHeardMicroseconds = 0;
    std::size_t m_probeStep = 0;
    /// The time the probe not yet answered carried.
    std::optional<std::uint64_t> m_outstandingProbe;
};

} // namespace Ubiquitty::Protocol::Sync
