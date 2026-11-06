// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Platform/Apple/ImageDecode.h"

#include "Core/Image/TerminalImage.h"
#include "Core/Preferences/Preferences.h"
#include "Core/Preferences/PreferencesAccessors.h"
#include "Platform/Apple/CFAuto.h"

#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>

#include <array>
#include <cstdlib>
#include <optional>

namespace Ubiquitty::Apple {

namespace {

// CoreGraphics' name for the colorspace Core presents images in.
CFStringRef presentationColorSpaceName()
{
    return Preferences::Effective::imagePresentationColorSpace(Preferences::values()) == Image::PresentationColorSpace::DisplayP3
        ? kCGColorSpaceDisplayP3
        : kCGColorSpaceSRGB;
}

// One positive pixel dimension from an image's properties, or nothing when the
// key is absent or holds anything else.
std::optional<long long> pixelDimension(CFDictionaryRef properties, CFStringRef key)
{
    const CFTypeRef value = ::CFDictionaryGetValue(properties, key);
    if (!value || ::CFGetTypeID(value) != ::CFNumberGetTypeID())
        return std::nullopt;
    long long dimension = 0;
    if (!::CFNumberGetValue(static_cast<CFNumberRef>(value), kCFNumberLongLongType, &dimension) || dimension <= 0)
        return std::nullopt;
    return dimension;
}

} // namespace

namespace detail {

bool isStatedSizeRefused(CGImageSourceRef source)
{
    const UniqueCF<CFDictionaryRef> properties(::CGImageSourceCopyPropertiesAtIndex(source, 0, nullptr));
    if (!properties)
        return false;
    const std::optional<long long> width = pixelDimension(properties.get(), kCGImagePropertyPixelWidth);
    const std::optional<long long> height = pixelDimension(properties.get(), kCGImagePropertyPixelHeight);
    return width && height && Image::isDecodedImageBytesRefused(*width, *height);
}

} // namespace detail

bool decodeImageRGBA(std::span<const std::uint8_t> bytes, std::uint8_t** rgbaOut, int* widthOut, int* heightOut)
{
    if (bytes.empty() || !rgbaOut || !widthOut || !heightOut)
        return false;
    const UniqueCF<CFDataRef> data(::CFDataCreate(kCFAllocatorDefault, bytes.data(), static_cast<CFIndex>(bytes.size())));
    if (!data)
        return false;
    const UniqueCF<CGImageSourceRef> source(::CGImageSourceCreateWithData(data.get(), nullptr));
    if (!source)
        return false;
    if (detail::isStatedSizeRefused(source.get()))
        return false;
    // The image is drawn once and released, so ImageIO has no reason to keep a
    // decoded copy inside it.
    std::array<const void*, 1> optionKeys { kCGImageSourceShouldCache };
    std::array<const void*, 1> optionValues { kCFBooleanFalse };
    const UniqueCF<CFDictionaryRef> options(::CFDictionaryCreate(kCFAllocatorDefault, optionKeys.data(), optionValues.data(),
        static_cast<CFIndex>(optionKeys.size()), &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));
    const UniqueCF<CGImageRef> image(::CGImageSourceCreateImageAtIndex(source.get(), 0, options.get()));
    if (!image)
        return false;
    const std::size_t width = ::CGImageGetWidth(image.get());
    const std::size_t height = ::CGImageGetHeight(image.get());
    if (Image::isDecodedImageBytesRefused(static_cast<long long>(width), static_cast<long long>(height)))
        return false;
    const std::size_t stride = width * 4;
    // Cleared, because CoreGraphics composites premultiplied: the source's
    // transparent regions stay transparent rather than leaving what was there.
    std::uint8_t* pixels = static_cast<std::uint8_t*>(std::calloc(stride, height));
    if (!pixels)
        return false;
    // The presentation space, not DeviceRGB, which is unmanaged: CoreGraphics
    // passes the source's numbers through without matching its embedded
    // profile, so a Display P3 photo would arrive with P3 primaries and be read
    // as whatever the layer says. Naming the destination makes this draw the
    // color-managed conversion.
    const UniqueCF<CGColorSpaceRef> colorSpace(::CGColorSpaceCreateWithName(presentationColorSpaceName()));
    if (!colorSpace) {
        std::free(pixels);
        return false;
    }
    const UniqueCF<CGContextRef> context(::CGBitmapContextCreate(pixels, width, height, 8, stride, colorSpace.get(),
        ::CGBitmapInfoMake(kCGImageAlphaPremultipliedLast, kCGImageComponentInteger, kCGImageByteOrder32Big, kCGImagePixelFormatPacked)));
    if (!context) {
        std::free(pixels);
        return false;
    }
    ::CGContextDrawImage(context.get(), CGRectMake(0, 0, static_cast<CGFloat>(width), static_cast<CGFloat>(height)), image.get());
    // Straight RGBA is what the image path expects; opaque and fully
    // transparent pixels are unchanged.
    for (std::size_t index = 0, count = width * height; index < count; ++index) {
        std::uint8_t* pixel = pixels + index * 4;
        const unsigned alpha = pixel[3];
        if (alpha == 0 || alpha == 255)
            continue;
        for (int channel = 0; channel < 3; ++channel)
            pixel[channel] = static_cast<std::uint8_t>((pixel[channel] * 255u + alpha / 2) / alpha);
    }
    *rgbaOut = pixels;
    *widthOut = static_cast<int>(width);
    *heightOut = static_cast<int>(height);
    return true;
}

} // namespace Ubiquitty::Apple
