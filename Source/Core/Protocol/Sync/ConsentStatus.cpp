// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Protocol/Sync/ConsentStatus.h"

#include "Core/Protocol/Sync/Wire.h"

#include <utility>

namespace Ubiquitty::Protocol::Sync {

std::optional<std::vector<std::uint8_t>> consentStatusNotify(const ConsentStatus& status)
{
    if (!status.question.requestID || !status.question.sessionID || status.phase > ConsentStatusPhase::Expired)
        return std::nullopt;
    std::vector<std::uint8_t> body;
    body.reserve(maxConsentStatusBytes);
    WireWriter writer(body);
    writer.varint(std::to_underlying(NotifyKind::ConsentStatus));
    writer.u32(status.question.requestID);
    writer.u32(status.question.sessionID);
    writer.u8(std::to_underlying(status.phase));
    return body;
}

std::optional<ConsentStatus> decodeConsentStatus(std::span<const std::uint8_t> record)
{
    WireReader reader(record);
    ConsentStatus status;
    status.question.requestID = reader.u32();
    status.question.sessionID = reader.u32();
    const std::uint8_t phase = reader.u8();
    if (!reader.ok() || !reader.isAtEnd() || !status.question.requestID || !status.question.sessionID
        || phase > std::to_underlying(ConsentStatusPhase::Expired))
        return std::nullopt;
    status.phase = static_cast<ConsentStatusPhase>(phase);
    return status;
}

} // namespace Ubiquitty::Protocol::Sync
