// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
// Source/Core/Themes/ChromeStyle.h — the colours in-grid chrome draws with,
// derived from the terminal's live palette.
//
// Chrome drawn into cells (the command palette, a find bar, an inspector) has to
// sit on whatever theme the user is running and stay readable on all of them.
// Picking constants would look right on the themes the author tested and
// unreadable on the rest; picking palette indices directly has the same problem
// one step later, because a theme is free to put its bright-black three Lc from
// its background.
//
// So every role is derived from the palette and then passed through an APCA
// contrast floor. A theme that would produce an unreadable pairing gets a
// foreground walked toward whichever pole restores legibility, hue and
// saturation preserved.
//
// Dark or light is derived, never carried. The background's own luminance is the
// answer. A flag would be a second source of truth that disagrees the moment a
// program emits OSC 11 — and it would disagree invisibly, one cell away from the
// grid it contradicts.

#include "Core/Rendering/ChromeStyle.h"

#include <cstdint>

namespace Ubiquitty {

struct TermPalette; // Core/TermPalette.h

namespace Themes {

/// The resolved colours, declared in Core/Rendering because a bag of packed
/// colours is model rather than behaviour. Named here so a caller that only
/// wants the resolution needs one include.
using Rendering::ChromeStyle;

/// Resolves the chrome palette from a terminal palette.
///
/// Pure: the same palette always gives the same style, which is what lets a
/// golden image assert the picture rather than the frontend that drew it.
///
/// - Parameter palette: The live palette, including any OSC 4/10/11 overrides.
/// - Returns: Colours every role can be drawn with without further checking.
[[nodiscard]] ChromeStyle resolveChromeStyle(const TermPalette& palette);

} // namespace Themes
} // namespace Ubiquitty
