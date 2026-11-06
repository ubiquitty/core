// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_tab_band.cpp — the tab strip's colours (Source/Core/Themes/TabBand.h):
// the band surface derived in Oklch, the pill fills by polarity and focus, and
// the coverage a tab title and its accessory marks are drawn at over the band.
//
// The band cases pin the anchors the header's own comment names, so a change
// to the lift is a change to a stated number rather than a drift. The coverage
// cases pin the system's measured numbers, the inactive recession, and the
// Increase Contrast floors over every built-in theme — the last because the
// defect they exist to prevent is theme-specific: a title that reads on the
// theme its author ran and not on the one the user has.

#include "Core/Themes/Contrast.h"
#include "Core/Themes/TabBand.h"
#include "Core/Themes/Themes.h"

#include <cmath>
#include <cstdint>
#include <cstdio>

using Ubiquitty::ColorRGBA;
using Ubiquitty::Themes::activeTabAccessoryCoverage;
using Ubiquitty::Themes::tabAccessoryCoverage;
using Ubiquitty::Themes::TabAccessoryMark;
using Ubiquitty::Themes::tabBandTint;
using Ubiquitty::Themes::TabStripFacts;
using Ubiquitty::Themes::tabStripPaint;
using Ubiquitty::Themes::TabStripPaint;
using Ubiquitty::Themes::tabTitleCoverage;
using Ubiquitty::Themes::TabTitleFacts;
using Ubiquitty::Themes::Contrast::apcaLc;
using Ubiquitty::Themes::Contrast::lcBody;
using Ubiquitty::Themes::Contrast::lcSecondary;
using Ubiquitty::Themes::Contrast::wcagContrastRatio;
using Ubiquitty::Themes::Contrast::wcagMinimumRatio;

namespace {

std::uint32_t packed(ColorRGBA c)
{
    return (static_cast<std::uint32_t>(c.red) << 16) | (static_cast<std::uint32_t>(c.green) << 8) | static_cast<std::uint32_t>(c.blue);
}

ColorRGBA rgb(std::uint32_t packedRGB)
{
    return { static_cast<uint8_t>((packedRGB >> 16) & 0xFF), static_cast<uint8_t>((packedRGB >> 8) & 0xFF), static_cast<uint8_t>(packedRGB & 0xFF), 255 };
}

// The foreground over the band at a coverage, composited as the screen does.
std::uint32_t blend(ColorRGBA foreground, ColorRGBA band, double coverage)
{
    const auto channel = [&](int ink, int fill) { return static_cast<std::uint32_t>(std::lround(ink * coverage + fill * (1.0 - coverage))); };
    return (channel(foreground.red, band.red) << 16) | (channel(foreground.green, band.green) << 8) | channel(foreground.blue, band.blue);
}

bool reads(std::uint32_t ink, std::uint32_t fill, double targetLc)
{
    return std::fabs(apcaLc(ink, fill)) >= targetLc && wcagContrastRatio(ink, fill) >= wcagMinimumRatio;
}

} // namespace

// The anchors the header comment states: Noir's navy lifts to #23252b, pure
// black to #121212, white drops to #f2f2f2, and a pure grey stays neutral.
TEST(testBandAnchors)
{
    CHECK(packed(tabBandTint(rgb(0x15171c))) == 0x23252bu);
    CHECK(packed(tabBandTint(rgb(0x000000))) == 0x121212u);
    CHECK(packed(tabBandTint(rgb(0xffffff))) == 0xf2f2f2u);
    const ColorRGBA grey = tabBandTint(rgb(0x282828));
    CHECK(grey.red == grey.green && grey.green == grey.blue);
}

// The strip's paint by polarity and focus. In the main window a dark theme
// fills every pill with the background and a light theme fills none; a window
// that is not main keeps the background on the selected pill and sinks every
// other pill into the band, whatever the polarity.
TEST(testStripPaintByPolarityAndFocus)
{
    const ColorRGBA dark = rgb(0x15171c), light = rgb(0xf4f5f8);
    const auto same = [](ColorRGBA a, ColorRGBA b) { return packed(a) == packed(b); };

    const TabStripPaint darkMain = tabStripPaint(dark, { .isWindowActive = true });
    CHECK(same(darkMain.band, tabBandTint(dark)));
    CHECK(darkMain.hasSelectedPill && same(darkMain.selectedPill, dark));
    CHECK(darkMain.hasUnselectedPill && same(darkMain.unselectedPill, dark));

    const TabStripPaint lightMain = tabStripPaint(light, { .isWindowActive = true });
    CHECK(same(lightMain.band, tabBandTint(light)));
    CHECK(!lightMain.hasSelectedPill && !lightMain.hasUnselectedPill);

    for (ColorRGBA background : { dark, light }) {
        const TabStripPaint inactive = tabStripPaint(background, { .isWindowActive = false });
        CHECK(same(inactive.band, tabBandTint(background)));
        CHECK(inactive.hasSelectedPill && same(inactive.selectedPill, background));
        CHECK(!inactive.hasUnselectedPill);
        // The selected pill is told from the band it sits in.
        CHECK(packed(inactive.selectedPill) != packed(inactive.band));
    }
}

