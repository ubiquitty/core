// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_render_configuration.cpp — every renderer setting reaches the struct
// every renderer is configured from, and every field of it is reachable from a
// document.
//
// Two directions, because each catches what the other cannot:
//   - key → field: every key `reachOf` places in `Renderer` is driven from a
//     document to a non-default value and must move the fold. A key placed
//     there without a derivation fails here; a key added to `Key` without a
//     placement fails to compile in `reachOf`.
//   - field → key: one document sets every renderer key off its default and
//     every field of the struct must differ from the default table. A field
//     added to the struct without a derivation fails here, and a field added
//     without being named below fails the size assertion.

#include "Harness.h"

#include "Core/Preferences/EffectiveValue.h"
#include "Core/Preferences/FontRoutePreferences.h"
#include "Core/Preferences/Key.h"
#include "Core/Preferences/Preferences.h"
#include "Core/Preferences/RenderConfigurationFold.h"
#include "Core/Preferences/RenderStyle.h"
#include "Core/Preferences/Vocabulary.h"
#include "Core/Rendering/RenderConfiguration.h"

#include <cmath>
#include <functional>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace Preferences = Ubiquitty::Preferences;
namespace Effective = Ubiquitty::Preferences::Effective;
using Preferences::Key;
using Preferences::SerializeKind;
using Preferences::Value;
using Preferences::Values;
using Ubiquitty::Rendering::RenderConfiguration;

