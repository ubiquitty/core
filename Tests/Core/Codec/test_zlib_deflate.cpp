// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

#include "Core/Codec/ZlibDeflate.h"
#include "Core/Codec/ZlibInflate.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace Zlib = Ubiquitty::Codec::Zlib;

namespace {

/// Bytes no LZ77 match shortens: a linear congruential sequence's high bytes.
std::vector<std::uint8_t> noise(std::size_t size)
{
    std::vector<std::uint8_t> bytes(size);
    std::uint32_t state = 0x2545F491u;
    for (auto& byte : bytes) {
        state = state * 1664525u + 1013904223u;
        byte = static_cast<std::uint8_t>(state >> 24);
    }
    return bytes;
}

/// Encodes `input` with `compressInto` into a buffer of exactly its bound,
/// and checks the stream inflates back to `input` and no more.
std::vector<std::uint8_t> roundTrip(std::span<const std::uint8_t> input)
{
    std::vector<std::uint8_t> stream(Zlib::compressBound(input.size()));
    const std::size_t length = Zlib::compressInto(input, stream);
    CHECK(length <= stream.size());
    stream.resize(length);
    std::vector<std::uint8_t> output(input.size());
    CHECK(Zlib::inflateExact(stream, output));
    CHECK(std::ranges::equal(output, input));
    return stream;
}

} // namespace

TEST(theBoundIsStoredBlocksAroundAZlibHeaderAndTrailer)
{
    CHECK_EQ(Zlib::compressBound(0), 2u + 5u + 4u);
    CHECK_EQ(Zlib::compressBound(1), 2u + 5u + 1u + 4u);
    CHECK_EQ(Zlib::compressBound(65535), 2u + 5u + 65535u + 4u);
    CHECK_EQ(Zlib::compressBound(65536), 2u + 10u + 65536u + 4u);
}

TEST(aCompressibleInputIsOneStaticBlockAsCompressWritesIt)
{
    std::vector<std::uint8_t> input(990 * 190 * 4, 0);
    for (std::size_t index = 0; index < input.size(); index += 4 * 97)
        input[index] = static_cast<std::uint8_t>(index);
    const auto stream = roundTrip(input);
    CHECK(stream == Zlib::compress(input));
    CHECK(stream.size() * 20 < input.size());
}

TEST(anIncompressibleInputIsStoredBlocksAtExactlyTheBound)
{
    for (const std::size_t size : { std::size_t { 300 }, std::size_t { 65535 }, std::size_t { 65536 }, std::size_t { 200'000 } }) {
        const auto input = noise(size);
        const auto stream = roundTrip(input);
        // The static block would be longer, so the encoder stores instead.
        CHECK(Zlib::compress(input).size() > stream.size());
        CHECK_EQ(stream.size(), Zlib::compressBound(size));
    }
}

TEST(anEmptyInputIsOneEmptyFinalBlock)
{
    const auto stream = roundTrip({});
    CHECK(stream.size() <= Zlib::compressBound(0));
}

UBIQUITTY_TEST_MAIN()
