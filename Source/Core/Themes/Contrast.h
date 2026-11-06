// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Theme contrast policy uses the renderer's shared perceptual color math.
// Colors are packed 24-bit sRGB; the high byte is ignored.

#include "Core/Rendering/PerceptualContrast.h"

#include <cmath>
#include <cstdint>

namespace Ubiquitty::Themes::Contrast {

/// Measures signed APCA lightness contrast.
///
/// - Parameters:
///   - foregroundRGB: The text color.
///   - backgroundRGB: The background color.
/// - Returns: Positive contrast for dark text on light, negative for light on dark.
[[nodiscard]] inline double apcaLc(std::uint32_t foregroundRGB, std::uint32_t backgroundRGB)
{
    return Rendering::PerceptualContrast::apcaLc(foregroundRGB, backgroundRGB);
}

/// Adjusts foreground lightness to meet a contrast floor while preserving hue and saturation.
///
/// - Parameters:
///   - foregroundRGB: The text color.
///   - backgroundRGB: The background color.
///   - targetLc: The required absolute APCA contrast.
/// - Returns: The original color when legible, the nearest qualifying color, or the most legible pole.
[[nodiscard]] inline std::uint32_t contrastFloor(std::uint32_t foregroundRGB, std::uint32_t backgroundRGB, double targetLc)
{
    return Rendering::PerceptualContrast::contrastFloor(foregroundRGB, backgroundRGB, targetLc);
}

// The two poles `labelColorOnFill` chooses between. Near-black rather than pure
// black: a chip's label is chrome, and pure black on a saturated fill reads as a
// hole punched in it.
inline constexpr std::uint32_t labelDark = 0x1F1F21;
inline constexpr std::uint32_t labelLight = 0xFFFFFF;

/// Chooses the more legible near-black or white label color on a filled chip.
///
/// Perceptual contrast accounts for saturated midtones that a luminance threshold misclassifies.
///
/// - Parameter fillRGB: The chip background color.
/// - Returns: The label pole with the greater absolute APCA contrast.
[[nodiscard]] inline std::uint32_t labelColorOnFill(std::uint32_t fillRGB)
{
    const double darkLc = std::fabs(apcaLc(labelDark, fillRGB));
    const double lightLc = std::fabs(apcaLc(labelLight, fillRGB));
    return darkLc >= lightLc ? labelDark : labelLight;
}

// Role floors (conservative for UI chrome). Body/primary text; secondary/dim
// metadata. Decorative non-text separators carry no floor — do not pass them here.
inline constexpr double lcBody = 75.0;
inline constexpr double lcSecondary = 60.0;

/// Makes themed badge ink legible against its title-row wash.
///
/// A DECSET 7402 letterbox can replace the theme background without changing its foreground.
///
/// - Parameters:
///   - themeForegroundRGB: The theme's preferred ink.
///   - washRGB: The title-row background.
/// - Returns: Ink adjusted to the secondary contrast floor.
[[nodiscard]] inline std::uint32_t chromeBadgeInk(std::uint32_t themeForegroundRGB, std::uint32_t washRGB)
{
    return contrastFloor(themeForegroundRGB, washRGB, lcSecondary);
}

/// The share of a loud tag's colour in the title row and tab strip it washes:
/// enough that the tag registers before anything on the row is read, and
/// little enough that the theme's own ground holds, so a dark theme's chrome
/// stays dark under a loud tag and a light theme's light.
inline constexpr double loudTagWashShare = 0.3;

/// A loud tag's wash over a theme's chrome: the tag's colour mixed into the
/// row's own ground at ``loudTagWashShare``, opaque.
///
/// - Parameters:
///   - tagRGB: The tag's colour, packed 0xRRGGBB.
///   - groundRGB: What the row shows with no loud tag, packed 0xRRGGBB.
/// - Returns: The wash, packed 0xRRGGBB.
[[nodiscard]] constexpr std::uint32_t loudTagWash(std::uint32_t tagRGB, std::uint32_t groundRGB)
{
    const auto channel = [&](unsigned shift) {
        const double tag = static_cast<double>((tagRGB >> shift) & 0xFFU);
        const double ground = static_cast<double>((groundRGB >> shift) & 0xFFU);
        return static_cast<std::uint32_t>(ground + (tag - ground) * loudTagWashShare + 0.5);
    };
    return (channel(16) << 16) | (channel(8) << 8) | channel(0);
}

// The coverage a chrome badge's ink is drawn at over the wash: a build label
// recedes behind the title beside it.
inline constexpr double chromeBadgeCoverage = 0.45;

/// Measures the symmetric WCAG 2 contrast ratio.
///
/// - Parameters:
///   - aRGB: The first color.
///   - bRGB: The second color.
/// - Returns: A ratio from 1 through 21.
[[nodiscard]] inline double wcagContrastRatio(std::uint32_t aRGB, std::uint32_t bRGB)
{
    return Rendering::PerceptualContrast::wcagContrastRatio(aRGB, bRGB);
}

// WCAG 2 success criterion 1.4.3 (AA) for text below the large-text size.
inline constexpr double wcagMinimumRatio = 4.5;

/// Finds the least ink coverage that meets both contrast floors in quantized sRGB.
///
/// - Parameters:
///   - inkRGB: The foreground color.
///   - fillRGB: The background color.
///   - targetLc: The required absolute APCA contrast.
///   - targetRatio: The required WCAG 2 contrast ratio.
/// - Returns: Coverage in 1/255 steps, or 1 when the floors cannot be reached.
[[nodiscard]] inline double minReadingCoverage(std::uint32_t inkRGB, std::uint32_t fillRGB, double targetLc, double targetRatio)
{
    return Rendering::PerceptualContrast::minReadingCoverage(inkRGB, fillRGB, targetLc, targetRatio);
}

} // namespace Ubiquitty::Themes::Contrast
