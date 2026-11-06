// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_png.cpp — host-side tests for the minimal PNG decoder used
// by Kitty graphics f=100. test_vt100 already drives a filter-0 RGBA
// PNG end-to-end through the Kitty pipeline; this file calls into
// Codec::PNG::decode directly so the four other PNG filters (Sub / Up /
// Average / Paeth) and the structured error branches surface
// individually rather than disappearing into a single "decode
// failed" return through Kitty.

#include "Core/Codec/PNGDecoder.h"
#include "Core/Codec/ZlibInflate.h"
#include "Core/Rendering/PNGEncoder.h"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <span>
#include <string>
#include <vector>

// Caller-owned inflate state for Codec::PNG::decode. The suite is single-threaded,
// so one shared instance is fine; Codec::PNG::decode resets it on each call.
static Ubiquitty::Codec::Zlib::Inflater testInflater;

/// The four hex blobs are 2×2 8-bit RGB PNGs assembled in Python
/// (zlib + a hand-coded filterer); pixel data is:
///   (10, 20, 30)   (40, 50, 60)
///   (70, 80, 90)   (100, 110, 120)
/// The four files differ only in the per-row PNG filter tag (1=Sub,
/// 2=Up, 3=Average, 4=Paeth) — they all decode back to the same
/// pixels, which is what we assert.

static const uint8_t pngSub[] = {
    0x89,
    0x50,
    0x4e,
    0x47,
    0x0d,
    0x0a,
    0x1a,
    0x0a,
    0x00,
    0x00,
    0x00,
    0x0d,
    0x49,
    0x48,
    0x44,
    0x52,
    0x00,
    0x00,
    0x00,
    0x02,
    0x00,
    0x00,
    0x00,
    0x02,
    0x08,
    0x02,
    0x00,
    0x00,
    0x00,
    0xfd,
    0xd4,
    0x9a,
    0x73,
    0x00,
    0x00,
    0x00,
    0x13,
    0x49,
    0x44,
    0x41,
    0x54,
    0x78,
    0xda,
    0x63,
    0xe4,
    0x12,
    0x91,
    0x03,
    0x02,
    0x46,
    0xb7,
    0x80,
    0x28,
    0x20,
    0x05,
    0x00,
    0x0b,
    0x59,
    0x01,
    0xe3,
    0x92,
    0x34,
    0xf5,
    0x5a,
    0x00,
    0x00,
    0x00,
    0x00,
    0x49,
    0x45,
    0x4e,
    0x44,
    0xae,
    0x42,
    0x60,
    0x82,
};

static const uint8_t pngUp[] = {
    0x89,
    0x50,
    0x4e,
    0x47,
    0x0d,
    0x0a,
    0x1a,
    0x0a,
    0x00,
    0x00,
    0x00,
    0x0d,
    0x49,
    0x48,
    0x44,
    0x52,
    0x00,
    0x00,
    0x00,
    0x02,
    0x00,
    0x00,
    0x00,
    0x02,
    0x08,
    0x02,
    0x00,
    0x00,
    0x00,
    0xfd,
    0xd4,
    0x9a,
    0x73,
    0x00,
    0x00,
    0x00,
    0x12,
    0x49,
    0x44,
    0x41,
    0x54,
    0x78,
    0xda,
    0x63,
    0xe2,
    0x12,
    0x91,
    0xd3,
    0x30,
    0xb2,
    0x61,
    0xb2,
    0x01,
    0x03,
    0x00,
    0x0d,
    0x12,
    0x02,
    0x3f,
    0x34,
    0x4d,
    0xbf,
    0x2b,
    0x00,
    0x00,
    0x00,
    0x00,
    0x49,
    0x45,
    0x4e,
    0x44,
    0xae,
    0x42,
    0x60,
    0x82,
};

