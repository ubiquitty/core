// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_tiff_decoder — the baseline TIFF reader, fed the bytes a program on the
// other end of the pty chose.
//
// Every input here is attacker-controlled: a TIFF reaches this decoder because
// something printed one. So the cases are mostly refusals, and each names the
// byte it refuses on — a decoder that answers Success has already promised the
// caller a buffer of that many valid pixels.
//
// The scratch buffer is poisoned before every decode. A decoder that reports a
// size it did not write is worse than one that fails: the caller shows whatever
// the buffer held, which is the previous image or nothing at all.

#include "Core/Image/TIFFDecoder.h"

#include <cstdint>
#include <span>
#include <vector>

using namespace Ubiquitty::Image::TIFF;

namespace {

constexpr std::uint8_t poison = 0xCD;

void put16(std::vector<std::uint8_t>& b, std::uint16_t v, bool littleEndian)
{
    if (littleEndian) {
        b.push_back(static_cast<std::uint8_t>(v & 0xFF));
        b.push_back(static_cast<std::uint8_t>(v >> 8));
    } else {
        b.push_back(static_cast<std::uint8_t>(v >> 8));
        b.push_back(static_cast<std::uint8_t>(v & 0xFF));
    }
}

void put32(std::vector<std::uint8_t>& b, std::uint32_t v, bool littleEndian)
{
    for (int i = 0; i < 4; ++i) {
        const int shift = littleEndian ? (8 * i) : (8 * (3 - i));
        b.push_back(static_cast<std::uint8_t>((v >> shift) & 0xFF));
    }
}

struct Tag {
    std::uint16_t tag;
    std::uint16_t type;
    std::uint32_t count;
    std::uint32_t value;
};

/// A directory entry's inline value field is four bytes wide whatever the type,
/// so a SHORT is written into the first two and the rest is padding.
void putValueField(std::vector<std::uint8_t>& b, const Tag& t, bool littleEndian)
{
    if (t.type == 3 && t.count == 1) {
        put16(b, static_cast<std::uint16_t>(t.value), littleEndian);
        put16(b, 0, littleEndian);
    } else {
        put32(b, t.value, littleEndian);
    }
}

/// Builds a TIFF from a tag list, patching the strip offset to point past the
/// directory, and appends `pixelBytes` of sample data there.
///
/// - Parameters:
///   - tags: The directory entries, in the order they appear.
///   - pixelBytes: How many sample bytes to append.
///   - littleEndian: Which byte order to write.
/// - Returns: The whole file.
std::vector<std::uint8_t> buildTIFF(std::vector<Tag> tags, int pixelBytes, bool littleEndian = true)
{
    std::vector<std::uint8_t> b;
    b.push_back(littleEndian ? 'I' : 'M');
    b.push_back(littleEndian ? 'I' : 'M');
    put16(b, 42, littleEndian);
    put32(b, 8, littleEndian); // the directory follows the header

    const std::uint32_t pixelOffset = 8 + 2 + static_cast<std::uint32_t>(tags.size()) * 12 + 4;
    for (Tag& t : tags) {
        if (t.tag == 273 && t.value == 0) // StripOffsets, unset → point at the data
            t.value = pixelOffset;
    }

    put16(b, static_cast<std::uint16_t>(tags.size()), littleEndian);
    for (const Tag& t : tags) {
        put16(b, t.tag, littleEndian);
        put16(b, t.type, littleEndian);
        put32(b, t.count, littleEndian);
        putValueField(b, t, littleEndian);
    }
    put32(b, 0, littleEndian); // no next directory

    for (int i = 0; i < pixelBytes; ++i)
        b.push_back(static_cast<std::uint8_t>(0x11 * ((i % 15) + 1)));
    return b;
}

/// The tag set for a well-formed `width` x `height` image, minus nothing.
std::vector<Tag> baselineTags(int width, int height, int samplesPerPixel)
{
    return {
        { 256, 3, 1, static_cast<std::uint32_t>(width) }, // ImageWidth
        { 257, 3, 1, static_cast<std::uint32_t>(height) }, // ImageLength
        { 258, 3, 1, 8 }, // BitsPerSample
        { 259, 3, 1, 1 }, // Compression: none
        { 262, 3, 1, 2 }, // Photometric: RGB
        { 273, 4, 1, 0 }, // StripOffsets, patched
        { 277, 3, 1, static_cast<std::uint32_t>(samplesPerPixel) }, // SamplesPerPixel
        { 278, 3, 1, static_cast<std::uint32_t>(height) }, // RowsPerStrip
        { 279, 4, 1, static_cast<std::uint32_t>(width * height * samplesPerPixel) }, // StripByteCounts
        { 284, 3, 1, 1 }, // PlanarConfig: chunky
    };
}

/// Removes one tag, for the cases about a directory that omits it.
std::vector<Tag> without(std::vector<Tag> tags, std::uint16_t tag)
{
    std::vector<Tag> kept;
    for (const Tag& t : tags) {
        if (t.tag != tag)
            kept.push_back(t);
    }
    return kept;
}

/// Replaces one tag's value.
std::vector<Tag> with(std::vector<Tag> tags, std::uint16_t tag, std::uint32_t value)
{
    for (Tag& t : tags) {
        if (t.tag == tag)
            t.value = value;
    }
    return tags;
}

struct Decoded {
    DecodeStatus status;
    int width;
    int height;
    std::vector<std::uint8_t> scratch;
};

/// Decodes into a poisoned buffer, so a byte the decoder did not write is
/// visible as one it did not write.
Decoded decodeInto(const std::vector<std::uint8_t>& bytes, std::size_t scratchBytes = 4096)
{
    std::vector<std::uint8_t> scratch(scratchBytes, poison);
    const DecodeResult r = decode(std::span<const std::uint8_t>(bytes.data(), bytes.size()),
        std::span<std::uint8_t>(scratch.data(), scratch.size()));
    return { r.status, r.width, r.height, std::move(scratch) };
}

/// Whether every byte of `count` from the start is still the poison value.
bool untouched(const std::vector<std::uint8_t>& scratch, std::size_t count)
{
    for (std::size_t i = 0; i < count && i < scratch.size(); ++i) {
        if (scratch[i] != poison)
            return false;
    }
    return true;
}

} // namespace

