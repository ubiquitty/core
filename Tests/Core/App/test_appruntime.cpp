// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// Core/App/AppRuntime.{h,cpp} — the portable app-level theme/appearance + config-reload
// orchestration a frontend's app delegate drives. The point of this test is
// the LINK as much as the logic: AppRuntime.cpp must compile + link under the
// non-Apple host toolchain with no framework (it is platform-free per
// check-boundaries.py "Source/Core/App names no native framework"). The host glue is a
// pure-virtual AppRuntimeHost, so a stub satisfies the seam here.
//
// Logic pins: autoThemeIndex follows the resolveAutoTheme policy (override of
// matching darkness → named default → first of that darkness); the follow-system
// gate returns -1 when off.

#include "Core/App/AppRuntime.h"
#include "Core/Preferences/Configuration.h"
#include "Core/Preferences/Preferences.h"
#include "Core/Preferences/SelfWriteGuard.h"
#include "Core/Themes/Themes.h"
#include "PAL/PAL.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <unistd.h>

using Ubiquitty::App::AppRuntime;
using Ubiquitty::App::AppRuntimeHost;
using Ubiquitty::Preferences::ColorScheme;

// Stub host: records what the runtime asked of it — the applied theme — so a
// caller's route through selectTheme is observable; the config-reload glue is
// no-op (the runtime owns only the order).
struct StubHost final : AppRuntimeHost {
    int lastApplied = -999;
    int applyCount = 0;
    int reloads = 0;
    int menuRebuilds = 0;
    int reapplies = 0;

    void applyThemeToAllWindows(int themeIndex) override
    {
        lastApplied = themeIndex;
        applyCount++;
    }
    void reloadPreferencesFromFile() override { reloads++; }
    void rebuildConfigMenus() override { menuRebuilds++; }
    void reapplyPersistedSettings() override { reapplies++; }
};

// Points the preferences directory at a fresh temporary one for one block and
// puts `XDG_CONFIG_HOME` back afterwards, unset and empty kept apart.
class TemporaryPreferencesHome {
public:
    TemporaryPreferencesHome()
    {
        if (const char* value = std::getenv("XDG_CONFIG_HOME"))
            m_saved = value;
        char pattern[] = "/tmp/ubq-appruntime-XXXXXX";
        if (const char* made = ::mkdtemp(pattern))
            m_directory = made;
        ::setenv("XDG_CONFIG_HOME", m_directory.c_str(), 1);
    }
    ~TemporaryPreferencesHome()
    {
        if (m_saved)
            ::setenv("XDG_CONFIG_HOME", m_saved->c_str(), 1);
        else
            ::unsetenv("XDG_CONFIG_HOME");
        ::unlink(filePath().c_str());
        ::rmdir((m_directory + "/ubiquitty").c_str());
        ::rmdir(m_directory.c_str());
    }
    TemporaryPreferencesHome(const TemporaryPreferencesHome&) = delete;
    TemporaryPreferencesHome& operator=(const TemporaryPreferencesHome&) = delete;

    [[nodiscard]] std::string filePath() const { return m_directory + "/ubiquitty/config.toml"; }

private:
    std::string m_directory;
    std::optional<std::string> m_saved;
};

// Configures Core from `document`, as a load or a settings row would.
static void configureDocument(const std::string& document)
{
    (void)Ubiquitty::Preferences::configure(document, Ubiquitty::Preferences::ParseMode::Strict);
}

enum class Appearance : bool {
    Fixed,
    FollowsSystem
};

// The `[appearance]` lines naming the per-mode overrides and the follow switch.
static std::string appearanceDocument(Appearance appearance, const std::string& darkTheme = std::string())
{
    std::string document = "[appearance]\nfollow_system = " + std::string(appearance == Appearance::FollowsSystem ? "true" : "false") + "\n";
    if (!darkTheme.empty())
        document += "system_dark_theme = \"" + darkTheme + "\"\n";
    return document;
}

// First built-in theme index of the requested darkness — the resolveAutoTheme
// tail the runtime falls through to when no override / named default matches.
static int firstOfDarkness(bool dark)
{
    for (int i = 0; i < Ubiquitty::Themes::count(); ++i)
        if (Ubiquitty::Themes::isDark(i) == dark)
            return i;
    return -1;
}

// The NAME of the theme at `index` — the per-mode override now persists by name.
static std::string themeNameAt(int index)
{
    const Ubiquitty::Themes::Theme* t = Ubiquitty::Themes::get(index);
    return (t && t->name) ? std::string(t->name) : std::string();
}

