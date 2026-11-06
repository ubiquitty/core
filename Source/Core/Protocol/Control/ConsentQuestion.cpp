// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Protocol/Sync/Consent.h"

#include "Core/Protocol/Sync/Envelope.h"
#include "Core/Protocol/Sync/Wire.h"
#include "PAL/Capability.h"

#include <utility>

namespace Ubiquitty::Protocol::Sync {

namespace {

constexpr std::size_t maxAgentLabelBytes = 256;

bool validText(std::string_view text, std::size_t maximum)
{
    return text.size() <= maximum && text.find('\0') == std::string_view::npos
        && isValidUTF8({ reinterpret_cast<const std::uint8_t*>(text.data()), text.size() });
}

bool valid(const ConsentQuestion& question)
{
    if (!question.requestID || !question.sessionID || question.action > std::to_underlying(ConsentKind::Capability)
        || (!question.peerProcessID && !question.peerProcess.empty())
        || !validText(question.peerProcess, maxConsentIdentityBytes) || !validText(question.agentLabel, maxAgentLabelBytes))
        return false;
    if (question.action == std::to_underlying(ConsentKind::Capability))
        return question.capabilityCode <= std::to_underlying(Capability::None) && question.capabilityOrigin <= std::to_underlying(CapabilityOrigin::Remote);
    return !question.capabilityCode && !question.capabilityOrigin && !question.isRecipientsOwnRequest;
}

} // namespace

std::optional<std::vector<std::uint8_t>> consentQuestionNotify(const ConsentQuestion& question)
{
    if (!valid(question))
        return std::nullopt;
    // Measured before any byte is copied.
    const std::size_t bytes = varintBytes(std::to_underlying(NotifyKind::ConsentQuestion)) + 1 + 4 + 4 + 4
        + utf8Bytes(question.peerProcess) + utf8Bytes(question.agentLabel)
        + (question.action == std::to_underlying(ConsentKind::Capability) ? 2 + 1 + 1 : 0);
    if (bytes > maxFrameBodyBytes)
        return std::nullopt;
    std::vector<std::uint8_t> body;
    body.reserve(bytes);
    WireWriter writer(body);
    writer.varint(std::to_underlying(NotifyKind::ConsentQuestion));
    writer.u8(question.action);
    writer.u32(question.requestID);
    writer.u32(question.sessionID);
    writer.u32(question.peerProcessID);
    writer.utf8(question.peerProcess);
    writer.utf8(question.agentLabel);
    if (question.action == std::to_underlying(ConsentKind::Capability)) {
        writer.u16(question.capabilityCode);
        writer.u8(question.capabilityOrigin);
        writer.u8(question.isRecipientsOwnRequest ? 1 : 0);
    }
    return body;
}

std::optional<ConsentQuestion> decodeConsentQuestion(std::span<const std::uint8_t> record)
{
    WireReader reader(record);
    ConsentQuestion question;
    question.action = reader.u8();
    question.requestID = reader.u32();
    question.sessionID = reader.u32();
    question.peerProcessID = reader.u32();
    const std::string_view process = reader.utf8(maxConsentIdentityBytes);
    const std::string_view label = reader.utf8(maxAgentLabelBytes);
    if (question.action == std::to_underlying(ConsentKind::Capability)) {
        question.capabilityCode = reader.u16();
        question.capabilityOrigin = reader.u8();
        const std::uint8_t own = reader.u8();
        if (own > 1)
            reader.fail();
        question.isRecipientsOwnRequest = own == 1;
    }
    if (!reader.ok() || !reader.isAtEnd())
        return std::nullopt;
    question.peerProcess = process;
    question.agentLabel = label;
    if (!valid(question))
        return std::nullopt;
    return question;
}

std::optional<std::vector<std::uint8_t>> consentResolvedNotify(const ConsentResolution& resolution)
{
    if (!resolution.requestID || !resolution.sessionID || resolution.kind > ConsentKind::Structure)
        return std::nullopt;
    std::vector<std::uint8_t> body;
    body.reserve(maxConsentResolutionBytes);
    WireWriter writer(body);
    writer.varint(std::to_underlying(NotifyKind::ConsentResolved));
    writer.u8(std::to_underlying(resolution.kind));
    writer.u32(resolution.requestID);
    writer.u32(resolution.sessionID);
    return body;
}

std::optional<ConsentResolution> decodeConsentResolved(std::span<const std::uint8_t> record)
{
    WireReader reader(record);
    const std::uint8_t kind = reader.u8();
    ConsentResolution resolution;
    resolution.requestID = reader.u32();
    resolution.sessionID = reader.u32();
    if (!reader.ok() || !reader.isAtEnd() || kind > std::to_underlying(ConsentKind::Structure)
        || !resolution.requestID || !resolution.sessionID)
        return std::nullopt;
    resolution.kind = static_cast<ConsentKind>(kind);
    return resolution;
}

} // namespace Ubiquitty::Protocol::Sync
