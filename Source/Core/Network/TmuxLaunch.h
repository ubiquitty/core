// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/Protocol/Tmux/Commands.h"
#include "Core/Transport/SSHHost.h"

// Portable assembly of `tmux -CC` launch arguments. The frontend resolves the
// executable path; host validation and remote shell quoting remain here.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Ubiquitty::Network {

// Default control-mode arguments: attach-or-create so a launch works whether or not
// a session exists (a bare `attach` immediately %exits when there's none). `-CC`
// engages control mode.
constexpr std::string_view defaultControlArguments = "-CC new-session -A -s main";

// Split a space-separated tmux argument string into tokens, dropping empty runs (so
// a double/leading/trailing space can't inject a blank argument).
[[nodiscard]] inline std::vector<std::string> splitArguments(std::string_view arguments)
{
    std::vector<std::string> tokens;
    size_t start = 0;
    while (start < arguments.size()) {
        const size_t space = arguments.find(' ', start);
        const size_t end = (space == std::string_view::npos) ? arguments.size() : space;
        if (end > start)
            tokens.emplace_back(arguments.substr(start, end - start));
        if (space == std::string_view::npos)
            break;
        start = space + 1;
    }
    return tokens;
}

// Resolve which control-mode argument string to use: the caller's override when
// non-empty, else the attach-or-create default.
[[nodiscard]] inline std::string_view effectiveControlArguments(std::string_view override) { return override.empty() ? defaultControlArguments : override; }

// Assemble the LOCAL launch argv: the resolved tmux path, then the control-mode
// argument tokens. `tmuxPath` is the frontend's platform-resolved absolute path
// (or a bare "tmux" fallback); `extraArguments` overrides the default control
// string when non-empty. Returns an empty vector if `tmuxPath` is empty (no
// program to exec).
[[nodiscard]] inline std::vector<std::string> buildLocalLaunchArguments(std::string_view tmuxPath, std::string_view extraArguments)
{
    std::vector<std::string> argv;
    if (tmuxPath.empty())
        return argv;
    argv.emplace_back(tmuxPath);
    for (auto& token : splitArguments(effectiveControlArguments(extraArguments)))
        argv.push_back(std::move(token));
    return argv;
}

/// Reads what a person typed in a tmux Session field as the control-mode
/// arguments it means.
///
/// Blank is the attach-or-create default; a typed `attach` or
/// `new-session …` command is passed through behind `-CC`; anything else is a
/// session name, attached to or created under that name.
///
/// - Parameter sessionField: The field, as typed; surrounding whitespace is
///   ignored.
/// - Returns: The control-mode arguments, ready for the launch builders.
[[nodiscard]] inline std::string mapSessionArgument(std::string_view sessionField)
{
    while (!sessionField.empty() && (sessionField.front() == ' ' || sessionField.front() == '\t' || sessionField.front() == '\n'))
        sessionField.remove_prefix(1);
    while (!sessionField.empty() && (sessionField.back() == ' ' || sessionField.back() == '\t' || sessionField.back() == '\n'))
        sessionField.remove_suffix(1);
    if (sessionField.empty())
        return std::string(defaultControlArguments);
    if (sessionField == "attach" || sessionField.starts_with("attach ") || sessionField == "new-session"
        || sessionField.starts_with("new-session "))
        return "-CC " + std::string(sessionField);
    return "-CC new-session -A -s " + std::string(sessionField);
}

/// The directories a tool is looked for in, in order: the install prefixes a
/// Finder-launched app's minimal `PATH` misses, then that `PATH`.
///
/// - Parameter pathEnvironment: The process's `PATH`.
/// - Returns: The directories, each once.
[[nodiscard]] inline std::vector<std::string> toolSearchDirectories(std::string_view pathEnvironment)
{
    std::vector<std::string> directories { "/opt/homebrew/bin", "/usr/local/bin", "/opt/local/bin", "/usr/bin" };
    std::size_t start = 0;
    while (start <= pathEnvironment.size()) {
        const std::size_t colon = pathEnvironment.find(':', start);
        const std::size_t end = colon == std::string_view::npos ? pathEnvironment.size() : colon;
        const std::string directory(pathEnvironment.substr(start, end - start));
        if (!directory.empty() && std::find(directories.begin(), directories.end(), directory) == directories.end())
            directories.push_back(directory);
        if (colon == std::string_view::npos)
            break;
        start = colon + 1;
    }
    return directories;
}

/// Why a tmux launch could not be assembled.
///
/// The values are explicit because the Swift mirrors pin them.
enum class TmuxLaunchFailure : std::uint8_t {
    /// The launch was assembled.
    None = 0,
    /// No `tmux` was found in any searched directory.
    TmuxUnavailable = 1,
    /// The ssh destination is not one this app hands to ssh.
    InvalidHost = 2,
};

