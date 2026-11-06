// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

#include "Core/Unicode/Encodings.h"

#include <cstdint>
#include <cstdio>
#include <vector>

using namespace Ubiquitty::Unicode;

// Feed a byte run through a decoder; collect every emitted codePoint.
// Invalid sequences contribute U+FFFD, with a re-feed of the offending
// byte after a mid-sequence break (mirrors the production transcode).
static std::vector<std::uint32_t> decodeAll(Encoding enc, const std::uint8_t* in, int n)
{
    EncDecoder d;
    encReset(&d, enc);
    std::vector<std::uint32_t> outputBuffer;
    for (int i = 0; i < n; ++i) {
        bool wasMid = (d.need > 0);
        std::uint32_t codePoint;
        EncStatus st = encFeed(&d, in[i], &codePoint);
        if (st == EncStatus::Emit) {
            outputBuffer.push_back(codePoint);
        } else if (st == EncStatus::Invalid) {
            outputBuffer.push_back(0xFFFD);
            if (wasMid) {
                std::uint32_t cp2;
                EncStatus st2 = encFeed(&d, in[i], &cp2);
                if (st2 == EncStatus::Emit)
                    outputBuffer.push_back(cp2);
                else if (st2 == EncStatus::Invalid)
                    outputBuffer.push_back(0xFFFD);
            }
        }
    }
    return outputBuffer;
}

// ---- name resolution ----
TEST(testNameResolution)
{
    auto e = encodingFromName("utf-8");
    CHECK(e && *e == Encoding::UTF8);
    e = encodingFromName("UTF8");
    CHECK(e && *e == Encoding::UTF8);
    e = encodingFromName("latin1");
    CHECK(e && *e == Encoding::Latin1);
    e = encodingFromName("ISO-8859-1");
    CHECK(e && *e == Encoding::Latin1);
    e = encodingFromName("iso_8859.15");
    CHECK(e && *e == Encoding::Iso8859_15);
    e = encodingFromName("Latin9");
    CHECK(e && *e == Encoding::Iso8859_15);
    e = encodingFromName("KOI8-R");
    CHECK(e && *e == Encoding::Koi8R);
    e = encodingFromName("windows-1252");
    CHECK(e && *e == Encoding::Windows1252);
    e = encodingFromName("CP1252");
    CHECK(e && *e == Encoding::Windows1252);
    e = encodingFromName("Shift_JIS");
    CHECK(e && *e == Encoding::ShiftJis);
    e = encodingFromName("sjis");
    CHECK(e && *e == Encoding::ShiftJis);
    e = encodingFromName("EUC-JP");
    CHECK(e && *e == Encoding::EucJp);
    e = encodingFromName("euc-kr");
    CHECK(e && *e == Encoding::EucKr);
    e = encodingFromName("cp949");
    CHECK(e && *e == Encoding::EucKr);
    e = encodingFromName("GB18030");
    CHECK(e && *e == Encoding::Gb18030);
    e = encodingFromName("gbk");
    CHECK(e && *e == Encoding::Gb18030);
    e = encodingFromName("Big5");
    CHECK(e && *e == Encoding::Big5);
    CHECK(!encodingFromName("not-a-charset"));
    CHECK(!encodingFromName(""));
}

