// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
// Escaping for TOML basic strings, shared by every writer that quotes a value
// into config.toml.

#include <string>
#include <string_view>

namespace Ubiquitty::Preferences {

/// Escapes `text` for a TOML basic string. The quotes around it are the
/// caller's to write; this is the body that goes between them.
///
/// Every writer that interpolates a value between quotes needs this, because
/// the values are not the writer's: a theme name comes from a downloaded file's
/// name or from an editor field, and a key binding carries whatever a person
/// typed. Escaping only the quote leaves a raw newline able to close the string
/// and start a line the parser reads as configuration, so the escape set is the
/// full one the reader decodes rather than the two characters that look
/// dangerous.
///
/// Symmetric with `parseQuotedString` in Preferences.cpp, which decodes
/// `\b \t \n \f \r \" \\` and `\uXXXX`. A byte at or above 0x80 passes through:
/// TOML basic strings are UTF-8, and splitting a multi-byte sequence into
/// escapes would change the text.
///
/// - Parameter text: The raw value to quote.
/// - Returns: The escaped body, ready to place between two `"` characters.
[[nodiscard]] inline std::string escapeTOMLBasicString(std::string_view text)
{
    std::string escaped;
    escaped.reserve(text.size() + 8);
    for (const char rawByte : text) {
        const auto byte = static_cast<unsigned char>(rawByte);
        switch (byte) {
        case '"':
            escaped += "\\\"";
            break;
        case '\\':
            escaped += "\\\\";
            break;
        case '\b':
            escaped += "\\b";
            break;
        case '\t':
            escaped += "\\t";
            break;
        case '\n':
            escaped += "\\n";
            break;
        case '\f':
            escaped += "\\f";
            break;
        case '\r':
            escaped += "\\r";
            break;
        default:
            // The remaining C0 controls and DEL have no short escape. TOML
            // spells them \uXXXX, and leaving them raw is what would let one
            // through into the file.
            if (byte < 0x20 || byte == 0x7F) {
                // Every byte reaching here is below 0x80, so the high half of
                // the scalar is always "00".
                constexpr std::string_view hexDigits = "0123456789ABCDEF";
                escaped += "\\u00";
                escaped.push_back(hexDigits[byte >> 4]);
                escaped.push_back(hexDigits[byte & 0x0F]);
            } else {
                escaped.push_back(rawByte);
            }
            break;
        }
    }
    return escaped;
}

} // namespace Ubiquitty::Preferences
