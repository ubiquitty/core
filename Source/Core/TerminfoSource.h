// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Source/Core/TerminfoSource.h — the terminfo SOURCE format, decoded.
//
// A terminfo entry is written in the spelling terminfo(5) defines and answered
// in the bytes that spelling denotes: `\E` is an escape, `^G` is a bell, `\007`
// is the same bell written in octal, and a number may be decimal, octal or hex
// the way C spells them. Turning one into the other is a pure function of the
// text, so it lives apart from the entry it happens to be applied to and from
// the lookup that applies it -- a decoder tied to one entry can only be tested
// through whichever spellings that entry happens to use, and ours uses a
// fraction of the format.
//
// Header-only because these are two small pure functions over a string_view,
// and because the arms that carry no Terminfo TU still need them where they
// parse an entry of their own.

#pragma once

#include <string>
#include <string_view>

namespace Ubiquitty::Terminfo::Source {

/// Decodes one terminfo string value into the bytes the compiled entry holds.
///
/// `\E` and `\e` are ESC, `^X` is a control, `\ddd` is octal, and `\n \r \t
/// \b \f \s \l \0 \\ \, \: \^` are the characters they name. Anything else
/// after a backslash is itself. `%` code and `$<>` padding pass through.
///
/// - Parameter raw: One capability's value, as the source spells it.
/// - Returns: The bytes the compiled entry holds for it.
inline std::string decodeValue(std::string_view raw)
{
    std::string out;
    out.reserve(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        const char c = raw[i];
        if (c == '^' && i + 1 < raw.size()) {
            const char next = raw[++i];
            out.push_back(next == '?' ? '\x7f' : static_cast<char>(next & 0x1f));
            continue;
        }
        if (c != '\\' || i + 1 >= raw.size()) {
            out.push_back(c);
            continue;
        }
        const char next = raw[++i];
        switch (next) {
        case 'E':
        case 'e':
            out.push_back('\033');
            break;
        case 'n':
        case 'l':
            out.push_back('\n');
            break;
        case 'r':
            out.push_back('\r');
            break;
        case 't':
            out.push_back('\t');
            break;
        case 'b':
            out.push_back('\b');
            break;
        case 'f':
            out.push_back('\f');
            break;
        case 's':
            out.push_back(' ');
            break;
        case '0':
        case '1':
        case '2':
        case '3':
        case '4':
        case '5':
        case '6':
        case '7': {
            int value = next - '0';
            int digits = 1;
            while (digits < 3 && i + 1 < raw.size() && raw[i + 1] >= '0' && raw[i + 1] <= '7') {
                value = value * 8 + (raw[++i] - '0');
                ++digits;
            }
            // terminfo spells NUL as \0 and the compiled entry stores \200,
            // which is the byte tputs writes.
            out.push_back(static_cast<char>(value == 0 ? 0200 : value));
            break;
        }
        default:
            out.push_back(next);
            break;
        }
    }
    return out;
}

/// The decimal digits of a terminfo numeric, which the source may spell in
/// hex or octal the way C does.
///
/// - Parameter raw: One numeric capability's value, in any of the three bases.
/// - Returns: The same number written in decimal, or the part of it that was
///   well formed when a character stops the number early.
inline std::string decodeNumber(std::string_view raw)
{
    int base = 10;
    if (raw.size() > 2 && raw[0] == '0' && (raw[1] == 'x' || raw[1] == 'X')) {
        base = 16;
        raw.remove_prefix(2);
    } else if (raw.size() > 1 && raw[0] == '0') {
        base = 8;
        raw.remove_prefix(1);
    }
    long value = 0;
    for (const char c : raw) {
        int digit;
        if (c >= '0' && c <= '9')
            digit = c - '0';
        else if (c >= 'a' && c <= 'f')
            digit = 10 + (c - 'a');
        else if (c >= 'A' && c <= 'F')
            digit = 10 + (c - 'A');
        else
            break;
        // A digit the base does not have ends the number, the way strtol ends
        // it: `09` is octal followed by a 9, not the number nine. Without this
        // the loop reads every decimal digit whatever the base, so a malformed
        // entry answers a plausible number instead of stopping at the part
        // that was well formed.
        if (digit >= base)
            break;
        value = value * base + digit;
    }
    return std::to_string(value);
}

} // namespace Ubiquitty::Terminfo::Source