// ---- single-byte exactness + round-trip ----
TEST(testSingleByteRoundTrip)
{
    // Latin-1: byte == codePoint over the whole 0x00..0xFF range.
    for (int b = 0; b < 0x100; ++b) {
        std::uint8_t by = static_cast<std::uint8_t>(b);
        auto v = decodeAll(Encoding::Latin1, &by, 1);
        CHECK(v.size() == 1 && v[0] == static_cast<std::uint32_t>(b));
    }
    // ISO-8859-15 vs Latin-1: the eight differing slots.
    struct {
        std::uint8_t byte;
        std::uint32_t codePoint;
    } l9[] = {
        { 0xA4, 0x20AC },
        { 0xA6, 0x0160 },
        { 0xA8, 0x0161 },
        { 0xB4, 0x017D },
        { 0xB8, 0x017E },
        { 0xBC, 0x0152 },
        { 0xBD, 0x0153 },
        { 0xBE, 0x0178 },
    };
    for (auto& t : l9) {
        auto v = decodeAll(Encoding::Iso8859_15, &t.byte, 1);
        CHECK(v.size() == 1 && v[0] == t.codePoint);
    }
    // KOI8-R: a few anchors.
    struct {
        std::uint8_t byte;
        std::uint32_t codePoint;
    } koi[] = {
        { 0xE1, 0x0410 }, // А
        { 0xC1, 0x0430 }, // а
        { 0xA3, 0x0451 }, // ё
        { 0xB3, 0x0401 }, // Ё
        { 0x80, 0x2500 }, // box drawing ─
    };
    for (auto& t : koi) {
        auto v = decodeAll(Encoding::Koi8R, &t.byte, 1);
        CHECK(v.size() == 1 && v[0] == t.codePoint);
    }
    // CP1252: the 0x80..0x9F window that Latin-1 leaves as C1 controls.
    struct {
        std::uint8_t byte;
        std::uint32_t codePoint;
    } w[] = {
        { 0x80, 0x20AC }, // €
        { 0x82, 0x201A },
        { 0x85, 0x2026 },
        { 0x91, 0x2018 },
        { 0x92, 0x2019 },
        { 0x93, 0x201C },
        { 0x99, 0x2122 }, // ™
    };
    for (auto& t : w) {
        auto v = decodeAll(Encoding::Windows1252, &t.byte, 1);
        CHECK(v.size() == 1 && v[0] == t.codePoint);
    }
    // CP1252 unmapped slots (0x81/0x8D/0x8F/0x90/0x9D) -> U+FFFD.
    for (std::uint8_t by : { 0x81, 0x8D, 0x8F, 0x90, 0x9D }) {
        auto v = decodeAll(Encoding::Windows1252, &by, 1);
        CHECK(v.size() == 1 && v[0] == 0xFFFD);
    }
}

// ---- Shift_JIS ----
TEST(testShiftJis)
{
    // Full-width あ (U+3042) -> 82 A0.
    std::uint8_t a[] = { 0x82, 0xA0 };
    auto v = decodeAll(Encoding::ShiftJis, a, 2);
    CHECK(v.size() == 1 && v[0] == 0x3042);
    // 漢 (U+6F22) -> 8A BF.
    std::uint8_t k[] = { 0x8A, 0xBF };
    v = decodeAll(Encoding::ShiftJis, k, 2);
    CHECK(v.size() == 1 && v[0] == 0x6F22);
    // Half-width katakana ｱ (single byte 0xB1) -> U+FF71.
    std::uint8_t hw[] = { 0xB1 };
    v = decodeAll(Encoding::ShiftJis, hw, 1);
    CHECK(v.size() == 1 && v[0] == 0xFF71);
    // ASCII passes through.
    std::uint8_t asc[] = { 'A', 'B' };
    v = decodeAll(Encoding::ShiftJis, asc, 2);
    CHECK(v.size() == 2 && v[0] == 'A' && v[1] == 'B');
    // Lead with an invalid trail -> U+FFFD (trail 0x20 invalid).
    std::uint8_t bad[] = { 0x82, 0x20 };
    v = decodeAll(Encoding::ShiftJis, bad, 2);
    // The bad trail (0x20) is ASCII, re-fed as a fresh start.
    CHECK(v.size() >= 1 && v[0] == 0xFFFD);
    CHECK(v.back() == 0x20);
}

// ---- EUC-JP ----
TEST(testEucJp)
{
    // あ -> A4 A2.
    std::uint8_t a[] = { 0xA4, 0xA2 };
    auto v = decodeAll(Encoding::EucJp, a, 2);
    CHECK(v.size() == 1 && v[0] == 0x3042);
    // 漢 -> B4 C1.
    std::uint8_t k[] = { 0xB4, 0xC1 };
    v = decodeAll(Encoding::EucJp, k, 2);
    CHECK(v.size() == 1 && v[0] == 0x6F22);
    // SS2 half-width kana: 8E B1 -> U+FF71.
    std::uint8_t ss2[] = { 0x8E, 0xB1 };
    v = decodeAll(Encoding::EucJp, ss2, 2);
    CHECK(v.size() == 1 && v[0] == 0xFF71);
    // SS3 JIS X 0212: 8F A2 AF -> U+02D8 (breve) per JIS X 0212.
    std::uint8_t ss3[] = { 0x8F, 0xA2, 0xAF };
    v = decodeAll(Encoding::EucJp, ss3, 3);
    CHECK(v.size() == 1 && v[0] == 0x02D8);
}

