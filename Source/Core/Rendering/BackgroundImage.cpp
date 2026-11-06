// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// BackgroundImage.cpp — see BackgroundImage.h for the contract.
//

#include "Core/Rendering/BackgroundImage.h"

#include "Core/Image/Inline/Images.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>

// The composite is one sequence of float operations shared with the Linux
// Vulkan backdrop shader and compared byte for byte, so no
// multiply-add may be fused here where the shader's `precise` keeps it apart.
#if defined(__clang__)
#pragma STDC FP_CONTRACT OFF
#elif defined(__GNUC__)
#pragma GCC optimize("fp-contract=off")
#endif

namespace Ubiquitty::Rendering {

namespace {

// The IEC 61966-2-1 decode, in double so each table entry is the nearest
// float to the exact value.
double decodeSRGB(double encoded)
{
    return encoded <= 0.04045 ? encoded / 12.92 : std::pow((encoded + 0.055) / 1.055, 2.4);
}

SRGBTransferTables buildTables()
{
    SRGBTransferTables tables;
    for (int code = 0; code < 256; ++code)
        tables.decoded[static_cast<std::size_t>(code)] = static_cast<float>(decodeSRGB(code / 255.0));
    for (int code = 0; code < 255; ++code)
        tables.codeBoundaries[static_cast<std::size_t>(code)] = static_cast<float>(decodeSRGB((code + 0.5) / 255.0));
    for (int code = 0; code < 256; ++code)
        tables.fractions[static_cast<std::size_t>(code)] = static_cast<float>(code / 255.0);
    return tables;
}

// The eight-bit code a linear value rounds to: how many code boundaries lie at
// or below it.
std::uint8_t encodeLinear(const SRGBTransferTables& tables, float linear)
{
    const auto boundary = std::upper_bound(tables.codeBoundaries.begin(), tables.codeBoundaries.end(), linear);
    return static_cast<std::uint8_t>(boundary - tables.codeBoundaries.begin());
}

// How a texel past the image's edge is read: the Metal arms' two samplers.
enum class TexelAddressing : std::uint8_t {
    Repeat, // Tile: the image's neighbour across the edge
    ClampToZero, // every other mode: transparent black
};

struct LinearTexel {
    float red = 0.0f;
    float green = 0.0f;
    float blue = 0.0f;
    float alpha = 0.0f;
};

// One texel as a GPU's sRGB texture reads it: decoded to linear light and
// premultiplied by its alpha. Past the edge it is the image's neighbour under
// repeat addressing and transparent black otherwise.
LinearTexel texelAt(const BackgroundImagePixels& image, const SRGBTransferTables& tables, int x, int y, TexelAddressing addressing)
{
    if (addressing == TexelAddressing::Repeat) {
        x %= image.width;
        y %= image.height;
        if (x < 0)
            x += image.width;
        if (y < 0)
            y += image.height;
    } else if (x < 0 || y < 0 || x >= image.width || y >= image.height) {
        return {};
    }
    const std::size_t offset = (static_cast<std::size_t>(y) * static_cast<std::size_t>(image.width) + static_cast<std::size_t>(x)) * 4;
    const float alpha = tables.fractions[image.rgba[offset + 3]];
    return { tables.decoded[image.rgba[offset]] * alpha, tables.decoded[image.rgba[offset + 1]] * alpha,
        tables.decoded[image.rgba[offset + 2]] * alpha, alpha };
}

} // namespace

const SRGBTransferTables& srgbTransferTables()
{
    static const SRGBTransferTables tables = buildTables();
    return tables;
}

BackgroundImageSampling backgroundImageSampling(const BackgroundImagePlacement& placement)
{
    const float spanX = placement.rightInPixels - placement.leftInPixels;
    const float spanY = placement.bottomInPixels - placement.topInPixels;
    if (spanX <= 0.0f || spanY <= 0.0f)
        return {};
    return { placement.u0, placement.v0, (placement.u1 - placement.u0) / spanX, (placement.v1 - placement.v0) / spanY };
}

BackgroundImagePixels decodeBackgroundImage(std::span<const std::uint8_t> fileBytes)
{
    BackgroundImagePixels image;
    unsigned char* rgba = nullptr;
    int width = 0;
    int height = 0;
    if (fileBytes.empty() || !Image::Inline::decodeImageContainer(fileBytes, &rgba, &width, &height) || !rgba)
        return image;
    const std::unique_ptr<unsigned char, decltype(&std::free)> owned(rgba, &std::free);
    if (width <= 0 || height <= 0)
        return image;
    image.width = width;
    image.height = height;
    image.rgba.assign(owned.get(), owned.get() + static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4);
    return image;
}

void compositeBackgroundImage(const BackgroundImageLayer& layer, std::span<std::uint8_t> rgba, IntSize frame)
{
    if (!layer.isVisible() || frame.width <= 0 || frame.height <= 0
        || rgba.size() < static_cast<std::size_t>(frame.width) * static_cast<std::size_t>(frame.height) * 4)
        return;
    const BackgroundImagePixels& image = *layer.pixels;
    const BackgroundImagePlacement placement = backgroundImagePlacement(layer.mode, static_cast<float>(frame.width),
        static_cast<float>(frame.height), static_cast<float>(image.width), static_cast<float>(image.height));
    const float spanX = placement.rightInPixels - placement.leftInPixels;
    const float spanY = placement.bottomInPixels - placement.topInPixels;
    if (spanX <= 0.0f || spanY <= 0.0f)
        return;
    const BackgroundImageSampling sampling = backgroundImageSampling(placement);
    const TexelAddressing addressing = placement.shouldRepeat ? TexelAddressing::Repeat : TexelAddressing::ClampToZero;
    const SRGBTransferTables& tables = srgbTransferTables();
    const float opacity = std::min(layer.opacity, 1.0f);
    // The pixels whose centres the placed rectangle covers, as a rasteriser
    // decides coverage: left and top inclusive, right and bottom exclusive.
    const int firstX = std::max(static_cast<int>(std::ceil(placement.leftInPixels - 0.5f)), 0);
    const int lastX = std::min(static_cast<int>(std::ceil(placement.rightInPixels - 0.5f)), frame.width);
    const int firstY = std::max(static_cast<int>(std::ceil(placement.topInPixels - 0.5f)), 0);
    const int lastY = std::min(static_cast<int>(std::ceil(placement.bottomInPixels - 0.5f)), frame.height);
    for (int y = firstY; y < lastY; ++y) {
        const float v = sampling.v0 + (static_cast<float>(y) + 0.5f - placement.topInPixels) * sampling.vPerPixel;
        const float texelY = v * static_cast<float>(image.height) - 0.5f;
        const float floorY = std::floor(texelY);
        const float weightY = texelY - floorY;
        const int y0 = static_cast<int>(floorY);
        for (int x = firstX; x < lastX; ++x) {
            const float u = sampling.u0 + (static_cast<float>(x) + 0.5f - placement.leftInPixels) * sampling.uPerPixel;
            const float texelX = u * static_cast<float>(image.width) - 0.5f;
            const float floorX = std::floor(texelX);
            const float weightX = texelX - floorX;
            const int x0 = static_cast<int>(floorX);
            const LinearTexel a = texelAt(image, tables, x0, y0, addressing);
            const LinearTexel b = texelAt(image, tables, x0 + 1, y0, addressing);
            const LinearTexel c = texelAt(image, tables, x0, y0 + 1, addressing);
            const LinearTexel d = texelAt(image, tables, x0 + 1, y0 + 1, addressing);
            const auto bilinear = [&](float LinearTexel::* channel) {
                const float top = a.*channel + (b.*channel - a.*channel) * weightX;
                const float bottom = c.*channel + (d.*channel - c.*channel) * weightX;
                return (top + (bottom - top) * weightY) * opacity;
            };
            const std::size_t offset = (static_cast<std::size_t>(y) * static_cast<std::size_t>(frame.width) + static_cast<std::size_t>(x)) * 4;
            const float open = 1.0f - tables.fractions[rgba[offset + 3]];
            if (open <= 0.0f)
                continue;
            const float sourceAlpha = bilinear(&LinearTexel::alpha);
            if (sourceAlpha <= 0.0f)
                continue;
            rgba[offset] = encodeLinear(tables, tables.decoded[rgba[offset]] + bilinear(&LinearTexel::red) * open);
            rgba[offset + 1] = encodeLinear(tables, tables.decoded[rgba[offset + 1]] + bilinear(&LinearTexel::green) * open);
            rgba[offset + 2] = encodeLinear(tables, tables.decoded[rgba[offset + 2]] + bilinear(&LinearTexel::blue) * open);
            const float alpha = tables.fractions[rgba[offset + 3]] + sourceAlpha * open;
            rgba[offset + 3] = static_cast<std::uint8_t>(std::floor(std::clamp(alpha, 0.0f, 1.0f) * 255.0f + 0.5f));
        }
    }
}

} // namespace Ubiquitty::Rendering