namespace {

Values parseDocument(const std::string& document)
{
    Values values;
    std::istringstream in(document);
    (void)Preferences::parse(in, values, Preferences::ParseMode::Strict);
    return values;
}

std::string line(const Preferences::WriteDescriptor& descriptor, const std::string& literal)
{
    return "[" + std::string(descriptor.section) + "]\n" + descriptor.key + " = " + literal + "\n";
}

// Literals a key of this kind might take that differ from its default. More
// than one, because a bounded key's default can sit at either end of its range
// and a free-text key may be a family, a colour or a path.
std::vector<std::string> candidateLiterals(const Preferences::WriteDescriptor& descriptor, const Value& defaultValue)
{
    std::vector<std::string> literals;
    switch (descriptor.kind) {
    case SerializeKind::Bool:
    case SerializeKind::LegacyBoolOrToken:
        literals.push_back(Preferences::tomlBool(!defaultValue.boolean));
        break;
    case SerializeKind::Int:
        for (long long delta : { 1LL, -1LL, 50LL, -50LL, 100LL })
            literals.push_back(Preferences::tomlInt(defaultValue.integer + delta));
        break;
    case SerializeKind::Double:
        for (double delta : { 0.5, -0.5, 0.25, -0.25, 3.0, 60.0 })
            literals.push_back(Preferences::tomlDouble(defaultValue.number + delta));
        break;
    case SerializeKind::EnumToken:
    case SerializeKind::String:
        for (const auto& term : Preferences::vocabularyFor(descriptor.id))
            literals.push_back(Preferences::tomlString(term.token));
        for (const char* text : { "#123456", "Courier New", "/tmp/backdrop.png", "zero", "wght=500", "tint", "display-p3", "deuteranopia" })
            literals.push_back(Preferences::tomlString(text));
        break;
    }
    return literals;
}

// The literal that moves the fold for this key, if any does.
std::optional<std::string> movingLiteral(const Preferences::WriteDescriptor& descriptor)
{
    const Values defaulted;
    const RenderConfiguration baseline = Effective::renderConfiguration(defaulted);
    const std::optional<Value> defaultValue = Preferences::effectiveValue(descriptor.id, defaulted);
    if (!defaultValue.has_value())
        return std::nullopt;
    for (const std::string& literal : candidateLiterals(descriptor, *defaultValue)) {
        const Values stated = parseDocument(line(descriptor, literal));
        if (!stated.skippedKeys.empty())
            continue;
        if (!(Effective::renderConfiguration(stated) == baseline))
            return literal;
    }
    return std::nullopt;
}

struct Field {
    const char* name;
    std::function<bool(const RenderConfiguration&, const RenderConfiguration&)> isEqual;
};

#define UBIQUITTY_FIELD(member)                                                                                                \
    Field                                                                                                                      \
    {                                                                                                                          \
        #member, [](const RenderConfiguration& left, const RenderConfiguration& right) { return left.member == right.member; } \
    }

const std::vector<Field>& fields()
{
    static const std::vector<Field> all = {
        UBIQUITTY_FIELD(fontFamily),
        UBIQUITTY_FIELD(fontSizePoints),
        UBIQUITTY_FIELD(proportionalFontFamily),
        UBIQUITTY_FIELD(proportionalFontSizePoints),
        UBIQUITTY_FIELD(symbolFallbackFamily),
        UBIQUITTY_FIELD(cjkFallbackFamily),
        UBIQUITTY_FIELD(emojiFallbackFamily),
        UBIQUITTY_FIELD(languageFallbacks),
        UBIQUITTY_FIELD(languageFallbackCount),
        UBIQUITTY_FIELD(rangeMaps),
        UBIQUITTY_FIELD(rangeMapCount),
        UBIQUITTY_FIELD(fontFeatures),
        UBIQUITTY_FIELD(fontVariations),
        UBIQUITTY_FIELD(hasLigatures),
        UBIQUITTY_FIELD(fontWeight),
        UBIQUITTY_FIELD(boldFontWeight),
        UBIQUITTY_FIELD(fontWidthPercent),
        UBIQUITTY_FIELD(lineSpacingPoints),
        UBIQUITTY_FIELD(characterSpacingPoints),
        UBIQUITTY_FIELD(baselineOffsetPoints),
        UBIQUITTY_FIELD(shouldBoostWideGlyphs),
        UBIQUITTY_FIELD(shouldMatchFallbackSize),
        UBIQUITTY_FIELD(wideGlyphScale),
        UBIQUITTY_FIELD(shouldDrawBrailleGrid),
        UBIQUITTY_FIELD(brailleStyle),
        UBIQUITTY_FIELD(shadeStyle),
        UBIQUITTY_FIELD(shouldBrightenBold),
        UBIQUITTY_FIELD(selectionStyle),
        UBIQUITTY_FIELD(nonOwnerPresentation),
        UBIQUITTY_FIELD(underlineThicknessScale),
        UBIQUITTY_FIELD(underlineOffsetPoints),
        UBIQUITTY_FIELD(shouldUnderlineSkipInk),
        UBIQUITTY_FIELD(shouldUnderlineFollowInk),
        UBIQUITTY_FIELD(underlineSkipInkPaddingPoints),
        UBIQUITTY_FIELD(shouldUnderlineLinks),
        UBIQUITTY_FIELD(linkUnderlineStyle),
        UBIQUITTY_FIELD(shouldStyleLinksPerKind),
        UBIQUITTY_FIELD(shouldUseSystemBlueLinkColor),
        UBIQUITTY_FIELD(paddingLeftPoints),
        UBIQUITTY_FIELD(paddingRightPoints),
        UBIQUITTY_FIELD(paddingTopPoints),
        UBIQUITTY_FIELD(paddingBottomPoints),
        UBIQUITTY_FIELD(titlelessPaddingTopPoints),
        UBIQUITTY_FIELD(usesDisplayP3),
        UBIQUITTY_FIELD(colorblindMode),
        UBIQUITTY_FIELD(backgroundOpacity),
        UBIQUITTY_FIELD(defaultBackgroundOpacity),
        UBIQUITTY_FIELD(blurRadiusPoints),
        UBIQUITTY_FIELD(shouldEnhanceTextContrast),
        UBIQUITTY_FIELD(shouldEnhanceTextOnBlur),
        UBIQUITTY_FIELD(usesBackgroundVibrancy),
        UBIQUITTY_FIELD(backgroundImagePath),
        UBIQUITTY_FIELD(backgroundImageOpacity),
        UBIQUITTY_FIELD(backgroundImageMode),
        UBIQUITTY_FIELD(cursorAlpha),
        UBIQUITTY_FIELD(shouldCursorInvertText),
        UBIQUITTY_FIELD(shouldFadeBlink),
        UBIQUITTY_FIELD(isBellVisual),
        UBIQUITTY_FIELD(bellFlashPeakAlpha),
        UBIQUITTY_FIELD(usesCustomTekColors),
        UBIQUITTY_FIELD(tekBeamRGB),
        UBIQUITTY_FIELD(tekBackgroundRGB),
        UBIQUITTY_FIELD(drawsTekWritingSpot),
        UBIQUITTY_FIELD(drawsTekGlowVectors),
        UBIQUITTY_FIELD(maxFramesPerSecond),
        UBIQUITTY_FIELD(isPresentSynchronized),
    };
    return all;
}

#undef UBIQUITTY_FIELD

// A field added to the struct moves its size; this fails to compile until the
// list above names it and the size here is updated with it. Every member is a
// fixed-width type, so the layout is the same on every target.
static_assert(sizeof(RenderConfiguration) == 10688, "a RenderConfiguration field is missing from the list above");

// Keys with no `Key` handle that still reach the renderer, at a non-default
// value each. The write table covers what a settings surface edits; these are
// file-only.
constexpr const char* fileOnlyRendererLines = "[render]\n"
                                              "max_fps = 30\n"
                                              "vsync = false\n"
                                              "[bell]\n"
                                              "visual_intensity = 0.8\n"
                                              "[appearance]\n"
                                              "default_background_opacity = 0.5\n"
                                              "[[font.language_fallbacks]]\n"
                                              "script = \"han\"\n"
                                              "language = \"ja\"\n"
                                              "mode = \"custom\"\n"
                                              "family = \"Hiragino Sans\"\n"
                                              "[[font.range_map]]\n"
                                              "range = \"U+E000-U+F8FF\"\n"
                                              "mode = \"custom\"\n"
                                              "family = \"Symbols Nerd Font\"\n";

} // namespace

