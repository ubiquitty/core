// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Shared font route selection order over fake inventory and caches.
// Header-only and platform-free.

#include "Harness.h"

#include "Core/Rendering/Font/FontFallbackPolicy.h"
#include "Core/Rendering/Font/ScriptClassifier.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace Ubiquitty::Rendering::Font;

namespace {

constexpr FontHandle primaryFontHandle { 1, 1 };
constexpr FontHandle rangeFont { 2, 1 };
constexpr FontHandle hanFont { 3, 1 };
constexpr FontHandle symbolFont { 4, 1 };
constexpr FontHandle emojiFont { 5, 1 };
constexpr FontHandle arabicFont { 6, 1 };
constexpr FontHandle genericFont { 7, 1 };
constexpr FontHandle technicalFont { 8, 1 };
constexpr FontHandle japaneseHanFont { 9, 1 };
constexpr FontHandle traditionalHanFont { 10, 1 };
constexpr FontHandle bundledSymbolFont { 11, 1 };
constexpr FontHandle bundledGeometricFont { 12, 1 };

// The language icons `onefetch --nerd-fonts` prints: seti C++, C and Emacs
// Lisp, devicons Swift and Python, and the Material shell glyph.
constexpr std::array<std::uint32_t, 6> onefetchLanguageIcons { 0xE61D, 0xE61E, 0xE632, 0xE755, 0xE73C, 0xF1183 };

bool same(std::string_view left, const char* right) { return left == right; }

class PolicyFakeInventory final : public FontInventory {
public:
    FontFamilyRequest lastFamilyRequest;
    [[nodiscard]] std::uint32_t catalogGeneration() const override { return 1; }
    void bumpCatalogGeneration() override { }

    FontHandle primaryFont(FontStyle, std::uint8_t) override { return primaryFontHandle; }

    FontHandle validateFamily(const FontFamilyRequest& request) override
    {
        lastFamilyRequest = request;
        if (same(request.family, "Range Font"))
            return rangeFont;
        if (same(request.family, "Han Font"))
            return hanFont;
        if (same(request.family, "Japanese Han Font"))
            return japaneseHanFont;
        if (same(request.family, "Traditional Han Font"))
            return traditionalHanFont;
        if (same(request.family, "Symbols Font"))
            return symbolFont;
        if (same(request.family, "STIX Two Math"))
            return technicalFont;
        if (same(request.family, "Symbols Nerd Font Mono"))
            return bundledSymbolFont;
        if (same(request.family, "Monaspace Neon"))
            return bundledGeometricFont;
        if (same(request.family, "Emoji Font"))
            return emojiFont;
        if (same(request.family, "Arabic Font"))
            return arabicFont;
        return {};
    }

    GlyphProbeResult glyphForCodePoint(FontHandle font, std::uint32_t codePoint) override
    {
        if (font == primaryFontHandle && (codePoint == 'A' || codePoint == '1' || codePoint == 0x2764 || codePoint == 0x25CF))
            return { font, { 65 }, true, false };
        if (font == bundledGeometricFont && (codePoint == 0x23FA || codePoint == 0x25B6 || codePoint == 0x25CF || codePoint == 0x25EF))
            return { font, { 900 }, true, false };
        if (font == rangeFont && (codePoint == 0x4E01 || codePoint == 0x4E02 || codePoint == 0x2764 || codePoint == 0x25EF))
            return { font, { 200 }, true, false };
        if (font == hanFont && codePoint == 0x6771)
            return { font, { 300 }, true, false };
        if ((font == japaneseHanFont || font == traditionalHanFont) && codePoint == 0x6771)
            return { font, { 301 }, true, false };
        if (font == symbolFont && (codePoint == 0xE0B0 || codePoint == 0xF0001 || codePoint == 0x100001))
            return { font, { 400 }, true, false };
        if (font == technicalFont && codePoint == 0x23BE)
            return { font, { 450 }, true, false };
        if (font == bundledSymbolFont && std::ranges::find(onefetchLanguageIcons, codePoint) != onefetchLanguageIcons.end())
            return { font, { 800 }, true, false };
        if (font == emojiFont && (codePoint == 0x1F600 || codePoint == 0x1F680 || codePoint == 0x2705 || codePoint == 0x2B1C || codePoint == '1' || codePoint == 0x2764 || codePoint == 0x25B6))
            return { font, { 500 }, true, false };
        if (font == arabicFont && codePoint == 0x0633)
            return { font, { 600 }, true, false };
        if (font == genericFont && (codePoint == 0x03A9 || codePoint == 0x1F680))
            return { font, { 700 }, true, false };
        return { font, {}, false, false };
    }

