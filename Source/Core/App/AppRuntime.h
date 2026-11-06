// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// App-level orchestration. The macOS frontend is the only consumer today: the
// GTK arm reads its configuration once at startup and has neither a config
// watch nor an appearance follow, which are the two events this drives. The
// seam is written to be portable rather than demonstrated to be -- adopting it
// there is a Linux feature, not a re-wiring.
//
// Owns the POLICY
// (decisions over Preferences + the theme registry); the platform implements
// AppRuntimeHost for the non-portable glue and feeds the runtime platform events.
// Platform-free (check-boundaries.py: "Source/Core/App names no native framework").

#include "Core/Preferences/AppearanceMode.h"

#include <string>

namespace Ubiquitty::App {

/// Who chose a theme, which decides only whether the choice is persisted.
///
/// A manual choice is the user's and outlives the session, so it is written to
/// config.toml with follow-system turned off. An auto-switch choice belongs to
/// the OS appearance and must not be written: persisting it would pin the theme
/// and silently defeat the toggle that produced it.
enum class ThemeSelection : unsigned char {
    Manual,
    AutoSwitch
};

// The platform glue the AppRuntime drives: the frontend implements it, the
// runtime calls it to enact a decision it can't carry out portably.
class AppRuntimeHost {
public:
    virtual ~AppRuntimeHost() = default;

    /// Applies `themeIndex` to every open window and re-derives every surface
    /// that carries a theme colour with it.
    ///
    /// One handler rather than one per surface: an implementation re-derives
    /// everything theme-derived together — renderers, titlebar, tab strip — so
    /// a theme cannot reach the terminal body while some chrome keeps the
    /// colours of the theme before it. Persistence is the runtime's business,
    /// not this handler's.
    ///
    /// - Parameter themeIndex: A registered theme index; always >= 0 here,
    ///   since the runtime rejects an out-of-range index before calling.
    virtual void applyThemeToAllWindows(int themeIndex) = 0;

    // --- Preferences reload (onConfigFileChanged) ---
    /// Re-reads the preferences file and re-registers what it declares.
    ///
    /// The live instance is replaced wholesale and the file's `[[themes]]` and
    /// `[security]` policy re-registered with it; a first run creates the file.
    /// The host implements this because only it knows where the file is and how
    /// its own frontend refreshes.
    virtual void reloadPreferencesFromFile() = 0;
    // Rebuild the menus that mirror the config's [[themes]] / [[profiles]].
    virtual void rebuildConfigMenus() = 0;
    // Push every is-set Preferences field back into the live windows.
    virtual void reapplyPersistedSettings() = 0;
};

class AppRuntime {
public:
    explicit AppRuntime(AppRuntimeHost& host)
        : m_host(host)
    {
    }

    AppRuntime(const AppRuntime&) = delete;
    AppRuntime& operator=(const AppRuntime&) = delete;

    /// Applies a theme, whatever chose it. The single path a theme change takes.
    ///
    /// Every entry point routes here — the palette, the Settings pane, the menu, preferences-vocabulary-ok: the macOS Settings pane, a platform's own window
    /// the OS appearance switch — so none of them can apply a theme differently
    /// from the others. A second path that applies a theme its own way reaches a
    /// different set of surfaces, and whichever it misses keeps the colours of
    /// the theme before it with nothing left to correct them.
    ///
    /// - Parameters:
    ///   - themeIndex: A registered theme index; out-of-range is a no-op.
    ///   - selection: Manual persists the choice, AutoSwitch does not.
    void selectTheme(int themeIndex, ThemeSelection selection);

    // The OS light/dark mode flipped (scheme = the NEW mode). When follow-system
    // is on, resolve the auto-switch theme for the new mode and apply it to
    // every window via the host; otherwise the manual theme stands (no-op).
    void onSystemAppearanceChanged(Preferences::ColorScheme scheme);

    /// The preferences file changed on disk.
    ///
    /// A file holding exactly the app's last own write reloads nothing
    /// (`Preferences::originOfFileChange`); anything else re-parses the file,
    /// rebuilds the config menus and re-applies the persisted settings, in that
    /// order. The platform owns the watch itself and calls this for each event
    /// it raises, however many writes one event covers.
    void onConfigFileChanged();

    // The theme index the auto-switch resolves for `scheme`: a matching-darkness
    // per-mode override → the darkness-appropriate named default → the first
    // built-in of that darkness, or -1. Not gated by follow-system.
    [[nodiscard]] int autoThemeIndex(Preferences::ColorScheme scheme) const;

    // autoThemeIndex gated by follow-system: -1 when follow-system is off (the
    // manual theme owns the window) or no theme matches; else the auto index.
    [[nodiscard]] int themeIndexForSystemAppearance(Preferences::ColorScheme scheme) const;

private:
    AppRuntimeHost& m_host;
};

} // namespace Ubiquitty::App
