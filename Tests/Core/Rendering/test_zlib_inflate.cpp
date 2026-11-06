// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

#include "Core/Codec/ZlibInflate.h"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <span>
#include <string>

using Ubiquitty::Codec::Zlib::Inflater;
using Ubiquitty::Codec::Zlib::InflateStatus;

/// Hand-built zlib stream wrapping a single stored deflate block.
///   78 01       — CMF/FLG (deflate, 32K window, FCHECK valid)
///   01          — BFINAL=1, BTYPE=00 stored block
///   05 00 fa ff — LEN=5 NLEN=fffa (~LEN)
///   "Hello"
///   05 8c 01 f5 — adler32 of "Hello" big-endian
static const uint8_t storedStream[] = { 0x78, 0x01, 0x01, 0x05, 0x00, 0xfa, 0xff, 0x48, 0x65, 0x6c, 0x6c, 0x6f, 0x05, 0x8c, 0x01, 0xf5 };
static const char* storedPlainText = "Hello";

/// zlib output of `compressobj(level=9, strategy=Z_FIXED)` over the
/// 35-byte string below. Forces a fixed-Huffman block.
static const uint8_t fixedStream[] = { 0x78, 0x01, 0xcb, 0x48, 0xcd, 0xc9, 0xc9, 0x57, 0x28, 0xcf, 0x2f, 0xca, 0x49, 0x51, 0xc8, 0xc0, 0xce, 0x06, 0x00, 0xef, 0x93, 0x0d, 0x55 };
static const char* fixedPlainText = "hello world hello world hello world";

/// Default-strategy zlib(level=9) of the 360-byte string below. Yields
/// a dynamic-Huffman block.
static const uint8_t dynamicStream[] = { 0x78, 0xda, 0x0b, 0xc9, 0x48, 0x55, 0x28, 0x2c, 0xcd, 0x4c, 0xce, 0x56, 0x48, 0x2a, 0xca, 0x2f, 0xcf, 0x53, 0x48, 0xcb, 0xaf, 0x50, 0xc8, 0x2a, 0xcd, 0x2d, 0x28, 0x56, 0xc8, 0x2f, 0x4b, 0x2d, 0x52, 0x28, 0x01, 0x4a, 0xe7, 0x24, 0x56, 0x55, 0x2a, 0xa4, 0xe4,
    0xa7, 0xeb, 0x29, 0x84, 0x8c, 0x2a, 0x26, 0x57, 0x31, 0x00, 0x65, 0x31, 0x81, 0x39 };
static const char* dynamicSentence = "The quick brown fox jumps over the lazy dog. ";
enum {
    dynamicRepeats = 8,
    dynamicPlainLength = 45 * 8
};

/// Decodes a stream with an inflater that has seen nothing before, the shape
/// every single-call case below shares.
static Inflater::DecodeResult inflateOnce(std::span<const uint8_t> stream, std::span<uint8_t> output)
{
    Inflater inflater;
    return inflater.decode(stream, output);
}

/// The stored-block stream with the bytes from `offset` on replaced. Each
/// malformed-header case differs from the valid stream in exactly one field,
/// so naming the field is the whole fixture.
static std::array<uint8_t, sizeof(storedStream)> storedStreamWith(std::size_t offset, std::initializer_list<uint8_t> replacement)
{
    std::array<uint8_t, sizeof(storedStream)> stream {};
    std::ranges::copy(storedStream, stream.begin());
    std::ranges::copy(replacement, stream.begin() + static_cast<std::ptrdiff_t>(offset));
    return stream;
}

TEST(testInflateStoredBlock)
{
    uint8_t outputBuffer[64];
    const Inflater::DecodeResult stored = inflateOnce(storedStream, outputBuffer);
    CHECK(stored.status == InflateStatus::Done);
    CHECK_EQ(stored.bytesProduced, strlen(storedPlainText));
    CHECK_MSG(memcmp(outputBuffer, storedPlainText, stored.bytesProduced) == 0, "the stored block's payload comes back verbatim");
    CHECK_EQ(stored.bytesConsumed, sizeof(storedStream));
}