    GlyphProbeResult glyphForCluster(FontHandle font, std::span<const std::uint32_t> codePoints, Presentation presentation) override
    {
        ++clusterProbeCalls;
        if (codePoints.empty() || codePoints.back() == 0x10FFFF) {
            return {};
        }
        if ((presentation == Presentation::Emoji && font != emojiFont) || (presentation == Presentation::Text && font == emojiFont)) {
            return {};
        }
        return glyphForCodePoint(font, codePoints.front());
    }

    CascadeResult cascadeForCodePoint(const CascadeRequest& request) override
    {
        ++cascadeCalls;
        cascadeLanguage = std::string(request.language.value);
        cascadePresentation = request.presentation;
        cascadeCodePoints.assign(request.codePoints.begin(), request.codePoints.end());
        if (request.codePoints.empty())
            return {};
        std::uint32_t codePoint = request.codePoints[0];
        if (codePoint == 0x6771)
            return { hanFont, { 300 }, true, false };
        if (codePoint == 0x0633)
            return { genericFont, { 610 }, true, false };
        if (codePoint == 0x03A9)
            return { genericFont, { 700 }, true, false };
        if (codePoint == 0x03B1)
            return { genericFont, { 701 }, true, false };
        // An installed face that fills private-use code points with glyphs of
        // its own, as any coverage-ranked cascade can find.
        if (codePoint == 0xE632 || codePoint == 0xF1183 || codePoint == 0xF8F0)
            return { genericFont, { 702 }, true, false };
        if (codePoint == 0x1F680) {
            return request.presentation == Presentation::Emoji ? CascadeResult { emojiFont, { 500 }, true, false } : CascadeResult { genericFont, { 700 }, true, false };
        }
        return {};
    }

    CascadeResult cascadeForRun(const CascadeRequest& request) override
    {
        ++shapeCalls;
        return cascadeForCodePoint(request);
    }

    [[nodiscard]] bool isLastResort(FontHandle) const override { return false; }
    [[nodiscard]] std::string_view familyName(FontHandle font) const override { return font == primaryFontHandle ? "Primary Font" : "fake"; }
    [[nodiscard]] std::string_view postScriptName(FontHandle) const override { return "fake"; }

    int cascadeCalls { 0 };
    int shapeCalls { 0 };
    int clusterProbeCalls { 0 };
    Presentation cascadePresentation { Presentation::Default };
    std::string cascadeLanguage;
    std::vector<std::uint32_t> cascadeCodePoints;
};

struct Harness {
    PolicyFakeInventory inventory;
    HanFallbackCache han { 8 };
    ScriptFallbackCache script { 8 };
    GenericFallbackCache generic { 8 };
    EmojiFallbackCache emoji { 8 };
    SymbolFallbackCache symbol { 8 };
    RangeMapCache rangeMap { 8 };
    NegativeRouteCache negative { 8 };
    FontFallbackPolicy policy { inventory, { han, script, generic, emoji, symbol, rangeMap, negative } };
};

FontRequest requestFor(std::uint32_t codePoint, CellWidthClass width = CellWidthClass::Narrow)
{
    static std::uint32_t storage[4];
    storage[0] = codePoint;
    FontRequest request;
    request.codePoints = std::span<const std::uint32_t>(storage, 1);
    request.script = classifyScript(codePoint);
    request.width = width;
    request.preferenceGeneration = 1;
    return request;
}

// No family configured, but a named metrics generation: genericCacheKey below
// spells the same one, and a route cached under a different generation is a
// route the lookup cannot find.
FontFallbackPreferences emptyPreferences()
{
    return FontFallbackPreferences({}, {}, {}, {}, {}, 1);
}

FontFallbackCacheKey genericCacheKey(const FontRequest& request)
{
    FontFallbackCacheKey key;
    key.primaryFont = primaryFontHandle;
    key.metricsGeneration = 1;
    key.preferenceGeneration = request.preferenceGeneration;
    key.catalogGeneration = primaryFontHandle.generation;
    key.script = request.script;
    key.style = request.style;
    key.variableWeight = request.variableWeight;
    key.routeMode = FontSource::GenericCache;
    return key;
}

} // namespace