TEST(appruntime)
{

    StubHost host;
    AppRuntime runtime(host);

    // --- autoThemeIndex: no override, no follow-system relevance (it is ungated) ---
    // With no per-mode override set, the resolution lands on the named default
    // ("Noir" / "Noir Clair") when present, else the first of that darkness.
    configureDocument("");
    {
        const int dark = runtime.autoThemeIndex(ColorScheme::Dark);
        const int light = runtime.autoThemeIndex(ColorScheme::Light);
        const int noir = Ubiquitty::Themes::indexForName("Noir");
        const int noirClair = Ubiquitty::Themes::indexForName("Noir Clair");
        CHECK(dark >= 0);
        CHECK(light >= 0);
        CHECK(Ubiquitty::Themes::isDark(dark));
        CHECK(!Ubiquitty::Themes::isDark(light));
        CHECK(dark == (noir >= 0 ? noir : firstOfDarkness(true)));
        CHECK(light == (noirClair >= 0 ? noirClair : firstOfDarkness(false)));
    }

    // --- autoThemeIndex: a configured override of MATCHING darkness wins ---
    {
        const int someDark = firstOfDarkness(true);
        CHECK(someDark >= 0);
        configureDocument(appearanceDocument(Appearance::Fixed, themeNameAt(someDark)));
        CHECK(runtime.autoThemeIndex(ColorScheme::Dark) == someDark);
    }

    // --- autoThemeIndex: an override of the WRONG darkness is ignored ---
    {
        const int someLight = firstOfDarkness(false);
        CHECK(someLight >= 0);
        configureDocument(appearanceDocument(Appearance::Fixed, themeNameAt(someLight))); // a light theme stored for dark mode
        const int resolved = runtime.autoThemeIndex(ColorScheme::Dark);
        CHECK(resolved != someLight);
        CHECK(Ubiquitty::Themes::isDark(resolved)); // falls through to a dark theme
    }

    // --- themeIndexForSystemAppearance: gated by follow-system ---
    {
        configureDocument(appearanceDocument(Appearance::Fixed));
        CHECK(runtime.themeIndexForSystemAppearance(ColorScheme::Dark) == -1);

        configureDocument(appearanceDocument(Appearance::FollowsSystem));
        const int gated = runtime.themeIndexForSystemAppearance(ColorScheme::Dark);
        CHECK(gated == runtime.autoThemeIndex(ColorScheme::Dark));
        CHECK(gated >= 0);
    }

    // --- onSystemAppearanceChanged applies through the host when follow-system on ---
    {
        configureDocument(appearanceDocument(Appearance::FollowsSystem));
        host.lastApplied = -999;
        runtime.onSystemAppearanceChanged(ColorScheme::Light);
        CHECK(host.lastApplied == runtime.autoThemeIndex(ColorScheme::Light));
        CHECK(host.lastApplied >= 0);

        // follow-system off: no apply (manual theme stands).
        configureDocument(appearanceDocument(Appearance::Fixed));
        host.lastApplied = -999;
        runtime.onSystemAppearanceChanged(ColorScheme::Dark);
        CHECK(host.lastApplied == -999);
    }

    // --- onConfigFileChanged: a file holding the app's own write reloads nothing ---
    {
        TemporaryPreferencesHome home;
        host.reloads = host.menuRebuilds = host.reapplies = 0;
        CHECK(Ubiquitty::Preferences::configureOwnSectionKey("mouse", "scroll_bounce", "true"));
        runtime.onConfigFileChanged();
        runtime.onConfigFileChanged();
        CHECK_MSG(host.reloads == 0 && host.menuRebuilds == 0 && host.reapplies == 0,
            "every event over the app's own bytes, however many one write raised, reloads nothing");

        // A foreign write runs reload → rebuild → reapply, in that order.
        const std::string_view foreign = "[mouse]\nscroll_bounce = false\n";
        CHECK(Ubiquitty::PAL::storageWriteAtomic(home.filePath().c_str(), std::as_bytes(std::span { foreign })));
        runtime.onConfigFileChanged();
        CHECK(host.reloads == 1 && host.menuRebuilds == 1 && host.reapplies == 1);
    }
}

