// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_themeresolve.cpp — pins the settings-config policies the portable
// Themes headers hold:
//
//   - ::Theme::packedColor(slot)  — pack an ANSI/fg/bg/cursor color into
//     0xRRGGBBAA, returning 0 for an out-of-range slot (Themes/Themes.h).
//   - Ubiquitty::Themes::resolveSystemTheme(...) — the follow-system light/dark
//     index policy: keep the stored index if set/in-range/matching-darkness,
//     else the first theme of that darkness, else 0 (Themes/ThemeResolve.h).
//   - Ubiquitty::Themes::firstThemeOfDarkness(...) / resolveAutoTheme(...) — the
//     live auto-switch policy with the curated named-default tier folded in
//     ("Noir" / "Noir Clair"). The auto-switch and the picker getter both
//     route through resolveAutoTheme so they cannot diverge
//     (Themes/ThemeResolve.h).
//
// Both are pure (no registry/Preferences/platform types), so the test fabricates a
// synthetic theme + a darkness lookup and links standalone; ColorRGBA comes from
// Core/Color.h.

#include "Core/Themes/ThemeResolve.h"
#include "Core/Themes/Themes.h"

#include <cstdio>

// The light/dark axis these resolvers take is the portable ColorScheme enum
// (Preferences/AppearanceMode.h, pulled in via ThemeResolve.h). Light == 0 / `false`,
// Dark == 1 / `true`.
using Ubiquitty::Preferences::ColorScheme;

TEST(testPackedColor)
{
    Ubiquitty::Themes::Theme t {};
    // ansi[0] = pure red, ansi[1] = green w/ explicit alpha, fg/bg/cursor set.
    t.ansi[0] = Ubiquitty::ColorRGBA { 0xFF, 0x00, 0x00, 0xFF };
    t.ansi[1] = Ubiquitty::ColorRGBA { 0x00, 0xFF, 0x00, 0x80 };
    t.ansi[15] = Ubiquitty::ColorRGBA { 0x12, 0x34, 0x56, 0x78 };
    t.defaultForeground = Ubiquitty::ColorRGBA { 0xAA, 0xBB, 0xCC, 0xDD };
    t.defaultBackground = Ubiquitty::ColorRGBA { 0x10, 0x20, 0x30, 0x40 };
    t.cursor = Ubiquitty::ColorRGBA { 0x01, 0x02, 0x03, 0x04 };

    CHECK(t.packedColor(0) == 0xFF0000FFu);
    CHECK(t.packedColor(1) == 0x00FF0080u);
    CHECK(t.packedColor(15) == 0x12345678u);
    CHECK(t.packedColor(Ubiquitty::Themes::Theme::SlotForeground) == 0xAABBCCDDu);
    CHECK(t.packedColor(Ubiquitty::Themes::Theme::SlotBackground) == 0x10203040u);
    CHECK(t.packedColor(Ubiquitty::Themes::Theme::SlotCursor) == 0x01020304u);

    // Named slots are 16/17/18 by contract (and forwarded by the Swift interop).
    CHECK(Ubiquitty::Themes::Theme::SlotForeground == 16);
    CHECK(Ubiquitty::Themes::Theme::SlotBackground == 17);
    CHECK(Ubiquitty::Themes::Theme::SlotCursor == 18);

    // Out-of-range slots → 0 (slot 15 is the last valid ANSI index; 16/17/18
    // are the named slots; everything else returns 0).
    CHECK(t.packedColor(-1) == 0u);
    CHECK(t.packedColor(19) == 0u);
    CHECK(t.packedColor(1000) == 0u);

    // A value-initialized ColorRGBA has rgb 0 but alpha 255 (its default member
    // initializer), so an unset palette slot packs to 0x000000FF, not 0. (Only
    // an *out-of-range slot* yields a true 0 — that's the contract above.)
    Ubiquitty::Themes::Theme z {};
    CHECK(z.packedColor(5) == 0x000000FFu);
}

// A tiny darkness table for resolveSystemTheme: index i is dark iff darks[i].
// themeCount is the array length the policy is told about.
TEST(testResolveSystemTheme)
{
    using Ubiquitty::Themes::resolveSystemTheme;

    // 4 themes: [light, dark, light, dark]
    const bool darks[] = { false, true, false, true };
    const int n = 4;
    auto isDark = [&](int i) { return darks[i]; };

    // Stored index set, in range, matching darkness → kept.
    CHECK(resolveSystemTheme(ColorScheme::Dark, n, isDark, /*set*/ true, /*idx*/ 3) == 3);
    CHECK(resolveSystemTheme(ColorScheme::Light, n, isDark, true, 2) == 2);

    // Stored index set but WRONG darkness → first-of-darkness fallback.
    CHECK(resolveSystemTheme(ColorScheme::Dark, n, isDark, true, /*idx*/ 0) == 1); // first dark
    CHECK(resolveSystemTheme(ColorScheme::Light, n, isDark, true, /*idx*/ 1) == 0); // first light

    // Stored index out of range → first-of-darkness fallback.
    CHECK(resolveSystemTheme(ColorScheme::Dark, n, isDark, true, 99) == 1);
    CHECK(resolveSystemTheme(ColorScheme::Light, n, isDark, true, -5) == 0);

    // Not set → first-of-darkness.
    CHECK(resolveSystemTheme(ColorScheme::Dark, n, isDark, false, 3) == 1);
    CHECK(resolveSystemTheme(ColorScheme::Light, n, isDark, false, 2) == 0);

    // No theme of the wanted darkness anywhere → 0.
    const bool allLight[] = { false, false, false };
    auto allLightFn = [&](int i) { return allLight[i]; };
    CHECK(resolveSystemTheme(ColorScheme::Dark, 3, allLightFn, false, 0) == 0);
    CHECK(resolveSystemTheme(ColorScheme::Dark, 3, allLightFn, true, 2) == 0);

    // Empty table → 0.
    CHECK(resolveSystemTheme(ColorScheme::Dark, 0, isDark, true, 0) == 0);
    CHECK(resolveSystemTheme(ColorScheme::Light, 0, isDark, false, 0) == 0);
}

