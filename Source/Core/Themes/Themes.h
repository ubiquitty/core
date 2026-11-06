// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/Color.h"
#include "Core/Preferences/AppearanceMode.h"
#include "Core/Themes/StatusColors.h"
#include "Core/Themes/ThemeAppearance.h"

#include <cstdint>
#include <string>
// Built-in color themes. The built-in table lives in builtin-themes.toml,
// compiled in and parsed at startup by the same reader as user [[themes]] blocks.
// Frontends may register runtime themes; count()/get() expose the union with
// built-ins first, so indices form one flat space.

namespace Ubiquitty::Themes {

struct Theme {
    const char* name; // nul-terminated, displayed in menu
    ColorRGBA ansi[16];
    ColorRGBA defaultForeground;
    ColorRGBA defaultBackground;
    ColorRGBA cursor;
    // luma(defaultBackground) < 128 (Rec. 709). Precomputed at registration;
    // constant for the life of the theme table.
    bool isDark;
    ThemeAppearance appearance {};
    /// The status colours this theme names for itself (`status_failed` and
    /// its siblings in a theme file); an absent slot is derived.
    StatusColorOverrides statusOverrides {};
    /// The frontend refreshes this runtime palette for either operating-system appearance.
    bool usesSystemAppearance = false;

    /// The colours this theme gives each status.
    ///
    /// What the theme file names (`status_failed = "#rrggbb"` and its
    /// siblings), and for a slot it does not name, the colour derived from
    /// the theme's own ANSI colours, foreground and background
    /// (``deriveStatusColors``).
    ///
    /// - Returns: One colour per ``StatusColorSlot``.
    [[nodiscard]] constexpr StatusColors statusColors() const { return withStatusColorOverrides(deriveStatusColors(ansi, defaultForeground, defaultBackground), statusOverrides); }

    /// Serializes the named palette as a portable Ubiquitty TOML theme.
    [[nodiscard]] std::string serializeTOML() const;

    // Slot indices for packedColor: 0..15 the ANSI palette, these three the named
    // colors. Kept in lock-step with the Swift-interop slot constants.
    enum : int {
        SlotForeground = 16,
        SlotBackground = 17,
        SlotCursor = 18,
    };

    /// Assigns a resolved frontend color to one palette slot.
    ///
    /// - Parameters:
    ///   - slot: An ANSI index or a named foreground, background, or cursor slot; other values are ignored.
    ///   - color: The resolved color to store.
    void setColor(int slot, ColorRGBA color)
    {
        if (slot >= 0 && slot < 16) {
            ansi[slot] = color;
        } else if (slot == SlotForeground) {
            defaultForeground = color;
        } else if (slot == SlotBackground) {
            defaultBackground = color;
        } else if (slot == SlotCursor) {
            cursor = color;
        }
    }

    // Pack one of this theme's colors into 0xRRGGBBAA. `slot` is 0..15 (ANSI) or
    // SlotForeground / SlotBackground / SlotCursor; any other slot returns 0.
    [[nodiscard]] constexpr uint32_t packedColor(int slot) const
    {
        ColorRGBA c;
        if (slot >= 0 && slot < 16)
            c = ansi[slot];
        else if (slot == SlotForeground)
            c = defaultForeground;
        else if (slot == SlotBackground)
            c = defaultBackground;
        else if (slot == SlotCursor)
            c = cursor;
        else
            return 0;
        return (static_cast<uint32_t>(c.red) << 24) | (static_cast<uint32_t>(c.green) << 16) | (static_cast<uint32_t>(c.blue) << 8) | static_cast<uint32_t>(c.alpha);
    }
};

/// Identifies changes to the custom theme catalog, including in-place replacements.
[[nodiscard]] uint64_t generation();

// Total themes (built-ins + customs).
[[nodiscard]] int count();

// Get the theme at index (0..count()-1). nullptr if out of range.
[[nodiscard]] const Theme* get(int index);

// Built-in slice only — used by tests that pin compile-time invariants
// of the themes array.
[[nodiscard]] int builtInCount();
[[nodiscard]] const Theme* builtInGet(int index);

// Index of the theme named `name`: exact match first, then the first ASCII
// case-insensitive match; -1 when none matches or `name` is null.
[[nodiscard]] int indexForName(const char* name);

// True when the indexed theme's background luma is below 128. Out-of-range
// returns false (treat "unknown theme" as light).
[[nodiscard]] bool isDark(int index);

/// The house theme for one appearance, by name.
///
/// The name, not the index: an index is a transient handle into a registry
/// that user `[[themes]]` extend, so only the name survives a config reload.
/// Callers that need the handle take ``curatedDefaultIndex``.
///
/// - Parameter scheme: Which appearance the default is wanted for.
/// - Returns: A registry name, never null.
[[nodiscard]] const char* curatedDefaultName(Ubiquitty::Preferences::ColorScheme scheme);

/// The house theme for one appearance, resolved against the registry.
///
/// - Parameter scheme: Which appearance the default is wanted for.
/// - Returns: A live index, or -1 when the curated theme is absent.
[[nodiscard]] int curatedDefaultIndex(Ubiquitty::Preferences::ColorScheme scheme);

// Runtime custom themes: built-ins always come first in the unified indexing
// space so persisted theme indices stay stable.

/// Appends a custom theme to the registry.
///
/// `name` is copied into registry storage and the stored theme's `name` pointer
/// is repointed at that copy, so a caller may pass a temporary. The registry
/// keeps element addresses stable, which is what lets `get()` hand out a
/// `Theme*` that survives a later registration.
///
/// - Parameter theme: The theme to copy in.
/// - Returns: Its unified index, always at least `builtInCount()` — built-ins
///   come first so a persisted index stays stable across a config reload.
int registerCustom(const Theme&);

/// Replaces a custom theme in place, by unified index.
///
/// - Parameters:
///   - unifiedIndex: The index `registerCustom` returned.
///   - theme: The replacement.
/// - Returns: False when the index names a built-in or nothing at all. A
///   built-in is rejected rather than shadowed, so the unified indexing space
///   cannot drift from what a persisted setting recorded.
bool replaceCustom(int unifiedIndex, const Theme&);

// Drop every registered custom theme.
void clearCustom();

// Number of custom themes registered.
[[nodiscard]] int customCount();

} // namespace Ubiquitty::Themes