// selectTheme is the single path every theme change takes. Pinned here: both
// selections reach the one apply handler and differ only in whether the choice
// is persisted — two callers doing different amounts of work for the same user
// action is how a theme reaches some surfaces and not others.
TEST(appruntime_select_theme_is_the_single_path)
{
    StubHost host;
    AppRuntime runtime(host);
    // A manual selection is stored, so it lands in a scratch home rather than
    // in the configuration of whoever runs the suite.
    TemporaryPreferencesHome home;

    const int target = firstOfDarkness(true);
    CHECK(target >= 0);

    // --- Manual: applies and persists, and turns follow-system off ---
    configureDocument(appearanceDocument(Appearance::FollowsSystem));
    runtime.selectTheme(target, Ubiquitty::App::ThemeSelection::Manual);
    CHECK(host.applyCount == 1);
    CHECK(host.lastApplied == target);
    // The configuration moves with the selection, so a reader between here and
    // the next reload sees the choice rather than the theme it replaced, and
    // the stored document is the live one.
    const Ubiquitty::Preferences::Values& live = Ubiquitty::Preferences::values();
    CHECK(live.themeName == themeNameAt(target));
    CHECK(live.followSystemAppearance == false); // set, and set to off
    const std::string stored = Ubiquitty::Preferences::liveDocument();
    CHECK_MSG(stored.find("theme = \"" + themeNameAt(target) + "\"") != std::string::npos, stored.c_str());
    std::ifstream file(home.filePath());
    std::ostringstream fileText;
    fileText << file.rdbuf();
    CHECK_MSG(fileText.str() == stored, "the selection is persisted as the live document");

    // --- AutoSwitch: applies but must not persist ---
    // Writing an auto-switch pick would pin the theme and silently defeat the
    // follow-system toggle that produced it.
    const int other = firstOfDarkness(false);
    CHECK(other >= 0);
    runtime.selectTheme(other, Ubiquitty::App::ThemeSelection::AutoSwitch);
    CHECK(host.applyCount == 2);
    CHECK(host.lastApplied == other);
    CHECK(Ubiquitty::Preferences::liveDocument() == stored); // nothing written
    CHECK(Ubiquitty::Preferences::values().themeName == themeNameAt(target)); // the manual choice still stands

    // --- Out of range reaches neither handler ---
    runtime.selectTheme(-1, Ubiquitty::App::ThemeSelection::Manual);
    runtime.selectTheme(Ubiquitty::Themes::count(), Ubiquitty::App::ThemeSelection::Manual);
    CHECK(host.applyCount == 2);
    CHECK(Ubiquitty::Preferences::liveDocument() == stored);
}

// The OS appearance switch is not a special case: it routes through the same
// selectTheme, as an unpersisted selection.
TEST(appruntime_system_appearance_routes_through_select_theme)
{
    StubHost host;
    AppRuntime runtime(host);
    configureDocument(appearanceDocument(Appearance::FollowsSystem));
    const std::string before = Ubiquitty::Preferences::liveDocument();

    runtime.onSystemAppearanceChanged(ColorScheme::Dark);
    CHECK(host.applyCount == 1);
    CHECK(host.lastApplied == runtime.autoThemeIndex(ColorScheme::Dark));
    CHECK(Ubiquitty::Preferences::liveDocument() == before); // an automatic pick is not stored

    // Follow-system off: the manual theme owns the window, so nothing is applied.
    configureDocument(appearanceDocument(Appearance::Fixed));
    runtime.onSystemAppearanceChanged(ColorScheme::Light);
    CHECK(host.applyCount == 1);
}

TEST(adaptiveAutomaticThemeRemainsSelectedInBothAppearances)
{
    StubHost host;
    AppRuntime runtime(host);
    configureDocument("[appearance]\nfollow_system = true\nsystem_light_theme = \"Adaptive\"\nsystem_dark_theme = \"Adaptive\"\n");
    Ubiquitty::Themes::Theme theme = *Ubiquitty::Themes::builtInGet(0);
    theme.name = "Adaptive";
    theme.usesSystemAppearance = true;
    const int index = Ubiquitty::Themes::registerCustom(theme);
    for (const bool isDark : { false, true }) {
        theme.isDark = isDark;
        CHECK(Ubiquitty::Themes::replaceCustom(index, theme));
        CHECK(runtime.autoThemeIndex(ColorScheme::Light) == index);
        CHECK(runtime.autoThemeIndex(ColorScheme::Dark) == index);
        runtime.onSystemAppearanceChanged(isDark ? ColorScheme::Dark : ColorScheme::Light);
        CHECK(host.lastApplied == index);
    }
    theme.usesSystemAppearance = false;
    CHECK(Ubiquitty::Themes::replaceCustom(index, theme));
    CHECK(runtime.autoThemeIndex(ColorScheme::Light) != index);
    CHECK(runtime.autoThemeIndex(ColorScheme::Dark) == index);
    Ubiquitty::Themes::clearCustom();
}

UBIQUITTY_TEST_MAIN()
