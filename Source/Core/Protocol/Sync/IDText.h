// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/Protocol/Sync/Messages.h"

#include <array>
#include <charconv>
#include <optional>
#include <string>
#include <string_view>

namespace Ubiquitty::Protocol::Sync {

/// Writes a minted identity in the one public JSON spelling, high half first.
/// - Parameter identity: A nonzero attachment, instance or pane identity.
/// - Returns: Exactly 32 lowercase hexadecimal bytes, or empty for zero.
[[nodiscard]] inline std::string formatID128(ID128 identity)
{
    if (identity == ID128 {})
        return {};
    std::array<char, 32> digits {};
    constexpr std::string_view alphabet = "0123456789abcdef";
    for (std::size_t index = 0; index < 16; ++index) {
        digits[index] = alphabet[(identity.high >> (60 - 4 * index)) & 0xf];
        digits[16 + index] = alphabet[(identity.low >> (60 - 4 * index)) & 0xf];
    }
    return { digits.data(), digits.size() };
}

/// Reads only that complete canonical spelling, never a label or a partial ID.
/// - Parameter text: Exactly 32 lowercase hexadecimal digits, high half first.
/// - Returns: The nonzero identity, or nothing for any other representation.
[[nodiscard]] inline std::optional<ID128> parseID128(std::string_view text)
{
    if (text.size() != 32)
        return std::nullopt;
    for (char digit : text) {
        if (!((digit >= '0' && digit <= '9') || (digit >= 'a' && digit <= 'f')))
            return std::nullopt;
    }
    ID128 identity;
    const char* middle = text.data() + 16;
    const auto high = std::from_chars(text.data(), middle, identity.high, 16);
    const auto low = std::from_chars(middle, middle + 16, identity.low, 16);
    if (high.ec != std::errc {} || high.ptr != middle || low.ec != std::errc {} || low.ptr != middle + 16 || identity == ID128 {})
        return std::nullopt;
    return identity;
}

} // namespace Ubiquitty::Protocol::Sync