TEST(testInflateFixedHuffman)
{
    uint8_t outputBuffer[128];
    const Inflater::DecodeResult fixed = inflateOnce(fixedStream, outputBuffer);
    CHECK(fixed.status == InflateStatus::Done);
    CHECK_EQ(fixed.bytesProduced, strlen(fixedPlainText));
    CHECK_MSG(memcmp(outputBuffer, fixedPlainText, fixed.bytesProduced) == 0, "the fixed-code block decodes to its text");
}

/// Whether the first `dynamicPlainLength` bytes of `output` are the sentence
/// repeated `dynamicRepeats` times, checked one repetition at a time.
static void checkDynamicPlainText(const uint8_t* output, const char* meaning)
{
    for (int repetition = 0; repetition < dynamicRepeats; ++repetition)
        CHECK_MSG(memcmp(output + repetition * 45, dynamicSentence, 45) == 0, meaning);
}

TEST(testInflateDynamicHuffman)
{
    uint8_t outputBuffer[512];
    const Inflater::DecodeResult dynamic = inflateOnce(dynamicStream, outputBuffer);
    CHECK(dynamic.status == InflateStatus::Done);
    CHECK_EQ(dynamic.bytesProduced, static_cast<std::size_t>(dynamicPlainLength));
    checkDynamicPlainText(outputBuffer, "every repetition of the sentence decodes from the dynamic block");
}

/// Feed input in 4-byte chunks and verify identical results to the
/// one-shot decode above.
TEST(testInflateStreaming)
{
    constexpr std::size_t feedSize = 4;
    constexpr int maxRounds = 10000;
    Inflater inflater;
    uint8_t outputBuffer[512];
    const std::span<const uint8_t> input(dynamicStream);
    const std::span<uint8_t> output(outputBuffer);
    std::size_t consumed = 0;
    std::size_t produced = 0;
    InflateStatus status = InflateStatus::NeedMoreInput;
    for (int round = 1; status != InflateStatus::Done; ++round) {
        const std::size_t feedLength = std::min(feedSize, input.size() - consumed);
        const Inflater::DecodeResult step = inflater.decode(input.subspan(consumed, feedLength), output.subspan(produced));
        status = step.status;
        consumed += step.bytesConsumed;
        produced += step.bytesProduced;
        CHECK_MSG(status == InflateStatus::NeedMoreInput || status == InflateStatus::Done, "a short feed either asks for more input or finishes the stream");
        REQUIRE_MSG(round <= maxRounds, "four bytes at a time still reaches the end of the stream");
    }
    CHECK_EQ(produced, static_cast<std::size_t>(dynamicPlainLength));
    checkDynamicPlainText(outputBuffer, "the chunked decode produces the one-shot decode's bytes");
}

/// Sanity check that a corrupted adler32 trailer is rejected.
TEST(testInflateBadAdler)
{
    // The final byte is the low byte of the big-endian Adler-32 trailer.
    const std::size_t last = sizeof(storedStream) - 1;
    const auto tampered = storedStreamWith(last, { static_cast<uint8_t>(storedStream[last] ^ 0x01) });
    uint8_t outputBuffer[64];
    CHECK(inflateOnce(tampered, outputBuffer).status == InflateStatus::Error);
}

/// reset() takes the decoder back to a usable initial state.
TEST(testInflateResetClearsState)
{
    Inflater z;
    uint8_t outputBuffer[64];
    (void)z.decode({ storedStream, sizeof(storedStream) }, { outputBuffer, sizeof(outputBuffer) });
    z.reset();
    const Inflater::DecodeResult again = z.decode(storedStream, outputBuffer);
    CHECK_MSG(again.status == InflateStatus::Done, "a reset inflater finishes the same stream a second time");
    CHECK_EQ(again.bytesProduced, strlen(storedPlainText));
}

/// CMF.CINFO > 7 means a window > 32K — out of spec; we reject.
TEST(testInflateBadZlibHeader)
{
    // CMF 0x88: deflate with CINFO 8, a 64K window.
    const auto wideWindow = storedStreamWith(0, { 0x88 });
    uint8_t outputBuffer[64];
    CHECK(inflateOnce(wideWindow, outputBuffer).status == InflateStatus::Error);
}

