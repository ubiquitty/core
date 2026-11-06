// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

#include "Core/Unicode/UTF8Decoder.h"
#include <cstdint>
#include <cstdio>
#include <span>

using Ubiquitty::Unicode::UTF8Decoder;
using Ubiquitty::Unicode::utf8Feed;
using Ubiquitty::Unicode::utf8Reset;
using Ubiquitty::Unicode::UTF8Status;

static UTF8Status feedSeq(UTF8Decoder* d, const std::uint8_t* bytes, int n, std::uint32_t* codePoint)
{
    UTF8Status st = UTF8Status::Need;
    for (int i = 0; i < n; i++) {
        st = utf8Feed(d, bytes[i], codePoint);
        if (st != UTF8Status::Need && i + 1 < n)
            return st;
    }
    return st;
}

TEST(testASCIIPassthrough)
{
    UTF8Decoder d;
    utf8Reset(&d);
    for (int b = 0; b < 0x80; b++) {
        std::uint32_t codePoint = 0xFFFF;
        UTF8Status st = utf8Feed(&d, static_cast<std::uint8_t>(b), &codePoint);
        CHECK(st == UTF8Status::Emit);
        CHECK(codePoint == static_cast<std::uint32_t>(b));
    }
}

TEST(test2ByteWellFormed)
{
    UTF8Decoder d;
    utf8Reset(&d);
    /// U+00A9 © -> C2 A9
    const std::uint8_t seq[] = { 0xC2, 0xA9 };
    std::uint32_t codePoint = 0;
    CHECK(feedSeq(&d, seq, 2, &codePoint) == UTF8Status::Emit);
    CHECK(codePoint == 0x00A9);
}

TEST(testRunTwoByteInputsMatchStreamingDecoder)
{
    for (unsigned first = 0; first < 256; ++first) {
        for (unsigned second = 0; second < 256; ++second) {
            UTF8Decoder decoder;
            utf8Reset(&decoder);
            std::uint32_t expected = 0;
            const auto firstStatus = utf8Feed(&decoder, static_cast<std::uint8_t>(first), &expected);
            const bool isPairComplete = firstStatus == UTF8Status::Need
                && utf8Feed(&decoder, static_cast<std::uint8_t>(second), &expected) == UTF8Status::Emit;
            // A valid prefix makes a rejected pair's exact stopping point
            // observable, including ASCII, malformed, and truncated tails.
            const char input[] = { '\xc2', '\xa9', static_cast<char>(first), static_cast<char>(second) };
            for (std::size_t capacity = 0; capacity <= 2; ++capacity) {
                constexpr std::uint32_t untouched = 0xFFFFFFFF;
                std::uint32_t output[] = { untouched, untouched, untouched };
                int consumed = -1;
                const int count = Ubiquitty::Unicode::utf8DecodeRun(input, std::span { output, capacity }, &consumed);
                const int expectedCount = capacity == 0 ? 0 : (capacity == 2 && isPairComplete ? 2 : 1);
                CHECK(count == expectedCount);
                CHECK(consumed == expectedCount * 2);
                CHECK(output[0] == (capacity == 0 ? untouched : 0xA9));
                CHECK(output[1] == (expectedCount == 2 ? expected : untouched));
                CHECK(output[2] == untouched);
            }
        }
    }
}

TEST(test3ByteWellFormed)
{
    UTF8Decoder d;
    utf8Reset(&d);
    /// U+20AC € -> E2 82 AC
    const std::uint8_t seq[] = { 0xE2, 0x82, 0xAC };
    std::uint32_t codePoint = 0;
    CHECK(feedSeq(&d, seq, 3, &codePoint) == UTF8Status::Emit);
    CHECK(codePoint == 0x20AC);
}

