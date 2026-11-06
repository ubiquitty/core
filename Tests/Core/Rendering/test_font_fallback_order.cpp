// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_font_fallback_order — which tier wins when two of them can answer.
//
// `FontFallbackPolicy::route` is a ladder of nine stages, and its sequence is
// the whole of the policy: every stage can produce a route, and the first one
// that does decides what a cell draws. Swapping two stages — a pure
// reordering, nothing added or removed — passes every case that tests one tier
// at a time.
//
// The reason is in the fixture rather than in the assertions. Its inventory
// answers each code point from exactly one font, so for any request at most one
// stage can produce a route and the rest return missing. Every case exercises
// one tier at a time, and a ladder whose rungs never compete has no order to
// test. Swapping any adjacent pair is inert by construction.
//
// So this file supplies an inventory where fonts OVERLAP: several code points
// are covered by two families at once, one reachable from each of two stages.
// A request for such a code point makes the two compete, and the assertion is
// which one wins.
//
// Every case here fails if the two stages it names are exchanged, and that was
// checked by exchanging them rather than by reading the ladder.

#include "Core/Rendering/Font/FontFallbackPolicy.h"
#include "Core/Rendering/Font/ScriptClassifier.h"

#include <cstdint>
#include <span>
#include <string_view>

using namespace Ubiquitty::Rendering::Font;

namespace {

constexpr FontHandle primaryFontHandle { 1, 1 };
constexpr FontHandle rangeFont { 2, 1 };
constexpr FontHandle hanFont { 3, 1 };
constexpr FontHandle symbolFont { 4, 1 };
constexpr FontHandle emojiFont { 5, 1 };
constexpr FontHandle scriptFont { 6, 1 };
constexpr FontHandle cascadeFont { 7, 1 };

// Code points chosen so each is covered by two families, one per competing
// stage. The script class each falls into is what selects the branch, so they
// are picked from the ranges `classifyScript` sorts into Han, Symbols and Emoji.
constexpr std::uint32_t contestedLatin = 'A'; // primary and rangeMap
constexpr std::uint32_t contestedHan = 0x6771; // rangeMap, explicit Han, Han cascade
constexpr std::uint32_t contestedSymbol = 0xE0B0; // rangeMap and symbol
constexpr std::uint32_t contestedEmoji = 0x1F600; // rangeMap, named emoji, emoji cascade
constexpr std::uint32_t contestedArabic = 0x0633; // script fallback and native cascade

bool same(std::string_view left, const char* right) { return left == right; }

/// An inventory whose families overlap, so more than one stage can answer.
///
/// The shared fixture in test_font_fallback_policy.cpp answers each code point
/// once on purpose — it is testing that each stage works. This one exists to
/// make them compete, which is a different question and needs a different fake.
class OverlappingInventory final : public FontInventory {
public:
    [[nodiscard]] std::uint32_t catalogGeneration() const override { return 1; }
    void bumpCatalogGeneration() override { }

    FontHandle primaryFont(FontStyle, std::uint8_t) override { return primaryFontHandle; }

    FontHandle validateFamily(const FontFamilyRequest& request) override
    {
        if (same(request.family, "Range Font"))
            return rangeFont;
        if (same(request.family, "Han Font"))
            return hanFont;
        if (same(request.family, "Symbols Font"))
            return symbolFont;
        if (same(request.family, "Emoji Font"))
            return emojiFont;
        if (same(request.family, "Script Font"))
            return scriptFont;
        return {};
    }

    GlyphProbeResult glyphForCodePoint(FontHandle font, std::uint32_t codePoint) override
    {
        // The primary covers only Latin, so every other contest below starts
        // past the first rung rather than being decided by it.
        if (font == primaryFontHandle && codePoint == contestedLatin)
            return { font, { 65 }, true, false };
        // The range font covers all four contested code points, so it competes
        // with whichever stage would otherwise claim each one.
        if (font == rangeFont
            && (codePoint == contestedLatin || codePoint == contestedHan
                || codePoint == contestedSymbol || codePoint == contestedEmoji))
            return { font, { 200 }, true, false };
        if (font == hanFont && codePoint == contestedHan)
            return { font, { 300 }, true, false };
        if (font == symbolFont && codePoint == contestedSymbol)
            return { font, { 400 }, true, false };
        if (font == emojiFont && codePoint == contestedEmoji)
            return { font, { 500 }, true, false };
        if (font == scriptFont && codePoint == contestedArabic)
            return { font, { 600 }, true, false };
        return { font, {}, false, false };
    }

