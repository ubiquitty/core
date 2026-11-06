// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

//
// Rendering/BackgroundImageLayout.h — where a backdrop image lands behind the
// grid, and how much of it the quad samples. Portable: plain floats, no GPU
// types, so a frontend maps the result into whatever its instance struct
// spells. Preferences::BackgroundImageMode aliases the mode here.
//

#include <cstdint>

namespace Ubiquitty::Rendering {

/// How a backdrop image is sized behind the grid.
///
/// Fill covers the view and crops the overflow; Fit letterboxes the whole image;
/// Stretch fills both axes and abandons the aspect ratio; Tile repeats at native
/// size; Center places it once at native size and crops if it is larger than the
/// view. The numbering is persisted and matches the settings popup's order, so
/// an enumerator's value is part of the file format rather than an internal
/// detail.
enum class BackgroundImageMode : std::uint8_t {
    Fill = 0,
    Fit = 1,
    Stretch = 2,
    Tile = 3,
    Center = 4,
};

/// Where the backdrop quad sits and which part of the image it samples.
///
/// The rectangle is in device pixels with the origin top-left. The texture
/// coordinates may fall outside 0..1 — that is how Tile asks for repetition, and
/// why ``shouldRepeat`` says which sampler addressing the quad needs rather than
/// leaving a backend to infer it from the numbers.
struct BackgroundImagePlacement {
    float leftInPixels = 0.0f;
    float topInPixels = 0.0f;
    float rightInPixels = 0.0f;
    float bottomInPixels = 0.0f;
    float u0 = 0.0f;
    float v0 = 0.0f;
    float u1 = 1.0f;
    float v1 = 1.0f;
    /// Whether the quad samples beyond the image and needs repeat addressing.
    bool shouldRepeat = false;
};

/// Places the backdrop quad for one frame.
///
/// Two ways to show an image larger than its destination: scale it down, or
/// sample a window of it. Fill and Center do the second — Fill scales to cover
/// and narrows the texture coordinates, Center keeps native size and narrows
/// them per axis — so a caller cannot treat the returned coordinates as always
/// spanning the whole image.
///
/// Center decides each axis independently, because an image wider than the view
/// but shorter than it is centred vertically and cropped horizontally in the
/// same frame.
///
/// A view or image with a non-positive extent yields the default placement, so a
/// caller that draws it unconditionally covers nothing rather than dividing by
/// zero.
///
/// - Parameters:
///   - mode: The configured sizing.
///   - viewWidthInPixels: The destination's width in device pixels.
///   - viewHeightInPixels: The destination's height in device pixels.
///   - imageWidthInPixels: The image's native width.
///   - imageHeightInPixels: The image's native height.
/// - Returns: The quad's rectangle, its texture coordinates, and whether it
///   needs repeat addressing.
[[nodiscard]] BackgroundImagePlacement backgroundImagePlacement(BackgroundImageMode mode,
    float viewWidthInPixels,
    float viewHeightInPixels,
    float imageWidthInPixels,
    float imageHeightInPixels);

} // namespace Ubiquitty::Rendering