TEST(test4ByteWellFormed)
{
    UTF8Decoder d;
    utf8Reset(&d);
    /// U+1F600 grinning face -> F0 9F 98 80
    const std::uint8_t seq[] = { 0xF0, 0x9F, 0x98, 0x80 };
    std::uint32_t codePoint = 0;
    CHECK(feedSeq(&d, seq, 4, &codePoint) == UTF8Status::Emit);
    CHECK(codePoint == 0x1F600);
}

TEST(testOverlongRejected)
{
    UTF8Decoder d;
    // Overlong 2-byte for '/': C0 AF — lead C0 is below C2 so it fails
    // at the lead byte.
    utf8Reset(&d);
    std::uint32_t codePoint = 0;
    CHECK(utf8Feed(&d, 0xC0, &codePoint) == UTF8Status::Invalid);

    // Overlong 3-byte for '/': E0 80 AF — lead E0 accepted, sequence
    // completes but codePoint < min.
    utf8Reset(&d);
    const std::uint8_t over3[] = { 0xE0, 0x80, 0xAF };
    CHECK(feedSeq(&d, over3, 3, &codePoint) == UTF8Status::Invalid);

    // Overlong 4-byte for '/': F0 80 80 AF.
    utf8Reset(&d);
    const std::uint8_t over4[] = { 0xF0, 0x80, 0x80, 0xAF };
    CHECK(feedSeq(&d, over4, 4, &codePoint) == UTF8Status::Invalid);
}

TEST(testSurrogateRejected)
{
    UTF8Decoder d;
    utf8Reset(&d);
    /// U+D800 encoded as ED A0 80.
    const std::uint8_t seq[] = { 0xED, 0xA0, 0x80 };
    std::uint32_t codePoint = 0;
    CHECK(feedSeq(&d, seq, 3, &codePoint) == UTF8Status::Invalid);

    utf8Reset(&d);
    /// U+DFFF encoded as ED BF BF.
    const std::uint8_t seq2[] = { 0xED, 0xBF, 0xBF };
    CHECK(feedSeq(&d, seq2, 3, &codePoint) == UTF8Status::Invalid);
}

TEST(testAboveMaxRejected)
{
    UTF8Decoder d;
    utf8Reset(&d);
    /// U+110000 encoded as F4 90 80 80 — past 0x10FFFF. Lead F4 is
    /// accepted; sequence completes; codePoint > 0x10FFFF rejected.
    const std::uint8_t seq[] = { 0xF4, 0x90, 0x80, 0x80 };
    std::uint32_t codePoint = 0;
    CHECK(feedSeq(&d, seq, 4, &codePoint) == UTF8Status::Invalid);

    // F5/F6/F7 lead is itself invalid.
    utf8Reset(&d);
    CHECK(utf8Feed(&d, 0xF5, &codePoint) == UTF8Status::Invalid);
}

// The second byte of E0, ED, F0 and F4 is bounded, so the sequence is refused at
// that byte and the decoder is free to read the byte as the start of the next
// character: Unicode Table 3-7.
TEST(testRestrictedLeadsRefuseTheirSecondByte)
{
    struct Case {
        std::uint8_t lead;
        std::uint8_t refused;
        std::uint8_t accepted;
    };
    const Case cases[] = { { 0xE0, 0x9F, 0xA0 }, { 0xED, 0xA0, 0x9F }, { 0xF0, 0x8F, 0x90 }, { 0xF4, 0x90, 0x8F } };
    for (const Case& c : cases) {
        UTF8Decoder d;
        utf8Reset(&d);
        std::uint32_t codePoint = 0;
        CHECK(utf8Feed(&d, c.lead, &codePoint) == UTF8Status::Need);
        CHECK(utf8Feed(&d, c.refused, &codePoint) == UTF8Status::Invalid);
        CHECK(d.need == 0);
        CHECK(utf8Feed(&d, c.lead, &codePoint) == UTF8Status::Need);
        CHECK(utf8Feed(&d, c.accepted, &codePoint) == UTF8Status::Need);
    }
}