TEST(testAWellFormedImageDecodesInBothByteOrders)
{
    // The control every refusal below is measured against. Both byte orders,
    // because the readers are separate expressions and one can be written
    // backwards without the other noticing.
    for (bool littleEndian : { true, false }) {
        const std::vector<std::uint8_t> bytes = buildTIFF(baselineTags(2, 2, 3), 2 * 2 * 3, littleEndian);
        CHECK(isTIFF(std::span<const std::uint8_t>(bytes.data(), bytes.size())));

        const Decoded d = decodeInto(bytes);
        CHECK(d.status == DecodeStatus::Success);
        CHECK(d.width == 2 && d.height == 2);
        // Three samples in, four out, with the alpha the decoder supplies.
        CHECK(d.scratch[0] == 0x11 && d.scratch[1] == 0x22 && d.scratch[2] == 0x33);
        CHECK_MSG(d.scratch[3] == 255, "a three-sample image gets an opaque alpha");
        CHECK_MSG(!untouched(d.scratch, 2 * 2 * 4), "every pixel was written");
    }

    // Four samples are copied straight through, alpha included.
    std::vector<Tag> rgba = with(baselineTags(2, 2, 4), 279, 2 * 2 * 4);
    const std::vector<std::uint8_t> bytes = buildTIFF(rgba, 2 * 2 * 4);
    const Decoded d = decodeInto(bytes);
    CHECK(d.status == DecodeStatus::Success);
    CHECK(d.scratch[3] == 0x44);
}

