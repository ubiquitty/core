// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
// Source/Core/Themes/TabBand.h — tab-strip band surface derived in Oklch.
//
// A dark theme lifts, a light theme drops, but in perceptual space so a
// desaturated navy (#15171c) does not gray out. Old sRGB desat 0.18 +16 gave
// #15171c -> #25272b (oklch 0.272 0.007 264) desaturated; Oklch L+0.06
// (L<0.02 +0.18 for #000000 -> #121212 vs +16 #101010, L<0.08 +0.07)
// C=max(0.012,C*0.95) keeps pure gray #282828 neutral (C<0.001 -> 0) else
// #23252b for Noir and #153a45 for Solarized Dark (#002b36 0.048) — toned down.
// L-0.04 keep C — white -> #f2f2f2. Same L/C/H for sRGB and P3, only fit
// differs (P3 can keep higher C).

#include "Core/Color.h"
#include "Core/Themes/Contrast.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>

namespace Ubiquitty::Themes {

namespace detail {

/// Decodes one sRGB-encoded component to linear light.
///
/// - Parameter c: The encoded component in 0...1.
/// - Returns: The linear component in 0...1.
inline double srgbToLinear(double c)
{
    return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}
/// Encodes one linear-light component back to sRGB.
///
/// - Parameter c: The linear component in 0...1.
/// - Returns: The encoded component in 0...1.
inline double linearToSRGB(double c)
{
    return c <= 0.0031308 ? c * 12.92 : 1.055 * std::pow(c, 1.0 / 2.4) - 0.055;
}

/// A colour in Oklch: perceptual lightness, chroma, and hue in degrees.
struct Oklch {
    double l = 0, c = 0, h = 0;
};

/// Converts encoded sRGB components to Oklch.
///
/// - Parameters:
///   - r: The encoded red component in 0...1.
///   - g: The encoded green component in 0...1.
///   - b: The encoded blue component in 0...1.
/// - Returns: The colour in Oklch.
inline Oklch rgbToOklch(double r, double g, double b)
{
    const double lr = srgbToLinear(r), lg = srgbToLinear(g), lb = srgbToLinear(b);
    const double l_ = 0.4122214708 * lr + 0.5363325363 * lg + 0.0514459929 * lb;
    const double m_ = 0.2119034982 * lr + 0.6806995451 * lg + 0.1073969566 * lb;
    const double s_ = 0.0883024619 * lr + 0.2817188376 * lg + 0.6299787005 * lb;
    const double l = l_ > 0 ? std::cbrt(l_) : 0, m = m_ > 0 ? std::cbrt(m_) : 0,
                 s = s_ > 0 ? std::cbrt(s_) : 0;
    const double L = 0.2104542553 * l + 0.793617785 * m - 0.0040720468 * s;
    const double a = 1.9779984951 * l - 2.428592205 * m + 0.4505937099 * s;
    const double bOk = 0.0259040371 * l + 0.7827717662 * m - 0.808675766 * s;
    const double C = std::sqrt(a * a + bOk * bOk);
    double H = std::atan2(bOk, a) * 180.0 / std::numbers::pi;
    if (H < 0)
        H += 360.0;
    return { L, C, H };
}

/// Converts Oklch to linear-light sRGB components, unclamped: a chroma the
/// gamut cannot hold comes back outside 0...1 for the caller to fit.
///
/// - Parameters:
///   - L: The perceptual lightness.
///   - C: The chroma.
///   - H: The hue in degrees.
///   - outR: The linear red component, possibly outside 0...1.
///   - outG: The linear green component, possibly outside 0...1.
///   - outB: The linear blue component, possibly outside 0...1.
inline void oklchToLinear(double L, double C, double H, double& outR, double& outG, double& outB)
{
    const double a = C * std::cos(H * std::numbers::pi / 180.0);
    const double bOk = C * std::sin(H * std::numbers::pi / 180.0);
    const double ll = L + 0.3963377774 * a + 0.2158037573 * bOk;
    const double mm = L - 0.1055613458 * a - 0.0638541728 * bOk;
    const double ss = L - 0.0894841775 * a - 1.291485548 * bOk;
    const double l3 = ll * ll * ll, m3 = mm * mm * mm, s3 = ss * ss * ss;
    outR = 4.0767416621 * l3 - 3.3077115913 * m3 + 0.2309699292 * s3;
    outG = -1.2684380046 * l3 + 2.6097574011 * m3 - 0.3413193965 * s3;
    outB = -0.0041960863 * l3 - 0.7034186147 * m3 + 1.707614701 * s3;
}

/// Fits an Oklch colour into the sRGB gamut and quantises it.
///
/// Chroma is walked down until the linear components fit, so an out-of-gamut
/// tint keeps its lightness and hue rather than clipping per channel.
///
/// - Parameter o: The colour to fit.
/// - Returns: The nearest in-gamut colour, opaque.
inline ColorRGBA oklchToSRGB(Oklch o)
{
    double currentC = std::max(0.0, o.c);
    double currentL = std::clamp(o.l, 0.0, 1.0);
    const double H = std::fmod(o.h, 360.0);
    double lr = 0, lg = 0, lb = 0;
    for (int i = 0; i < 24; ++i) {
        oklchToLinear(currentL, currentC, H, lr, lg, lb);
        if (lr >= 0 && lr <= 1 && lg >= 0 && lg <= 1 && lb >= 0 && lb <= 1)
            break;
        currentC *= 0.9;
    }
    if (lr < 0 || lr > 1 || lg < 0 || lg > 1 || lb < 0 || lb > 1) {
        currentC = 0;
        oklchToLinear(currentL, currentC, H, lr, lg, lb);
    }
    auto q = [](double v) -> uint8_t {
        double s = linearToSRGB(std::clamp(v, 0.0, 1.0));
        return static_cast<uint8_t>(std::lround(std::clamp(s, 0.0, 1.0) * 255.0));
    };
    return { q(lr), q(lg), q(lb), 255 };
}

/// Packs a colour's channels as 0xRRGGBB, the form `Contrast` reads.
///
/// - Parameter c: The colour; its alpha is dropped.
/// - Returns: The packed channels.
[[nodiscard]] inline std::uint32_t packedRGB(ColorRGBA c)
{
    return (static_cast<std::uint32_t>(c.red) << 16) | (static_cast<std::uint32_t>(c.green) << 8) | static_cast<std::uint32_t>(c.blue);
}

} // namespace detail

[[nodiscard]] inline ColorRGBA tabBandTint(ColorRGBA bg)
{
    const detail::Oklch o = detail::rgbToOklch(bg.red / 255.0, bg.green / 255.0, bg.blue / 255.0);
    const bool isDark = o.l < 0.5;
    double newL;
    if (isDark) {
        if (o.l < 0.02)
            newL = o.l + 0.18; // pure black #000000 L0 -> 0.18 #121212 vs sRGB +16 #101010
        else if (o.l < 0.08)
            newL = o.l + 0.07;
        else
            newL = o.l + 0.06;
    } else {
        newL = o.l - 0.04;
    }
    double newC;
    if (isDark) {
        if (o.c < 0.001)
            newC = 0; // keep pure gray #282828 neutral, don't tint 0->0.012
        else
            newC = std::max(0.012, o.c * 0.95); // navy 0.010->0.012, Solarized 0.048->0.046 toned down
    } else {
        newC = o.c;
    }
    return detail::oklchToSRGB({ newL, newC, o.h });
}

[[nodiscard]] inline std::uint32_t tabBandTint(std::uint32_t rgb)
{
    const ColorRGBA bg { static_cast<uint8_t>((rgb >> 16) & 0xFF),
        static_cast<uint8_t>((rgb >> 8) & 0xFF),
        static_cast<uint8_t>(rgb & 0xFF), 255 };
    return detail::packedRGB(tabBandTint(bg));
}

[[nodiscard]] inline bool isDark(ColorRGBA c)
{
    return detail::rgbToOklch(c.red / 255.0, c.green / 255.0, c.blue / 255.0).l < 0.5;
}

/// The facts the strip's paint is decided by.
struct TabStripFacts {
    bool isWindowActive = true; ///< The strip's tab group holds the key or main window of the active application.
};

/// What the strip paints: the band, and the pill fills where a pill takes one.
///
/// A pill with no fill keeps AppKit's native glass. Opaque colours; the
/// frontend applies the title row's alpha.
struct TabStripPaint {
    ColorRGBA band; ///< The strip's ground.
    bool hasSelectedPill = false; ///< Whether the selected pill takes `selectedPill`.
    ColorRGBA selectedPill; ///< The selected pill's fill, when `hasSelectedPill`.
    bool hasUnselectedPill = false; ///< Whether every other pill takes `unselectedPill`.
    ColorRGBA unselectedPill; ///< An unselected pill's fill, when `hasUnselectedPill`.
};

/// The strip's paint for a theme background and the window's focus.
///
/// The band is the background nudged off itself (``tabBandTint``). In the main
/// window a dark theme fills every pill with the background, so each tab
/// matches the dark body where AppKit's glass would read as a too-light grey;
/// a light theme keeps the native glass, which tinting a near-white ground
/// through would darken and over-saturate. An inactive window tells so the
/// same way in both polarities: the selected pill keeps the background and
/// every other pill leaves the band exposed, which is the relationship the system's
/// own inactive tab bar shows.
///
/// - Parameters:
///   - background: The theme's default background, or the chrome tint standing
///     in for it.
///   - facts: The window's activation.
/// - Returns: The paint, opaque.
[[nodiscard]] inline TabStripPaint tabStripPaint(ColorRGBA background, TabStripFacts facts)
{
    TabStripPaint paint;
    paint.band = tabBandTint(background);
    if (!facts.isWindowActive) {
        paint.hasSelectedPill = true;
        paint.selectedPill = background;
    } else if (isDark(background)) {
        paint.hasSelectedPill = true;
        paint.selectedPill = background;
        paint.hasUnselectedPill = true;
        paint.unselectedPill = background;
    }
    return paint;
}

/// The facts a tab title's coverage is decided by.
struct TabTitleFacts {
    bool isSelected = false; ///< The tab is its group's selected one.
    bool isWindowActive = true; ///< The strip's tab group holds the key or main window of the active application.
    bool shouldIncreaseContrast = false; ///< The system's Increase Contrast switch is on.
};

/// The opacity the system's own tab bar draws its labels at, measured on
/// macOS 26 Finder. The label there is `labelColor`, the pole at 0.847, and
/// the ink measured as a fraction of the pole was 0.72 on the selected tab,
/// 0.56 on the rest, and 0.26 (light) or 0.32 (dark) on every tab of a window
/// that is not main, selected or not. The theme foreground stands where
/// `labelColor` stands, so the numbers are stated relative to it.
inline constexpr double selectedTabTitleCoverage = 0.85;
inline constexpr double unselectedTabTitleCoverage = 0.66;
inline constexpr double inactiveLightTabTitleCoverage = 0.31;
inline constexpr double inactiveDarkTabTitleCoverage = 0.38;

/// The coverage of the theme foreground a tab title is drawn at over the band.
///
/// The system's numbers by default, so a themed strip reads as the native one
/// does with the theme's own ink in place of the label colour. With Increase
/// Contrast on, the coverage rises to the least that clears the WCAG 2 AA
/// ratio and APCA's floor for the role — body for the selected tab, secondary
/// for the rest — where the foreground can reach it at all. A window that is
/// inactive takes the system's inactive coverage whatever the tab, which is how
/// the strip tells focus: every title recedes together.
///
/// - Parameters:
///   - foreground: The theme's default foreground.
///   - background: The theme's default background, which the band derives from.
///   - facts: The tab's selection, its window's activation, and the switch.
/// - Returns: The coverage in 0...1.
[[nodiscard]] inline double tabTitleCoverage(ColorRGBA foreground, ColorRGBA background, TabTitleFacts facts)
{
    const ColorRGBA band = tabBandTint(background);
    if (!facts.isWindowActive)
        return isDark(band) ? inactiveDarkTabTitleCoverage : inactiveLightTabTitleCoverage;
    const double coverage = facts.isSelected ? selectedTabTitleCoverage : unselectedTabTitleCoverage;
    if (!facts.shouldIncreaseContrast)
        return coverage;
    const double floor = Contrast::minReadingCoverage(detail::packedRGB(foreground), detail::packedRGB(band),
        facts.isSelected ? Contrast::lcBody : Contrast::lcSecondary, Contrast::wcagMinimumRatio);
    return std::max(coverage, floor);
}

/// A mark a tab's accessory slot draws in the theme's ink.
enum class TabAccessoryMark : std::uint8_t {
    ShortcutHint, ///< The "⌘N" that selects the tab; text, read as secondary.
    ProgressRing, ///< The ring an OSC 9;4 report draws.
    ActivityDots, ///< The ellipsis unread output reads as.
};

/// The coverage each accessory mark wears in an active window, relative to the
/// theme foreground as the title's numbers are.
inline constexpr double selectedTabHintCoverage = 1.0;
inline constexpr double unselectedTabHintCoverage = 0.45;
inline constexpr double tabProgressRingCoverage = 0.45;
inline constexpr double tabActivityDotsCoverage = 0.4;

/// The coverage a mark wears in an active window, before any contrast floor.
///
/// - Parameters:
///   - mark: The mark being drawn.
///   - isSelected: Whether the mark's tab is its group's selected one.
/// - Returns: The coverage in 0...1.
[[nodiscard]] constexpr double activeTabAccessoryCoverage(TabAccessoryMark mark, bool isSelected)
{
    switch (mark) {
    case TabAccessoryMark::ShortcutHint:
        return isSelected ? selectedTabHintCoverage : unselectedTabHintCoverage;
    case TabAccessoryMark::ProgressRing:
        return tabProgressRingCoverage;
    case TabAccessoryMark::ActivityDots:
        return tabActivityDotsCoverage;
    }
    return tabActivityDotsCoverage;
}

/// The coverage of the theme foreground an accessory mark is drawn at over the
/// band.
///
/// A mark reads as part of its tab's title, so it recedes by the title's own
/// proportion: an inactive window scales the mark by the title's inactive
/// coverage over its active one. Keyed on the title rather than a factor of
/// its own, so the mark and the text beside it cannot recede by different
/// amounts. With Increase Contrast on, the hint is text and takes the
/// secondary reading floor as an unselected title does; the ring and the dots
/// are not text and keep their numbers.
///
/// - Parameters:
///   - mark: The mark being drawn.
///   - foreground: The theme's default foreground.
///   - background: The theme's default background, which the band derives from.
///   - facts: The tab's selection, its window's activation, and the switch.
/// - Returns: The coverage in 0...1.
[[nodiscard]] inline double tabAccessoryCoverage(TabAccessoryMark mark, ColorRGBA foreground, ColorRGBA background, TabTitleFacts facts)
{
    const double coverage = activeTabAccessoryCoverage(mark, facts.isSelected);
    if (!facts.isWindowActive) {
        TabTitleFacts active = facts;
        active.isWindowActive = true;
        active.shouldIncreaseContrast = false;
        return coverage * tabTitleCoverage(foreground, background, facts) / tabTitleCoverage(foreground, background, active);
    }
    if (mark != TabAccessoryMark::ShortcutHint || !facts.shouldIncreaseContrast)
        return coverage;
    const std::uint32_t band = detail::packedRGB(tabBandTint(background));
    return std::max(coverage, Contrast::minReadingCoverage(detail::packedRGB(foreground), band, Contrast::lcSecondary, Contrast::wcagMinimumRatio));
}

/// The share of its active ink the window title row keeps in an inactive
/// window, measured off the native titlebar in both states. A factor rather
/// than a second colour, so a themed row dims by the same proportion.
inline constexpr double inactiveWindowTitleScale = 0.43;

} // namespace Ubiquitty::Themes
