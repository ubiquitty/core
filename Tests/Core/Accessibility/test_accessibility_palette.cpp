// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_accessibility_palette.cpp — pins the portable accessibility palette
// transforms (Source/Core/Accessibility/Palette.cpp): the CVD-safe hue remap
// and the high-contrast foreground/background separation boost.
//
// The transforms are the rendering-path-independent half of the macOS
// accessibility cluster; the renderer just calls them on a freshly theme-applied
// CellPalette before fillExtended(). These checks pin the invariants the visual
// effect rests on so a silent regression shows here, not as a wrong-colored
// screen on hardware.

#include "Core/Accessibility/Palette.h"
#include "Core/CellColor.h"
#include "Core/Preferences/Preferences.h"

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>

using Ubiquitty::Accessibility::ColorblindMode;

static double luma(const Ubiquitty::ColorRGBA& c) { return 0.2126 * c.red + 0.7152 * c.green + 0.0722 * c.blue; }

static bool sameColor(const Ubiquitty::ColorRGBA& a, const Ubiquitty::ColorRGBA& b)
{
    return a.red == b.red && a.green == b.green && a.blue == b.blue && a.alpha == b.alpha;
}

// A neutral-dark seeded palette stands in for any applied theme.
static Ubiquitty::CellPalette darkPalette()
{
    Ubiquitty::CellPalette p;
    p.seedDefaults();
    return p;
}

// A light-background palette: the contrast boost picks its pole from the
// background's luma, so every arm below the choice has two shapes and a
// dark-only fixture exercises one of them.
static Ubiquitty::CellPalette lightPalette()
{
    Ubiquitty::CellPalette p;
    p.seedDefaults();
    p.defaultBackground = { 250, 250, 250, 255 };
    p.defaultForeground = { 20, 20, 20, 255 };
    return p;
}

// None is a strict no-op on every channel of every slot.
TEST(testNoneIsNoop)
{
    Ubiquitty::CellPalette before = darkPalette();
    Ubiquitty::CellPalette after = before;
    Ubiquitty::Accessibility::applyColorblindRemap(after, ColorblindMode::None);
    for (int i = 0; i < 256; ++i) {
        CHECK(after.indexed[i].red == before.indexed[i].red);
        CHECK(after.indexed[i].green == before.indexed[i].green);
        CHECK(after.indexed[i].blue == before.indexed[i].blue);
    }
    CHECK(after.defaultForeground.red == before.defaultForeground.red);
    CHECK(after.defaultBackground.blue == before.defaultBackground.blue);
}

// Deuteranopia/protanopia must separate the red↔green pair: after the remap the
// red slot (1) and green slot (2) differ on the blue-yellow axis they share, and
// red is an orange-shifted hue (it gains green to become orange).
TEST(testRedGreenRemapSeparates)
{
    Ubiquitty::CellPalette p = darkPalette();
    const Ubiquitty::ColorRGBA origRed = p.indexed[1];
    const Ubiquitty::ColorRGBA origGreen = p.indexed[2];
    Ubiquitty::Accessibility::applyColorblindRemap(p, ColorblindMode::Deuteranopia);

    // Red role gained a green component (pure red → orange).
    CHECK(p.indexed[1].green > origRed.green);
    // Green role gained a blue component (pure green → blue-green).
    CHECK(p.indexed[2].blue > origGreen.blue);
    // The two slots are now distinguishable by more than the failing red-green
    // axis alone: their blue channels differ.
    CHECK(p.indexed[1].blue != p.indexed[2].blue || p.indexed[1].green != p.indexed[2].green);

    // Protanopia takes the same red-green branch.
    Ubiquitty::CellPalette q = darkPalette();
    Ubiquitty::Accessibility::applyColorblindRemap(q, ColorblindMode::Protanopia);
    CHECK(q.indexed[1].red == p.indexed[1].red);
    CHECK(q.indexed[2].green == p.indexed[2].green);
}