// Fails-without-it: a TIFF that omits RowsPerStrip reports Success and returns
// the scratch buffer exactly as it arrived — the previous image, or whatever the
// allocation held.
TEST(testAnImageWithNoRowsPerStripIsStillOneStrip)
{
    // The baseline's default for the tag is 2^32-1, which means one strip covers
    // the whole image. Rounding the strip count up from that overflows to zero
    // strips, and a loop that runs zero times writes nothing while the caller is
    // told how many pixels to read.
    const std::vector<std::uint8_t> bytes = buildTIFF(without(baselineTags(2, 2, 3), 278), 2 * 2 * 3);
    const Decoded d = decodeInto(bytes);

    CHECK(d.status == DecodeStatus::Success);
    CHECK(d.width == 2 && d.height == 2);
    CHECK_MSG(!untouched(d.scratch, 2 * 2 * 4), "the pixels were written rather than left as they were");
    CHECK(d.scratch[0] == 0x11 && d.scratch[3] == 255);

    // The same image with the tag present decodes to the same bytes, which is
    // what makes the omission a default rather than a different picture.
    const Decoded stated = decodeInto(buildTIFF(baselineTags(2, 2, 3), 2 * 2 * 3));
    CHECK(stated.status == DecodeStatus::Success);
    for (int i = 0; i < 2 * 2 * 4; ++i)
        CHECK(d.scratch[i] == stated.scratch[i]);

    // A tag larger than the image is the same case by another route, and zero
    // means the whole image too. Each of these leaves one strip, which is what
    // this fixture's single offset describes — a smaller value needs more
    // offsets, and that is the case below.
    for (std::uint32_t rows : { 0u, 2u, 3u, 1000u, 0xFFFFFFFFu }) {
        const Decoded any = decodeInto(buildTIFF(with(baselineTags(2, 2, 3), 278, rows), 2 * 2 * 3));
        CHECK_MSG(any.status == DecodeStatus::Success, "every rows-per-strip decodes");
        CHECK_MSG(!untouched(any.scratch, 2 * 2 * 4), "and writes every pixel");
    }
}

TEST(testAnythingThatIsNotATIFFIsRefusedBeforeItIsRead)
{
    // isTIFF is what a caller sniffs with, so it has to answer for the same
    // inputs the decoder does.
    const std::vector<std::uint8_t> good = buildTIFF(baselineTags(1, 1, 3), 3);

    for (const std::vector<std::uint8_t>& bad : {
             std::vector<std::uint8_t> {},
             std::vector<std::uint8_t> { 'I' },
             std::vector<std::uint8_t> { 'I', 'I', 42, 0, 8, 0, 0 }, // one byte short of a header
             std::vector<std::uint8_t> { 'X', 'X', 42, 0, 8, 0, 0, 0 }, // no byte-order mark
             std::vector<std::uint8_t> { 'I', 'M', 42, 0, 8, 0, 0, 0 }, // mismatched marks
             std::vector<std::uint8_t> { 'I', 'I', 43, 0, 8, 0, 0, 0 }, // not the magic 42
             std::vector<std::uint8_t> { 'M', 'M', 0, 43, 0, 0, 0, 8 }, // likewise, big-endian
         }) {
        CHECK(!isTIFF(std::span<const std::uint8_t>(bad.data(), bad.size())));
        const Decoded d = decodeInto(bad);
        CHECK(d.status == DecodeStatus::Truncated || d.status == DecodeStatus::BadSignature);
        CHECK_MSG(untouched(d.scratch, 64), "a refusal writes nothing");
    }

    // Both spellings of the magic are accepted, so the refusals are about the
    // bytes rather than about the check rejecting everything.
    CHECK(isTIFF(std::span<const std::uint8_t>(good.data(), good.size())));
    const std::vector<std::uint8_t> bigEndian = buildTIFF(baselineTags(1, 1, 3), 3, false);
    CHECK(isTIFF(std::span<const std::uint8_t>(bigEndian.data(), bigEndian.size())));

    // No scratch is a refusal rather than a write to a null pointer.
    std::vector<std::uint8_t> empty;
    const DecodeResult noRoom = decode(std::span<const std::uint8_t>(good.data(), good.size()),
        std::span<std::uint8_t>(empty.data(), 0));
    CHECK(noRoom.status == DecodeStatus::Truncated);
}

