// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

//
// UTF8Repair.h — text that has to be UTF-8, made so.
//
// A title, a directory, a label and a notification are bytes a program or a
// person chose, and travel as bytes. Something that hands them to a reader
// which takes text (a row a frontend draws, a JSON string) cannot hand it a
// byte sequence no UTF-8 decoder accepts: one stray byte makes a whole
// document undecodable. This is the one repair: each maximal run that is not
// part of a well-formed sequence becomes U+FFFD, as the Unicode Standard's
// recommended practice has it, and everything well formed is kept as it is.
//
// Well formed is the standard's definition: shortest form, no surrogate,
// nothing past U+10FFFF.
//

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace Ubiquitty::Unicode {

namespace UTF8RepairDetail {

/// How many bytes at the head of `text` are one well-formed sequence, or how
/// many an ill-formed one's maximal subpart takes.
struct Sequence {
    /// The bytes consumed; at least one for text that is not empty.
    std::size_t length = 0;
    /// Whether those bytes are a scalar value's encoding.
    bool isWellFormed = false; // bool-ok: names the one condition it reports
};

/// Reads one sequence from the head of a text.
///
/// - Parameter text: The bytes; not empty.
/// - Returns: The sequence, well formed or not.
[[nodiscard]] constexpr Sequence sequenceAt(std::string_view text)
{
    const auto byteAt = [&](std::size_t index) { return static_cast<std::uint8_t>(text[index]); };
    const std::uint8_t lead = byteAt(0);
    if (lead < 0x80)
        return { 1, true };
    std::size_t length = 0;
    // The range of the second byte, which is where shortest form, the
    // surrogates and the upper bound are each decided.
    std::uint8_t secondLow = 0x80;
    std::uint8_t secondHigh = 0xBF;
    if (lead >= 0xC2 && lead <= 0xDF) {
        length = 2;
    } else if (lead >= 0xE0 && lead <= 0xEF) {
        length = 3;
        secondLow = lead == 0xE0 ? 0xA0 : 0x80;
        secondHigh = lead == 0xED ? 0x9F : 0xBF;
    } else if (lead >= 0xF0 && lead <= 0xF4) {
        length = 4;
        secondLow = lead == 0xF0 ? 0x90 : 0x80;
        secondHigh = lead == 0xF4 ? 0x8F : 0xBF;
    } else {
        return { 1, false };
    }
    for (std::size_t index = 1; index < length; ++index) {
        const std::uint8_t low = index == 1 ? secondLow : 0x80;
        const std::uint8_t high = index == 1 ? secondHigh : 0xBF;
        if (index >= text.size() || byteAt(index) < low || byteAt(index) > high)
            return { index, false };
    }
    return { length, true };
}

} // namespace UTF8RepairDetail

/// How much of a text is well-formed UTF-8 from its start.
///
/// - Parameter text: The bytes.
/// - Returns: The length of the longest well-formed prefix: the text's own
///   length when all of it is.
[[nodiscard]] constexpr std::size_t wellFormedUTF8PrefixLength(std::string_view text)
{
    std::size_t length = 0;
    while (length < text.size()) {
        const UTF8RepairDetail::Sequence sequence = UTF8RepairDetail::sequenceAt(text.substr(length));
        if (!sequence.isWellFormed)
            break;
        length += sequence.length;
    }
    return length;
}

/// Makes a text well-formed UTF-8.
///
/// Every maximal subpart of an ill-formed sequence is replaced by U+FFFD;
/// well-formed text is left as it is and nothing is allocated for it.
///
/// - Parameter text: The text, repaired in place.
/// - Returns: Whether anything was replaced.
inline bool repairUTF8(std::string& text)
{
    using UTF8RepairDetail::Sequence;
    using UTF8RepairDetail::sequenceAt;
    const std::string_view view = text;
    const std::size_t firstFault = wellFormedUTF8PrefixLength(view);
    if (firstFault == view.size())
        return false;
    std::string repaired(view.substr(0, firstFault));
    for (std::size_t index = firstFault; index < view.size();) {
        const Sequence sequence = sequenceAt(view.substr(index));
        if (sequence.isWellFormed)
            repaired.append(view.substr(index, sequence.length));
        else
            repaired.append("\xEF\xBF\xBD");
        index += sequence.length;
    }
    text = std::move(repaired);
    return true;
}

} // namespace Ubiquitty::Unicode
