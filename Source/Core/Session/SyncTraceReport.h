// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
// What a Sync Debug window shows for one remote session, gathered in one pass.
//
// The trace records; this reads it. A frontend asks for a report and draws the
// fields, so every frontend shows the same numbers under the same names and
// none of them walks the trace's rings or names a wire type itself. Plain
// values only — no platform types and no references into the trace — so the
// report outlives the session that produced it and crosses into Swift as a
// copy.

#include "Core/Protocol/Sync/Envelope.h"
#include "Core/Protocol/Sync/SyncTrace.h"
#include "Core/Session/RemoteSessionCore.h"
#include "Core/Transport/Channel.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace Ubiquitty::Session {

/// One remote session's traffic, protocol and timing, as of one moment.
struct SyncTraceReport {
    /// One frame from the trace's recent-frames ring, named.
    struct Frame {
        /// The frame's position since the trace began or was reset; a jump of
        /// more than one between neighbours means the ring overflowed between
        /// them.
        std::uint64_t ordinal = 0;
        /// Wall-clock microseconds, for the time of day a reader sees.
        std::int64_t unixMicros = 0;
        /// Microseconds since the trace's epoch, for the gap to the previous
        /// frame; immune to the wall clock being stepped.
        std::int64_t monotonicMicros = 0;
        /// Whether the client sent it rather than the host.
        bool isOutbound = false;
        /// The wire tag.
        std::uint8_t type = 0;
        /// Bytes as the frame travelled, header included.
        std::uint32_t wireBytes = 0;
        /// Bytes once decompressed; equal to `wireBytes` for an uncompressed
        /// frame.
        std::uint32_t logicalBytes = 0;
        /// The wire tag's protocol name.
        std::string typeName;
    };

    /// The totals for one wire type in one direction, named.
    struct TypeRow {
        /// Whether the row counts frames the client sent.
        bool isOutbound = false;
        /// The wire tag.
        std::uint8_t type = 0;
        /// Frames of this type seen in this direction.
        std::uint64_t count = 0;
        /// Their bytes as they travelled.
        std::uint64_t wireBytes = 0;
        /// Their bytes once decompressed.
        std::uint64_t logicalBytes = 0;
        /// The wire tag's protocol name.
        std::string typeName;
    };

    /// One entry from the connection-event timeline.
    struct Event {
        /// Wall-clock microseconds.
        std::int64_t unixMicros = 0;
        /// Microseconds since the trace's epoch.
        std::int64_t monotonicMicros = 0;
        /// What happened, as the session recorded it.
        std::string text;
    };

    /// The session's connection state.
    State state = State::Idle;
    /// The carrier the session runs over.
    TransportMode transportMode = TransportMode::TCP;
    /// The carrier's name as a reader sees it: "QUIC" or "ssh-stdio".
    std::string transportName;
    /// The daemon session this connection is attached to, zero until the host
    /// pins one.
    std::uint32_t sessionID = 0;
    /// When the report was taken, in wall-clock microseconds.
    std::int64_t sampleUnixMicros = 0;

    /// Frame and byte counters since the trace began or was reset.
    Protocol::Sync::SyncTrace::Totals totals;
    /// Keystroke-to-screen-frame latency over the wire.
    Protocol::Sync::SyncTrace::LatencyStats latency;
    /// Whether the carrier reported transport statistics; only QUIC does.
    bool hasTransportStats = false;
    /// The carrier's statistics, meaningful only when `hasTransportStats`.
    Transport::TransportStats transport;

    /// Per-type totals, inbound first, then outbound, each by ascending tag.
    std::vector<TypeRow> typeRows;
    /// The most recent frames, oldest first.
    std::vector<Frame> recentFrames;
    /// The most recent connection events, oldest first.
    std::vector<Event> events;
};

/// How many recent frames a report carries: the Sync Debug window's frame log
/// shows this many.
inline constexpr std::size_t syncTraceReportFrameLimit = 250;

/// How many connection events a report carries.
inline constexpr std::size_t syncTraceReportEventLimit = 64;

/// Builds a report from a trace and the facts about the session around it.
///
/// Separate from the session so a test can build one from a trace it recorded
/// itself; ``makeSyncTraceReport(const RemoteSessionCore&, std::int64_t)`` is
/// the form a frontend calls.
///
/// - Parameters:
///   - trace: The trace to read.
///   - transport: The carrier's statistics, or nil when it reported none.
///   - state: The session's connection state.
///   - transportMode: The carrier the session runs over.
///   - sessionID: The daemon session id, zero when none is pinned.
///   - sampleUnixMicros: The wall-clock time to stamp the report with.
/// - Returns: The report, with frames and events capped at the report limits.
[[nodiscard]] inline SyncTraceReport makeSyncTraceReport(const Protocol::Sync::SyncTrace& trace,
    const Transport::TransportStats* transport, State state, TransportMode transportMode, std::uint32_t sessionID,
    std::int64_t sampleUnixMicros)
{
    SyncTraceReport report;
    report.state = state;
    report.transportMode = transportMode;
    report.transportName = transportMode == TransportMode::QUIC ? "QUIC" : "ssh-stdio";
    report.sessionID = sessionID;
    report.sampleUnixMicros = sampleUnixMicros;
    report.totals = trace.totals();
    report.latency = trace.latency();
    if (transport && transport->valid) {
        report.hasTransportStats = true;
        report.transport = *transport;
    }
    for (const auto& row : trace.perType()) {
        report.typeRows.push_back({ row.outbound, row.type, row.stat.count, row.stat.wireBytes,
            row.stat.logicalBytes, Protocol::Sync::frameKindName(row.type) });
    }
    for (const auto& frame : trace.recent(syncTraceReportFrameLimit)) {
        report.recentFrames.push_back({ frame.ordinal, frame.unixMicros, frame.monotonicMicros, frame.outbound,
            frame.type, frame.wireBytes, frame.logicalBytes, Protocol::Sync::frameKindName(frame.type) });
    }
    for (const auto& event : trace.events(syncTraceReportEventLimit)) {
        report.events.push_back({ event.unixMicros, event.monotonicMicros, event.text });
    }
    return report;
}

/// Builds the report a Sync Debug window shows for one remote session.
///
/// Read on the session's own thread, the one its trace and carrier are touched
/// on, which on every frontend today is the main thread.
///
/// - Parameters:
///   - core: The remote session.
///   - sampleUnixMicros: The wall-clock time to stamp the report with.
/// - Returns: The report.
[[nodiscard]] inline SyncTraceReport makeSyncTraceReport(const RemoteSessionCore& core, std::int64_t sampleUnixMicros)
{
    Transport::TransportStats stats;
    const bool hasStats = core.queryTransportStats(stats) && stats.valid;
    return makeSyncTraceReport(core.trace(), hasStats ? &stats : nullptr, core.state(),
        core.isUsingQUIC() ? TransportMode::QUIC : TransportMode::TCP, core.sessionID(), sampleUnixMicros);
}

} // namespace Ubiquitty::Session