TEST(testLoneContinuation)
{
    UTF8Decoder d;
    utf8Reset(&d);
    std::uint32_t codePoint = 0;
    CHECK(utf8Feed(&d, 0x80, &codePoint) == UTF8Status::Invalid);
    CHECK(utf8Feed(&d, 0xBF, &codePoint) == UTF8Status::Invalid);
}

TEST(testTruncatedRecover)
{
    UTF8Decoder d;
    utf8Reset(&d);
    std::uint32_t codePoint = 0;
    // Start a 3-byte sequence then send ASCII.
    CHECK(utf8Feed(&d, 0xE2, &codePoint) == UTF8Status::Need);
    // ASCII 'A' breaks the continuation — Invalid + decoder reset.
    CHECK(utf8Feed(&d, 0x41, &codePoint) == UTF8Status::Invalid);
    // The follow-up byte after recovery starts fresh.
    CHECK(utf8Feed(&d, 0x41, &codePoint) == UTF8Status::Emit);
    CHECK(codePoint == 0x41);
}

TEST(testResetClears)
{
    UTF8Decoder d;
    utf8Reset(&d);
    std::uint32_t codePoint = 0;
    CHECK(utf8Feed(&d, 0xE2, &codePoint) == UTF8Status::Need);
    utf8Reset(&d);
    CHECK(d.need == 0);
    CHECK(utf8Feed(&d, 0x41, &codePoint) == UTF8Status::Emit);
    CHECK(codePoint == 0x41);
}

// The policy token, both directions. It is a settings key, so it arrives as a
// word a user typed and leaves as a word written back to their file — a
// name-and-integer round trip, the shape in which config enums go wrong.
TEST(testEveryPolicyNameRoundTripsAndAnUnknownOneIsRefused)
{
    using Ubiquitty::Unicode::InvalidUTF8Policy;
    using Ubiquitty::Unicode::invalidUTF8PolicyFromName;
    using Ubiquitty::Unicode::invalidUTF8PolicyToName;

    // Every policy's canonical name reads back as itself. Driven from the enum
    // rather than a list of pairs, so a policy added without a name fails here.
    for (InvalidUTF8Policy policy : { InvalidUTF8Policy::FFFD, InvalidUTF8Policy::Drop,
             InvalidUTF8Policy::Latin1 }) {
        const char* name = invalidUTF8PolicyToName(policy);
        CHECK(name != nullptr);
        auto back = invalidUTF8PolicyFromName(name);
        CHECK_MSG(back.has_value(), name);
        CHECK_MSG(*back == policy, name);
    }

    // The three names are distinct, so the round trip above is not three
    // spellings resolving to one policy.
    CHECK(std::strcmp(invalidUTF8PolicyToName(InvalidUTF8Policy::FFFD), "fffd") == 0);
    CHECK(std::strcmp(invalidUTF8PolicyToName(InvalidUTF8Policy::Drop), "drop") == 0);
    CHECK(std::strcmp(invalidUTF8PolicyToName(InvalidUTF8Policy::Latin1), "latin1") == 0);

    // Each policy's second accepted spelling, which the writer never produces
    // and so no round trip reaches.
    auto got = invalidUTF8PolicyFromName("replacement");
    CHECK(got.has_value() && *got == InvalidUTF8Policy::FFFD);
    got = invalidUTF8PolicyFromName("skip");
    CHECK(got.has_value() && *got == InvalidUTF8Policy::Drop);
    got = invalidUTF8PolicyFromName("iso88591");
    CHECK(got.has_value() && *got == InvalidUTF8Policy::Latin1);

    // Case and separators are ignored, which is what lets a user write the name
    // the way it reads rather than the way it is stored.
    for (const char* spelling : { "FFFD", "F-F-F-D", "f_f.f d", "Fffd" }) {
        got = invalidUTF8PolicyFromName(spelling);
        CHECK_MSG(got.has_value() && *got == InvalidUTF8Policy::FFFD, spelling);
    }
    got = invalidUTF8PolicyFromName("ISO-8859-1");
    CHECK(got.has_value() && *got == InvalidUTF8Policy::Latin1);
    got = invalidUTF8PolicyFromName("Latin_1");
    CHECK(got.has_value() && *got == InvalidUTF8Policy::Latin1);

    // A policy value the enum does not name answers the replacement character,
    // which is the safe reading: the field is a byte a config file and the sync
    // wire both write, and substituting U+FFFD is the choice that shows the user
    // something went wrong rather than dropping bytes or reinterpreting them.
    const char* unknownName = invalidUTF8PolicyToName(static_cast<InvalidUTF8Policy>(99));
    CHECK(std::strcmp(unknownName, "fffd") == 0);
    // And it resolves back to the named policy rather than staying unknown.
    auto resolved = invalidUTF8PolicyFromName(unknownName);
    CHECK(resolved.has_value() && *resolved == InvalidUTF8Policy::FFFD);

    // A name no policy carries is refused and the caller's value is left alone,
    // so a typo keeps whatever was configured rather than changing it.
    for (const char* unknown : { "", "-", "___", "fff", "fffdd", "dropp", "dro",
             "latin", "latin11", "replacementt", "utf8", "none" }) {
        auto result = invalidUTF8PolicyFromName(unknown);
        CHECK_MSG(!result.has_value(), unknown);
    }
}

