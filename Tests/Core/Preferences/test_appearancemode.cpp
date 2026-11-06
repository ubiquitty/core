// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

#include <optional>

// test_appearancemode.cpp — the System / Light / Dark picker tri-state
// (Preferences/AppearanceMode.h::appearanceModeFromValues). Pins:
//
//   followSystem  themeIndexSet  index in range  isDark  ->  mode
//   ------------  -------------  --------------  ------     ------
//   yes           *              *               *      ->  System  (follow wins)
//   no            no             *               *      ->  System  (unset fallback)
//   no            yes            no              *      ->  System  (out-of-range fallback)
//   no            yes            yes             yes    ->  Dark
//   no            yes            yes             no     ->  Light
//
// The enum values match the macOS picker tags (0 System / 1 Light / 2 Dark).
// Header-only template policy, links standalone.

#include "Core/Preferences/AppearanceMode.h"

#include <cstdio>

using Ubiquitty::Preferences::AppearanceMode;
using Ubiquitty::Preferences::appearanceModeFromValues;
using Ubiquitty::Preferences::followSystemAppearanceEffective;

// A four-theme registry: indices 0 (light), 1 (dark), 2 (light), 3 (dark).
static bool isDark4(int index) { return index == 1 || index == 3; }

TEST(appearancemode)
{

    constexpr int themeCount = 4;

    // followSystemAppearanceEffective: unset means on, and a stated value wins.
    // Critical: an UNSET key (set==false) resolves to true so a fresh install
    // auto-tracks the OS dark/light mode — the macOS followSystemOn default.
    CHECK(followSystemAppearanceEffective(std::nullopt) == true);
    CHECK(followSystemAppearanceEffective(std::optional { true }) == true);
    CHECK(followSystemAppearanceEffective(std::optional { false }) == false);

    // Enum values must match the macOS picker tags exactly (the .mm casts to NSInteger).
    CHECK(static_cast<int>(AppearanceMode::System) == 0);
    CHECK(static_cast<int>(AppearanceMode::Light) == 1);
    CHECK(static_cast<int>(AppearanceMode::Dark) == 2);

    // Follow-system on: always System, regardless of the theme fields.
    CHECK(appearanceModeFromValues(/*followSystem=*/true, /*themeIndexSet=*/false,
              /*themeIndex=*/0, themeCount, isDark4)
        == AppearanceMode::System);
    CHECK(appearanceModeFromValues(true, true, 1 /*dark*/, themeCount, isDark4) == AppearanceMode::System);
    CHECK(appearanceModeFromValues(true, true, 0 /*light*/, themeCount, isDark4) == AppearanceMode::System);

    // Follow-system off, no theme index pinned: fall back to System.
    CHECK(appearanceModeFromValues(false, /*themeIndexSet=*/false, 0, themeCount, isDark4) == AppearanceMode::System);

    // Follow-system off, a pinned index of each darkness: darkness decides.
    CHECK(appearanceModeFromValues(false, true, 0 /*light*/, themeCount, isDark4) == AppearanceMode::Light);
    CHECK(appearanceModeFromValues(false, true, 2 /*light*/, themeCount, isDark4) == AppearanceMode::Light);
    CHECK(appearanceModeFromValues(false, true, 1 /*dark*/, themeCount, isDark4) == AppearanceMode::Dark);
    CHECK(appearanceModeFromValues(false, true, 3 /*dark*/, themeCount, isDark4) == AppearanceMode::Dark);

    // Follow-system off, pinned but out of range (stale index after the theme
    // table shrank, or a negative): fall back to System rather than indexing past
    // the end or calling isDark with a bad index.
    CHECK(appearanceModeFromValues(false, true, themeCount /*==4, off the end*/, themeCount, isDark4) == AppearanceMode::System);
    CHECK(appearanceModeFromValues(false, true, 99, themeCount, isDark4) == AppearanceMode::System);
    CHECK(appearanceModeFromValues(false, true, -1, themeCount, isDark4) == AppearanceMode::System);

    // Boundary: the last in-range index is honored (count-1), the first is too.
    CHECK(appearanceModeFromValues(false, true, themeCount - 1 /*==3, dark*/, themeCount, isDark4) == AppearanceMode::Dark);
    CHECK(appearanceModeFromValues(false, true, 0, themeCount, isDark4) == AppearanceMode::Light);

    // Empty registry (no themes): a pinned index is necessarily out of range ->
    // System.
    CHECK(appearanceModeFromValues(false, true, 0, /*themeCount=*/0, isDark4) == AppearanceMode::System);
}

UBIQUITTY_TEST_MAIN()
