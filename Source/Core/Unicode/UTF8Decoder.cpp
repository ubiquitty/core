// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Unicode/UTF8Decoder.h"
#include <span>

#include "simdutf.h"

#include <cstdint>
#include <cstring>

#if defined(__aarch64__)
#include <arm_neon.h>
#endif

namespace Ubiquitty::Unicode {

#if UBIQUITTY_ENABLE_TEST_HOOKS
// How many runs the vectorised converter has answered. A parity case comparing
// the two paths has no other way to tell that the fast one ran: every quantity
// it can compute from the input — bytes consumed, code points returned — is the
// same on both, so a build with the fast path compiled out agrees with itself
// and reads as a passing comparison.
std::uint64_t simdRunCountForTest = 0;
std::uint64_t threeByteSIMDRunCountForTest = 0;
// Every byte a run decode has consumed, on either path, so a caller can be held
// to decoding its input once.
std::uint64_t decodedBytesForTest = 0;
#endif

// Bound the converter by output positions as well as input bytes. Every
// non-continuation byte starts at most one output, even in malformed input;
// the converter validates the resulting span and retains its scalar fallback.
static inline int nonASCIISpanEnd(std::span<const char> bytes, int capacity)
{
    const char* in = bytes.data();
    const int length = static_cast<int>(bytes.size());
    int i = 0;
    int leadingBytes = 0;
    constexpr std::uint64_t highBits = 0x8080808080808080ull;
    while (i + 8 <= length) {
        std::uint64_t word;
        std::memcpy(&word, in + i, 8);
        const std::uint64_t asciiHighBits = ~word & highBits;
        if (asciiHighBits)
            break;
        const std::uint64_t leadingByteBits = (word & (word << 1) & highBits) >> 7;
        // Each byte is zero or one, so the high product byte sums all eight
        // without a carry. A generic-target popcount may require a library call.
        const int wordLeadingBytes = static_cast<int>((leadingByteBits * 0x0101010101010101ull) >> 56);
        if (wordLeadingBytes >= capacity - leadingBytes)
            break; // locate the output boundary within this word
        leadingBytes += wordLeadingBytes;
        i += 8;
    }
    while (i < length && static_cast<std::uint8_t>(in[i]) >= 0x80) {
        if (static_cast<std::uint8_t>(in[i]) >= 0xC0) {
            if (leadingBytes == capacity)
                break;
            ++leadingBytes;
        }
        ++i;
    }
    return i;
}

// Trim a trailing sequence the span boundary split, so the converter only ever
// sees whole sequences. Malformed tails are left in place — the validating
// converter rejects the span and the scalar loop takes over with an exact stop.
static inline int trimSplitTrailingSequence(const char* in, int end)
{
    int lead = end - 1;
    const int lowestPossibleLead = (end >= 4) ? end - 4 : 0;
    while (lead >= lowestPossibleLead && (static_cast<std::uint8_t>(in[lead]) & 0xC0) == 0x80)
        --lead;
    if (lead < lowestPossibleLead)
        return end;
    const std::uint8_t leadByte = static_cast<std::uint8_t>(in[lead]);
    int sequenceLength;
    if ((leadByte & 0xE0) == 0xC0)
        sequenceLength = 2;
    else if ((leadByte & 0xF0) == 0xE0)
        sequenceLength = 3;
    else if ((leadByte & 0xF8) == 0xF0)
        sequenceLength = 4;
    else
        return end;
    return (lead + sequenceLength > end) ? lead : end;
}

// Case-insensitive, separator-insensitive token compare against a lowercase,
// separator-free literal — so "FFFD", "fffd", "f-f-f-d" all match "fffd".
static bool tokenEq(std::string_view name, const char* canon)
{
    std::size_t j = 0;
    for (char ch : name) {
        if (ch == '-' || ch == '_' || ch == '.' || ch == ' ')
            continue;
        if (ch >= 'A' && ch <= 'Z')
            ch = static_cast<char>(ch - 'A' + 'a');
        if (canon[j] == '\0' || canon[j] != ch)
            return false;
        ++j;
    }
    return canon[j] == '\0';
}

std::optional<InvalidUTF8Policy> invalidUTF8PolicyFromName(std::string_view name)
{
    if (tokenEq(name, "fffd") || tokenEq(name, "replacement"))
        return InvalidUTF8Policy::FFFD;
    if (tokenEq(name, "drop") || tokenEq(name, "skip"))
        return InvalidUTF8Policy::Drop;
    if (tokenEq(name, "latin1") || tokenEq(name, "iso88591"))
        return InvalidUTF8Policy::Latin1;
    return std::nullopt;
}

const char* invalidUTF8PolicyToName(InvalidUTF8Policy policy)
{
    switch (policy) {
    case InvalidUTF8Policy::Drop:
        return "drop";
    case InvalidUTF8Policy::Latin1:
        return "latin1";
    case InvalidUTF8Policy::FFFD:
        break;
    }
    return "fffd";
}

// `min` is the shortest-form floor for the sequence length being decoded, which
// is a UTF-8 encoding rule; the rest is the scalar-value rule, shared with every
// other producer of a code point via isScalarValue.
static bool validCodePoint(std::uint32_t codePoint, std::uint32_t min)
{
    return codePoint >= min && isScalarValue(codePoint);
}

void utf8Reset(UTF8Decoder* d)
{
    d->acc = 0;
    d->min = 0;
    d->need = 0;
    d->lowestContinuation = 0x80;
    d->highestContinuation = 0xBF;
}

UTF8Status utf8Feed(UTF8Decoder* d, std::uint8_t byte, std::uint32_t* outCodePoint)
{
    if (d->need > 0) {
        if (byte >= d->lowestContinuation && byte <= d->highestContinuation) {
            d->lowestContinuation = 0x80;
            d->highestContinuation = 0xBF;
            d->acc = (d->acc << 6) | static_cast<std::uint32_t>(byte & 0x3F);
            if (--d->need > 0)
                return UTF8Status::Need;
            std::uint32_t codePoint = d->acc;
            std::uint32_t min = d->min;
            d->acc = 0;
            d->min = 0;
            if (!validCodePoint(codePoint, min))
                return UTF8Status::Invalid;
            *outCodePoint = codePoint;
            return UTF8Status::Emit;
        }
        // A byte outside the permitted continuation range ends the maximal
        // subpart before it: drop state; caller may re-feed this byte as a fresh start.
        utf8Reset(d);
        return UTF8Status::Invalid;
    }

    if (byte < 0x80) {
        *outCodePoint = byte;
        return UTF8Status::Emit;
    }
    if ((byte & 0xE0) == 0xC0 && byte >= 0xC2) {
        d->acc = byte & 0x1F;
        d->min = 0x80;
        d->need = 1;
        d->lowestContinuation = 0x80;
        d->highestContinuation = 0xBF;
        return UTF8Status::Need;
    }
    if ((byte & 0xF0) == 0xE0) {
        d->acc = byte & 0x0F;
        d->min = 0x800;
        d->need = 2;
        d->lowestContinuation = 0x80;
        d->highestContinuation = 0xBF;
        // E0 would be overlong below A0; ED would be a surrogate from A0.
        if (byte == 0xE0)
            d->lowestContinuation = 0xA0;
        else if (byte == 0xED)
            d->highestContinuation = 0x9F;
        return UTF8Status::Need;
    }
    if ((byte & 0xF8) == 0xF0 && byte <= 0xF4) {
        d->acc = byte & 0x07;
        d->min = 0x10000;
        d->need = 3;
        d->lowestContinuation = 0x80;
        d->highestContinuation = 0xBF;
        // F0 would be overlong below 90; F4 would pass U+10FFFF from 90.
        if (byte == 0xF0)
            d->lowestContinuation = 0x90;
        else if (byte == 0xF4)
            d->highestContinuation = 0x8F;
        return UTF8Status::Need;
    }
    // Lone continuation, overlong lead, or 5/6-byte form.
    return UTF8Status::Invalid;
}

// Decode a run of complete MULTI-byte UTF-8 sequences (lead >= 0xC2) from
// in[0..length) into out[0..maxOut), stopping at the first ASCII / control /
// continuation byte (< 0xC2), an incomplete trailing sequence, an invalid
// sequence, or maxOut codePoints. May return fewer than maxOut with more
// decodable input remaining (SIMD path bounds its span by bytes) — every caller
// loops. *bytesConsumed gets the input bytes that produced the returned
// codePoints. Accept/reject (overlong, surrogate, > 0x10FFFF) matches utf8Feed.
int utf8DecodeRun(std::span<const char> input, std::span<std::uint32_t> out, int* bytesConsumed)
{
    const char* in = input.data();
    const int length = static_cast<int>(input.size());
    std::uint32_t* outputBuffer = out.data();
    const int maxOut = static_cast<int>(out.size());
    int p = 0, n = 0;
#if defined(__aarch64__)
    // Deinterleave complete three-byte sequences without a separate span scan.
    // Validate every lane before storing: the scalar tail must still stop at
    // the first malformed sequence, including overlongs and surrogates.
    if (maxOut >= 32 && length >= 96 && (static_cast<std::uint8_t>(in[0]) & 0xF0) == 0xE0) {
        while (length - p >= 48 && maxOut - n >= 16) {
            const uint8x16x3_t bytes = vld3q_u8(reinterpret_cast<const std::uint8_t*>(in + p));
            const uint8x16_t leads = bytes.val[0];
            const uint8x16_t seconds = bytes.val[1];
            const uint8x16_t thirds = bytes.val[2];
            const uint8x16_t invalidLead = veorq_u8(vandq_u8(leads, vdupq_n_u8(0xF0)), vdupq_n_u8(0xE0));
            const uint8x16_t invalidSecond = veorq_u8(vandq_u8(seconds, vdupq_n_u8(0xC0)), vdupq_n_u8(0x80));
            const uint8x16_t invalidThird = veorq_u8(vandq_u8(thirds, vdupq_n_u8(0xC0)), vdupq_n_u8(0x80));
            const uint8x16_t overlong = vandq_u8(vceqq_u8(leads, vdupq_n_u8(0xE0)), vcltq_u8(seconds, vdupq_n_u8(0xA0)));
            const uint8x16_t surrogate = vandq_u8(vceqq_u8(leads, vdupq_n_u8(0xED)), vcgeq_u8(seconds, vdupq_n_u8(0xA0)));
            const uint8x16_t invalid = vorrq_u8(vorrq_u8(invalidLead, invalidSecond),
                vorrq_u8(invalidThird, vorrq_u8(overlong, surrogate)));
            if (vmaxvq_u8(invalid))
                break;

            const uint8x16_t leadPayload = vandq_u8(leads, vdupq_n_u8(0x0F));
            const uint8x16_t secondPayload = vandq_u8(seconds, vdupq_n_u8(0x3F));
            const uint8x16_t thirdPayload = vandq_u8(thirds, vdupq_n_u8(0x3F));
            const uint16x8_t low = vorrq_u16(vshlq_n_u16(vmovl_u8(vget_low_u8(leadPayload)), 12),
                vorrq_u16(vshlq_n_u16(vmovl_u8(vget_low_u8(secondPayload)), 6), vmovl_u8(vget_low_u8(thirdPayload))));
            const uint16x8_t high = vorrq_u16(vshlq_n_u16(vmovl_u8(vget_high_u8(leadPayload)), 12),
                vorrq_u16(vshlq_n_u16(vmovl_u8(vget_high_u8(secondPayload)), 6), vmovl_u8(vget_high_u8(thirdPayload))));
            vst1q_u32(outputBuffer + n, vmovl_u16(vget_low_u16(low)));
            vst1q_u32(outputBuffer + n + 4, vmovl_u16(vget_high_u16(low)));
            vst1q_u32(outputBuffer + n + 8, vmovl_u16(vget_low_u16(high)));
            vst1q_u32(outputBuffer + n + 12, vmovl_u16(vget_high_u16(high)));
            p += 48;
            n += 16;
        }
#if UBIQUITTY_ENABLE_TEST_HOOKS
        if (n) {
            ++simdRunCountForTest;
            ++threeByteSIMDRunCountForTest;
        }
#endif
    }
#endif
    // SIMD fast path: stop before a sequence would exceed maxOut, trim a
    // boundary-split trailing sequence, then validate+convert in one shot.
    // Length gate (~96 bytes): simdutf's per-call cost loses to the scalar loop
    // on short runs. Invalid spans fall through to scalar.
    {
        constexpr int simdMinBytes = 96;
        const int capacity = maxOut - n;
        const int remainingLength = length - p;
        if (capacity >= simdMinBytes / 4) {
            // Bound the scan before running it: an unbounded scan over a
            // multibyte flood would walk the whole remaining chunk per call.
            // Four bytes per output also bounds malformed continuation runs.
            // Multiply only when the result cannot exceed the input length.
            const int scanLimit = capacity > remainingLength / 4 ? remainingLength : capacity * 4;
            int mb = nonASCIISpanEnd(input.subspan(static_cast<std::size_t>(p), static_cast<std::size_t>(scanLimit)), capacity);
            if (mb == 0) {
#if UBIQUITTY_ENABLE_TEST_HOOKS
                decodedBytesForTest += static_cast<std::uint64_t>(p);
#endif
                *bytesConsumed = p;
                return n;
            }
            if (mb >= simdMinBytes)
                mb = trimSplitTrailingSequence(in + p, mb);
            if (mb >= simdMinBytes) {
                // convert_utf8_to_utf32 validates and converts in one sweep,
                // returning 0 on any invalid byte → fall through to scalar, which
                // stops exactly at the bad byte (preserving bytesConsumed).
                const std::size_t nc = simdutf::convert_utf8_to_utf32(in + p, static_cast<std::size_t>(mb), reinterpret_cast<char32_t*>(outputBuffer + n));
                if (nc > 0) {
#if UBIQUITTY_ENABLE_TEST_HOOKS
                    if (!n)
                        ++simdRunCountForTest;
                    decodedBytesForTest += static_cast<std::uint64_t>(p + mb);
#endif
                    *bytesConsumed = p + mb;
                    return n + static_cast<int>(nc);
                }
            }
        }
    }
    while (p < length && n < maxOut) {
        const std::uint8_t b0 = static_cast<std::uint8_t>(in[p]);
        if (b0 < 0xC2)
            break; // ASCII / control / continuation
#if defined(__x86_64__) || defined(_M_X64)
        // Fixed-width payloads reduce scalar bookkeeping on x86. ARM retains
        // the shared continuation schedule below. Both obey the same prefix
        // contract.
        // C2..DF and one continuation encode exactly U+0080..U+07FF.
        // This includes Cyrillic and most accented Latin: neither a variable
        // continuation loop nor the surrogate/upper-bound checks apply.
        if (b0 < 0xE0) {
            if (length - p < 2)
                break;
            const auto continuation = static_cast<std::uint8_t>(in[p + 1]);
            if ((continuation & 0xC0) != 0x80)
                break;
            outputBuffer[n++] = (static_cast<std::uint32_t>(b0 & 0x1F) << 6) | (continuation & 0x3F);
            p += 2;
            continue;
        }
        if (b0 < 0xF0) {
            if (length - p < 3)
                break;
            const auto second = static_cast<std::uint8_t>(in[p + 1]);
            const auto third = static_cast<std::uint8_t>(in[p + 2]);
            if ((second & 0xC0) != 0x80 || (third & 0xC0) != 0x80)
                break;
            const auto codePoint = (static_cast<std::uint32_t>(b0 & 0x0F) << 12)
                | (static_cast<std::uint32_t>(second & 0x3F) << 6) | (third & 0x3F);
            if (!validCodePoint(codePoint, 0x800))
                break;
            outputBuffer[n++] = codePoint;
            p += 3;
            continue;
        }
        if (b0 > 0xF4 || length - p < 4)
            break;
        const auto second = static_cast<std::uint8_t>(in[p + 1]);
        const auto third = static_cast<std::uint8_t>(in[p + 2]);
        const auto fourth = static_cast<std::uint8_t>(in[p + 3]);
        if ((second & 0xC0) != 0x80 || (third & 0xC0) != 0x80 || (fourth & 0xC0) != 0x80)
            break;
        const auto codePoint = (static_cast<std::uint32_t>(b0 & 0x07) << 18)
            | (static_cast<std::uint32_t>(second & 0x3F) << 12)
            | (static_cast<std::uint32_t>(third & 0x3F) << 6) | (fourth & 0x3F);
        if (!validCodePoint(codePoint, 0x10000))
            break;
        outputBuffer[n++] = codePoint;
        p += 4;
#else
        int need;
        std::uint32_t acc, min;
        if ((b0 & 0xE0) == 0xC0) {
            need = 1;
            acc = b0 & 0x1Fu;
            min = 0x80;
        } else if ((b0 & 0xF0) == 0xE0) {
            need = 2;
            acc = b0 & 0x0Fu;
            min = 0x800;
        } else if ((b0 & 0xF8) == 0xF0 && b0 <= 0xF4) {
            need = 3;
            acc = b0 & 0x07u;
            min = 0x10000;
        } else
            break; // invalid lead (overlong / 5-6 byte)
        if (p + 1 + need > length)
            break; // incomplete trailing sequence
        bool ok = true;
        for (int k = 1; k <= need; ++k) {
            const std::uint8_t bk = static_cast<std::uint8_t>(in[p + k]);
            if ((bk & 0xC0) != 0x80) {
                ok = false;
                break;
            }
            acc = (acc << 6) | static_cast<std::uint32_t>(bk & 0x3F);
        }
        if (!ok || !validCodePoint(acc, min))
            break;
        outputBuffer[n++] = acc;
        p += 1 + need;
#endif
    }
#if UBIQUITTY_ENABLE_TEST_HOOKS
    decodedBytesForTest += static_cast<std::uint64_t>(p);
#endif
    *bytesConsumed = p;
    return n;
}

} // namespace Ubiquitty::Unicode
