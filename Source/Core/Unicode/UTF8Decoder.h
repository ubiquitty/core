// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

// Strict stateful UTF-8 decoder: one byte per utf8Feed, emits a codePoint when
// the sequence completes. State holds the accumulator, the shortest-form lower
// bound and the range the next continuation byte may take (Unicode Table 3-7),
// so an overlong form, a surrogate or a value past U+10FFFF is refused at the
// first byte that makes it ill-formed, which is where its maximal subpart
// ends. No allocation, no exceptions.
// Invalid resets the decoder; a caller mid-sequence should re-feed the
// offending byte if it could itself begin a fresh sequence.

namespace Ubiquitty::Unicode {

struct UTF8Decoder {
    std::uint32_t acc; // partial codePoint accumulator
    std::uint32_t min; // shortest-form lower bound
    std::uint8_t need; // continuation bytes still expected
    std::uint8_t lowestContinuation; // lowest value the next continuation byte may take
    std::uint8_t highestContinuation; // highest value the next continuation byte may take
};

enum class UTF8Status : std::uint8_t {
    Need,
    Emit,
    Invalid,
};

// What the terminal substitutes for an invalid / truncated UTF-8 sequence.
// Value 0 is the default so a zero-initialized Terminal gets FFFD without a setter.
enum class InvalidUTF8Policy : std::uint8_t {
    FFFD = 0, // emit U+FFFD (REPLACEMENT CHARACTER)
    Drop, // skip the bad bytes, emit nothing
    Latin1, // interpret each undecodable byte as Latin-1 / ISO-8859-1
};

// Parse a config token ("fffd" | "drop" | "latin1") into a policy. Returns
// std::nullopt on an unrecognized token, so the caller keeps its default.
// Case- and separator-insensitive, like encodingFromName.
[[nodiscard]] std::optional<InvalidUTF8Policy> invalidUTF8PolicyFromName(std::string_view name);

// Canonical lowercase token for a policy (round-trips invalidUTF8PolicyFromName).
const char* invalidUTF8PolicyToName(InvalidUTF8Policy policy);

/// Reports whether `codePoint` is a Unicode scalar value.
///
/// Excludes anything above U+10FFFF and the UTF-16 surrogate range, which are
/// the two ways a 32-bit field holds something no encoder can represent.
/// `appendUTF8` emits nothing for either, while a UTF-16 length function counts
/// two units — so a producer that skips this check leaves two consumers
/// disagreeing about how long the same cell is.
///
/// One definition, because there were two: the UTF-8 decoder rejected these
/// before building a Cell and the sync wire decoder did not, so which rule
/// applied depended on whether the text arrived from a local pty or a peer.
///
/// - Parameter codePoint: The value to test.
/// - Returns: True when the value is a scalar an encoder can represent.
[[nodiscard]] constexpr bool isScalarValue(std::uint32_t codePoint)
{
    return codePoint <= 0x10FFFF && !(codePoint >= 0xD800 && codePoint <= 0xDFFF);
}

void utf8Reset(UTF8Decoder* d);
UTF8Status utf8Feed(UTF8Decoder* d, std::uint8_t byte, std::uint32_t* outCodePoint);

/// Batch-decodes a run of complete multi-byte UTF-8 sequences.
///
/// Decodes leads >= 0xC2 into `out`, stopping at the first ASCII, control or
/// continuation byte, an incomplete trailing sequence, an invalid sequence, or
/// `out`'s capacity. May return fewer code points than fit with decodable
/// input remaining, so every caller loops. Accept/reject (overlong, surrogate,
/// beyond U+10FFFF) matches `utf8Feed` exactly.
///
/// The input is octets, not text: a NUL is a byte of the payload, which is why
/// the parameter is a span and not the char* that would quietly end at one.
///
/// - Parameters:
///   - in: The bytes to decode.
///   - out: Receives the decoded code points; its size bounds the run.
///   - bytesConsumed: Receives the count of input bytes the returned code
///     points consumed.
/// - Returns: The number of code points written to `out`.
int utf8DecodeRun(std::span<const char> in, std::span<std::uint32_t> out, int* bytesConsumed);

#if UBIQUITTY_ENABLE_TEST_HOOKS
/// How many runs the vectorised converter has answered.
///
/// Zero in a build without it, so a parity case can tell "the two paths agree"
/// from "there is only one path here".
extern std::uint64_t simdRunCountForTest;

/// How many bytes run decodes have consumed, on either path.
///
/// A batched writer that decodes ahead of what it places can be held to
/// decoding each byte once by comparing this with the bytes it was given.
extern std::uint64_t decodedBytesForTest;
#endif

} // namespace Ubiquitty::Unicode
