// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>

namespace Ubiquitty::Rendering {

/// Measures and adjusts contrast between packed 24-bit sRGB colors.
///
/// Contrast thresholds belong to callers; the calculations ignore the high byte.
struct PerceptualContrast {
    /// Measures signed APCA lightness contrast.
    ///
    /// - Parameters:
    ///   - foregroundRGB: The text color.
    ///   - backgroundRGB: The background color.
    /// - Returns: Positive contrast for dark text on light, negative for light on dark.
    [[nodiscard]] static double apcaLc(std::uint32_t foregroundRGB, std::uint32_t backgroundRGB);

    /// Adjusts foreground lightness to meet a contrast floor while preserving hue and saturation.
    ///
    /// - Parameters:
    ///   - foregroundRGB: The text color.
    ///   - backgroundRGB: The background color.
    ///   - targetLc: The required absolute APCA contrast.
    /// - Returns: The original color when legible, the nearest qualifying color, or the most legible pole.
    [[nodiscard]] static std::uint32_t contrastFloor(std::uint32_t foregroundRGB, std::uint32_t backgroundRGB, double targetLc);

    /// Measures the symmetric WCAG 2 contrast ratio.
    ///
    /// - Parameters:
    ///   - aRGB: The first color.
    ///   - bRGB: The second color.
    /// - Returns: A ratio from 1 through 21.
    [[nodiscard]] static double wcagContrastRatio(std::uint32_t aRGB, std::uint32_t bRGB);

    /// Finds the least ink coverage that meets both contrast floors in quantized sRGB.
    ///
    /// - Parameters:
    ///   - inkRGB: The foreground color.
    ///   - fillRGB: The background color.
    ///   - targetLc: The required absolute APCA contrast.
    ///   - targetRatio: The required WCAG 2 contrast ratio.
    /// - Returns: Coverage in 1/255 steps, or 1 when the floors cannot be reached.
    [[nodiscard]] static double minReadingCoverage(std::uint32_t inkRGB, std::uint32_t fillRGB, double targetLc, double targetRatio);
};

} // namespace Ubiquitty::Rendering
