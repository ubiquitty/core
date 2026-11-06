// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// LaunchTheme.h — the theme a person's configuration opens in.
//
// A window that launches and a reader that is no terminal at all, such as a
// list of a host's sessions, ask the same question: knowing nothing but the
// local configuration, which theme? One ladder answers both, so a list is
// drawn in the inks of the terminal that opens beside it.

#include "Core/Preferences/Preferences.h"
#include "Core/Themes/Themes.h"

namespace Ubiquitty::Runtime {

/// Resolves the config's [appearance] theme to a theme-registry index.
///
/// Built-ins first, then user `[[themes]]` — which resolve only after
/// `registerThemesFromValues` ran.
///
/// - Parameter config: The resolved config naming the theme, if any.
/// - Returns: The registry index, or -1 when none is configured or the name
///   does not resolve.
[[nodiscard]] inline int configuredThemeIndex(const Preferences::Values& config)
{
    if (!config.themeName.value_or("").empty())
        return Themes::indexForName(config.themeName->c_str());
    return -1;
}

/// The theme a launched window opens under, whether config names one or not.
///
/// The configured name when it resolves, else the curated house default, else
/// whatever theme the registry holds first. Distinct from
/// ``configuredThemeIndex``, which reports only what the user wrote and so
/// answers -1 for the common case of a config with no `[appearance] theme` —
/// and distinct again from ``Themes::resolveAutoTheme``, which is driven by the
/// OS appearance rather than by the launch.
///
/// One ladder for every arm, because a frontend improvising its own lands
/// somewhere different: one that stops at the configured name opens its grid on
/// the palette a fresh Terminal is constructed with and its chrome on whatever
/// the toolkit defaults to, while one carrying a fallback opens on the house
/// theme. The configuration that exposes it is the commonest there is — a
/// config with no `[appearance] theme` at all — so the arms disagree on the
/// screen the product opens in for a user who configured nothing.
///
/// This is the lowest rung of the theme ladder, not the authority: it answers
/// what a pane opens on knowing nothing but the local config. A pane is free to
/// be re-themed above it — by a per-pane, per-tab or per-window override, or by
/// a session that carries a theme of its own from the host it attached to, the
/// way a tmux session does. Anything of that kind resolves later and applies
/// over this; a caller that re-asserts the launch theme afterwards would undo
/// it, so seed with this once at launch and let the specific rungs win.
///
/// - Parameter config: The resolved configuration.
/// - Returns: A live theme index, or -1 when the registry holds nothing.
[[nodiscard]] inline int launchThemeIndex(const Preferences::Values& config)
{
    const int configured = configuredThemeIndex(config);
    if (configured >= 0 && configured < Themes::count())
        return configured;
    // Dark unconditionally: this is the launch fallback, not the appearance
    // switch. Which appearance the OS is in belongs to
    // ``Themes::resolveAutoTheme``, and a window that consulted it here would
    // follow the system even for a user who turned following off.
    const int curated = Themes::curatedDefaultIndex(Preferences::ColorScheme::Dark);
    if (curated >= 0 && curated < Themes::count())
        return curated;
    return Themes::count() > 0 ? 0 : -1;
}

} // namespace Ubiquitty::Runtime
