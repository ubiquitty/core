// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_color_resolve — pins the portable Pen+palette resolver
// (Source/Core/Rendering/ColorResolve.cpp) against the seeded default TermPalette:
// default / indexed / true-color sides, bold-as-bright gating, reverse (+ the
// reverseAlso XOR), hidden, dim ordering, underline color, and the focused
// block-cursor cross-fade bake.

#include "Core/Pen.h"
#include "Core/Rendering/ColorResolve.h"

#include "Core/TermPalette.h"
#include "Core/TerminalOperations.h"

#include <cmath>
#include <cstdio>
#include <cstring>

using namespace Ubiquitty;
using namespace Ubiquitty::Rendering;

static Ubiquitty::Pen makePen(Ubiquitty::VT::AttributeSet attributes, uint16_t foregroundIndex, uint16_t backgroundIndex)
{
    Ubiquitty::Pen p {};
    p.attributes = attributes;
    p.foregroundIndex = foregroundIndex;
    p.backgroundIndex = backgroundIndex;
    p.underlineIndex = colorDefault;
    return p;
}

static bool eq(RGB8 c, int r, int g, int b) { return c.red == r && c.green == g && c.blue == b; }
static bool approx(float a, float b) { return std::fabs(a - b) < 1e-5f; }

TEST(colorResolve)
{

    TermPalette pal;
    pal.seedDefaults(); // foreground C0C0C0, background 101014; ansi[1]=205,0,0; ansi[9]=255,0,0

    // Default foreground/background.
    {
        auto r = resolveCell(makePen({}, colorDefault, colorDefault), pal, false, false);
        CHECK(eq(r.foreground, 0xC0, 0xC0, 0xC0));
        CHECK(eq(r.background, 0x10, 0x10, 0x14));
    }

    // Indexed foreground (ANSI 1 = red 205,0,0).
    CHECK(eq(resolveCell(makePen({}, 1, colorDefault), pal, false, false).foreground, 205, 0, 0));

    // resolvePaletteColor (chrome / fills): default foreground vs background, and an index.
    CHECK(eq(resolvePaletteColor(pal, colorDefault, true), 0xC0, 0xC0, 0xC0));
    CHECK(eq(resolvePaletteColor(pal, colorDefault, false), 0x10, 0x10, 0x14));
    CHECK(eq(resolvePaletteColor(pal, 1, false), 205, 0, 0));

    // True-color (colorRGB) reads the pen's direct channels.
    {
        Ubiquitty::Pen p = makePen({}, colorRGB, colorDefault);
        p.foregroundRed = 10;
        p.foregroundGreen = 20;
        p.foregroundBlue = 30;
        CHECK(eq(resolveCell(p, pal, false, false).foreground, 10, 20, 30));
    }

    // Bold-as-bright: ANSI 1 -> 9 (255,0,0), only when brightenBold; and never
    // for non-0..7 indices.
    CHECK(eq(resolveCell(makePen(Ubiquitty::VT::Attribute::Bold, 1, colorDefault), pal, false, false).foreground, 205, 0, 0));
    CHECK(eq(resolveCell(makePen(Ubiquitty::VT::Attribute::Bold, 1, colorDefault), pal, false, true).foreground, 255, 0, 0));
    CHECK(eq(resolveCell(makePen(Ubiquitty::VT::Attribute::Bold, colorDefault, colorDefault), pal, false, true).foreground, 0xC0, 0xC0, 0xC0));

    // Reverse: swaps foreground/background; reverseAlso XORs it.
    {
        auto r = resolveCell(makePen(Ubiquitty::VT::Attribute::Reverse, 1, colorDefault), pal, false, false);
        CHECK(eq(r.foreground, 0x10, 0x10, 0x14));
        CHECK(eq(r.background, 205, 0, 0));
        // Attribute::Reverse + reverseAlso=true -> double swap -> no swap.
        CHECK(eq(resolveCell(makePen(Ubiquitty::VT::Attribute::Reverse, 1, colorDefault), pal, true, false).foreground, 205, 0, 0));
        // reverseAlso alone swaps.
        CHECK(eq(resolveCell(makePen({}, 1, colorDefault), pal, true, false).foreground, 0x10, 0x10, 0x14));
    }

    // Hidden collapses foreground onto background.
    {
        auto r = resolveCell(makePen(Ubiquitty::VT::Attribute::Hidden, 1, colorDefault), pal, false, false);
        CHECK(eq(r.foreground, 0x10, 0x10, 0x14));
        CHECK(eq(r.foreground, r.background.red, r.background.green, r.background.blue));
    }

    // Dim averages foreground toward background (integer), applied last. foreground=15 (255,255,255).
    {
        auto r = resolveCell(makePen(Ubiquitty::VT::Attribute::Dim, 15, colorDefault), pal, false, false);
        CHECK(eq(r.foreground, (255 + 0x10) / 2, (255 + 0x10) / 2, (255 + 0x14) / 2));
    }

    // Underline color.
    {
        Ubiquitty::Pen p = makePen({}, 2, colorDefault); // foreground green ANSI 2 = 0,205,0
        RGB8 foreground = resolveCell(p, pal, false, false).foreground;
        CHECK(eq(resolveUnderlineColor(p, pal, foreground), 0, 205, 0)); // default -> foreground
        p.underlineIndex = colorRGB;
        p.underlineRed = 1;
        p.underlineGreen = 2;
        p.underlineBlue = 3;
        CHECK(eq(resolveUnderlineColor(p, pal, foreground), 1, 2, 3)); // rgb
        p.underlineIndex = 4; // ANSI 4 = 0,0,238
        CHECK(eq(resolveUnderlineColor(p, pal, foreground), 0, 0, 238)); // indexed
    }

    // Underline opacity, which follows the colour the rule NAMES rather than the
    // text's. The two coincide at an opaque palette, which is every palette the
    // protocol can produce — the alpha exists only in a theme — so a resolver
    // reading the wrong slot agrees everywhere until someone edits one entry.
    {
        std::uint8_t paletteAlpha[256];
        for (int index = 0; index < 256; ++index)
            paletteAlpha[index] = 255;
        paletteAlpha[4] = 64; // the slot SGR 58 will name
        Ubiquitty::Pen p = makePen({}, 2, colorDefault);

        // colorDefault tracks the text, opacity included, so a translucent
        // foreground carries through and the named-slot alpha is not consulted.
        CHECK(approx(resolveUnderlineAlpha(p, paletteAlpha, 0.5f, 1.0f), 0.5f));

        // A direct colour has no alpha in the protocol: opaque, and still blinking.
        p.underlineIndex = colorRGB;
        CHECK(approx(resolveUnderlineAlpha(p, paletteAlpha, 0.5f, 1.0f), 1.0f));
        CHECK(approx(resolveUnderlineAlpha(p, paletteAlpha, 0.5f, 0.25f), 0.25f));

        // An indexed colour takes its slot's opacity — not slot 2's, which is
        // what the text resolved from.
        p.underlineIndex = 4;
        CHECK(approx(resolveUnderlineAlpha(p, paletteAlpha, 1.0f, 1.0f), 64 / 255.0f));
        CHECK(approx(resolveUnderlineAlpha(p, paletteAlpha, 1.0f, 0.5f), (64 / 255.0f) * 0.5f));
        // No table is opaque throughout, which is what a caller with no theme
        // alpha means. Answering with the foreground's alpha instead would make
        // an SGR 58 rule fade with text it is not painted in.
        CHECK(approx(resolveUnderlineAlpha(p, nullptr, 0.5f, 1.0f), 1.0f));

        // Out of range falls back to the foreground, as the colour does.
        p.underlineIndex = 300;
        CHECK(approx(resolveUnderlineAlpha(p, paletteAlpha, 0.5f, 1.0f), 0.5f));
    }

    // Block-cursor bake (white cursor).
    {
        const Vec4 cellForeground { 1, 0, 0, 1 };
        const Vec4 cellBackground { 0, 0, 0.5f, 1 };
        const Vec4 cursor { 1, 1, 1, 1 };
        // mixA 0 -> untouched.
        auto z = bakeBlockCursor(cellForeground, cellBackground, cursor, 0.0f, { CursorTextInk::Source::Inverse });
        CHECK(approx(z.background.x, 0) && approx(z.background.z, 0.5f) && approx(z.foreground.x, 1));
        // Full bake, no invert -> background becomes the cursor color, foreground unchanged.
        auto f = bakeBlockCursor(cellForeground, cellBackground, cursor, 1.0f, {});
        CHECK(approx(f.background.x, 1) && approx(f.background.y, 1) && approx(f.background.z, 1));
        CHECK(approx(f.foreground.x, 1) && approx(f.foreground.y, 0));
        // Full bake, invert: white cursor -> luma 1 -> inverse 0 (black glyph).
        auto inv = bakeBlockCursor(cellForeground, cellBackground, cursor, 1.0f, { CursorTextInk::Source::Inverse });
        CHECK(approx(inv.foreground.x, 0) && approx(inv.foreground.y, 0) && approx(inv.foreground.z, 0) && approx(inv.foreground.w, 1));
        // A named cursor text color inks the glyph in it, crossfaded by mixA
        // as the inverse is, whatever the cursor's own color.
        const CursorTextInk named { CursorTextInk::Source::Color, { 0.2f, 0.4f, 0.6f, 1 } };
        auto full = bakeBlockCursor(cellForeground, cellBackground, cursor, 1.0f, named);
        CHECK(approx(full.foreground.x, 0.2f) && approx(full.foreground.y, 0.4f) && approx(full.foreground.z, 0.6f) && approx(full.foreground.w, 1));
        auto half = bakeBlockCursor(cellForeground, cellBackground, cursor, 0.5f, named);
        CHECK(approx(half.foreground.x, 0.6f) && approx(half.foreground.y, 0.2f) && approx(half.foreground.z, 0.3f));
    }
}

