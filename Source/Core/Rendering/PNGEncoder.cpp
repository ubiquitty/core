// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// Source/Core/Rendering/PNGEncoder.cpp — see PNGEncoder.h.
//

#include "Core/Rendering/PNGEncoder.h"
#include "Core/Codec/PNGDecoder.h"

#include "Core/Codec/ZlibDeflate.h"

#include <algorithm>
#include <array>
#include <span>

namespace Ubiquitty::Rendering {

namespace {

void put32(std::vector<uint8_t>& out, uint32_t value)
{
    out.push_back(static_cast<uint8_t>(value >> 24));
    out.push_back(static_cast<uint8_t>(value >> 16));
    out.push_back(static_cast<uint8_t>(value >> 8));
    out.push_back(static_cast<uint8_t>(value));
}

// CRC-32 (reflected 0xEDB88320) table, built at compile time — immutable, so
// crc32() is race-free even when called from the Agent rasterizer + Linux paths.
constexpr std::array<uint32_t, 256> crc32Table = [] {
    std::array<uint32_t, 256> t {};
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int k = 0; k < 8; ++k)
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        t[i] = c;
    }
    return t;
}();

uint32_t crc32(std::span<const uint8_t> data)
{
    uint32_t c = 0xFFFFFFFFu;
    for (const uint8_t byte : data)
        c = crc32Table[(c ^ byte) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

void writeChunk(std::vector<uint8_t>& out, const char* type, const std::vector<uint8_t>& data)
{
    put32(out, static_cast<uint32_t>(data.size()));
    std::size_t crcStart = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), data.begin(), data.end());
    put32(out, crc32(std::span(out).subspan(crcStart)));
}

} // namespace

std::vector<uint8_t> encodePNG(std::span<const uint8_t> rgba, IntSize size)
{
    const int width = size.width;
    const int height = size.height;
    std::vector<uint8_t> out;
    // The buffer must hold exactly width * height pixels; a caller passing a
    // buffer that does not match the dimensions would be read past its end.
    if (width <= 0 || height <= 0 || rgba.size() != static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * rgbaChannelCount)
        return out;

    out.insert(out.end(), Codec::PNG::signature.begin(), Codec::PNG::signature.end());

    std::vector<uint8_t> ihdr;
    put32(ihdr, static_cast<uint32_t>(width));
    put32(ihdr, static_cast<uint32_t>(height));
    ihdr.push_back(8); // bit depth
    ihdr.push_back(6); // color type: truecolor + alpha (RGBA)
    ihdr.push_back(0); // compression: deflate
    ihdr.push_back(0); // filter: adaptive
    ihdr.push_back(0); // interlace: none
    writeChunk(out, "IHDR", ihdr);

    // Filtered scanlines: filter byte 0 (None) + the RGBA row. LZ77 already
    // captures the horizontal background runs within a row and the identical
    // blank rows (a row matches the one above at distance = rowBytes), so the
    // None filter compresses well without per-row filter heuristics.
    const std::size_t rowBytes = static_cast<std::size_t>(width) * rgbaChannelCount;
    std::vector<uint8_t> raw;
    raw.reserve(static_cast<std::size_t>(height) * (1 + rowBytes));
    for (int y = 0; y < height; ++y) {
        raw.push_back(0);
        const uint8_t* row = rgba.data() + static_cast<std::size_t>(y) * rowBytes;
        raw.insert(raw.end(), row, row + rowBytes);
    }

    writeChunk(out, "IDAT", Codec::Zlib::compress(raw));

    writeChunk(out, "IEND", std::vector<uint8_t>());
    return out;
}

} // namespace Ubiquitty::Rendering