TEST(testPrimaryAndRangeMapOrder)
{
    Harness harness;
    GlyphRoute route = harness.policy.route(requestFor('A'), emptyPreferences());
    CHECK(route.source == FontSource::Primary);
    CHECK(hasRouteFlag(route.flags, RouteFlag::MonoAtlas));

    RangeMapPreference range { 0x4E00, 0x9FFF, "Range Font" };
    FontFallbackPreferences preferences = emptyPreferences();
    preferences.rangeMaps = std::span<const RangeMapPreference>(&range, 1);
    route = harness.policy.route(requestFor(0x4E01, CellWidthClass::Wide), preferences);
    CHECK(route.source == FontSource::RangeMap);
    CHECK(route.font == rangeFont);
    CHECK(route.width == CellWidthClass::Wide);
    CHECK(hasRouteFlag(route.flags, RouteFlag::WideAtlas));
}

TEST(testHanAndSymbolEmojiScriptRoutes)
{
    Harness harness;
    FontFallbackPreferences preferences = emptyPreferences();
    preferences.cjkFallbackFamily = "Han Font";
    GlyphRoute route = harness.policy.route(requestFor(0x6771, CellWidthClass::Wide), preferences);
    CHECK(route.source == FontSource::ExplicitHan);
    CHECK(route.font == hanFont);

    preferences = emptyPreferences();
    route = harness.policy.route(requestFor(0x6771, CellWidthClass::Wide), preferences);
    CHECK(route.source == FontSource::AutoHanCascade);
    CHECK(harness.han.counters().nativeCascadeCalls == 1);

    preferences = emptyPreferences();
    preferences.symbolFallbackFamily = "Symbols Font";
    route = harness.policy.route(requestFor(0xE0B0), preferences);
    CHECK(route.source == FontSource::SymbolFallback);
    CHECK(hasRouteFlag(route.flags, RouteFlag::SymbolPath));
    for (const auto codePoint : { 0xF0001U, 0x100001U }) {
        route = harness.policy.route(requestFor(codePoint), preferences);
        CHECK(route.source == FontSource::SymbolFallback);
        CHECK(route.font == symbolFont);
        CHECK(route.glyph.value == 400);
    }

    route = harness.policy.route(requestFor(0x23BE), preferences);
    CHECK(route.source == FontSource::SymbolFallback);
    CHECK(route.font == technicalFont);
    CHECK(route.glyph.value == 450);

    preferences = emptyPreferences();
    preferences.emojiFallbackFamily = "Emoji Font";
    FontRequest emojiRequest = requestFor(0x1F600, CellWidthClass::Wide);
    emojiRequest.presentation = Presentation::Emoji;
    route = harness.policy.route(emojiRequest, preferences);
    CHECK(route.source == FontSource::EmojiCascade);
    CHECK(hasRouteFlag(route.flags, RouteFlag::ColorAtlas));

    LanguageFallbackPreference arabic { ScriptClass::Arabic, { "ar" }, "Arabic Font" };
    preferences = emptyPreferences();
    preferences.languageFallbacks = std::span<const LanguageFallbackPreference>(&arabic, 1);
    route = harness.policy.route(requestFor(0x0633), preferences);
    CHECK(route.source == FontSource::ScriptFallback);
}