static const uint8_t pngAvg[] = {
    0x89,
    0x50,
    0x4e,
    0x47,
    0x0d,
    0x0a,
    0x1a,
    0x0a,
    0x00,
    0x00,
    0x00,
    0x0d,
    0x49,
    0x48,
    0x44,
    0x52,
    0x00,
    0x00,
    0x00,
    0x02,
    0x00,
    0x00,
    0x00,
    0x02,
    0x08,
    0x02,
    0x00,
    0x00,
    0x00,
    0xfd,
    0xd4,
    0x9a,
    0x73,
    0x00,
    0x00,
    0x00,
    0x16,
    0x49,
    0x44,
    0x41,
    0x54,
    0x78,
    0xda,
    0x63,
    0xe6,
    0x12,
    0x91,
    0x53,
    0xd6,
    0xd0,
    0x65,
    0x76,
    0x74,
    0xf3,
    0xd6,
    0xd5,
    0xd5,
    0x05,
    0x00,
    0x0c,
    0x55,
    0x02,
    0x14,
    0x8b,
    0x37,
    0xb4,
    0xf6,
    0x00,
    0x00,
    0x00,
    0x00,
    0x49,
    0x45,
    0x4e,
    0x44,
    0xae,
    0x42,
    0x60,
    0x82,
};

static const uint8_t pngPaeth[] = {
    0x89,
    0x50,
    0x4e,
    0x47,
    0x0d,
    0x0a,
    0x1a,
    0x0a,
    0x00,
    0x00,
    0x00,
    0x0d,
    0x49,
    0x48,
    0x44,
    0x52,
    0x00,
    0x00,
    0x00,
    0x02,
    0x00,
    0x00,
    0x00,
    0x02,
    0x08,
    0x02,
    0x00,
    0x00,
    0x00,
    0xfd,
    0xd4,
    0x9a,
    0x73,
    0x00,
    0x00,
    0x00,
    0x13,
    0x49,
    0x44,
    0x41,
    0x54,
    0x78,
    0xda,
    0x63,
    0xe1,
    0x12,
    0x91,
    0x03,
    0x02,
    0x16,
    0x1b,
    0x1b,
    0x1b,
    0x20,
    0x05,
    0x00,
    0x0a,
    0x80,
    0x01,
    0xad,
    0xcb,
    0xb5,
    0x92,
    0x07,
    0x00,
    0x00,
    0x00,
    0x00,
    0x49,
    0x45,
    0x4e,
    0x44,
    0xae,
    0x42,
    0x60,
    0x82,
};

/// The expected 2×2 RGB pixel grid, top-to-bottom, left-to-right.
static const uint8_t expectedPixels[] = {
    10,
    20,
    30,
    40,
    50,
    60,
    70,
    80,
    90,
    100,
    110,
    120,
};

/// Describes the first way a decoded filter fixture differs from the 2x2
/// gradient, or returns an empty string when the decode matches it exactly.
static std::string firstDifference(std::span<const uint8_t> png)
{
    uint8_t scratch[2048];
    const Ubiquitty::Codec::PNG::DecodeResult res = Ubiquitty::Codec::PNG::decode(png, scratch, testInflater);
    char description[128];
    if (res.status != Ubiquitty::Codec::PNG::DecodeStatus::Success) {
        std::snprintf(description, sizeof(description), "Codec::PNG::decode returned status %d", static_cast<int>(res.status));
        return description;
    }
    if (res.width != 2 || res.height != 2 || res.bytesPerPixel != 3 || static_cast<int>(res.pixels.size()) != 12) {
        std::snprintf(description, sizeof(description), "unexpected header w=%d h=%d bpp=%d len=%d", res.width, res.height, res.bytesPerPixel, static_cast<int>(res.pixels.size()));
        return description;
    }
    for (int i = 0; i < 12; i++) {
        if (res.pixels[i] != expectedPixels[i]) {
            std::snprintf(description, sizeof(description), "pixel[%d]=%u expected %u", i, res.pixels[i], expectedPixels[i]);
            return description;
        }
    }
    return {};
}

