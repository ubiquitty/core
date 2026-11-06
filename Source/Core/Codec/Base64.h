// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
//
// Base64.h — the standard base64 alphabet, shared by every path that takes an
// encoded payload off the wire or puts one on it.
//
// Both directions run on the vendored simdutf kernels, which every target that
// compiles portable Core now links. One implementation rather than one per
// caller: these decode clipboard payloads a remote program writes and pixels a
// remote program draws, so a platform that decodes differently is a divergence
// in the attack surface itself.
//
// Strict on malformed input rather than lenient, because the callers are
// attacker-reachable. A decoder that skips what it cannot read hands its caller
// a shorter buffer than the one the sender described, and nothing downstream
// can tell that from a small image.
//
// Whitespace is rejected along with every other non-alphabet byte. MIME base64
// permits line breaks; the escape sequences these serve do not, and accepting
// them here would decode a payload the terminal's own length accounting had
// already refused.

#include <cstddef>
#include <limits>
#include <optional>
#include <span>
#include <string_view>

namespace Ubiquitty::Codec {

/// Decodes base64 characters into `output`.
///
/// The standard alphabet only — `+` and `/`, not the URL-safe `-` and `_`. A
/// caller that needs the URL-safe variant needs a second function rather than a
/// flag here, because the two alphabets disagree about bytes that are legal in
/// both.
///
/// Fails rather than returning a partial result on every malformed input: a
/// byte outside the alphabet, padding anywhere but at the end, padding that
/// does not describe the final group, or an output span too small for the
/// payload. A final group left unpadded still decodes, which is the one
/// leniency here and the one every base64 producer relies on.
///
/// A failure leaves the contents of `output` unspecified, since the decoder may
/// have written part of the payload before it reached the fault, and returns no
/// span into them.
///
/// Empty input decodes to an empty span and succeeds, so "empty" and "failed"
/// are an engaged and a disengaged result rather than two readings of a count.
///
/// - Parameters:
///   - in: The encoded characters. A view, so the count travels with the bytes
///     and cannot be given separately from them.
///   - output: Where the decoded bytes go. `base64DecodedCapacity` sizes it
///     exactly; a smaller span fails rather than truncating.
/// - Returns: The prefix of `output` holding the decoded bytes, or
///   `std::nullopt` on malformed input or an `output` too small for it.
[[nodiscard]] std::optional<std::span<char>> base64Decode(std::string_view in, std::span<char> output);

/// The output capacity `base64Decode` needs for `in`, exact for every input it
/// accepts.
///
/// Reads the length and at most the last two characters. Up to two trailing
/// `=` close the final group and carry no bits; a final group of two or three
/// characters yields one or two bytes; a final group of one character cannot
/// form a byte and adds none. A successful decode into a span of this size
/// therefore fills it, and a caller can keep that buffer as the result rather
/// than copy out of it.
///
/// Not a validator: an input the decoder refuses gets a capacity too, and the
/// decoder still refuses it into a span of that size.
[[nodiscard]] constexpr std::size_t base64DecodedCapacity(std::string_view in)
{
    std::size_t characters = in.size();
    for (int padding = 0; padding < 2 && characters && in[characters - 1] == '='; ++padding)
        --characters;
    const std::size_t finalGroup = characters % 4;
    return characters / 4 * 3 + (finalGroup > 1 ? finalGroup - 1 : 0);
}

/// Encodes `in` into `output`, padded to a whole final group.
///
/// Empty input encodes to an empty span and succeeds.
///
/// - Parameters:
///   - in: The bytes to encode.
///   - output: Where the characters go, with no terminator written.
///     `base64EncodedLength` sizes it; a smaller span fails rather than
///     truncating, and writes nothing.
/// - Returns: The prefix of `output` holding the characters, or `std::nullopt`
///   when `output` is too small.
[[nodiscard]] std::optional<std::span<char>> base64Encode(std::span<const char> in, std::span<char> output);

/// The exact character count `base64Encode` writes for `byteCount` bytes.
///
/// Saturates to `numeric_limits<size_t>::max()` instead of wrapping when
/// `byteCount` is within 2 of `SIZE_MAX` or the quadrupled group count would
/// overflow: such a length can never name a real span, and the saturated
/// result fails every caller size check instead of under-sizing its output.
[[nodiscard]] constexpr std::size_t base64EncodedLength(std::size_t byteCount)
{
    constexpr std::size_t max = std::numeric_limits<std::size_t>::max();
    if (byteCount > max - 2)
        return max;
    const std::size_t groups = (byteCount + 2) / 3;
    if (groups > max / 4)
        return max;
    return groups * 4;
}

} // namespace Ubiquitty::Codec
