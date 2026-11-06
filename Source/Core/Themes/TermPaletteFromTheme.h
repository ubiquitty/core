// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Apply a Theme to a Terminal's LIVE 16-bit TermPalette (distinct from
// CellPalette::fromTheme, which fills the 8-bit CellPalette). Sets ANSI 0-15 plus
// the default foreground/background/cursor; 16-255 stay the xterm cube seeded at
// construction. Dependency arrow points Themes -> Core, never the other way.

#include "Core/TermPalette.h"
#include "Core/Themes/Themes.h"
#include "Forward.h"

#include <optional>

namespace Ubiquitty::Accessibility {
struct PaletteAdaptation; // Core/Accessibility/Palette.h
}

namespace Ubiquitty::Themes {

/// Applies a theme's colours to a terminal's live palette.
///
/// Writes the 16 ANSI slots plus the default foreground, background and cursor.
/// Indices 16 through 255 are untouched — they are the xterm cube, seeded
/// separately — so a caller changing the theme must refill the cube or the
/// extended range keeps describing the palette that was replaced.
///
/// - Parameters:
///   - theme: The colours to apply.
///   - palette: The live palette to write into.
void applyToTermPalette(const Theme&, Ubiquitty::TermPalette&);

/// Resolves the palette a theme applies as.
///
/// The theme's sixteen colours and its three named ones over the compiled-in
/// palette, adapted for colour vision and contrast, with the cube filled from
/// the adapted base. One resolution for a session applying a theme, the match
/// that names the theme a palette came from, and a host reading a theme's
/// colours without applying it, so all three agree on what the theme draws.
///
/// - Parameters:
///   - themeIndex: The theme's index.
///   - adaptation: The adaptation to resolve under, normally
///     ``Accessibility/paletteAdaptation()``.
/// - Returns: The palette, or nothing when there is no theme at `themeIndex`.
[[nodiscard]] std::optional<Ubiquitty::TermPalette> resolveThemePalette(int themeIndex, const Ubiquitty::Accessibility::PaletteAdaptation& adaptation);

// Resolve a theme by index and apply it as the Terminal's palette authority
// (applyToTermPalette + cube fill + the baseline OSC 104/110-119 resets restore
// to). False when the index names no theme.
[[nodiscard]] bool applyThemeToTerminal(Ubiquitty::Terminal&, int themeIndex);

} // namespace Ubiquitty::Themes