/// FCHECK byte mismatched against CMF — header CRC fails.
TEST(testInflateBadFcheck)
{
    // FLG 0x00 leaves CMF * 256 + FLG off a multiple of 31.
    const auto badCheckBits = storedStreamWith(1, { 0x00 });
    uint8_t outputBuffer[64];
    CHECK(inflateOnce(badCheckBits, outputBuffer).status == InflateStatus::Error);
}

/// Stored block whose declared LEN/NLEN one's-complement check fails
/// is rejected.
TEST(testInflateBadStoredLengthNlength)
{
    // NLEN 0x0000 is not the complement of LEN 0x0005.
    const auto uncomplemented = storedStreamWith(5, { 0x00, 0x00 });
    uint8_t outputBuffer[64];
    CHECK(inflateOnce(uncomplemented, outputBuffer).status == InflateStatus::Error);
}

/// Tiny output buffer forces NeedMoreOutput; second call with a
/// fresh buffer finishes the stream.
TEST(testInflateNeedMoreOutput)
{
    Inflater inflater;
    uint8_t narrow[2];
    uint8_t roomy[16];
    const Inflater::DecodeResult partial = inflater.decode(storedStream, narrow);
    CHECK(partial.status == InflateStatus::NeedMoreOutput);
    CHECK_EQ(partial.bytesProduced, 2u);
    const Inflater::DecodeResult finished = inflater.decode(std::span<const uint8_t>(storedStream).subspan(partial.bytesConsumed), roomy);
    CHECK(finished.status == InflateStatus::Done);
    std::string joined(narrow, narrow + partial.bytesProduced);
    joined.append(roomy, roomy + finished.bytesProduced);
    CHECK_EQ(joined.size(), strlen(storedPlainText));
    CHECK_STR_EQ(joined, storedPlainText);
}

/// Canonical empty deflate stream: 78 9c (CMF/FLG) + 03 00 (BFINAL=1,
/// BTYPE=01 fixed Huffman, end-of-block code) + 00 00 00 01 (adler32
/// of empty input). Must decode cleanly and reach InflateStatus::Done.
TEST(testInflateEmptyStream)
{
    uint8_t empty[] = { 0x78, 0x9c, 0x03, 0x00, 0x00, 0x00, 0x00, 0x01 };
    uint8_t outputBuffer[8];
    const Inflater::DecodeResult nothing = inflateOnce(empty, outputBuffer);
    CHECK(nothing.status == InflateStatus::Done);
    CHECK_EQ(nothing.bytesProduced, 0u);
}

/// Empty stream with a corrupted adler trailer must surface as error,
/// not as silent InflateStatus::Done.
TEST(testInflateEmptyStreamBadAdler)
{
    uint8_t bad[] = { 0x78, 0x9c, 0x03, 0x00, 0x00, 0x00, 0x00, 0x02 };
    uint8_t outputBuffer[8];
    CHECK_MSG(inflateOnce(bad, outputBuffer).status == InflateStatus::Error, "an empty stream still answers for its trailer");
}

/// inflateExact: the one-shot wrapper accepts only an exact-length decode.
TEST(testInflateExact)
{
    uint8_t outputBuffer[5];
    CHECK_MSG(Ubiquitty::Codec::Zlib::inflateExact({ storedStream, sizeof(storedStream) }, { outputBuffer, sizeof(outputBuffer) }), "exact-length decode succeeds");
    CHECK_MSG(memcmp(outputBuffer, storedPlainText, sizeof(outputBuffer)) == 0, "exact decode bytes");
    uint8_t tooShort[4];
    CHECK_MSG(!Ubiquitty::Codec::Zlib::inflateExact({ storedStream, sizeof(storedStream) }, { tooShort, sizeof(tooShort) }), "shorter-than-stream output rejected");
    uint8_t tooLong[6];
    CHECK_MSG(!Ubiquitty::Codec::Zlib::inflateExact({ storedStream, sizeof(storedStream) }, { tooLong, sizeof(tooLong) }), "longer-than-stream output rejected");
}

UBIQUITTY_TEST_MAIN()