TEST(testHanLanguagePrecedesTheScriptDefaultAndKeepsExactTagsSeparate)
{
    Harness harness;
    const std::array rules {
        LanguageFallbackPreference { ScriptClass::Han, { "*" }, "Han Font" },
        LanguageFallbackPreference { ScriptClass::Han, { "zh-Hant" }, "Traditional Han Font" },
        LanguageFallbackPreference { ScriptClass::Han, { "ja" }, "Japanese Han Font" },
    };
    auto preferences = emptyPreferences();
    preferences.languageFallbacks = rules;
    struct Case {
        std::string_view language;
        FontHandle expected;
    };
    for (const auto& test : {
             Case { "ja", japaneseHanFont },
             Case { "zh-Hant", traditionalHanFont },
             Case { "JA", japaneseHanFont },
             Case { "zh-hANT", traditionalHanFont },
             Case { "zh-Hans", hanFont },
             Case { "ja-JP", hanFont },
             Case { "zh-Hant-TW", hanFont },
             Case { "", hanFont },
             Case { "ja", japaneseHanFont },
         }) {
        auto request = requestFor(0x6771, CellWidthClass::Wide);
        request.language = { test.language };
        const auto route = harness.policy.route(request, preferences);
        CHECK(route.source == FontSource::ExplicitHan);
        CHECK(route.font == test.expected);
    }
    CHECK_EQ(harness.inventory.cascadeCalls, 0);
}

TEST(testHanLanguageMissFallsThroughWithoutBorrowingAnotherLanguage)
{
    Harness harness;
    std::array rules {
        LanguageFallbackPreference { ScriptClass::Han, { "ja" }, "Japanese Han Font" },
        LanguageFallbackPreference { ScriptClass::Han, { "" }, "Traditional Han Font" },
    };
    auto preferences = emptyPreferences();
    preferences.languageFallbacks = rules;
    auto request = requestFor(0x6771, CellWidthClass::Wide);
    request.language = { "zh-Hans" };
    CHECK(harness.policy.route(request, preferences).font == traditionalHanFont);

    preferences.languageFallbacks = std::span(rules).first(1);
    preferences.cjkFallbackFamily = "Traditional Han Font";
    CHECK(harness.policy.route(request, preferences).font == traditionalHanFont);
    preferences.cjkFallbackFamily = {};
    const auto route = harness.policy.route(request, preferences);
    CHECK(route.source == FontSource::AutoHanCascade);
    CHECK(route.font == hanFont);
    CHECK_STR_EQ(harness.inventory.cascadeLanguage, "zh-Hans");

    const std::array cluster { 0x6771U, 0xFE00U };
    request.codePoints = cluster;
    request.language = { "ja-JP" };
    CHECK(harness.policy.route(request, preferences).source == FontSource::AutoHanCascade);
    CHECK_EQ(harness.inventory.shapeCalls, 1);
    CHECK_STR_EQ(harness.inventory.cascadeLanguage, "ja-JP");
    CHECK(harness.inventory.cascadeCodePoints == std::vector<std::uint32_t>(cluster.begin(), cluster.end()));
}

// A plain-text dingbat (the Claude Code spinner's ✽, not Emoji_Presentation=Yes)
// whose glyph the primary font lacks must never route to the emoji cascade —
// even with an emoji fallback family configured. Before the classifier was
// pinned to the Emoji_Presentation SSOT it classified these as Emoji and
// routed them to the Apple Color Emoji bitmap font on the monochrome path,
// where the bitmap-strike scaling made the glyph size wander frame to frame.
TEST(testTextDingbatNeverRoutesToEmojiCascade)
{
    Harness harness;
    FontFallbackPreferences preferences = emptyPreferences();
    preferences.emojiFallbackFamily = "Emoji Font";
    const std::uint32_t spinner[] = { 0x2722, 0x2733, 0x2736, 0x273B, 0x273D };
    for (std::uint32_t codePoint : spinner) {
        GlyphRoute route = harness.policy.route(requestFor(codePoint), preferences);
        CHECK(route.source != FontSource::EmojiCascade);
        CHECK(!hasRouteFlag(route.flags, RouteFlag::ColorAtlas));
    }
    // And the real default-presentation emoji next to them still take the
    // cascade + color atlas (both sides of a hand-rolled range gap).
    for (std::uint32_t codePoint : { 0x2705u, 0x2B1Cu }) {
        GlyphRoute route = harness.policy.route(requestFor(codePoint, CellWidthClass::Wide), preferences);
        CHECK(route.source == FontSource::EmojiCascade);
        CHECK(hasRouteFlag(route.flags, RouteFlag::ColorAtlas));
    }
}

