// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <bit>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <span>

#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
#include <immintrin.h>
#endif

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

#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
/// Scans complete AVX2 groups for a control-string terminator.
///
/// The caller checks AVX2 support and scans the remaining tail. Every vector
/// load stays within `bytes`, including when a terminator lies at its end.
///
/// - Parameter bytes: The remaining control-string input.
/// - Returns: The first terminator's offset, or the end of the complete vector groups.
template<bool DoesC1Terminate>
[[gnu::target("avx2")]] [[nodiscard]] inline std::size_t controlStringPayloadPrefixAVX2(std::span<const char> bytes)
{
    std::size_t offset = 0;
    while (bytes.size() - offset >= 32) {
        const auto word = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(bytes.data() + offset));
        auto matches = _mm256_or_si256(_mm256_cmpeq_epi8(word, _mm256_set1_epi8(0x07)),
            _mm256_cmpeq_epi8(word, _mm256_set1_epi8(0x1B)));
        matches = _mm256_or_si256(matches, _mm256_cmpeq_epi8(word, _mm256_set1_epi8(0x18)));
        matches = _mm256_or_si256(matches, _mm256_cmpeq_epi8(word, _mm256_set1_epi8(0x1A)));
        if constexpr (DoesC1Terminate)
            matches = _mm256_or_si256(matches, _mm256_cmpeq_epi8(word, _mm256_set1_epi8(static_cast<char>(0x9C))));
        const auto mask = static_cast<std::uint32_t>(_mm256_movemask_epi8(matches));
        if (mask)
            return offset + std::countr_zero(mask);
        offset += 32;
    }
    return offset;
}
#endif

/// Finds where a control string's payload ends.
///
/// The payloads are the largest byte runs the parser sees — a sixel frame, a kitty base64 image, an
/// OSC 1337 file — and the caller copies the run whole. Long x86 runs use AVX2
/// after a runtime feature check; other inputs and the remaining tail use the word scan.
///
/// - Parameters:
///   - buffer: The input bytes.
///   - start: Where the payload begins.
///   - length: How many bytes `buffer` holds.
///   - doesC1Terminate: Whether 0x9C ends a string in the current input encoding.
/// - Returns: The index of the first terminator at or after `start`, or `length` when there is none.
[[nodiscard]] inline int controlStringTerminatorIndex(const char* buffer, int start, int length, bool doesC1Terminate)
{
    assert(start >= 0 && length >= start);
    int i = start;
#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
    // Keep short strings on the inline word path; a long payload amortizes
    // the feature check and the call across instruction-set boundaries.
    if (length - i >= 256 && __builtin_cpu_supports("avx2")) {
        const std::span<const char> bytes { buffer + i, static_cast<std::size_t>(length - i) };
        const std::size_t prefix = doesC1Terminate ? controlStringPayloadPrefixAVX2<true>(bytes) : controlStringPayloadPrefixAVX2<false>(bytes);
        i += static_cast<int>(prefix);
    }
#endif
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