TEST(testEveryRendererKeyReachesTheConfiguration)
{
    int checked = 0;
    for (int index = 0; index < Preferences::writeDescriptorCount; ++index) {
        const auto& descriptor = Preferences::writeDescriptors[index];
        if (Effective::reachOf(descriptor.id) != Effective::KeyReach::Renderer)
            continue;
        ++checked;
        if (!movingLiteral(descriptor).has_value()) {
            std::printf("FAIL [%s] %s is placed in the renderer and moves no field of its configuration\n", descriptor.section, descriptor.key);
            failures++;
        }
    }
    CHECK_MSG(checked >= 50, "the sweep covers the renderer's whole key surface");
}

TEST(testEveryFieldIsReachableFromADocument)
{
    std::string document = fileOnlyRendererLines;
    for (int index = 0; index < Preferences::writeDescriptorCount; ++index) {
        const auto& descriptor = Preferences::writeDescriptors[index];
        if (Effective::reachOf(descriptor.id) != Effective::KeyReach::Renderer)
            continue;
        // Colour mode is what lets the custom tube colours through, and the
        // padding shorthand is overruled by the edges, so the document states
        // the values that make every field observable at once.
        if (descriptor.id == Key::TekColorMode) {
            document += line(descriptor, "1");
            continue;
        }
        if (const std::optional<std::string> literal = movingLiteral(descriptor))
            document += line(descriptor, *literal);
    }
    const Values stated = parseDocument(document);
    for (const auto& diagnostic : stated.skippedKeys)
        std::printf("unexpected diagnostic: line %d [%s] %s\n", diagnostic.line, diagnostic.section.c_str(), diagnostic.key.c_str());
    CHECK(stated.skippedKeys.empty());

    const RenderConfiguration baseline = Effective::renderConfiguration(Values {});
    const RenderConfiguration configured = Effective::renderConfiguration(stated);
    for (const Field& field : fields()) {
        if (field.isEqual(baseline, configured)) {
            std::printf("FAIL %s is not reachable from a document\n", field.name);
            failures++;
        }
    }
}