TEST(testClusterAndPresentationCoveragePrecedesClaimingAFont)
{
    Harness harness;
    FontFallbackPreferences preferences = emptyPreferences();
    preferences.emojiFallbackFamily = "Emoji Font";
    CHECK(harness.policy.route(requestFor('A'), preferences).source == FontSource::Primary);
    CHECK_EQ(harness.inventory.clusterProbeCalls, 0);

    const std::array heartEmoji { 0x2764U, 0xFE0FU };
    FontRequest request = requestFor(heartEmoji.front(), CellWidthClass::Wide);
    request.codePoints = heartEmoji;
    request.presentation = Presentation::Emoji;
    CHECK(harness.inventory.glyphForCodePoint(primaryFontHandle, heartEmoji.front()).present);
    GlyphRoute route = harness.policy.route(request, preferences);
    CHECK(route.source == FontSource::EmojiCascade);
    CHECK(route.font == emojiFont);
    CHECK(hasRouteFlag(route.flags, RouteFlag::ColorAtlas));
    CHECK(hasRouteFlag(route.flags, RouteFlag::ShapedRun));

    const std::array heartText { 0x2764U, 0xFE0EU };
    request.codePoints = heartText;
    request.presentation = Presentation::Text;
    route = harness.policy.route(request, preferences);
    CHECK(route.source == FontSource::Primary);
    CHECK(!hasRouteFlag(route.flags, RouteFlag::ColorAtlas));

    const std::array keycap { 0x31U, 0xFE0FU, 0x20E3U };
    request = requestFor(keycap.front(), CellWidthClass::Wide);
    request.codePoints = keycap;
    request.presentation = Presentation::Emoji;
    route = harness.policy.route(request, preferences);
    CHECK(route.source == FontSource::EmojiCascade);
    CHECK(route.font == emojiFont);

    RangeMapPreference range { heartEmoji.front(), heartEmoji.front(), "Range Font" };
    preferences.rangeMaps = std::span(&range, 1);
    request = requestFor(heartEmoji.front(), CellWidthClass::Wide);
    request.codePoints = heartEmoji;
    request.presentation = Presentation::Emoji;
    CHECK(harness.inventory.glyphForCodePoint(rangeFont, heartEmoji.front()).present);
    route = harness.policy.route(request, preferences);
    CHECK(route.source == FontSource::EmojiCascade);
    CHECK(route.font == emojiFont);
}

TEST(testClusterMissesAndCachedFontsCannotAnswerForAnotherCluster)
{
    Harness harness;
    FontFallbackPreferences preferences = emptyPreferences();
    preferences.emojiFallbackFamily = "Emoji Font";
    const std::array unsupported { 0x1F600U, 0x200DU, 0x10FFFFU };
    const std::array supported { 0x1F600U, 0xFE0FU };
    FontRequest request = requestFor(supported.front(), CellWidthClass::Wide);
    request.presentation = Presentation::Emoji;
    request.codePoints = unsupported;
    CHECK(isMissing(harness.policy.route(request, preferences)));
    request.codePoints = supported;
    CHECK(harness.policy.route(request, preferences).font == emojiFont);
    request.codePoints = unsupported;
    CHECK(isMissing(harness.policy.route(request, preferences)));
    CHECK_EQ(harness.negative.size(), 0U);

    request = requestFor(supported.front(), CellWidthClass::Wide);
    request.presentation = Presentation::Text;
    CHECK(isMissing(harness.policy.route(request, preferences)));
    request.presentation = Presentation::Emoji;
    const GlyphRoute emoji = harness.policy.route(request, preferences);
    CHECK(emoji.font == emojiFont);
    CHECK(hasRouteFlag(emoji.flags, RouteFlag::ColorAtlas));
}

