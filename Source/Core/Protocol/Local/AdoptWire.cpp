// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Protocol/Local/AdoptWire.h"

#include "Core/Crypto/HMAC.h"
#include "Core/Protocol/Local/Control.h"
#include "Core/Protocol/Sync/Wire.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace Ubiquitty::Protocol::Local {
namespace {

constexpr std::size_t commandBodyBytes = adoptWireCommandBytes - adoptWireFrameHeaderBytes;
constexpr std::size_t prepareMetadataBytes = adoptWirePrepareHeaderBytes - adoptWireFrameHeaderBytes;

bool isValid(AdoptWireOperation operation)
{
    return operation.daemonInstanceID && (operation.operationID.low || operation.operationID.high);
}

bool isValid(AdoptWireRejection rejection)
{
    switch (rejection) {
    case AdoptWireRejection::InvalidPayload:
    case AdoptWireRejection::NotOwner:
    case AdoptWireRejection::NoMemory:
    case AdoptWireRejection::InvalidCandidate:
    case AdoptWireRejection::HostFull:
        return true;
    }
    return false;
}

bool isValid(AdoptWireRefusal reason)
{
    switch (reason) {
    case AdoptWireRefusal::InvalidIdentity:
    case AdoptWireRefusal::InvalidPolicy:
    case AdoptWireRefusal::NoMemory:
    case AdoptWireRefusal::Busy:
    case AdoptWireRefusal::StaleAuthority:
    case AdoptWireRefusal::Unauthorised:
    case AdoptWireRefusal::Conflict:
    case AdoptWireRefusal::Reentrant:
    case AdoptWireRefusal::ReplyUnavailable:
    case AdoptWireRefusal::InvalidTime:
    case AdoptWireRefusal::GenerationExhausted:
    case AdoptWireRefusal::ActiveCandidate:
    case AdoptWireRefusal::RetirementUnproven:
        return true;
    }
    return false;
}

bool isCommand(AdoptWireMessage kind)
{
    switch (kind) {
    case AdoptWireMessage::Commit:
    case AdoptWireMessage::Abort:
    case AdoptWireMessage::Status:
        return true;
    case AdoptWireMessage::Prepare:
    case AdoptWireMessage::Outcome:
    case AdoptWireMessage::Refused:
        return false;
    }
    return false;
}

std::expected<void, AdoptWireError> validateOutcome(const AdoptWireOutcome& outcome)
{
    if (!outcome.correlation || !isValid(outcome.operation))
        return std::unexpected(AdoptWireError::InvalidIdentity);
    const bool hasPane = outcome.target.paneID.low || outcome.target.paneID.high;
    const bool hasTarget = outcome.target.sessionID && hasPane;
    const bool isEmpty = !outcome.target.sessionID && !hasPane;
    if ((!hasTarget && !isEmpty) || (outcome.rejection && !isValid(*outcome.rejection)))
        return std::unexpected(AdoptWireError::InvalidPayload);
    switch (outcome.disposition) {
    case AdoptWireDisposition::Unknown:
        if (isEmpty && !outcome.rejection)
            return {};
        break;
    case AdoptWireDisposition::Prepared:
    case AdoptWireDisposition::Committed:
        if (hasTarget && !outcome.rejection)
            return {};
        break;
    case AdoptWireDisposition::Aborted:
    case AdoptWireDisposition::Expired:
        if (!outcome.rejection)
            return {};
        break;
    case AdoptWireDisposition::Rejected:
        if (outcome.rejection)
            return {};
        break;
    }
    return std::unexpected(AdoptWireError::InvalidPayload);
}

void write32(std::span<std::uint8_t, 4> bytes, std::uint32_t value)
{
    for (std::size_t index = 0; index < bytes.size(); ++index)
        bytes[index] = static_cast<std::uint8_t>(value >> (8 * index));
}

void writeFrameHeader(std::span<std::uint8_t, adoptWireFrameHeaderBytes> bytes, AdoptWireMessage kind, std::uint32_t bodyBytes)
{
    bytes[0] = std::to_underlying(kind);
    write32(bytes.last<4>(), bodyBytes);
}

void writeRequest(std::span<std::uint8_t, commandBodyBytes> bytes, std::uint64_t correlation, AdoptWireOperation operation)
{
    LocalControlDetail::write64(bytes.subspan<0, 8>(), correlation);
    LocalControlDetail::write64(bytes.subspan<8, 8>(), operation.operationID.low);
    LocalControlDetail::write64(bytes.subspan<16, 8>(), operation.operationID.high);
    LocalControlDetail::write64(bytes.subspan<24, 8>(), operation.daemonInstanceID);
}

AdoptWireOperation readOperation(Sync::WireReader& reader)
{
    return { .operationID = { .low = reader.u64(), .high = reader.u64() }, .daemonInstanceID = reader.u64() };
}

std::expected<std::span<const std::uint8_t>, AdoptWireError> recordBody(std::span<const std::uint8_t> bytes, AdoptWireMessage kind, std::size_t maxRecordBytes)
{
    if (bytes.size() < adoptWireFrameHeaderBytes)
        return std::unexpected(AdoptWireError::Malformed);
    const auto header = decodeAdoptWireHeader(bytes.first(adoptWireFrameHeaderBytes), maxRecordBytes);
    if (!header)
        return std::unexpected(header.error());
    if (header->kind != kind || header->bodyBytes != bytes.size() - adoptWireFrameHeaderBytes)
        return std::unexpected(AdoptWireError::Malformed);
    return bytes.subspan(adoptWireFrameHeaderBytes);
}

std::expected<void, AdoptWireError> validatePrepare(const AdoptWirePrepare& prepare, std::size_t maxRecordBytes)
{
    if (!prepare.correlation || !isValid(prepare.preparation.operation))
        return std::unexpected(AdoptWireError::InvalidIdentity);
    if (prepare.terminalBody.empty() || prepare.preparation.bodyBytes != prepare.terminalBody.size())
        return std::unexpected(AdoptWireError::InvalidPayload);
    if (maxRecordBytes < adoptWirePrepareHeaderBytes
        || prepare.terminalBody.size() > maxRecordBytes - adoptWirePrepareHeaderBytes
        || prepare.terminalBody.size() > std::numeric_limits<std::uint32_t>::max() - prepareMetadataBytes)
        return std::unexpected(AdoptWireError::TooLong);
    std::array<std::uint8_t, 32> digest {};
    Crypto::sha256(prepare.terminalBody, digest);
    if (!Crypto::constantTimeEqual(digest, prepare.preparation.bodySHA256))
        return std::unexpected(AdoptWireError::DigestMismatch);
    return {};
}

} // namespace

