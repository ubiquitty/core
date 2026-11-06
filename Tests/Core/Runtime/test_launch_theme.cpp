// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_launch_theme — the theme a launched window opens on.
//
// Two functions that look like one and are not. `configuredThemeIndex` reports
// only what the user wrote, so it answers -1 for a config with no [appearance]
// theme; `launchThemeIndex` continues down to the house default and always
// answers a theme. A frontend that improvises the second one lands somewhere
// its siblings do not — with no fallback a window opens on the palette a fresh
// Terminal is constructed with — so the cases below are written from the config
// alone, never from what an arm happens to do.
//
// The -1 case is a control on the split, not an afterthought: the headless
// render paths seed a terminal through `seedConfiguredTheme`, which applies a
// theme only when one is configured. Folding the ladder into that would repaint
// every snapshot golden, so a test that only checked `launchThemeIndex` would
// pass equally well after the mistake.

#include "Harness.h"

#include "Core/Preferences/Preferences.h"
#include "Core/Runtime/TerminalLaunchSeed.h"
#include "Core/Themes/Themes.h"

#include <cstring>
#include <string>

namespace {

namespace Rt = Ubiquitty::Runtime;
namespace Cfg = Ubiquitty::Preferences;
namespace Th = Ubiquitty::Themes;

Cfg::Values namingTheme(const char* name)
{
    Cfg::Values config;
    config.themeName = name;
    return config;
}

bool isRegistered(int index) { return index >= 0 && index < Th::count(); }

} // namespace

// The curated pair is spelled as a name, and a name only works while the registry
// carries it. Renaming a built-in theme without updating the pair would leave
// every fallback resolving -1 and each caller improvising its own. Checked here
// rather than trusted.
TEST(testTheCuratedDefaultsResolve)
{
    CHECK(isRegistered(Th::curatedDefaultIndex(Cfg::ColorScheme::Dark)));
    CHECK(isRegistered(Th::curatedDefaultIndex(Cfg::ColorScheme::Light)));

    const Th::Theme* dark = Th::get(Th::curatedDefaultIndex(Cfg::ColorScheme::Dark));
    const Th::Theme* light = Th::get(Th::curatedDefaultIndex(Cfg::ColorScheme::Light));
    CHECK(dark != nullptr && light != nullptr);
    // Named for an appearance, so each must actually be of it. A pair that
    // resolved but pointed both ways at the same darkness would still pass a
    // bare in-range check.
    if (dark && light) {
        CHECK(dark->isDark);
        CHECK(!light->isDark);
    }
    CHECK(std::strcmp(Th::curatedDefaultName(Cfg::ColorScheme::Dark), Th::curatedDefaultName(Cfg::ColorScheme::Light)) != 0);
}

// The ladder has three rungs and the shipping registry collapses the lower two:
// the house default leads builtin-themes.toml, so `curatedDefaultIndex(Dark)`
// and the "first theme there is" last resort both answer 0. Nothing below can
// tell them apart — replacing the curated rung with -1 leaves every case green,
// which was checked rather than assumed.
//
// Pinned here so the coincidence is a tripwire instead of a silent hole: if the
// house default stops leading the table, this fails, and the cases below start
// testing the curated rung for real.
TEST(testTheHouseDefaultLeadsTheRegistry)
{
    CHECK(Th::curatedDefaultIndex(Cfg::ColorScheme::Dark) == 0);
}

TEST(testAConfiguredNameIsTheLaunchTheme)
{
    const int noir = Th::indexForName("Noir");
    CHECK(isRegistered(noir));
    const Th::Theme* other = nullptr;
    int otherIndex = -1;
    for (int i = 0; i < Th::count(); ++i) {
        if (i == noir)
            continue;
        other = Th::get(i);
        otherIndex = i;
        break;
    }
    CHECK(other != nullptr);
    if (!other)
        return;

    // A theme other than the fallback, so a ladder that ignored the config and
    // answered the house default every time would fail rather than coincide.
    const Cfg::Values config = namingTheme(other->name);
    CHECK(Rt::launchThemeIndex(config) == otherIndex);
    CHECK(Rt::configuredThemeIndex(config) == otherIndex);
}

TEST(testNoConfiguredThemeStillOpensOnATheme)
{
    const Cfg::Values config; // no [appearance] theme, the common case

    CHECK(Rt::launchThemeIndex(config) == Th::curatedDefaultIndex(Cfg::ColorScheme::Dark));

    // The control. `seedConfiguredTheme` applies nothing when this is -1, which
    // is what keeps a headless render on the palette its config asked for.
    CHECK(Rt::configuredThemeIndex(config) == -1);
}

TEST(testTheSeedReportsWhetherTheConfiguredThemeIsOnTheTerminal)
{
    // One verdict for every headless driver: no name is a clean answer and
    // leaves the constructed palette alone, a known name applies, an unknown
    // name is refused — so no arm improvises a theme of its own for the
    // first case or renders silently through the third.
    Ubiquitty::Terminal term;
    Ubiquitty::hardReset(term);
    const auto constructed = term.palette.defaultBackground;
    Cfg::Values config;
    CHECK(Rt::seedConfiguredTheme(term, config));
    CHECK(term.palette.defaultBackground.red == constructed.red
        && term.palette.defaultBackground.green == constructed.green
        && term.palette.defaultBackground.blue == constructed.blue);

    config.themeName = "no theme is called this";
    CHECK(!Rt::seedConfiguredTheme(term, config));

    config.themeName = Th::get(0)->name;
    CHECK(Rt::seedConfiguredTheme(term, config));
}

TEST(testAnUnknownNameFallsToTheHouseDefault)
{
    const Cfg::Values config = namingTheme("a theme nobody registered");

    CHECK(Rt::configuredThemeIndex(config) == -1);
    CHECK(Rt::launchThemeIndex(config) == Th::curatedDefaultIndex(Cfg::ColorScheme::Dark));
}

// An empty string is not a name. Values carries `themeNameSet` separately, so a
// config that set the key to "" reaches here with the flag on and the value
// blank — and `indexForName("")` is a lookup, not a refusal.
TEST(testAnEmptyNameIsNotAName)
{
    const Cfg::Values config = namingTheme("");

    CHECK(Rt::configuredThemeIndex(config) == -1);
    CHECK(Rt::launchThemeIndex(config) == Th::curatedDefaultIndex(Cfg::ColorScheme::Dark));
}

// The launch rung does not follow the OS appearance: it is the theme a window
// opens on for a user who configured none, including one who turned
// follow-system off. Which appearance is in force is
// `Themes::resolveAutoTheme`'s question, and answering it here would re-add the
// per-arm divergence in a new place.
TEST(testTheLaunchRungIsNotTheAppearanceSwitch)
{
    Cfg::Values config;
    config.followSystemAppearance = false;
    CHECK(Rt::launchThemeIndex(config) == Th::curatedDefaultIndex(Cfg::ColorScheme::Dark));

    config.followSystemAppearance = true;
    CHECK(Rt::launchThemeIndex(config) == Th::curatedDefaultIndex(Cfg::ColorScheme::Dark));
}

UBIQUITTY_TEST_MAIN()