    CascadeResult cascadeForCodePoint(const CascadeRequest& request) override
    {
        if (request.codePoints.empty())
            return {};
        // The cascade answers for every contested code point, so it is always
        // the losing side of whichever named stage precedes it.
        const std::uint32_t codePoint = request.codePoints[0];
        if (codePoint == contestedHan || codePoint == contestedEmoji || codePoint == contestedArabic)
            return { cascadeFont, { 900 }, true, false };
        return {};
    }

    GlyphProbeResult glyphForCluster(FontHandle font, std::span<const std::uint32_t> codePoints, Presentation) override
    {
        return codePoints.empty() ? GlyphProbeResult {} : glyphForCodePoint(font, codePoints.front());
    }

    CascadeResult cascadeForRun(const CascadeRequest& request) override { return cascadeForCodePoint(request); }

    [[nodiscard]] bool isLastResort(FontHandle) const override { return false; }
    [[nodiscard]] std::string_view familyName(FontHandle) const override { return "fake"; }
    [[nodiscard]] std::string_view postScriptName(FontHandle) const override { return "fake"; }
};

struct Harness {
    OverlappingInventory inventory;
    HanFallbackCache han { 8 };
    ScriptFallbackCache script { 8 };
    GenericFallbackCache generic { 8 };
    EmojiFallbackCache emoji { 8 };
    SymbolFallbackCache symbol { 8 };
    RangeMapCache rangeMap { 8 };
    NegativeRouteCache negative { 8 };
    FontFallbackPolicy policy { inventory, { han, script, generic, emoji, symbol, rangeMap, negative } };
};

std::uint32_t requestStorage = 0;

FontRequest requestFor(std::uint32_t codePoint, CellWidthClass width = CellWidthClass::Narrow)
{
    requestStorage = codePoint;
    FontRequest request;
    request.codePoints = std::span<const std::uint32_t>(&requestStorage, 1);
    request.script = classifyScript(codePoint);
    request.width = width;
    request.preferenceGeneration = 1;
    return request;
}

RangeMapPreference wholePlane { 0x0000, 0x10FFFF, "Range Font" };

/// Preferences with a range map over everything, plus every named fallback.
///
/// One helper rather than one per case: the point is that several stages are
/// armed at once, and a helper that arms only the pair under test would be the
/// same single-stage fixture in a smaller package.
FontFallbackPreferences competingPreferences()
{
    return FontFallbackPreferences(
        std::span<const RangeMapPreference>(&wholePlane, 1),
        {},
        "Han Font",
        "Symbols Font",
        "Emoji Font",
        1);
}

/// The same, with the range map removed so the next stage down can be seen.
FontFallbackPreferences withoutRangeMap()
{
    FontFallbackPreferences preferences = competingPreferences();
    preferences.rangeMaps = {};
    return preferences;
}

} // namespace

TEST(theInventoryLetsTwoStagesAnswerAtOnce)
{
    // The control. Without it a case below could pass because only one stage
    // ever answered, which is the state this file exists to leave — and it
    // would read exactly like the tier ordering holding.
    Harness harness;
    OverlappingInventory& inventory = harness.inventory;
    CHECK(inventory.glyphForCodePoint(rangeFont, contestedHan).present);
    CHECK(inventory.glyphForCodePoint(hanFont, contestedHan).present);
    CHECK(inventory.glyphForCodePoint(rangeFont, contestedSymbol).present);
    CHECK(inventory.glyphForCodePoint(symbolFont, contestedSymbol).present);
    CHECK(inventory.glyphForCodePoint(rangeFont, contestedEmoji).present);
    CHECK(inventory.glyphForCodePoint(emojiFont, contestedEmoji).present);
}

TEST(thePrimaryOutranksARangeMapThatAlsoCoversTheCodePoint)
{
    Harness harness;
    const GlyphRoute route = harness.policy.route(requestFor(contestedLatin), competingPreferences());
    CHECK(route.source == FontSource::Primary);
    CHECK(route.font == primaryFontHandle);
}

