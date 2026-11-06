// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

#include "Core/Unicode/UTF8Decoder.h"
#include "Core/Unicode/UTF8Encoder.h"
#include <cstdint>
#include <cstdio>
#include <string>

using Ubiquitty::Unicode::appendUTF8;
using Ubiquitty::Unicode::UTF8Decoder;
using Ubiquitty::Unicode::utf8Feed;
using Ubiquitty::Unicode::utf8Reset;
using Ubiquitty::Unicode::UTF8Status;

// Encode one scalar to a fresh string and return it.
static std::string encode(std::uint32_t codePoint)
{
    std::string outputBuffer;
    appendUTF8(outputBuffer, codePoint);
    return outputBuffer;
}

// Feed a UTF-8 string into the strict decoder and assert it yields exactly the
// one expected scalar — the encoder's output is well-formed iff the decoder
// accepts it and round-trips the value.
static std::uint32_t decodeSingle(const std::string& bytes, bool* ok)
{
    UTF8Decoder d;
    utf8Reset(&d);
    std::uint32_t outputBuffer = 0xFFFFFFFFu;
    int emitted = 0;
    for (unsigned char byte : bytes) {
        std::uint32_t codePoint = 0;
        UTF8Status st = utf8Feed(&d, byte, &codePoint);
        if (st == UTF8Status::Emit) {
            outputBuffer = codePoint;
            emitted++;
        } else if (st == UTF8Status::Invalid) {
            *ok = false;
            return 0;
        }
    }
    *ok = (emitted == 1);
    return outputBuffer;
}

TEST(testByteLengths)
{
    CHECK(encode(0x00).size() == 1); // NUL
    CHECK(encode(0x41).size() == 1); // 'A'
    CHECK(encode(0x7F).size() == 1); // last ASCII
    CHECK(encode(0x80).size() == 2); // first 2-byte
    CHECK(encode(0x7FF).size() == 2); // last 2-byte
    CHECK(encode(0x800).size() == 3); // first 3-byte
    CHECK(encode(0xFFFF).size() == 3); // last BMP
    CHECK(encode(0x10000).size() == 4); // first astral
    CHECK(encode(0x10FFFF).size() == 4); // last legal scalar
}

TEST(testKnownSequences)
{
    // U+00A9 © -> C2 A9
    CHECK(encode(0x00A9) == std::string("\xC2\xA9", 2));
    // U+20AC € -> E2 82 AC
    CHECK(encode(0x20AC) == std::string("\xE2\x82\xAC", 3));
    // U+1F600 😀 -> F0 9F 98 80 (astral / emoji)
    CHECK(encode(0x1F600) == std::string("\xF0\x9F\x98\x80", 4));
    // U+10000 (first astral) -> F0 90 80 80
    CHECK(encode(0x10000) == std::string("\xF0\x90\x80\x80", 4));
    // U+10FFFF (last scalar) -> F4 8F BF BF
    CHECK(encode(0x10FFFF) == std::string("\xF4\x8F\xBF\xBF", 4));
}

// Every scalar across the BMP and a stride through the astral planes must
// survive an encode → strict-decode round trip. The astral stride is the
// regression guard against a BMP-only encoder that truncates >= U+10000.
TEST(testRoundTripBMPAndAstral)
{
    for (std::uint32_t codePoint = 0; codePoint <= 0xFFFF; ++codePoint) {
        // Surrogate halves are not Unicode scalars — the strict decoder rejects
        // their 3-byte form, and selection sources never emit them. Skip.
        if (codePoint >= 0xD800 && codePoint <= 0xDFFF)
            continue;
        bool ok = false;
        std::uint32_t got = decodeSingle(encode(codePoint), &ok);
        CHECK(ok);
        CHECK(got == codePoint);
    }
    for (std::uint32_t codePoint = 0x10000; codePoint <= 0x10FFFF; codePoint += 0x111) {
        bool ok = false;
        std::uint32_t got = decodeSingle(encode(codePoint), &ok);
        CHECK(ok);
        CHECK(got == codePoint);
    }
    // Explicit emoji + astral anchors.
    for (std::uint32_t codePoint : { 0x1F600u, 0x1F4A9u, 0x1F680u, 0x20000u, 0x2F800u, 0x10FFFFu }) {
        bool ok = false;
        std::uint32_t got = decodeSingle(encode(codePoint), &ok);
        CHECK(ok);
        CHECK(got == codePoint);
    }
}

TEST(testOutOfRangeDropped)
{
    // Beyond U+10FFFF is not a Unicode scalar — emit nothing.
    CHECK(encode(0x10FFFF).size() == 4); // last legal scalar still encodes
    CHECK(encode(0x110000).empty()); // first out-of-range value: dropped
    CHECK(encode(0x140000).empty());
    CHECK(encode(0xFFFFFFFFu).empty());
    // Regression guard: a now-removed file-local copy in AgentSession.cpp had an
    // unconditional final `else` (no <= 0x10FFFF guard), so it would emit a stray
    // 4-byte sequence for values just past the legal range. The shared encoder all
    // those callers route through drops them — pin the boundary so the divergence
    // can't creep back.
    CHECK(encode(0x110000).empty());
}

TEST(testAppendAccumulates)
{
    // Mixed ASCII + 2/3/4-byte runs append in order with no separators.
    std::string outputBuffer;
    appendUTF8(outputBuffer, 'h');
    appendUTF8(outputBuffer, 'i');
    appendUTF8(outputBuffer, 0x00A9); // ©
    appendUTF8(outputBuffer, 0x1F600); // 😀
    CHECK(outputBuffer == std::string("hi\xC2\xA9\xF0\x9F\x98\x80"));
}

TEST(testFixedBufferForm)
{
    // The raw-buffer overload writes the same bytes and advances *length.
    char buffer[16];
    std::size_t length = 0;
    appendUTF8(buffer, &length, 'A');
    appendUTF8(buffer, &length, 0x1F600);
    CHECK(length == 5);
    CHECK(std::string(buffer, length) == std::string("A\xF0\x9F\x98\x80"));

    // Out-of-range writes nothing and leaves *length untouched.
    std::size_t before = length;
    appendUTF8(buffer, &length, 0x110000);
    CHECK(length == before);
}

UBIQUITTY_TEST_MAIN()