// The fields resolveCell leaves unread, asserted one at a time.
//
// `buildCellInstances` memoises the resolve (`memoPen`) behind a memcmp over
// all 20 bytes of Pen, so a cell differing only in a field the resolver never
// reads still forces a re-resolve for a byte-identical answer. Narrowing that
// key to the consulted fields is only sound while this list is exhaustive, and
// the list is a claim about the FUNCTION rather than about the memo.
//
// So each case varies exactly one dropped field between two otherwise identical
// pens and requires the two ResolvedCells to match. It fails the day the
// resolver starts reading one of them — which is the only ordering that makes
// it a control rather than a description of whatever the code does.
//
// Both bool arguments are varied across every case, because they compose with
// the pen's own state: brightenBold gates a remap and reverseAlso XORs with
// Attribute::Reverse, so a field consulted only on one of those paths would
// otherwise sit unexercised.
TEST(colorResolveIgnoresUnconsultedFields)
{
    TermPalette palette;
    palette.seedDefaults();
    const Palette8 narrow = narrowPalette(palette);

    // Indexed and RGB-direct both, since the resolver takes a different branch
    // per side and a dropped field could be read on only one of them.
    const uint16_t foregrounds[] = { colorDefault, 3, colorRGB };
    const uint16_t backgrounds[] = { colorDefault, 5, colorRGB };

    auto basePen = [&](uint16_t foreground, uint16_t background) {
        Ubiquitty::Pen p {};
        p.foregroundIndex = foreground;
        p.backgroundIndex = background;
        p.underlineIndex = colorDefault;
        p.foregroundRed = 11;
        p.foregroundGreen = 22;
        p.foregroundBlue = 33;
        p.backgroundRed = 44;
        p.backgroundGreen = 55;
        p.backgroundBlue = 66;
        return p;
    };

    // Applies `mutate` to a copy and requires the resolve to be unchanged, over
    // every colour-side and bool-argument combination.
    auto ignores = [&](const char* what, auto mutate) {
        for (uint16_t foreground : foregrounds) {
            for (uint16_t background : backgrounds) {
                for (int reverseAlso = 0; reverseAlso < 2; ++reverseAlso) {
                    for (int brightenBold = 0; brightenBold < 2; ++brightenBold) {
                        Ubiquitty::Pen plain = basePen(foreground, background);
                        Ubiquitty::Pen varied = plain;
                        mutate(varied);
                        // A mutation that changed nothing would make the case
                        // pass by comparing a pen with itself.
                        CHECK(std::memcmp(&plain, &varied, sizeof(Ubiquitty::Pen)) != 0);
                        const ResolvedCell a = resolveCell(plain, narrow, reverseAlso != 0, brightenBold != 0);
                        const ResolvedCell b = resolveCell(varied, narrow, reverseAlso != 0, brightenBold != 0);
                        const bool same = eq(a.foreground, b.foreground.red, b.foreground.green, b.foreground.blue)
                            && eq(a.background, b.background.red, b.background.green, b.background.blue);
                        if (!same)
                            std::printf("  %s changed the resolve (fg %u bg %u, reverseAlso %d, brightenBold %d)\n",
                                what, foreground, background, reverseAlso, brightenBold);
                        CHECK(same);
                    }
                }
            }
        }
    };

    // The underline colour, in both of its spellings.
    ignores("underlineIndex", [](Ubiquitty::Pen& p) { p.underlineIndex = 9; });
    ignores("underlineIndex=colorRGB", [](Ubiquitty::Pen& p) { p.underlineIndex = colorRGB; });
    ignores("underline RGB", [](Ubiquitty::Pen& p) {
        p.underlineRed = 200;
        p.underlineGreen = 100;
        p.underlineBlue = 50;
    });

    // The three packed bytes.
    ignores("underlineStyle", [](Ubiquitty::Pen& p) { p.underlineStyle = 0xFF; });
    ignores("font", [](Ubiquitty::Pen& p) { p.font = 0xE0; });
    ignores("extendedAttributes", [](Ubiquitty::Pen& p) {
        p.extendedAttributes.flags.add(Ubiquitty::VT::ExtendedAttribute::BlinkFast);
    });

    // The twelve Attribute bits the resolver never reads. Bold, Reverse, Hidden
    // and Dim are absent on purpose: those four are consulted, and a case
    // asserting they are ignored would be asserting the resolver is broken.
    const Ubiquitty::VT::Attribute unconsulted[] = {
        Ubiquitty::VT::Attribute::Blink,
        Ubiquitty::VT::Attribute::Emoji,
        Ubiquitty::VT::Attribute::Encircled,
        Ubiquitty::VT::Attribute::ForceEmojiPresentation,
        Ubiquitty::VT::Attribute::Framed,
        Ubiquitty::VT::Attribute::Italic,
        Ubiquitty::VT::Attribute::Overline,
        Ubiquitty::VT::Attribute::Protected,
        Ubiquitty::VT::Attribute::Strike,
        Ubiquitty::VT::Attribute::Underline,
        Ubiquitty::VT::Attribute::Wide,
        Ubiquitty::VT::Attribute::WideContinuation,
    };
    for (Ubiquitty::VT::Attribute attribute : unconsulted)
        ignores("an attribute bit", [attribute](Ubiquitty::Pen& p) { p.attributes.add(attribute); });

    // The consulted four, from the other side: at least one of them must MOVE
    // the resolve, or the loop above is passing because resolveCell ignores its
    // pen entirely and every case compares two identical answers.
    {
        Ubiquitty::Pen plain = basePen(3, colorDefault);
        Ubiquitty::Pen bold = plain;
        bold.attributes.add(Ubiquitty::VT::Attribute::Bold);
        const ResolvedCell a = resolveCell(plain, narrow, false, /*brightenBold=*/true);
        const ResolvedCell b = resolveCell(bold, narrow, false, /*brightenBold=*/true);
        CHECK(!eq(a.foreground, b.foreground.red, b.foreground.green, b.foreground.blue));
    }
}