/// Drive each filter type through Codec::PNG::decode and verify the decoded
/// pixels match the expected gradient. The filters get exercised
/// directly here so a regression in the unfilter switch surfaces as
/// a single named test failure rather than a Kitty-graphics integration
/// miss.
static void checkDecode(const uint8_t* png, int pngLength, const char* name)
{
    const std::string difference = firstDifference({ png, static_cast<std::size_t>(pngLength) });
    CHECK_MSG(difference.empty(), (std::string(name) + " filter: " + difference).c_str());
}

TEST(testFilterSub) { checkDecode(pngSub, sizeof(pngSub), "Sub"); }
TEST(testFilterUp) { checkDecode(pngUp, sizeof(pngUp), "Up"); }
TEST(testFilterAvg) { checkDecode(pngAvg, sizeof(pngAvg), "Avg"); }
TEST(testFilterPaeth) { checkDecode(pngPaeth, sizeof(pngPaeth), "Paeth"); }

/// Garbage signature → Ubiquitty::Codec::PNG::DecodeStatus::BadSignature.
TEST(testBadSignature)
{
    uint8_t scratch[256];
    Ubiquitty::Codec::PNG::DecodeResult res;
    res = {};
    uint8_t bytes[16] = { 'n', 'o', 't', ' ', 'p', 'n', 'g', '!' };
    res = Ubiquitty::Codec::PNG::decode({ bytes, static_cast<std::size_t>(sizeof(bytes)) }, scratch, testInflater);
    CHECK_MSG(res.status == Ubiquitty::Codec::PNG::DecodeStatus::BadSignature, "expected Ubiquitty::Codec::PNG::DecodeStatus::BadSignature");
}

/// Chunk header claiming length 100 with only the 4-byte type byte
/// following → Ubiquitty::Codec::PNG::DecodeStatus::Truncated (advertised length exceeds the
/// remaining input).
TEST(testTruncated)
{
    uint8_t scratch[256];
    Ubiquitty::Codec::PNG::DecodeResult res;
    res = {};
    uint8_t stream[] = {
        0x89,
        0x50,
        0x4e,
        0x47,
        0x0d,
        0x0a,
        0x1a,
        0x0a, // signature
        0x00,
        0x00,
        0x00,
        0x64, // chunkLength = 100
        0x49,
        0x48,
        0x44,
        0x52, // "IHDR"
        // claimed 100 bytes of data — actually zero
    };
    res = Ubiquitty::Codec::PNG::decode({ stream, static_cast<std::size_t>(sizeof(stream)) }, scratch, testInflater);
    CHECK_MSG(res.status == Ubiquitty::Codec::PNG::DecodeStatus::Truncated, "expected Ubiquitty::Codec::PNG::DecodeStatus::Truncated");
}

/// A hand-assembled 2x2 8-bit RGB PNG whose IDAT decompresses CLEANLY but whose
/// first scanline carries filter byte 9 — outside the valid 0..4 range. Byte
/// corruption cannot produce this: zlib's checksum fails first, yielding
/// InflateFailed. Only a stream assembled byte by byte, as this one is, reaches
/// wuffs' bad_filter.
static const uint8_t badFilterPNG[] = {
    // The signature.
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a,
    // IHDR, 13 bytes: 2 by 2, depth 8, colour type 2 (RGB), compression,
    // filter, and interlace methods 0; then its CRC.
    0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52,
    0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02, 0x08, 0x02, 0x00, 0x00, 0x00,
    0xfd, 0xd4, 0x9a, 0x73,
    // IDAT, 20 bytes: a zlib stream whose first scanline opens with filter
    // byte 9; then its CRC.
    0x00, 0x00, 0x00, 0x14, 0x49, 0x44, 0x41, 0x54,
    0x78, 0x9c, 0xe3, 0xfc, 0xcf, 0xc0, 0xc0, 0x00, 0xc2, 0x0c,
    0xff, 0xff, 0xff, 0x67, 0x00, 0x00, 0x1f, 0x6d, 0x05, 0x05,
    0xf9, 0xdc, 0xa6, 0xcb,
    // IEND, empty, and its CRC.
    0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82
};