TEST(testTheFoldOfAnEmptyDocumentIsNotTheZeroValue)
{
    // The struct's member initialisers are zeroes; the default table is the
    // fold. A renderer handed `RenderConfiguration {}` would draw without
    // ligatures, without link underlines and with no padding.
    CHECK(!(Effective::renderConfiguration(Values {}) == RenderConfiguration {}));
    CHECK(Effective::renderConfiguration(Values {}).hasLigatures);
}

TEST(testAnOverlongTextValueReadsAsItsDefault)
{
    const std::string longFamily(Ubiquitty::Rendering::FamilyText {}.bytes.size() + 1, 'a');
    const Values stated = parseDocument("[font]\nfamily = \"" + longFamily + "\"\n");
    CHECK_MSG(Effective::renderConfiguration(stated).fontFamily.isEmpty(), "a family too long to hold is not truncated into another name");
}

TEST(testTypographyComparisonSeparatesSheetsFromStyle)
{
    const RenderConfiguration baseline = Effective::renderConfiguration(Values {});
    const RenderConfiguration restyled = Effective::renderConfiguration(parseDocument("[underline]\nthickness = 2.0\n"));
    const RenderConfiguration resized = Effective::renderConfiguration(parseDocument("[font]\nsize = 19.0\n"));
    CHECK_MSG(baseline.hasSameTypography(restyled), "a frame-style change cuts no new sheets");
    CHECK_MSG(!baseline.hasSameTypography(resized), "a size change does");
}

// A profile is folded into the configuration before the renderer's fold, so
// a window opened under one draws its face, size, fallback, leading and
// padding, and every key it leaves unset keeps the configuration's answer.
TEST(testAProfileFoldsIntoTheRendererConfiguration)
{
    const Values values = parseDocument("[font]\nfamily = \"Base Mono\"\nsize = 13.0\nligatures = true\n[appearance]\npadding_left = 3.0\n");
    Preferences::Profile profile("Work");
    profile.fontFamily = "Work Mono";
    profile.fontSize = 18.0;
    profile.fallbackFont = "Work Symbols";
    profile.lineSpacing = 2.0;
    profile.paddingTop = 9.0;

    const RenderConfiguration plain = Effective::renderConfiguration(values);
    const RenderConfiguration overlaid = Effective::renderConfiguration(values, profile);
    CHECK(overlaid.fontFamily.view() == "Work Mono");
    CHECK(overlaid.fontSizePoints == 18.0);
    CHECK(overlaid.symbolFallbackFamily.view() == "Work Symbols");
    CHECK(overlaid.lineSpacingPoints == 2.0);
    CHECK(overlaid.paddingTopPoints == 9.0);
    // A profile's titled top leaves the separate titleless inset alone.
    CHECK_MSG(overlaid.titlelessPaddingTopPoints == plain.titlelessPaddingTopPoints, "the profile's titled top leaves the titleless inset alone");
    CHECK_MSG(overlaid.paddingLeftPoints == plain.paddingLeftPoints, "an edge the profile leaves unset keeps the configuration's");
    CHECK_MSG(overlaid.hasLigatures, "a key the profile cannot set keeps the configuration's");
    CHECK_MSG(Effective::renderConfiguration(values, Preferences::Profile("Empty")) == plain, "an empty profile changes nothing");
}

