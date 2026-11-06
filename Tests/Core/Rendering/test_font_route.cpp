// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// FontRoute and ScriptClassifier: portable font-routing request/route types and
// script classification. Header-only and platform-free.

#include "Harness.h"
#include "TerminalFixture.h"

#include "Core/CellPen.h"
#include "Core/Rendering/Font/FontRoute.h"
#include "Core/Rendering/Font/ScriptClassifier.h"
#include "Core/Terminal.h"
#include "Core/Unicode/UTF8Encoder.h"

#include <array>
#include <cstdio>
#include <vector>

using namespace Ubiquitty::Rendering::Font;

TEST(testClusterPresentationMatchesStoredTerminalPresentation)
{
    struct Case {
        std::vector<std::uint32_t> codePoints;
        Presentation expected;
    };
    const std::array<Case, 14> cases { {
        { { 'A' }, Presentation::Default },
        { { 'A', 0xFE0F }, Presentation::Default },
        { { '1', 0xFE0F }, Presentation::Default },
        { { '1', 0x20E3 }, Presentation::Emoji },
        { { '1', 0xFE0E, 0x20E3 }, Presentation::Emoji },
        { { '1', 0xFE0F, 0x20E3, 0xFE0E }, Presentation::Emoji },
        { { 0x2764 }, Presentation::Default },
        { { 0x2764, 0xFE0F }, Presentation::Emoji },
        { { 0x2764, 0xFE0F, 0xFE0E }, Presentation::Text },
        { { 0x2764, 0xFE0E, 0xFE0F }, Presentation::Emoji },
        { { 0x1F600 }, Presentation::Emoji },
        { { 0x1F600, 0xFE0E }, Presentation::Text },
        { { 0x1F469, 0x200D, 0x1F680 }, Presentation::Emoji },
        { { 0x1F1EC, 0x1F1E7 }, Presentation::Emoji },
    } };
    CHECK(presentationForCluster({}) == Presentation::Default);
    for (const auto& test : cases) {
        CHECK(presentationForCluster(test.codePoints) == test.expected);
        std::string bytes;
        for (auto codePoint : test.codePoints) {
            Ubiquitty::Unicode::appendUTF8(bytes, codePoint);
        }
        for (bool shouldSplitInput : { false, true }) { // bool-ok: Exercises both burst and byte-split parser input.
            Ubiquitty::Terminal terminal;
            Ubiquitty::hardReset(terminal);
            if (shouldSplitInput) {
                for (const char byte : bytes) {
                    UbiquittyTest::feed(terminal, std::string_view(&byte, 1));
                }
            } else {
                UbiquittyTest::feed(terminal, bytes);
            }
            const auto* cell = UbiquittyTest::cell(terminal, 0, 0);
            REQUIRE(cell);
            CHECK(Ubiquitty::cellPen(*cell).attributes.contains(Ubiquitty::VT::Attribute::Emoji) == (test.expected == Presentation::Emoji));
        }
    }
}

