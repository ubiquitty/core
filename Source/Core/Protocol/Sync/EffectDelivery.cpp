// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Protocol/Sync/EffectDelivery.h"

#include "Core/Protocol/Sync/Envelope.h"
#include "Core/Protocol/Sync/Wire.h"

#include <algorithm>
#include <new>

namespace Ubiquitty::Protocol::Sync {

namespace {

bool isValidPacket(ID128 paneID, const EffectPacket& packet)
{
    if (paneID == ID128 {} || !packet.throughSequence || packet.remainingRecords >= maxEffectRecords)
        return false;
    if (!packet.record)
        return !packet.remainingRecords;
    const EffectView& record = *packet.record;
    return record.paneID == paneID && record.sequence && record.count
        && record.generation <= packet.generation
        && record.sequence <= packet.throughSequence
        && packet.remainingRecords <= packet.throughSequence - record.sequence;
}

} // namespace

std::expected<EffectPacket, EffectCodecError> decodeEffectPacket(ID128 paneID, std::span<const std::uint8_t> bytes)
{
    if (bytes.size() > maxFrameBodyBytes)
        return std::unexpected(EffectCodecError::TooLarge);
    WireReader reader(bytes);
    EffectPacket packet;
    packet.generation = reader.u64();
    packet.throughSequence = reader.u64();
    packet.remainingRecords = reader.u16();
    if (!reader.ok())
        return std::unexpected(EffectCodecError::Malformed);
    if (!reader.isAtEnd()) {
        const auto decoded = decodeEffect(paneID, reader.take(reader.remaining()));
        if (!decoded)
            return std::unexpected(decoded.error());
        packet.record = *decoded;
    }
    if (!isValidPacket(paneID, packet))
        return std::unexpected(EffectCodecError::Malformed);
    return packet;
}

std::expected<EffectCodecBytes, EffectCodecError> encodeEffectPacket(ID128 paneID,
    std::uint64_t generation, std::uint64_t throughSequence, std::uint16_t remainingRecords, std::span<const std::uint8_t> encodedRecord)
{
    if (encodedRecord.size() > maxFrameBodyBytes - effectPacketHeaderBytes)
        return std::unexpected(EffectCodecError::TooLarge);
    EffectPacket packet { .generation = generation, .throughSequence = throughSequence, .remainingRecords = remainingRecords, .record = std::nullopt };
    if (!encodedRecord.empty()) {
        const auto decoded = decodeEffect(paneID, encodedRecord);
        if (!decoded)
            return std::unexpected(decoded.error());
        packet.record = *decoded;
    }
    if (!isValidPacket(paneID, packet))
        return std::unexpected(EffectCodecError::Malformed);
    const std::size_t size = effectPacketHeaderBytes + encodedRecord.size();
    EffectCodecBytes bytes { std::unique_ptr<std::uint8_t[]>(new (std::nothrow) std::uint8_t[size]), size };
    if (!bytes.bytes)
        return std::unexpected(EffectCodecError::NoMemory);
    for (std::size_t index = 0; index < sizeof(generation); ++index) {
        bytes.bytes[index] = static_cast<std::uint8_t>(generation >> (index * 8));
        bytes.bytes[sizeof(generation) + index] = static_cast<std::uint8_t>(throughSequence >> (index * 8));
    }
    bytes.bytes[effectPacketHeaderBytes - 2] = static_cast<std::uint8_t>(remainingRecords);
    bytes.bytes[effectPacketHeaderBytes - 1] = static_cast<std::uint8_t>(remainingRecords >> 8);
    std::copy(encodedRecord.begin(), encodedRecord.end(), bytes.bytes.get() + effectPacketHeaderBytes);
    return bytes;
}

std::expected<void, EffectCodecError> EffectReceiveCursor::restore(EffectReceiptState state)
{
    if (m_paneID == ID128 {} || m_hasReceived || state.consumed < state.acknowledged)
        return std::unexpected(EffectCodecError::Malformed);
    m_state = state;
    return {};
}

std::expected<EffectReception, EffectCodecError> EffectReceiveCursor::accept(const EffectPacket& packet, EffectScreenBoundary screen)
{
    if (!isValidPacket(m_paneID, packet) || packet.throughSequence < m_state.consumed
        || (screen.view != ViewState::Live && screen.view != ViewState::Dormant && screen.view != ViewState::RecordsOnly)
        || (screen.view == ViewState::Live && packet.generation > screen.appliedGeneration))
        return std::unexpected(EffectCodecError::Malformed);
    if (m_pendingThrough) {
        if (packet.generation != m_pendingGeneration || packet.throughSequence != m_pendingThrough || packet.remainingRecords != m_expectedRemaining || !packet.record)
            return std::unexpected(EffectCodecError::Malformed);
    }
    const std::uint64_t previousSequence = m_pendingThrough ? m_lastBatchSequence : 0;
    if (packet.record && packet.record->sequence <= previousSequence)
        return std::unexpected(EffectCodecError::Malformed);

    EffectReception reception = EffectReception::CursorOnly;
    if (packet.record && packet.record->sequence > m_state.consumed) {
        m_state.consumed = packet.record->sequence;
        reception = EffectReception::Record;
    }
    m_hasReceived = true;
    if (packet.remainingRecords) {
        m_pendingGeneration = packet.generation;
        m_pendingThrough = packet.throughSequence;
        m_expectedRemaining = packet.remainingRecords - 1;
        m_lastBatchSequence = packet.record->sequence;
    } else {
        m_state.acknowledged = packet.throughSequence;
        // Skipped records have no application action to replay. A resumed
        // consecutive delivery must not resurrect an already retired sound.
        m_state.consumed = std::max(m_state.consumed, packet.throughSequence);
        m_pendingGeneration = 0;
        m_pendingThrough = 0;
        m_lastBatchSequence = 0;
        m_expectedRemaining = 0;
    }
    return reception;
}

} // namespace Ubiquitty::Protocol::Sync
