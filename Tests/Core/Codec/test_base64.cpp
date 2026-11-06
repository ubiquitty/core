// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_base64 — the codec's documented rejections and both directions, asserted
// directly.
//
// Every other path to `Codec::base64Decode` reaches it through a caller that
// frames the payload: OSC 52 through the clipboard, OSC 1337 through the image
// dispatcher, the fuzzers through the VT parser. Those exercise the happy path
// and whatever malformed shapes their own framing happens to produce, and none
// of them chooses an input to test a verdict — so a decoder that started
// accepting a byte outside the alphabet would keep every one of them green
// until something downstream noticed a short buffer.
//
// The header names four rejections and one positive. Each gets a case, because
// a decoder reached by attacker-controlled bytes is where "refuses" and "writes
// past the end" are the same line of code read two ways.
//
// The values here are measured rather than derived from the prose. The final
// group's boundary is not obvious from reading: one character carries 6 bits and
// cannot form a byte, so it is cut mid-character and rejected; two carry 12 and
// yield one byte with 4 spare; three carry 18 and yield two with 2 spare. Both
// of the latter succeed, padded or not.

#include "Core/Codec/Base64.h"
#include "Core/Kitty/Graphics.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using Ubiquitty::Codec::base64Decode;
using Ubiquitty::Codec::base64DecodedCapacity;
using Ubiquitty::Codec::base64Encode;
using Ubiquitty::Codec::base64EncodedLength;

namespace {

/// The size of a successful result, or nothing for a failed one.
std::optional<std::size_t> sizeOf(std::optional<std::span<char>> result)
{
    return result.transform([](std::span<char> written) { return written.size(); });
}

/// Decodes into a buffer wide enough for any input these cases use.
std::optional<std::size_t> decode(std::string_view in)
{
    std::array<char, 64> output {};
    return sizeOf(base64Decode(in, output));
}

} // namespace

TEST(base64RejectsAByteOutsideTheAlphabet)
{
    // The standard alphabet only. Whitespace is rejected with everything else,
    // because the escape sequences this serves have already counted the payload
    // and a decoder that skipped a newline would return fewer bytes than the
    // sender described.
    CHECK(decode("QQ*A") == std::nullopt);
    CHECK(decode("QQ A") == std::nullopt);
    CHECK(decode("QQ\nA") == std::nullopt);
    CHECK(decode("QQ-A") == std::nullopt); // URL-safe; the header sends that caller to a
                                           // second function rather than a flag
    CHECK(decode("QQ_A") == std::nullopt);
    CHECK(decode("QQ\tA") == std::nullopt);
    CHECK(decode("QQ\rA") == std::nullopt);

    // Whitespace is the one the kernels underneath disagree about: simdutf skips
    // ASCII spaces at every option it offers, because MIME base64 permits line
    // breaks, and there is no setting that refuses them. Base64.cpp refuses them
    // itself. Delete that check and these five cases decode rather than fail.
    CHECK(decode(" QUJD") == std::nullopt);
    CHECK(decode("QUJD ") == std::nullopt);
}

TEST(base64RejectsANonPadByteAfterPadding)
{
    // Two payloads concatenated look exactly like one padded payload followed by
    // more characters, and accepting them would decode a message the sender did
    // not send.
    CHECK(decode("QQ=A") == std::nullopt);
    CHECK(decode("QQ==QUJD") == std::nullopt);

    // Padding has to describe the final group. A count of characters that is not
    // a multiple of four once padding appears is a payload whose sender and
    // receiver disagree about where it ended, so it is refused rather than
    // interpreted — and so is padding that follows an already-whole group.
    CHECK(decode("QQ==") == 1u);
    CHECK(decode("QQ=") == std::nullopt);
    CHECK(decode("QUJD=") == std::nullopt);
    CHECK(decode("QUJD==") == std::nullopt);
    CHECK(decode("====") == std::nullopt);

    // Unpadded is not the same as mispadded: a final group with no padding at
    // all is what every producer that omits it emits, and it decodes.
    CHECK(decode("QQ") == 1u);
}