// firstThemeOfDarkness — the shared first-of-darkness scan both resolvers reuse.
TEST(testFirstThemeOfDarkness)
{
    using Ubiquitty::Themes::firstThemeOfDarkness;
    const bool darks[] = { false, false, true, true };
    auto isDark = [&](int i) { return darks[i]; };

    CHECK(firstThemeOfDarkness(ColorScheme::Dark, 4, isDark) == 2);
    CHECK(firstThemeOfDarkness(ColorScheme::Light, 4, isDark) == 0);

    // No theme of the wanted darkness → -1 (the policy distinguishes this from
    // index 0; resolveSystemTheme then substitutes 0, resolveAutoTheme keeps -1).
    const bool allDark[] = { true, true };
    auto allDarkFn = [&](int i) { return allDark[i]; };
    CHECK(firstThemeOfDarkness(ColorScheme::Light, 2, allDarkFn) == -1);
    CHECK(firstThemeOfDarkness(ColorScheme::Dark, 0, isDark) == -1);
}

// resolveAutoTheme — the live auto-switch policy with the curated named-default
// tier folded between the per-mode override and the first-of-darkness fallback.
// This is the tier the macOS auto-switch had and the picker getter lacked; both
// now route here, so the test pins all four tiers.
TEST(testResolveAutoTheme)
{
    using Ubiquitty::Themes::resolveAutoTheme;

    // 4 themes: [light0, dark1, light2, dark3]. Pretend the curated default for
    // dark is index 3 and for light is index 2 (a name lookup the caller does).
    const bool darks[] = { false, true, false, true };
    const int n = 4;
    auto isDark = [&](int i) { return darks[i]; };
    auto darkDefault = [] { return 3; };
    auto lightDefault = [] { return 2; };
    auto noDefault = [] { return -1; };

    // Tier 1: stored override set, in range, matching darkness → kept (wins over
    // the named default).
    CHECK(resolveAutoTheme(ColorScheme::Dark, n, isDark, /*set*/ true, /*idx*/ 1, darkDefault) == 1);
    CHECK(resolveAutoTheme(ColorScheme::Light, n, isDark, true, 0, lightDefault) == 0);

    // Tier 2: no usable override → the named default, when it matches darkness.
    CHECK(resolveAutoTheme(ColorScheme::Dark, n, isDark, /*set*/ false, 0, darkDefault) == 3);
    CHECK(resolveAutoTheme(ColorScheme::Light, n, isDark, false, 0, lightDefault) == 2);

    // Override set but WRONG darkness → falls through to the named default.
    CHECK(resolveAutoTheme(ColorScheme::Dark, n, isDark, true, /*idx*/ 0, darkDefault) == 3); // idx 0 is light, default dark 3
    // Override out of range → named default.
    CHECK(resolveAutoTheme(ColorScheme::Dark, n, isDark, true, 99, darkDefault) == 3);

    // Tier 2 guard: a named default of the WRONG darkness is rejected; falls to
    // tier 3 (first of darkness). darkDefault() = 3 is dark, so for a light
    // request it is rejected → first light = 0.
    CHECK(resolveAutoTheme(ColorScheme::Light, n, isDark, false, 0, darkDefault) == 0);
    // A named default out of range is rejected too → first of darkness.
    auto oobDefault = [] { return 999; };
    CHECK(resolveAutoTheme(ColorScheme::Dark, n, isDark, false, 0, oobDefault) == 1);

    // Tier 3: no override, no named default (-1) → first theme of that darkness.
    CHECK(resolveAutoTheme(ColorScheme::Dark, n, isDark, false, 0, noDefault) == 1);
    CHECK(resolveAutoTheme(ColorScheme::Light, n, isDark, false, 0, noDefault) == 0);

    // Tier 4: no theme of the wanted darkness anywhere → -1 (mirrors the macOS
    // auto-switch's firstThemeMatchingScheme == -1 edge; this is the value the
    // picker getter now also returns, so the two agree).
    const bool allLight[] = { false, false, false };
    auto allLightFn = [&](int i) { return allLight[i]; };
    CHECK(resolveAutoTheme(ColorScheme::Dark, 3, allLightFn, false, 0, noDefault) == -1);
    CHECK(resolveAutoTheme(ColorScheme::Dark, 0, isDark, true, 0, darkDefault) == -1);
}

UBIQUITTY_TEST_MAIN()
