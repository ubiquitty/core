// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Accessibility/Palette.h"
#include "Core/Color.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace Ubiquitty::Accessibility {

PaletteAdaptation& paletteAdaptation()
{
    static PaletteAdaptation adaptation;
    return adaptation;
}

void adaptPalette(Ubiquitty::CellPalette& palette, const PaletteAdaptation& adaptation)
{
    applyColorblindRemap(palette, adaptation.mode);
    applyContrastBoost(palette, adaptation.contrastStrength);
}

void adaptPalette(Ubiquitty::TermPalette& palette, const PaletteAdaptation& adaptation)
{
    if (adaptation.mode == ColorblindMode::None && adaptation.contrastStrength <= 0.0)
        return;
    const auto down = [](const PaletteRGB& c) { return ColorRGBA { channel16To8(c.red), channel16To8(c.green), channel16To8(c.blue), 255 }; };
    const auto up = [](const ColorRGBA& c) { return PaletteRGB { channel8To16(c.red), channel8To16(c.green), channel8To16(c.blue) }; };
    CellPalette eight;
    for (int i = 0; i < 16; ++i)
        eight.indexed[i] = down(palette.indexed[i]);
    eight.defaultForeground = down(palette.defaultForeground);
    eight.defaultBackground = down(palette.defaultBackground);
    eight.cursor = down(palette.cursor);
    adaptPalette(eight, adaptation);
    for (int i = 0; i < 16; ++i)
        palette.indexed[i] = up(eight.indexed[i]);
    palette.defaultForeground = up(eight.defaultForeground);
    palette.defaultBackground = up(eight.defaultBackground);
    palette.cursor = up(eight.cursor);
}

namespace {

double luma(const ColorRGBA& c) { return Ubiquitty::perceivedLightness(c); }

std::uint8_t clamp8(double v)
{
    if (v < 0.0)
        return 0;
    if (v > 255.0)
        return 255;
    return static_cast<std::uint8_t>(std::lround(v));
}

// Replace `slot`'s hue with `target` while preserving the slot's luminance: scale
// the target's RGB so its luma matches the slot's. Keeps a dim theme's red dim
// and a bright theme's green bright after substitution.
ColorRGBA retone(const ColorRGBA& slot, const ColorRGBA& target)
{
    const double slotLuma = luma(slot);
    const double targetLuma = luma(target);
    if (targetLuma < 1.0) {
        // Target is near-black; fall back to the target verbatim rather than
        // dividing by ~0 (only happens for a degenerate replacement entry).
        return { target.red, target.green, target.blue, slot.alpha };
    }
    const double scale = slotLuma / targetLuma;
    return { clamp8(target.red * scale), clamp8(target.green * scale), clamp8(target.blue * scale), slot.alpha };
}

// CVD-distinguishable reference hues (Okabe-Ito). retone() re-scales these to the
// slot's luminance, so these values fix the HUE only, not the brightness.
constexpr ColorRGBA vividOrange { 230, 159, 0 }; // red-role replacement (deutan/protan)
constexpr ColorRGBA blueGreen { 0, 158, 115 }; // green-role replacement (deutan/protan)
constexpr ColorRGBA skyBlue { 86, 180, 233 }; // blue-role replacement (tritan)
constexpr ColorRGBA reddishPurple { 204, 121, 167 }; // yellow-role replacement (tritan)

// Remap the red↔green confusion axis (deuteranopia, protanopia): red → vivid
// orange, green → blue-green, so error/ok separate along the surviving
// blue-yellow axis. Cyan (6/14) is left alone (already distinct under red-green CVD).
void remapRedGreen(Ubiquitty::CellPalette& p)
{
    // ANSI: 1 red, 2 green, 3 yellow, 9 bright-red, 10 bright-green, 11 bright-yellow.
    p.indexed[1] = retone(p.indexed[1], vividOrange);
    p.indexed[9] = retone(p.indexed[9], vividOrange);
    p.indexed[2] = retone(p.indexed[2], blueGreen);
    p.indexed[10] = retone(p.indexed[10], blueGreen);
    // Yellow sits between red and green on the failing axis; nudge it toward the
    // sky-blue end so it doesn't collapse onto the orange'd red.
    p.indexed[3] = retone(p.indexed[3], skyBlue);
    p.indexed[11] = retone(p.indexed[11], skyBlue);
}

// Remap the blue↔yellow confusion axis (tritanopia): blue → sky blue (distinct
// from cyan), yellow → reddish purple, so info/warning separate along red-green.
void remapBlueYellow(Ubiquitty::CellPalette& p)
{
    // ANSI: 3 yellow, 4 blue, 11 bright-yellow, 12 bright-blue.
    p.indexed[4] = retone(p.indexed[4], skyBlue);
    p.indexed[12] = retone(p.indexed[12], skyBlue);
    p.indexed[3] = retone(p.indexed[3], reddishPurple);
    p.indexed[11] = retone(p.indexed[11], reddishPurple);
}

} // namespace

void applyColorblindRemap(Ubiquitty::CellPalette& palette, Ubiquitty::Accessibility::ColorblindMode mode)
{
    switch (mode) {
    case Ubiquitty::Accessibility::ColorblindMode::None:
        return;
    case Ubiquitty::Accessibility::ColorblindMode::Deuteranopia:
    case Ubiquitty::Accessibility::ColorblindMode::Protanopia:
        remapRedGreen(palette);
        return;
    case Ubiquitty::Accessibility::ColorblindMode::Tritanopia:
        remapBlueYellow(palette);
        return;
    }
}

void applyContrastBoost(Ubiquitty::CellPalette& palette, double strength)
{
    if (strength <= 0.0)
        return;
    strength = std::min(strength, 1.0);

    const double backgroundLuma = luma(palette.defaultBackground);
    const bool darkBackground = backgroundLuma < 128.0;
    // On a dark background push colors toward white (255); on a light one toward
    // black (0). The amount scales with strength.
    const double pole = darkBackground ? 255.0 : 0.0;

    auto boost = [&](ColorRGBA& c) {
        c.red = clamp8(c.red + (pole - c.red) * strength * 0.6);
        c.green = clamp8(c.green + (pole - c.green) * strength * 0.6);
        c.blue = clamp8(c.blue + (pole - c.blue) * strength * 0.6);
    };

    for (int i = 0; i < 16; ++i)
        boost(palette.indexed[i]);
    boost(palette.cursor);

    // The fg/bg gap is most legibility-critical: push it apart harder (0.8) than
    // the ANSI swatches (0.6).
    auto separate = [&](ColorRGBA& c, double towards) {
        c.red = clamp8(c.red + (towards - c.red) * strength * 0.8);
        c.green = clamp8(c.green + (towards - c.green) * strength * 0.8);
        c.blue = clamp8(c.blue + (towards - c.blue) * strength * 0.8);
    };
    separate(palette.defaultForeground, darkBackground ? 255.0 : 0.0);
    separate(palette.defaultBackground, darkBackground ? 0.0 : 255.0);
}

#if UBIQUITTY_ENABLE_TEST_HOOKS
namespace ForTest {
std::uint8_t clamp8(double value) { return Ubiquitty::Accessibility::clamp8(value); } // NOLINT
ColorRGBA retone(const ColorRGBA& slot, const ColorRGBA& target)
{
    return Ubiquitty::Accessibility::retone(slot, target);
}
} // namespace ForTest
#endif

} // namespace Ubiquitty::Accessibility
