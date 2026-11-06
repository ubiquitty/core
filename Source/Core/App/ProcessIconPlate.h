// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/Themes/Contrast.h"
#include "Core/Title/ProcessIcons.h"

#include <cstdint>
#include <string>

namespace Ubiquitty::App {

/// The contrast a drawn icon needs with a title's ground to go without a
/// plate. A logo is exempt from WCAG 2's 3:1 for graphics (1.4.11), and an
/// owner's own colour on a light ground often measures under it while
/// reading plainly; under 2:1 a mark fades into its ground.
inline constexpr double processIconMinimumContrast = 2.0;

/// Whether an icon needs a plate behind it to be made out on a ground: one
/// drawn in its own colours, without a tile of its own, too close to the
/// ground. A template icon wears the title's ink and never does.
///
/// - Parameters:
///   - icon: The program's entry.
///   - groundRGB: The title's ground the icon is drawn on.
/// - Returns: Whether to draw a plate, in the title's ink, behind it.
[[nodiscard]] inline bool processIconNeedsPlate(const Title::ProcessIcon& icon, std::uint32_t groundRGB)
{
    return icon.ink == Title::ProcessIconInk::Drawn && !icon.hasOwnPlate
        && Themes::Contrast::wcagContrastRatio(icon.inkRGB, groundRGB) < processIconMinimumContrast;
}

/// ``processIconNeedsPlate`` for a frontend that holds a key.
///
/// - Parameters:
///   - key: A catalog key.
///   - groundRGB: The title's ground the icon is drawn on.
/// - Returns: Whether to draw a plate behind it; false for an unknown key.
[[nodiscard]] inline bool processIconNeedsPlate(const std::string& key, std::uint32_t groundRGB)
{
    const auto* icon = Title::processIconWithKey(key);
    return icon && processIconNeedsPlate(*icon, groundRGB);
}

} // namespace Ubiquitty::App
