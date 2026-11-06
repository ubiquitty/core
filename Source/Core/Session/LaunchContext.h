// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
//
// The context every program a host or an app launches into a pane receives in
// its environment: the socket that serves the pane, and the session, window,
// tab, and pane it runs in, each written as a typed identity (`pane:12`).
//
// A tool run inside a pane reads these to address `here` without asking anyone
// which pane it is in. They are identifiers, not credentials: the socket still
// authenticates every request, so a forged value can only name an object the
// caller could already have named by its ID.
//
// A launcher sets every name it knows and the PTY layer drops any the child
// would otherwise inherit, so a stale context from an enclosing pane never
// reaches a program in another one.

#include "Core/Session/Identity.h"

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Ubiquitty::Session {

/// The variable naming the socket that serves the pane.
inline constexpr std::string_view socketEnvironmentName = "UBIQUITTY_SOCKET";
/// The variable naming the host session the pane is backed by.
inline constexpr std::string_view sessionEnvironmentName = "UBIQUITTY_SESSION";
/// The variable naming the window the pane was opened in.
inline constexpr std::string_view windowEnvironmentName = "UBIQUITTY_WINDOW";
/// The variable naming the tab the pane was opened in.
inline constexpr std::string_view tabEnvironmentName = "UBIQUITTY_TAB";
/// The variable naming the pane itself.
inline constexpr std::string_view paneEnvironmentName = "UBIQUITTY_PANE";

/// Every context variable, socket first.
inline constexpr std::array<std::string_view, 5> contextEnvironmentNames {
    socketEnvironmentName,
    sessionEnvironmentName,
    windowEnvironmentName,
    tabEnvironmentName,
    paneEnvironmentName,
};

/// Reports whether a `NAME=value` environment entry sets a context variable.
///
/// Header-defined so the PTY layer can strip inherited context without linking
/// anything else of the session module.
///
/// - Parameter entry: One environment entry.
/// - Returns: Whether its name is one of ``contextEnvironmentNames``.
[[nodiscard]] constexpr bool isContextEnvironmentEntry(std::string_view entry)
{
    for (const std::string_view name : contextEnvironmentNames) {
        if (entry.size() > name.size() && entry.starts_with(name) && entry[name.size()] == '=')
            return true;
    }
    return false;
}

/// The variable that hands `ubq` a token for this machine's daemon.
inline constexpr std::string_view agentTokenEnvironmentName = "UBIQUITTY_AGENT_TOKEN";
/// The variable that hands `ubq -H` a token another host's daemon issued.
inline constexpr std::string_view remoteTokenEnvironmentName = "UBQ_REMOTE_TOKEN";

/// Every variable that carries a credential.
///
/// A credential is given to the one process it was handed to. A daemon that
/// process starts, the shells that daemon runs, and the ssh it runs to reach
/// another host never inherit one: a shell's environment is readable by
/// everything it runs, and ssh forwards whatever `SendEnv` names.
inline constexpr std::array<std::string_view, 2> credentialEnvironmentNames {
    agentTokenEnvironmentName,
    remoteTokenEnvironmentName,
};

/// Reports whether a `NAME=value` environment entry sets a credential.
///
/// Header-defined, as ``isContextEnvironmentEntry`` is, so a launcher strips
/// credentials without linking the session module.
///
/// - Parameter entry: One environment entry.
/// - Returns: Whether its name is one of ``credentialEnvironmentNames``.
[[nodiscard]] constexpr bool isCredentialEnvironmentEntry(std::string_view entry)
{
    for (const std::string_view name : credentialEnvironmentNames) {
        if (entry.size() > name.size() && entry.starts_with(name) && entry[name.size()] == '=')
            return true;
    }
    return false;
}

/// Where a launched program runs.
///
/// A window and a tab are where the pane was opened: a pane keeps its ID when it
/// moves, and a tool that needs its current placement asks the socket.
struct LaunchContext {
    /// The socket serving the pane; empty when nothing serves it.
    std::string socketPath;
    /// The host session backing the pane, when a host holds one.
    std::optional<Identity::ObjectID> session;
    /// The window the pane was opened in.
    std::optional<Identity::ObjectID> window;
    /// The tab the pane was opened in.
    std::optional<Identity::ObjectID> tab;
    /// The pane itself.
    std::optional<Identity::ObjectID> pane;

    /// Returns the identities set, innermost last, for ``resolveTarget``'s
    /// `here`.
    ///
    /// - Returns: The session, window, tab, and pane that are known.
    [[nodiscard]] std::vector<Identity::ObjectID> identities() const;
};

/// Writes a context as the environment entries a launcher passes its child.
///
/// - Parameter context: Where the child runs.
/// - Returns: One `NAME=value` entry for every part of the context that is set.
[[nodiscard]] std::vector<std::string> contextEnvironment(const LaunchContext& context);

/// Reads the context a process was launched with.
///
/// A value that is not an identity of the variable's own kind is ignored
/// rather than trusted, so `UBIQUITTY_PANE=window:3` names no pane.
///
/// - Parameter lookup: Returns a variable's value, or null when it is unset;
///   `std::getenv` in a program, a table in a test.
/// - Returns: The context; parts whose variable is unset or malformed are empty.
[[nodiscard]] LaunchContext contextFromEnvironment(const char* (*lookup)(const char* name));

} // namespace Ubiquitty::Session
