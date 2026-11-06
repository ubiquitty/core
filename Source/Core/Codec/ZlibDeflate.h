// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
// Encode a zlib (RFC 1950) stream wrapping a static-Huffman DEFLATE (RFC 1951)
// block, or stored blocks where those are shorter: the inverse of
// ZlibInflate. Self-contained LZ77 + fixed-Huffman compressor. Header-only so
// any consumer gets it by include alone.

#include "Base/Types.h"
#include "Core/Codec/DeflateTables.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

namespace Ubiquitty::Codec::Zlib {
namespace DeflateDetail {

// RFC 1951 code tables, shared with the inflate side (namespace Rendering).

// The DEFLATE bit stream is filled LSB-first within each byte. Huffman codes are
// defined MSB-first, so they are bit-reversed before packing; extra bits pack
// verbatim (LSB-first).
// Every byte with its bits in the opposite order, so a Huffman code's reversal
// is two lookups rather than a loop over its bits.
inline constexpr std::array<std::uint8_t, 256> reversedByte = [] {
    std::array<std::uint8_t, 256> table {};
    for (int value = 0; value < 256; ++value) {
        int reversed = 0;
        for (int bit = 0; bit < 8; ++bit)
            reversed = (reversed << 1) | ((value >> bit) & 1);
        table[static_cast<std::size_t>(value)] = static_cast<std::uint8_t>(reversed);
    }
    return table;
}();

/// Output that grows with what is written: `compress`'s.
struct GrowingOutput {
    std::vector<std::uint8_t>& bytes;

    /// Appends one byte.
    /// - Parameter byte: The byte.
    void put(std::uint8_t byte) { bytes.push_back(byte); }
    [[nodiscard]] static constexpr bool isFull() { return false; }
};

/// Output of a fixed capacity, which takes nothing past its end and remembers
/// that it was asked to: `compressInto`'s.
struct BoundedOutput {
    std::span<std::uint8_t> bytes;
    std::size_t size = 0;
    bool hasOverflowed = false;

    /// Writes one byte, or records that there was no room for it.
    /// - Parameter byte: The byte.
    void put(std::uint8_t byte)
    {
        if (size == bytes.size()) {
            hasOverflowed = true;
            return;
        }
        bytes[size++] = byte;
    }
    [[nodiscard]] bool isFull() const { return hasOverflowed; }
};

template<typename Output>
struct BitWriter {
    Output& out;
    std::uint32_t accumulator = 0;
    int bitCount = 0;