TEST(textContrastPreservesIntentAndStrengthensDimColors)
{
    const Pen normal = makePen({}, colorDefault, colorDefault);
    const Pen dim = makePen(VT::Attribute::Dim, colorDefault, colorDefault);
    const Pen hidden = makePen(VT::Attribute::Hidden, colorDefault, colorDefault);
    const ResolvedCell low { { 80, 80, 80 }, { 70, 70, 70 } };
    const auto strong = enhanceTextContrast(low, normal);
    const auto subdued = enhanceTextContrast(low, dim);
    CHECK(strong.foreground.red > low.foreground.red);
    CHECK(subdued.foreground.red > low.foreground.red);
    CHECK(subdued.foreground.red < strong.foreground.red);
    CHECK(eq(strong.background, 70, 70, 70));
    CHECK(eq(enhanceTextContrast(low, hidden).foreground, 80, 80, 80));
    CHECK(eq(enhanceTextContrast({ { 70, 70, 70 }, { 70, 70, 70 } }, normal).foreground, 70, 70, 70));
    CHECK(eq(enhanceTextContrast({ { 255, 255, 255 }, { 0, 0, 0 } }, normal).foreground, 255, 255, 255));
    const auto light = enhanceTextContrast({ { 210, 210, 210 }, { 230, 230, 230 } }, normal);
    CHECK(light.foreground.red < 210);
    CHECK(eq(light.background, 230, 230, 230));
    const auto orange = enhanceTextContrast({ { 80, 40, 10 }, { 20, 20, 20 } }, normal);
    CHECK(orange.foreground.red > 80);
    CHECK(orange.foreground.red > orange.foreground.green);
    CHECK(orange.foreground.green > orange.foreground.blue);
    const double originalHue = 30.0 / 70.0;
    const double adjustedHue = static_cast<double>(orange.foreground.green - orange.foreground.blue)
        / (orange.foreground.red - orange.foreground.blue);
    CHECK(std::abs(originalHue - adjustedHue) < 0.02);
    CHECK(eq(enhanceTextContrast({ { 0, 0, 0 }, { 255, 255, 255 } }, normal).foreground, 0, 0, 0));
}