TEST(testNativeCascadeReceivesPresentationOnColdAndWarmRoutes)
{
    Harness harness;
    FontRequest request = requestFor(0x1F680, CellWidthClass::Wide);
    request.presentation = Presentation::Emoji;
    GlyphRoute route = harness.policy.route(request, emptyPreferences());
    CHECK(route.font == emojiFont);
    CHECK(harness.inventory.cascadePresentation == Presentation::Emoji);
    CHECK(hasRouteFlag(route.flags, RouteFlag::ColorAtlas));

    request.presentation = Presentation::Text;
    route = harness.policy.route(request, emptyPreferences());
    CHECK(route.font == genericFont);
    CHECK(harness.inventory.cascadePresentation == Presentation::Text);
    CHECK(!hasRouteFlag(route.flags, RouteFlag::ColorAtlas));

    request.presentation = Presentation::Emoji;
    const auto calls = harness.inventory.cascadeCalls;
    route = harness.policy.route(request, emptyPreferences());
    CHECK(route.font == emojiFont);
    CHECK_EQ(harness.inventory.cascadeCalls, calls);
    CHECK(hasRouteFlag(route.flags, RouteFlag::ColorAtlas));
}

TEST(testGenericNativeAndMissingRoutes)
{
    Harness harness;
    FontRequest greek = requestFor(0x03A9);
    harness.generic.store({ genericCacheKey(greek), genericFont, FontSource::GenericCache, false });
    GlyphRoute route = harness.policy.route(greek, emptyPreferences());
    CHECK(route.source == FontSource::GenericCache);

    Harness nativeHarness;
    route = nativeHarness.policy.route(greek, emptyPreferences());
    CHECK(route.source == FontSource::NativeCascade);
    CHECK(nativeHarness.generic.counters().nativeCascadeCalls == 1);

    FontRequest greekAlpha = requestFor(0x03B1);
    route = nativeHarness.policy.route(greekAlpha, emptyPreferences());
    CHECK(route.source == FontSource::NativeCascade);
    CHECK(route.glyph.value == 701);
    CHECK(nativeHarness.generic.counters().nativeCascadeCalls == 2);

    route = nativeHarness.policy.route(requestFor(0x10FFFF), emptyPreferences());
    CHECK(route.source == FontSource::Missing);
    CHECK(hasRouteFlag(route.flags, RouteFlag::MissingGlyph));
}

TEST(aCodePointNothingHasIsWalkedOnceAndRememberedByItself)
{
    // The ladder ends by storing a negative route, and the point of storing one
    // is to stop the next ask walking the whole thing again: a miss costs a
    // primary probe, a range-map scan, the script and generic caches and a
    // native cascade call, and a page of a script nobody installed asks that
    // for every distinct character on it.
    Harness harness;
    const FontRequest missing = requestFor(0x10FFFF);
    CHECK(harness.policy.route(missing, emptyPreferences()).source == FontSource::Missing);
    const std::uint64_t afterFirst = harness.generic.counters().nativeCascadeCalls;
    CHECK_MSG(afterFirst == 1, "the first ask walks the ladder");

    CHECK(harness.policy.route(missing, emptyPreferences()).source == FontSource::Missing);
    CHECK_MSG(harness.generic.counters().nativeCascadeCalls == afterFirst,
        "and the second is answered from the negative cache without walking it again");
    CHECK_MSG(harness.negative.counters().negativeHits == 1,
        "counted, so a cache that stopped answering is visible as a number rather than as a slow frame");

    // The discriminating case, and the reason the key carries the code point:
    // a negative is about ONE character, not about its script. Keyed by script
    // alone, the miss above would answer for every other character sharing its
    // class — a page of Greek where one character is absent would render as
    // nothing at all.
    //
    // Each request is routed before the next is built: `requestFor` hands out
    // one static buffer, so two live requests alias it and the second silently
    // rewrites the first. Holding both here reported 0x03FF resolving to
    // 0x03A9's glyph, which is the fixture and not the policy.
    Harness shared;
    CHECK(classifyScript(0x03FF) == classifyScript(0x03A9));
    CHECK_MSG(shared.policy.route(requestFor(0x03FF), emptyPreferences()).source == FontSource::Missing,
        "the character nothing has is missing, which is what puts it in the negative cache");
    CHECK_MSG(shared.policy.route(requestFor(0x03A9), emptyPreferences()).source != FontSource::Missing,
        "a character its neighbour lacks still resolves: the negative is the character's, not the script's");
}