/// Decodes a complete PNG with scratch room for the 2x2 fixtures and reports
/// only how the decode ended.
static Ubiquitty::Codec::PNG::DecodeStatus decodeStatusOf(std::span<const uint8_t> png)
{
    uint8_t room[256];
    return Ubiquitty::Codec::PNG::decode(png, room, testInflater).status;
}

/// The filter byte is the only difference from a valid image, so a BadFilter
/// result cannot be an accident of some other malformation.
TEST(testBadFilterByte)
{
    CHECK_MSG(decodeStatusOf(badFilterPNG) == Ubiquitty::Codec::PNG::DecodeStatus::BadFilter, "a scanline tagged with filter type 9 is refused as a bad filter");
}

/// Damage to the compressed stream inside the Sub fixture's IDAT chunk, given
/// the offset where the chunk's data starts. The signature, the IHDR, and every
/// chunk length stay as the fixture wrote them.
static void flipEarlyStreamByte(std::span<uint8_t> png, std::size_t payload) { png[payload + 2] ^= 0xFF; }

static void breakZlibHeader(std::span<uint8_t> png, std::size_t payload) { png[payload + 1] ^= 0x0F; }

static void scramblePayload(std::span<uint8_t> png, std::size_t payload)
{
    for (std::size_t index = payload; index + 12 < png.size(); ++index)
        png[index] ^= 0x5A;
}

/// Finds where the Sub fixture's first IDAT chunk data starts, or returns 0
/// when it has none. Searching keeps the offset correct across a fixture edit.
///
/// Compared byte by byte rather than with memcmp against a literal. GCC 13.3
/// on aarch64 folds `memcmp(&constantArray[runtimeIndex], "literal", n)` as
/// though the index were zero, so the search never matches and the case
/// fails on Linux while passing under clang. Assigning the address to a
/// variable first also avoids it, which is a fix nobody would keep.
static std::size_t idatPayloadStart()
{
    for (std::size_t chunkType = 0; chunkType + 4 < sizeof(pngSub); ++chunkType) {
        if (pngSub[chunkType] == 'I' && pngSub[chunkType + 1] == 'D' && pngSub[chunkType + 2] == 'A' && pngSub[chunkType + 3] == 'T')
            return chunkType + 4;
    }
    return 0;
}

/// Corrupting bytes inside the IDAT payload — after the signature, IHDR and
/// chunk structure all validate — is the only way to reach the wuffs decode
/// failure mapping. Every other negative test here trips a PRE-wuffs structural
/// check and returns before wuffs is invoked, which is why mapWuffsDecodeFailure
/// had never executed once. Verified empirically: each corruption below lands on
/// InflateFailed, the function's fallthrough arm.
TEST(testCorruptIdatPayloadIsInflateFailure)
{
    const std::size_t payloadStart = idatPayloadStart();
    REQUIRE_MSG(payloadStart > 0, "the Sub fixture carries an IDAT chunk to corrupt");

    const struct {
        void (*corrupt)(std::span<uint8_t> png, std::size_t payload);
        const char* meaning;
    } corruptions[] = {
        { flipEarlyStreamByte, "one flipped byte early in the compressed stream fails to inflate" },
        { breakZlibHeader, "an invalid zlib header fails to inflate" },
        { scramblePayload, "a scrambled payload fails to inflate" },
    };
    for (const auto& corruption : corruptions) {
        std::array<uint8_t, sizeof(pngSub)> damaged {};
        std::ranges::copy(pngSub, damaged.begin());
        corruption.corrupt(damaged, payloadStart);
        CHECK_MSG(decodeStatusOf(damaged) == Ubiquitty::Codec::PNG::DecodeStatus::InflateFailed, corruption.meaning);
    }
}