    void bits(std::uint32_t value, int count)
    {
        value &= (count >= 32) ? 0xFFFFFFFFu : ((1u << count) - 1u);
        accumulator |= value << bitCount;
        bitCount += count;
        while (bitCount >= 8) {
            out.put(static_cast<std::uint8_t>(accumulator & 0xFF));
            accumulator >>= 8;
            bitCount -= 8;
        }
    }
    void huffman(std::uint32_t code, int length)
    {
        // Deflate writes a Huffman code most-significant bit first while the
        // bit stream takes them least-significant first, so every code is
        // reversed on the way out — once per literal, which is once per byte of
        // payload that finds no match. A byte-wide table settles it in two
        // lookups where a loop takes one iteration per bit; no code here is
        // longer than nine bits, so the high byte is only ever the low two.
        const std::uint32_t reversed = (static_cast<std::uint32_t>(reversedByte[code & 0xFF]) << 8)
            | reversedByte[(code >> 8) & 0xFF];
        bits(reversed >> (16 - length), length);
    }
    void flush()
    {
        if (bitCount > 0) {
            out.put(static_cast<std::uint8_t>(accumulator & 0xFF));
            accumulator = 0;
            bitCount = 0;
        }
    }
};

// Static literal/length code (RFC 1951 §3.2.6): the (code, bitLength) for a
// literal byte (0-255), end-of-block (256), or a length symbol (257-285).
inline void staticLiteralLengthCode(int symbol, std::uint32_t& code, int& length)
{
    if (symbol <= 143) {
        code = 0x30u + symbol;
        length = 8;
    } else if (symbol <= 255) {
        code = 0x190u + (symbol - 144);
        length = 9;
    } else if (symbol <= 279) {
        code = static_cast<std::uint32_t>(symbol - 256);
        length = 7;
    } else {
        code = 0xC0u + (symbol - 280);
        length = 8;
    }
}

template<typename Output>
void emitLiteral(BitWriter<Output>& writer, std::uint8_t byte)
{
    std::uint32_t code;
    int length;
    staticLiteralLengthCode(byte, code, length);
    writer.huffman(code, length);
}

template<typename Output>
void emitMatch(BitWriter<Output>& writer, int matchLength, int distance)
{
    int lengthIndex = 28;
    while (lengthIndex > 0 && lengthBase[lengthIndex] > matchLength)
        --lengthIndex;
    std::uint32_t code;
    int codeLength;
    staticLiteralLengthCode(257 + lengthIndex, code, codeLength);
    writer.huffman(code, codeLength);
    if (lengthExtra[lengthIndex])
        writer.bits(static_cast<std::uint32_t>(matchLength - lengthBase[lengthIndex]), lengthExtra[lengthIndex]);

    int distanceIndex = 29;
    while (distanceIndex > 0 && distanceBase[distanceIndex] > distance)
        --distanceIndex;
    writer.huffman(static_cast<std::uint32_t>(distanceIndex), 5); // static distance: 5-bit
    if (distanceExtra[distanceIndex])
        writer.bits(static_cast<std::uint32_t>(distance - distanceBase[distanceIndex]), distanceExtra[distanceIndex]);
}

inline constexpr int windowSize = 32768;
inline constexpr int hashBits = 15;
inline constexpr int hashSize = 1 << hashBits;

/// The hash chains a compress call matches against, kept between calls.
///
/// Building them fresh cost a 256 KB allocation and fill on every compress,
/// which for the few-KB row deltas a shared session sends is the whole of the
/// work; a stamp per bucket makes the reset a counter bump instead.
///
/// `prev` needs no reset at all: it is reachable only by walking a chain from
/// a bucket this call stamped, and every position on such a chain was inserted
/// by this call, which wrote its `prev` slot. Window aliasing inside one call
/// is the case the chain walk's own non-decreasing check already ends.
struct MatchState {
    std::vector<int> head;
    std::vector<int> prev;
    std::vector<std::uint32_t> stamp;
    std::uint32_t generation = 0;
};

/// The calling thread's match state, shared by every output kind.
/// - Returns: The state, empty until its first use on this thread.
inline MatchState& matchState()
{
    static thread_local MatchState state;
    return state;
}

/// Compresses a byte range into one static-Huffman DEFLATE block.
///
/// BFINAL is set, so the result is a complete stream rather than the first of
/// several blocks. Static Huffman rather than dynamic: the tables are fixed by
/// RFC 1951 §3.2.6 and cost no header, which suits the short payloads this
/// encodes at the price of a worse ratio on long ones. A bounded output stops
/// the search once it is full, leaving the block incomplete.
///
/// - Parameters:
///   - data: The bytes to compress.
///   - writer: Receives the block, without a zlib header or checksum.
template<typename Output>
void deflateStaticInto(std::span<const std::uint8_t> data, BitWriter<Output>& writer)
{
    writer.bits(1, 1); // BFINAL = 1 (one block)
    writer.bits(1, 2); // BTYPE = 01 (static Huffman)

    constexpr int minMatch = 3;
    constexpr int maxMatchLength = 258;
    constexpr int windowMask = windowSize - 1;
    constexpr int maxChain = 256; // bound the match search per position

    MatchState& state = matchState();
    if (state.head.empty()) {
        state.head.assign(hashSize, -1);
        state.prev.assign(windowSize, -1);
        state.stamp.assign(hashSize, 0);
    }
    ++state.generation;
    if (state.generation == 0) {
        // The counter wrapped, so every stamp below it would read as current.
        std::ranges::fill(state.stamp, std::uint32_t { 0 });
        state.generation = 1;
    }
    const std::uint32_t generation = state.generation;
    int* const head = state.head.data();
    int* const prev = state.prev.data();
    std::uint32_t* const stamp = state.stamp.data();

    auto hash3 = [&](std::size_t i) -> int { return ((static_cast<int>(data[i]) << 10) ^ (static_cast<int>(data[i + 1]) << 5) ^ static_cast<int>(data[i + 2])) & (hashSize - 1); };
    auto bucketHead = [&](int h) { return stamp[h] == generation ? head[h] : -1; };
    auto insert = [&](std::size_t i) {
        if (i + minMatch <= data.size()) {
            int h = hash3(i);
            prev[i & windowMask] = bucketHead(h);
            head[h] = static_cast<int>(i);
            stamp[h] = generation;
        }
    };

    std::size_t i = 0;
    while (i < data.size() && !writer.out.isFull()) {
        int bestLength = 0;
        int bestDistance = 0;
        if (i + minMatch <= data.size()) {
            const int maxMatch = static_cast<int>(std::min(static_cast<std::size_t>(maxMatchLength), data.size() - i));
            int candidate = bucketHead(hash3(i));
            int chain = maxChain;
            while (candidate >= 0 && chain-- > 0) {
                const int distance = static_cast<int>(i) - candidate;
                if (distance > windowSize)
                    break;
                // Eight bytes at a time while a whole word fits, then bytes.
                // Both reads stay in bounds: matchLength + 8 <= maxMatch and
                // maxMatch <= size - i, and candidate < i. The word loop finds
                // the same match length the byte loop would — on a mismatch it
                // lands exactly on the first differing byte, so the byte loop
                // below sees that mismatch immediately and stops. Output is
                // unchanged; this is the same LZ77 decision made faster.
                int matchLength = 0;
                while (matchLength + 8 <= maxMatch) {
                    std::uint64_t left = 0;
                    std::uint64_t right = 0;
                    std::memcpy(&left, data.data() + candidate + matchLength, 8);
                    std::memcpy(&right, data.data() + i + matchLength, 8);
                    if (left != right) {
                        const std::uint64_t differing = left ^ right;
                        matchLength += static_cast<int>((std::endian::native == std::endian::little
                                                                ? std::countr_zero(differing)
                                                                : std::countl_zero(differing))
                            / 8);
                        break;
                    }
                    matchLength += 8;
                }
                while (matchLength < maxMatch && data[candidate + matchLength] == data[i + matchLength])
                    ++matchLength;
                if (matchLength > bestLength) {
                    bestLength = matchLength;
                    bestDistance = distance;
                    if (matchLength >= maxMatch)
                        break;
                }
                int next = prev[candidate & windowMask];
                if (next >= candidate) // stale (window-aliased) chain entry
                    break;
                candidate = next;
            }
        }
        if (bestLength >= minMatch) {
            emitMatch(writer, bestLength, bestDistance);
            const std::size_t end = i + bestLength;
            while (i < end) {
                insert(i);
                ++i;
            }
        } else {
            emitLiteral(writer, data[i]);
            insert(i);
            ++i;
        }
    }

    std::uint32_t endCode;
    int endLength;
    staticLiteralLengthCode(256, endCode, endLength); // end-of-block
    writer.huffman(endCode, endLength);
    writer.flush();
}

/// Compresses a byte range into one static-Huffman DEFLATE block.
///
/// - Parameter data: The bytes to compress.
/// - Returns: The DEFLATE block, without a zlib header or checksum.
inline std::vector<std::uint8_t> deflateStatic(std::span<const std::uint8_t> data)
{
    std::vector<std::uint8_t> out;
    GrowingOutput output { out };
    BitWriter<GrowingOutput> writer { output };
    deflateStaticInto(data, writer);
    return out;
}

/// The longest stored block (RFC 1951 §3.2.4).
inline constexpr std::size_t maxStoredBlockBytes = 65535;

/// The bytes `deflateStored` writes for `inputBytes`: a five-byte header per
/// block, one block for empty input.
[[nodiscard]] constexpr std::size_t storedBytes(std::size_t inputBytes)
{
    const std::size_t blocks = inputBytes ? (inputBytes - 1) / maxStoredBlockBytes + 1 : 1;
    return inputBytes + 5 * blocks;
}

/// Writes a byte range as stored DEFLATE blocks, the last one final.
///
/// - Parameters:
///   - data: The bytes to store.
///   - output: At least `storedBytes(data.size())` bytes.
/// - Returns: The bytes written.
inline std::size_t deflateStored(std::span<const std::uint8_t> data, std::span<std::uint8_t> output)
{
    std::size_t written = 0;
    std::size_t offset = 0;
    do {
        const std::size_t length = std::min(maxStoredBlockBytes, data.size() - offset);
        const bool isFinal = offset + length == data.size();
        // BFINAL, then BTYPE 00, then padding to the byte; LEN and its
        // complement follow, least significant byte first.
        output[written++] = isFinal ? 1 : 0;
        output[written++] = static_cast<std::uint8_t>(length);
        output[written++] = static_cast<std::uint8_t>(length >> 8);
        output[written++] = static_cast<std::uint8_t>(~length);
        output[written++] = static_cast<std::uint8_t>(~length >> 8);
        if (length)
            std::memcpy(output.data() + written, data.data() + offset, length);
        written += length;
        offset += length;
    } while (offset < data.size());
    return written;
}

/// Computes the Adler-32 checksum of a byte range.
///
/// The trailer zlib requires (RFC 1950 §2.2), which is why it lives beside the
/// deflater rather than in a checksum module: nothing else in the tree wants it.
///
/// - Parameter data: The bytes to sum.
/// - Returns: The checksum, `b << 16 | a`.
inline std::uint32_t adler32(std::span<const std::uint8_t> data)
{
    constexpr std::uint32_t modulus = 65521;
    // Reduce once per block rather than once per byte. Reducing per byte costs
    // two integer divisions for every input byte, and an arm64 divide is both
    // slow and poorly pipelined — it dominated the checksum, and the checksum is
    // a whole extra pass over input that deflate has already read.
    //
    // 5552 is the bound every zlib takes: a starts below the modulus and rises
    // by at most 255 a byte, b by at most a, so neither leaves 32 bits inside a
    // block that long. The sums stay congruent, so the result is unchanged.
    constexpr std::size_t maxBlock = 5552;
    std::uint32_t a = 1;
    std::uint32_t b = 0;
    std::size_t offset = 0;
    while (offset < data.size()) {
        const std::size_t block = std::min(maxBlock, data.size() - offset);
        for (std::size_t i = 0; i < block; ++i) {
            a += data[offset + i];
            b += a;
        }
        a %= modulus;
        b %= modulus;
        offset += block;
    }
    return (b << 16) | a;
}

} // namespace DeflateDetail

/// The longest zlib stream `compressInto` writes for `inputBytes`: the
/// two-byte header, stored blocks, and the Adler-32 trailer. The caller bounds
/// `inputBytes` so the sum does not wrap.
[[nodiscard]] constexpr std::size_t compressBound(std::size_t inputBytes)
{
    return 2 + DeflateDetail::storedBytes(inputBytes) + 4;
}

/// Writes a complete zlib stream over `input` into a caller's buffer, without
/// allocating it: one static-Huffman block where that is no longer than stored
/// blocks, stored blocks otherwise. Decodes back via Codec::Zlib::Inflater.
///
/// - Parameters:
///   - input: The bytes to compress.
///   - output: At least `compressBound(input.size())` bytes.
/// - Returns: The stream's length.
inline std::size_t compressInto(std::span<const std::uint8_t> input, std::span<std::uint8_t> output)
{
    output[0] = 0x78; // CMF: CM=8 deflate, CINFO=7 -> 32 KB window
    output[1] = 0x01; // FLG: FCHECK so (0x7801 % 31 == 0), FDICT=0, FLEVEL=0
    const auto blocks = output.subspan(2, DeflateDetail::storedBytes(input.size()));
    DeflateDetail::BoundedOutput bounded { blocks };
    DeflateDetail::BitWriter<DeflateDetail::BoundedOutput> writer { bounded };
    DeflateDetail::deflateStaticInto(input, writer);
    const std::size_t blockBytes = bounded.hasOverflowed ? DeflateDetail::deflateStored(input, blocks) : bounded.size;
    const std::uint32_t checksum = DeflateDetail::adler32(input);
    std::size_t written = 2 + blockBytes;
    output[written++] = static_cast<std::uint8_t>(checksum >> 24);
    output[written++] = static_cast<std::uint8_t>(checksum >> 16);
    output[written++] = static_cast<std::uint8_t>(checksum >> 8);
    output[written++] = static_cast<std::uint8_t>(checksum);
    return written;
}

// Returns a complete zlib stream (2-byte header + deflate block + adler32
// trailer) over `input`. Decodes back via Codec::Zlib::Inflater /
// Codec::Zlib::inflateExact.
inline std::vector<std::uint8_t> compress(std::span<const std::uint8_t> input)
{
    std::vector<std::uint8_t> out;
    out.push_back(0x78); // CMF: CM=8 deflate, CINFO=7 -> 32 KB window
    out.push_back(0x01); // FLG: FCHECK so (0x7801 % 31 == 0), FDICT=0, FLEVEL=0
    const std::vector<std::uint8_t> deflated = DeflateDetail::deflateStatic(input);
    out.insert(out.end(), deflated.begin(), deflated.end());
    const std::uint32_t checksum = DeflateDetail::adler32(input);
    out.push_back(static_cast<std::uint8_t>(checksum >> 24));
    out.push_back(static_cast<std::uint8_t>(checksum >> 16));
    out.push_back(static_cast<std::uint8_t>(checksum >> 8));
    out.push_back(static_cast<std::uint8_t>(checksum));
    return out;
}

} // namespace Ubiquitty::Codec::Zlib
