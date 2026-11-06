// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// SyncTrace.cpp — see SyncTrace.h.
//

#include "Core/Protocol/Sync/SyncTrace.h"

#include "Core/Protocol/Sync/NativeAttachment.h"

#include <algorithm>
#include <chrono>
#include <utility>

namespace Ubiquitty::Protocol::Sync {

namespace {
// How many recent frames the live log retains. ~600 covers a few seconds of a
// busy delta stream — enough to read a stall's shape without unbounded memory.
constexpr std::size_t traceRingCapacity = 600;
// Connection-event timeline depth.
constexpr std::size_t eventCapacity = 64;
// Echo-latency sample window (for the percentiles).
constexpr std::size_t latencyCapacity = 120;

int64_t steadyMicros() { return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

int64_t unixMicros() { return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch()).count(); }
} // namespace

SyncTrace::SyncTrace()
    : m_inbound(256)
    , m_outbound(256)
    , m_ring(traceRingCapacity)
    , m_events(eventCapacity)
    , m_latencyRing(latencyCapacity)
    , m_epochSteadyMicros(steadyMicros())
{
}

void SyncTrace::reset()
{
    std::ranges::fill(m_inbound, FrameTypeStat {});
    std::ranges::fill(m_outbound, FrameTypeStat {});
    m_totals = Totals {};
    m_ringHead = 0;
    m_ringCount = 0;
    m_nextOrdinal = 1;
    m_eventHead = 0;
    m_eventCount = 0;
    m_pendingInputMonotonicMicros = -1;
    m_latencyHead = 0;
    m_latencyCount = 0;
    m_lastLatencyMicros = -1;
    m_lastScreenMonotonicMicros = -1;
    m_epochSteadyMicros = steadyMicros();
}

void SyncTrace::recordInputSent()
{
    // From the oldest input still unanswered, so a burst of keys on a slow
    // link reports the whole first-key-to-paint lag and not the last key's.
    if (m_pendingInputMonotonicMicros < 0)
        m_pendingInputMonotonicMicros = steadyMicros() - m_epochSteadyMicros;
}

void SyncTrace::record(uint8_t type, FrameDirection direction, std::size_t wireBytes, std::size_t logicalBytes)
{
    const bool outbound = (direction == FrameDirection::Outbound);
    const int64_t monotonic = steadyMicros() - m_epochSteadyMicros;

    FrameTypeStat& stat = (outbound ? m_outbound : m_inbound)[type];
    ++stat.count;
    stat.wireBytes += wireBytes;
    stat.logicalBytes += logicalBytes;

    if (outbound) {
        ++m_totals.outboundFrames;
        m_totals.outboundWireBytes += wireBytes;
        if (wireBytes > m_totals.largestOutboundWireBytes)
            m_totals.largestOutboundWireBytes = static_cast<uint32_t>(wireBytes);
    } else {
        ++m_totals.inboundFrames;
        m_totals.inboundWireBytes += wireBytes;
        m_totals.inboundLogicalBytes += logicalBytes;
        if (wireBytes > m_totals.largestInboundWireBytes)
            m_totals.largestInboundWireBytes = static_cast<uint32_t>(wireBytes);
    }

    TracedFrame& slot = m_ring[m_ringHead];
    slot.ordinal = m_nextOrdinal++;
    slot.unixMicros = unixMicros();
    slot.monotonicMicros = monotonic;
    slot.type = type;
    slot.outbound = outbound;
    slot.wireBytes = static_cast<uint32_t>(wireBytes);
    slot.logicalBytes = static_cast<uint32_t>(logicalBytes);
    m_ringHead = (m_ringHead + 1) % m_ring.size();
    if (m_ringCount < m_ring.size())
        ++m_ringCount;
}

void SyncTrace::recordInbound(uint8_t type, std::size_t wireBytes, std::size_t logicalBytes) { record(type, FrameDirection::Inbound, wireBytes, logicalBytes); }

void SyncTrace::recordOutbound(uint8_t type, std::size_t wireBytes) { record(type, FrameDirection::Outbound, wireBytes, wireBytes); }

void SyncTrace::recordScreenApplied()
{
    const int64_t monotonic = steadyMicros() - m_epochSteadyMicros;
    // First screen change after a keystroke → an echo-latency sample.
    if (m_pendingInputMonotonicMicros >= 0) {
        const int64_t latency = monotonic - m_pendingInputMonotonicMicros;
        if (latency >= 0) {
            m_latencyRing[m_latencyHead] = latency;
            m_latencyHead = (m_latencyHead + 1) % m_latencyRing.size();
            if (m_latencyCount < m_latencyRing.size())
                ++m_latencyCount;
            m_lastLatencyMicros = latency;
        }
        m_pendingInputMonotonicMicros = -1;
    }
    m_lastScreenMonotonicMicros = monotonic;
}

void SyncTrace::recordResync() { ++m_totals.resyncs; }

void SyncTrace::recordViewportStated() { ++m_totals.viewportsStated; }

void SyncTrace::recordEvent(const std::string& text)
{
    TraceEvent& slot = m_events[m_eventHead];
    slot.unixMicros = unixMicros();
    slot.monotonicMicros = steadyMicros() - m_epochSteadyMicros;
    slot.text = text;
    m_eventHead = (m_eventHead + 1) % m_events.size();
    if (m_eventCount < m_events.size())
        ++m_eventCount;
}

SyncTrace::Totals SyncTrace::totals() const
{
    Totals out = m_totals;
    const int64_t now = steadyMicros() - m_epochSteadyMicros;
    out.uptimeMicros = now;
    out.lastScreenAgeMicros = m_lastScreenMonotonicMicros >= 0 ? now - m_lastScreenMonotonicMicros : -1;
    return out;
}

SyncTrace::LatencyStats SyncTrace::latency() const
{
    LatencyStats out;
    out.count = m_latencyCount;
    out.lastMicros = m_lastLatencyMicros;
    if (m_latencyCount == 0)
        return out;

    std::vector<int64_t> sorted;
    sorted.reserve(m_latencyCount);
    for (std::size_t i = 0; i < m_latencyCount; ++i)
        sorted.push_back(m_latencyRing[i]);
    std::ranges::sort(sorted);

    const std::size_t n = sorted.size();
    out.minMicros = sorted.front();
    out.maxMicros = sorted.back();
    out.p50Micros = sorted[(n - 1) * 50 / 100];
    out.p95Micros = sorted[(n - 1) * 95 / 100];
    return out;
}

std::vector<SyncTrace::TypeRow> SyncTrace::perType() const
{
    std::vector<TypeRow> rows;
    for (int outbound = 0; outbound <= 1; ++outbound) {
        const std::vector<FrameTypeStat>& table = outbound ? m_outbound : m_inbound;
        for (std::size_t type = 0; type < table.size(); ++type) {
            if (table[type].count == 0)
                continue;
            TypeRow row;
            row.type = static_cast<uint8_t>(type);
            row.outbound = outbound != 0;
            row.stat = table[type];
            rows.push_back(row);
        }
    }
    return rows;
}

std::vector<TracedFrame> SyncTrace::recent(std::size_t max) const
{
    const std::size_t want = max < m_ringCount ? max : m_ringCount;
    std::vector<TracedFrame> out;
    out.reserve(want);
    // The oldest retained entry is `want` slots behind the head (mod capacity).
    const std::size_t capacity = m_ring.size();
    std::size_t index = (m_ringHead + capacity - want) % capacity;
    for (std::size_t i = 0; i < want; ++i) {
        out.push_back(m_ring[index]);
        index = (index + 1) % capacity;
    }
    return out;
}

std::vector<TraceEvent> SyncTrace::events(std::size_t max) const
{
    const std::size_t want = max < m_eventCount ? max : m_eventCount;
    std::vector<TraceEvent> out;
    out.reserve(want);
    const std::size_t capacity = m_events.size();
    std::size_t index = (m_eventHead + capacity - want) % capacity;
    for (std::size_t i = 0; i < want; ++i) {
        out.push_back(m_events[index]);
        index = (index + 1) % capacity;
    }
    return out;
}

} // namespace Ubiquitty::Protocol::Sync