std::expected<AdoptWireHeader, AdoptWireError> decodeAdoptWireHeader(std::span<const std::uint8_t> bytes, std::size_t maxRecordBytes)
{
    if (bytes.size() != adoptWireFrameHeaderBytes)
        return std::unexpected(AdoptWireError::Malformed);
    Sync::WireReader reader(bytes);
    AdoptWireHeader header { .kind = static_cast<AdoptWireMessage>(reader.u8()), .bodyBytes = reader.u32() };
    const auto recordBytes = std::uint64_t { header.bodyBytes } + adoptWireFrameHeaderBytes;
    if (recordBytes > std::numeric_limits<std::size_t>::max() || recordBytes > maxRecordBytes)
        return std::unexpected(AdoptWireError::TooLong);
    switch (header.kind) {
    case AdoptWireMessage::Prepare:
        if (header.bodyBytes > prepareMetadataBytes)
            return header;
        break;
    case AdoptWireMessage::Commit:
    case AdoptWireMessage::Abort:
    case AdoptWireMessage::Status:
        if (header.bodyBytes == commandBodyBytes)
            return header;
        break;
    case AdoptWireMessage::Outcome:
        if (header.bodyBytes == adoptWireOutcomeBytes - adoptWireFrameHeaderBytes)
            return header;
        break;
    case AdoptWireMessage::Refused:
        if (header.bodyBytes == adoptWireRefusedBytes - adoptWireFrameHeaderBytes)
            return header;
        break;
    }
    return std::unexpected(AdoptWireError::Malformed);
}

