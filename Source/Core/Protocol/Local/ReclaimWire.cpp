// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Protocol/Local/ReclaimWire.h"

#include "Core/Protocol/Sync/Wire.h"

#include <algorithm>
#include <cassert>
#include <limits>
#include <utility>

namespace Ubiquitty::Protocol::Local {
namespace {

bool isValid(Sync::ID128 identity)
{
    return identity.low || identity.high;
}

bool isValid(AdoptWireOperation operation)
{
    return isValid(operation.operationID) && operation.daemonInstanceID;
}

bool isCommand(ReclaimWireMessage kind)
{
    switch (kind) {
    case ReclaimWireMessage::Prepare:
    case ReclaimWireMessage::Commit:
    case ReclaimWireMessage::Abort:
    case ReclaimWireMessage::Status:
        return true;
    case ReclaimWireMessage::Capsule:
    case ReclaimWireMessage::Outcome:
    case ReclaimWireMessage::Refused:
        return false;
    }
    return false;
}

/// Writes only into a fixed owner after its complete extent was admitted.
class RecordWriter {
public:
    explicit RecordWriter(std::span<std::uint8_t> bytes)
        : m_bytes(bytes)
    {
    }

    void scalar(std::uint64_t value, std::size_t width)
    {
        assert(width <= 8 && width <= m_bytes.size() - m_offset);
        for (std::size_t index = 0; index < width; ++index)
            m_bytes[m_offset++] = static_cast<std::uint8_t>(value >> (index * 8));
    }

    void bytes(std::span<const std::uint8_t> bytes)
    {
        assert(bytes.size() <= m_bytes.size() - m_offset);
        std::ranges::copy(bytes, m_bytes.begin() + m_offset);
        m_offset += bytes.size();
    }

    void identity(Sync::ID128 identity)
    {
        scalar(identity.low, 8);
        scalar(identity.high, 8);
    }

    void operation(AdoptWireOperation operation)
    {
        identity(operation.operationID);
        scalar(operation.daemonInstanceID, 8);
    }

    void selector(const ReclaimWireSelector& selector)
    {
        operation(selector.adoption);
        operation(selector.operation);
        scalar(selector.target.handle, 4);
        scalar(selector.target.sessionID, 4);
        scalar(selector.target.daemonInstanceID, 8);
        identity(selector.target.hostInstanceID);
        identity(selector.target.attachmentID);
        identity(selector.target.paneID);
        identity(selector.barrier.clientInstance);
        scalar(selector.barrier.finalInputSeq, 8);
    }

    void optionalDigest(const std::optional<std::array<std::uint8_t, 32>>& digest)
    {
        scalar(digest.has_value(), 1);
        bytes(digest.value_or(std::array<std::uint8_t, 32> {}));
    }

    void header(ReclaimWireMessage kind, std::size_t recordBytes)
    {
        scalar(std::to_underlying(kind), 1);
        scalar(recordBytes - reclaimWireFrameHeaderBytes, 4);
    }