TEST(blockCursorOpacityIsIndependentOfWindowOpacity)
{
    for (float opacity : { 0.0f, 0.2f, 1.0f }) {
        const Vec4 foreground { 1, 1, 1, 1 };
        const Vec4 background { 0, 0, 0, opacity };
        const Vec4 cursor { 0, 0.5f, 1, 1 };
        CHECK(bakeBlockCursor(foreground, background, cursor, 1, {}).background.w == 1);
        CHECK(bakeBlockCursor(foreground, background, cursor, 0, {}).background.w == opacity);
        CHECK(std::abs(bakeBlockCursor(foreground, background, cursor, 0.5f, {}).background.w - (opacity + 1) / 2) < 0.0001f);
    }
}

TEST(blockCursorFadesCoverageWithoutTintingTowardTransparentGround)
{
    const Vec4 foreground { 1, 1, 1, 1 };
    const Vec4 background { 0.1f, 0.2f, 0.4f, 0 };
    for (const Vec4 cursor : { Vec4 { 1, 1, 1, 1 }, Vec4 { 0, 0.5f, 1, 1 } }) {
        for (float coverage : { 0.1f, 0.25f, 0.5f, 0.75f, 1.0f }) {
            const auto faded = bakeBlockCursor(foreground, background, cursor, coverage, {}).background;
            CHECK(approx(faded.x, cursor.x));
            CHECK(approx(faded.y, cursor.y));
            CHECK(approx(faded.z, cursor.z));
            CHECK(approx(faded.w, coverage));
        }
    }
    const Vec4 tintedGround { 0.1f, 0.2f, 0.4f, 0.3f };
    const Vec4 cursor { 1, 1, 1, 1 };
    const auto faded = bakeBlockCursor(foreground, tintedGround, cursor, 0.5f, {}).background;
    CHECK(approx(faded.x * faded.w, 0.5f + tintedGround.x * 0.15f));
    CHECK(approx(faded.y * faded.w, 0.5f + tintedGround.y * 0.15f));
    CHECK(approx(faded.z * faded.w, 0.5f + tintedGround.z * 0.15f));
}

