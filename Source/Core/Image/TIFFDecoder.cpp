// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Image/TIFFDecoder.h"
#include "Base/Types.h"

#include <algorithm>
#include <climits>
#include <cstdint>
#include <cstring>
#include <optional>

namespace Ubiquitty::Image::TIFF {

namespace {

/// Which byte order the file's header declared.
///
/// TIFF states it in the first two bytes: "II" for little-endian, "MM" for
/// big. A file declaring neither is not a TIFF, so there is no third case and
/// no default.
enum class ByteOrder : std::uint8_t {
    Big,
    Little
};

// Endian-aware readers over the input buffer.
uint16_t read16(const uint8_t* p, ByteOrder order)
{
    return order == ByteOrder::Little ? static_cast<uint16_t>(p[0] | (p[1] << 8))
                                      : static_cast<uint16_t>((p[0] << 8) | p[1]);
}

uint32_t read32(const uint8_t* p, ByteOrder order)
{
    return order == ByteOrder::Little ? (static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24))
                                      : (static_cast<uint32_t>(p[3]) | (static_cast<uint32_t>(p[2]) << 8) | (static_cast<uint32_t>(p[1]) << 16) | (static_cast<uint32_t>(p[0]) << 24));
}

// TIFF field types we read (SHORT and LONG cover the baseline tags here).
constexpr uint16_t typeShort = 3;
constexpr uint16_t typeLong = 4;

// Baseline tags.
constexpr uint16_t imageWidth = 256;
constexpr uint16_t imageLength = 257;
constexpr uint16_t bitsPerSample = 258;
constexpr uint16_t compressionTag = 259;
constexpr uint16_t photometricTag = 262;
constexpr uint16_t stripOffsets = 273;
constexpr uint16_t samplesPerPixelTag = 277;
constexpr uint16_t rowsPerStripTag = 278;
constexpr uint16_t stripByteCounts = 279;
constexpr uint16_t planarConfigTag = 284;

// A directory entry, resolved to its scalar value (count==1) or the file
// offset where its array lives (count>1, value doesn't fit in 4 bytes).
struct Entry {
    uint16_t tag;
    uint16_t type;
    uint32_t count;
    uint32_t valueOrOffset; // inline value (count*size<=4) or array offset
};

int typeSize(uint16_t type)
{
    switch (type) {
    case typeShort:
        return 2;
    case typeLong:
        return 4;
    default:
        return 0; // type we don't read
    }
}

// Read the i-th element of a tag's value array (handles inline vs offset and
// SHORT vs LONG). Caller guarantees i < count.
std::optional<uint32_t> readArrayElem(std::span<const std::uint8_t> in, ByteOrder order, const Entry& e, uint32_t i)
{
    const int sz = typeSize(e.type);
    if (sz == 0)
        return std::nullopt;
    // Sixty-four bits throughout: `valueOrOffset` fills a uint32, and on a
    // 32-bit target a plain `long` would narrow it to something negative rather
    // than to something the bound below rejects.
    const std::int64_t total = static_cast<std::int64_t>(sz) * static_cast<std::int64_t>(e.count);
    static thread_local uint8_t inlineBytes[4];
    const uint8_t* base;
    std::int64_t off = static_cast<std::int64_t>(i) * sz;
    std::int64_t limit;
    if (total <= 4) {
        // Inline: the value field holds the packed elements in file byte order.
        if (order == ByteOrder::Little) {
            inlineBytes[0] = static_cast<uint8_t>(e.valueOrOffset & 0xFF);
            inlineBytes[1] = static_cast<uint8_t>((e.valueOrOffset >> 8) & 0xFF);
            inlineBytes[2] = static_cast<uint8_t>((e.valueOrOffset >> 16) & 0xFF);
            inlineBytes[3] = static_cast<uint8_t>((e.valueOrOffset >> 24) & 0xFF);
        } else {
            inlineBytes[3] = static_cast<uint8_t>(e.valueOrOffset & 0xFF);
            inlineBytes[2] = static_cast<uint8_t>((e.valueOrOffset >> 8) & 0xFF);
            inlineBytes[1] = static_cast<uint8_t>((e.valueOrOffset >> 16) & 0xFF);
            inlineBytes[0] = static_cast<uint8_t>((e.valueOrOffset >> 24) & 0xFF);
        }
        base = inlineBytes;
        limit = 4;
    } else {
        base = in.data();
        off += static_cast<std::int64_t>(e.valueOrOffset);
        limit = static_cast<std::int64_t>(in.size());
    }
    // One bound for both, against the storage each one lives in.
    if (off + sz > limit)
        return std::nullopt;
    return (sz == 2) ? read16(base + off, order) : read32(base + off, order);
}

} // namespace