TEST(base64RejectsAnOutputSpanTooSmallForThePayload)
{
    // The one that separates a refusal from a write past the end. Three bytes
    // for every four characters, and a smaller span fails rather than filling
    // what fits.
    std::array<char, 64> output {};
    CHECK(sizeOf(base64Decode("QUJD", std::span<char>(output.data(), 3))) == 3u);
    CHECK(base64Decode("QUJD", std::span<char>(output.data(), 2)) == std::nullopt);
    CHECK(base64Decode("QUJD", std::span<char>(output.data(), 0)) == std::nullopt);

    // The span is judged against what the payload produces, not against a bound
    // derived from the character count: two characters yield one byte and fit a
    // span of one, though four characters' worth of ceiling would not.
    CHECK(sizeOf(base64Decode("QQ", std::span<char>(output.data(), 1))) == 1u);
    CHECK(base64Decode("QQ", std::span<char>(output.data(), 0)) == std::nullopt);
}

TEST(base64RejectsATrailingGroupCutMidCharacter)
{
    // Six leftover bits cannot form a byte, so the encoding was cut inside one.
    CHECK(decode("Q") == std::nullopt);
    CHECK(decode("QUJDQ") == std::nullopt);

    // Four and two leftover bits are the legal remainders, and both succeed —
    // the boundary is at six rather than at "any remainder", which is the part
    // the prose states and the loop does not show.
    CHECK(decode("QQ") == 1u);
    CHECK(decode("QQQ") == 2u);
    CHECK(decode("QUJDQQ") == 4u);
    CHECK(decode("QUJDQQQ") == 5u);
}

TEST(base64DecodesEmptyInputToAnEmptySpanAndSucceeds)
{
    // "Empty" is an engaged empty span and "failed" is no span at all, so a
    // caller separates them by the result's presence and never by its size. A
    // span of nothing is not a failure when there was nothing to write.
    CHECK(decode("") == 0u);

    std::array<char, 64> output {};
    const auto fromNothing = base64Decode("", std::span<char>(output.data(), 0));
    REQUIRE(fromNothing.has_value());
    CHECK(fromNothing->empty());

    const auto intoRoom = base64Decode("", output);
    REQUIRE(intoRoom.has_value());
    CHECK(intoRoom->empty());
    CHECK(intoRoom->data() == output.data());
}

TEST(base64AnswersNoSpanForAFailedDecode)
{
    // A failure hands back no span, so nothing the decoder wrote before it
    // reached the fault can be read as a result. Each of these puts a whole
    // group ahead of its fault, which the decoder is free to have written:
    // whitespace, which the kernels underneath skip; padding mid-stream; and an
    // output with room for the first group only.
    std::array<char, 64> output {};
    CHECK(base64Decode("QUJD QUJD", output) == std::nullopt);
    CHECK(base64Decode("QUJD\nQUJD", output) == std::nullopt);
    CHECK(base64Decode("QUJDQQ==QUJD", output) == std::nullopt);
    CHECK(base64Decode("QUJDQUJD", std::span<char>(output.data(), 5)) == std::nullopt);

    // The encoder refuses on the same terms, before it writes anything.
    CHECK(base64Encode(std::span<const char>("ABCD", 4), std::span<char>(output.data(), 7)) == std::nullopt);
}

TEST(base64DecodesWhatItAccepts)
{
    // The positive path, so a decoder that rejected everything would fail here
    // rather than pass every rejection case above. The result is a prefix of
    // the caller's own span, which is where its bytes are read from.
    std::array<char, 64> output {};
    const auto written = base64Decode("QUJD", output);
    REQUIRE(written.has_value());
    CHECK(written->data() == output.data());
    CHECK(std::string_view(written->data(), written->size()) == "ABC");

    const auto padded = base64Decode("QUJDRA==", output);
    REQUIRE(padded.has_value());
    CHECK(padded->data() == output.data());
    CHECK(std::string_view(padded->data(), padded->size()) == "ABCD");
}

