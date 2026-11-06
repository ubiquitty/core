// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Source/Core/Diagnostics/EscapeDecode.h — decode common C-string backslash escapes
// (\n \r \t \e \a \b \f \v \0 \\ \xHH) out of a plain string so a diagnostic
// --feed value passed as a shell SINGLE-quoted literal can still carry control
// bytes. The shell's $'...' form already expands these; this is the fallback
// for callers that pass a plain 'literal'. Bytes with no recognised escape (and
// a trailing or unknown backslash) pass through verbatim, so the function is a
// no-op on input that contains no escapes.
//
// Pure portable C++ (no platform types); host-tested. Used by the macOS
// snapshot/probe diagnostic harness and any other frontend's --feed path.

#include <string>
#include <string_view>

namespace Ubiquitty::Diagnostics {

// Decode the C-string escapes in `text` into a freshly built std::string.
/// Decodes C-string backslash escapes into the bytes they name.
///
/// Recognised two-character escapes: `\n` `\r` `\t` `\e` (ESC, 0x1b) `\a`
/// `\b` `\f` `\v` `\0` (NUL) and `\\`. `\xHH` consumes one or two hex
/// digits, so `\xH` and `\xHH` are both valid.
///
/// A backslash beginning no recognised escape — including a lone trailing one,
/// and an `\x` with no hex digit after it — is emitted verbatim and decoding
/// resumes at the following byte, so a value that carries no escapes survives
/// unchanged rather than losing characters to a partial match.
///
/// - Parameter text: The raw value, typically a shell single-quoted argument.
/// - Returns: The decoded bytes, which may contain embedded NULs.
inline std::string decodeEscapes(std::string_view text)
{
    std::string out;
    out.reserve(text.size());

    const char* p = text.data();
    const char* end = p + text.size();
    for (; p < end; ++p) {
        if (*p != '\\') {
            out.push_back(*p);
            continue;
        }
        // A backslash at the very end has no escape body: emit it literally.
        if (p + 1 >= end) {
            out.push_back('\\');
            break;
        }
        const char c = *(p + 1);
        switch (c) {
        case 'n':
            out.push_back('\n');
            ++p;
            break;
        case 'r':
            out.push_back('\r');
            ++p;
            break;
        case 't':
            out.push_back('\t');
            ++p;
            break;
        case 'e':
            out.push_back('\x1b');
            ++p;
            break;
        case 'a':
            out.push_back('\a');
            ++p;
            break;
        case 'b':
            out.push_back('\b');
            ++p;
            break;
        case 'f':
            out.push_back('\f');
            ++p;
            break;
        case 'v':
            out.push_back('\v');
            ++p;
            break;
        case '0':
            out.push_back('\0');
            ++p;
            break;
        case '\\':
            out.push_back('\\');
            ++p;
            break;
        case 'x': {
            // \xH or \xHH — one or two hex digits after the 'x'.
            auto hexValue = [](char ch, int* value) -> bool {
                if (ch >= '0' && ch <= '9') {
                    *value = ch - '0';
                    return true;
                }
                if (ch >= 'a' && ch <= 'f') {
                    *value = ch - 'a' + 10;
                    return true;
                }
                if (ch >= 'A' && ch <= 'F') {
                    *value = ch - 'A' + 10;
                    return true;
                }
                return false;
            };
            const char* digits = p + 2;
            int high = 0;
            int low = 0;
            int taken = 0;
            if (digits < end && hexValue(digits[0], &high)) {
                taken = 1;
                if (digits + 1 < end && hexValue(digits[1], &low))
                    taken = 2;
            }
            if (taken == 2) {
                out.push_back(static_cast<char>((high << 4) | low));
                p += 3;
            } else if (taken == 1) {
                out.push_back(static_cast<char>(high));
                p += 2;
            } else {
                // \x with no hex digit: keep the backslash, resume at 'x'.
                out.push_back('\\');
            }
            break;
        }
        default:
            // Unknown escape: keep the backslash, resume at the next byte so the
            // escaped character itself is emitted verbatim on the next loop.
            out.push_back('\\');
            break;
        }
    }
    return out;
}

} // namespace Ubiquitty::Diagnostics
// namespace Ubiquitty::Core
