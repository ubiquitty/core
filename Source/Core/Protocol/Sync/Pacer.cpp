// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Protocol/Sync/Pacer.h"

#include <algorithm>

namespace Ubiquitty::Protocol::Sync {

void AttachmentPacer::noteDatagram(std::uint32_t handle, std::uint64_t generation, std::uint64_t bytes, std::uint64_t now)
{
    m_datagrams.push_back({ .handle = handle, .generation = generation, .bytes = bytes, .sentMicroseconds = now });
    m_datagramBytesOutstanding += bytes;
}

void AttachmentPacer::cancelDatagram(std::uint32_t handle, std::uint64_t generation)
{
    std::erase_if(m_datagrams, [&](const DatagramInFlight& datagram) {
        if (datagram.handle != handle || datagram.generation != generation)
            return false;
        m_datagramBytesOutstanding -= datagram.bytes;
        return true;
    });
}

void AttachmentPacer::noteBulkStreamBytes(std::uint64_t bytes)
{
    m_streamBytesSent += bytes;
    m_bulkSpans.push_back({ .end = m_streamBytesSent, .bytes = bytes });
    m_bulkBytesOutstanding += bytes;
}

void AttachmentPacer::noteAck(std::uint64_t now, std::uint64_t hostTimeEcho, std::uint64_t holdMicroseconds, std::uint64_t bytesReceived)
{
    // A cumulative count only grows; a smaller one is stale or forged and
    // retires nothing.
    if (bytesReceived > m_streamBytesReceived && bytesReceived <= m_streamBytesSent) {
        m_bytesDeliveredSinceAck += bytesReceived - m_streamBytesReceived;
        m_streamBytesReceived = bytesReceived;
        while (!m_bulkSpans.empty() && m_bulkSpans.front().end <= m_streamBytesReceived) {
            m_bulkBytesOutstanding -= m_bulkSpans.front().bytes;
            m_bulkSpans.pop_front();
        }
    }
    if (hostTimeEcho != 0)
        noteRoundTrip(now, hostTimeEcho, holdMicroseconds);
    noteHeard(now);
    if (m_lastAckMicroseconds && now > *m_lastAckMicroseconds && m_bytesDeliveredSinceAck > 0) {
        const std::uint64_t interval = now - *m_lastAckMicroseconds;
        m_rateSamples.push_back({ .at = now, .bytesPerSecond = m_bytesDeliveredSinceAck * 1'000'000 / interval });
    }
    while (!m_rateSamples.empty() && now - m_rateSamples.front().at > m_policy.rateFilterMicroseconds)
        m_rateSamples.pop_front();
    m_bytesDeliveredSinceAck = 0;
    m_lastAckMicroseconds = now;
}

void AttachmentPacer::retireDatagrams(std::uint32_t handle, std::uint64_t generation)
{
    std::erase_if(m_datagrams, [&](const DatagramInFlight& datagram) {
        if (datagram.handle != handle || datagram.generation > generation)
            return false;
        m_datagramBytesOutstanding -= datagram.bytes;
        m_bytesDeliveredSinceAck += datagram.bytes;
        return true;
    });
}

bool AttachmentPacer::noteEcho(std::uint64_t now, std::uint64_t timeEcho, std::uint64_t holdMicroseconds)
{
    noteHeard(now);
    // Only the probe outstanding is a sample: an Echo of any other time is a
    // peer's to choose, and a forged one would set the round trip.
    if (!m_outstandingProbe || timeEcho != *m_outstandingProbe)
        return false;
    m_outstandingProbe.reset();
    noteRoundTrip(now, timeEcho, holdMicroseconds);
    return true;
}

void AttachmentPacer::noteProbe(std::uint64_t now, std::uint64_t time)
{
    m_lastProbeMicroseconds = now;
    m_outstandingProbe = time;
    ++m_probeStep;
}

std::uint64_t AttachmentPacer::probeIntervalMicroseconds(std::size_t step) const
{
    std::uint64_t interval = m_policy.probeIdleMicroseconds;
    for (std::size_t doubled = 0; doubled < step && interval < m_policy.maxProbeIntervalMicroseconds; ++doubled)
        interval *= 2;
    return std::min(interval, std::max(m_policy.maxProbeIntervalMicroseconds, m_policy.probeIdleMicroseconds));
}

void AttachmentPacer::noteRoundTrip(std::uint64_t now, std::uint64_t hostTimeEcho, std::uint64_t holdMicroseconds)
{
    // An echo from the future, or a hold longer than the whole interval, is
    // not a sample.
    if (hostTimeEcho > now || now - hostTimeEcho < holdMicroseconds)
        return;
    const std::uint64_t sample = now - hostTimeEcho - holdMicroseconds;
    // RFC 6298 §2: SRTT <- (1 - 1/8) * SRTT + 1/8 * R'.
    m_smoothedRoundTrip = m_smoothedRoundTrip ? (*m_smoothedRoundTrip * 7 + sample) / 8 : sample;
    m_lastRoundTripSampleMicroseconds = now;
}

std::uint64_t AttachmentPacer::datagramTimeoutMicroseconds() const
{
    return std::max(m_policy.datagramLossFloorMicroseconds, 2 * m_smoothedRoundTrip.value_or(0));
}

std::vector<DatagramInFlight> AttachmentPacer::expireDatagrams(std::uint64_t now)
{
    const std::uint64_t timeout = datagramTimeoutMicroseconds();
    std::vector<DatagramInFlight> lost;
    std::erase_if(m_datagrams, [&](const DatagramInFlight& datagram) {
        if (now - datagram.sentMicroseconds < timeout)
            return false;
        m_datagramBytesOutstanding -= datagram.bytes;
        lost.push_back(datagram);
        return true;
    });
    return lost;
}

std::uint64_t AttachmentPacer::deliveryRateBytesPerSecond() const
{
    std::uint64_t rate = 0;
    for (const RateSample& sample : m_rateSamples)
        rate = std::max(rate, sample.bytesPerSecond);
    return rate;
}

std::uint64_t AttachmentPacer::windowBytes(std::uint64_t now) const
{
    if (!m_smoothedRoundTrip || now - m_lastRoundTripSampleMicroseconds > m_policy.roundTripStaleMicroseconds)
        return m_policy.windowFloorBytes;
    const std::uint64_t window = 2 * deliveryRateBytesPerSecond() * *m_smoothedRoundTrip / 1'000'000;
    return std::max(m_policy.windowFloorBytes, window);
}

std::uint64_t AttachmentPacer::outstandingBytes() const
{
    return (m_streamBytesSent - m_streamBytesReceived) + m_datagramBytesOutstanding;
}

bool AttachmentPacer::admits(std::uint64_t bytes, std::uint64_t now, std::uint64_t queuedBytes) const
{
    const std::uint64_t outstanding = outstandingBytes() + queuedBytes;
    return outstanding == 0 || outstanding + bytes <= windowBytes(now);
}

std::uint64_t AttachmentPacer::bulkWindowBytes(std::uint64_t now) const
{
    if (!m_smoothedRoundTrip || now - m_lastRoundTripSampleMicroseconds > m_policy.roundTripStaleMicroseconds)
        return bulkWindowFloorBytes;
    return std::max(bulkWindowFloorBytes, 2 * deliveryRateBytesPerSecond() * *m_smoothedRoundTrip / 1'000'000);
}

bool AttachmentPacer::admitsBulk(std::uint64_t bytes, std::uint64_t now, std::uint64_t queuedBytes) const
{
    return admits(bytes, now, queuedBytes) && (m_bulkBytesOutstanding == 0 || m_bulkBytesOutstanding + bytes <= bulkWindowBytes(now));
}

bool AttachmentPacer::isProbeDue(std::uint64_t now) const
{
    return now >= nextProbeMicroseconds();
}

std::uint64_t AttachmentPacer::nextProbeMicroseconds() const
{
    const std::uint64_t quietSince = std::max(m_lastScreenDataMicroseconds, m_lastProbeMicroseconds);
    return quietSince + probeIntervalMicroseconds(m_probeStep) + 1;
}

std::optional<std::uint64_t> AttachmentPacer::nextDatagramExpiryMicroseconds() const
{
    if (m_datagrams.empty())
        return std::nullopt;
    std::uint64_t oldest = m_datagrams.front().sentMicroseconds;
    for (const DatagramInFlight& datagram : m_datagrams)
        oldest = std::min(oldest, datagram.sentMicroseconds);
    return oldest + datagramTimeoutMicroseconds();
}

} // namespace Ubiquitty::Protocol::Sync
