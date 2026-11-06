// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
// The [network] block resolved into the RemoteOptions the ssh/mosh argv
// builders consume: the proxy kind's name mapped to its enum, the proxy gated
// on being usable, and the agent-forwarding default a launch may override.
//
// Separate from RemoteLaunch.h so that header keeps no Values dependency — the
// argv assembly is exercised where no config exists at all, and only the
// resolution of a config needs one. A frontend that resolved this itself would
// be deciding what a `[network]` value means, which is not a question about the
// machine it is running on.

#include "Core/Network/RemoteLaunch.h"
#include "Core/Preferences/Preferences.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace Ubiquitty::Network {

/// Whether a launch forwards the SSH authentication agent.
///
/// Three-valued because a launch may have no opinion: a connect started from a
/// window with a forward-agent control says so either way, while one started
/// from a menu or a URL says nothing and the configured default stands. A plain
/// bool cannot express the third case, and a caller with no opinion would have
/// to read the config itself to fill one in.
enum class AgentForwarding : std::uint8_t {
    /// Whatever `[network] ssh_forward_agent` says.
    FromConfig,
    /// This launch forwards, whatever the config says.
    Forward,
    /// This launch does not forward, whatever the config says.
    Withhold,
};

/// The proxy kind a `[network] proxy_kind` value names.
///
/// An unrecognised name — including the empty one a config that sets no proxy
/// leaves behind — is no proxy rather than an error, because the port and host
/// beside it decide usability anyway.
///
/// - Parameter name: The configured value.
/// - Returns: The matching kind, or ``ProxyKind::None``.
[[nodiscard]] inline ProxyKind proxyKindFromName(std::string_view name)
{
    if (name == "socks5")
        return ProxyKind::SOCKS5;
    if (name == "socks4")
        return ProxyKind::SOCKS4;
    if (name == "http")
        return ProxyKind::HTTP;
    return ProxyKind::None;
}

/// Resolves the `[network]` block into the options an ssh or mosh launch honors.
///
/// The proxy is carried across as configured rather than pre-screened: whether
/// it is usable is ``ProxyConfig::enabled`` and ``isUsableProxy``, which the
/// argv builders already consult, and a second screen here could only disagree
/// with them.
///
/// - Parameters:
///   - config: The resolved configuration.
///   - agentForwarding: What this launch has to say about agent forwarding.
/// - Returns: The options, ready for ``buildSSHArguments`` or
///   ``buildMoshArguments``.
[[nodiscard]] inline RemoteOptions remoteOptionsFromValues(const Preferences::Values& config, AgentForwarding agentForwarding = AgentForwarding::FromConfig)
{
    RemoteOptions options;
    options.forwardAgent = agentForwarding == AgentForwarding::FromConfig ? config.sshForwardAgent.value_or(false) : agentForwarding == AgentForwarding::Forward;
    options.proxy.kind = proxyKindFromName(config.proxyKind.value_or(""));
    options.proxy.host = config.proxyHost.value_or("");
    options.proxy.port = config.proxyPort.value_or(0);
    return options;
}

/// The interactive shell tool a launch runs.
enum class ShellTool : std::uint8_t {
    SSH,
    Mosh,
};

/// The argv for an interactive ssh or mosh shell, under the `[network]`
/// options the configuration resolves.
///
/// One call a frontend makes rather than three, so the options a shell runs
/// under are read in one place: the configuration, then the launch's own word
/// on agent forwarding.
///
/// - Parameters:
///   - config: The resolved configuration.
///   - tool: Which shell.
///   - binaryPath: The resolved executable.
///   - host: The `[user@]host` destination.
///   - agentForwarding: What this launch says about agent forwarding.
/// - Returns: The argv, empty for an empty path or a destination ssh would
///   misread.
[[nodiscard]] inline std::vector<std::string> buildShellLaunchArguments(const Preferences::Values& config, ShellTool tool,
    std::string_view binaryPath, std::string_view host, AgentForwarding agentForwarding)
{
    const RemoteOptions options = remoteOptionsFromValues(config, agentForwarding);
    return tool == ShellTool::SSH ? buildSSHArguments(binaryPath, host, options) : buildMoshArguments(binaryPath, host, options);
}

/// The launch a profile's `startup_host` asks for, under the `[network]`
/// options the configuration resolves.
///
/// The value is the profile's and the options are the user's own: agent
/// forwarding and a proxy come from the configuration, never from the
/// shareable profile string, which ``buildStartupHostArguments`` holds to a
/// host and nothing else.
///
/// - Parameters:
///   - config: The resolved configuration.
///   - sshPath: The ssh the frontend resolved; empty when there is none.
///   - moshPath: The mosh the frontend resolved; empty when there is none.
///   - startupHost: The profile's resolved `startup_host`.
/// - Returns: The argv, or the reason there is none. A refusal opens the
///   local shell, never something else.
[[nodiscard]] inline StartupHostLaunch buildStartupHostLaunch(const Preferences::Values& config, const std::string& sshPath,
    const std::string& moshPath, const std::string& startupHost)
{
    return buildStartupHostArguments(sshPath, moshPath, startupHost, remoteOptionsFromValues(config));
}

} // namespace Ubiquitty::Network