// Tritanopia must separate the blue↔yellow pair: blue (4) and yellow (3) move
// onto the surviving red-green axis.
TEST(testBlueYellowRemapSeparates)
{
    Ubiquitty::CellPalette p = darkPalette();
    const Ubiquitty::ColorRGBA origYellow = p.indexed[3];
    Ubiquitty::Accessibility::applyColorblindRemap(p, ColorblindMode::Tritanopia);
    // Yellow role gained blue (yellow → reddish purple).
    CHECK(p.indexed[3].blue > origYellow.blue);
    // Blue role stays blue-dominant but distinct from yellow's new tone.
    CHECK(p.indexed[4].blue >= p.indexed[4].red);
}

// The remap re-tones replacements toward the slot's original luminance, so a
// dim theme color stays dim and a bright one stays bright (within tolerance).
TEST(testRemapPreservesLuminanceFeel)
{
    Ubiquitty::CellPalette p = darkPalette();
    const double redLumaBefore = luma(p.indexed[1]);
    const double brightRedLumaBefore = luma(p.indexed[9]);
    Ubiquitty::Accessibility::applyColorblindRemap(p, ColorblindMode::Deuteranopia);
    // Bright red must remain brighter than normal red after substitution.
    CHECK(luma(p.indexed[9]) > luma(p.indexed[1]));
    // Each slot's luminance stays in the neighborhood of its original (the
    // re-tone matches luma exactly up to rounding/gridFloor).
    CHECK(std::fabs(luma(p.indexed[1]) - redLumaBefore) < 24.0);
    CHECK(std::fabs(luma(p.indexed[9]) - brightRedLumaBefore) < 24.0);
}

// Contrast boost on a dark background widens the foreground↔background luminance
// gap and never panics on the alpha channel.
TEST(testContrastBoostWidensGapDark)
{
    Ubiquitty::CellPalette p = darkPalette(); // dark bg, light fg
    const double gapBefore = luma(p.defaultForeground) - luma(p.defaultBackground);
    const std::uint8_t foregroundAlpha = p.defaultForeground.alpha;
    Ubiquitty::Accessibility::applyContrastBoost(p, 1.0);
    const double gapAfter = luma(p.defaultForeground) - luma(p.defaultBackground);
    CHECK(gapAfter >= gapBefore);
    CHECK(p.defaultForeground.alpha == foregroundAlpha); // alpha untouched
}

// strength 0 is a no-op.
TEST(testContrastBoostZeroIsNoop)
{
    Ubiquitty::CellPalette before = darkPalette();
    Ubiquitty::CellPalette after = before;
    Ubiquitty::Accessibility::applyContrastBoost(after, 0.0);
    CHECK(after.defaultForeground.red == before.defaultForeground.red);
    CHECK(after.indexed[1].green == before.indexed[1].green);
}

TEST(testContrastBoostWidensGapLight)
{
    // The mirror of the dark case. On a light background the swatches are pushed
    // toward black and the foreground and background separate the other way, so
    // a fixture that only ever seeds a dark theme leaves the whole light half of
    // this function unrun.
    Ubiquitty::CellPalette p = lightPalette();
    const double backgroundBefore = luma(p.defaultBackground);
    const double gapBefore = luma(p.defaultBackground) - luma(p.defaultForeground);
    Ubiquitty::Accessibility::applyContrastBoost(p, 1.0);
    const double gapAfter = luma(p.defaultBackground) - luma(p.defaultForeground);

    CHECK(gapAfter >= gapBefore);
    // The background goes toward white and the foreground toward black, which is
    // the opposite of the dark case and what says the pole was chosen from the
    // background rather than fixed.
    CHECK(luma(p.defaultBackground) >= backgroundBefore);
    CHECK(luma(p.defaultForeground) <= 20.0 * 0.2126 + 20.0 * 0.7152 + 20.0 * 0.0722 + 0.5);
}

