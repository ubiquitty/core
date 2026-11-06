// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// Host-side tests for Source/Core/Themes/Themes.cpp — the compile-time theme
// table and the lookups over it.
//
// Pins:
//   - count() > 0
//   - get(i) returns a valid pointer for every i in range; nullptr OOR
//   - every theme has a non-empty, unique name
//   - no two shipping themes carry the same palette
//   - indexForName resolves each shipping name (exact + case-insensitive),
//     and returns -1 for misses / null

#include "Core/CellColor.h"
#include "Core/Themes/Themes.h"
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

static bool colorsEqual(Ubiquitty::ColorRGBA a, Ubiquitty::ColorRGBA b) { return a.red == b.red && a.green == b.green && a.blue == b.blue; }

TEST(testCountPositive)
{
    int n = Ubiquitty::Themes::count();
    CHECK(n > 0);
}

TEST(testGetInRangeReturnsTheme)
{
    int n = Ubiquitty::Themes::count();
    int i;
    for (i = 0; i < n; i++) {
        const Ubiquitty::Themes::Theme* t = Ubiquitty::Themes::get(i);
        CHECK(t != nullptr);
        if (t) {
            CHECK(t->name != nullptr);
            CHECK(t->name[0] != 0);
        }
    }
}

TEST(testGetOutOfRangeReturnsNull)
{
    CHECK(Ubiquitty::Themes::get(-1) == nullptr);
    CHECK(Ubiquitty::Themes::get(Ubiquitty::Themes::count()) == nullptr);
    CHECK(Ubiquitty::Themes::get(99999) == nullptr);
}

TEST(testUniqueNames)
{
    int n = Ubiquitty::Themes::count();
    int i, j;
    for (i = 0; i < n; i++) {
        const Ubiquitty::Themes::Theme* ti = Ubiquitty::Themes::get(i);
        if (!ti)
            continue;
        for (j = i + 1; j < n; j++) {
            const Ubiquitty::Themes::Theme* tj = Ubiquitty::Themes::get(j);
            if (!tj)
                continue;
            if (std::strcmp(ti->name, tj->name) == 0) {
                printf("FAIL duplicate theme name '%s' at indices %d and %d\n", ti->name, i, j);
                failures++;
            }
        }
    }
}

/// Two shipping themes that look the same under two names is a content bug in
/// the table, not a lookup bug, so this reads the table directly.
///
/// Three, because two themes within +/-2 per channel of each other are one
/// theme wearing two names, and a person picking between them sees no
/// difference. The property belongs to the themes we ship, whatever reads
/// them.
TEST(testShippingThemesAreDistinguishable)
{
    constexpr int leastVisibleChannelDifference = 3;
    const auto differsVisibly = [](Ubiquitty::ColorRGBA a, Ubiquitty::ColorRGBA b) {
        const auto apart = [](std::uint8_t x, std::uint8_t y) {
            return std::abs(static_cast<int>(x) - static_cast<int>(y)) >= leastVisibleChannelDifference;
        };
        return apart(a.red, b.red) || apart(a.green, b.green) || apart(a.blue, b.blue);
    };

    const int n = Ubiquitty::Themes::count();
    for (int i = 0; i < n; i++) {
        const Ubiquitty::Themes::Theme* first = Ubiquitty::Themes::get(i);
        if (!first)
            continue;
        for (int k = i + 1; k < n; k++) {
            const Ubiquitty::Themes::Theme* second = Ubiquitty::Themes::get(k);
            if (!second)
                continue;
            bool isDistinguishable = differsVisibly(first->defaultForeground, second->defaultForeground)
                || differsVisibly(first->defaultBackground, second->defaultBackground)
                || differsVisibly(first->cursor, second->cursor);
            for (int channel = 0; channel < 16 && !isDistinguishable; channel++)
                isDistinguishable = differsVisibly(first->ansi[channel], second->ansi[channel]);
            if (!isDistinguishable) {
                std::printf("FAIL themes %d and %d ship the same palette\n", i, k);
                failures++;
            }
        }
    }
}

/// isDark accessor — Latte/light themes report false; Mocha/dark
/// themes report true; OOR index reports false (light default).
TEST(test_isDark_metadata)
{
    int n = Ubiquitty::Themes::count();
    int i;
    for (i = 0; i < n; i++) {
        const Ubiquitty::Themes::Theme* t = Ubiquitty::Themes::get(i);
        if (!t)
            continue;
        // Recompute the Rec. 709 luma and pin against the precomputed
        // flag — catches table-entry typos.
        unsigned luma = (2126u * t->defaultBackground.red + 7152u * t->defaultBackground.green + 722u * t->defaultBackground.blue) / 10000u;
        bool expectedDark = (luma < 128u);
        if (Ubiquitty::Themes::isDark(i) != expectedDark) {
            printf("FAIL theme[%d] '%s' isDark=%d expected=%d (luma=%u)\n", i, t->name, Ubiquitty::Themes::isDark(i) ? 1 : 0, expectedDark ? 1 : 0, luma);
            failures++;
        }
    }
    CHECK(Ubiquitty::Themes::isDark(-1) == false);
    CHECK(Ubiquitty::Themes::isDark(99999) == false);
}

