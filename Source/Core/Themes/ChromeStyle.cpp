// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Source/Core/Themes/ChromeStyle.cpp — see ChromeStyle.h.

#include "Core/Themes/ChromeStyle.h"

#include "Core/TermPalette.h"
#include "Core/Themes/Contrast.h"

namespace Ubiquitty::Themes {

using Rendering::ChromeStyle;

namespace {

// The palette carries 16-bit channels (OSC wire precision); the contrast helpers
// and Pen both work in 8.
constexpr std::uint32_t packed(const PaletteRGB& colour)
{
    return (static_cast<std::uint32_t>(colour.red >> 8) << 16)
        | (static_cast<std::uint32_t>(colour.green >> 8) << 8)
        | static_cast<std::uint32_t>(colour.blue >> 8);
}

constexpr int channelRed(std::uint32_t rgb) { return static_cast<int>((rgb >> 16) & 0xFF); }
constexpr int channelGreen(std::uint32_t rgb) { return static_cast<int>((rgb >> 8) & 0xFF); }
constexpr int channelBlue(std::uint32_t rgb) { return static_cast<int>(rgb & 0xFF); }

// Rec. 709 relative luminance, integer, 0..255. Enough to answer "is this dark",
// which is the only question asked of it — the perceptual work downstream is
// APCA's, and duplicating its model here would give two answers to one question.
constexpr int luminance(std::uint32_t rgb)
{
    return (2126 * channelRed(rgb) + 7152 * channelGreen(rgb) + 722 * channelBlue(rgb)) / 10000;
}

// Which pole a shade moves toward. An enum rather than a bool because every call
// site here passes a literal, and `shade(colour, 8, true)` at the call site says
// nothing about which way "true" is — the reader has to open this function to
// find out, and half the calls pass the negation of a variable.
enum class Towards : std::uint8_t {
    White,
    Black,
};

// Moves a colour toward one pole by a fraction. Used for the surface and the
// selection — backgrounds, which carry no contrast floor of their own, because
// the floor applies to what is drawn ON them.
constexpr std::uint32_t shade(std::uint32_t rgb, int percent, Towards towards)
{
    const auto move = [&](int channel) {
        const int target = towards == Towards::White ? 255 : 0;
        const int shifted = channel + ((target - channel) * percent) / 100;
        return shifted < 0 ? 0 : (shifted > 255 ? 255 : shifted);
    };
    return (static_cast<std::uint32_t>(move(channelRed(rgb))) << 16)
        | (static_cast<std::uint32_t>(move(channelGreen(rgb))) << 8)
        | static_cast<std::uint32_t>(move(channelBlue(rgb)));
}

} // namespace

ChromeStyle resolveChromeStyle(const TermPalette& palette)
{
    const std::uint32_t background = packed(palette.defaultBackground);
    const std::uint32_t foreground = packed(palette.defaultForeground);

    ChromeStyle style;
    // Derived, not carried: the background's own luminance is the only answer
    // that cannot disagree with the grid beside it.
    style.isDark = luminance(background) < 128;

    // The popover lifts off the terminal rather than matching it, so its edge
    // reads even where no border pixel lands — 8% toward the opposite pole, which
    // is a visible step on both light and dark themes without becoming a second
    // background colour the user did not choose.
    style.surface = shade(background, 8, style.isDark ? Towards::White : Towards::Black);

    // Text roles are floored against the SURFACE rather than the terminal
    // background, because that is what they are drawn on. Flooring against the
    // background would pass a pairing that then fails by the 8% the surface moved.
    style.text = Contrast::contrastFloor(foreground, style.surface, Contrast::lcBody);
    style.dim = Contrast::contrastFloor(foreground, style.surface, Contrast::lcSecondary);

    // ANSI blue is the accent macOS already uses for this surface. Floored like
    // any other text: a theme whose blue sits near its background would otherwise
    // put the match highlight — the one thing the eye is hunting for — at the
    // lowest contrast on screen.
    style.accent = Contrast::contrastFloor(packed(palette.indexed[4]), style.surface, Contrast::lcBody);

    // The selected row is a background, so it takes a shade rather than a floor,
    // and its text is floored against it.
    //
    // Derived from the accent and pushed HARD toward the theme's own pole, which
    // is the part that is not cosmetic: a mid-tone selection cannot carry body
    // text at all. Lc 75 needs real luminance separation, so a 50%-grey row caps
    // out around Lc 56 whatever colour the text is — the floor is then
    // unreachable and contrastFloor returns its best effort, leaving the one row
    // the user is looking at the least readable on screen. Keeping the selection
    // near the background's own end of the range leaves the text somewhere to go.
    style.selection = shade(style.accent, 86, style.isDark ? Towards::Black : Towards::White);
    style.selectionText = Contrast::contrastFloor(foreground, style.selection, Contrast::lcBody);

    // The border is chrome rather than text and carries no floor by APCA's own
    // rule — a decorative separator is exempt. It is derived from the text colour
    // so it tracks the theme, dimmed so it frames without competing.
    style.border = shade(style.text, 35, style.isDark ? Towards::Black : Towards::White);
    return style;
}

} // namespace Ubiquitty::Themes
