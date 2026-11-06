// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/Protocol/Sync/Messages.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace Ubiquitty::Protocol::Sync {

/// The size of an own-consent status `Notify` body: the kind's one-byte
/// varint, then `request:u32 session:u32 phase:u8`. A client awaiting
/// consent refuses a larger `Notify` before reading or copying it.
inline constexpr std::size_t maxConsentStatusBytes = 1 + 4 + 4 + 1;

/// The status of an attachment's own exact consent question.
enum class ConsentStatusPhase : std::uint8_t {
    Pending = 0,
    Approved = 1,
    Denied = 2,
    Expired = 3,
};

/// A bounded own-status value, carrying no arbitrary notification or authority.
struct ConsentStatus {
    ConsentBinding question;
    ConsentStatusPhase phase = ConsentStatusPhase::Pending;

    friend bool operator==(const ConsentStatus&, const ConsentStatus&) = default;
};

/// Encodes the one own-status notification an awaiting attachment can receive:
/// a `ConsentStatus` `Notify` body.
/// - Parameter status: Its exact nonzero question, session and known phase.
/// - Returns: The whole body, or nothing for an invalid value.
[[nodiscard]] std::optional<std::vector<std::uint8_t>> consentStatusNotify(const ConsentStatus& status);

/// Reads a `ConsentStatus` record: `request:u32 session:u32 phase:u8`, both
/// identifiers nonzero, a known phase and nothing after it.
/// - Parameter record: A `ConsentStatus` `Notify`'s record.
/// - Returns: The typed status, or nothing for a malformed record.
[[nodiscard]] std::optional<ConsentStatus> decodeConsentStatus(std::span<const std::uint8_t> record);

} // namespace Ubiquitty::Protocol::Sync
