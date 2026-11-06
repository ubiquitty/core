// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Values to Rendering::Font route-policy view, including language
// fallback/range-map storage lifetime and generation keys.

#include "Harness.h"

#include "Core/Preferences/FontRoutePreferences.h"
#include "Core/Preferences/Preferences.h"

#include <cstdio>
#include <sstream>
#include <string>

static Ubiquitty::Preferences::Values parseString(const std::string& text)
{
    Ubiquitty::Preferences::Values config;
    std::istringstream input(text);
    Ubiquitty::Preferences::parse(input, config);
    return config;
}

TEST(testPreferenceView)
{
    Ubiquitty::Preferences::Values config = parseString("[font]\n"
                                                        "fallback = \"Symbols Nerd Font Mono\"\n"
                                                        "symbol_fallback = \"Symbols Override\"\n"
                                                        "emoji_fallback = \"Emoji Font\"\n"
                                                        "cjk_fallback = \"Han Font\"\n"
                                                        "[[font.language_fallbacks]]\n"
                                                        "script = \"han\"\n"
                                                        "language = \"ja\"\n"
                                                        "mode = \"custom\"\n"
                                                        "family = \"Hiragino Sans\"\n"
                                                        "[[font.range_map]]\n"
                                                        "range = \"U+4E00-U+9FFF\"\n"
                                                        "mode = \"custom\"\n"
                                                        "family = \"Range Font\"\n");

    Ubiquitty::Preferences::FontRoutePreferences preferences = Ubiquitty::Preferences::buildFontRoutePreferences(config);
    auto view = preferences.view();
    CHECK(!view.cjkFallbackFamily.empty() && view.cjkFallbackFamily == "Han Font");
    CHECK(!view.symbolFallbackFamily.empty() && view.symbolFallbackFamily == "Symbols Override");
    CHECK(!view.emojiFallbackFamily.empty() && view.emojiFallbackFamily == "Emoji Font");
    CHECK(view.languageFallbacks.size() == 1);
    CHECK(view.languageFallbacks[0].script == Ubiquitty::Rendering::Font::ScriptClass::Han);
    CHECK(view.languageFallbacks[0].language.value == "ja");
    CHECK(std::string(view.languageFallbacks[0].family) == "Hiragino Sans");
    CHECK(view.rangeMaps.size() == 1 + Ubiquitty::Rendering::Font::defaultRangeMaps.size());
    CHECK(view.rangeMaps[0].firstCodePoint == 0x4E00);
    CHECK(view.rangeMaps[0].lastCodePoint == 0x9FFF);
    CHECK(std::string(view.rangeMaps[0].family) == "Range Font");
    CHECK(preferences.generation != 0);
}

TEST(testGenerationChanges)
{
    Ubiquitty::Preferences::Values ja = parseString("[font]\ncjk_fallback = \"ja\"\n");
    Ubiquitty::Preferences::Values zh = parseString("[font]\ncjk_fallback = \"zh-Hans\"\n");
    CHECK(Ubiquitty::Preferences::buildFontRoutePreferences(ja).generation != Ubiquitty::Preferences::buildFontRoutePreferences(zh).generation);

    Ubiquitty::Preferences::Values rangeA = parseString("[[font.range_map]]\nrange = \"U+4E00-U+9FFF\"\nmode = \"custom\"\nfamily = \"A\"\n");
    Ubiquitty::Preferences::Values rangeB = parseString("[[font.range_map]]\nrange = \"U+4E00-U+9FFF\"\nmode = \"custom\"\nfamily = \"B\"\n");
    CHECK(Ubiquitty::Preferences::buildFontRoutePreferences(rangeA).generation != Ubiquitty::Preferences::buildFontRoutePreferences(rangeB).generation);
}

TEST(rangeSizingOverridesAreDistinctFromTheInheritedDefault)
{
    using Ubiquitty::Rendering::FontMetrics::FallbackSizing;
    auto configuration = parseString("[font]\nmatch_fallback_size = false\n[[font.range_map]]\nrange = \"U+2426\"\nmode = \"custom\"\nfamily = \"Apple Symbols\"\n");
    auto inherited = Ubiquitty::Preferences::buildFontRoutePreferences(configuration);
    REQUIRE(inherited.view().rangeMaps.size() == 1 + Ubiquitty::Rendering::Font::defaultRangeMaps.size());
    CHECK(!inherited.shouldMatchFallbackSize);
    CHECK(inherited.view().rangeMaps[0].sizing == FallbackSizing::Inherit);
    configuration.rangeMap[0].shouldMatchSize = true;
    auto matched = Ubiquitty::Preferences::buildFontRoutePreferences(configuration);
    CHECK(matched.view().rangeMaps[0].sizing == FallbackSizing::MatchPrimary);
    CHECK(matched.generation != inherited.generation);
    configuration.rangeMap[0].shouldMatchSize = false;
    auto nominal = Ubiquitty::Preferences::buildFontRoutePreferences(configuration);
    CHECK(nominal.view().rangeMaps[0].sizing == FallbackSizing::Nominal);
    CHECK(nominal.generation != matched.generation);
    CHECK(nominal.generation != inherited.generation);
}

TEST(theBuiltInRangesFollowTheReadersOwnAndYieldToEmoji)
{
    // An empty file still routes the geometric symbols to the bundled face, and
    // a range the reader wrote comes first so it can take them back.
    const auto empty = Ubiquitty::Preferences::buildFontRoutePreferences(parseString(""));
    REQUIRE(empty.view().rangeMaps.size() == Ubiquitty::Rendering::Font::defaultRangeMaps.size());
    for (const auto& range : empty.view().rangeMaps) {
        CHECK(range.family == Ubiquitty::Rendering::Font::bundledGeometricSymbolFamily);
        CHECK(range.shouldYieldToEmoji);
    }
    const auto own = Ubiquitty::Preferences::buildFontRoutePreferences(
        parseString("[[font.range_map]]\nrange = \"U+25A0-U+25FF\"\nmode = \"custom\"\nfamily = \"Mine\"\n"));
    REQUIRE(own.view().rangeMaps.size() == 1 + Ubiquitty::Rendering::Font::defaultRangeMaps.size());
    CHECK(own.view().rangeMaps[0].family == "Mine");
    CHECK(!own.view().rangeMaps[0].shouldYieldToEmoji);
}

UBIQUITTY_TEST_MAIN()