// ---- EUC-KR / CP949 ----
TEST(testEucKr)
{
    // 가 (U+AC00) -> B0 A1.
    std::uint8_t g[] = { 0xB0, 0xA1 };
    auto v = decodeAll(Encoding::EucKr, g, 2);
    CHECK(v.size() == 1 && v[0] == 0xAC00);
    // 한 (U+D55C) -> C7 D1.
    std::uint8_t h[] = { 0xC7, 0xD1 };
    v = decodeAll(Encoding::EucKr, h, 2);
    CHECK(v.size() == 1 && v[0] == 0xD55C);
    // CP949 extension byte (lead 0x81, trail 0x41) decodes (UHC range).
    std::uint8_t uhc[] = { 0x81, 0x41 };
    v = decodeAll(Encoding::EucKr, uhc, 2);
    CHECK(v.size() == 1 && v[0] != 0xFFFD);
}

// ---- Big5 ----
TEST(testBig5)
{
    // 一 (U+4E00) -> A4 40.
    std::uint8_t y[] = { 0xA4, 0x40 };
    auto v = decodeAll(Encoding::Big5, y, 2);
    CHECK(v.size() == 1 && v[0] == 0x4E00);
    // 中 (U+4E2D) -> A4 A4.
    std::uint8_t z[] = { 0xA4, 0xA4 };
    v = decodeAll(Encoding::Big5, z, 2);
    CHECK(v.size() == 1 && v[0] == 0x4E2D);
}

// ---- GB18030 ----
TEST(testGb18030)
{
    // ASCII.
    std::uint8_t asc[] = { 'X' };
    auto v = decodeAll(Encoding::Gb18030, asc, 1);
    CHECK(v.size() == 1 && v[0] == 'X');
    // 2-byte GBK: 中 (U+4E2D) -> D6 D0.
    std::uint8_t k[] = { 0xD6, 0xD0 };
    v = decodeAll(Encoding::Gb18030, k, 2);
    CHECK(v.size() == 1 && v[0] == 0x4E2D);
    // 4-byte BMP: U+00A5 (¥) -> 81 30 84 36.
    std::uint8_t bmp[] = { 0x81, 0x30, 0x84, 0x36 };
    v = decodeAll(Encoding::Gb18030, bmp, 4);
    CHECK(v.size() == 1 && v[0] == 0x00A5);
    // 4-byte BMP: U+0452 (ѓ) -> 81 30 D3 30.
    std::uint8_t bmp2[] = { 0x81, 0x30, 0xD3, 0x30 };
    v = decodeAll(Encoding::Gb18030, bmp2, 4);
    CHECK(v.size() == 1 && v[0] == 0x0452);
    // 4-byte SMP: U+10000 -> 90 30 81 30.
    std::uint8_t smp[] = { 0x90, 0x30, 0x81, 0x30 };
    v = decodeAll(Encoding::Gb18030, smp, 4);
    CHECK(v.size() == 1 && v[0] == 0x10000);
    // 4-byte SMP: U+1F600 (grinning face) -> 94 39 FC 36.
    std::uint8_t emoji[] = { 0x94, 0x39, 0xFC, 0x36 };
    v = decodeAll(Encoding::Gb18030, emoji, 4);
    CHECK(v.size() == 1 && v[0] == 0x1F600);
    // 4-byte SMP top: U+10FFFF -> E3 32 9A 35.
    std::uint8_t top[] = { 0xE3, 0x32, 0x9A, 0x35 };
    v = decodeAll(Encoding::Gb18030, top, 4);
    CHECK(v.size() == 1 && v[0] == 0x10FFFF);
}