TEST(everyNerdFontIconDrawsFromTheBundledFaceWhateverSymbolFamilyIsConfigured)
{
    // A symbol family that carries no icons, as `symbol_fallback = "STIX Two
    // Math"` configures, leaves the private-use icons to the bundled face that
    // defines them, never to a face the system cascade ranks by coverage.
    Harness harness;
    FontFallbackPreferences preferences = emptyPreferences();
    preferences.symbolFallbackFamily = "STIX Two Math";
    for (const std::uint32_t icon : onefetchLanguageIcons) {
        const GlyphRoute route = harness.policy.route(requestFor(icon), preferences);
        CHECK_MSG(route.font == bundledSymbolFont, "the bundled symbol face draws the icon");
        CHECK(route.source == FontSource::SymbolFallback);
    }
    CHECK_MSG(harness.inventory.cascadeCalls == 0, "and the system cascade is never asked for one");

    Harness unconfigured;
    for (const std::uint32_t icon : onefetchLanguageIcons)
        CHECK_MSG(unconfigured.policy.route(requestFor(icon), emptyPreferences()).font == bundledSymbolFont, "with no symbol family configured too");
}

TEST(aPrivateUseCharacterNoNamedFaceCarriesIsMissingNotACascadeAnswer)
{
    Harness harness;
    FontFallbackPreferences preferences = emptyPreferences();
    preferences.symbolFallbackFamily = "STIX Two Math";
    CHECK(isMissing(harness.policy.route(requestFor(0xF8F0), preferences)));
    CHECK_MSG(harness.inventory.cascadeCalls == 0, "the cascade's private-use answer is never taken");

    // A symbol family naming the primary asks for the primary's own tofu.
    Harness pinned;
    FontFallbackPreferences pinnedPreferences = emptyPreferences();
    pinnedPreferences.symbolFallbackFamily = "primary font";
    CHECK(isMissing(pinned.policy.route(requestFor(0xE61D), pinnedPreferences)));
    CHECK(isMissing(pinned.policy.route(requestFor(0xE632), pinnedPreferences)));
    CHECK(pinned.inventory.cascadeCalls == 0);
}

// The geometric symbols a primary face lacks come from the bundled face rather
// than whichever system face the later stages reach; a face that has one keeps
// it, a reader's range takes precedence, and emoji presentation still reaches
// the emoji stage.
TEST(geometricSymbolsTheFaceLacksComeFromTheBundledFace)
{
    Harness harness;
    FontFallbackPreferences preferences = emptyPreferences();
    preferences.emojiFallbackFamily = "Emoji Font";
    preferences.rangeMaps = std::span<const RangeMapPreference>(defaultRangeMaps.data(), defaultRangeMaps.size());

    GlyphRoute route = harness.policy.route(requestFor(0x25CF), preferences);
    CHECK(route.source == FontSource::Primary);
    for (const auto codePoint : { 0x23FAU, 0x25EFU, 0x25B6U }) {
        route = harness.policy.route(requestFor(codePoint), preferences);
        CHECK(route.source == FontSource::RangeMap);
        CHECK(route.font == bundledGeometricFont);
    }

    FontRequest emoji = requestFor(0x25B6);
    emoji.presentation = Presentation::Emoji;
    route = harness.policy.route(emoji, preferences);
    CHECK(route.source == FontSource::EmojiCascade);
    CHECK(route.font == emojiFont);

    // A code point in a built-in range the bundled face lacks goes on to the
    // stages after the ranges rather than stopping at the range.
    const int cascadesBefore = harness.inventory.cascadeCalls;
    route = harness.policy.route(requestFor(0x25A0), preferences);
    CHECK(route.font != bundledGeometricFont);
    CHECK(harness.inventory.cascadeCalls == cascadesBefore + 1);

    const std::array<RangeMapPreference, 2> own { { { 0x25A0, 0x25FF, "Range Font" }, defaultRangeMaps[1] } };
    preferences.rangeMaps = std::span<const RangeMapPreference>(own.data(), own.size());
    route = harness.policy.route(requestFor(0x25EF), preferences);
    CHECK(route.source == FontSource::RangeMap);
    CHECK(route.font == rangeFont);
}