namespace {

/// Decodes one run and reports what it took and produced.
struct Run {
    int count;
    int consumed;
    std::uint32_t first;
};

Run decodeRun(const char* in, int length, int maxOut = 64)
{
    std::uint32_t out[64] = {};
    int consumed = -1;
    const int n = Ubiquitty::Unicode::utf8DecodeRun(std::span { in, static_cast<std::size_t>(length) }, std::span { out, static_cast<std::size_t>(maxOut) }, &consumed);
    return { n, consumed, n > 0 ? out[0] : 0u };
}

} // namespace

TEST(testTheRunDecoderTakesWholeSequencesAndStopsExactly)
{
    // Every multibyte width, and the byte count each contributes — a run that
    // reported the wrong consumed count would leave the caller re-reading or
    // skipping bytes, and neither shows up in the code points alone.
    const Run two = decodeRun("\xC3\xA9\xC3\xA9", 4);
    CHECK(two.count == 2 && two.consumed == 4 && two.first == 0xE9);
    const Run three = decodeRun("\xE4\xB8\x80\xE4\xB8\x80", 6);
    CHECK(three.count == 2 && three.consumed == 6 && three.first == 0x4E00);
    const Run four = decodeRun("\xF0\x9F\x98\x80\xF0\x9F\x98\x80", 8);
    CHECK(four.count == 2 && four.consumed == 8 && four.first == 0x1F600);

    // A run of mixed widths, so the per-width arms are reached in one pass the
    // way real text arrives.
    const Run mixed = decodeRun("\xC3\xA9\xE4\xB8\x80\xF0\x9F\x98\x80", 9);
    CHECK(mixed.count == 3 && mixed.consumed == 9);

    // It stops before anything below 0xC2 rather than consuming it: ASCII, a
    // control byte, a stray continuation, and the two overlong leads.
    for (const char* stopper : { "A", "\x07", "\x80", "\xBF", "\xC0", "\xC1" }) {
        std::string text = "\xC3\xA9";
        text += stopper;
        text += "\xC3\xA9";
        const Run run = decodeRun(text.data(), static_cast<int>(text.size()));
        CHECK_MSG(run.count == 1, stopper);
        CHECK_MSG(run.consumed == 2, stopper);
    }

    // Nothing at all, and a first byte it will not start on, are both an empty
    // run rather than a negative count.
    const Run empty = decodeRun("", 0);
    CHECK(empty.count == 0 && empty.consumed == 0);
    const Run ascii = decodeRun("plain text", 10);
    CHECK(ascii.count == 0 && ascii.consumed == 0);
}