/// Pure signature only → Ubiquitty::Codec::PNG::DecodeStatus::MissingChunks (no IHDR/IDAT).
TEST(testMissingChunks)
{
    uint8_t scratch[256];
    Ubiquitty::Codec::PNG::DecodeResult res;
    res = {};
    uint8_t sig[] = { 0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a };
    res = Ubiquitty::Codec::PNG::decode({ sig, static_cast<std::size_t>(sizeof(sig)) }, scratch, testInflater);
    CHECK_MSG(res.status == Ubiquitty::Codec::PNG::DecodeStatus::MissingChunks, "expected Ubiquitty::Codec::PNG::DecodeStatus::MissingChunks");
}

/// Unsupported bit depth (16) → Ubiquitty::Codec::PNG::DecodeStatus::Unsupported. We mutate the
/// depth byte in the pngSub fixture so the rest of the chunk
/// structure stays valid.
TEST(testUnsupportedDepth)
{
    uint8_t buffer[sizeof(pngSub)];
    uint8_t scratch[256];
    Ubiquitty::Codec::PNG::DecodeResult res;
    memcpy(buffer, pngSub, sizeof(buffer));
    // IHDR depth byte sits at offset 24 (sig 8 + length 4 + 'IHDR' 4 + W 4 + H 4 = 24).
    buffer[24] = 16;
    res = {};
    res = Ubiquitty::Codec::PNG::decode({ buffer, static_cast<std::size_t>(sizeof(buffer)) }, scratch, testInflater);
    CHECK_MSG(res.status == Ubiquitty::Codec::PNG::DecodeStatus::Unsupported, "expected Ubiquitty::Codec::PNG::DecodeStatus::Unsupported for depth=16");
}

/// Tiny scratch buffer → Ubiquitty::Codec::PNG::DecodeStatus::ScratchTooSmall.
TEST(testScratchSmall)
{
    uint8_t scratch[8]; // nowhere near enough for inflate + pixels
    Ubiquitty::Codec::PNG::DecodeResult res;
    res = {};
    res = Ubiquitty::Codec::PNG::decode({ pngSub, static_cast<std::size_t>(sizeof(pngSub)) }, scratch, testInflater);
    CHECK_MSG(res.status == Ubiquitty::Codec::PNG::DecodeStatus::ScratchTooSmall, "expected Ubiquitty::Codec::PNG::DecodeStatus::ScratchTooSmall with tiny scratch");
}

/// Crafted IHDR claiming width = 0x80000000 (sign bit set). Plain
/// int cast would wrap to a negative value; the decoder must reject
/// before any size math runs.
TEST(testHostileWidthOverflow)
{
    uint8_t scratch[256];
    Ubiquitty::Codec::PNG::DecodeResult res;
    uint8_t buffer[sizeof(pngSub)];
    memcpy(buffer, pngSub, sizeof(buffer));
    // Width is at offset 16..19 (sig 8 + length 4 + 'IHDR' 4).
    buffer[16] = 0x80;
    buffer[17] = 0x00;
    buffer[18] = 0x00;
    buffer[19] = 0x00;
    res = {};
    res = Ubiquitty::Codec::PNG::decode({ buffer, static_cast<std::size_t>(sizeof(buffer)) }, scratch, testInflater);
    // Either UNSUPPORTED (dimension cap) or BAD_IHDR (negative-w guard
    // if signed-overflow makes w <= 0). Both are safe rejections; we
    // just must not write past scratch.
    CHECK_MSG(res.status == Ubiquitty::Codec::PNG::DecodeStatus::Unsupported || res.status == Ubiquitty::Codec::PNG::DecodeStatus::BadIHDR, "hostile width rejected without arithmetic wrap");
}