TEST(testADirectoryThatDoesNotFitIsRefused)
{
    const std::vector<std::uint8_t> good = buildTIFF(baselineTags(2, 2, 3), 2 * 2 * 3);

    // The directory offset points past the end. Written as the largest value the
    // field holds, because that is the one that overflows a sum done in 32 bits.
    std::vector<std::uint8_t> farOffset = good;
    for (int i = 0; i < 4; ++i)
        farOffset[4 + i] = 0xFF;
    CHECK(decodeInto(farOffset).status == DecodeStatus::Truncated);

    // Just past the end, so the refusal is about the bound rather than only
    // about the extreme.
    std::vector<std::uint8_t> pastEnd = good;
    const std::uint32_t justPast = static_cast<std::uint32_t>(good.size());
    for (int i = 0; i < 4; ++i)
        pastEnd[4 + i] = static_cast<std::uint8_t>((justPast >> (8 * i)) & 0xFF);
    CHECK(decodeInto(pastEnd).status == DecodeStatus::Truncated);

    // An entry count larger than the entries that follow it.
    std::vector<std::uint8_t> tooManyEntries = good;
    tooManyEntries[8] = 0xFF;
    tooManyEntries[9] = 0x00;
    CHECK(decodeInto(tooManyEntries).status == DecodeStatus::Truncated);

    // A file that stops inside the header.
    for (std::size_t cut : { std::size_t(0), std::size_t(1), std::size_t(7) }) {
        const std::vector<std::uint8_t> stub(good.begin(), good.begin() + static_cast<long>(cut));
        CHECK(decodeInto(stub).status == DecodeStatus::Truncated);
    }
}

TEST(testDimensionsOutsideWhatCanBeDrawnAreRefused)
{
    // Zero in either direction is not an image, and the ceiling is what keeps a
    // width times height times four from being computed on a number no buffer
    // can hold.
    for (std::uint32_t width : { 0u, 0x8000u, 0xFFFFu }) {
        const Decoded d = decodeInto(buildTIFF(with(baselineTags(2, 2, 3), 256, width), 2 * 2 * 3));
        CHECK_MSG(d.status == DecodeStatus::BadDimensions, "a width outside the range is refused");
        CHECK(untouched(d.scratch, 64));
    }
    for (std::uint32_t height : { 0u, 0x8000u, 0xFFFFu }) {
        const Decoded d = decodeInto(buildTIFF(with(baselineTags(2, 2, 3), 257, height), 2 * 2 * 3));
        CHECK_MSG(d.status == DecodeStatus::BadDimensions, "a height outside the range is refused");
    }

    // A directory with neither dimension tag leaves both at zero.
    CHECK(decodeInto(buildTIFF(without(without(baselineTags(2, 2, 3), 256), 257), 12)).status
        == DecodeStatus::BadDimensions);

    // The largest accepted dimension is one below the ceiling, so the bound is
    // measured from both sides rather than from one.
    const Decoded wide = decodeInto(buildTIFF(with(baselineTags(2, 2, 3), 256, 0x7FFFu), 2 * 2 * 3));
    CHECK_MSG(wide.status != DecodeStatus::BadDimensions, "0x7FFF is inside the range");
}

TEST(testAFeatureTheDecoderDoesNotCarryIsRefusedRatherThanGuessed)
{
    // Each of these is a valid TIFF this decoder does not read. Answering
    // Unsupported is what keeps it from reading compressed bytes as samples.
    struct Case {
        const char* name;
        std::uint16_t tag;
        std::uint32_t value;
    };
    for (const Case& c : { Case { "compressed", 259, 5u },
             Case { "packbits", 259, 32773u },
             Case { "planar", 284, 2u },
             Case { "greyscale", 262, 1u },
             Case { "palette", 262, 3u },
             Case { "one sample", 277, 1u },
             Case { "two samples", 277, 2u },
             Case { "five samples", 277, 5u } }) {
        const Decoded d = decodeInto(buildTIFF(with(baselineTags(2, 2, 3), c.tag, c.value), 2 * 2 * 3));
        CHECK_MSG(d.status == DecodeStatus::Unsupported, c.name);
        CHECK_MSG(untouched(d.scratch, 64), c.name);
    }

    // Strip data the directory never located.
    CHECK(decodeInto(buildTIFF(without(baselineTags(2, 2, 3), 273), 2 * 2 * 3)).status
        == DecodeStatus::Unsupported);
    CHECK(decodeInto(buildTIFF(without(baselineTags(2, 2, 3), 279), 2 * 2 * 3)).status
        == DecodeStatus::Unsupported);

    // Samples that are not eight bits, and a bit depth array the decoder cannot
    // read the type of.
    for (std::uint32_t bits : { 1u, 4u, 16u, 32u }) {
        const Decoded d = decodeInto(buildTIFF(with(baselineTags(2, 2, 3), 258, bits), 2 * 2 * 3));
        CHECK_MSG(d.status == DecodeStatus::Unsupported, "only eight-bit samples are read");
    }
    std::vector<Tag> oddType = baselineTags(2, 2, 3);
    for (Tag& t : oddType) {
        if (t.tag == 258)
            t.type = 11; // FLOAT, which this reader has no size for
    }
    CHECK(decodeInto(buildTIFF(oddType, 2 * 2 * 3)).status == DecodeStatus::Unsupported);

    // A bit-depth tag absent entirely is taken as the baseline eight, so the
    // refusals above are about the value rather than about the tag being read
    // at all.
    CHECK(decodeInto(buildTIFF(without(baselineTags(2, 2, 3), 258), 2 * 2 * 3)).status
        == DecodeStatus::Success);
}

