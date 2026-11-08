// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Source/Core/Codec/Base64.cpp — see Base64.h.

#include "Core/Codec/Base64.h"

#include "simdutf.h"

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

} // namespace

std::optional<std::span<char>> base64Decode(std::string_view in, std::span<char> output)
{
    // A single remainder cannot encode a byte. Reject it before checking the
    // decoded length: otherwise "QUJD " and "QUJD" both predict three bytes.
    if (in.size() % 4 == 1)
        return std::nullopt;
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
    // simdutf validates the alphabet and padding but skips ASCII whitespace.
    // At the remaining lengths, any skipped byte reduces the decoded size.
    // Count the actual trailing padding too: "QQ A" predicts three bytes,
    // whereas "QQQ=" predicts two. This needs no second scan of the payload.
    if (written != base64DecodedCapacity(in))
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