/// Crafted IHDR with w = h = 30000 RGBA: 30000*30000*4 = 3.6 GB, well
/// past pngMaxBytes. Must reject as UNSUPPORTED — never attempt to
/// compute rawTotal in int.
TEST(testHostileGiantDimensions)
{
    uint8_t scratch[256];
    Ubiquitty::Codec::PNG::DecodeResult res;
    uint8_t buffer[sizeof(pngSub)];
    memcpy(buffer, pngSub, sizeof(buffer));
    // w = 30000 → 0x00007530
    buffer[16] = 0x00;
    buffer[17] = 0x00;
    buffer[18] = 0x75;
    buffer[19] = 0x30;
    // h = 30000
    buffer[20] = 0x00;
    buffer[21] = 0x00;
    buffer[22] = 0x75;
    buffer[23] = 0x30;
    // ct = 6 (RGBA, 4 bpp)
    buffer[25] = 6;
    res = {};
    res = Ubiquitty::Codec::PNG::decode({ buffer, static_cast<std::size_t>(sizeof(buffer)) }, scratch, testInflater);
    CHECK_MSG(res.status == Ubiquitty::Codec::PNG::DecodeStatus::Unsupported, "giant dimensions rejected as UNSUPPORTED");
}

/// Crafted chunkLength with sign bit set (>= 0x80000000). The signed-int
/// cast in the chunk-skip arithmetic would otherwise wrap.
TEST(testHostileChunkLength)
{
    uint8_t scratch[256];
    Ubiquitty::Codec::PNG::DecodeResult res;
    uint8_t stream[] = {
        0x89,
        0x50,
        0x4e,
        0x47,
        0x0d,
        0x0a,
        0x1a,
        0x0a, // signature
        0x80,
        0x00,
        0x00,
        0x00, // chunkLength sign bit set
        0x49,
        0x48,
        0x44,
        0x52, // "IHDR"
    };
    res = {};
    res = Ubiquitty::Codec::PNG::decode({ stream, static_cast<std::size_t>(sizeof(stream)) }, scratch, testInflater);
    CHECK_MSG(res.status == Ubiquitty::Codec::PNG::DecodeStatus::Truncated, "hostile chunkLength rejected as TRUNCATED");
}

// Every remaining IHDR rejection. The decoder validates the header field by
// field, so each case patches exactly one byte of a valid fixture and leaves
// the rest of the chunk structure intact — a rejection that fires for the wrong
// reason would otherwise look identical to the right one.
//
// IHDR data begins at offset 16 (signature 8 + length 4 + type 4):
//   16..19 width   20..23 height   24 depth   25 colour type
//   26 compression method   27 filter method   28 interlace method

// A patched copy of pngSub, decoded against a scratch big enough to succeed.
static Ubiquitty::Codec::PNG::DecodeStatus decodeWithPatchedByte(std::size_t offset, uint8_t value)
{
    uint8_t buffer[sizeof(pngSub)];
    uint8_t scratch[2048];
    memcpy(buffer, pngSub, sizeof(buffer));
    buffer[offset] = value;
    const Ubiquitty::Codec::PNG::DecodeResult res
        = Ubiquitty::Codec::PNG::decode({ buffer, sizeof(buffer) }, scratch, testInflater);
    return res.status;
}

// Zero width and zero height are BadIHDR, not Unsupported: a dimension of zero
// is a malformed header, where an oversized one is a header we decline.
TEST(testZeroWidthIsBadIHDR)
{
    CHECK_MSG(decodeWithPatchedByte(19, 0) == Ubiquitty::Codec::PNG::DecodeStatus::BadIHDR,
        "width 0 must be BadIHDR");
}

TEST(testZeroHeightIsBadIHDR)
{
    CHECK_MSG(decodeWithPatchedByte(23, 0) == Ubiquitty::Codec::PNG::DecodeStatus::BadIHDR,
        "height 0 must be BadIHDR");
}

// Colour types 0 (greyscale), 3 (palette) and 4 (greyscale+alpha) are legal PNG
// that kitty f=100 does not carry; only 2 and 6 are accepted.
TEST(testPaletteColorTypeIsUnsupported)
{
    CHECK_MSG(decodeWithPatchedByte(25, 3) == Ubiquitty::Codec::PNG::DecodeStatus::Unsupported,
        "colour type 3 (palette) must be Unsupported");
    CHECK_MSG(decodeWithPatchedByte(25, 0) == Ubiquitty::Codec::PNG::DecodeStatus::Unsupported,
        "colour type 0 (greyscale) must be Unsupported");
    CHECK_MSG(decodeWithPatchedByte(25, 4) == Ubiquitty::Codec::PNG::DecodeStatus::Unsupported,
        "colour type 4 (greyscale+alpha) must be Unsupported");
}

