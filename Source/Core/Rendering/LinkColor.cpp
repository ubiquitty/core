// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Rendering/LinkColor.h"

#include "Core/Rendering/ColorResolve.h"

namespace Ubiquitty::Rendering {

namespace {

// WCAG relative luminance of an 8-bit sRGB color, in [0,1].
double relativeLuminance(ColorRGBA c)
{
    using Ubiquitty::Rendering::SRGBGamma;
    auto linear = [](uint8_t v) { return SRGBGamma::linearize(v / 255.0); };
    return 0.2126 * linear(c.red) + 0.7152 * linear(c.green) + 0.0722 * linear(c.blue);
}

// WCAG contrast ratio between two colors, in [1, 21].
double contrastRatio(ColorRGBA a, ColorRGBA b)
{
    double la = relativeLuminance(a), lb = relativeLuminance(b);
    double hi = la > lb ? la : lb, lo = la > lb ? lb : la;
    return (hi + 0.05) / (lo + 0.05);
}

} // namespace

ColorRGBA resolveLinkColor(ColorRGBA blue, ColorRGBA brightBlue, ColorRGBA background, ColorRGBA foreground)
{
    // Prefer whichever blue reads better on this theme's background.
    ColorRGBA best = blue;
    if (contrastRatio(brightBlue, background) > contrastRatio(best, background))
        best = brightBlue;
    best.alpha = 255;
    // Legibility floor (WCAG AA large text): a low-contrast blue blends toward
    // the foreground until it clears.
    constexpr double minContrast = 3.0;
    for (int step = 0; step < 8 && contrastRatio(best, background) < minContrast; ++step) {
        best.red = static_cast<uint8_t>((best.red + foreground.red) / 2);
        best.green = static_cast<uint8_t>((best.green + foreground.green) / 2);
        best.blue = static_cast<uint8_t>((best.blue + foreground.blue) / 2);
    }
    return best;
}

} // namespace Ubiquitty::Rendering
