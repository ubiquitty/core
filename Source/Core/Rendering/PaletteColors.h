// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
// Source/Core/Rendering/PaletteColors.h — a palette's colours in the 8-bit
// channels every theme is authored in: what a frontend's chrome and an
// embedding host match the terminal with.
//
// One narrowing for every reader. A frontend narrowing the 16-bit palette
// itself is a second reading of the theme, and the two drift the day one of
// them rounds differently. Here rather than in Core/Themes for the reason
// ChromeStyle.h gives: the chrome colours the model layer reads off a terminal
// narrow the same way, and the model layer cannot include a behaviour module.

#include "Core/Color.h"
#include "Core/PaletteRGB.h"
#include "Core/TermPalette.h"

#include <array>
#include <cstddef>
#include <optional>

namespace Ubiquitty::Rendering {

/// How many ANSI colours a palette's base holds: the eight standard colours and
/// their eight bright twins.
inline constexpr std::size_t ansiColorCount = 16;

/// The colours a palette shows text, its ground and its cursor in.
struct PaletteColors {
    /// The default text colour, which OSC 10 sets.
    ColorRGBA foreground {};
    /// The default background, which OSC 11 sets.
    ColorRGBA background {};
    /// The cursor colour, which OSC 12 sets.
    ColorRGBA cursor {};
    /// ANSI colours 0 through 15, which OSC 4 sets, each at the foreground
    /// opacity the palette draws it with.
    std::array<ColorRGBA, ansiColorCount> ansi {};
    /// The selection background a program set with OSC 17, opaque; none where
    /// it set none, and for a theme, which names none. Without one the frame
    /// fills a selection in the `[selection]` style.
    std::optional<ColorRGBA> selectionBackground;
    /// The selected text colour a program set with OSC 19, opaque; none where
    /// it set none, and the selected text keeps its own colour.
    std::optional<ColorRGBA> selectionForeground;
};

/// Narrows one palette colour to 8-bit channels, opaque.
///
/// 257 is the palette's widening factor (``channel8To16``), so the narrowing is
/// exact for every colour that arrived as 8-bit, which is every colour a theme
/// can express.
///
/// - Parameter color: The palette's 16-bit colour.
/// - Returns: The colour, alpha opaque.
[[nodiscard]] constexpr ColorRGBA narrowPaletteColor(const PaletteRGB& color)
{
    return { static_cast<uint8_t>(color.red / 257), static_cast<uint8_t>(color.green / 257), static_cast<uint8_t>(color.blue / 257), 0xFF };
}

/// The colours of a palette, as chrome reads them.
///
/// - Parameter palette: The palette, with any OSC 4, 10, 11 or 12 overrides.
/// - Returns: Its default text, background and cursor colours, opaque, and its
///   sixteen ANSI colours at their own opacity.
[[nodiscard]] constexpr PaletteColors paletteColors(const TermPalette& palette)
{
    PaletteColors colors;
    colors.foreground = narrowPaletteColor(palette.defaultForeground);
    colors.background = narrowPaletteColor(palette.defaultBackground);
    colors.cursor = narrowPaletteColor(palette.cursor);
    for (std::size_t index = 0; index < ansiColorCount; ++index) {
        colors.ansi[index] = narrowPaletteColor(palette.indexed[index]);
        colors.ansi[index].alpha = palette.indexedAlpha[index];
    }
    return colors;
}

} // namespace Ubiquitty::Rendering