bool isTIFF(std::span<const uint8_t> bytes)
{
    if (bytes.size() < 8)
        return false;
    if (!(bytes[0] == 'I' && bytes[1] == 'I') && !(bytes[0] == 'M' && bytes[1] == 'M'))
        return false;
    const ByteOrder order = bytes[0] == 'I' ? ByteOrder::Little : ByteOrder::Big;
    return read16(bytes.data() + 2, order) == 42;
}

DecodeResult decode(std::span<const uint8_t> input, std::span<uint8_t> scratch)
{
    DecodeResult result;
    const uint8_t* in = input.data();
    const int inputLength = saturatingCast<int>(input.size());
    const int scratchCapacity = saturatingCast<int>(scratch.size());
    if (!in || inputLength < 8 || scratch.empty()) {
        result.status = DecodeStatus::Truncated;
        return result;
    }
    const bool isDeclaredOrder = (in[0] == 'I' && in[1] == 'I') || (in[0] == 'M' && in[1] == 'M');
    const ByteOrder order = in[0] == 'I' ? ByteOrder::Little : ByteOrder::Big;
    if (!isDeclaredOrder || read16(in + 2, order) != 42) {
        result.status = DecodeStatus::BadSignature;
        return result;
    }

    const uint32_t ifdOffset = read32(in + 4, order);
    // 64-bit throughout: `long` is 32 bits on LLP64, so a near-UINT32_MAX
    // ifdOffset would narrow negative rather than fail the bound below.
    if (static_cast<std::int64_t>(ifdOffset) + 2 > inputLength) {
        result.status = DecodeStatus::Truncated;
        return result;
    }
    const int entryCount = read16(in + ifdOffset, order);
    const std::int64_t entriesEnd = static_cast<std::int64_t>(ifdOffset) + 2 + static_cast<std::int64_t>(entryCount) * 12;
    if (entriesEnd > inputLength) {
        result.status = DecodeStatus::Truncated;
        return result;
    }

    // Defaults per the TIFF baseline.
    uint32_t width = 0, height = 0;
    uint32_t compression = 1;
    uint32_t photometric = 2;
    uint32_t samplesPerPixel = 1;
    uint32_t rowsPerStrip = 0xFFFFFFFFu;
    uint32_t planarConfig = 1;
    Entry bitsEntry { 0, 0, 0, 0 };
    Entry stripOffsetsEntry { 0, 0, 0, 0 };
    Entry stripCountsEntry { 0, 0, 0, 0 };
    bool haveBits = false, haveOffsets = false, haveCounts = false;

    for (int i = 0; i < entryCount; ++i) {
        const uint8_t* p = in + ifdOffset + 2 + i * 12;
        Entry e;
        e.tag = read16(p, order);
        e.type = read16(p + 2, order);
        e.count = read32(p + 4, order);
        e.valueOrOffset = read32(p + 8, order);
        // Scalar shortcut for the single-value tags.
        const uint32_t scalar = (e.type == typeShort) ? read16(p + 8, order) : e.valueOrOffset;
        switch (e.tag) {
        case imageWidth:
            width = scalar;
            break;
        case imageLength:
            height = scalar;
            break;
        case compressionTag:
            compression = scalar;
            break;
        case photometricTag:
            photometric = scalar;
            break;
        case samplesPerPixelTag:
            samplesPerPixel = scalar;
            break;
        case rowsPerStripTag:
            rowsPerStrip = scalar;
            break;
        case planarConfigTag:
            planarConfig = scalar;
            break;
        case bitsPerSample:
            bitsEntry = e;
            haveBits = true;
            break;
        case stripOffsets:
            stripOffsetsEntry = e;
            haveOffsets = true;
            break;
        case stripByteCounts:
            stripCountsEntry = e;
            haveCounts = true;
            break;
        default:
            break;
        }
    }

    if (width == 0 || height == 0 || width > 0x7FFFu || height > 0x7FFFu) {
        result.status = DecodeStatus::BadDimensions;
        return result;
    }
    if (compression != 1 || planarConfig != 1 || photometric != 2 || (samplesPerPixel != 3 && samplesPerPixel != 4) || !haveOffsets || !haveCounts) {
        result.status = DecodeStatus::Unsupported;
        return result;
    }
    // Every sample must be 8 bits.
    if (haveBits) {
        for (uint32_t s = 0; s < samplesPerPixel && s < bitsEntry.count; ++s) {
            const std::optional<uint32_t> bitsValue = readArrayElem(input, order, bitsEntry, s);
            if (!bitsValue || *bitsValue != 8) {
                result.status = DecodeStatus::Unsupported;
                return result;
            }
        }
    }

    const std::int64_t outBytes = static_cast<std::int64_t>(width) * static_cast<std::int64_t>(height) * 4;
    if (outBytes > scratchCapacity) {
        result.status = DecodeStatus::ScratchTooSmall;
        return result;
    }

    // The baseline default is 2^32-1, meaning one strip covers the image. Left
    // as it arrives, the round-up below overflows to zero strips, the copy loop
    // never runs, and the scratch buffer is handed back as pixels — whatever it
    // held before. Clamping to the height is also what the tag means.
    if (rowsPerStrip == 0 || rowsPerStrip > height)
        rowsPerStrip = height;
    const uint32_t stripCount = (height + rowsPerStrip - 1) / rowsPerStrip;
    if (stripOffsetsEntry.count < stripCount || stripCountsEntry.count < stripCount) {
        result.status = DecodeStatus::Truncated;
        return result;
    }

    const uint32_t srcStride = width * samplesPerPixel; // bytes per source row
    uint32_t row = 0;
    for (uint32_t s = 0; s < stripCount; ++s) {
        const std::optional<uint32_t> offsetValue = readArrayElem(input, order, stripOffsetsEntry, s);
        const std::optional<uint32_t> countValue = readArrayElem(input, order, stripCountsEntry, s);
        if (!offsetValue || !countValue) {
            result.status = DecodeStatus::Truncated;
            return result;
        }
        const uint32_t stripOffset = *offsetValue;
        // The strip's byte count is read to prove the entry is present and in
        // bounds, and not used: the bound below is computed from the stride and
        // the row count, which is the smaller of the two.
        uint32_t rowsThisStrip = rowsPerStrip;
        if (row + rowsThisStrip > height)
            rowsThisStrip = height - row;
        const std::int64_t need = static_cast<std::int64_t>(srcStride) * rowsThisStrip;
        if (static_cast<std::int64_t>(stripOffset) + need > inputLength) {
            result.status = DecodeStatus::Truncated;
            return result;
        }
        const uint8_t* src = in + stripOffset;
        for (uint32_t r = 0; r < rowsThisStrip; ++r) {
            const uint8_t* srow = src + static_cast<std::int64_t>(r) * srcStride;
            uint8_t* drow = scratch.data() + static_cast<std::int64_t>(row + r) * width * 4;
            if (samplesPerPixel == 4) {
                std::memcpy(drow, srow, static_cast<size_t>(width) * 4);
            } else {
                for (uint32_t x = 0; x < width; ++x) {
                    drow[x * 4 + 0] = srow[x * 3 + 0];
                    drow[x * 4 + 1] = srow[x * 3 + 1];
                    drow[x * 4 + 2] = srow[x * 3 + 2];
                    drow[x * 4 + 3] = 255;
                }
            }
        }
        row += rowsThisStrip;
    }

    result.width = static_cast<int>(width);
    result.height = static_cast<int>(height);
    result.pixels = { scratch.data(), static_cast<std::size_t>(outBytes) };
    result.status = DecodeStatus::Success;
    return result;
}

} // namespace Ubiquitty::Image::TIFF