TEST(secondaryTextVibrancyPreservesPrimaryAndChromaticInk)
{
    const Vec4 ground { 0.1f, 0.1f, 0.1f, 0.2f };
    const Vec4 primary { 0.9f, 0.9f, 0.9f, 1 };
    CHECK(approx(secondaryTextCoverage({ 0.5f, 0.5f, 0.5f, 1 }, ground, primary, ground), 0.5f / 0.9f));
    CHECK(approx(secondaryTextCoverage({ 0.3f, 0.3f, 0.3f, 1 }, ground, primary, ground), 0.3f / 0.9f));
    CHECK(secondaryTextCoverage(primary, ground, primary, ground) == 0);
    CHECK(secondaryTextCoverage({ 0, 0, 0, 1 }, ground, primary, ground) > 0);
    CHECK(secondaryTextCoverage({ 0, 0.8f, 0.8f, 1 }, ground, primary, ground) == 0);
    CHECK(secondaryTextCoverage({ 0.5f, 0.5f, 0.5f, 0 }, ground, primary, ground) == 0);
    CHECK(secondaryTextCoverage({ 0.5f, 0.5f, 0.5f, 1 }, { 0, 0.2f, 0.6f, 1 }, primary, ground) == 0);
    CHECK(approx(secondaryTextCoverage({ 0.5f, 0.5f, 0.5f, 1 }, { 1, 1, 1, 0 }, { 0, 0, 0, 1 }, { 1, 1, 1, 0 }), 0.5f));
}

