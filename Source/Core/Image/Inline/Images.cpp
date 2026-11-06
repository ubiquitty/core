// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// OSC 1337 File= inline image — decode + placement, per the published protocol
// at https://iterm2.com/documentation-images.html. Container → RGBA via the PAL
// image decoder when registered, else the internal portable decoders (PNG subset
// + baseline uncompressed TIFF). The payload decodes through the shared base64
// codec, the one OSC 52 uses.

#include "Core/Image/Inline/Images.h"
#include "Core/Codec/Base64.h"

#include "Core/Codec/PNGDecoder.h"
#include "Core/Codec/ZlibInflate.h"
#include "Core/Image/TIFFDecoder.h"
#include "Core/Image/TerminalImage.h"
#include "Core/Terminal.h"
#include "PAL/PAL.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <span>
#include <string_view>

namespace Ubiquitty::Image::Inline {

// Copy up to capacity-1 bytes of a non-terminated token into dest and
// NUL-terminate it.
static void copyTokenClamped(std::span<char> dest, std::string_view src)
{
    if (dest.empty())
        return;
    // One less than the destination, for the terminator the caller expects.
    const std::size_t take = std::min(src.size(), dest.size() - 1);
    std::memcpy(dest.data(), src.data(), take);
    dest[take] = 0;
}

// Container → RGBA via the internal portable decoders, used when no platform
// decoder is registered. Sniffs PNG (kitty's subset) and baseline uncompressed
// TIFF; other formats need the platform decoder.
static bool decodeContainerToRGBA(std::span<const std::uint8_t> bytes, unsigned char** rgbaOut, int* widthOut, int* heightOut)
{
    const int scratchCapacity = static_cast<int>(Ubiquitty::Image::maxDecodedImageBytes * 2UL + 4096UL);
    unsigned char* scratch = static_cast<unsigned char*>(std::malloc(static_cast<size_t>(scratchCapacity)));
    if (!scratch)
        return false;

    const unsigned char* srcPixels = nullptr;
    int width = 0, height = 0, bpp = 0;

    if (Ubiquitty::Image::TIFF::isTIFF(bytes)) {
        const Ubiquitty::Image::TIFF::DecodeResult res = Ubiquitty::Image::TIFF::decode(bytes, { scratch, static_cast<std::size_t>(scratchCapacity) });
        if (res.status != Ubiquitty::Image::TIFF::DecodeStatus::Success || res.pixels.empty() || res.width <= 0 || res.height <= 0) {
            std::free(scratch);
            return false;
        }
        srcPixels = res.pixels.data();
        width = res.width;
        height = res.height;
        bpp = 4; // TIFF decoder always emits RGBA
    } else {
        // Stack-owned per-call inflate state (~33 KB; the header sanctions stack use).
        Ubiquitty::Codec::Zlib::Inflater inflater;
        const Ubiquitty::Codec::PNG::DecodeResult res = Ubiquitty::Codec::PNG::decode(bytes, { scratch, static_cast<std::size_t>(scratchCapacity) }, inflater);
        if (res.status != Ubiquitty::Codec::PNG::DecodeStatus::Success || res.pixels.empty() || res.width <= 0 || res.height <= 0) {
            std::free(scratch);
            return false;
        }
        srcPixels = res.pixels.data();
        width = res.width;
        height = res.height;
        bpp = res.bytesPerPixel;
    }

    if (width <= 0 || height <= 0) {
        std::free(scratch);
        return false;
    }
    // Refuse rather than wrap: a decoder-controlled width*height that overflows
    // int, or whose RGBA bytes exceed the decoded-image cap, drops the unit
    // while the session carries on.
    const std::int64_t pixelCount = static_cast<std::int64_t>(width) * static_cast<std::int64_t>(height);
    if (pixelCount > static_cast<std::int64_t>(Ubiquitty::Image::maxDecodedImageBytes / 4)) {
        std::free(scratch);
        return false;
    }
    const int n = static_cast<int>(pixelCount);
    const std::size_t byteCount = static_cast<std::size_t>(n) * 4;
    unsigned char* rgba = static_cast<unsigned char*>(std::malloc(byteCount));
    if (!rgba) {
        std::free(scratch);
        return false;
    }
    if (bpp == 4) {
        std::memcpy(rgba, srcPixels, byteCount);
    } else {
        for (int i = 0; i < n; ++i) {
            rgba[i * 4 + 0] = srcPixels[i * 3 + 0];
            rgba[i * 4 + 1] = srcPixels[i * 3 + 1];
            rgba[i * 4 + 2] = srcPixels[i * 3 + 2];
            rgba[i * 4 + 3] = 255;
        }
    }
    std::free(scratch);
    *rgbaOut = rgba;
    *widthOut = width;
    *heightOut = height;
    return true;
}

// OSC 1337 ; File=<key=value;…>:<base64>. width/height accept N (cells), Npx
// (device pixels), N% (of the grid axis), or auto; preserveAspectRatio
// defaults to 1. inlineImageExtent states the sizing rule.
bool decodeImageContainer(std::span<const std::uint8_t> bytes, unsigned char** rgbaOut, int* widthOut, int* heightOut)
{
    const Ubiquitty::PAL::ImageDecodeCallback decodeImage = Ubiquitty::PAL::imageDecoder();
    if (!decodeImage)
        return decodeContainerToRGBA(bytes, rgbaOut, widthOut, heightOut);
    if (!decodeImage(bytes, rgbaOut, widthOut, heightOut))
        return false;
    // The platform decoder is held to the bound Core's own decoders keep.
    const std::int64_t pixelCount = static_cast<std::int64_t>(*widthOut) * static_cast<std::int64_t>(*heightOut);
    if (*widthOut <= 0 || *heightOut <= 0 || pixelCount > static_cast<std::int64_t>(Ubiquitty::Image::maxDecodedImageBytes / 4)) {
        std::free(*rgbaOut);
        *rgbaOut = nullptr;
        return false;
    }
    return true;
}

void handleInlineImage(Ubiquitty::Terminal& t, const char* arguments)
{
    const char* colon = std::strchr(arguments, ':');
    if (!colon || colon[1] == 0)
        return;
    const char* base64 = colon + 1;
    // `strlen` in `size_t`: narrowing to `int` here would truncate a payload
    // past INT_MAX into a small positive capacity and under-allocate below.
    const std::size_t base64Length = std::strlen(base64);
    if (base64Length == 0)
        return;

    // Pull the width / height / preserveAspectRatio tokens out of the key=value
    // list (everything before the ':').
    char widthToken[24] = { 0 };
    char heightToken[24] = { 0 };
    char preserveToken[8] = { 0 };
    const char* scan = arguments;
    while (scan < colon) {
        const char* semicolon = static_cast<const char*>(std::memchr(scan, ';', static_cast<size_t>(colon - scan)));
        const char* tokenEnd = semicolon ? semicolon : colon;
        const char* equals = static_cast<const char*>(std::memchr(scan, '=', static_cast<size_t>(tokenEnd - scan)));
        if (equals) {
            int keyLength = static_cast<int>(equals - scan);
            const char* value = equals + 1;
            int valueLength = static_cast<int>(tokenEnd - value);
            if (std::string_view(scan, static_cast<size_t>(keyLength)) == "width")
                copyTokenClamped(widthToken, { value, static_cast<std::size_t>(valueLength) });
            else if (std::string_view(scan, static_cast<size_t>(keyLength)) == "height")
                copyTokenClamped(heightToken, { value, static_cast<std::size_t>(valueLength) });
            else if (std::string_view(scan, static_cast<size_t>(keyLength)) == "preserveAspectRatio")
                copyTokenClamped(preserveToken, { value, static_cast<std::size_t>(valueLength) });
        }
        if (!semicolon)
            break;
        scan = semicolon + 1;
    }

    // Refuse rather than wrap or over-allocate: the decoded size is computed
    // in `size_t` and capped at the same container budget the scratch sizing
    // uses, so a hostile length drops the image while the session carries on.
    // No room for a byte is no container, refused before an allocation.
    const std::string_view encoded(base64, base64Length);
    const std::size_t rawCapacity = Codec::base64DecodedCapacity(encoded);
    constexpr std::size_t maxContainerBytes = static_cast<std::size_t>(Ubiquitty::Image::maxDecodedImageBytes) * 2 + 4096;
    if (!rawCapacity || rawCapacity > maxContainerBytes)
        return;
    unsigned char* rgbaPixels = nullptr;
    int pixelWidth = 0;
    int pixelHeight = 0;
    {
        // Scoped so the container is released once decoded, before the pixels
        // are stored.
        const std::unique_ptr<char[]> rawBytes(new (std::nothrow) char[rawCapacity]);
        if (!rawBytes)
            return;
        const auto raw = Codec::base64Decode(encoded, { rawBytes.get(), rawCapacity });
        if (!raw || raw->empty())
            return;
        if (!decodeImageContainer({ reinterpret_cast<const uint8_t*>(raw->data()), raw->size() }, &rgbaPixels, &pixelWidth, &pixelHeight))
            return;
    }
    if (!rgbaPixels)
        return;
    const int cursorColumn = t.screens.active ? t.screens.active->cursorX : 0;
    const Ubiquitty::Image::InlineImageExtent extent = Ubiquitty::Image::inlineImageExtent(widthToken, heightToken, preserveToken,
        { pixelWidth, pixelHeight }, { t.cellPixelWidth, t.cellPixelHeight }, { t.columns, t.rows }, cursorColumn);
    if (!Ubiquitty::Image::storeRGBAImage(&t, rgbaPixels, pixelWidth, pixelHeight, extent.placementCells.width, extent.placementCells.height, extent.drawnPixels)) {
        std::free(rgbaPixels);
        return;
    }
    Ubiquitty::Image::advanceCursorAfterImage(&t, extent.coveredCells, Ubiquitty::Image::CursorMotion::Advance);
}

} // namespace Ubiquitty::Image::Inline