TEST(testAnImageLargerThanTheBufferIsRefusedBeforeAnythingIsWritten)
{
    // The caller states its capacity and the decoder has to answer against it,
    // not against what the file claims.
    const std::vector<std::uint8_t> bytes = buildTIFF(baselineTags(16, 16, 3), 16 * 16 * 3);
    const Decoded small = decodeInto(bytes, 16 * 16 * 4 - 1);
    CHECK(small.status == DecodeStatus::ScratchTooSmall);
    CHECK_MSG(untouched(small.scratch, small.scratch.size()), "nothing is written before the size is checked");

    // Exactly enough is enough.
    const Decoded exact = decodeInto(bytes, 16 * 16 * 4);
    CHECK(exact.status == DecodeStatus::Success);
    CHECK(!untouched(exact.scratch, 16 * 16 * 4));
}

TEST(testStripDataOutsideTheFileIsRefused)
{
    // The strip offset and byte count come out of the file, so they point
    // wherever the file says — including past its own end.
    std::vector<Tag> farStrip = with(baselineTags(2, 2, 3), 273, 0xFFFFFF00u);
    CHECK(decodeInto(buildTIFF(farStrip, 2 * 2 * 3)).status == DecodeStatus::Truncated);

    std::vector<Tag> maxStrip = with(baselineTags(2, 2, 3), 273, 0xFFFFFFFFu);
    CHECK(decodeInto(buildTIFF(maxStrip, 2 * 2 * 3)).status == DecodeStatus::Truncated);

    // A file whose sample data is shorter than the dimensions promise.
    CHECK(decodeInto(buildTIFF(baselineTags(4, 4, 3), 4 * 4 * 3 - 1)).status == DecodeStatus::Truncated);

    // More strips than the offsets array holds. Four rows at one row per strip
    // needs four offsets, and the directory declares one.
    std::vector<Tag> tooFewOffsets = with(baselineTags(4, 4, 3), 278, 1);
    CHECK(decodeInto(buildTIFF(tooFewOffsets, 4 * 4 * 3)).status == DecodeStatus::Truncated);
}

TEST(testAMultiStripImageIsAssembledInOrder)
{
    // Strip offsets and counts live in an array outside the directory once
    // there is more than one, which is a separate read path from the inline
    // single-strip case every test above uses.
    const int width = 2, height = 4, samples = 3;
    const int rowBytes = width * samples;

    // Two strips of two rows each: the arrays follow the directory, then the
    // sample data.
    std::vector<Tag> tags = {
        { 256, 3, 1, width },
        { 257, 3, 1, height },
        { 258, 3, 1, 8 },
        { 259, 3, 1, 1 },
        { 262, 3, 1, 2 },
        { 273, 4, 2, 0 }, // StripOffsets: two LONGs, offset patched below
        { 277, 3, 1, samples },
        { 278, 3, 1, 2 }, // two rows per strip
        { 279, 4, 2, 0 }, // StripByteCounts: two LONGs
        { 284, 3, 1, 1 },
    };
    const std::uint32_t afterDirectory = 8 + 2 + static_cast<std::uint32_t>(tags.size()) * 12 + 4;
    const std::uint32_t offsetsAt = afterDirectory;
    const std::uint32_t countsAt = offsetsAt + 8;
    const std::uint32_t pixelsAt = countsAt + 8;
    for (Tag& t : tags) {
        if (t.tag == 273)
            t.value = offsetsAt;
        if (t.tag == 279)
            t.value = countsAt;
    }

    std::vector<std::uint8_t> b = buildTIFF(tags, 0);
    put32(b, pixelsAt, true); // strip 0 offset
    put32(b, pixelsAt + static_cast<std::uint32_t>(rowBytes) * 2, true); // strip 1
    put32(b, static_cast<std::uint32_t>(rowBytes) * 2, true); // strip 0 bytes
    put32(b, static_cast<std::uint32_t>(rowBytes) * 2, true);
    for (int i = 0; i < width * height * samples; ++i)
        b.push_back(static_cast<std::uint8_t>(i + 1));

    const Decoded d = decodeInto(b);
    CHECK(d.status == DecodeStatus::Success);
    CHECK(d.width == width && d.height == height);
    CHECK_MSG(!untouched(d.scratch, width * height * 4), "every row was written");
    // The second strip's first row is the third row of the image, so a decoder
    // that assembled them out of order shows up here rather than in a total.
    CHECK(d.scratch[0] == 1);
    CHECK_MSG(d.scratch[2 * width * 4] == static_cast<std::uint8_t>(2 * rowBytes + 1),
        "the second strip lands after the first");

    // An offsets array that runs off the end of the file is refused rather than
    // read past.
    std::vector<Tag> pastEnd = tags;
    for (Tag& t : pastEnd) {
        if (t.tag == 273)
            t.value = static_cast<std::uint32_t>(b.size()) - 2;
    }
    std::vector<std::uint8_t> broken = buildTIFF(pastEnd, 0);
    broken.insert(broken.end(), b.begin() + static_cast<long>(afterDirectory), b.end());
    CHECK(decodeInto(broken).status == DecodeStatus::Truncated);
}