    [[nodiscard]] std::size_t size() const { return m_offset; }

private:
    std::span<std::uint8_t> m_bytes;
    std::size_t m_offset = 0;
};

Sync::ID128 readIdentity(Sync::WireReader& reader)
{
    return { .low = reader.u64(), .high = reader.u64() };
}

AdoptWireOperation readOperation(Sync::WireReader& reader)
{
    return { .operationID = readIdentity(reader), .daemonInstanceID = reader.u64() };
}

ReclaimWireSelector readSelector(Sync::WireReader& reader)
{
    ReclaimWireSelector selector;
    selector.adoption = readOperation(reader);
    selector.operation = readOperation(reader);
    selector.target.handle = reader.u32();
    selector.target.sessionID = reader.u32();
    selector.target.daemonInstanceID = reader.u64();
    selector.target.hostInstanceID = readIdentity(reader);
    selector.target.attachmentID = readIdentity(reader);
    selector.target.paneID = readIdentity(reader);
    selector.barrier.clientInstance = readIdentity(reader);
    selector.barrier.finalInputSeq = reader.u64();
    return selector;
}

std::optional<std::array<std::uint8_t, 32>> readOptionalDigest(Sync::WireReader& reader)
{
    const auto present = reader.u8();
    const auto bytes = reader.take(32);
    if (!reader.ok() || present > 1) {
        reader.fail();
        return std::nullopt;
    }
    if (!present) {
        if (std::ranges::any_of(bytes, [](auto byte) { return byte != 0; }))
            reader.fail();
        return std::nullopt;
    }
    std::array<std::uint8_t, 32> digest;
    std::ranges::copy(bytes, digest.begin());
    return digest;
}

std::expected<std::span<const std::uint8_t>, AdoptWireError> recordBody(std::span<const std::uint8_t> bytes,
    ReclaimWireMessage kind, std::size_t maxRecordBytes)
{
    if (bytes.size() < reclaimWireFrameHeaderBytes)
        return std::unexpected(AdoptWireError::Malformed);
    const auto header = decodeReclaimWireHeader(bytes.first(reclaimWireFrameHeaderBytes), maxRecordBytes);
    if (!header)
        return std::unexpected(header.error());
    if (header->kind != kind || header->bodyBytes != bytes.size() - reclaimWireFrameHeaderBytes)
        return std::unexpected(AdoptWireError::Malformed);
    return bytes.subspan(reclaimWireFrameHeaderBytes);
}

std::expected<void, AdoptWireError> validateCommand(const ReclaimWireCommand& command)
{
    if (!isCommand(command.kind))
        return std::unexpected(AdoptWireError::Malformed);
    if (!command.correlation || !isValidReclaimWireSelector(command.selector))
        return std::unexpected(AdoptWireError::InvalidIdentity);
    if ((command.kind == ReclaimWireMessage::Prepare && command.capsuleSHA256)
        || (command.kind == ReclaimWireMessage::Commit && !command.capsuleSHA256))
        return std::unexpected(AdoptWireError::InvalidPayload);
    return {};
}

std::expected<void, AdoptWireError> validateOutcome(const ReclaimWireOutcome& outcome)
{
    if (!isValidReclaimWireSelector(outcome.selector))
        return std::unexpected(AdoptWireError::InvalidIdentity);
    if (outcome.outcome.operation != outcome.selector.operation)
        return std::unexpected(AdoptWireError::InvalidPayload);
    const auto encoded = encodeAdoptWireOutcome(outcome.outcome);
    if (!encoded)
        return std::unexpected(encoded.error());
    const auto& target = outcome.outcome.target;
    if ((target.sessionID || isValid(target.paneID))
        && (target.sessionID != outcome.selector.target.sessionID || target.paneID != outcome.selector.target.paneID))
        return std::unexpected(AdoptWireError::InvalidPayload);
    if ((outcome.outcome.disposition == AdoptWireDisposition::Prepared || outcome.outcome.disposition == AdoptWireDisposition::Committed)
        && !outcome.capsuleSHA256)
        return std::unexpected(AdoptWireError::InvalidPayload);
    return {};
}

std::expected<void, AdoptWireError> validateRefused(const ReclaimWireRefused& refused)
{
    if (!isValidReclaimWireSelector(refused.selector))
        return std::unexpected(AdoptWireError::InvalidIdentity);
    if (refused.refusal.operation != refused.selector.operation)
        return std::unexpected(AdoptWireError::InvalidPayload);
    const auto encoded = encodeAdoptWireRefused(refused.refusal);
    if (!encoded)
        return std::unexpected(encoded.error());
    return {};
}

std::expected<std::size_t, AdoptWireError> capsuleRecordBytes(const ReclaimWireCapsule& capsule, const ReclaimWireLimits& limits)
{
    if (!isValidReclaimWireSelector(capsule.selector))
        return std::unexpected(AdoptWireError::InvalidIdentity);
    if (const auto valid = validateOutcome({ .selector = capsule.selector, .capsuleSHA256 = capsule.capsuleSHA256, .outcome = capsule.prepared }); !valid)
        return std::unexpected(valid.error());
    if (capsule.prepared.disposition != AdoptWireDisposition::Prepared || capsule.terminalBody.empty())
        return std::unexpected(AdoptWireError::InvalidPayload);
    if (limits.maxRuns > reclaimWireMaxRuns || capsule.acceptedRuns.size() > limits.maxRuns
        || capsule.acceptedRuns.size() > reclaimWireMaxRuns || capsule.terminalBody.size() > limits.maxTerminalBytes
        || capsule.acceptedPTY.size() > limits.maxAcceptedPTYBytes)
        return std::unexpected(AdoptWireError::TooLong);
    std::size_t covered = 0;
    for (std::size_t index = 0; index < capsule.acceptedRuns.size(); ++index) {
        const auto& run = capsule.acceptedRuns[index];
        if (!run.bytes || run.bytes > capsule.acceptedPTY.size() - covered
            || (index && capsule.acceptedRuns[index - 1].owner == run.owner))
            return std::unexpected(AdoptWireError::InvalidPayload);
        covered += static_cast<std::size_t>(run.bytes);
    }
    if (covered != capsule.acceptedPTY.size())
        return std::unexpected(AdoptWireError::InvalidPayload);
    const std::uint64_t wireAllowance = std::uint64_t { std::numeric_limits<std::uint32_t>::max() } + reclaimWireFrameHeaderBytes;
    std::size_t remaining = limits.maxRecordBytes;
    std::size_t recordBytes = 0;
    const auto charge = [&](std::size_t bytes) {
        if (bytes > remaining || bytes > wireAllowance - recordBytes)
            return false;
        remaining -= bytes;
        recordBytes += bytes;
        return true;
    };
    if (!charge(reclaimWireCapsuleHeaderBytes) || !charge(capsule.acceptedRuns.size() * 16)
        || !charge(capsule.terminalBody.size()) || !charge(capsule.acceptedPTY.size()))
        return std::unexpected(AdoptWireError::TooLong);
    return recordBytes;
}

} // namespace

bool isValidReclaimWireSelector(const ReclaimWireSelector& selector)
{
    return isValid(selector.adoption) && isValid(selector.operation)
        && selector.adoption.operationID != selector.operation.operationID
        && selector.adoption.daemonInstanceID == selector.operation.daemonInstanceID
        && selector.operation.daemonInstanceID == selector.target.daemonInstanceID
        && selector.target.handle && selector.target.sessionID && isValid(selector.target.hostInstanceID)
        && isValid(selector.target.attachmentID) && isValid(selector.target.paneID) && isValid(selector.barrier.clientInstance);
}

std::expected<std::array<std::uint8_t, localControlReplyBytes>, AdoptWireError> encodeLocalReclaimReady(const ReclaimWireReady& ready)
{
    if (!ready.correlation || !ready.daemonInstanceID)
        return std::unexpected(AdoptWireError::InvalidIdentity);
    if (ready.status != ReclaimWireReadyStatus::Ready && ready.status != ReclaimWireReadyStatus::Refused)
        return std::unexpected(AdoptWireError::InvalidPayload);
    std::array<std::uint8_t, localControlReplyBytes> bytes;
    RecordWriter writer(bytes);
    writer.bytes(localControlPreamble);
    writer.scalar(localReclaimReplyOperation, 1);
    writer.scalar(std::to_underlying(ready.status), 1);
    writer.scalar(ready.correlation, 8);
    writer.scalar(ready.daemonInstanceID, 8);
    return bytes;
}

std::expected<ReclaimWireReady, AdoptWireError> decodeLocalReclaimReady(std::span<const std::uint8_t> bytes)
{
    if (bytes.size() != localControlReplyBytes || !std::ranges::equal(bytes.first(localControlPreamble.size()), localControlPreamble))
        return std::unexpected(AdoptWireError::Malformed);
    Sync::WireReader reader(bytes.subspan(localControlPreamble.size()));
    const auto operation = reader.u8();
    ReclaimWireReady ready { .correlation = 0, .daemonInstanceID = 0, .status = static_cast<ReclaimWireReadyStatus>(reader.u8()) };
    ready.correlation = reader.u64();
    ready.daemonInstanceID = reader.u64();
    if (!reader.ok() || !reader.isAtEnd() || operation != localReclaimReplyOperation)
        return std::unexpected(AdoptWireError::Malformed);
    if (const auto valid = encodeLocalReclaimReady(ready); !valid)
        return std::unexpected(valid.error());
    return ready;
}

std::expected<ReclaimWireHeader, AdoptWireError> decodeReclaimWireHeader(std::span<const std::uint8_t> bytes, std::size_t maxRecordBytes)
{
    if (bytes.size() != reclaimWireFrameHeaderBytes)
        return std::unexpected(AdoptWireError::Malformed);
    Sync::WireReader reader(bytes);
    ReclaimWireHeader header { .kind = static_cast<ReclaimWireMessage>(reader.u8()), .bodyBytes = reader.u32() };
    const auto recordBytes = std::uint64_t { header.bodyBytes } + reclaimWireFrameHeaderBytes;
    if (recordBytes > std::numeric_limits<std::size_t>::max() || recordBytes > maxRecordBytes)
        return std::unexpected(AdoptWireError::TooLong);
    switch (header.kind) {
    case ReclaimWireMessage::Prepare:
    case ReclaimWireMessage::Commit:
    case ReclaimWireMessage::Abort:
    case ReclaimWireMessage::Status:
        if (recordBytes == reclaimWireCommandBytes)
            return header;
        break;
    case ReclaimWireMessage::Capsule:
        if (recordBytes > reclaimWireCapsuleHeaderBytes)
            return header;
        break;
    case ReclaimWireMessage::Outcome:
        if (recordBytes == reclaimWireOutcomeBytes)
            return header;
        break;
    case ReclaimWireMessage::Refused:
        if (recordBytes == reclaimWireRefusedBytes)
            return header;
        break;
    }
    return std::unexpected(AdoptWireError::Malformed);
}

std::expected<std::array<std::uint8_t, reclaimWireCommandBytes>, AdoptWireError> encodeReclaimWireCommand(const ReclaimWireCommand& command)
{
    if (const auto valid = validateCommand(command); !valid)
        return std::unexpected(valid.error());
    std::array<std::uint8_t, reclaimWireCommandBytes> bytes;
    RecordWriter writer(bytes);
    writer.header(command.kind, bytes.size());
    writer.scalar(command.correlation, 8);
    writer.selector(command.selector);
    writer.optionalDigest(command.capsuleSHA256);
    return bytes;
}

std::expected<ReclaimWireCommand, AdoptWireError> decodeReclaimWireCommand(std::span<const std::uint8_t> bytes)
{
    if (bytes.size() < reclaimWireFrameHeaderBytes)
        return std::unexpected(AdoptWireError::Malformed);
    const auto kind = static_cast<ReclaimWireMessage>(bytes.front());
    if (!isCommand(kind))
        return std::unexpected(AdoptWireError::Malformed);
    const auto body = recordBody(bytes, kind, reclaimWireCommandBytes);
    if (!body)
        return std::unexpected(body.error());
    Sync::WireReader reader(*body);
    ReclaimWireCommand command { .kind = kind, .correlation = reader.u64(), .selector = readSelector(reader), .capsuleSHA256 = readOptionalDigest(reader) };
    if (!reader.ok() || !reader.isAtEnd())
        return std::unexpected(AdoptWireError::Malformed);
    if (const auto valid = validateCommand(command); !valid)
        return std::unexpected(valid.error());
    return command;
}

std::expected<EncodedReclaimWireCapsule, AdoptWireError> encodeReclaimWireCapsule(const ReclaimWireCapsule& capsule, const ReclaimWireLimits& limits)
{
    const auto recordBytes = capsuleRecordBytes(capsule, limits);
    if (!recordBytes)
        return std::unexpected(recordBytes.error());
    const auto prepared = encodeAdoptWireOutcome(capsule.prepared);
    if (!prepared)
        return std::unexpected(prepared.error());
    EncodedReclaimWireCapsule encoded;
    RecordWriter writer(encoded.bytes);
    writer.header(ReclaimWireMessage::Capsule, *recordBytes);
    writer.selector(capsule.selector);
    writer.bytes(capsule.capsuleSHA256);
    writer.scalar(capsule.inputWritten.has_value(), 1);
    writer.scalar(capsule.inputWritten.value_or(0), 8);
    writer.scalar(capsule.terminalBody.size(), 8);
    writer.scalar(capsule.acceptedPTY.size(), 8);
    writer.scalar(capsule.acceptedRuns.size(), 2);
    writer.bytes(*prepared);
    for (const auto& run : capsule.acceptedRuns) {
        writer.scalar(run.owner, 8);
        writer.scalar(run.bytes, 8);
    }
    encoded.length = writer.size();
    encoded.terminalBody = capsule.terminalBody;
    encoded.acceptedPTY = capsule.acceptedPTY;
    return encoded;
}

std::expected<ReclaimWireCapsule, AdoptWireError> decodeReclaimWireCapsule(std::span<const std::uint8_t> bytes,
    const ReclaimWireLimits& limits, std::span<ReclaimWireRun> runStorage)
{
    const auto body = recordBody(bytes, ReclaimWireMessage::Capsule, limits.maxRecordBytes);
    if (!body)
        return std::unexpected(body.error());
    Sync::WireReader reader(*body);
    ReclaimWireCapsule capsule;
    capsule.selector = readSelector(reader);
    const auto digest = reader.take(capsule.capsuleSHA256.size());
    const auto hasReceipt = reader.u8();
    const auto inputWritten = reader.u64();
    const auto terminalBytes = reader.u64();
    const auto acceptedBytes = reader.u64();
    const auto runCount = reader.u16();
    const auto prepared = decodeAdoptWireOutcome(reader.take(adoptWireOutcomeBytes));
    if (!reader.ok() || hasReceipt > 1 || (!hasReceipt && inputWritten) || !prepared)
        return std::unexpected(AdoptWireError::Malformed);
    if (limits.maxRuns > reclaimWireMaxRuns || runCount > reclaimWireMaxRuns || runCount > limits.maxRuns
        || runCount > runStorage.size() || terminalBytes > limits.maxTerminalBytes || acceptedBytes > limits.maxAcceptedPTYBytes
        || !std::in_range<std::size_t>(terminalBytes) || !std::in_range<std::size_t>(acceptedBytes))
        return std::unexpected(AdoptWireError::TooLong);
    std::ranges::copy(digest, capsule.capsuleSHA256.begin());
    capsule.prepared = *prepared;
    if (hasReceipt)
        capsule.inputWritten = inputWritten;
    std::array<ReclaimWireRun, reclaimWireMaxRuns> candidateRuns;
    for (std::size_t index = 0; index < runCount; ++index)
        candidateRuns[index] = { .owner = reader.u64(), .bytes = reader.u64() };
    capsule.acceptedRuns = std::span(candidateRuns).first(runCount);
    capsule.terminalBody = reader.take(static_cast<std::size_t>(terminalBytes));
    capsule.acceptedPTY = reader.take(static_cast<std::size_t>(acceptedBytes));
    if (!reader.ok() || !reader.isAtEnd())
        return std::unexpected(AdoptWireError::Malformed);
    if (const auto valid = capsuleRecordBytes(capsule, limits); !valid)
        return std::unexpected(valid.error());
    std::ranges::copy(capsule.acceptedRuns, runStorage.begin());
    capsule.acceptedRuns = runStorage.first(runCount);
    return capsule;
}

std::expected<std::array<std::uint8_t, reclaimWireOutcomeBytes>, AdoptWireError> encodeReclaimWireOutcome(const ReclaimWireOutcome& outcome)
{
    if (const auto valid = validateOutcome(outcome); !valid)
        return std::unexpected(valid.error());
    const auto retained = encodeAdoptWireOutcome(outcome.outcome);
    if (!retained)
        return std::unexpected(retained.error());
    std::array<std::uint8_t, reclaimWireOutcomeBytes> bytes;
    RecordWriter writer(bytes);
    writer.header(ReclaimWireMessage::Outcome, bytes.size());
    writer.selector(outcome.selector);
    writer.optionalDigest(outcome.capsuleSHA256);
    writer.bytes(*retained);
    return bytes;
}

std::expected<ReclaimWireOutcome, AdoptWireError> decodeReclaimWireOutcome(std::span<const std::uint8_t> bytes)
{
    const auto body = recordBody(bytes, ReclaimWireMessage::Outcome, reclaimWireOutcomeBytes);
    if (!body)
        return std::unexpected(body.error());
    Sync::WireReader reader(*body);
    ReclaimWireOutcome outcome { .selector = readSelector(reader), .capsuleSHA256 = readOptionalDigest(reader) };
    const auto retained = decodeAdoptWireOutcome(reader.take(adoptWireOutcomeBytes));
    if (!reader.ok() || !reader.isAtEnd() || !retained)
        return std::unexpected(AdoptWireError::Malformed);
    outcome.outcome = *retained;
    if (const auto valid = validateOutcome(outcome); !valid)
        return std::unexpected(valid.error());
    return outcome;
}

std::expected<std::array<std::uint8_t, reclaimWireRefusedBytes>, AdoptWireError> encodeReclaimWireRefused(const ReclaimWireRefused& refused)
{
    if (const auto valid = validateRefused(refused); !valid)
        return std::unexpected(valid.error());
    const auto refusal = encodeAdoptWireRefused(refused.refusal);
    if (!refusal)
        return std::unexpected(refusal.error());
    std::array<std::uint8_t, reclaimWireRefusedBytes> bytes;
    RecordWriter writer(bytes);
    writer.header(ReclaimWireMessage::Refused, bytes.size());
    writer.selector(refused.selector);
    writer.optionalDigest(refused.capsuleSHA256);
    writer.bytes(*refusal);
    return bytes;
}

std::expected<ReclaimWireRefused, AdoptWireError> decodeReclaimWireRefused(std::span<const std::uint8_t> bytes)
{
    const auto body = recordBody(bytes, ReclaimWireMessage::Refused, reclaimWireRefusedBytes);
    if (!body)
        return std::unexpected(body.error());
    Sync::WireReader reader(*body);
    ReclaimWireRefused refused { .selector = readSelector(reader), .capsuleSHA256 = readOptionalDigest(reader) };
    const auto refusal = decodeAdoptWireRefused(reader.take(adoptWireRefusedBytes));
    if (!reader.ok() || !reader.isAtEnd() || !refusal)
        return std::unexpected(AdoptWireError::Malformed);
    refused.refusal = *refusal;
    if (const auto valid = validateRefused(refused); !valid)
        return std::unexpected(valid.error());
    return refused;
}

} // namespace Ubiquitty::Protocol::Local
