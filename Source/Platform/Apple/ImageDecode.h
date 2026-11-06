// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

//
// ImageDecode.h — the PAL image decoder for Apple platforms, through ImageIO.
//
// Core's inline-image and Kitty `f=100` paths hand an encoded container to
// `PAL::imageDecoder()`; this is the one a frontend installs there. ImageIO and
// CoreGraphics are C, so the decoder is C++ and any Apple shell can install it.
//

#include <ImageIO/ImageIO.h>

#include <cstdint>
#include <span>

namespace Ubiquitty::Apple {

/// Decodes an encoded image container — PNG, JPEG, GIF and the rest — to
/// tightly packed straight-alpha RGBA8 in the presentation colorspace.
///
/// The colorspace is the one Core presents images in (`[images]` in the
/// preferences, `Core/Image/ImagePresentation.h`), so the bytes, the sRGB
/// texture format they are uploaded as and the layer's colorspace describe one
/// space: an unmanaged decode would pass a wide-gamut source's numbers through
/// unmatched. A size Core refuses to hold, `Image::isDecodedImageBytesRefused`,
/// is refused here before any pixel is allocated: from the size the container
/// states when it states one, and from the created image's otherwise.
///
/// - Parameters:
///   - bytes: The encoded container.
///   - rgbaOut: Receives a `calloc`'d buffer the caller releases with
///     `std::free`.
///   - widthOut: Receives the width in pixels.
///   - heightOut: Receives the height in pixels.
/// - Returns: Whether the image decoded; nothing is written to the outputs
///   when it did not.
bool decodeImageRGBA(std::span<const std::uint8_t> bytes, std::uint8_t** rgbaOut, int* widthOut, int* heightOut);

namespace detail {

/// Whether the size a container states for its first image is one Core
/// refuses, `Image::isDecodedImageBytesRefused`.
///
/// `decodeImageRGBA` asks this before it creates the image. ImageIO reads the
/// size from the header without decoding a pixel, and creating the image is
/// lazy too, so this is what keeps a few kilobytes of highly compressible file
/// from reaching the allocation and the draw.
///
/// - Parameter source: The container's image source.
/// - Returns: Whether the stated size is refused. A container that does not
///   state both dimensions is not refused here; the created image's own size
///   is checked after.
[[nodiscard]] bool isStatedSizeRefused(CGImageSourceRef source);

} // namespace detail

} // namespace Ubiquitty::Apple