// Compression, filter and interlace methods are each checked separately, so a
// non-zero value in any one of them has to be rejected on its own.
TEST(testNonZeroCompressionMethodIsUnsupported)
{
    CHECK_MSG(decodeWithPatchedByte(26, 1) == Ubiquitty::Codec::PNG::DecodeStatus::Unsupported,
        "compression method 1 must be Unsupported");
}

TEST(testNonZeroFilterMethodIsUnsupported)
{
    CHECK_MSG(decodeWithPatchedByte(27, 1) == Ubiquitty::Codec::PNG::DecodeStatus::Unsupported,
        "filter method 1 must be Unsupported");
}

// Adam7. The decoder has no de-interlacer, so it declines rather than producing
// a wrongly-ordered image.
TEST(testInterlacedIsUnsupported)
{
    CHECK_MSG(decodeWithPatchedByte(28, 1) == Ubiquitty::Codec::PNG::DecodeStatus::Unsupported,
        "interlace method 1 (Adam7) must be Unsupported");
}

// A second IHDR is malformed even when the first was valid.
TEST(testDuplicateIHDRIsBadIHDR)
{
    uint8_t buffer[sizeof(pngSub) + 25];
    uint8_t scratch[2048];
    // signature + the whole first IHDR chunk (4 length + 4 type + 13 data + 4 CRC)
    constexpr std::size_t ihdrChunkEnd = 8 + 25;
    memcpy(buffer, pngSub, ihdrChunkEnd);
    memcpy(buffer + ihdrChunkEnd, pngSub + 8, 25);
    memcpy(buffer + ihdrChunkEnd + 25, pngSub + ihdrChunkEnd, sizeof(pngSub) - ihdrChunkEnd);
    const Ubiquitty::Codec::PNG::DecodeResult res
        = Ubiquitty::Codec::PNG::decode({ buffer, sizeof(buffer) }, scratch, testInflater);
    CHECK_MSG(res.status == Ubiquitty::Codec::PNG::DecodeStatus::BadIHDR,
        "a second IHDR must be BadIHDR");
}

// An IHDR whose declared length is not 13 is rejected before any field is read.
TEST(testShortIHDRIsBadIHDR)
{
    CHECK_MSG(decodeWithPatchedByte(11, 12) == Ubiquitty::Codec::PNG::DecodeStatus::BadIHDR,
        "IHDR length 12 must be BadIHDR");
}

// An empty scratch span is rejected before the structural walk, separately from
// the later "scratch smaller than the decoded image" check.
TEST(testEmptyScratchIsScratchTooSmall)
{
    const Ubiquitty::Codec::PNG::DecodeResult res
        = Ubiquitty::Codec::PNG::decode({ pngSub, sizeof(pngSub) }, {}, testInflater);
    CHECK_MSG(res.status == Ubiquitty::Codec::PNG::DecodeStatus::ScratchTooSmall,
        "an empty scratch span must be ScratchTooSmall");
}

// A null input span is a BadSignature, not a crash.
TEST(testNullInputIsBadSignature)
{
    uint8_t scratch[256];
    const Ubiquitty::Codec::PNG::DecodeResult res = Ubiquitty::Codec::PNG::decode({}, scratch, testInflater);
    CHECK_MSG(res.status == Ubiquitty::Codec::PNG::DecodeStatus::BadSignature,
        "an empty input span must be BadSignature");
}

// ---- the encoder ------------------------------------------------------------
//
// encodePNG is checked here against this file's own decoder rather than
// against a fixture, because a fixture only says the bytes
// match the bytes someone recorded — a round trip says the image survives, and
// the decoder it round-trips through is the one the rest of this file already
// holds to the format.
//
// Every case uses a NON-SQUARE image with a distinct colour per pixel. A square
// of one colour survives a transposed width and height, a swapped red and blue,
// and a row stride computed from the wrong dimension.

