// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Portable accessibility transforms over a resolved CellPalette: a
// color-vision-deficiency (CVD) safe hue remap and a high-contrast separation
// boost. Both run on the palette's 16 ANSI base + default fg/bg/cursor before
// fillExtended() seeds the 256-color cube, so the cube regenerates from the
// adapted base. Pure functions of a CellPalette + mode.
//
// CVD remap SUBSTITUTES the confusable ANSI hues (red↔green under deuteranopia/
// protanopia, blue↔yellow under tritanopia) with replacements from a
// CVD-distinguishable set (Okabe-Ito), re-toned toward each original slot's
// brightness to keep the theme's luminance feel.

#include "Core/Accessibility/ColorblindMode.h"
#include "Core/CellColor.h"
#include "Core/TermPalette.h"

namespace Ubiquitty::Accessibility {

/// Remaps the palette's ANSI base hues for a colour-vision deficiency.
///
/// Touches slots 0 through 15 plus the default foreground and background;
/// `ColorblindMode::None` is a no-op. Call `CellPalette::fillExtended()`
/// afterward so the 256-colour cube regenerates from the adapted base.
///
/// Call once on a freshly theme-applied palette. The remap substitutes hues
/// rather than shifting them, so running it on its own output substitutes the
/// substitutions and walks the palette away from the theme.
///
/// - Parameters:
///   - palette: The resolved palette to adapt, in place.
///   - mode: Which deficiency to adapt for.
void applyColorblindRemap(Ubiquitty::CellPalette& palette, ColorblindMode mode);

/// Pushes each palette colour's luminance away from the background's.
///
/// Widens the foreground-to-background gap as well, and leaves alpha untouched.
/// Applied after the CVD remap and before `fillExtended()`, for the same reason:
/// the cube is seeded from the base, so a base adapted afterwards leaves the
/// cube describing the palette that was replaced.
///
/// - Parameters:
///   - palette: The resolved palette to adapt, in place.
///   - strength: 0 for no separation, 1 for maximal.
void applyContrastBoost(Ubiquitty::CellPalette& palette, double strength);

/// The adaptation every theme application runs through.
///
/// Two facts from two owners: the colour-vision mode is the reader's
/// `[appearance] colorblind_mode`, and the contrast strength is the system's
/// own accessibility switch (Increase Contrast on macOS), which only a GUI
/// frontend can read. Held once for the process, where the theme application
/// finds it, so a window opened by any frontend, a theme switched from a menu
/// and a theme re-seeded after a reload all adapt the same way — a remap
/// applied in one frontend's own theme routine would leave every other
/// window on the unadapted palette with the setting silently doing nothing.
struct PaletteAdaptation {
    /// Which deficiency the ANSI base is remapped for; `None` leaves it.
    ColorblindMode mode = ColorblindMode::None;
    /// The separation boost, 0 for none and 1 for the system's full request.
    double contrastStrength = 0.0;
};

/// Adapts a freshly theme-applied palette: the remap, then the boost, before
/// the caller fills the extended cube.
///
/// - Parameters:
///   - palette: The resolved palette to adapt, in place.
///   - adaptation: The mode and strength to apply.
void adaptPalette(Ubiquitty::CellPalette& palette, const PaletteAdaptation& adaptation);

/// ``adaptPalette`` for the model's 16-bit palette.
///
/// The remap and the boost are defined on 8-bit colour, which is what every
/// theme is authored in; the base slots round-trip through it and back, and
/// a 16-bit value an application set through OSC 4 is replaced by the theme
/// anyway before this runs.
///
/// - Parameters:
///   - palette: The model palette to adapt, in place, before its cube is filled.
///   - adaptation: The mode and strength to apply.
void adaptPalette(Ubiquitty::TermPalette& palette, const PaletteAdaptation& adaptation);

/// The process-wide adaptation the theme application reads.
///
/// The launch seed writes the mode from the configuration; a GUI frontend
/// writes the contrast strength from the system and rewrites it when the
/// system switch moves. A headless render — the snapshot tool — writes
/// nothing, so a golden never records the machine's accessibility state.
///
/// - Returns: The adaptation, writable.
[[nodiscard]] PaletteAdaptation& paletteAdaptation();

#if UBIQUITTY_ENABLE_TEST_HOOKS
/// Test seam over the two helpers this file keeps private.
///
/// Both carry an arm no caller here can reach: `clamp8` guards against a
/// negative, and every producer of its argument is a product of non-negatives
/// or a blend whose strength is clamped to one; `retone` falls back for a target
/// darker than luma 1.0, and the only targets are four fixed Okabe-Ito hues, all
/// far above it. The guards are worth keeping for the next caller and cannot be
/// exercised through `applyColorblindRemap` or `applyContrastBoost`, so a test
/// reaches them here rather than the arms going unmeasured.
namespace ForTest {

/// Clamps `value` into a channel byte, rounding to nearest.
///
/// - Parameter value: The unclamped channel value.
/// - Returns: `value` clamped to 0...255.
[[nodiscard]] std::uint8_t clamp8(double value);

/// Replaces `slot`'s hue with `target`'s while keeping `slot`'s luminance.
///
/// - Parameters:
///   - slot: The palette entry being replaced.
///   - target: The reference hue to take.
/// - Returns: `target` rescaled to `slot`'s luma, keeping `slot`'s alpha.
[[nodiscard]] Ubiquitty::ColorRGBA retone(const Ubiquitty::ColorRGBA& slot, const Ubiquitty::ColorRGBA& target);

} // namespace ForTest
#endif

} // namespace Ubiquitty::Accessibility
