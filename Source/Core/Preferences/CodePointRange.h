// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
// The code-point range spelling `[[font.range_map]]` uses, parsed in one place.
//
// Header-only on purpose. The reader (Preferences.cpp) is in the unit-test
// CORE_SRCS set and the writer (RangeMapWrite.cpp) is not, so a shared parser
// living in either .cpp would drag one into the other's link. Inline, both get
// it and neither gains a dependency.
//
// Three callers share it — the reader, the writer, and anything editing a
// range from a UI. Two parsers that agree are two parsers that can stop
// agreeing, and this one is the acceptance test for what
// the reader keeps: a range this refuses is an entry the reader drops.

#include <cstdint>
#include <optional>
#include <string_view>

namespace Ubiquitty::Preferences {

/// A code-point range, inclusive at both ends.
///
/// A single code point is a range of one, so every caller handles one case.
struct CodePointRange {
    std::uint32_t firstCodePoint = 0;
    std::uint32_t lastCodePoint = 0;
};

namespace Detail {

/// The value of one hex digit, or -1 when the character is not one.
[[nodiscard]] constexpr int hexDigitValue(char character)
{
    if (character >= '0' && character <= '9')
        return character - '0';
    if (character >= 'a' && character <= 'f')
        return character - 'a' + 10;
    if (character >= 'A' && character <= 'F')
        return character - 'A' + 10;
    return -1;
}

/// `text` with leading and trailing blanks removed.
[[nodiscard]] constexpr std::string_view trimmedRangeToken(std::string_view text)
{
    const auto isBlank = [](char character) {
        return character == ' ' || character == '\t' || character == '\r' || character == '\n';
    };
    while (!text.empty() && isBlank(text.front()))
        text.remove_prefix(1);
    while (!text.empty() && isBlank(text.back()))
        text.remove_suffix(1);
    return text;
}

} // namespace Detail

/// Parses one code point: an optional `U+` prefix, then one to six hex digits.
///
/// - Parameter token: The text to read; blanks around it are allowed.
/// - Returns: The code point, or nothing when the spelling is not one or the
///   value is above the Unicode maximum.
[[nodiscard]] constexpr std::optional<std::uint32_t> parseHexCodePoint(std::string_view token)
{
    std::string_view text = Detail::trimmedRangeToken(token);
    if (text.size() >= 2 && (text[0] == 'U' || text[0] == 'u') && text[1] == '+')
        text.remove_prefix(2);
    if (text.empty() || text.size() > 6)
        return std::nullopt;
    std::uint32_t value = 0;
    for (const char character : text) {
        const int digit = Detail::hexDigitValue(character);
        if (digit < 0)
            return std::nullopt;
        value = (value << 4) | static_cast<std::uint32_t>(digit);
    }
    if (value > 0x10FFFF)
        return std::nullopt;
    return value;
}

/// Parses `first-last`, or a bare code point as a range of one.
///
/// Returned whole rather than through two out-parameters: a dashed range whose
/// second half fails to parse would otherwise leave the first assigned and the
/// second stale.
///
/// - Parameter text: The range spelling, as the file carries it.
/// - Returns: The range, or nothing when either end fails to parse or the
///   range runs backwards.
[[nodiscard]] constexpr std::optional<CodePointRange> parseCodePointRange(std::string_view text)
{
    const std::string_view::size_type dash = text.find('-');
    if (dash == std::string_view::npos) {
        if (const std::optional<std::uint32_t> only = parseHexCodePoint(text))
            return CodePointRange { *only, *only };
        return std::nullopt;
    }
    const std::optional<std::uint32_t> first = parseHexCodePoint(text.substr(0, dash));
    const std::optional<std::uint32_t> last = parseHexCodePoint(text.substr(dash + 1));
    if (!first || !last || *first > *last)
        return std::nullopt;
    return CodePointRange { *first, *last };
}

} // namespace Ubiquitty::Preferences