TEST(secondaryTextVibrancyPreservesEveryGrayFadeStep)
{
    for (const bool isDark : { false, true }) {
        const Vec4 ground = isDark ? Vec4 { 0, 0, 0, 0 } : Vec4 { 1, 1, 1, 0 };
        const Vec4 primary = isDark ? Vec4 { 1, 1, 1, 1 } : Vec4 { 0, 0, 0, 1 };
        float previous = 0;
        for (int step = 1; step < 255; ++step) {
            const float gray = (isDark ? step : 255 - step) / 255.0f;
            const float coverage = secondaryTextCoverage({ gray, gray, gray, 1 }, ground, primary, ground);
            CHECK(coverage > previous);
            CHECK(coverage - previous < 0.004f);
            CHECK(approx(secondaryTextCoverage({ gray, gray, gray, 0.5f }, ground, primary, ground), coverage * 0.5f));
            previous = coverage;
        }
    }
}

TEST(secondaryTextVibrancySupportsShadedInputsAndPreservesReverseContrast)
{
    const Vec4 dark { 0.1f, 0.1f, 0.1f, 0 };
    const Vec4 light { 0.9f, 0.9f, 0.9f, 1 };
    const Vec4 shaded { 0.2f, 0.2f, 0.2f, 0.15f };
    CHECK(secondaryTextCoverage({ 0.5f, 0.5f, 0.5f, 1 }, shaded, light, dark) > 0);
    CHECK(secondaryTextCoverage({ 0, 0, 0, 1 }, { 0.8f, 0.8f, 0.8f, 0.7f }, light, dark) == 0);
    CHECK(secondaryTextCoverage({ 0.5f, 0.5f, 0.5f, 1 }, { 0.8f, 0.8f, 0.8f, 0.15f }, dark, light) > 0);
    CHECK(secondaryTextCoverage({ 1, 1, 1, 1 }, shaded, dark, light) == 0);
}

UBIQUITTY_TEST_MAIN()