TEST(base64DecodedCapacityIsExactAtEdgeLengthsAndAtTheLargestPayload)
{
    // Every remainder mod 4, unpadded. A final group of one character is a quad
    // cut short: six bits, no byte, and the decoder refuses it.
    static_assert(base64DecodedCapacity("") == 0);
    static_assert(base64DecodedCapacity("Q") == 0);
    static_assert(base64DecodedCapacity("QQ") == 1);
    static_assert(base64DecodedCapacity("QQQ") == 2);
    static_assert(base64DecodedCapacity("QUJD") == 3);
    static_assert(base64DecodedCapacity("QUJDQ") == 3);
    static_assert(base64DecodedCapacity("QUJDQQ") == 4);
    static_assert(base64DecodedCapacity("QUJDQQQ") == 5);
    static_assert(base64DecodedCapacity("QUJDQUJD") == 6);

    // Padding closes the final group and carries no bits. Only the last two
    // characters are read as padding, so a third `=` counts as a character.
    static_assert(base64DecodedCapacity("QQ==") == 1);
    static_assert(base64DecodedCapacity("QQQ=") == 2);
    static_assert(base64DecodedCapacity("QUJDQQ==") == 4);
    static_assert(base64DecodedCapacity("QUJDQQQ=") == 5);
    static_assert(base64DecodedCapacity("=") == 0);
    static_assert(base64DecodedCapacity("==") == 0);
    static_assert(base64DecodedCapacity("QUJD===") == 3);

    // The largest kitty payload: a transfer accumulates at most
    // `maxAPCCapacity` characters. Each final-group shape there is exact, and
    // the result fits the `int` lengths the image store keeps.
    constexpr auto maxCharacters = static_cast<std::size_t>(Ubiquitty::Kitty::maxAPCCapacity);
    constexpr std::size_t wholeGroups = maxCharacters / 4 * 3;
    static_assert(maxCharacters % 4 == 0);
    std::string largest(maxCharacters, 'A');
    const std::string_view unpadded = largest;
    CHECK_EQ(base64DecodedCapacity(unpadded), wholeGroups);
    CHECK_EQ(base64DecodedCapacity(unpadded.substr(0, maxCharacters - 1)), wholeGroups - 1);
    CHECK_EQ(base64DecodedCapacity(unpadded.substr(0, maxCharacters - 2)), wholeGroups - 2);
    CHECK_EQ(base64DecodedCapacity(unpadded.substr(0, maxCharacters - 3)), wholeGroups - 3);
    largest[maxCharacters - 1] = '=';
    CHECK_EQ(base64DecodedCapacity(largest), wholeGroups - 1);
    largest[maxCharacters - 2] = '=';
    const std::size_t capacity = base64DecodedCapacity(largest);
    CHECK_EQ(capacity, wholeGroups - 2);
    CHECK(capacity <= static_cast<std::size_t>(std::numeric_limits<int>::max()));

    // A capacity that is not a multiple of three sends the decoder's last
    // group down its bounded tail path. It fills the span exactly there too,
    // and writes nothing either side of it.
    std::vector<char> output(capacity + 2, '#');
    const auto decoded = base64Decode(largest, std::span(output).subspan(1, capacity));
    REQUIRE(decoded.has_value());
    CHECK_EQ(decoded->size(), capacity);
    CHECK(std::ranges::all_of(*decoded, [](char byte) { return byte == 0; }));
    CHECK(output.front() == '#');
    CHECK(output.back() == '#');
    std::ranges::fill(output, '#');
    CHECK(base64Decode(largest, std::span(output).subspan(1, capacity - 1)) == std::nullopt);
    CHECK(output.front() == '#');
    CHECK(output[capacity] == '#');
    CHECK(output.back() == '#');
}

TEST(base64DecodedCapacityFitsExactSpansAcrossPaddedAndUnpaddedGroups)
{
    // A span of exactly the capacity decodes and is filled; one byte fewer is
    // refused. Guard bytes either side show the decoder kept to the span.
    struct Encoding {
        std::string_view input;
        std::string_view decoded;
    };
    constexpr std::array<Encoding, 10> encodings { {
        { "", "" },
        { "QQ==", "A" },
        { "QQ", "A" },
        { "QUI=", "AB" },
        { "QUI", "AB" },
        { "QUJD", "ABC" },
        { "QUJDRA==", "ABCD" },
        { "QUJDRA", "ABCD" },
        { "QUJDREU=", "ABCDE" },
        { "QUJDREU", "ABCDE" },
    } };
    for (const auto& encoding : encodings) {
        const std::size_t capacity = base64DecodedCapacity(encoding.input);
        REQUIRE(capacity == encoding.decoded.size());
        std::array<char, 64> output;
        output.fill('#');
        auto destination = std::span(output).subspan(1, capacity);
        const auto written = base64Decode(encoding.input, destination);
        REQUIRE(written.has_value());
        CHECK(written->data() == destination.data());
        CHECK(std::string_view(written->data(), written->size()) == encoding.decoded);
        CHECK(output.front() == '#');
        for (std::size_t index = capacity + 1; index < output.size(); ++index)
            CHECK(output[index] == '#');

        if (!capacity)
            continue;
        output.fill('#');
        destination = std::span(output).subspan(1, capacity - 1);
        CHECK(base64Decode(encoding.input, destination) == std::nullopt);
        CHECK(output.front() == '#');
        for (std::size_t index = capacity; index < output.size(); ++index)
            CHECK(output[index] == '#');
    }
}

