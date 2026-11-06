// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// How the shade glyphs ░ ▒ ▓ (and the legacy-computing shades built from the
// same primitive) reach the screen. Core owns the whole answer: the style, the
// dither order and its phase. A frontend evaluates `isShadePixelLit` — or its
// line-for-line transcription in a shader — and nothing else, so every arm
// lights the same pixels.

#include "Base/Compiler.h"

#include <cstdint>
#include <string_view>

namespace Ubiquitty::Rendering {

/// The `[appearance] shade_blocks` choice.
enum class UBIQUITTY_CLOSED_ENUM ShadeStyle : std::uint8_t {
    /// Solid foreground on a quarter, half or three quarters of the device
    /// pixels, the background untouched on the rest.
    Pattern = 0,
    /// The foreground composited at the shade's density over the whole area.
    Fill = 1,
};

/// The style when no preference names one.
inline constexpr ShadeStyle defaultShadeStyle = ShadeStyle::Pattern;

/// Resolves a configuration token, falling back to the default for an unknown one.
[[nodiscard]] constexpr ShadeStyle shadeStyleFromString(std::string_view token)
{
    if (token == "fill")
        return ShadeStyle::Fill;
    return defaultShadeStyle;
}

/// Returns the configuration token for a style.
[[nodiscard]] constexpr std::string_view shadeStyleToString(ShadeStyle style)
{
    switch (style) {
    case ShadeStyle::Pattern:
        return "pattern";
    case ShadeStyle::Fill:
        return "fill";
    }
    return "pattern";
}

/// How many of every four positions a shade density lights: 1, 2 or 3 for
/// ░ ▒ ▓, 4 for a full block.
[[nodiscard]] constexpr int shadeLitCount(float density)
{
    return static_cast<int>(density * 4.0f + 0.5f);
}

/// The additive phase that anchors the dither to the cell grid's origin.
///
/// The field is a function of the device pixel's position relative to the
/// grid's top-left corner, not of the cell it falls in: a pattern restarted at
/// every cell seams wherever the cell's pixel width is not a multiple of the
/// period, and a pattern keyed on the surface moves with the window's padding.
///
/// - Parameters:
///   - gridOriginX: The grid's left edge in device pixels, already rounded.
///   - gridOriginY: The grid's top edge in device pixels, already rounded.
/// - Returns: A value in 0...3 to add before testing a pixel.
[[nodiscard]] constexpr int shadePhase(int gridOriginX, int gridOriginY)
{
    return (-(gridOriginX + 2 * gridOriginY)) & 3;
}

/// Which pixels of the field a shade lights: the field's own, or the
/// complement that the legacy "inverse medium shade" family names.
enum class UBIQUITTY_CLOSED_ENUM ShadePolarity : std::uint8_t {
    Normal,
    Inverse,
};

/// The phase shift that turns the half-density field into its complement.
///
/// At two lit positions of every four, advancing the order by two lights
/// exactly the positions the unshifted field leaves dark, so an inverse medium
/// shade is the same field at another phase rather than a second pattern.
inline constexpr int inverseShadePhaseOffset = 2;

/// Whether the shade field lights the device pixel at (x, y).
///
/// A diagonal order rather than a Bayer matrix: a 4x4 Bayer puts its four
/// lowest thresholds in even rows, so a quarter density comes out as
/// horizontal bands at terminal cell sizes. Advancing two columns per row
/// inks every row at every density. The period is 4 across and 2 down.
///
/// - Parameters:
///   - x: Device pixel column, in surface coordinates.
///   - y: Device pixel row, in surface coordinates.
///   - litCount: `shadeLitCount` of the density.
///   - phase: `shadePhase` of the grid origin.
[[nodiscard]] constexpr bool isShadePixelLit(int x, int y, int litCount, int phase)
{
    return ((x + 2 * y + phase) & 3) < litCount;
}

} // namespace Ubiquitty::Rendering
