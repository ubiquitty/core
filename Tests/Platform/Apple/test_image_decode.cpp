// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// The ImageIO decoder against Core's per-image budget.
//
// A PNG states its size in a header a few bytes long, and a uniform picture
// deflates to almost nothing: a 1-bit 16384 x 16384 one is about 32 KB of file
// and a gibibyte of RGBA. ImageIO creates such an image without decoding it, so
// a check of the created image's sides alone lets it through to the allocation
// and the draw, and Core's own check after the decode comes too late for either.
// These pin the refusal before both, at the budget's edge, and the pictures
// inside it decoding in full.
//
// The decoder checks twice: the size the container states, before the image is
// created, and the created image's, before the allocation. Every format ImageIO
// was tried with on macOS 27 states its size, and creating the image is lazy,
// so through `decodeImageRGBA` either check alone refuses these pictures: the
// tests of the decoder pin the two together. The stated size is also pinned on
// its own, through `detail::isStatedSizeRefused`. The created image's check is
// the backstop for a container that states no size, and no file here reaches
// it alone.

#include "Harness.h"

#include "Core/Image/TerminalImage.h"
#include "Platform/Apple/CFAuto.h"
#include "Platform/Apple/ImageDecode.h"

#include <ImageIO/ImageIO.h>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <string_view>
#include <sys/resource.h>
#include <vector>
#include <zlib.h>

