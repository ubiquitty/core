// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Source/Core/Themes/ThemeApply.cpp — see Core/Themes/ThemeApply.h.

#include "Core/Themes/ThemeApply.h"

#include "Core/Accessibility/Palette.h"
#include "Core/Preferences/Preferences.h"
#include "Core/TermPalette.h"
#include "Core/Themes/TermPaletteFromTheme.h"
#include "Core/Themes/ThemeResolve.h"
#include "Core/Themes/Themes.h"

namespace Ubiquitty::Themes {

int themeCount() { return count(); }

const char* themeName(int index)
{
    const Theme* theme = get(index);
    return theme ? theme->name : nullptr;
}

bool isThemeDark(int index) { return isDark(index); }

int resolvedAutomaticThemeIndex(AutomaticAppearance appearance)
{
    const Preferences::Values& configuration = Preferences::values();
    const Preferences::ColorScheme scheme = appearance == AutomaticAppearance::Dark ? Preferences::ColorScheme::Dark : Preferences::ColorScheme::Light;
    const std::optional<std::string>& overrideName = appearance == AutomaticAppearance::Dark ? configuration.systemDarkThemeName : configuration.systemLightThemeName;
    const bool isOverrideNamed = !overrideName.value_or("").empty();
    const int overrideIndex = isOverrideNamed ? indexForName(overrideName->c_str()) : -1;
    return resolveAutoTheme(scheme, count(), [scheme](int index) {
        const Theme* theme = get(index);
        return theme && theme->usesSystemAppearance ? scheme == Preferences::ColorScheme::Dark : isDark(index); }, isOverrideNamed, overrideIndex, [scheme] { return curatedDefaultIndex(scheme); });
}

bool themeColorBytes(int index, unsigned char* indexedRGB, unsigned char* defaultsRGB, unsigned char* indexedAlpha)
{
    const Theme* theme = get(index);
    if (!theme || !indexedRGB || !defaultsRGB)
        return false;
    // Seeded first, so 16-255 are the xterm cube; the theme then writes the
    // sixteen it carries and the three named ones over the top. This is the
    // same derivation every other consumer of a theme gets.
    TermPalette palette;
    palette.seedDefaults();
    applyToTermPalette(*theme, palette);
    for (int slot = 0; slot < 256; ++slot) {
        indexedRGB[static_cast<unsigned>(slot) * 3 + 0] = channel16To8(palette.indexed[slot].red);
        indexedRGB[static_cast<unsigned>(slot) * 3 + 1] = channel16To8(palette.indexed[slot].green);
        indexedRGB[static_cast<unsigned>(slot) * 3 + 2] = channel16To8(palette.indexed[slot].blue);
        if (indexedAlpha)
            indexedAlpha[slot] = palette.indexedAlpha[static_cast<size_t>(slot)];
    }
    defaultsRGB[0] = channel16To8(palette.defaultForeground.red);
    defaultsRGB[1] = channel16To8(palette.defaultForeground.green);
    defaultsRGB[2] = channel16To8(palette.defaultForeground.blue);
    defaultsRGB[3] = channel16To8(palette.defaultBackground.red);
    defaultsRGB[4] = channel16To8(palette.defaultBackground.green);
    defaultsRGB[5] = channel16To8(palette.defaultBackground.blue);
    defaultsRGB[6] = channel16To8(palette.cursor.red);
    defaultsRGB[7] = channel16To8(palette.cursor.green);
    defaultsRGB[8] = channel16To8(palette.cursor.blue);
    return true;
}

std::optional<Rendering::PaletteColors> themeColors(int index)
{
    const std::optional<TermPalette> palette = resolveThemePalette(index, Accessibility::paletteAdaptation());
    if (!palette)
        return std::nullopt;
    return Rendering::paletteColors(*palette);
}

} // namespace Ubiquitty::Themes
