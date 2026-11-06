// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/Protocol/Local/Control.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>

namespace Ubiquitty::Protocol::Local {

/// Bounds the same-host handoff's terminal body independently of the remote
/// carrier grammar.
inline constexpr std::size_t maxAdoptBodyBytes = std::size_t { 36 } << 20;

/// Whether the local owner admitted a descriptor-bearing adopt exchange.
enum class HandoffReadyStatus : std::uint8_t {
    Ready,
    Refused,
};

/// Pins the operation before its first descriptor-bearing record.
struct HandoffReady {
    std::uint64_t correlation = 0;
    std::uint64_t daemonInstanceID = 0;
    HandoffReadyStatus status = HandoffReadyStatus::Refused;
    bool operator==(const HandoffReady&) const = default;
};

/// Selects operation 3 before any descriptor-bearing record is sent.
[[nodiscard]] inline std::expected<std::array<std::uint8_t, localControlRequestBytes>, LocalControlError> encodeLocalHandoffRequest(std::uint64_t correlation)
{
    if (!correlation)
        return std::unexpected(LocalControlError::InvalidIdentity);
    return LocalControlDetail::encodeRequest(LocalControlOperation::SessionHandoff, correlation);
}

/// Correlates an operation-3 readiness reply with its serving daemon.
[[nodiscard]] std::array<std::uint8_t, localControlReplyBytes> encodeLocalHandoffReady(const HandoffReady&);
[[nodiscard]] std::optional<HandoffReady> decodeLocalHandoffReady(std::span<const std::uint8_t>);

} // namespace Ubiquitty::Protocol::Local