TEST(testHalfAByteOrderMarkIsNotAByteOrder)
{
    // Both marks are two identical bytes, so a file starting with one of each
    // is neither — and each half is a separate comparison that could be written
    // against the wrong index.
    for (const char* mark : { "MI", "IM", "Mx", "Ix" }) {
        std::vector<std::uint8_t> bytes = buildTIFF(baselineTags(2, 2, 3), 2 * 2 * 3);
        bytes[0] = static_cast<std::uint8_t>(mark[0]);
        bytes[1] = static_cast<std::uint8_t>(mark[1]);
        CHECK_MSG(!isTIFF(std::span<const std::uint8_t>(bytes.data(), bytes.size())), mark);
        CHECK_MSG(decodeInto(bytes).status == DecodeStatus::BadSignature, mark);
    }
}

TEST(testATagTheDecoderDoesNotReadIsSkipped)
{
    // A real TIFF carries tags this decoder has no interest in — resolution,
    // software, dates. Walking past them is what lets an ordinary file decode
    // rather than only a file trimmed to what is read.
    std::vector<Tag> withExtras = baselineTags(2, 2, 3);
    withExtras.push_back({ 254, 4, 1, 0 }); // NewSubfileType
    withExtras.push_back({ 296, 3, 1, 2 }); // ResolutionUnit
    withExtras.push_back({ 305, 3, 1, 0 }); // Software
    withExtras.push_back({ 65000, 3, 1, 7 }); // a private tag

    const Decoded d = decodeInto(buildTIFF(withExtras, 2 * 2 * 3));
    CHECK(d.status == DecodeStatus::Success);
    CHECK(d.width == 2 && d.height == 2);
    CHECK(d.scratch[0] == 0x11 && d.scratch[3] == 255);
}

