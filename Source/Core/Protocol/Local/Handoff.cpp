// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Protocol/Local/Handoff.h"

#include <algorithm>
#include <utility>

namespace Ubiquitty::Protocol::Local {

std::array<std::uint8_t, localControlReplyBytes> encodeLocalHandoffReady(const HandoffReady& reply)
{
    std::array<std::uint8_t, localControlReplyBytes> bytes {};
    std::ranges::copy(localControlPreamble, bytes.begin());
    bytes[localControlPreamble.size()] = localHandoffReplyOperation;
    bytes[localControlPreamble.size() + 1] = std::to_underlying(reply.status);
    LocalControlDetail::write64(std::span { bytes }.subspan<localControlPreamble.size() + 2, sizeof(std::uint64_t)>(), reply.correlation);
    LocalControlDetail::write64(std::span { bytes }.last<sizeof(std::uint64_t)>(), reply.daemonInstanceID);
    return bytes;
}

std::optional<HandoffReady> decodeLocalHandoffReady(std::span<const std::uint8_t> bytes)
{
    if (bytes.size() != localControlReplyBytes || !std::ranges::equal(bytes.first(localControlPreamble.size()), localControlPreamble)
        || bytes[localControlPreamble.size()] != localHandoffReplyOperation
        || bytes[localControlPreamble.size() + 1] > std::to_underlying(HandoffReadyStatus::Refused))
        return std::nullopt;
    HandoffReady reply {
        .correlation = LocalControlDetail::read64(bytes.subspan<localControlPreamble.size() + 2, sizeof(std::uint64_t)>()),
        .daemonInstanceID = LocalControlDetail::read64(bytes.last<sizeof(std::uint64_t)>()),
        .status = static_cast<HandoffReadyStatus>(bytes[localControlPreamble.size() + 1]),
    };
    return reply.correlation && reply.daemonInstanceID ? std::optional(reply) : std::nullopt;
}

} // namespace Ubiquitty::Protocol::Local