TEST(testShapedRunFlag)
{
    Harness harness;
    static std::uint32_t arabicRun[] { 0x0633, 0x0644 };
    FontRequest request;
    request.codePoints = std::span<const std::uint32_t>(arabicRun, 2);
    request.script = ScriptClass::Arabic;
    request.width = CellWidthClass::Cluster;
    request.preferenceGeneration = 1;
    GlyphRoute route = harness.policy.route(request, emptyPreferences());
    CHECK(route.source == FontSource::NativeCascade);
    CHECK(hasRouteFlag(route.flags, RouteFlag::ShapedRun));
}

TEST(rangeSizingReachesTheInventoryWithThePrimaryFace)
{
    using Ubiquitty::Rendering::FontMetrics::FallbackSizing;
    for (const auto sizing : { FallbackSizing::Inherit, FallbackSizing::MatchPrimary, FallbackSizing::Nominal }) {
        Harness harness;
        RangeMapPreference range { 0x4E00, 0x9FFF, "Range Font", sizing };
        auto preferences = emptyPreferences();
        preferences.rangeMaps = std::span(&range, 1);
        const auto route = harness.policy.route(requestFor(0x4E01, CellWidthClass::Wide), preferences);
        CHECK(route.source == FontSource::RangeMap);
        CHECK(harness.inventory.lastFamilyRequest.sizing == sizing);
        CHECK(harness.inventory.lastFamilyRequest.sizingBase == primaryFontHandle);
    }
}

TEST(rangesUsingOneFamilyKeepIndependentSizes)
{
    using Ubiquitty::Rendering::FontMetrics::FallbackSizing;
    Harness harness;
    const std::array ranges {
        RangeMapPreference { 0x4E01, 0x4E01, "Range Font", FallbackSizing::MatchPrimary },
        RangeMapPreference { 0x4E02, 0x4E02, "Range Font", FallbackSizing::Nominal },
    };
    auto preferences = emptyPreferences();
    preferences.rangeMaps = ranges;
    CHECK(harness.policy.route(requestFor(0x4E01, CellWidthClass::Wide), preferences).source == FontSource::RangeMap);
    CHECK(harness.inventory.lastFamilyRequest.sizing == FallbackSizing::MatchPrimary);
    CHECK(harness.policy.route(requestFor(0x4E02, CellWidthClass::Wide), preferences).source == FontSource::RangeMap);
    CHECK(harness.inventory.lastFamilyRequest.sizing == FallbackSizing::Nominal);
}

TEST(privateUseIconsKeepCellSizingUnlessARangeOverridesIt)
{
    using Ubiquitty::Rendering::FontMetrics::FallbackSizing;
    Harness harness;
    auto preferences = emptyPreferences();
    preferences.symbolFallbackFamily = "Symbols Font";
    CHECK(harness.policy.route(requestFor(0xE0B0), preferences).source == FontSource::SymbolFallback);
    CHECK(harness.inventory.lastFamilyRequest.sizing == FallbackSizing::Nominal);
    RangeMapPreference range { 0xE0B0, 0xE0B0, "Symbols Font", FallbackSizing::MatchPrimary };
    preferences.rangeMaps = std::span(&range, 1);
    CHECK(harness.policy.route(requestFor(0xE0B0), preferences).source == FontSource::RangeMap);
    CHECK(harness.inventory.lastFamilyRequest.sizing == FallbackSizing::MatchPrimary);
}

UBIQUITTY_TEST_MAIN()