TEST(testALastStripShorterThanTheRestIsNotOverread)
{
    // Rows per strip rarely divides the height, so the final strip is short.
    // Reading a whole strip's worth from it would run past the sample data and
    // into whatever follows.
    const int width = 2, samples = 3, height = 3, rowBytes = width * samples;
    std::vector<Tag> tags = {
        { 256, 3, 1, width },
        { 257, 3, 1, height },
        { 258, 3, 1, 8 },
        { 259, 3, 1, 1 },
        { 262, 3, 1, 2 },
        { 273, 4, 2, 0 }, // two strips
        { 277, 3, 1, samples },
        { 278, 3, 1, 2 }, // two rows per strip: strips of 2 and 1
        { 279, 4, 2, 0 },
        { 284, 3, 1, 1 },
    };
    const std::uint32_t afterDirectory = 8 + 2 + static_cast<std::uint32_t>(tags.size()) * 12 + 4;
    const std::uint32_t offsetsAt = afterDirectory;
    const std::uint32_t countsAt = offsetsAt + 8;
    const std::uint32_t pixelsAt = countsAt + 8;
    for (Tag& t : tags) {
        if (t.tag == 273)
            t.value = offsetsAt;
        if (t.tag == 279)
            t.value = countsAt;
    }

    std::vector<std::uint8_t> b = buildTIFF(tags, 0);
    put32(b, pixelsAt, true);
    put32(b, pixelsAt + static_cast<std::uint32_t>(rowBytes) * 2, true);
    put32(b, static_cast<std::uint32_t>(rowBytes) * 2, true);
    put32(b, static_cast<std::uint32_t>(rowBytes), true);
    for (int i = 0; i < width * height * samples; ++i)
        b.push_back(static_cast<std::uint8_t>(i + 1));

    const Decoded d = decodeInto(b);
    CHECK(d.status == DecodeStatus::Success);
    CHECK(d.height == 3);
    CHECK_MSG(!untouched(d.scratch, width * height * 4), "all three rows were written");
    // The third row is the short strip's only row.
    CHECK(d.scratch[2 * width * 4] == static_cast<std::uint8_t>(2 * rowBytes + 1));
    // And nothing past the image was touched.
    CHECK_MSG(d.scratch[width * height * 4] == poison, "the row after the image is untouched");
}

TEST(testAnArrayLivingOutsideTheDirectoryIsReadAtItsOwnWidth)
{
    // A tag whose values do not fit the four-byte field points at an array
    // elsewhere in the file, and the elements there are read at the type's own
    // width. A three-sample bit depth is six bytes, so it takes that path — the
    // only tag in a baseline RGB file that does.
    const int width = 2, height = 2, samples = 3;
    std::vector<Tag> tags = {
        { 256, 3, 1, width },
        { 257, 3, 1, height },
        { 258, 3, 3, 0 }, // BitsPerSample: three SHORTs, offset patched
        { 259, 3, 1, 1 },
        { 262, 3, 1, 2 },
        { 273, 4, 1, 0 },
        { 277, 3, 1, samples },
        { 278, 3, 1, height },
        { 279, 4, 1, width * height * samples },
        { 284, 3, 1, 1 },
    };
    const std::uint32_t afterDirectory = 8 + 2 + static_cast<std::uint32_t>(tags.size()) * 12 + 4;
    const std::uint32_t bitsAt = afterDirectory;
    const std::uint32_t pixelsAt = bitsAt + 6;
    for (Tag& t : tags) {
        if (t.tag == 258)
            t.value = bitsAt;
        if (t.tag == 273)
            t.value = pixelsAt;
    }

    std::vector<std::uint8_t> b = buildTIFF(tags, 0);
    for (int i = 0; i < 3; ++i)
        put16(b, 8, true);
    for (int i = 0; i < width * height * samples; ++i)
        b.push_back(static_cast<std::uint8_t>(0x11 * ((i % 15) + 1)));

    const Decoded d = decodeInto(b);
    CHECK_MSG(d.status == DecodeStatus::Success, "three eight-bit samples read from an array");
    CHECK(d.scratch[0] == 0x11 && d.scratch[3] == 255);

    // One sample of a different depth in that array is still refused, so the
    // read above is per element rather than a look at the first.
    std::vector<std::uint8_t> mixed = b;
    mixed[bitsAt + 2] = 16;
    CHECK(decodeInto(mixed).status == DecodeStatus::Unsupported);

    // An array pointing past the end of the file is refused rather than read.
    std::vector<Tag> farBits = tags;
    for (Tag& t : farBits) {
        if (t.tag == 258)
            t.value = 0xFFFFFF00u;
    }
    std::vector<std::uint8_t> far = buildTIFF(farBits, 0);
    far.insert(far.end(), b.begin() + static_cast<long>(afterDirectory), b.end());
    CHECK(decodeInto(far).status == DecodeStatus::Unsupported);

    // More bit depths than there are samples: the loop stops at the samples,
    // which is the bound that decides how many are checked.
    std::vector<Tag> extraBits = tags;
    for (Tag& t : extraBits) {
        if (t.tag == 258)
            t.count = 4;
    }
    std::vector<std::uint8_t> withFour = buildTIFF(extraBits, 0);
    for (int i = 0; i < 4; ++i)
        put16(withFour, 8, true);
    for (int i = 0; i < width * height * samples; ++i)
        withFour.push_back(static_cast<std::uint8_t>(0x11 * ((i % 15) + 1)));
    // The strip offset moved: the array is two bytes longer than before.
    for (Tag& t : extraBits) {
        if (t.tag == 273)
            t.value = bitsAt + 8;
    }
    std::vector<std::uint8_t> rebuilt = buildTIFF(extraBits, 0);
    rebuilt.insert(rebuilt.end(), withFour.begin() + static_cast<long>(afterDirectory), withFour.end());
    CHECK_MSG(decodeInto(rebuilt).status == DecodeStatus::Success, "extra bit depths are ignored");
}

