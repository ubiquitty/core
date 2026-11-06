// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/Preferences/Preferences.h"
#include "Core/Rendering/Font/FontFallbackPolicy.h"

#include <cstdint>
#include <string>
#include <vector>

namespace Ubiquitty::Preferences {

struct FontRoutePreferences {
    std::string cjkFallbackFamily;
    std::string symbolFallbackFamily;
    std::string emojiFallbackFamily;
    std::vector<std::string> languageStorage;
    std::vector<std::string> familyStorage;
    std::vector<Rendering::Font::LanguageFallbackPreference> languageFallbacks;
    std::vector<Rendering::Font::RangeMapPreference> rangeMaps;
    bool shouldMatchFallbackSize { true };
    std::uint32_t generation { 0 };

    [[nodiscard]] Rendering::Font::FontFallbackPreferences view() const;
};

FontRoutePreferences buildFontRoutePreferences(const Values&);

} // namespace Ubiquitty::Preferences
