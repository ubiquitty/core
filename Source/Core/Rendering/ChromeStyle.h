// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
// Source/Core/Rendering/ChromeStyle.h — the colours in-grid chrome draws with.
//
// The STRUCT is here and its RESOLUTION is in Core/Themes, and the split is the
// layer boundary rather than taste: a bag of packed colours is model, while
// deriving them from a palette consults contrast policy. Rule 10 is absolute —
// an L1 renderer cannot include an L2 module — so a layout that needed the type
// would otherwise have had to invent a second one with the same fields.
//
// See Core/Themes/ChromeStyle.h for how a palette becomes one of these.

#include <cstdint>

namespace Ubiquitty::Rendering {

/// The resolved colours for one frame of in-grid chrome, as packed `0xRRGGBB`.
///
/// Packed rather than `PaletteRGB` because these are truecolor values headed for
/// a `Pen`'s r/g/b bytes — identical to what an `SGR 38;2` cell carries, so no
/// new render path and no capability is involved.
struct ChromeStyle {
    std::uint32_t surface = 0; ///< The popover's own background.
    std::uint32_t border = 0; ///< Box-drawing rules and corners.
    std::uint32_t text = 0; ///< Body text, at the body contrast floor.
    std::uint32_t dim = 0; ///< Secondary text, at the lower floor.
    std::uint32_t accent = 0; ///< Match highlights and the active affordance.
    std::uint32_t selection = 0; ///< The selected row's background.
    std::uint32_t selectionText = 0; ///< Text on that row, floored against it.
    bool isDark = false; ///< Whether the terminal background reads as dark.
};

} // namespace Ubiquitty::Rendering
