// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Source/Core/Rendering/ColorResolve.cpp — see ColorResolve.h. Keyed on the
// Core 16-bit TermPalette rather than CellPalette, so it builds on Linux /
// Agent / headless without Preferences / platform headers.

#include "Core/Rendering/ColorResolve.h"
#include "Core/CellPen.h"
#include "Core/Color.h"
#include "Core/Rendering/PerceptualContrast.h"
#include "Core/Screen/Addressing.h"

#include "Core/TermPalette.h"
#include "Core/TerminalOperations.h"

#include <algorithm>
#include <cmath>

namespace Ubiquitty::Rendering {

float secondaryTextCoverage(Vec4 foreground, Vec4 background, Vec4 defaultForeground, Vec4 defaultBackground)
{
    const float chroma = std::max({ foreground.x, foreground.y, foreground.z }) - std::min({ foreground.x, foreground.y, foreground.z });
    const float backgroundDifference = std::abs(background.x - defaultBackground.x)
        + std::abs(background.y - defaultBackground.y) + std::abs(background.z - defaultBackground.z);
    if (chroma > 0.04f || foreground.w <= 0 || background.w >= 1) {
        return 0;
    }
    const auto luma = [](Vec4 color) { return color.x * 0.2126f + color.y * 0.7152f + color.z * 0.0722f; };
    const float endpoint = luma(defaultForeground) > luma(defaultBackground) ? 0.0f : 1.0f;
    if (backgroundDifference > 0.015f
        && (luma(foreground) - luma(background)) * (0.5f - endpoint) <= 0) {
        // Explicit backgrounds can reverse the text polarity, as selections
        // do. Native ink follows the material's polarity, so those cells keep
        // ordinary ink while shaded input fields can still be vibrant.
        return 0;
    }
    const float span = luma(defaultForeground) - endpoint;
    if (std::abs(span) < 0.1f) {
        return 0;
    }
    const float position = (luma(foreground) - endpoint) / span;
    if (position >= 1) {
        return 0;
    }
    // Keep the darkest fade sample on the native plane. Ordinary black ink
    // would flash visibly when a fade crosses the theme's background value.
    return std::max(position, 1e-7f) * foreground.w;
}

namespace {

RGB8 fromPalette(const PaletteRGB& p) { return { channel16To8(p.red), channel16To8(p.green), channel16To8(p.blue) }; }

// One color side: colorRGB -> the pen's direct r/g/b, otherwise the palette
// slot (default / indexed) via resolvePaletteColor.
RGB8 resolveSide(const Pen& pen, const TermPalette& pal, bool wantBackground) // bool-ok: the two sides of one cell, and the function resolves exactly one of them per call
{
    const uint16_t index = wantBackground ? pen.backgroundIndex : pen.foregroundIndex;
    if (index == colorRGB)
        return wantBackground ? RGB8 { pen.backgroundRed, pen.backgroundGreen, pen.backgroundBlue } : RGB8 { pen.foregroundRed, pen.foregroundGreen, pen.foregroundBlue };
    return resolvePaletteColor(pal, index, !wantBackground);
}

} // namespace

RGB8 resolvePaletteColor(const TermPalette& pal, uint16_t index, bool foreground)
{
    if (index == colorDefault)
        return fromPalette(foreground ? pal.defaultForeground : pal.defaultBackground);
    if (index < 256)
        return fromPalette(pal.indexed[index]);
    return fromPalette(foreground ? pal.defaultForeground : pal.defaultBackground);
}

ResolvedCell resolveCell(const Pen& pen, const TermPalette& pal, bool reverseAlso, bool brightenBold) // bool-ok: both compose with the pen's own state — reverseAlso XORs with `Reverse` and brightenBold gates a remap — so an enum would name the same two cases twice
{
    RGB8 foreground = resolveSide(pen, pal, false);
    RGB8 background = resolveSide(pen, pal, true);

    // Bold remaps a literal ANSI foreground index 0..7 to its bright 8..15 twin.
    // Keys off the raw index, so colorDefault / colorRGB / 8..255 are
    // never brightened.
    const uint16_t foregroundIndex = pen.foregroundIndex;
    if (brightenBold && pen.attributes.contains(Ubiquitty::VT::Attribute::Bold) && foregroundIndex < 8)
        foreground = fromPalette(pal.indexed[foregroundIndex + 8]);

    bool isReverse = pen.attributes.contains(Ubiquitty::VT::Attribute::Reverse);
    if (reverseAlso)
        isReverse = !isReverse;
    if (isReverse) {
        const RGB8 t = foreground;
        foreground = background;
        background = t;
    }

    // Hidden collapses foreground onto background.
    if (pen.attributes.contains(Ubiquitty::VT::Attribute::Hidden))
        foreground = background;

    // Dim fades foreground halfway toward background, applied last so it survives reverse.
    if (pen.attributes.contains(Ubiquitty::VT::Attribute::Dim)) {
        foreground.red = static_cast<uint8_t>((foreground.red + background.red) / 2);
        foreground.green = static_cast<uint8_t>((foreground.green + background.green) / 2);
        foreground.blue = static_cast<uint8_t>((foreground.blue + background.blue) / 2);
    }

    return { foreground, background };
}

Palette8 narrowPalette(const TermPalette& palette)
{
    Palette8 narrowed;
    for (int index = 0; index < 256; ++index)
        narrowed.indexed[index] = fromPalette(palette.indexed[index]);
    narrowed.defaultForeground = fromPalette(palette.defaultForeground);
    narrowed.defaultBackground = fromPalette(palette.defaultBackground);
    return narrowed;
}

namespace {

RGB8 resolveSide8(const Pen& pen, const Palette8& pal, bool wantBackground) // bool-ok: mirrors the TermPalette overload, which is the claim this makes
{
    const uint16_t index = wantBackground ? pen.backgroundIndex : pen.foregroundIndex;
    if (index == colorRGB)
        return wantBackground ? RGB8 { pen.backgroundRed, pen.backgroundGreen, pen.backgroundBlue } : RGB8 { pen.foregroundRed, pen.foregroundGreen, pen.foregroundBlue };
    if (index < 256)
        return pal.indexed[index];
    return wantBackground ? pal.defaultBackground : pal.defaultForeground;
}

} // namespace

// The same order the TermPalette overload applies, with the narrowing lifted
// out. Kept as a second body rather than a shared template: the two differ only
// in how a slot is fetched, and a template over that would put the 16-bit path's
// divide back inside this one's inline expansion for no reader's benefit.
ResolvedCell resolveCell(const Pen& pen, const Palette8& pal, bool reverseAlso, bool brightenBold) // bool-ok: mirrors the TermPalette overload, which is the claim this makes
{
    RGB8 foreground = resolveSide8(pen, pal, false);
    RGB8 background = resolveSide8(pen, pal, true);

    const uint16_t foregroundIndex = pen.foregroundIndex;
    if (brightenBold && pen.attributes.contains(Ubiquitty::VT::Attribute::Bold) && foregroundIndex < 8)
        foreground = pal.indexed[foregroundIndex + 8];

    bool reverse = pen.attributes.contains(Ubiquitty::VT::Attribute::Reverse); // bool-ok: mirrors the TermPalette overload, which is the claim this makes
    if (reverseAlso)
        reverse = !reverse;
    if (reverse) {
        const RGB8 t = foreground;
        foreground = background;
        background = t;
    }

    if (pen.attributes.contains(Ubiquitty::VT::Attribute::Hidden))
        foreground = background;

    if (pen.attributes.contains(Ubiquitty::VT::Attribute::Dim)) {
        foreground.red = static_cast<uint8_t>((foreground.red + background.red) / 2);
        foreground.green = static_cast<uint8_t>((foreground.green + background.green) / 2);
        foreground.blue = static_cast<uint8_t>((foreground.blue + background.blue) / 2);
    }

    return { foreground, background };
}

ResolvedCell enhanceTextContrast(ResolvedCell resolved, const Pen& pen)
{
    const auto pack = [](RGB8 color) -> std::uint32_t {
        return (static_cast<std::uint32_t>(color.red) << 16)
            | (static_cast<std::uint32_t>(color.green) << 8) | color.blue;
    };
    const auto foreground = pack(resolved.foreground);
    const auto background = pack(resolved.background);
    if (foreground == background || pen.attributes.contains(VT::Attribute::Hidden)) {
        return resolved;
    }
    // Dim text retains a lower contrast floor than ordinary text.
    const double target = pen.attributes.contains(VT::Attribute::Dim) ? 60.0 : 75.0;
    const auto adjusted = PerceptualContrast::contrastFloor(foreground, background, target);
    resolved.foreground = { static_cast<std::uint8_t>(adjusted >> 16),
        static_cast<std::uint8_t>(adjusted >> 8), static_cast<std::uint8_t>(adjusted) };
    return resolved;
}

RGB8 resolveUnderlineColor(const Pen& pen, const TermPalette& pal, RGB8 resolvedForeground)
{
    if (pen.underlineIndex == colorDefault)
        return resolvedForeground;
    if (pen.underlineIndex == colorRGB)
        return { pen.underlineRed, pen.underlineGreen, pen.underlineBlue };
    if (pen.underlineIndex < 256)
        return fromPalette(pal.indexed[pen.underlineIndex]);
    return resolvedForeground;
}

float resolveUnderlineAlpha(const Pen& pen, const std::uint8_t* paletteAlpha, float resolvedForegroundAlpha, float blinkCurve)
{
    if (pen.underlineIndex == colorDefault)
        return resolvedForegroundAlpha;
    // A direct colour carries no alpha channel in the protocol, so the rule is
    // opaque and blinks with the text.
    if (pen.underlineIndex == colorRGB)
        return blinkCurve;
    if (pen.underlineIndex < 256)
        return paletteAlpha ? (paletteAlpha[pen.underlineIndex] / 255.0f) * blinkCurve : blinkCurve;
    return resolvedForegroundAlpha;
}

CursorBake bakeBlockCursor(Vec4 cellForeground, Vec4 cellBackground, Vec4 cursorRGB, float mixA, CursorTextInk ink)
{
    const float a = mixA;
    Vec4 background = cellBackground;
    const float groundCoverage = cellBackground.w * (1 - a);
    background.w = a + groundCoverage;
    if (background.w > 0) {
        // Compose coverage before unpremultiplying. Interpolating straight
        // RGB toward transparent ground darkens the cursor twice as it fades.
        background.x = (cursorRGB.x * a + cellBackground.x * groundCoverage) / background.w;
        background.y = (cursorRGB.y * a + cellBackground.y * groundCoverage) / background.w;
        background.z = (cursorRGB.z * a + cellBackground.z * groundCoverage) / background.w;
    }

    Vec4 foreground = cellForeground;
    if (ink.source == CursorTextInk::Source::Cell)
        return { foreground, background };
    Vec4 target = ink.color;
    if (ink.source == CursorTextInk::Source::Inverse) {
        // Grayscale inverse of the cursor color.
        const float inverse = 1.0f - Ubiquitty::perceivedLightness(cursorRGB.x, cursorRGB.y, cursorRGB.z);
        target = { inverse, inverse, inverse, 1.0f };
    }
    foreground.x += (target.x - foreground.x) * a;
    foreground.y += (target.y - foreground.y) * a;
    foreground.z += (target.z - foreground.z) * a;
    foreground.w += (1.0f - foreground.w) * a;
    return { foreground, background };
}

} // namespace Ubiquitty::Rendering