/// A tmux launch, or why there is none.
struct TmuxLaunchPlan {
    /// The argument vector to spawn; empty when ``failure`` is set.
    std::vector<std::string> arguments;
    /// Why there is no launch.
    TmuxLaunchFailure failure { TmuxLaunchFailure::None };
    /// The directories searched for a local `tmux`, for the sentence a
    /// missing one is reported in; empty for a remote launch.
    std::vector<std::string> searchedDirectories;
};

/// Plans a local tmux control-mode launch.
///
/// - Parameters:
///   - tmuxPath: The `tmux` the frontend found in ``toolSearchDirectories``,
///     empty when none was.
///   - pathEnvironment: The process's `PATH`, which named where it looked.
///   - sessionField: The Session field as typed.
/// - Returns: The launch, or ``TmuxLaunchFailure::TmuxUnavailable`` with the
///   directories searched.
[[nodiscard]] inline TmuxLaunchPlan planLocalTmuxLaunch(std::string_view tmuxPath, std::string_view pathEnvironment, std::string_view sessionField)
{
    TmuxLaunchPlan plan;
    if (tmuxPath.empty()) {
        plan.failure = TmuxLaunchFailure::TmuxUnavailable;
        plan.searchedDirectories = toolSearchDirectories(pathEnvironment);
        return plan;
    }
    plan.arguments = buildLocalLaunchArguments(tmuxPath, mapSessionArgument(sessionField));
    return plan;
}

// Applies the shared SSH destination policy before constructing an argument vector.
[[nodiscard]] inline bool isSafeSSHHost(std::string_view host) { return Transport::validateSSHHost(host); }

/// Wraps an argument vector as one shell-quoted command for the SSH server.
///
/// - Parameters:
///   - sshPath: The resolved SSH executable.
///   - host: The explicit SSH destination.
///   - command: The remote executable and its literal arguments.
/// - Returns: The SSH argument vector, or empty for a refused destination or command.
[[nodiscard]] inline std::vector<std::string> buildSSHControlArguments(std::string_view sshPath, std::string_view host, const std::vector<std::string>& command)
{
    if (sshPath.empty() || !isSafeSSHHost(host) || command.empty()) {
        return {};
    }
    std::string remoteCommand;
    for (const auto& argument : command) {
        if (argument.find('\0') != std::string::npos) {
            return {};
        }
        if (!remoteCommand.empty()) {
            remoteCommand += ' ';
        }
        remoteCommand += Transport::shellSingleQuote(argument);
    }
    return { std::string(sshPath), "-t", "-e", "none", std::string(host), Transport::remoteShellCommand(remoteCommand) };
}

// Assemble the REMOTE launch argv: ssh wrapping a remote `tmux …`. The fixed
// `-t -e none` flags give the remote tmux a pty so it engages control mode (-t) and
// drop the ssh escape char so it can't corrupt the octal-escaped control stream
// (-e none). Returns an empty vector if the host is unsafe or either path is empty.
[[nodiscard]] inline std::vector<std::string> buildRemoteLaunchArguments(std::string_view sshPath, std::string_view host, std::string_view extraArguments)
{
    return buildSSHControlArguments(sshPath, host, buildLocalLaunchArguments("tmux", extraArguments));
}

/// Plans a tmux control-mode launch over ssh.
///
/// - Parameters:
///   - sshPath: The resolved ssh executable.
///   - host: The ssh destination.
///   - sessionField: The Session field as typed.
/// - Returns: The launch, or ``TmuxLaunchFailure::InvalidHost`` for a
///   destination ssh would misread.
[[nodiscard]] inline TmuxLaunchPlan planRemoteTmuxLaunch(std::string_view sshPath, std::string_view host, std::string_view sessionField)
{
    TmuxLaunchPlan plan;
    plan.arguments = buildRemoteLaunchArguments(sshPath, host, mapSessionArgument(sessionField));
    if (plan.arguments.empty())
        plan.failure = TmuxLaunchFailure::InvalidHost;
    return plan;
}

/// Reattaches the exact remote server lifetime and session without creating one.
///
/// - Parameters:
///   - sshPath: The resolved SSH executable.
///   - host: The saved SSH destination.
///   - reference: The confirmed tmux server lifetime and session identity.
/// - Returns: The guarded SSH launch arguments, or empty for an invalid reference.
[[nodiscard]] inline std::vector<std::string> buildRemoteRestoreArguments(std::string_view sshPath, std::string_view host, const Protocol::Tmux::SessionReference& reference)
{
    return buildSSHControlArguments(sshPath, host, Protocol::Tmux::restoreSessionArguments("tmux", reference));
}

} // namespace Ubiquitty::Network
