// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/CellColor.h"
#include "Core/Preferences/Preferences.h"
#include "Core/Themes/Themes.h"

// Values-aware theme helpers. Kept out of Themes.h so the widely-included base
// header stays free of the Values dependency.

namespace Ubiquitty::Themes {

// Translate one ThemeEntry into a `::Theme` POD. Unset color slots fall
// back to the matching slot on `base` (pass the active built-in for user
// themes, nullptr for fully-specified built-ins). isDark is taken from the
// entry when set, else derived from the background luma.
[[nodiscard]] Theme themeFromValues(const Preferences::ThemeEntry& entry, const Theme* base);

/// Replaces the runtime theme registry with what a preference set declares.
///
/// Wholesale rather than additive: a reload has to be able to REMOVE a theme
/// the file no longer names, and a registry that only ever grew would keep
/// answering for one a person deleted.
///
/// - Parameter values: The preferences whose `[[themes]]` entries to register.
void registerThemesFromValues(const Preferences::Values& values);

/// The palette a styled copy renders its colours through.
///
/// "Style for Copy" is a rule about which colours a copy carries, not about
/// which colours are on screen: unset, empty or "default" renders in the live
/// palette the reader is looking at, a named theme renders in that theme's,
/// and a name no build knows falls back to the live one rather than to an
/// empty palette. Bold-as-bright is a preference and not a theme's colour, so
/// the live palette's setting carries into a theme's copy. Answered here
/// because every macOS window kind asks it, and a rule held in one
/// window controller would leave the others with no "Style for Copy".
///
/// - Parameters:
///   - values: The configuration whose `[selection] copy_style_theme` applies.
///   - livePalette: The palette on screen, theme plus any OSC overrides.
/// - Returns: The palette the styled flavours resolve each cell through.
[[nodiscard]] CellPalette copyPalette(const Preferences::Values& values, const CellPalette& livePalette);

} // namespace Ubiquitty::Themes