TEST(base64DecodedCapacityDoesNotAdmitMalformedInput)
{
    // The capacity is arithmetic on the length, so a malformed input gets one
    // too. The decoder still refuses it into a span of that size, and writes
    // nothing outside the span on the way.
    for (const std::string_view input : { "QQ=A", "QQ==QUJD", "QQ=", "QUJD=", "QUJD==", "====", "=", "Q",
             "QQ A", "QQ\nA", "QQ\tA", " QUJD", "QUJD ", "QQ-A", "QQ_A" }) {
        const std::size_t capacity = base64DecodedCapacity(input);
        std::array<char, 64> output;
        output.fill('#');
        REQUIRE(capacity < output.size() - 1);
        CHECK(base64Decode(input, std::span(output).subspan(1, capacity)) == std::nullopt);
        CHECK(output.front() == '#');
        for (std::size_t index = capacity + 1; index < output.size(); ++index)
            CHECK(output[index] == '#');
    }
}

TEST(base64EncodesWithAWholeFinalGroup)
{
    // Each remainder pads to four, so the character count is a function of the
    // byte count alone — which is what base64EncodedLength promises a caller
    // sizing a buffer, and what a caller writing into a fixed array relies on.
    std::array<char, 64> output {};
    const auto whole = base64Encode(std::span<const char>("ABC", 3), output);
    REQUIRE(whole.has_value());
    CHECK(whole->data() == output.data());
    CHECK(std::string_view(whole->data(), whole->size()) == "QUJD");
    const auto one = base64Encode(std::span<const char>("A", 1), output);
    REQUIRE(one.has_value());
    CHECK(std::string_view(one->data(), one->size()) == "QQ==");
    const auto two = base64Encode(std::span<const char>("AB", 2), output);
    REQUIRE(two.has_value());
    CHECK(std::string_view(two->data(), two->size()) == "QUI=");

    CHECK(base64EncodedLength(0) == 0);
    CHECK(base64EncodedLength(1) == 4);
    CHECK(base64EncodedLength(3) == 4);
    CHECK(base64EncodedLength(4) == 8);

    // Nothing to encode writes nothing and succeeds, on the same terms the
    // decoder answers an empty input.
    CHECK(sizeOf(base64Encode(std::span<const char>(), output)) == 0u);
}

TEST(base64EncodeRefusesASpanTooSmallRatherThanTruncating)
{
    // One short of the whole final group, which is where a truncating encoder
    // would emit an unpadded prefix the decoder then reads as a shorter payload.
    std::array<char, 64> output {};
    CHECK(base64Encode(std::span<const char>("ABCD", 4), std::span<char>(output.data(), 7)) == std::nullopt);
    CHECK(sizeOf(base64Encode(std::span<const char>("ABCD", 4), std::span<char>(output.data(), 8))) == 8u);
}

TEST(base64RoundTripsEveryLengthThroughTheGroupBoundary)
{
    // Both directions against each other, across the three remainders and past
    // them, so an encoder and a decoder that drifted together would still have
    // to drift into agreement with the byte counts.
    std::array<char, 256> encoded {};
    std::array<char, 256> decoded {};
    const char source[] = "The quick brown fox jumps over the lazy dog";
    for (std::size_t length = 0; length <= sizeof(source) - 1; ++length) {
        const auto characters = base64Encode(std::span<const char>(source, length), encoded);
        REQUIRE_MSG(characters.has_value(), source);
        CHECK_MSG(characters->size() == base64EncodedLength(length), source);
        const auto bytes = base64Decode(std::string_view(characters->data(), characters->size()), decoded);
        REQUIRE_MSG(bytes.has_value(), source);
        CHECK_MSG(std::string_view(bytes->data(), bytes->size()) == std::string_view(source, length), source);
    }
}

UBIQUITTY_TEST_MAIN()
