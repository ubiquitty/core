// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/Preferences/Preferences.h"

#include <expected>
#include <span>

namespace Ubiquitty::Preferences {

/// Independent receiver bounds for the private per-session configuration.
struct HandoverValuesLimits {
    std::size_t maxSectionBytes = 0;
    std::size_t maxStringBytes = 0;
    std::size_t maxCollectionEntries = 0;
    /// Sum of Values, vector element storage, and string bytes, not allocator overhead.
    std::size_t maxDecodedBytes = 0;
};

/// Why an exact configuration cannot enter an unpublished successor.
enum class HandoverValuesError : std::uint8_t {
    NewerVersion,
    Malformed,
    OverCapacity,
    DigestMismatch,
};

/// Encodes every stored field without resolving defaults or reparsing TOML.
///
/// This private record includes security settings and is not a shareable profile.
/// The caller authenticates the local successor and freezes the source throughout
/// encoding. Optional presence, list order, binary strings, and floating-point
/// bits are preserved. Bounded STL owners retain their normal allocation behavior.
/// - Parameters:
///   - values: The session's exact captured configuration.
///   - limits: Independent section, string, collection, and decoded-data bounds.
/// - Returns: One versioned section, or refusal without changing the source.
[[nodiscard]] std::expected<std::vector<std::uint8_t>, HandoverValuesError> encodeHandoverValues(
    const Values& values, const HandoverValuesLimits& limits);

/// Validates the complete record before allocating any variable-size owners.
///
/// The digest detects corruption, not authority. No script, default resolver,
/// platform action, or preferences writer runs. The returned value stays private
/// until its containing session is admitted. A record of any version other
/// than the one this build writes is refused, never read in another layout.
/// - Parameters:
///   - bytes: Exactly one private configuration section.
///   - limits: The receiver's resource bounds, never supplied by the record.
/// - Returns: All stored values, or a refusal with no partial destination.
[[nodiscard]] std::expected<Values, HandoverValuesError> decodeHandoverValues(
    std::span<const std::uint8_t> bytes, const HandoverValuesLimits& limits);

} // namespace Ubiquitty::Preferences
