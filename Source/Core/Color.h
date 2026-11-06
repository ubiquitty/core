// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include "Base/Types.h"

// Colour encoding shared by Pen, the SGR handlers and the renderer. A pen slot
// holds either a palette index, one of these sentinels, or an RGB triple flagged
// by colorRGB — the renderer branches on which.

// Color index sentinels

namespace Ubiquitty {

/// An RGBA quad, one byte per channel.
///
/// `red`/`green`/`blue` match the SGR true-colour wire format, which carries
/// only those three. `alpha` is UI-only and an SGR emitter must never read it —
/// there is nowhere on the wire to put it.
///
/// The default member initialiser is what keeps this an aggregate, so `{r, g, b}`
/// fills `alpha` with 255 rather than leaving it indeterminate.
struct ColorRGBA {
    uint8_t red, green, blue;
    uint8_t alpha = 255;
};
constexpr uint16_t colorDefault = 0x100;
constexpr uint16_t colorRGB = 0x101;

/// Rec. 709 luma weights, applied to channels as stored.
///
/// These weight **gamma-encoded** channels, which is what makes the result an
/// approximation of perceived lightness rather than a photometric quantity, and
/// why it is not the number a contrast ratio is defined on.
constexpr double redLumaWeight = 0.2126;
constexpr double greenLumaWeight = 0.7152;
constexpr double blueLumaWeight = 0.0722;

/// Rec. 709 luma of gamma-encoded channels, in the unit the channels are given in.
///
/// Answers "does this read as light or dark", which is what a palette adjustment
/// or a cursor inversion asks. It is **not** WCAG relative luminance: that
/// linearises each channel first, is what a contrast ratio is defined on, and
/// diverges from this by a factor of 19 at a channel value of 5. Choosing
/// between them is choosing between two quantities, not two precisions —
/// ``Themes::relativeLuminance`` is that one, and the screen luminance in
/// ``Themes::Contrast`` is a third, on APCA's own transfer curve.
///
/// - Parameters:
///   - red: The red channel, gamma-encoded.
///   - green: The green channel, gamma-encoded.
///   - blue: The blue channel, gamma-encoded.
/// - Returns: The weighted sum, in the unit and arithmetic type of the inputs.
template<typename Channel>
[[nodiscard]] constexpr Channel perceivedLightness(Channel red, Channel green, Channel blue)
{
    return static_cast<Channel>(redLumaWeight) * red + static_cast<Channel>(greenLumaWeight) * green
        + static_cast<Channel>(blueLumaWeight) * blue;
}

/// Rec. 709 luma of a stored colour, over the 0-255 range its channels use.
///
/// - Parameter color: The colour to weigh. `alpha` is not read.
/// - Returns: The weighted sum of the three channels, in 0-255.
[[nodiscard]] constexpr double perceivedLightness(ColorRGBA color)
{
    return perceivedLightness<double>(color.red, color.green, color.blue);
}

} // namespace Ubiquitty
