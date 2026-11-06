// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_chrome_style — the colours in-grid chrome draws with, derived from the
// terminal's own palette.
//
// The failure this exists to prevent is silent and theme-specific: chrome that
// looks right on the themes its author ran and is unreadable on the rest. So the
// assertions are on CONTRAST rather than on colour values — a test pinning
// literal RGB would pass on a palette nobody uses and say nothing about the one
// the user has.
//
// The cases are three palettes chosen to disagree: a dark theme, a light theme,
// and a hostile one whose foreground sits almost on its background. The last is
// the discriminating input — every implementation agrees on the first two.

#include "Harness.h"

#include "Core/TermPalette.h"
#include "Core/Themes/ChromeStyle.h"
#include "Core/Themes/Contrast.h"

#include <cmath>

using Ubiquitty::PaletteRGB;
using Ubiquitty::TermPalette;
using Ubiquitty::Themes::ChromeStyle;
using Ubiquitty::Themes::resolveChromeStyle;

namespace {

// The palette carries 16-bit channels; these fixtures are written in 8-bit and
// widened, which is how a theme file's values reach it.
PaletteRGB rgb(int red, int green, int blue)
{
    return PaletteRGB { static_cast<std::uint16_t>(red * 257),
        static_cast<std::uint16_t>(green * 257),
        static_cast<std::uint16_t>(blue * 257) };
}

TermPalette themeWith(PaletteRGB background, PaletteRGB foreground)
{
    TermPalette palette;
    palette.seedDefaults();
    palette.defaultBackground = background;
    palette.defaultForeground = foreground;
    palette.cursor = foreground;
    return palette;
}

double contrast(std::uint32_t foreground, std::uint32_t background)
{
    return std::fabs(Ubiquitty::Themes::Contrast::apcaLc(foreground, background));
}

// `contrastFloor` walks the foreground in 8-bit HSL, so it lands on the first
// representable step at or past the target — which is sometimes a fraction of an
// Lc SHORT of it, on this fixture set against both the body and the secondary
// floor. The gap is the quantisation of the colour space, not a failure to
// walk, and asserting exact attainment would be asserting something the helper
// does not promise.
//
// Half an Lc is far below any perceptual threshold and far above the observed
// error, so it separates "landed on the nearest step" from "did not move".
constexpr double roundingSlack = 0.5;

} // namespace

TEST(chromeStyleReadsOnDarkAndLightThemes)
{
    const TermPalette dark = themeWith(rgb(0x10, 0x10, 0x14), rgb(0xC0, 0xC0, 0xC0));
    const ChromeStyle darkStyle = resolveChromeStyle(dark);
    CHECK_MSG(darkStyle.isDark, "a near-black background reads as dark");

    const TermPalette light = themeWith(rgb(0xFD, 0xF6, 0xE3), rgb(0x30, 0x30, 0x30));
    const ChromeStyle lightStyle = resolveChromeStyle(light);
    CHECK_MSG(!lightStyle.isDark, "and a near-white one does not");

    // Derived, never carried: nothing in the palette says which it is, so the
    // two answers came from the backgrounds themselves.
    CHECK_MSG(darkStyle.surface != lightStyle.surface, "the two themes resolve different surfaces");

    for (const ChromeStyle& style : { darkStyle, lightStyle }) {
        CHECK_MSG(contrast(style.text, style.surface) >= Ubiquitty::Themes::Contrast::lcBody - roundingSlack,
            "body text clears the body floor against the surface it is drawn on");
        CHECK_MSG(contrast(style.dim, style.surface) >= Ubiquitty::Themes::Contrast::lcSecondary - roundingSlack,
            "secondary text clears the secondary floor");
        CHECK_MSG(contrast(style.accent, style.surface) >= Ubiquitty::Themes::Contrast::lcBody - roundingSlack,
            "and the accent does too — it is what the eye is hunting for");
        CHECK_MSG(contrast(style.selectionText, style.selection) >= Ubiquitty::Themes::Contrast::lcBody - roundingSlack,
            "the selected row's text is floored against the SELECTION, not the surface");
    }
}

// The discriminating case. A theme whose foreground sits almost on its
// background produces a legible pairing only if something moved it — every
// implementation passes the two cases above, including one that returns the
// palette's colours unchanged.
TEST(chromeStyleRescuesAnUnreadableTheme)
{
    const TermPalette hostile = themeWith(rgb(0x20, 0x20, 0x20), rgb(0x28, 0x28, 0x28));

    // The premise, asserted rather than assumed: this pairing really is
    // unreadable as given. Without this the case could pass on a theme that was
    // fine all along.
    CHECK_MSG(contrast(0x282828, 0x202020) < Ubiquitty::Themes::Contrast::lcBody,
        "the fixture's own foreground-on-background is below the body floor");

    const ChromeStyle style = resolveChromeStyle(hostile);
    CHECK_MSG(contrast(style.text, style.surface) >= Ubiquitty::Themes::Contrast::lcBody - roundingSlack,
        "body text was walked to a readable value");
    CHECK_MSG(style.text != 0x282828, "which means it is not the palette's foreground");
    CHECK_MSG(contrast(style.selectionText, style.selection) >= Ubiquitty::Themes::Contrast::lcBody - roundingSlack,
        "and the selected row is readable too, which is the row a user is looking at");
}

// Same palette, same style, every time — what lets a golden image assert the
// picture rather than the frontend that drew it.
TEST(chromeStyleIsPure)
{
    const TermPalette palette = themeWith(rgb(0x1E, 0x1E, 0x2E), rgb(0xCD, 0xD6, 0xF4));
    const ChromeStyle first = resolveChromeStyle(palette);
    const ChromeStyle second = resolveChromeStyle(palette);

    CHECK_EQ(static_cast<int>(first.surface), static_cast<int>(second.surface));
    CHECK_EQ(static_cast<int>(first.text), static_cast<int>(second.text));
    CHECK_EQ(static_cast<int>(first.accent), static_cast<int>(second.accent));
    CHECK_EQ(first.isDark ? 1 : 0, second.isDark ? 1 : 0);

    // An OSC 11 background change moves the whole style with it, including the
    // dark/light answer — the case a carried flag would get wrong.
    TermPalette recoloured = palette;
    recoloured.defaultBackground = rgb(0xFF, 0xFF, 0xFF);
    const ChromeStyle after = resolveChromeStyle(recoloured);
    CHECK_MSG(!after.isDark, "a background swapped to white reads as light immediately");
    CHECK_MSG(after.surface != first.surface, "and the surface followed it");
}

UBIQUITTY_TEST_MAIN()
