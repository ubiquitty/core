// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Rendering/PerceptualContrast.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace Ubiquitty::Rendering {

namespace {

// APCA-W3 0.1.9 (SAPC-8) constants, spelled out from the reference SA98G table.
constexpr double sRGBGammaExponent = 2.4; // simple-gamma power (not the WCAG-2 piecewise curve)
constexpr double redLuminanceCoefficient = 0.2126729;
constexpr double greenLuminanceCoefficient = 0.7151522;
constexpr double blueLuminanceCoefficient = 0.0721750;
constexpr double normalBackgroundExponent = 0.56; // dark-text-on-light polarity
constexpr double normalTextExponent = 0.57;
constexpr double reverseTextExponent = 0.62; // light-text-on-dark polarity
constexpr double reverseBackgroundExponent = 0.65;
constexpr double blackSoftClampThreshold = 0.022;
constexpr double blackSoftClampExponent = std::numbers::sqrt2;
constexpr double outputScale = 1.14; // Both contrast polarities use the same scale.
constexpr double lowContrastOffset = 0.027; // loBoWoffset == loWoBoffset
constexpr double minLuminanceDelta = 0.0005;
constexpr double lowContrastClip = 0.1;

struct LinearColor {
    double red, green, blue; // each 0..1
};

LinearColor unpack(std::uint32_t packedRGB)
{
    return { static_cast<double>((packedRGB >> 16) & 0xFF) / 255.0, static_cast<double>((packedRGB >> 8) & 0xFF) / 255.0,
        static_cast<double>(packedRGB & 0xFF) / 255.0 };
}

std::uint32_t pack(LinearColor color)
{
    auto quantize = [](double value) -> std::uint32_t {
        return static_cast<std::uint32_t>(std::lround(std::clamp(value, 0.0, 1.0) * 255.0));
    };
    return (quantize(color.red) << 16) | (quantize(color.green) << 8) | quantize(color.blue);
}

// sRGB → screen luminance, APCA "simple gamma" (single power curve, not WCAG-2's
// piecewise split).
double screenLuminance(LinearColor color)
{
    return redLuminanceCoefficient * std::pow(color.red, sRGBGammaExponent)
        + greenLuminanceCoefficient * std::pow(color.green, sRGBGammaExponent)
        + blueLuminanceCoefficient * std::pow(color.blue, sRGBGammaExponent);
}

// The APCA contrast core over already-computed luminances. Returns Lc, signed.
double apcaFromLuminance(double textLuminance, double backgroundLuminance)
{
    // Soft-clamp near-black luminances so very dark colors don't over-report.
    if (textLuminance < blackSoftClampThreshold) {
        textLuminance += std::pow(blackSoftClampThreshold - textLuminance, blackSoftClampExponent);
    }
    if (backgroundLuminance < blackSoftClampThreshold) {
        backgroundLuminance += std::pow(blackSoftClampThreshold - backgroundLuminance, blackSoftClampExponent);
    }

    // Luminances too close to separate — no perceptual contrast.
    if (std::fabs(backgroundLuminance - textLuminance) < minLuminanceDelta) {
        return 0.0;
    }

    double sapcContrast = 0.0;
    double outputContrast = 0.0;
    if (backgroundLuminance > textLuminance) {
        // Normal polarity: dark text on a light background (positive Lc).
        sapcContrast = (std::pow(backgroundLuminance, normalBackgroundExponent)
                           - std::pow(textLuminance, normalTextExponent))
            * outputScale;
        outputContrast = (sapcContrast < lowContrastClip) ? 0.0 : sapcContrast - lowContrastOffset;
    } else {
        // Reverse polarity: light text on a dark background (negative Lc).
        sapcContrast = (std::pow(backgroundLuminance, reverseBackgroundExponent)
                           - std::pow(textLuminance, reverseTextExponent))
            * outputScale;
        outputContrast = (sapcContrast > -lowContrastClip) ? 0.0 : sapcContrast + lowContrastOffset;
    }
    return outputContrast * 100.0;
}

// HSL round-trip: the floor walk moves lightness only, so hue and saturation are
// preserved by construction.
struct HueSaturationLightness {
    double hue, saturation, lightness; // hue in [0,1) turns, saturation/lightness in [0,1]
};

HueSaturationLightness toHueSaturationLightness(LinearColor color)
{
    const double maximum = std::max({ color.red, color.green, color.blue });
    const double minimum = std::min({ color.red, color.green, color.blue });
    const double delta = maximum - minimum;
    HueSaturationLightness result { 0.0, 0.0, (maximum + minimum) / 2.0 };
    if (delta <= 0.0) {
        return result; // achromatic: hue undefined, saturation 0
    }
    result.saturation = result.lightness > 0.5 ? delta / (2.0 - maximum - minimum) : delta / (maximum + minimum);
    if (maximum == color.red) {
        result.hue = (color.green - color.blue) / delta + (color.green < color.blue ? 6.0 : 0.0);
    } else if (maximum == color.green) {
        result.hue = (color.blue - color.red) / delta + 2.0;
    } else {
        result.hue = (color.red - color.green) / delta + 4.0;
    }
    result.hue /= 6.0;
    return result;
}

double hueToComponent(double lowAnchor, double highAnchor, double huePosition)
{
    if (huePosition < 0.0) {
        huePosition += 1.0;
    }
    if (huePosition > 1.0) {
        huePosition -= 1.0;
    }
    if (huePosition < 1.0 / 6.0) {
        return lowAnchor + (highAnchor - lowAnchor) * 6.0 * huePosition;
    }
    if (huePosition < 1.0 / 2.0) {
        return highAnchor;
    }
    if (huePosition < 2.0 / 3.0) {
        return lowAnchor + (highAnchor - lowAnchor) * (2.0 / 3.0 - huePosition) * 6.0;
    }
    return lowAnchor;
}

LinearColor fromHueSaturationLightness(HueSaturationLightness color)
{
    if (color.saturation <= 0.0) {
        return { color.lightness, color.lightness, color.lightness };
    }
    const double highAnchor
        = color.lightness < 0.5 ? color.lightness * (1.0 + color.saturation)
                                : color.lightness + color.saturation - color.lightness * color.saturation;
    const double lowAnchor = 2.0 * color.lightness - highAnchor;
    return { hueToComponent(lowAnchor, highAnchor, color.hue + 1.0 / 3.0), hueToComponent(lowAnchor, highAnchor, color.hue),
        hueToComponent(lowAnchor, highAnchor, color.hue - 1.0 / 3.0) };
}

// Screen luminance of `base` with its lightness overridden — the value the floor
// search probes as it walks the foreground toward a pole.
double luminanceAtLightness(HueSaturationLightness base, double lightness)
{
    return screenLuminance(fromHueSaturationLightness({ base.hue, base.saturation, lightness }));
}

// WCAG 2 relative luminance: the piecewise sRGB decode, which is not the
// simple-gamma screen luminance APCA works on above. Each is what its own
// ratio is defined on.
double wcagRelativeLuminance(LinearColor color)
{
    const auto decode = [](double channel) {
        return channel <= 0.04045 ? channel / 12.92 : std::pow((channel + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * decode(color.red) + 0.7152 * decode(color.green) + 0.0722 * decode(color.blue);
}

} // namespace

double PerceptualContrast::apcaLc(std::uint32_t foregroundRGB, std::uint32_t backgroundRGB)
{
    return apcaFromLuminance(screenLuminance(unpack(foregroundRGB)), screenLuminance(unpack(backgroundRGB)));
}

double PerceptualContrast::wcagContrastRatio(std::uint32_t aRGB, std::uint32_t bRGB)
{
    const double a = wcagRelativeLuminance(unpack(aRGB & 0xFFFFFF));
    const double b = wcagRelativeLuminance(unpack(bRGB & 0xFFFFFF));
    return (std::max(a, b) + 0.05) / (std::min(a, b) + 0.05);
}

double PerceptualContrast::minReadingCoverage(std::uint32_t inkRGB, std::uint32_t fillRGB, double targetLc, double targetRatio)
{
    inkRGB &= 0xFFFFFF;
    fillRGB &= 0xFFFFFF;
    const LinearColor ink = unpack(inkRGB);
    const LinearColor fill = unpack(fillRGB);
    for (int step = 1; step < 255; ++step) {
        const double coverage = step / 255.0;
        const std::uint32_t candidate = pack({ ink.red * coverage + fill.red * (1.0 - coverage),
            ink.green * coverage + fill.green * (1.0 - coverage),
            ink.blue * coverage + fill.blue * (1.0 - coverage) });
        if (std::fabs(apcaLc(candidate, fillRGB)) >= targetLc && wcagContrastRatio(candidate, fillRGB) >= targetRatio) {
            return coverage;
        }
    }
    return 1.0;
}

std::uint32_t PerceptualContrast::contrastFloor(std::uint32_t foregroundRGB, std::uint32_t backgroundRGB, double targetLc)
{
    foregroundRGB &= 0xFFFFFF;
    backgroundRGB &= 0xFFFFFF;

    const double backgroundLuminance = screenLuminance(unpack(backgroundRGB));
    if (std::fabs(apcaFromLuminance(screenLuminance(unpack(foregroundRGB)), backgroundLuminance)) >= targetLc) {
        return foregroundRGB; // already legible — leave the intent color untouched
    }

    // Pick the pole (black/white) that raises |Lc| more. Moving lightness toward
    // it is monotonic in |Lc|, so a binary search finds the minimum move.
    const HueSaturationLightness base = toHueSaturationLightness(unpack(foregroundRGB));
    const double contrastAtBlack = std::fabs(apcaFromLuminance(luminanceAtLightness(base, 0.0), backgroundLuminance));
    const double contrastAtWhite = std::fabs(apcaFromLuminance(luminanceAtLightness(base, 1.0), backgroundLuminance));
    const double poleLightness = contrastAtBlack >= contrastAtWhite ? 0.0 : 1.0;
    const double bestPoleContrast = std::max(contrastAtBlack, contrastAtWhite);

    if (bestPoleContrast < targetLc) { // floor unreachable on this background — best-effort pole color
        return pack(fromHueSaturationLightness({ base.hue, base.saturation, poleLightness }));
    }

    // Smallest fraction in [0,1] along the walk base.lightness → poleLightness
    // that clears the floor.
    double belowFloor = 0.0, atOrAboveFloor = 1.0;
    std::uint32_t belowFloorRGB = foregroundRGB;
    std::uint32_t atOrAboveFloorRGB = poleLightness == 0.0 ? 0 : 0xFFFFFF;
    for (int iteration = 0; iteration < 40; ++iteration) {
        const double fraction = (belowFloor + atOrAboveFloor) / 2.0;
        const double lightness = base.lightness + (poleLightness - base.lightness) * fraction;
        const auto candidate = fromHueSaturationLightness({ base.hue, base.saturation, lightness });
        const auto candidateRGB = pack(candidate);
        if (std::fabs(apcaFromLuminance(screenLuminance(candidate), backgroundLuminance)) >= targetLc) {
            atOrAboveFloor = fraction;
            atOrAboveFloorRGB = candidateRGB;
        } else {
            belowFloor = fraction;
            belowFloorRGB = candidateRGB;
        }
        // Fixed hue and saturation make each channel monotonic in lightness.
        // Equal quantized endpoints cannot contain a different 8-bit color.
        if (belowFloorRGB == atOrAboveFloorRGB) {
            return atOrAboveFloorRGB;
        }
    }
    const double lightness = base.lightness + (poleLightness - base.lightness) * atOrAboveFloor;
    return pack(fromHueSaturationLightness({ base.hue, base.saturation, lightness }));
}

} // namespace Ubiquitty::Rendering