TEST(testProtanopiaTakesTheRedGreenRemap)
{
    // Deuteranopia and Protanopia share one arm of the switch, and a case that
    // only ever passes Deuteranopia leaves the Protanopia label unexecuted. The
    // two must agree exactly — a divergence would mean the shared arm stopped
    // being shared.
    Ubiquitty::CellPalette viaDeuteranopia = darkPalette();
    Ubiquitty::CellPalette viaProtanopia = darkPalette();
    Ubiquitty::Accessibility::applyColorblindRemap(viaDeuteranopia, ColorblindMode::Deuteranopia);
    Ubiquitty::Accessibility::applyColorblindRemap(viaProtanopia, ColorblindMode::Protanopia);

    for (int i = 0; i < 16; ++i) {
        CHECK(viaProtanopia.indexed[i].red == viaDeuteranopia.indexed[i].red);
        CHECK(viaProtanopia.indexed[i].green == viaDeuteranopia.indexed[i].green);
        CHECK(viaProtanopia.indexed[i].blue == viaDeuteranopia.indexed[i].blue);
    }

    // And it did something, so the agreement above is not two no-ops matching.
    Ubiquitty::CellPalette untouched = darkPalette();
    CHECK(viaProtanopia.indexed[1].red != untouched.indexed[1].red
        || viaProtanopia.indexed[1].green != untouched.indexed[1].green
        || viaProtanopia.indexed[1].blue != untouched.indexed[1].blue);
}

TEST(theTwoPrivateHelpersRefuseTheInputsTheirCallersCannotProduce)
{
    // Both arms are defensive against a caller that does not exist here:
    // `clamp8`'s producers are products of non-negatives and blends whose
    // strength is clamped to one, and `retone` only ever sees four fixed
    // Okabe-Ito hues, every one far brighter than luma 1.0. The guards are worth
    // keeping for whoever calls them next, and they are reached here rather than
    // left as two arms nothing measures.
    using namespace Ubiquitty::Accessibility::ForTest;

    CHECK(clamp8(-1.0) == 0);
    CHECK(clamp8(-1000.0) == 0);
    CHECK(clamp8(300.0) == 255);
    CHECK(clamp8(127.5) == 128); // rounds to nearest rather than truncating
    CHECK(clamp8(0.0) == 0);
    CHECK(clamp8(255.0) == 255);

    // A target darker than luma 1.0 is taken verbatim rather than divided by
    // nearly zero, and the slot's alpha is kept either way.
    const Ubiquitty::ColorRGBA slot { 200, 100, 50, 42 };
    const Ubiquitty::ColorRGBA nearBlack { 0, 0, 1, 255 };
    const Ubiquitty::ColorRGBA verbatim = retone(slot, nearBlack);
    CHECK(verbatim.red == nearBlack.red);
    CHECK(verbatim.green == nearBlack.green);
    CHECK(verbatim.blue == nearBlack.blue);
    CHECK(verbatim.alpha == slot.alpha);

    // A bright target takes the scaling path instead, so the case above is about
    // the fallback rather than about retone returning its argument.
    const Ubiquitty::ColorRGBA bright { 230, 159, 0, 255 };
    const Ubiquitty::ColorRGBA scaled = retone(slot, bright);
    CHECK(scaled.alpha == slot.alpha);
    CHECK(std::abs(luma(scaled) - luma(slot)) < 2.0); // luminance preserved
}

TEST(aModeTheEnumDoesNotCarryLeavesThePaletteAlone)
{
    // `colorblindModeFromInt` refuses an integer outside the enum, so nothing in
    // the config path produces one. The settings surface and the sync protocol
    // both hand this setting across a trust boundary as an integer, though, and
    // the underlying type is `uint8_t` — a value the enum does not name is
    // representable, and the switch answering it by falling out is what keeps a
    // remap from being applied on a reading of the value nobody wrote.
    Ubiquitty::CellPalette palette;
    palette.seedDefaults();
    const Ubiquitty::CellPalette untouched = palette;

    Ubiquitty::Accessibility::applyColorblindRemap(palette,
        static_cast<Ubiquitty::Accessibility::ColorblindMode>(99));
    for (std::size_t slot = 0; slot < 16; ++slot)
        CHECK(sameColor(palette.indexed[slot], untouched.indexed[slot]));

    // The control: a mode the enum does carry changes the palette through the
    // same call, so the agreement above is about the unknown value rather than
    // about this function never doing anything.
    Ubiquitty::CellPalette remapped = untouched;
    Ubiquitty::Accessibility::applyColorblindRemap(remapped,
        Ubiquitty::Accessibility::ColorblindMode::Tritanopia);
    CHECK(!sameColor(remapped.indexed[4], untouched.indexed[4]));
}

UBIQUITTY_TEST_MAIN()
