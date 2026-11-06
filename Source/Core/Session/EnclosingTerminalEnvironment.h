// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
//
// The variables that describe the terminal, or the multiplexer, a launcher was
// itself started in. A host started from a tmux pane, an app opened from
// another terminal's shell, or a frontend run from an ssh session inside screen
// carries them in its own environment, and every program it launches into a
// pane would otherwise inherit them.
//
// A program reads them to decide what the terminal it runs in can do, and the
// pane is not that terminal. A program finding `TMUX` sends its colors as the
// nearest of the 256 indexed ones and wraps its queries in tmux's passthrough; a kitty, WezTerm, or iTerm2 integration finding its own window
// ID talks to a window the program is not in.
//
// The PTY layer drops these from what a child inherits. A launcher that sets
// one as an override still gives it to the child: the scrub applies only to
// inherited entries. The session's own identity (`TERM`,
// `COLORTERM`, `TERM_PROGRAM`, `TERM_PROGRAM_VERSION`) is replaced rather than
// dropped, the `UBIQUITTY_*` context is the launcher's own, and the rest of
// the inherited environment, `SSH_*` and `TERMINFO_DIRS` included, is kept.

#include <array>
#include <string_view>

namespace Ubiquitty::Session {

/// The exact names an enclosing terminal or multiplexer sets in its children.
///
/// A terminal whose variables share a prefix it owns outright is listed in
/// ``enclosingTerminalEnvironmentPrefixes`` instead. kitty's are named one by
/// one because `KITTY_CONFIG_DIRECTORY` is the person's own setting, not
/// kitty's identity.
inline constexpr std::array<std::string_view, 32> enclosingTerminalEnvironmentNames {
    // Multiplexers.
    "TMUX",
    "TMUX_PANE",
    "STY",
    "ZELLIJ",
    // The enclosing terminal's terminfo, which describes it and not the pane.
    "TERMINFO",
    // Identities several terminals share.
    "TERM_SESSION_ID",
    "TERM_FEATURES",
    "TERMINAL_EMULATOR",
    "LC_TERMINAL",
    "LC_TERMINAL_VERSION",
    "COLORFGBG",
    "WINDOWID",
    // iTerm2.
    "ITERM_SESSION_ID",
    "ITERM_PROFILE",
    // kitty.
    "KITTY_WINDOW_ID",
    "KITTY_PID",
    "KITTY_PUBLIC_KEY",
    "KITTY_LISTEN_ON",
    "KITTY_INSTALLATION_DIR",
    // xterm.
    "XTERM_VERSION",
    "XTERM_SHELL",
    "XTERM_LOCALE",
    // VTE and the terminals built on it.
    "VTE_VERSION",
    "GNOME_TERMINAL_SCREEN",
    "GNOME_TERMINAL_SERVICE",
    "TILIX_ID",
    "TERMINATOR_UUID",
    "TERMINATOR_DBUS_NAME",
    "TERMINATOR_DBUS_PATH",
    // Windows Terminal.
    "WT_SESSION",
    "WT_PROFILE_ID",
    // Konsole, beside its prefixed names.
    "SHELL_SESSION_ID",
};

// An unfilled slot would be an empty name, and an empty name matches nothing a
// launcher meant to drop.
static_assert([] {
    for (const std::string_view name : enclosingTerminalEnvironmentNames) {
        if (name.empty())
            return false;
    }
    return true;
}());

/// The prefixes whose every variable belongs to one enclosing terminal or
/// multiplexer.
inline constexpr std::array<std::string_view, 5> enclosingTerminalEnvironmentPrefixes {
    "ZELLIJ_",
    "WEZTERM_",
    "GHOSTTY_",
    "ALACRITTY_",
    "KONSOLE_",
};

/// Reports whether a `NAME=value` environment entry describes the terminal or
/// multiplexer the launcher was started in.
///
/// Header-defined, as `isContextEnvironmentEntry` is, so the PTY layer can
/// drop these from what a child inherits without linking the session module.
/// The match is on the whole name: `TMUX_PLUGIN_MANAGER_PATH` and
/// `KITTY_CONFIG_DIRECTORY` are the person's own settings and are kept.
///
/// - Parameter entry: One environment entry.
/// - Returns: Whether its name is one of ``enclosingTerminalEnvironmentNames``
///   or begins with one of ``enclosingTerminalEnvironmentPrefixes``.
[[nodiscard]] constexpr bool isEnclosingTerminalEnvironmentEntry(std::string_view entry)
{
    const std::size_t separator = entry.find('=');
    if (separator == std::string_view::npos)
        return false;
    const std::string_view name = entry.substr(0, separator);
    for (const std::string_view candidate : enclosingTerminalEnvironmentNames) {
        if (name == candidate)
            return true;
    }
    for (const std::string_view prefix : enclosingTerminalEnvironmentPrefixes) {
        if (name.size() > prefix.size() && name.starts_with(prefix))
            return true;
    }
    return false;
}

} // namespace Ubiquitty::Session