TEST(testScriptClassification)
{
    CHECK(classifyScript(0x6771) == ScriptClass::Han);
    CHECK(classifyScript(0x3400) == ScriptClass::Han);
    CHECK(classifyScript(0x4DBF) == ScriptClass::Han);
    CHECK(classifyScript(0x4E00) == ScriptClass::Han);
    CHECK(classifyScript(0x9FFF) == ScriptClass::Han);
    CHECK(classifyScript(0xF900) == ScriptClass::Han);
    CHECK(classifyScript(0xFAFF) == ScriptClass::Han);
    CHECK(classifyScript(0x20000) == ScriptClass::Han);
    CHECK(classifyScript(0x2A6DF) == ScriptClass::Han);
    CHECK(classifyScript(0x2A700) == ScriptClass::Han);
    CHECK(classifyScript(0x2B73F) == ScriptClass::Han);
    CHECK(classifyScript(0x2B740) == ScriptClass::Han);
    CHECK(classifyScript(0x2B81F) == ScriptClass::Han);
    CHECK(classifyScript(0x2B820) == ScriptClass::Han);
    CHECK(classifyScript(0x2CEAF) == ScriptClass::Han);
    CHECK(classifyScript(0x2CEB0) == ScriptClass::Han);
    CHECK(classifyScript(0x2EBEF) == ScriptClass::Han);
    CHECK(classifyScript(0x30000) == ScriptClass::Han);
    CHECK(classifyScript(0x323AF) == ScriptClass::Han);

    CHECK(classifyScript(0x30AA) == ScriptClass::Kana);
    CHECK(classifyScript(0xAC00) == ScriptClass::Hangul);
    CHECK(classifyScript(0x041F) == ScriptClass::Cyrillic);
    CHECK(classifyScript(0x03A9) == ScriptClass::Greek);
    CHECK(classifyScript(0x0633) == ScriptClass::Arabic);
    CHECK(classifyScript(0x05D0) == ScriptClass::Hebrew);
    CHECK(classifyScript(0x0915) == ScriptClass::Devanagari);
    CHECK(classifyScript(0x0995) == ScriptClass::Bengali);
    CHECK(classifyScript(0x0A15) == ScriptClass::Gurmukhi);
    CHECK(classifyScript(0x0E01) == ScriptClass::Thai);
    CHECK(classifyScript(0x0E81) == ScriptClass::Lao);
    CHECK(classifyScript(0x1780) == ScriptClass::Khmer);
    CHECK(classifyScript(0x1000) == ScriptClass::Myanmar);
    CHECK(classifyScript(0x1F600) == ScriptClass::Emoji);
    CHECK(classifyScript(0x2500) == ScriptClass::Symbols);

    // Geometric Shapes and Supplemental Arrows-B stay OFF the symbol route, and
    // the reason is proportion rather than coverage. That route reaches
    // `symbol_fallback_font`, which ships as an icon face: its shapes and
    // curved arrows are drawn tighter and smaller than a text face's, so a
    // triangle beside a letter comes out visibly the wrong weight for the line
    // it sits in. Routed here once and reverted on sight.
    //
    // A reader who wants a particular face for these has the range editor,
    // which is the mechanism for exactly this and names a font per range.
    CHECK(classifyScript(0x25A0) != ScriptClass::Symbols); // ■ Geometric Shapes
    CHECK(classifyScript(0x25E4) != ScriptClass::Symbols); // ◤ its triangles
    CHECK(classifyScript(0x2934) != ScriptClass::Symbols); // ⤴ Supplemental Arrows-B
    CHECK(classifyScript(0x2935) != ScriptClass::Symbols); // ⤵
    CHECK(classifyScript(0x1D7D8) == ScriptClass::Math);
    CHECK(classifyScript('A') == ScriptClass::Latin);
    for (const auto codePoint : { 0xE000U, 0xF8FFU, 0xF0000U, 0xF0001U, 0xFFFFDU, 0x100000U, 0x10FFFDU }) {
        CHECK(classifyScript(codePoint) == ScriptClass::Symbols);
    }
    CHECK(classifyScript(0xFFFFE) == ScriptClass::Other);
    CHECK(classifyScript(0x10FFFE) == ScriptClass::Other);
    CHECK(classifyScript(0x10FFFF) == ScriptClass::Other);
}

TEST(testRouteTypes)
{
    std::uint32_t codePoints[] { 0x6771 };
    FontRequest request;
    request.codePoints = std::span<const std::uint32_t>(codePoints, 1);
    request.script = classifyScript(codePoints[0]);
    request.presentation = Presentation::Text;
    request.style = FontStyle::BoldItalic;
    request.width = CellWidthClass::Wide;
    request.preferenceGeneration = 42;

    CHECK(request.script == ScriptClass::Han);
    CHECK(request.width == CellWidthClass::Wide);
    CHECK(request.preferenceGeneration == 42);

    GlyphRoute route;
    route.source = FontSource::ExplicitHan;
    route.font = FontHandle { 7, 3 };
    route.glyph = GlyphID { 123 };
    route.flags = RouteFlag::WideAtlas | RouteFlag::ShapedRun;
    route.script = request.script;
    route.width = request.width;

    CHECK(route.font.isValid());
    CHECK(route.glyph.isValid());
    CHECK(!isMissing(route));
    CHECK(hasRouteFlag(route.flags, RouteFlag::WideAtlas));
    CHECK(hasRouteFlag(route.flags, RouteFlag::ShapedRun));
    CHECK(!hasRouteFlag(route.flags, RouteFlag::MonoAtlas));

    GlyphRoute missing;
    CHECK(isMissing(missing));
}

TEST(testShapingClassifiers)
{
    CHECK(scriptRequiresShapedRun(ScriptClass::Arabic));
    CHECK(scriptRequiresShapedRun(ScriptClass::Hebrew));
    CHECK(scriptRequiresShapedRun(ScriptClass::Devanagari));
    CHECK(scriptRequiresShapedRun(ScriptClass::Thai));
    CHECK(!scriptRequiresShapedRun(ScriptClass::Latin));
    CHECK(codePointRequiresShapedRun(0x0633));
    CHECK(codePointRequiresShapedRun(0x0915));
    CHECK(codePointRequiresShapedRun(0x1F600));
    CHECK(codePointRequiresShapedRun(0x200D));
    std::uint32_t family[] { 0x1F468, 0x200D, 0x1F469 };
    CHECK(clusterRequiresShapedRun(std::span<const std::uint32_t>(family, 3)));
}