TEST(testTitlelessPaddingTopIsIndependentOfTitledPadding)
{
    const auto rendered = [](const Values& values, const std::string& name = "") {
        const Preferences::Profile profile = Preferences::resolvedSessionProfile(values, name, {}, {});
        return Effective::renderConfiguration(values, profile);
    };

    const Values perEdge = parseDocument("[appearance]\npadding_top = 11\npadding_bottom = 4\ntitleless_padding_top = 17\n");
    CHECK(rendered(perEdge).paddingTopPoints == 11.0);
    CHECK(rendered(perEdge).titlelessPaddingTopPoints == 17.0);

    const Values shorthand = parseDocument("[appearance]\npadding = 12\ntitleless_padding_top = 17\n");
    CHECK(rendered(shorthand).paddingTopPoints == 12.0);
    CHECK(rendered(shorthand).titlelessPaddingTopPoints == 17.0);

    const Values fallback = parseDocument("[appearance]\npadding_top = 11\npadding_bottom = 4\n");
    CHECK(rendered(fallback).paddingTopPoints == 11.0);
    CHECK(rendered(fallback).titlelessPaddingTopPoints == 4.0);

    const Values profiles = parseDocument("[appearance]\npadding_top = 11\npadding_bottom = 4\ntitleless_padding_top = 17\n"
                                          "[[profiles]]\nname = \"Titled\"\npadding_top = 9\n"
                                          "[[profiles]]\nname = \"Titleless\"\ntitleless_padding_top = 21\n");
    CHECK(rendered(profiles, "Titled").paddingTopPoints == 9.0);
    CHECK(rendered(profiles, "Titled").titlelessPaddingTopPoints == 17.0);
    CHECK(rendered(profiles, "Titleless").paddingTopPoints == 11.0);
    CHECK(rendered(profiles, "Titleless").titlelessPaddingTopPoints == 21.0);
}

// The appliers every C++ renderer styles its frame through carry the cursor's
// alpha and the bell's flash; a frame styled without them draws a solid cursor
// and no flash whatever the document says.
TEST(testTheFrameAppliersCarryTheCursorAndTheBell)
{
    const RenderConfiguration configured = Effective::renderConfiguration(
        parseDocument("[cursor]\nalpha = 128\ninvert_text = false\n[bell]\nvisual = true\nvisual_intensity = 0.6\n"));
    Ubiquitty::Rendering::TerminalSnapshotOptions options;
    Effective::applyCursorSettings(options, configured);
    CHECK(std::abs(options.cursorAlpha - 128.0f / 255.0f) < 0.001f);
    CHECK(!options.shouldCursorInvertText);
    CHECK_MSG(std::abs(Effective::bellFlashAlpha(configured, 1, 1) - 0.6f) < 0.001f, "a still frame as the bell rings draws the peak");
    CHECK_MSG(Effective::bellFlashAlpha(configured, 0, 1) == 0.0f, "no flash started is no flash");
    const RenderConfiguration quiet = Effective::renderConfiguration(Values {});
    CHECK_MSG(Effective::bellFlashAlpha(quiet, 1, 1) == 0.0f, "an audible bell does not flash");
}

TEST(fallbackSizingChangesRoutesAndTypography)
{
    const Values defaults;
    const Values originalSize = parseDocument("[font]\nmatch_fallback_size = false\n");
    const RenderConfiguration automatic = Effective::renderConfiguration(defaults);
    const RenderConfiguration nominal = Effective::renderConfiguration(originalSize);
    CHECK(automatic.shouldMatchFallbackSize);
    CHECK(!nominal.shouldMatchFallbackSize);
    CHECK(!automatic.hasSameTypography(nominal));
    CHECK(Preferences::buildFontRoutePreferences(defaults).generation != Preferences::buildFontRoutePreferences(originalSize).generation);
}

TEST(rangeSizingChangesTypographyAndSurvivesTheRenderFold)
{
    const auto document = "[[font.range_map]]\nrange = \"U+2426\"\nmode = \"custom\"\nfamily = \"Apple Symbols\"\n";
    const auto inherited = Effective::renderConfiguration(parseDocument(document));
    const auto matched = Effective::renderConfiguration(parseDocument(std::string(document) + "match_size = true\n"));
    const auto nominal = Effective::renderConfiguration(parseDocument(std::string(document) + "match_size = false\n"));
    CHECK(inherited.rangeMaps[0].sizing == Ubiquitty::Rendering::FontMetrics::FallbackSizing::Inherit);
    CHECK(matched.rangeMaps[0].sizing == Ubiquitty::Rendering::FontMetrics::FallbackSizing::MatchPrimary);
    CHECK(nominal.rangeMaps[0].sizing == Ubiquitty::Rendering::FontMetrics::FallbackSizing::Nominal);
    CHECK(!matched.hasSameTypography(nominal));
    CHECK(!inherited.hasSameTypography(nominal));
}

UBIQUITTY_TEST_MAIN()
