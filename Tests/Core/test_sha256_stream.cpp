// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#ifndef UBIQUITTY_TEST_WITHOUT_CORE
#define UBIQUITTY_TEST_WITHOUT_CORE
#endif
#include "Core/Crypto/SHA256.h"
#include "Harness.h"

#include <algorithm>
#include <array>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace {

using Ubiquitty::Crypto::SHA256Error;
using Ubiquitty::Crypto::SHA256Hasher;
using Digest = std::array<unsigned char, 32>;

static_assert(sizeof(SHA256Hasher) <= 512);

std::span<const unsigned char> bytesOf(std::string_view text)
{
    return { reinterpret_cast<const unsigned char*>(text.data()), text.size() };
}

std::string digestHex(std::span<const unsigned char, 32> digest)
{
    constexpr std::string_view digits = "0123456789abcdef";
    std::string result;
    result.reserve(64);
    for (const auto byte : digest) {
        result.push_back(digits[byte >> 4]);
        result.push_back(digits[byte & 15]);
    }
    return result;
}

} // namespace

TEST(incrementalSHA256MatchesKnownAnswersAtEverySplit)
{
    const std::array<std::pair<std::string, std::string_view>, 4> vectors { {
        { "", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855" },
        { "abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad" },
        { "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1" },
        { std::string(128, 'a'), "6836cf13bac400e9105071cd6af47084dfacad4e5e302c94bfed24e013afb73e" },
    } };
    for (const auto& [input, expected] : vectors) {
        const auto bytes = bytesOf(input);
        CHECK(Ubiquitty::Crypto::sha256Hex(input) == expected);
        for (std::size_t split = 0; split <= bytes.size(); ++split) {
            SHA256Hasher hasher;
            REQUIRE(hasher.update(bytes.first(split)));
            REQUIRE(hasher.update({}));
            REQUIRE(hasher.update(bytes.subspan(split)));
            Digest digest;
            REQUIRE(hasher.finalize(digest));
            CHECK(digestHex(digest) == expected);
            Digest oneShot;
            Ubiquitty::Crypto::sha256(bytes.first(split), bytes.subspan(split), oneShot);
            CHECK(digest == oneShot);
        }
    }
}

TEST(incrementalSHA256KeepsEveryBlockAndPaddingBoundaryAcrossPartitions)
{
    std::array<unsigned char, 257> input;
    for (std::size_t index = 0; index < input.size(); ++index)
        input[index] = static_cast<unsigned char>((index * 37 + index / 7) & 255);
    constexpr std::array<std::size_t, 18> lengths { 0, 1, 2, 55, 56, 57, 63, 64, 65, 119, 120, 121, 127, 128, 129, 255, 256, 257 };
    constexpr std::array<std::size_t, 10> pieces { 1, 2, 3, 7, 16, 31, 63, 64, 65, 127 };
    for (const auto length : lengths) {
        const auto bytes = std::span(input).first(length);
        Digest expected;
        Ubiquitty::Crypto::sha256(bytes, expected);
        for (std::size_t split = 0; split <= length; ++split) {
            SHA256Hasher hasher;
            REQUIRE(hasher.update(bytes.first(split)));
            REQUIRE(hasher.update(bytes.subspan(split)));
            Digest digest;
            REQUIRE(hasher.finalize(digest));
            CHECK(digest == expected);
        }
        for (const auto piece : pieces) {
            SHA256Hasher hasher;
            auto remaining = bytes;
            while (!remaining.empty()) {
                const auto count = std::min(piece, remaining.size());
                REQUIRE(hasher.update(remaining.first(count)));
                remaining = remaining.subspan(count);
            }
            Digest digest;
            REQUIRE(hasher.finalize(digest));
            CHECK(digest == expected);
        }
    }
}

TEST(incrementalSHA256HashesEveryBinaryByte)
{
    std::array<unsigned char, 256> input;
    for (std::size_t index = 0; index < input.size(); ++index)
        input[index] = static_cast<unsigned char>(index);
    SHA256Hasher hasher;
    for (const auto& byte : input)
        REQUIRE(hasher.update({ &byte, 1 }));
    Digest digest;
    REQUIRE(hasher.finalize(digest));
    CHECK(digestHex(digest) == "40aff2e9d2d8922e47afd4648e6967497158785fbd1da870e7110266bf944880");
}

TEST(incrementalSHA256ConsumesBorrowedRangesBeforeReturning)
{
    std::array<unsigned char, 80> input;
    for (std::size_t index = 0; index < input.size(); ++index)
        input[index] = static_cast<unsigned char>(index * 3);
    Digest expected;
    Ubiquitty::Crypto::sha256(input, expected);
    SHA256Hasher hasher;
    REQUIRE(hasher.update(std::span(input).first(17)));
    std::fill_n(input.begin(), 17, 0xA5);
    REQUIRE(hasher.update(std::span(input).subspan(17)));
    input.fill(0);
    Digest digest;
    REQUIRE(hasher.finalize(digest));
    CHECK(digest == expected);
}

TEST(incrementalSHA256OwnersCanBeInterleaved)
{
    const auto bytes = bytesOf("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq");
    SHA256Hasher first;
    SHA256Hasher second;
    REQUIRE(first.update(bytesOf("a")));
    REQUIRE(second.update(bytes.first(17)));
    REQUIRE(first.update(bytesOf("bc")));
    REQUIRE(second.update(bytes.subspan(17)));
    Digest firstDigest;
    Digest secondDigest;
    REQUIRE(second.finalize(secondDigest));
    REQUIRE(first.finalize(firstDigest));
    CHECK(digestHex(firstDigest) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(digestHex(secondDigest) == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

TEST(incrementalSHA256FinalizesOnceAndRefusesEveryLaterOperation)
{
    SHA256Hasher hasher;
    REQUIRE(hasher.update({}));
    REQUIRE(hasher.update({}));
    Digest digest;
    REQUIRE(hasher.finalize(digest));
    CHECK(digestHex(digest) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    Digest unchanged;
    unchanged.fill(0xA5);
    const auto before = unchanged;
    const auto repeated = hasher.finalize(unchanged);
    REQUIRE(!repeated);
    CHECK(repeated.error() == SHA256Error::Finalized);
    CHECK(unchanged == before);
    for (const auto bytes : { std::span<const unsigned char> {}, bytesOf("abc") }) {
        const auto updated = hasher.update(bytes);
        REQUIRE(!updated);
        CHECK(updated.error() == SHA256Error::Finalized);
        const auto finalized = hasher.finalize(unchanged);
        REQUIRE(!finalized);
        CHECK(finalized.error() == SHA256Error::Finalized);
        CHECK(unchanged == before);
    }
}

TEST(incrementalSHA256MatchesTheMillionByteKnownAnswerWithFixedInputStorage)
{
    std::array<unsigned char, 1000> piece;
    piece.fill('a');
    SHA256Hasher hasher;
    for (int count = 0; count < 1000; ++count)
        REQUIRE(hasher.update(piece));
    Digest digest;
    REQUIRE(hasher.finalize(digest));
    CHECK(digestHex(digest) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

UBIQUITTY_TEST_MAIN()
