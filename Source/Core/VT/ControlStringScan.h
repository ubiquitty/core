// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <cstring>

namespace Ubiquitty::VT {

/// Whether `b` ends a control string.
///
/// The set is exact: BEL, CAN / SUB and ESC always, and the C1 string terminator only where C1
/// controls are recognized — under UTF-8 input 0x9C is a continuation byte and not a control.
///
/// - Parameters:
///   - b: The byte to classify.
///   - doesC1Terminate: Whether 0x9C ends a string in the current input encoding.
/// - Returns: Whether the byte ends a control string.
[[nodiscard]] inline bool isControlStringTerminator(std::uint8_t b, bool doesC1Terminate)
{
    return b == 0x07 || b == 0x1B || b == 0x18 || b == 0x1A || (b == 0x9C && doesC1Terminate);
}

/// Whether an eight-byte word could hold a control-string terminator.
///
/// Every terminator is below 0x20 except the C1 one, and a control string's payload is printable:
/// base64 is alphanumeric, sixel is 0x3F..0x7E plus a handful of punctuation. So this answers no for
/// eight payload bytes at a time, and the caller reads a word that answers yes one byte at a time.
///
/// Conservative in one direction only: a word this rejects holds no terminator. A word it accepts
/// may hold none — a stray carriage return inside an OSC — which costs that word's bytes a
/// per-byte read and nothing beyond it.
///
/// - Parameters:
///   - word: Eight payload bytes, in memory order.
///   - doesC1Terminate: Whether 0x9C ends a string in the current input encoding.
/// - Returns: Whether the word may hold a terminator.
[[nodiscard]] inline bool mayHoldControlStringTerminator(std::uint64_t word, bool doesC1Terminate)
{
    const std::uint64_t ones = 0x0101010101010101ULL;
    const std::uint64_t highBits = ones * 0x80u;
    // Any byte below 0x20, which is every terminator but the C1 one.
    if ((word - ones * 0x20u) & ~word & highBits)
        return true;
    if (!doesC1Terminate)
        return false;
    const std::uint64_t againstC1 = word ^ (ones * 0x9Cu);
    return (againstC1 - ones) & ~againstC1 & highBits;
}

/// Finds where a control string's payload ends.
///
/// The payloads are the largest byte runs the parser sees — a sixel frame, a kitty base64 image, an
/// OSC 1337 file — and the caller copies the run whole. A word that could hold a terminator is read
/// byte by byte; every other word is skipped whole.
///
/// - Parameters:
///   - buffer: The input bytes.
///   - start: Where the payload begins.
///   - length: How many bytes `buffer` holds.
///   - doesC1Terminate: Whether 0x9C ends a string in the current input encoding.
/// - Returns: The index of the first terminator at or after `start`, or `length` when there is none.
[[nodiscard]] inline int controlStringTerminatorIndex(const char* buffer, int start, int length, bool doesC1Terminate)
{
    int i = start;
    while (i + 8 <= length) {
        std::uint64_t word;
        std::memcpy(&word, buffer + i, 8);
        if (!mayHoldControlStringTerminator(word, doesC1Terminate)) {
            i += 8;
            continue;
        }
        for (int offset = 0; offset < 8; ++offset) {
            if (isControlStringTerminator(static_cast<std::uint8_t>(buffer[i + offset]), doesC1Terminate))
                return i + offset;
        }
        i += 8;
    }
    while (i < length) {
        if (isControlStringTerminator(static_cast<std::uint8_t>(buffer[i]), doesC1Terminate))
            break;
        ++i;
    }
    return i;
}

} // namespace Ubiquitty::VT