/// indexForName: exact match wins, case-insensitive fallback, -1 for misses and
/// null. This is the lookup the macOS config loader + snapshot CLI route through
/// so a persisted theme name resolves the same way everywhere.
TEST(test_indexForName)
{
    int n = Ubiquitty::Themes::count();
    // Every shipping theme resolves to its own index by its exact name.
    for (int i = 0; i < n; ++i) {
        const Ubiquitty::Themes::Theme* t = Ubiquitty::Themes::get(i);
        if (!t || !t->name)
            continue;
        int got = Ubiquitty::Themes::indexForName(t->name);
        if (got != i) {
            // A duplicate name would make the first win — but testUniqueNames
            // already pins uniqueness, so this must be an exact self-match.
            printf("FAIL indexForName('%s') = %d, expected %d\n", t->name, got, i);
            failures++;
        }
    }

    /// Case-insensitive fallback: upper/lower-case spellings of a real name
    /// resolve to the same index as the exact spelling.
    const Ubiquitty::Themes::Theme* first = Ubiquitty::Themes::get(0);
    if (first && first->name) {
        std::string upper(first->name);
        for (char& c : upper)
            if (c >= 'a' && c <= 'z')
                c = static_cast<char>(c - 'a' + 'A');
        int exact = Ubiquitty::Themes::indexForName(first->name);
        int folded = Ubiquitty::Themes::indexForName(upper.c_str());
        // When the exact spelling already has no lowercase letters the two are
        // the same string; either way they must agree.
        CHECK(folded == exact);
    }

    // Misses and null → -1.
    CHECK(Ubiquitty::Themes::indexForName("definitely not a theme name") == -1);
    CHECK(Ubiquitty::Themes::indexForName("") == -1);
    CHECK(Ubiquitty::Themes::indexForName(nullptr) == -1);
}

// WCAG contrast ratio mirror of CellColor.cpp, kept here so the test asserts the
// resolved color clears a legibility floor independently of the implementation.
static double linkRelativeLuminance(Ubiquitty::ColorRGBA c)
{
    auto lin = [](uint8_t v) {
        double s = v / 255.0;
        return s <= 0.04045 ? s / 12.92 : std::pow((s + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * lin(c.red) + 0.7152 * lin(c.green) + 0.0722 * lin(c.blue);
}
static double linkContrast(Ubiquitty::ColorRGBA a, Ubiquitty::ColorRGBA b)
{
    double la = linkRelativeLuminance(a), lb = linkRelativeLuminance(b);
    double hi = la > lb ? la : lb, lo = la > lb ? lb : la;
    return (hi + 0.05) / (lo + 0.05);
}

// The theme-derived link color must read on every built-in theme's background
// (WCAG AA large-text floor) and must be one of the theme's two blues unless a
// low-contrast palette forced the legibility blend.
TEST(testLinkColorLegiblePerTheme)
{
    int n = Ubiquitty::Themes::count();
    for (int i = 0; i < n; i++) {
        const Ubiquitty::Themes::Theme* t = Ubiquitty::Themes::get(i);
        if (!t)
            continue;
        Ubiquitty::CellPalette pal = Ubiquitty::CellPalette::fromTheme(*t);
        Ubiquitty::ColorRGBA link = pal.linkColor();
        double c = linkContrast(link, pal.defaultBackground);
        if (c < 3.0) {
            printf("FAIL link color illegible on '%s': contrast %.2f\n", t->name, c);
            failures++;
        }
        // When a theme's own blue already clears the floor, the resolved color
        // must be that blue (no needless blend) — proving the derivation tracks
        // the theme palette rather than inventing a color.
        Ubiquitty::ColorRGBA blue4 = pal.indexed[4], blue12 = pal.indexed[12];
        bool blue4ok = linkContrast(blue4, pal.defaultBackground) >= 3.0;
        bool blue12ok = linkContrast(blue12, pal.defaultBackground) >= 3.0;
        if (blue4ok || blue12ok) {
            bool isBlue4 = colorsEqual(link, blue4);
            bool isBlue12 = colorsEqual(link, blue12);
            if (!isBlue4 && !isBlue12) {
                printf("FAIL link color on '%s' is not a theme blue (%d,%d,%d)\n", t->name, link.red, link.green, link.blue);
                failures++;
            }
        }
    }
}

UBIQUITTY_TEST_MAIN()