namespace {

void appendBigEndian32(std::vector<std::uint8_t>& out, std::uint32_t value)
{
    for (int shift = 24; shift >= 0; shift -= 8)
        out.push_back(static_cast<std::uint8_t>(value >> shift));
}

// One chunk: its length, type and data, then the CRC ImageIO verifies over the
// type and data.
void appendChunk(std::vector<std::uint8_t>& png, std::string_view type, std::span<const std::uint8_t> data)
{
    appendBigEndian32(png, static_cast<std::uint32_t>(data.size()));
    const std::size_t typeStart = png.size();
    png.insert(png.end(), type.begin(), type.end());
    png.insert(png.end(), data.begin(), data.end());
    const uLong crc = ::crc32(0, png.data() + typeStart, static_cast<uInt>(png.size() - typeStart));
    appendBigEndian32(png, static_cast<std::uint32_t>(crc));
}

// A PNG of `size`, each row deflated as it is produced, so a picture that
// decodes to a gibibyte is encoded holding one row of it.
//
// - Parameters:
//   - size: The width and height in pixels.
//   - bitDepth: IHDR's bit depth.
//   - colorType: IHDR's colour type: 0 for grayscale, 6 for RGBA.
//   - rowAt: The unfiltered bytes of row `y`.
template<typename RowAt>
std::vector<std::uint8_t> encodePNG(Ubiquitty::IntSize size, std::uint8_t bitDepth, std::uint8_t colorType, RowAt rowAt)
{
    std::vector<std::uint8_t> png { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
    std::vector<std::uint8_t> header;
    appendBigEndian32(header, static_cast<std::uint32_t>(size.width));
    appendBigEndian32(header, static_cast<std::uint32_t>(size.height));
    header.insert(header.end(), { bitDepth, colorType, 0, 0, 0 });
    appendChunk(png, "IHDR", header);

    z_stream stream {};
    CHECK_EQ(::deflateInit(&stream, Z_BEST_COMPRESSION), Z_OK);
    std::vector<std::uint8_t> compressed;
    std::array<std::uint8_t, 16384> output {};
    const auto deflateInto = [&](std::span<const std::uint8_t> input, int flush) {
        stream.next_in = const_cast<Bytef*>(input.data());
        stream.avail_in = static_cast<uInt>(input.size());
        do {
            stream.next_out = output.data();
            stream.avail_out = static_cast<uInt>(output.size());
            ::deflate(&stream, flush);
            compressed.insert(compressed.end(), output.data(), output.data() + (output.size() - stream.avail_out));
        } while (stream.avail_out == 0);
    };
    constexpr std::array<std::uint8_t, 1> noFilter { 0 };
    for (int y = 0; y < size.height; ++y) {
        deflateInto(noFilter, Z_NO_FLUSH);
        deflateInto(rowAt(y), Z_NO_FLUSH);
    }
    deflateInto({}, Z_FINISH);
    ::deflateEnd(&stream);
    appendChunk(png, "IDAT", compressed);
    appendChunk(png, "IEND", {});
    return png;
}

// A black 1-bit grayscale picture: the most a few bytes of file can claim.
std::vector<std::uint8_t> blackPNG(Ubiquitty::IntSize size)
{
    const std::vector<std::uint8_t> row(static_cast<std::size_t>((size.width + 7) / 8), 0);
    return encodePNG(size, 1, 0, [&](int) { return std::span<const std::uint8_t>(row); });
}

// The process's peak resident size, which macOS reports in bytes.
std::uint64_t peakResidentBytes()
{
    rusage usage {};
    ::getrusage(RUSAGE_SELF, &usage);
    return static_cast<std::uint64_t>(usage.ru_maxrss);
}

// The decoder's answer, with the outputs left as a refusal must leave them.
struct Decoded {
    bool didDecode = false;
    std::uint8_t* rgba = nullptr;
    int width = -1;
    int height = -1;
};

Decoded decode(std::span<const std::uint8_t> png)
{
    Decoded decoded;
    decoded.didDecode = Ubiquitty::Apple::decodeImageRGBA(png, &decoded.rgba, &decoded.width, &decoded.height);
    return decoded;
}

// What the decoder reads from the container before creating its image.
bool isStatedSizeRefused(std::span<const std::uint8_t> bytes)
{
    const Ubiquitty::Apple::UniqueCF<CFDataRef> data(::CFDataCreate(kCFAllocatorDefault, bytes.data(), static_cast<CFIndex>(bytes.size())));
    const Ubiquitty::Apple::UniqueCF<CGImageSourceRef> source(data ? ::CGImageSourceCreateWithData(data.get(), nullptr) : nullptr);
    CHECK(source != nullptr);
    return source && Ubiquitty::Apple::detail::isStatedSizeRefused(source.get());
}

// A refusal writes nothing; an acceptance is freed, so a regression reports
// rather than holding what it allocated for the rest of the run.
void checkRefused(const Decoded& decoded)
{
    CHECK(!decoded.didDecode);
    CHECK(decoded.rgba == nullptr);
    CHECK_EQ(decoded.width, -1);
    CHECK_EQ(decoded.height, -1);
    std::free(decoded.rgba);
}

} // namespace

TEST(testRefusesAGibibytePictureWithoutAllocatingIt)
{
    const std::vector<std::uint8_t> png = blackPNG({ 16384, 16384 });
    // The file is the attack's whole cost: kilobytes, against a gibibyte decoded.
    CHECK(png.size() < 64u * 1024u);
    const std::uint64_t peakBefore = peakResidentBytes();
    checkRefused(decode(png));
    // Drawing it would make every page of the gibibyte resident; a refusal from
    // the header touches none of them.
    CHECK(peakResidentBytes() - peakBefore < 256u * 1024u * 1024u);
}

TEST(testRefusesARowOrAColumnPastTheBudget)
{
    // Both inside the axis ceiling, so only the byte budget refuses them.
    checkRefused(decode(blackPNG({ 4096, 4097 })));
    checkRefused(decode(blackPNG({ 4097, 4096 })));
}

TEST(testRefusesAStatedSizePastTheBudget)
{
    CHECK(isStatedSizeRefused(blackPNG({ 16384, 16384 })));
    CHECK(isStatedSizeRefused(blackPNG({ 4096, 4097 })));
    CHECK(isStatedSizeRefused(blackPNG({ 4097, 4096 })));
    CHECK(!isStatedSizeRefused(blackPNG({ 4096, 4096 })));
}

TEST(testLeavesAContainerThatStatesNoSizeToTheCreatedImage)
{
    // No format ImageIO knows, so it states no size, and no image is created
    // from it either.
    constexpr std::array<std::uint8_t, 16> noise {
        'n', 'o', 't', ' ', 'a', 'n', ' ', 'i', 'm', 'a', 'g', 'e', 0, 1, 2, 3
    };
    CHECK(!isStatedSizeRefused(noise));
    checkRefused(decode(noise));
}

TEST(testDecodesAPictureAtTheBudget)
{
    const Decoded decoded = decode(blackPNG({ 4096, 4096 }));
    CHECK(decoded.didDecode);
    CHECK_EQ(decoded.width, 4096);
    CHECK_EQ(decoded.height, 4096);
    if (decoded.rgba) {
        const std::size_t last = (std::size_t { 4096 } * 4096 - 1) * 4;
        for (const std::size_t pixel : { std::size_t { 0 }, last }) {
            CHECK_EQ(decoded.rgba[pixel + 0], 0);
            CHECK_EQ(decoded.rgba[pixel + 1], 0);
            CHECK_EQ(decoded.rgba[pixel + 2], 0);
            CHECK_EQ(decoded.rgba[pixel + 3], 255);
        }
    }
    std::free(decoded.rgba);
}

TEST(testDecodesASmallPictureToStraightAlpha)
{
    // Neutral colours, which every RGB space with a D65 white maps to
    // themselves, so the bytes do not depend on the presentation space. The
    // half-transparent white is premultiplied by the draw and restored after.
    constexpr std::array<std::uint8_t, 16> pixels {
        0, 0, 0, 255, 255, 255, 255, 255,
        255, 255, 255, 128, 90, 60, 30, 0
    };
    const std::vector<std::uint8_t> png = encodePNG({ 2, 2 }, 8, 6, [&](int y) {
        return std::span<const std::uint8_t>(pixels).subspan(static_cast<std::size_t>(y) * 8, 8);
    });
    const Decoded decoded = decode(png);
    CHECK(decoded.didDecode);
    CHECK_EQ(decoded.width, 2);
    CHECK_EQ(decoded.height, 2);
    // A transparent pixel's colour is gone with its alpha: it comes back zero.
    constexpr std::array<std::uint8_t, 16> expected {
        0, 0, 0, 255, 255, 255, 255, 255,
        255, 255, 255, 128, 0, 0, 0, 0
    };
    if (decoded.rgba) {
        for (std::size_t index = 0; index < expected.size(); ++index)
            CHECK_EQ(decoded.rgba[index], expected[index]);
    }
    std::free(decoded.rgba);
}

UBIQUITTY_TEST_MAIN()
