// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Keep this include group first: the STL headers below must be parsed before
// Base/Types.h installs the legacy `false`/`true` macros, which otherwise poison
// the STL template code.
#include "Core/Preferences/Preferences.h"
#include <sstream>
#include <string>
#include <vector>

#include "Core/CellColor.h"
#include "Core/Themes/BuiltinThemesData.h"
#include "Core/Themes/ThemePreferences.h"
#include "Core/Themes/ThemeTOMLWrite.h"
#include "Core/Themes/Themes.h"

// Cross-TU bridge to the custom-theme registry (ThemesCustom.cpp); a frontend
// without one supplies a no-op definition.
const Ubiquitty::Themes::Theme* customGetInternal(int);

namespace Ubiquitty::Themes {

std::string Theme::serializeTOML() const
{
    return renderThemeBlock(name, isDark, ansi, defaultForeground, defaultBackground, cursor, {}, statusOverrides);
}

namespace {

// Rec. 709 luma < 128. Mirrors the precomputed Theme.isDark.
bool prefRGBIsDark(ColorRGBA rgb)
{
    int y = (2126 * rgb.red + 7152 * rgb.green + 722 * rgb.blue) / 10000;
    return y < 128;
}

// Built-in themes parsed once from the embedded TOML. `names` owns the strings
// so each Theme's `const char* name` stays valid for the process life.
struct BuiltinTable {
    std::vector<std::string> names;
    std::vector<Theme> themes;
};

const BuiltinTable& builtins()
{
    static const BuiltinTable table = [] {
        BuiltinTable t;
        Preferences::Values cfg;
        std::istringstream in(std::string(builtinThemesTOML, builtinThemesTOMLLength));
        Preferences::parse(in, cfg);
        // Fill `names` first so the c_str() pointers never dangle through a
        // later vector reallocation.
        for (const auto& entry : cfg.themes)
            if (!entry.name.empty())
                t.names.push_back(entry.name);
        t.themes.reserve(t.names.size());
        std::size_t i = 0;
        for (const auto& entry : cfg.themes) {
            if (entry.name.empty())
                continue;
            // base = nullptr: built-ins specify every slot, so the fallback never
            // engages (and can't re-enter builtInGet during this init).
            Theme th = themeFromValues(entry, nullptr);
            th.name = t.names[i].c_str();
            t.themes.push_back(th);
            ++i;
        }
        return t;
    }();
    return table;
}

} // namespace

Theme themeFromValues(const Preferences::ThemeEntry& entry, const Theme* base)
{
    Theme t {};
    t.appearance = entry.appearance;
    t.name = nullptr; // caller / registry owns the name string
    auto pickColor = [](const Preferences::ColorEntry& src, ColorRGBA fallback) -> ColorRGBA {
        if (!src.set)
            return fallback;
        return ColorRGBA { src.red, src.green, src.blue, src.alpha };
    };
    for (int i = 0; i < 16; ++i) {
        ColorRGBA fallback = base ? base->ansi[i] : ColorRGBA { 0, 0, 0, 255 };
        t.ansi[i] = pickColor(entry.ansi[i], fallback);
    }
    t.defaultForeground = pickColor(entry.foreground, base ? base->defaultForeground : ColorRGBA { 255, 255, 255, 255 });
    t.defaultBackground = pickColor(entry.background, base ? base->defaultBackground : ColorRGBA { 0, 0, 0, 255 });
    t.cursor = pickColor(entry.cursor, base ? base->cursor : ColorRGBA { 255, 255, 255, 255 });
    t.isDark = entry.isDark.value_or(prefRGBIsDark(t.defaultBackground));
    // Named or derived, never inherited: a status colour a custom theme does
    // not name comes from its own ANSI colours, not from the base theme's.
    for (std::size_t slot = 0; slot < statusColorSlotCount; ++slot) {
        const Preferences::ColorEntry& named = entry.status[slot];
        if (named.set)
            t.statusOverrides[slot] = ColorRGBA { named.red, named.green, named.blue, named.alpha };
    }
    return t;
}

void registerThemesFromValues(const Preferences::Values& config)
{
    clearCustom();
    const Theme* base = builtInGet(0);
    for (const auto& entry : config.themes) {
        if (entry.name.empty())
            continue;
        Theme t = themeFromValues(entry, base);
        t.name = entry.name.c_str(); // registerCustom copies the string
        registerCustom(t);
    }
}

int builtInCount() { return static_cast<int>(builtins().themes.size()); }

const Theme* builtInGet(int index)
{
    const auto& list = builtins().themes;
    if (index < 0 || index >= static_cast<int>(list.size()))
        return nullptr;
    return &list[index];
}

int count() { return builtInCount() + customCount(); }

const Theme* get(int index)
{
    if (index < 0)
        return nullptr;
    const int n = builtInCount();
    if (index < n)
        return builtInGet(index);
    return ::customGetInternal(index - n);
}

bool isDark(int index)
{
    const Theme* t = get(index);
    return t && t->isDark;
}

const char* curatedDefaultName(Ubiquitty::Preferences::ColorScheme scheme)
{
    // Written once. Every arm that falls back to the house theme — the launch
    // seed, the appearance auto-switch on either frontend — reads it here, so
    // renaming the pair cannot leave one arm resolving a name that no longer
    // registers and silently landing on theme 0 instead.
    return scheme == Ubiquitty::Preferences::ColorScheme::Dark ? "Noir" : "Noir Clair";
}

int curatedDefaultIndex(Ubiquitty::Preferences::ColorScheme scheme)
{
    return indexForName(curatedDefaultName(scheme));
}

namespace {

// ASCII case-fold equality for two nul-terminated strings. Avoids strcasecmp's
// locale sensitivity and any platform string type; theme names are ASCII.
bool asciiEqualIgnoringCase(const char* a, const char* b)
{
    auto fold = [](char c) -> char { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; };
    while (*a && *b) {
        if (fold(*a) != fold(*b))
            return false;
        ++a;
        ++b;
    }
    return *a == *b;
}

} // namespace

int indexForName(const char* name)
{
    if (!name)
        return -1;
    const int n = count();
    // Exact match wins so two themes differing only in case resolve to the
    // one the caller spelled.
    for (int i = 0; i < n; ++i) {
        const Theme* t = get(i);
        if (t && t->name && std::string(t->name) == name)
            return i;
    }
    // Otherwise the first case-insensitive match.
    for (int i = 0; i < n; ++i) {
        const Theme* t = get(i);
        if (t && t->name && asciiEqualIgnoringCase(t->name, name))
            return i;
    }
    return -1;
}

CellPalette copyPalette(const Preferences::Values& values, const CellPalette& livePalette)
{
    const std::string themeName = values.copyStyleTheme.value_or(std::string {});
    if (!themeName.empty() && themeName != "default" && themeName != "plain") {
        const int index = indexForName(themeName.c_str());
        if (const Theme* theme = index >= 0 ? get(index) : nullptr) {
            CellPalette themed = CellPalette::fromTheme(*theme);
            themed.brightenBold = livePalette.brightenBold;
            return themed;
        }
    }
    return livePalette;
}

} // namespace Ubiquitty::Themes
