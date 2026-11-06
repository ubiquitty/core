// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/Preferences/PreferencesDefaults.h"

#include <optional>
// The System / Light / Dark tri-state the appearance picker shows, derived from
// Values via a darkness-lookup callable (no Values, Theme-registry, or platform type).

#include <cstdint>

namespace Ubiquitty::Preferences {

// Which OS appearance / theme darkness we are resolving for. Values map onto the
// is-dark bool: Light == 0, Dark == 1, so a static_cast either way is value-preserving.
enum class ColorScheme : uint8_t {
    Light = 0, // the OS light appearance / a light theme
    Dark = 1, // the OS dark appearance / a dark theme
};

// The appearance picker's tri-state. The integer values match the macOS picker's
// tags (0 System / 1 Light / 2 Dark) so the frontend can use them directly.
enum class AppearanceMode {
    System = 0, // follow the OS light/dark setting (auto-switch themes)
    Light = 1, // pinned to a light theme
    Dark = 2, // pinned to a dark theme
};

// Resolve [appearance] follow_system to its effective bool: default-on when the
// key was never set (followSystemSet == false), else the set value.
[[nodiscard]] inline bool followSystemAppearanceEffective(const std::optional<bool>& followSystem) { return followSystem.value_or(Defaults::followSystemAppearance); }

// Derive which picker entry is selected. isDark(i) must be valid for 0 <= i < themeCount.
// Follow-system wins → System; else a valid in-range pinned index selects Dark/Light by
// its darkness; else System (fallback for unset / out-of-range index).
template<typename DarknessFn> [[nodiscard]] AppearanceMode appearanceModeFromValues(bool followSystem, bool themeIndexSet, int themeIndex, int themeCount, DarknessFn isDark)
{
    if (followSystem)
        return AppearanceMode::System;
    if (themeIndexSet && themeIndex >= 0 && themeIndex < themeCount)
        return isDark(themeIndex) ? AppearanceMode::Dark : AppearanceMode::Light;
    return AppearanceMode::System;
}

} // namespace Ubiquitty::Preferences