namespace {

// Pixel (x, y) is distinguishable from every other: red carries x, green carries
// y, blue carries their sum. A pixel arriving at the wrong coordinate names
// where it came from.
void fillDistinct(std::vector<uint8_t>& rgba, int width, int height)
{
    rgba.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            uint8_t* p = &rgba[(static_cast<std::size_t>(y) * static_cast<std::size_t>(width)
                                   + static_cast<std::size_t>(x))
                * 4];
            p[0] = static_cast<uint8_t>(0x10 + x);
            p[1] = static_cast<uint8_t>(0x40 + y);
            p[2] = static_cast<uint8_t>(0x80 + x + y);
            p[3] = 0xFF;
        }
    }
}

} // namespace

TEST(testAnEncodedImageDecodesBackPixelForPixel)
{
    const int width = 5;
    const int height = 3;
    std::vector<uint8_t> rgba;
    fillDistinct(rgba, width, height);

    const std::vector<uint8_t> png = Ubiquitty::Rendering::encodePNG(rgba, { width, height });
    CHECK_MSG(!png.empty(), "the encoder produced bytes");

    uint8_t scratch[4096];
    const Ubiquitty::Codec::PNG::DecodeResult res
        = Ubiquitty::Codec::PNG::decode({ png.data(), png.size() }, scratch, testInflater);
    CHECK_MSG(res.status == Ubiquitty::Codec::PNG::DecodeStatus::Success, "our own decoder reads it");
    CHECK_MSG(res.width == width, "width survives the round trip");
    CHECK_MSG(res.height == height, "height survives, and is not the width");

    bool isIdentical = true;
    for (std::size_t i = 0; i < rgba.size() && i < res.pixels.size(); ++i) {
        if (res.pixels[i] != rgba[i]) {
            isIdentical = false;
            break;
        }
    }
    CHECK_MSG(res.pixels.size() == rgba.size(), "every pixel came back");
    CHECK_MSG(isIdentical, "and each one came back unchanged");
}

TEST(testASinglePixelImageRoundTrips)
{
    std::vector<uint8_t> rgba;
    fillDistinct(rgba, 1, 1);
    const std::vector<uint8_t> png = Ubiquitty::Rendering::encodePNG(rgba, { 1, 1 });

    uint8_t scratch[256];
    const Ubiquitty::Codec::PNG::DecodeResult res
        = Ubiquitty::Codec::PNG::decode({ png.data(), png.size() }, scratch, testInflater);
    CHECK_MSG(res.status == Ubiquitty::Codec::PNG::DecodeStatus::Success, "one pixel is a valid image");
    CHECK(res.width == 1 && res.height == 1);
    CHECK_MSG(res.pixels[0] == rgba[0] && res.pixels[3] == rgba[3], "the pixel and its alpha survive");
}

TEST(testATallImageIsNotDecodedAsAWideOne)
{
    // The transposed case on its own: 2 wide by 7 tall. An encoder that writes
    // the dimensions the wrong way round produces a file our decoder still reads
    // — as a 7x2 image — so the status is Success and only the numbers differ.
    const int width = 2;
    const int height = 7;
    std::vector<uint8_t> rgba;
    fillDistinct(rgba, width, height);
    const std::vector<uint8_t> png = Ubiquitty::Rendering::encodePNG(rgba, { width, height });

    uint8_t scratch[4096];
    const Ubiquitty::Codec::PNG::DecodeResult res
        = Ubiquitty::Codec::PNG::decode({ png.data(), png.size() }, scratch, testInflater);
    CHECK(res.status == Ubiquitty::Codec::PNG::DecodeStatus::Success);
    CHECK_MSG(res.width == 2, "two columns, not seven");
    CHECK_MSG(res.height == 7, "seven rows, not two");
}

UBIQUITTY_TEST_MAIN()