std::expected<EncodedAdoptWirePrepare, AdoptWireError> encodeAdoptWirePrepare(const AdoptWirePrepare& prepare, std::size_t maxRecordBytes)
{
    if (const auto valid = validatePrepare(prepare, maxRecordBytes); !valid)
        return std::unexpected(valid.error());
    EncodedAdoptWirePrepare encoded;
    auto bytes = std::span { encoded.header };
    writeFrameHeader(bytes.first<adoptWireFrameHeaderBytes>(), AdoptWireMessage::Prepare,
        static_cast<std::uint32_t>(prepareMetadataBytes + prepare.terminalBody.size()));
    writeRequest(bytes.subspan<adoptWireFrameHeaderBytes, commandBodyBytes>(), prepare.correlation, prepare.preparation.operation);
    LocalControlDetail::write64(bytes.subspan<adoptWireCommandBytes, 8>(), prepare.preparation.bodyBytes);
    std::ranges::copy(prepare.preparation.bodySHA256, bytes.last<32>().begin());
    encoded.terminalBody = prepare.terminalBody;
    return encoded;
}

std::expected<AdoptWirePrepare, AdoptWireError> decodeAdoptWirePrepare(std::span<const std::uint8_t> bytes, std::size_t maxRecordBytes)
{
    const auto body = recordBody(bytes, AdoptWireMessage::Prepare, maxRecordBytes);
    if (!body)
        return std::unexpected(body.error());
    Sync::WireReader reader(*body);
    AdoptWirePrepare prepare;
    prepare.correlation = reader.u64();
    prepare.preparation.operation = readOperation(reader);
    prepare.preparation.bodyBytes = reader.u64();
    const auto digest = reader.take(prepare.preparation.bodySHA256.size());
    if (!reader.ok() || prepare.preparation.bodyBytes > std::numeric_limits<std::size_t>::max()
        || prepare.preparation.bodyBytes != reader.remaining())
        return std::unexpected(AdoptWireError::Malformed);
    std::ranges::copy(digest, prepare.preparation.bodySHA256.begin());
    prepare.terminalBody = reader.take(static_cast<std::size_t>(prepare.preparation.bodyBytes));
    if (!reader.ok() || !reader.isAtEnd())
        return std::unexpected(AdoptWireError::Malformed);
    if (const auto valid = validatePrepare(prepare, maxRecordBytes); !valid)
        return std::unexpected(valid.error());
    return prepare;
}

std::expected<std::array<std::uint8_t, adoptWireCommandBytes>, AdoptWireError> encodeAdoptWireCommand(const AdoptWireCommand& command)
{
    if (!isCommand(command.kind))
        return std::unexpected(AdoptWireError::Malformed);
    if (!command.correlation || !isValid(command.operation))
        return std::unexpected(AdoptWireError::InvalidIdentity);
    std::array<std::uint8_t, adoptWireCommandBytes> bytes {};
    writeFrameHeader(std::span { bytes }.first<adoptWireFrameHeaderBytes>(), command.kind, static_cast<std::uint32_t>(commandBodyBytes));
    writeRequest(std::span { bytes }.subspan<adoptWireFrameHeaderBytes, commandBodyBytes>(), command.correlation, command.operation);
    return bytes;
}

std::expected<AdoptWireCommand, AdoptWireError> decodeAdoptWireCommand(std::span<const std::uint8_t> bytes)
{
    if (bytes.size() != adoptWireCommandBytes)
        return std::unexpected(AdoptWireError::Malformed);
    const auto kind = static_cast<AdoptWireMessage>(bytes.front());
    if (!isCommand(kind))
        return std::unexpected(AdoptWireError::Malformed);
    const auto body = recordBody(bytes, kind, adoptWireCommandBytes);
    if (!body)
        return std::unexpected(body.error());
    Sync::WireReader reader(*body);
    AdoptWireCommand command { .kind = kind, .correlation = reader.u64(), .operation = readOperation(reader) };
    if (!reader.ok() || !reader.isAtEnd())
        return std::unexpected(AdoptWireError::Malformed);
    if (!command.correlation || !isValid(command.operation))
        return std::unexpected(AdoptWireError::InvalidIdentity);
    return command;
}

