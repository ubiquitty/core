// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

//
// ThemeDocumentEdit.h — a theme editor's save, as an edit of the configuration
// document.
//
// The theme's block is written by the one theme-TOML serializer
// (ThemeTOMLWrite.h), creating it or replacing the block by its name. A rename
// removes the old block and moves every preference and profile naming the old
// theme to the new name, so a renamed theme stays the one each of them picks.
//

#include "Core/Color.h"
#include "Core/Preferences/Key.h"
#include "Core/Preferences/Preferences.h"
#include "Core/Preferences/Profile.h"
#include "Core/Themes/ThemeTOMLWrite.h"
#include "Core/Themes/Themes.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>

namespace Ubiquitty::Themes {

/// A theme's colors as packed 0xRRGGBBAA words, the shape a settings surface
/// holds them in.
struct PackedThemeColors {
    /// The foreground color.
    std::uint32_t foreground = 0;
    /// The background color.
    std::uint32_t background = 0;
    /// The cursor color.
    std::uint32_t cursor = 0;

    /// Sets one ANSI color; an index outside 0…15 is ignored.
    ///
    /// - Parameters:
    ///   - index: The ANSI slot.
    ///   - rgba: The packed color.
    void setANSI(int index, std::uint32_t rgba)
    {
        if (index >= 0 && index < static_cast<int>(m_ansi.size()))
            m_ansi[static_cast<std::size_t>(index)] = rgba;
    }

    /// One ANSI color; 0 for an index outside 0…15.
    ///
    /// - Parameter index: The ANSI slot.
    /// - Returns: The packed color.
    [[nodiscard]] std::uint32_t ansi(int index) const
    {
        return index >= 0 && index < static_cast<int>(m_ansi.size()) ? m_ansi[static_cast<std::size_t>(index)] : 0;
    }

    /// Unpacks a packed word into a color.
    [[nodiscard]] static ColorRGBA unpack(std::uint32_t rgba)
    {
        return ColorRGBA {
            static_cast<std::uint8_t>((rgba >> 24) & 0xFF),
            static_cast<std::uint8_t>((rgba >> 16) & 0xFF),
            static_cast<std::uint8_t>((rgba >> 8) & 0xFF),
            static_cast<std::uint8_t>(rgba & 0xFF),
        };
    }

    /// Packs a color into a word.
    [[nodiscard]] static std::uint32_t pack(ColorRGBA color)
    {
        return (static_cast<std::uint32_t>(color.red) << 24) | (static_cast<std::uint32_t>(color.green) << 16) | (static_cast<std::uint32_t>(color.blue) << 8) | static_cast<std::uint32_t>(color.alpha);
    }

private:
    std::array<std::uint32_t, 16> m_ansi {};
};

/// The configuration document with a theme saved into it.
///
/// The theme the save replaces lends the block its appearance and status
/// colors, which the editor does not show, so a save carries them through
/// rather than dropping them.
///
/// - Parameters:
///   - document: The live configuration document.
///   - configuration: The configuration the document resolves to.
///   - name: The theme's name.
///   - previousName: The name it is saved over; equal to `name` unless it is
///     a rename.
///   - isDark: Whether the theme is dark.
///   - colors: The theme's colors.
/// - Returns: The edited document.
[[nodiscard]] inline std::string documentSavingTheme(std::string document, const Preferences::Values& configuration, const std::string& name, const std::string& previousName, bool isDark, const PackedThemeColors& colors)
{
    std::array<ColorRGBA, 16> ansi {};
    for (std::size_t index = 0; index < ansi.size(); ++index)
        ansi[index] = PackedThemeColors::unpack(colors.ansi(static_cast<int>(index)));
    const Theme* sourceTheme = get(indexForName(previousName.c_str()));
    const ThemeAppearance appearance = sourceTheme ? sourceTheme->appearance : ThemeAppearance {};
    const StatusColorOverrides statusOverrides = sourceTheme ? sourceTheme->statusOverrides : StatusColorOverrides {};
    const std::string block = renderThemeBlock(name, isDark, ansi.data(), PackedThemeColors::unpack(colors.foreground), PackedThemeColors::unpack(colors.background), PackedThemeColors::unpack(colors.cursor), appearance, statusOverrides);
    if (previousName != name) {
        document = Preferences::removeNamedBlock(std::move(document), "themes", previousName);
        auto renamePreference = [&](Preferences::Key key, const std::optional<std::string>& value) {
            if (value != previousName)
                return;
            const Preferences::WriteDescriptor* descriptor = Preferences::descriptorFor(key);
            document = Preferences::setSectionKey(std::move(document), descriptor->section, descriptor->key, Preferences::serializeWrite(descriptor->kind, std::string_view(name)));
        };
        renamePreference(Preferences::Key::AppearanceTheme, configuration.themeName);
        renamePreference(Preferences::Key::AppearanceSystemLightTheme, configuration.systemLightThemeName);
        renamePreference(Preferences::Key::AppearanceSystemDarkTheme, configuration.systemDarkThemeName);
        for (const Preferences::Profile& profile : configuration.profiles) {
            if (profile.themeName != previousName)
                continue;
            Preferences::Profile renamedProfile = profile;
            renamedProfile.themeName = name;
            document = Preferences::spliceNamedBlock(std::move(document), "profiles", profile.name, "\n" + Preferences::serializeProfile(renamedProfile));
        }
    }
    return Preferences::spliceNamedBlock(std::move(document), "themes", name, block);
}

} // namespace Ubiquitty::Themes