// Without the switch, the coverage is the system's own number for the role,
// whatever the theme: the strip follows the native look and does not lift.
TEST(testDefaultCoverageIsTheSystemsOnEveryTheme)
{
    const int count = Ubiquitty::Themes::count();
    CHECK(count > 0);
    for (int i = 0; i < count; ++i) {
        const Ubiquitty::Themes::Theme* theme = Ubiquitty::Themes::get(i);
        if (!theme)
            continue;
        CHECK(tabTitleCoverage(theme->defaultForeground, theme->defaultBackground, { .isSelected = true }) == Ubiquitty::Themes::selectedTabTitleCoverage);
        CHECK(tabTitleCoverage(theme->defaultForeground, theme->defaultBackground, { .isSelected = false }) == Ubiquitty::Themes::unselectedTabTitleCoverage);
    }
    CHECK(Ubiquitty::Themes::selectedTabTitleCoverage > Ubiquitty::Themes::unselectedTabTitleCoverage);
}

// A window that is not main recedes every title to the same coverage, selected
// or not, keyed on the band's polarity — and further than any active title.
TEST(testInactiveWindowRecedesEveryTitleAlike)
{
    const ColorRGBA lightForeground = rgb(0x32363c), lightBackground = rgb(0xf4f5f8);
    const ColorRGBA darkForeground = rgb(0xc8ccd4), darkBackground = rgb(0x15171c);
    for (bool selected : { true, false }) { // bool-ok: both selection states of a tab, iterated
        const TabTitleFacts inactive { .isSelected = selected, .isWindowActive = false };
        CHECK(tabTitleCoverage(lightForeground, lightBackground, inactive) == Ubiquitty::Themes::inactiveLightTabTitleCoverage);
        CHECK(tabTitleCoverage(darkForeground, darkBackground, inactive) == Ubiquitty::Themes::inactiveDarkTabTitleCoverage);
        // The switch does not lift an inactive title: focus is what it tells.
        const TabTitleFacts inactiveContrast { .isSelected = selected, .isWindowActive = false, .shouldIncreaseContrast = true };
        CHECK(tabTitleCoverage(lightForeground, lightBackground, inactiveContrast) == Ubiquitty::Themes::inactiveLightTabTitleCoverage);
    }
    CHECK(Ubiquitty::Themes::inactiveLightTabTitleCoverage < Ubiquitty::Themes::unselectedTabTitleCoverage);
    CHECK(Ubiquitty::Themes::inactiveDarkTabTitleCoverage < Ubiquitty::Themes::unselectedTabTitleCoverage);
}

// With the switch on, the title clears both floors for its role on every
// built-in theme whose foreground can, and never drops under the system's
// number to do it.
TEST(testIncreaseContrastClearsTheFloorsOnEveryBuiltInTheme)
{
    const int count = Ubiquitty::Themes::count();
    int lifted = 0, unreachable = 0;
    for (int i = 0; i < count; ++i) {
        const Ubiquitty::Themes::Theme* theme = Ubiquitty::Themes::get(i);
        if (!theme)
            continue;
        const ColorRGBA band = tabBandTint(theme->defaultBackground);
        for (bool selected : { true, false }) { // bool-ok: both selection states of a tab, iterated
            const double targetLc = selected ? lcBody : lcSecondary;
            const double base = selected ? Ubiquitty::Themes::selectedTabTitleCoverage : Ubiquitty::Themes::unselectedTabTitleCoverage;
            const double coverage = tabTitleCoverage(theme->defaultForeground, theme->defaultBackground, { .isSelected = selected, .shouldIncreaseContrast = true });
            CHECK(coverage >= base);
            CHECK(coverage <= 1.0);
            if (!reads(packed(theme->defaultForeground), packed(band), targetLc)) {
                // The foreground itself cannot: the most that can be asked is all of it.
                CHECK(coverage == 1.0);
                ++unreachable;
                continue;
            }
            if (!reads(blend(theme->defaultForeground, band, coverage), packed(band), targetLc)) {
                printf("FAIL theme '%s' %s: coverage %.3f does not read on band #%06x\n", theme->name, selected ? "selected" : "unselected", coverage, packed(band));
                CHECK(false);
            }
            if (coverage > base)
                ++lifted;
        }
    }
    CHECK(lifted > 0);
    // Three shipping themes put their foreground under the secondary floors on
    // their own band (One Dark, Solarized Dark, Solarized Light), and seventeen
    // in all fall short of the body floor — Noir's own foreground stops at
    // Lc 72.6. The count is pinned so a theme edit that moves it is noticed.
    CHECK(unreachable == 20);
}

