// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

//
// Rendering/BackgroundImage.h — the window's background image, decoded and
// composited beneath a finished frame. The placement is
// BackgroundImageLayout's; this is what a software frame does with it, and the
// rule every renderer without a GPU sampler of its own follows.
//

#include "Base/Geometry.h"
#include "Core/Rendering/BackgroundImageLayout.h"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace Ubiquitty::Rendering {

/// A decoded background image: straight-alpha sRGB RGBA, eight bits a
/// channel, rows top to bottom.
struct BackgroundImagePixels {
    std::vector<std::uint8_t> rgba;
    int width = 0;
    int height = 0;

    /// Whether there is no image to draw.
    ///
    /// - Returns: True when either extent is zero or the pixels are missing.
    [[nodiscard]] bool isEmpty() const
    {
        return width <= 0 || height <= 0 || rgba.size() != static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4;
    }
};

/// Decodes an image file's bytes for use as a window background.
///
/// The platform's decoder when the frontend installed one (`PAL::imageDecoder`),
/// Core's own PNG and TIFF decoders otherwise, under the same size bound an
/// inline image is held to.
///
/// - Parameter fileBytes: The file's contents.
/// - Returns: The pixels, or an empty image when the bytes do not decode.
[[nodiscard]] BackgroundImagePixels decodeBackgroundImage(std::span<const std::uint8_t> fileBytes);

/// How a placed backdrop maps a pixel to the image: the image coordinate at
/// the rectangle's left and top edges and its step per pixel.
///
/// One division per axis, made once, so a renderer that samples per pixel
/// multiplies and adds and never divides — a GPU divides by approximating the
/// reciprocal, and the composite's renderers are compared byte for byte.
struct BackgroundImageSampling {
    float u0 = 0.0f;
    float v0 = 0.0f;
    float uPerPixel = 0.0f;
    float vPerPixel = 0.0f;
};

/// Returns the sampling a placement implies.
///
/// - Parameter placement: Where ``backgroundImagePlacement`` put the image.
/// - Returns: The edge coordinates and the per-pixel steps; zero steps for an
///   empty rectangle.
[[nodiscard]] BackgroundImageSampling backgroundImageSampling(const BackgroundImagePlacement& placement);

/// What one frame composites beneath itself: the image and the window's
/// `[appearance] background_image_mode` and `background_image_opacity`.
struct BackgroundImageLayer {
    /// The decoded image, borrowed for the frame; null draws no backdrop.
    const BackgroundImagePixels* pixels = nullptr;
    BackgroundImageMode mode = BackgroundImageMode::Fill;
    float opacity = 1.0f;

    /// Whether the layer draws anything.
    ///
    /// - Returns: True when an image is set and its opacity is above zero.
    [[nodiscard]] bool isVisible() const { return pixels && !pixels->isEmpty() && opacity > 0.0f; }
};

/// The sRGB transfer tables the composite works through, shared so every
/// renderer that runs it reads the same numbers.
///
/// Light is mixed linearly, the way a GPU blends into an sRGB target. Decoding
/// is a lookup and encoding a search of the code boundaries, so no `pow`
/// runs per pixel and a shader holding the same two tables reaches the same
/// byte.
struct SRGBTransferTables {
    /// The linear value of each eight-bit code.
    std::array<float, 256> decoded {};
    /// The linear value at which code `k + 1` begins: the decode of `(k + 0.5) / 255`.
    std::array<float, 255> codeBoundaries {};
    /// Each eight-bit code over 255, for alpha: a lookup rather than a
    /// division, because a GPU divides by approximating the reciprocal.
    std::array<float, 256> fractions {};
};

/// Returns the sRGB transfer tables.
///
/// - Returns: The tables, built once.
[[nodiscard]] const SRGBTransferTables& srgbTransferTables();

/// Composites the background image beneath a finished frame.
///
/// Destination-over in linear light: each pixel keeps what the frame drew
/// and the image fills the alpha the frame left open, which is the theme
/// background's translucency. The image is placed by
/// ``backgroundImagePlacement`` over the whole frame, padding included, and
/// sampled bilinearly at pixel centres — repeating for Tile, transparent past
/// the edge otherwise — then scaled by the opacity. A pixel outside the placed
/// rectangle is left as it is.
///
/// The frame's colour channels are taken as the colour of what the frame drew
/// and its alpha as how much of the window that covers, so a pixel becomes
/// `colour + image × opacity × (1 − alpha)` in linear light, with alpha
/// growing by the same share. That is the Metal arm's destination-over blend
/// into its sRGB target, so a software frame and a GPU frame of one window
/// show the same backdrop.
///
/// - Parameters:
///   - layer: The image and the window's settings; nothing happens when it is
///     not visible.
///   - rgba: The frame's straight-alpha sRGB pixels, row stride 4 × width,
///     changed in place.
///   - frame: The frame's size in pixels.
void compositeBackgroundImage(const BackgroundImageLayer& layer, std::span<std::uint8_t> rgba, IntSize frame);

} // namespace Ubiquitty::Rendering
