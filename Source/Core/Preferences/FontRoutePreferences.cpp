// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Preferences/FontRoutePreferences.h"

#include "Core/Preferences/PreferencesAccessors.h"

#include <span>

namespace Ubiquitty::Preferences {

namespace {

std::uint32_t hashString(std::uint32_t hash, const std::string& value)
{
    for (unsigned char ch : value) {
        hash ^= ch;
        hash *= 16777619u;
    }
    return hash;
}

std::uint32_t hashValue(std::uint32_t hash, std::uint32_t value)
{
    hash ^= value;
    hash *= 16777619u;
    return hash;
}

Rendering::Font::ScriptClass scriptClassFromConfigToken(const std::string& script)
{
    using Rendering::Font::ScriptClass;
    if (script == "han")
        return ScriptClass::Han;
    if (script == "kana")
        return ScriptClass::Kana;
    if (script == "hangul")
        return ScriptClass::Hangul;
    if (script == "cyrillic")
        return ScriptClass::Cyrillic;
    if (script == "greek")
        return ScriptClass::Greek;
    if (script == "arabic")
        return ScriptClass::Arabic;
    if (script == "hebrew")
        return ScriptClass::Hebrew;
    if (script == "devanagari")
        return ScriptClass::Devanagari;
    if (script == "bengali")
        return ScriptClass::Bengali;
    if (script == "gurmukhi")
        return ScriptClass::Gurmukhi;
    if (script == "thai")
        return ScriptClass::Thai;
    if (script == "lao")
        return ScriptClass::Lao;
    if (script == "khmer")
        return ScriptClass::Khmer;
    if (script == "myanmar")
        return ScriptClass::Myanmar;
    if (script == "emoji")
        return ScriptClass::Emoji;
    if (script == "symbols")
        return ScriptClass::Symbols;
    if (script == "math")
        return ScriptClass::Math;
    if (script == "latin")
        return ScriptClass::Latin;
    return ScriptClass::Other;
}

} // namespace

Rendering::Font::FontFallbackPreferences FontRoutePreferences::view() const
{
    return Rendering::Font::FontFallbackPreferences(
        std::span<const Rendering::Font::RangeMapPreference>(rangeMaps.data(), rangeMaps.size()),
        std::span<const Rendering::Font::LanguageFallbackPreference>(languageFallbacks.data(), languageFallbacks.size()),
        cjkFallbackFamily,
        symbolFallbackFamily,
        emojiFallbackFamily,
        generation);
}

FontRoutePreferences buildFontRoutePreferences(const Values& config)
{
    FontRoutePreferences out;
    out.shouldMatchFallbackSize = Effective::matchFallbackSize(config);
    out.cjkFallbackFamily = config.cjkFallbackFont.value_or(std::string());
    out.symbolFallbackFamily = Effective::symbolFallbackFamily(config);
    out.emojiFallbackFamily = config.emojiFallbackFont.value_or(std::string());

    out.languageStorage.reserve(config.languageFallbacks.size());
    out.familyStorage.reserve(config.languageFallbacks.size() + config.rangeMap.size());
    out.languageFallbacks.reserve(config.languageFallbacks.size());
    for (const LanguageFallbackEntry& fallback : config.languageFallbacks) {
        out.languageStorage.push_back(fallback.language);
        out.familyStorage.push_back(fallback.family);
        const std::string& language = out.languageStorage.back();
        const std::string& family = out.familyStorage.back();
        out.languageFallbacks.push_back({ scriptClassFromConfigToken(fallback.script), { language }, family });
    }

    out.rangeMaps.reserve(config.rangeMap.size() + Rendering::Font::defaultRangeMaps.size());
    for (const RangeMapEntry& map : config.rangeMap) {
        out.familyStorage.push_back(map.family);
        const std::string& family = out.familyStorage.back();
        const auto sizing = !map.shouldMatchSize ? Rendering::FontMetrics::FallbackSizing::Inherit
            : *map.shouldMatchSize               ? Rendering::FontMetrics::FallbackSizing::MatchPrimary
                                                 : Rendering::FontMetrics::FallbackSizing::Nominal;
        out.rangeMaps.push_back({ map.firstCodePoint, map.lastCodePoint, family, sizing });
    }
    // After the reader's own, so a range they wrote for the same code points
    // answers first.
    out.rangeMaps.insert(out.rangeMaps.end(), Rendering::Font::defaultRangeMaps.begin(), Rendering::Font::defaultRangeMaps.end());

    std::uint32_t generation = 2166136261u;
    generation = hashString(generation, config.fontFamily.value_or(""));
    generation = hashString(generation, config.fallbackFont.value_or(""));
    generation = hashString(generation, out.cjkFallbackFamily);
    generation = hashString(generation, out.symbolFallbackFamily);
    generation = hashString(generation, out.emojiFallbackFamily);
    generation = hashValue(generation, out.shouldMatchFallbackSize);
    for (const LanguageFallbackEntry& fallback : config.languageFallbacks) {
        generation = hashString(generation, fallback.script);
        generation = hashString(generation, fallback.language);
        generation = hashString(generation, fallback.mode);
        generation = hashString(generation, fallback.family);
    }
    for (const RangeMapEntry& map : config.rangeMap) {
        generation = hashValue(generation, map.firstCodePoint);
        generation = hashValue(generation, map.lastCodePoint);
        generation = hashString(generation, map.mode);
        generation = hashString(generation, map.family);
        generation = hashValue(generation, map.shouldMatchSize.has_value());
        generation = hashValue(generation, map.shouldMatchSize.value_or(false));
    }
    out.generation = generation ? generation : 1;
    return out;
}

} // namespace Ubiquitty::Preferences
