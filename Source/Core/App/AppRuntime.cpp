// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Source/Core/App/AppRuntime.cpp — see Core/App/AppRuntime.h.

#include "Core/App/AppRuntime.h"

#include "Core/Preferences/Configuration.h"
#include "Core/Preferences/EffectiveValue.h"
#include "Core/Preferences/Preferences.h"
#include "Core/Preferences/SelfWriteGuard.h"
#include "Core/Themes/ThemeApply.h"
#include "Core/Themes/Themes.h"

#include <optional>
#include <string>
#include <utility>

namespace Ubiquitty::App {

// Honor a matching-darkness per-mode override; else the named default
// ("Noir" / "Noir Clair"); else the first built-in of that darkness.
// Policy lives in Themes/ThemeResolve.h (resolveAutoTheme).
int AppRuntime::autoThemeIndex(Preferences::ColorScheme scheme) const
{
    const Themes::AutomaticAppearance appearance = scheme == Preferences::ColorScheme::Dark ? Themes::AutomaticAppearance::Dark : Themes::AutomaticAppearance::Light;
    return Ubiquitty::Themes::resolvedAutomaticThemeIndex(appearance);
}

int AppRuntime::themeIndexForSystemAppearance(Preferences::ColorScheme scheme) const
{
    const Preferences::Values& c = Preferences::values();
    if (!Preferences::followSystemAppearanceEffective(c.followSystemAppearance))
        return -1;
    return autoThemeIndex(scheme);
}

void AppRuntime::selectTheme(int themeIndex, ThemeSelection selection)
{
    if (themeIndex < 0 || themeIndex >= Ubiquitty::Themes::count())
        return;

    if (selection == ThemeSelection::Manual) {
        const Ubiquitty::Themes::Theme* theme = Ubiquitty::Themes::get(themeIndex);
        if (!theme || !theme->name)
            return;
        // The configuration moves before the windows do, so anything reading it
        // while the theme is being applied sees the selection rather than the
        // theme it replaced. One document edit — the name, and follow-system
        // off — configured and stored as the app's own write.
        std::string document = Preferences::liveDocument();
        for (const auto& [key, value] : { std::pair { Preferences::Key::AppearanceFollowSystem, Preferences::Value::ofBool(Preferences::SerializeKind::Bool, false) },
                 std::pair { Preferences::Key::AppearanceTheme, Preferences::Value::ofText(Preferences::SerializeKind::String, theme->name) } }) {
            if (std::optional<std::string> written = Preferences::writeValueIntoBody(key, value, document))
                document = std::move(*written);
        }
        (void)Preferences::configureOwnDocument(document);
    }

    m_host.applyThemeToAllWindows(themeIndex);
}

void AppRuntime::onSystemAppearanceChanged(Preferences::ColorScheme scheme)
{
    const int index = themeIndexForSystemAppearance(scheme);
    if (index >= 0)
        selectTheme(index, ThemeSelection::AutoSwitch);
}

void AppRuntime::onConfigFileChanged()
{
    if (Preferences::originOfFileChange() == Preferences::ChangeOrigin::OwnWrite)
        return;
    m_host.reloadPreferencesFromFile();
    m_host.rebuildConfigMenus();
    m_host.reapplyPersistedSettings();
}

} // namespace Ubiquitty::App
