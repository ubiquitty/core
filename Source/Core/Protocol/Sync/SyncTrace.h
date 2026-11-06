// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
// A lightweight live instrument for the Sync client: records every inbound/outbound
// frame (per-type histogram, running totals, delta-gap count, echo-latency estimate,
// and rings of recent frames + connection events) for a frontend to poll.
//
// Threading: no internal locking. Every record*() call and every read runs on the
// single session thread; a frontend polling from another thread must marshal first.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace Ubiquitty::Protocol::Sync {

/// Whether a traced frame was sent or received.
enum class FrameDirection : uint8_t {
    Inbound,
    Outbound
};

// One recorded frame, retained in the ring buffer for the live log.
struct TracedFrame {
    uint64_t ordinal = 0; // 1-based index since construction/reset (gaps reveal ring overflow)
    int64_t unixMicros = 0; // wall-clock (system_clock) microseconds — for "12:04:01.221"
    int64_t monotonicMicros = 0; // steady_clock microseconds since the epoch — for "+4ms" deltas
    uint8_t type = 0; // MessageType wire tag
    bool outbound = false; // false = host->client (decoded), true = client->host (sent)
    uint32_t wireBytes = 0; // header + payload, as it travels the transport
    uint32_t logicalBytes = 0; // decompressed size (== wireBytes unless the frame was compressed)
};

// Cumulative per-(type,direction) aggregate.
struct FrameTypeStat {
    uint64_t count = 0;
    uint64_t wireBytes = 0;
    uint64_t logicalBytes = 0;
};

// A timestamped connection event (state change, handshake, drop, …).
struct TraceEvent {
    int64_t unixMicros = 0;
    int64_t monotonicMicros = 0;
    std::string text;
};

class SyncTrace {
public:
    SyncTrace();

    // Record a decoded inbound frame. logicalBytes is the decompressed size
    // (pass it == wireBytes for an uncompressed frame).
    void recordInbound(uint8_t type, std::size_t wireBytes, std::size_t logicalBytes);
    // Record an outbound frame (whole framed bytes, type == bytes[0]).
    void recordOutbound(uint8_t type, std::size_t wireBytes);
    /// Records that the person's input left for the host, which opens an
    /// echo-latency sample unless an earlier input's is still open: typing
    /// ahead measures from the first key to the paint.
    void recordInputSent();
    /// Records that the screen took a checkpoint or an update.
    ///
    /// The first one after a keystroke closes an echo-latency sample.
    void recordScreenApplied();
    /// Records that the screen plane asked the host for a checkpoint: an
    /// update it could not apply, or a digest that did not match. A high count
    /// means a client drifting from its host.
    void recordResync();
    /// Records that the pane's size was stated to the host: a `Viewport` whose
    /// grid or pixels differ from the one last stated. A person's resize is
    /// one per settle; a count that climbs while nobody resizes is a client
    /// claiming a size it was not given.
    void recordViewportStated();
    // Append a connection event to the timeline (e.g. a state change).
    void recordEvent(const std::string& text);
    // Clear all counters + rings and restart the uptime clock.
    void reset();

    struct Totals {
        uint64_t inboundFrames = 0;
        uint64_t outboundFrames = 0;
        uint64_t inboundWireBytes = 0;
        uint64_t inboundLogicalBytes = 0;
        uint64_t outboundWireBytes = 0;
        uint64_t resyncs = 0;
        uint64_t viewportsStated = 0;
        uint32_t largestInboundWireBytes = 0;
        uint32_t largestOutboundWireBytes = 0;
        int64_t lastScreenAgeMicros = -1; // since the screen last changed; -1 if it has not
        int64_t uptimeMicros = 0; // monotonic since construction/last reset
    };
    [[nodiscard]] Totals totals() const;

    // Echo latency: the time from a keystroke's input event to the next screen
    // frame (a checkpoint or an Update) — a proxy for keystroke-to-paint over the
    // wire. All microseconds; -1 when no sample has been taken.
    struct LatencyStats {
        uint64_t count = 0;
        int64_t lastMicros = -1;
        int64_t minMicros = -1;
        int64_t p50Micros = -1;
        int64_t p95Micros = -1;
        int64_t maxMicros = -1;
    };
    [[nodiscard]] LatencyStats latency() const;

    // Per-(type,direction) rows that have been seen at least once, in a stable
    // order (inbound first, then outbound, ascending type).
    struct TypeRow {
        uint8_t type = 0;
        bool outbound = false;
        FrameTypeStat stat;
    };
    [[nodiscard]] std::vector<TypeRow> perType() const;

    // The most recent frames, OLDEST first, up to `max` (capped at the ring
    // capacity). Empty when nothing has been recorded.
    [[nodiscard]] std::vector<TracedFrame> recent(std::size_t max) const;

    // The most recent connection events, OLDEST first, up to `max`.
    [[nodiscard]] std::vector<TraceEvent> events(std::size_t max) const;

private:
    void record(uint8_t type, FrameDirection direction, std::size_t wireBytes, std::size_t logicalBytes);

    // Indexed by frame type byte (0..255); only touched entries carry counts.
    std::vector<FrameTypeStat> m_inbound; // 256 entries
    std::vector<FrameTypeStat> m_outbound; // 256 entries
    Totals m_totals;

    // Fixed-capacity ring of the most recent frames.
    std::vector<TracedFrame> m_ring;
    std::size_t m_ringHead = 0; // index of the next write
    std::size_t m_ringCount = 0; // valid entries (<= capacity)
    uint64_t m_nextOrdinal = 1;

    // Fixed-capacity ring of connection events.
    std::vector<TraceEvent> m_events;
    std::size_t m_eventHead = 0;
    std::size_t m_eventCount = 0;

    // Echo-latency estimation + a ring of recent samples (for percentiles).
    int64_t m_pendingInputMonotonicMicros = -1; // the oldest input awaiting a screen frame
    std::vector<int64_t> m_latencyRing;
    std::size_t m_latencyHead = 0;
    std::size_t m_latencyCount = 0;
    int64_t m_lastLatencyMicros = -1;

    int64_t m_lastScreenMonotonicMicros = -1; // for "time since the screen last changed"
    int64_t m_epochSteadyMicros = 0; // steady_clock value at construction/reset
};

} // namespace Ubiquitty::Protocol::Sync
