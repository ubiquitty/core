// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// The colorspace an inline image is decoded and presented in.
//
// Every frontend uploads image pixels to an sRGB-encoded texture and samples it
// into a layer whose colorspace the `color_space` setting picks. A decode that
// lands anywhere else means the bytes are read as something other than what
// they are: an unmanaged decode (CoreGraphics `DeviceRGB`, a raw copy) passes a
// Display P3 photo's numbers through with P3 primaries and the sampler then
// reads them as the layer's space.
//
// So the destination is one decision, and it is the same decision for kitty,
// sixel and iTerm2 pixels, for a background image, and for whatever presents
// them. The vocabulary is here, in the model; which of these a session uses is
// read from configuration by `Preferences::Effective::imagePresentationColorSpace`,
// and mapping the answer to a platform colorspace object is the frontend's part.

namespace Ubiquitty::Image {

/// The colorspace decoded image pixels are expressed in.
enum class PresentationColorSpace {
    SRGB,
    DisplayP3
};

} // namespace Ubiquitty::Image