TEST(testTheRunDecoderStopsAtTheFirstThingItCannotDecode)
{
    // Each of these is a byte sequence a hostile or truncated stream produces,
    // and each has to stop the run exactly where the good bytes end — the
    // consumed count is what the caller uses to resume.
    struct Case {
        const char* name;
        std::string text;
        int expectedCount;
        int expectedConsumed;
    };
    const std::string good = "\xC3\xA9"; // 2 bytes, 1 code point

    const Case cases[] = {
        // A five-byte lead form, which UTF-8 has never had.
        { "five-byte lead", good + "\xF8\x88\x80\x80\x80", 1, 2 },
        // Past the last plane: F5..FF lead.
        { "lead past U+10FFFF", good + "\xF5\x80\x80\x80", 1, 2 },
        // A lead with a byte that is not a continuation after it.
        { "broken continuation", good + "\xE4\x41\x80", 1, 2 },
        { "broken continuation late", good + "\xF0\x9F\x41\x80", 1, 2 },
        // Overlong: a code point encoded in more bytes than it needs.
        { "overlong two-byte", good + "\xE0\x80\xAF", 1, 2 },
        { "overlong four-byte", good + "\xF0\x80\x80\xAF", 1, 2 },
        // A surrogate half, which no encoder may produce.
        { "surrogate", good + "\xED\xA0\x80", 1, 2 },
        // Above the last code point.
        { "above the last plane", good + "\xF4\x90\x80\x80", 1, 2 },
        // Cut short by the end of the buffer rather than by a bad byte.
        { "truncated two-byte", good + "\xC3", 1, 2 },
        { "truncated three-byte", good + "\xE4\xB8", 1, 2 },
        { "truncated four-byte", good + "\xF0\x9F\x98", 1, 2 },
    };
    for (const Case& c : cases) {
        const Run run = decodeRun(c.text.data(), static_cast<int>(c.text.size()));
        CHECK_MSG(run.count == c.expectedCount, c.name);
        CHECK_MSG(run.consumed == c.expectedConsumed, c.name);
    }

    // The boundary values that are accepted, so the refusals above are about
    // the sequences rather than about anything unusual being rejected.
    CHECK(decodeRun("\xC2\x80", 2).first == 0x80); // the shortest two-byte form
    CHECK(decodeRun("\xE0\xA0\x80", 3).first == 0x800); // the shortest three-byte
    CHECK(decodeRun("\xF0\x90\x80\x80", 4).first == 0x10000); // the shortest four-byte
    CHECK(decodeRun("\xEF\xBF\xBD", 3).first == 0xFFFD);
    CHECK(decodeRun("\xF4\x8F\xBF\xBF", 4).first == 0x10FFFF); // the last code point
    CHECK(decodeRun("\xED\x9F\xBF", 3).first == 0xD7FF); // just below the surrogates
    CHECK(decodeRun("\xEE\x80\x80", 3).first == 0xE000); // just above them
}

TEST(testTheRunDecoderFillsNoMoreThanItWasGiven)
{
    // The caller's buffer is fixed and the input is not, so stopping at the
    // capacity is the normal case rather than an edge — and the consumed count
    // has to describe only what was written.
    const char* many = "\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9"; // five
    const Run bounded = decodeRun(many, 10, 3);
    CHECK(bounded.count == 3);
    CHECK_MSG(bounded.consumed == 6, "only the bytes behind the code points written");

    const Run none = decodeRun(many, 10, 0);
    CHECK_MSG(none.count == 0 && none.consumed == 0, "no room means nothing taken");

    const Run all = decodeRun(many, 10, 5);
    CHECK(all.count == 5 && all.consumed == 10);
}

UBIQUITTY_TEST_MAIN()