// The lift is the least that reads: one coverage step under it fails a floor.
TEST(testIncreaseContrastLiftsToTheFirstReadingStep)
{
    const ColorRGBA foreground = rgb(0x32363c), background = rgb(0xf4f5f8); // Noir Clair
    const ColorRGBA band = tabBandTint(background);
    const double coverage = tabTitleCoverage(foreground, background, { .isSelected = false, .shouldIncreaseContrast = true });
    CHECK(coverage > Ubiquitty::Themes::unselectedTabTitleCoverage);
    CHECK(reads(blend(foreground, band, coverage), packed(band), lcSecondary));
    CHECK(!reads(blend(foreground, band, coverage - 1.0 / 255.0), packed(band), lcSecondary));
}

namespace {

constexpr TabAccessoryMark accessoryMarks[] = { TabAccessoryMark::ShortcutHint, TabAccessoryMark::ProgressRing, TabAccessoryMark::ActivityDots };

} // namespace

// In an active window without the switch, each mark wears its own number on
// every theme.
TEST(testActiveAccessoryWearsItsOwnCoverage)
{
    const ColorRGBA foreground = rgb(0xc8ccd4), background = rgb(0x15171c);
    CHECK(tabAccessoryCoverage(TabAccessoryMark::ShortcutHint, foreground, background, { .isSelected = true }) == Ubiquitty::Themes::selectedTabHintCoverage);
    CHECK(tabAccessoryCoverage(TabAccessoryMark::ShortcutHint, foreground, background, { .isSelected = false }) == Ubiquitty::Themes::unselectedTabHintCoverage);
    CHECK(tabAccessoryCoverage(TabAccessoryMark::ProgressRing, foreground, background, { .isSelected = false }) == Ubiquitty::Themes::tabProgressRingCoverage);
    CHECK(tabAccessoryCoverage(TabAccessoryMark::ActivityDots, foreground, background, { .isSelected = false }) == Ubiquitty::Themes::tabActivityDotsCoverage);
}

// An inactive window recedes every mark by exactly the proportion its tab's
// title recedes, on every built-in theme, so the mark cannot stay bright
// beside a title that dimmed. The switch lifts neither.
TEST(testInactiveAccessoryRecedesWithItsTitle)
{
    const int count = Ubiquitty::Themes::count();
    CHECK(count > 0);
    for (int i = 0; i < count; ++i) {
        const Ubiquitty::Themes::Theme* theme = Ubiquitty::Themes::get(i);
        if (!theme)
            continue;
        const ColorRGBA foreground = theme->defaultForeground, background = theme->defaultBackground;
        for (bool selected : { true, false }) { // bool-ok: both selection states of a tab, iterated
            for (bool contrast : { false, true }) { // bool-ok: the Increase Contrast switch, both ways
                const TabTitleFacts inactive { .isSelected = selected, .isWindowActive = false, .shouldIncreaseContrast = contrast };
                const double titleShare = tabTitleCoverage(foreground, background, inactive) / tabTitleCoverage(foreground, background, { .isSelected = selected });
                CHECK(titleShare < 1.0);
                for (TabAccessoryMark mark : accessoryMarks) {
                    const double active = activeTabAccessoryCoverage(mark, selected);
                    const double coverage = tabAccessoryCoverage(mark, foreground, background, inactive);
                    CHECK(std::fabs(coverage - active * titleShare) < 1e-12);
                    CHECK(coverage < active);
                }
            }
        }
    }
}

// With the switch on, the hint is text and reads at the secondary floors on
// every built-in theme whose foreground can; the ring and the dots are not
// text and keep their numbers.
TEST(testIncreaseContrastLiftsOnlyTheHint)
{
    const int count = Ubiquitty::Themes::count();
    int lifted = 0;
    for (int i = 0; i < count; ++i) {
        const Ubiquitty::Themes::Theme* theme = Ubiquitty::Themes::get(i);
        if (!theme)
            continue;
        const ColorRGBA foreground = theme->defaultForeground, background = theme->defaultBackground;
        const ColorRGBA band = tabBandTint(background);
        const TabTitleFacts facts { .isSelected = false, .shouldIncreaseContrast = true };
        CHECK(tabAccessoryCoverage(TabAccessoryMark::ProgressRing, foreground, background, facts) == Ubiquitty::Themes::tabProgressRingCoverage);
        CHECK(tabAccessoryCoverage(TabAccessoryMark::ActivityDots, foreground, background, facts) == Ubiquitty::Themes::tabActivityDotsCoverage);
        const double hint = tabAccessoryCoverage(TabAccessoryMark::ShortcutHint, foreground, background, facts);
        CHECK(hint >= Ubiquitty::Themes::unselectedTabHintCoverage);
        CHECK(hint <= 1.0);
        if (!reads(packed(foreground), packed(band), lcSecondary)) {
            CHECK(hint == 1.0);
            continue;
        }
        if (!reads(blend(foreground, band, hint), packed(band), lcSecondary)) {
            printf("FAIL theme '%s': hint coverage %.3f does not read on band #%06x\n", theme->name, hint, packed(band));
            CHECK(false);
        }
        if (hint > Ubiquitty::Themes::unselectedTabHintCoverage)
            ++lifted;
    }
    CHECK(lifted > 0);
}

UBIQUITTY_TEST_MAIN()