TEST(aRangeMapOutranksTheHanStages)
{
    Harness harness;
    const GlyphRoute route = harness.policy.route(requestFor(contestedHan, CellWidthClass::Wide), competingPreferences());
    CHECK(route.source == FontSource::RangeMap);
    CHECK(route.font == rangeFont);

    // And with the range map gone the Han stage takes it, so the case above is
    // about which of two live stages wins rather than about the Han stage being
    // unable to answer at all.
    Harness withoutRange;
    const GlyphRoute han = withoutRange.policy.route(requestFor(contestedHan, CellWidthClass::Wide), withoutRangeMap());
    CHECK(han.source == FontSource::ExplicitHan);
    CHECK(han.font == hanFont);
}

TEST(aRangeMapOutranksTheSymbolStage)
{
    Harness harness;
    const GlyphRoute route = harness.policy.route(requestFor(contestedSymbol), competingPreferences());
    CHECK(route.source == FontSource::RangeMap);

    Harness withoutRange;
    const GlyphRoute symbol = withoutRange.policy.route(requestFor(contestedSymbol), withoutRangeMap());
    CHECK(symbol.source == FontSource::SymbolFallback);
    CHECK(symbol.font == symbolFont);
}

TEST(aRangeMapOutranksTheEmojiStages)
{
    Harness harness;
    const GlyphRoute route = harness.policy.route(requestFor(contestedEmoji, CellWidthClass::Wide), competingPreferences());
    CHECK(route.source == FontSource::RangeMap);

    Harness withoutRange;
    const GlyphRoute emoji = withoutRange.policy.route(requestFor(contestedEmoji, CellWidthClass::Wide), withoutRangeMap());
    CHECK(emoji.source == FontSource::EmojiCascade);
    CHECK(emoji.font == emojiFont); // the NAMED family, not the cascade's font
}

TEST(aNamedEmojiFamilyOutranksTheNativeEmojiCascade)
{
    // Both can answer for this code point: the named family covers it, and the
    // cascade returns a different font for it. The named one is checked first,
    // so the cascade's font must not appear.
    FontFallbackPreferences preferences = withoutRangeMap();
    Harness harness;
    const GlyphRoute route = harness.policy.route(requestFor(contestedEmoji, CellWidthClass::Wide), preferences);
    CHECK(route.font == emojiFont);
    CHECK(route.font != cascadeFont);

    // Drop the named family and the cascade takes it, which is what says the
    // assertion above is an ordering rather than the cascade being dead.
    preferences.emojiFallbackFamily = {};
    Harness cascadeOnly;
    const GlyphRoute viaCascade = cascadeOnly.policy.route(requestFor(contestedEmoji, CellWidthClass::Wide), preferences);
    CHECK(viaCascade.font == cascadeFont);
}

TEST(anExplicitHanFamilyOutranksTheHanCascade)
{
    FontFallbackPreferences preferences = withoutRangeMap();
    Harness harness;
    const GlyphRoute route = harness.policy.route(requestFor(contestedHan, CellWidthClass::Wide), preferences);
    CHECK(route.source == FontSource::ExplicitHan);
    CHECK(route.font == hanFont);

    preferences.cjkFallbackFamily = {};
    Harness cascadeOnly;
    const GlyphRoute viaCascade = cascadeOnly.policy.route(requestFor(contestedHan, CellWidthClass::Wide), preferences);
    CHECK(viaCascade.source == FontSource::AutoHanCascade);
    CHECK(viaCascade.font == cascadeFont);
}

TEST(aScriptFallbackOutranksTheNativeCascade)
{
    // Arabic reaches neither the Han nor the symbol branch, so this pair is the
    // one the ladder decides at its foot rather than inside a script arm.
    const LanguageFallbackPreference arabic { classifyScript(contestedArabic), {}, "Script Font" };
    FontFallbackPreferences preferences = withoutRangeMap();
    preferences.languageFallbacks = std::span<const LanguageFallbackPreference>(&arabic, 1);

    Harness harness;
    const GlyphRoute route = harness.policy.route(requestFor(contestedArabic), preferences);
    CHECK(route.font == scriptFont);
    CHECK(route.font != cascadeFont);

    preferences.languageFallbacks = {};
    Harness cascadeOnly;
    const GlyphRoute viaCascade = cascadeOnly.policy.route(requestFor(contestedArabic), preferences);
    CHECK(viaCascade.font == cascadeFont);
}

UBIQUITTY_TEST_MAIN()