std::expected<std::array<std::uint8_t, adoptWireOutcomeBytes>, AdoptWireError> encodeAdoptWireOutcome(const AdoptWireOutcome& outcome)
{
    if (const auto valid = validateOutcome(outcome); !valid)
        return std::unexpected(valid.error());
    std::array<std::uint8_t, adoptWireOutcomeBytes> bytes {};
    auto body = std::span { bytes }.subspan<adoptWireFrameHeaderBytes>();
    writeFrameHeader(std::span { bytes }.first<adoptWireFrameHeaderBytes>(), AdoptWireMessage::Outcome, static_cast<std::uint32_t>(body.size()));
    writeRequest(body.first<commandBodyBytes>(), outcome.correlation, outcome.operation);
    body[commandBodyBytes] = std::to_underlying(outcome.disposition);
    write32(body.subspan<commandBodyBytes + 1, 4>(), outcome.target.sessionID);
    LocalControlDetail::write64(body.subspan<commandBodyBytes + 5, 8>(), outcome.target.paneID.low);
    LocalControlDetail::write64(body.subspan<commandBodyBytes + 13, 8>(), outcome.target.paneID.high);
    body.back() = outcome.rejection ? std::to_underlying(*outcome.rejection) : 0;
    return bytes;
}

std::expected<AdoptWireOutcome, AdoptWireError> decodeAdoptWireOutcome(std::span<const std::uint8_t> bytes)
{
    const auto body = recordBody(bytes, AdoptWireMessage::Outcome, adoptWireOutcomeBytes);
    if (!body)
        return std::unexpected(body.error());
    Sync::WireReader reader(*body);
    AdoptWireOutcome outcome { .correlation = reader.u64(), .operation = readOperation(reader), .disposition = static_cast<AdoptWireDisposition>(reader.u8()), .target = { .sessionID = reader.u32(), .paneID = { .low = reader.u64(), .high = reader.u64() } } };
    const auto rejection = reader.u8();
    if (rejection)
        outcome.rejection = static_cast<AdoptWireRejection>(rejection);
    if (!reader.ok() || !reader.isAtEnd())
        return std::unexpected(AdoptWireError::Malformed);
    if (const auto valid = validateOutcome(outcome); !valid)
        return std::unexpected(valid.error());
    return outcome;
}

std::expected<std::array<std::uint8_t, adoptWireRefusedBytes>, AdoptWireError> encodeAdoptWireRefused(const AdoptWireRefused& refused)
{
    if (!refused.correlation || !isValid(refused.operation))
        return std::unexpected(AdoptWireError::InvalidIdentity);
    if (!isValid(refused.reason))
        return std::unexpected(AdoptWireError::InvalidPayload);
    std::array<std::uint8_t, adoptWireRefusedBytes> bytes {};
    writeFrameHeader(std::span { bytes }.first<adoptWireFrameHeaderBytes>(), AdoptWireMessage::Refused,
        static_cast<std::uint32_t>(bytes.size() - adoptWireFrameHeaderBytes));
    writeRequest(std::span { bytes }.subspan<adoptWireFrameHeaderBytes, commandBodyBytes>(), refused.correlation, refused.operation);
    bytes.back() = std::to_underlying(refused.reason);
    return bytes;
}

std::expected<AdoptWireRefused, AdoptWireError> decodeAdoptWireRefused(std::span<const std::uint8_t> bytes)
{
    const auto body = recordBody(bytes, AdoptWireMessage::Refused, adoptWireRefusedBytes);
    if (!body)
        return std::unexpected(body.error());
    Sync::WireReader reader(*body);
    AdoptWireRefused refused { .correlation = reader.u64(), .operation = readOperation(reader), .reason = static_cast<AdoptWireRefusal>(reader.u8()) };
    if (!reader.ok() || !reader.isAtEnd())
        return std::unexpected(AdoptWireError::Malformed);
    if (!refused.correlation || !isValid(refused.operation))
        return std::unexpected(AdoptWireError::InvalidIdentity);
    if (!isValid(refused.reason))
        return std::unexpected(AdoptWireError::InvalidPayload);
    return refused;
}

} // namespace Ubiquitty::Protocol::Local
