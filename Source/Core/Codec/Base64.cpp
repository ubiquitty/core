// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Source/Core/Codec/Base64.cpp — see Base64.h.

#include "Core/Codec/Base64.h"

#include "simdutf.h"

#include <algorithm>
#include <cstddef>

namespace Ubiquitty::Codec {

namespace {

// `loose` accepts a final group left unpadded and refuses every other departure
// from a whole encoding: padding before the end, padding that does not describe
// the final group, a stray `=` after a whole group.
//
// The unpadded final group is the one leniency, and it is here because real
// producers emit that encoding, which both stricter settings refuse.
// `strict` refuses it alone; `only_full_chunks` refuses it and demands a
// character count divisible by four, which rejects every unpadded payload
// outright.
constexpr simdutf::last_chunk_handling_options lastChunkHandling = simdutf::loose;

// simdutf skips ASCII whitespace at every option setting — MIME base64 permits
// line breaks, and there is no setting that refuses them. This decoder does
// refuse them, for the reason its header gives, so the check lives here.
//
// Every whitespace byte simdutf ignores is below `+`, the lowest character in
// the alphabet, and so is every other byte it would reject anyway. One
// comparison per byte, over a range the compiler vectorises, and it runs only
// after a decode that already succeeded — a malformed payload never reaches it.
//
// A length check would be cheaper and does not work: "QQ A" ignores its space
// and yields two bytes, which is also what the well-formed four-character
// "QQQ=" yields, so the pair (input length, output length) cannot tell them
// apart.
[[nodiscard]] bool hasByteBelowTheAlphabet(std::string_view in)
{
    // A minimum, not a search: the early exit a search wants is a branch the
    // vectoriser will not carry, and the answer is the same.
    unsigned char smallest = 0xFF;
    for (const char character : in)
        smallest = std::min(smallest, static_cast<unsigned char>(character));
    return smallest < static_cast<unsigned char>('+');
}

} // namespace

std::optional<std::span<char>> base64Decode(std::string_view in, std::span<char> output)
{
    // The capacity-carrying entry point rather than the plain one, which writes
    // with no ceiling and would need the caller's span pre-checked against a
    // bound computed from the character count. That bound over-counts whenever
    // the payload is padded, so pre-checking would refuse a span that the actual
    // payload fits — this asks the decoder instead, and it reports what it wrote.
    std::size_t written = output.size();
    const simdutf::result decoded = simdutf::base64_to_binary_safe(
        in.data(), in.size(), output.data(), written, simdutf::base64_default, lastChunkHandling);
    if (decoded.error != simdutf::SUCCESS)
        return std::nullopt;
    if (hasByteBelowTheAlphabet(in))
        return std::nullopt;
    return output.first(written);
}

std::optional<std::span<char>> base64Encode(std::span<const char> in, std::span<char> output)
{
    if (base64EncodedLength(in.size()) > output.size())
        return std::nullopt;
    return output.first(simdutf::binary_to_base64(in.data(), in.size(), output.data(), simdutf::base64_default));
}

} // namespace Ubiquitty::Codec