// The Emoji script class is the same classification Core's Attribute::Emoji
// uses (the flat-table Emoji_Presentation bit, Unicode::Width::classify & 0x04) — not a
// separate hand-rolled range. A broad 0x2600-0x27BF sweep tags plain-text
// Dingbats as Emoji, routing them to the Apple Color Emoji bitmap cascade
// whenever the primary font lacks the glyph — rendered through the monochrome
// coverage atlas, their apparent size varies with whichever bitmap strike
// CoreText scales, and a spinner's glyphs change size from frame to frame.
TEST(testEmojiClassMatchesCoreEmojiPresentation)
{
    // The spinner's plain-text dingbats: not Emoji_Presentation=Yes, must not
    // classify as Emoji (they fall to Other and reach the outline-font
    // cascade, never the color-bitmap font).
    CHECK(classifyScript(0x2722) != ScriptClass::Emoji); // ✢
    CHECK(classifyScript(0x2733) != ScriptClass::Emoji); // ✳
    CHECK(classifyScript(0x2736) != ScriptClass::Emoji); // ✶
    CHECK(classifyScript(0x273B) != ScriptClass::Emoji); // ✻
    CHECK(classifyScript(0x273D) != ScriptClass::Emoji); // ✽
    // Text-default pictograph: emoji only with VS16, never bare.
    CHECK(classifyScript(0x26A0) != ScriptClass::Emoji); // ⚠
    // Default-emoji-presentation BMP codepoints: Emoji, even
    // the ones outside a hand-rolled range's reach.
    CHECK(classifyScript(0x2705) == ScriptClass::Emoji); // ✅
    CHECK(classifyScript(0x2B1C) == ScriptClass::Emoji); // ⬜ (Misc Symbols and Arrows)
    CHECK(classifyScript(0x231A) == ScriptClass::Emoji); // ⌚ (below 0x2600)
    CHECK(classifyScript(0x2B50) == ScriptClass::Emoji); // ⭐
    // VS16 stays the hardcoded selector special case.
    CHECK(isEmoji(0xFE0F));

    // The never-diverge-again pin: across the ranges where hand-rolled
    // ranges disagree with the property in both directions, the
    // Emoji script class equals the Emoji_Presentation SSOT bit exactly.
    for (std::uint32_t codePoint = 0x2300; codePoint <= 0x2BFF; codePoint++) {
        const bool emojiClass = classifyScript(codePoint) == ScriptClass::Emoji;
        const bool isEmojiInTable = Ubiquitty::Unicode::Width::isEmojiPresentation(codePoint);
        if (emojiClass != isEmojiInTable) {
            printf("FAIL divergence at U+%04X: class=%d table=%d\n", codePoint, emojiClass, isEmojiInTable);
            ++failures;
        }
    }
    for (std::uint32_t codePoint = 0x1F000; codePoint <= 0x1FAFF; codePoint++) {
        const bool emojiClass = classifyScript(codePoint) == ScriptClass::Emoji;
        const bool isEmojiInTable = Ubiquitty::Unicode::Width::isEmojiPresentation(codePoint);
        if (emojiClass != isEmojiInTable) {
            printf("FAIL divergence at U+%04X: class=%d table=%d\n", codePoint, emojiClass, isEmojiInTable);
            ++failures;
        }
    }
}

// The inverse of the weight-trait map, for a picker built from a static
// family's members: each published stop's trait routes back to its own
// setting, a trait between stops to the nearer one, and a trait past the
// ends to the end stop — so a member's row names the weight the route lands
// on for it, and nothing else.
TEST(settingForNormalisedWeightTraitInvertsTheStops)
{
    const int stops[] = { 100, 200, 300, 400, 500, 600, 700, 800, 900 };
    for (const int stop : stops)
        CHECK_EQ(settingForNormalisedWeightTrait(normalisedWeightTraitForSetting(stop)), stop);
    // JetBrains Mono's static Thin reports -0.6, the 200 stop: the setting
    // that selects it is 200, whatever its OS/2 class says.
    CHECK_EQ(settingForNormalisedWeightTrait(-0.6f), 200);
    CHECK_EQ(settingForNormalisedWeightTrait(0.25f), 500);
    CHECK_EQ(settingForNormalisedWeightTrait(0.35f), 600);
    CHECK_EQ(settingForNormalisedWeightTrait(-0.95f), 100);
    CHECK_EQ(settingForNormalisedWeightTrait(0.9f), 900);
}

