// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
// HMAC-SHA256 (RFC 2104, FIPS 198-1) over `sha256`, and the comparison a MAC
// is checked with. The MAC a peer presents is compared in time that does not
// depend on where it differs: one checked byte by byte tells a forger how much
// of a guess was right.

#include "Core/Crypto/SHA256.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace Ubiquitty::Crypto {

/// Computes HMAC-SHA256.
///
/// - Parameters:
///   - key: The key, of any length; one longer than a SHA-256 block is hashed
///     first, as the construction defines.
///   - message: The bytes authenticated.
/// - Returns: The 32-byte MAC.
[[nodiscard]] inline std::array<std::uint8_t, 32> hmacSHA256(std::span<const std::uint8_t> key, std::span<const std::uint8_t> message)
{
    constexpr std::size_t blockBytes = 64;
    std::array<std::uint8_t, blockBytes> block {};
    if (key.size() > blockBytes) {
        std::array<std::uint8_t, 32> hashed {};
        sha256(key, hashed);
        std::copy(hashed.begin(), hashed.end(), block.begin());
    } else {
        std::copy(key.begin(), key.end(), block.begin());
    }
    std::vector<std::uint8_t> inner(blockBytes + message.size());
    for (std::size_t index = 0; index < blockBytes; ++index)
        inner[index] = block[index] ^ 0x36;
    std::copy(message.begin(), message.end(), inner.begin() + blockBytes);
    std::array<std::uint8_t, blockBytes + 32> outer {};
    for (std::size_t index = 0; index < blockBytes; ++index)
        outer[index] = block[index] ^ 0x5c;
    sha256(inner, std::span<std::uint8_t, 32> { outer.data() + blockBytes, 32 });
    std::array<std::uint8_t, 32> mac {};
    sha256(outer, mac);
    return mac;
}

/// Whether two MACs are equal, in time that does not depend on where they
/// differ.
///
/// - Parameters:
///   - left: One MAC.
///   - right: The other.
/// - Returns: Whether they are equal.
[[nodiscard]] inline bool constantTimeEqual(std::span<const std::uint8_t, 32> left, std::span<const std::uint8_t, 32> right)
{
    std::uint8_t difference = 0;
    for (std::size_t index = 0; index < 32; ++index)
        difference = static_cast<std::uint8_t>(difference | (left[index] ^ right[index]));
    volatile std::uint8_t sink = difference;
    return sink == 0;
}

} // namespace Ubiquitty::Crypto