TEST(testStripCountsAreCheckedSeparatelyFromStripOffsets)
{
    // The two arrays are declared independently, so a file can name enough
    // offsets and too few counts. Both are read per strip, and a short counts
    // array must stop the decode rather than reading whatever follows it.
    const int width = 2, samples = 3, height = 4, rowBytes = width * samples;
    std::vector<Tag> tags = {
        { 256, 3, 1, width },
        { 257, 3, 1, height },
        { 258, 3, 1, 8 },
        { 259, 3, 1, 1 },
        { 262, 3, 1, 2 },
        { 273, 4, 2, 0 }, // two offsets
        { 277, 3, 1, samples },
        { 278, 3, 1, 2 },
        { 279, 4, 1, 0 }, // one count, for two strips
        { 284, 3, 1, 1 },
    };
    const std::uint32_t afterDirectory = 8 + 2 + static_cast<std::uint32_t>(tags.size()) * 12 + 4;
    const std::uint32_t offsetsAt = afterDirectory;
    const std::uint32_t pixelsAt = offsetsAt + 8;
    for (Tag& t : tags) {
        if (t.tag == 273)
            t.value = offsetsAt;
        if (t.tag == 279)
            t.value = static_cast<std::uint32_t>(rowBytes) * 2;
    }

    std::vector<std::uint8_t> b = buildTIFF(tags, 0);
    put32(b, pixelsAt, true);
    put32(b, pixelsAt + static_cast<std::uint32_t>(rowBytes) * 2, true);
    for (int i = 0; i < width * height * samples; ++i)
        b.push_back(static_cast<std::uint8_t>(i + 1));

    const Decoded d = decodeInto(b);
    CHECK_MSG(d.status == DecodeStatus::Truncated, "too few strip counts stops the decode");
    CHECK(untouched(d.scratch, 64));

    // The same file with both arrays the right length decodes, so the refusal
    // is about the counts rather than about this shape.
    std::vector<Tag> matched = tags;
    for (Tag& t : matched) {
        if (t.tag == 279) {
            t.count = 2;
            t.value = pixelsAt + 8; // the counts array follows the offsets
        }
    }
    const std::uint32_t countsAt = offsetsAt + 8;
    const std::uint32_t movedPixels = countsAt + 8;
    for (Tag& t : matched) {
        if (t.tag == 273)
            t.value = offsetsAt;
        if (t.tag == 279)
            t.value = countsAt;
    }
    std::vector<std::uint8_t> ok = buildTIFF(matched, 0);
    put32(ok, movedPixels, true);
    put32(ok, movedPixels + static_cast<std::uint32_t>(rowBytes) * 2, true);
    put32(ok, static_cast<std::uint32_t>(rowBytes) * 2, true);
    put32(ok, static_cast<std::uint32_t>(rowBytes) * 2, true);
    for (int i = 0; i < width * height * samples; ++i)
        ok.push_back(static_cast<std::uint8_t>(i + 1));
    CHECK_MSG(decodeInto(ok).status == DecodeStatus::Success, "two counts for two strips decodes");

    // A counts array that runs off the end is refused where it is read, not
    // where it is declared.
    std::vector<Tag> farCounts = matched;
    for (Tag& t : farCounts) {
        if (t.tag == 279)
            t.value = 0xFFFFFF00u;
    }
    std::vector<std::uint8_t> broken = buildTIFF(farCounts, 0);
    broken.insert(broken.end(), ok.begin() + static_cast<long>(afterDirectory), ok.end());
    CHECK(decodeInto(broken).status == DecodeStatus::Truncated);
}

UBIQUITTY_TEST_MAIN()