// The weight every face draws at, from the three settings, in one table. Every
// arm places this number and none asks its platform for one, so a row here is
// what the macOS, Linux and web arms each draw.
TEST(testWeightForFaceResolvesEverySettingToOneNumber)
{
    using Ubiquitty::Rendering::FontFace;
    const WeightSettings unset {};
    // Nothing set: the upright weight and the face's own bold, never zero.
    CHECK_EQ(weightForFace(FontFace::Regular, 0, unset), uprightWeight);
    CHECK_EQ(weightForFace(FontFace::Italic, 0, unset), uprightWeight);
    CHECK_EQ(weightForFace(FontFace::Bold, 0, unset), faceOwnBoldWeight);
    CHECK_EQ(weightForFace(FontFace::BoldItalic, 0, unset), faceOwnBoldWeight);
    CHECK_EQ(uprightWeight, 400);
    CHECK_EQ(faceOwnBoldWeight, 700);

    // A light upright leaves bold where the family puts it: the weight that
    // makes bold bold is `font.bold_weight`, not a step from the upright.
    const WeightSettings light { .upright = 200 };
    CHECK_EQ(weightForFace(FontFace::Regular, 0, light), 200);
    CHECK_EQ(weightForFace(FontFace::Bold, 0, light), 700);
    CHECK(weightForFace(FontFace::Bold, 0, light) > weightForFace(FontFace::Regular, 0, light));

    // A heavy upright never draws its bold lighter than itself: with no bold
    // weight set, bold is the face's own or the upright, whichever is heavier.
    const WeightSettings heavy { .upright = 750 };
    CHECK_EQ(weightForFace(FontFace::Regular, 0, heavy), 750);
    CHECK_EQ(weightForFace(FontFace::Bold, 0, heavy), 750);
    CHECK_EQ(weightForFace(FontFace::BoldItalic, 0, heavy), 750);
    CHECK_EQ(weightForFace(FontFace::Bold, 0, { .upright = 1200 }), 900);
    // A bold weight the reader set is drawn as written, below the upright too.
    CHECK_EQ(weightForFace(FontFace::Bold, 0, { .upright = 800, .bold = 400 }), 400);

    // Each setting reaches its own faces only.
    const WeightSettings both { .upright = 350, .bold = 600 };
    CHECK_EQ(weightForFace(FontFace::Regular, 0, both), 350);
    CHECK_EQ(weightForFace(FontFace::Italic, 0, both), 350);
    CHECK_EQ(weightForFace(FontFace::Bold, 0, both), 600);
    CHECK_EQ(weightForFace(FontFace::BoldItalic, 0, both), 600);

    // Weights outside the axis clamp rather than being dropped.
    CHECK_EQ(weightForFace(FontFace::Regular, 0, { .upright = 1200 }), 900);
    CHECK_EQ(weightForFace(FontFace::Regular, 0, { .upright = 30 }), 100);

    // An SGR 1:n weight is the running program naming one run and outranks
    // every setting and the face, at 100 + 100n.
    CHECK_EQ(weightForFace(FontFace::Regular, 1, both), 200);
    CHECK_EQ(weightForFace(FontFace::Bold, 2, both), 300);
    CHECK_EQ(weightForFace(FontFace::Regular, 7, light), 800);
}

TEST(testBodyVariationSetsTheDefaultBoldFloor)
{
    using Ubiquitty::Rendering::FontFace;
    struct Case {
        WeightSettings settings;
        double upright;
        double bold;
    };
    for (const auto& test : {
             Case { { .uprightVariation = 800 }, 800, 800 },
             Case { { .upright = 900, .uprightVariation = 250 }, 250, 700 },
             Case { { .upright = 900, .uprightVariation = 800 }, 800, 800 },
             Case { { .uprightVariation = 800.5 }, 800.5, 800.5 },
             Case { { .uprightVariation = 950 }, 950, 950 },
             Case { { .uprightVariation = 0 }, 0, 700 },
             Case { { .bold = 600, .uprightVariation = 800 }, 800, 600 },
         }) {
        CHECK_EQ(weightForFace(FontFace::Regular, 0, test.settings), test.upright);
        CHECK_EQ(weightForFace(FontFace::Italic, 0, test.settings), test.upright);
        CHECK_EQ(weightForFace(FontFace::Bold, 0, test.settings), test.bold);
        CHECK_EQ(weightForFace(FontFace::BoldItalic, 0, test.settings), test.bold);
        for (const auto face : { FontFace::Regular, FontFace::Italic, FontFace::Bold, FontFace::BoldItalic }) {
            CHECK_EQ(weightForFace(face, 2, test.settings), 300);
        }
    }
}

// The width every face draws at: the setting, or normal when it is unset.
TEST(testWidthForSettingIsNormalWhenUnset)
{
    CHECK(widthForSetting(0.0) == 100.0);
    CHECK(widthForSetting(-5.0) == 100.0);
    CHECK(widthForSetting(112.5) == 112.5);
    CHECK(widthForSetting(75.0) == 75.0);
}

UBIQUITTY_TEST_MAIN()