// ---- split across feed boundaries ----
TEST(testSplitAcrossFeeds)
{
    // Shift_JIS あ split 82 | A0 across two feed batches.
    EncDecoder d;
    encReset(&d, Encoding::ShiftJis);
    std::uint32_t codePoint;
    CHECK(encFeed(&d, 0x82, &codePoint) == EncStatus::Need);
    CHECK(encFeed(&d, 0xA0, &codePoint) == EncStatus::Emit);
    CHECK(codePoint == 0x3042);

    // GB18030 4-byte split 90 | 30 81 | 30 across three batches.
    encReset(&d, Encoding::Gb18030);
    CHECK(encFeed(&d, 0x90, &codePoint) == EncStatus::Need);
    CHECK(encFeed(&d, 0x30, &codePoint) == EncStatus::Need);
    CHECK(encFeed(&d, 0x81, &codePoint) == EncStatus::Need);
    CHECK(encFeed(&d, 0x30, &codePoint) == EncStatus::Emit);
    CHECK(codePoint == 0x10000);

    // EUC-JP SS3 split 8F | A2 | AF.
    encReset(&d, Encoding::EucJp);
    CHECK(encFeed(&d, 0x8F, &codePoint) == EncStatus::Need);
    CHECK(encFeed(&d, 0xA2, &codePoint) == EncStatus::Need);
    CHECK(encFeed(&d, 0xAF, &codePoint) == EncStatus::Emit);
    CHECK(codePoint == 0x02D8);
}

// ---- invalid handling -> U+FFFD ----
TEST(testInvalidBytes)
{
    // Big5 lead with an invalid trail (0x20 control-adjacent).
    std::uint8_t bad[] = { 0xA4, 0x20 };
    auto v = decodeAll(Encoding::Big5, bad, 2);
    CHECK(v.size() >= 1 && v[0] == 0xFFFD);
    // GB18030 lead 0x80 is undefined.
    std::uint8_t g80[] = { 0x80 };
    v = decodeAll(Encoding::Gb18030, g80, 1);
    CHECK(v.size() == 1 && v[0] == 0xFFFD);
    // KOI8-R has no unmapped slots, but a lone EUC-KR lead at EOF emits
    // nothing (Need carried, never completed) — no spurious codePoint.
    EncDecoder d;
    encReset(&d, Encoding::EucKr);
    std::uint32_t codePoint;
    CHECK(encFeed(&d, 0xB0, &codePoint) == EncStatus::Need);
    // (stream ends; caller flushes nothing — correct.)
}

// ---- transcode-to-UTF-8 helper ----
TEST(testTranscodeToUTF8)
{
    EncDecoder d;
    encReset(&d, Encoding::Latin1);
    // 0xA9 (©) -> UTF-8 C2 A9.
    std::uint8_t in[] = { 'A', 0xA9, 'B' };
    std::uint8_t outputBuffer[16];
    std::size_t consumed = 0;
    std::size_t w = encDecodeToUTF8(&d, in, outputBuffer, &consumed);
    CHECK(consumed == 3);
    CHECK(w == 4);
    CHECK(outputBuffer[0] == 'A');
    CHECK(outputBuffer[1] == 0xC2 && outputBuffer[2] == 0xA9);
    CHECK(outputBuffer[3] == 'B');

    // Shift_JIS あ across a split: first feed just the lead, then the trail.
    encReset(&d, Encoding::ShiftJis);
    std::uint8_t lead[] = { 0x82 };
    std::uint8_t trail[] = { 0xA0 };
    std::uint8_t o2[16];
    std::size_t c1 = 0, c2 = 0;
    std::size_t w1 = encDecodeToUTF8(&d, lead, o2, &c1);
    CHECK(c1 == 1 && w1 == 0); // nothing emitted yet
    std::size_t w2 = encDecodeToUTF8(&d, trail, o2, &c2);
    CHECK(c2 == 1 && w2 == 3); // U+3042 -> E3 81 82
    CHECK(o2[0] == 0xE3 && o2[1] == 0x81 && o2[2] == 0x82);
}

UBIQUITTY_TEST_MAIN()
