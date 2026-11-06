// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
// Portable assembly of the argv for a NATIVE remote session: `ssh [user@]host`
// (optionally agent-forwarding), `mosh [user@]host`, plus the SOCKS/HTTP proxy
// wiring both consume. The frontend resolves the binary's absolute path itself.
//
// SECURITY: the host is validated with Transport::validateSSHHost before it
// reaches argv so a `-oProxyCommand=…` host can't smuggle an ssh option (argv
// injection). buildSSHArguments / buildMoshArguments re-check it as a floor.

#include "Core/Transport/SSHHost.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace Ubiquitty::Network {

// A SOCKS/HTTP proxy the launch should route through. Consumed differently per
// transport: ssh gets a ProxyCommand (via nc/connect), mosh gets it in the
// environment of the ssh bootstrap it spawns. Empty host = no proxy.
enum class ProxyKind {
    None,
    SOCKS5, // -o ProxyCommand="nc -X 5 -x host:port %h %p"
    SOCKS4, // -o ProxyCommand="nc -X 4 -x host:port %h %p"
    HTTP, // -o ProxyCommand="nc -X connect -x host:port %h %p"
};

struct ProxyConfig {
    ProxyKind kind = ProxyKind::None;
    std::string host; // proxy host (validated by the same SSH-host guard)
    int port = 0; // proxy port (1..65535)

    [[nodiscard]] bool enabled() const { return kind != ProxyKind::None && !host.empty() && port > 0 && port <= 65535; }
};

// Options the SSH/mosh launch honors.
struct RemoteOptions {
    bool forwardAgent = false; // ssh -A: forward the SSH auth agent
    ProxyConfig proxy; // optional SOCKS/HTTP proxy
};

namespace detail {

// `nc`'s -X flag value for a proxy kind: "5" SOCKS5, "4" SOCKS4, "connect" HTTP.
// Empty for None.
inline std::string_view ncProxyProtocolFlag(ProxyKind kind)
{
    switch (kind) {
    case ProxyKind::SOCKS5:
        return "5";
    case ProxyKind::SOCKS4:
        return "4";
    case ProxyKind::HTTP:
        return "connect";
    case ProxyKind::None:
        return {};
    }
    // Outside the enumeration; see xtermButtonCode for why this is not a default.
    return {};
}

// Build the ssh ProxyCommand value: `nc -X <proto> -x host:port %h %p` (ssh
// expands %h/%p to the destination). SECURITY: host/port come from validated
// config, and the value is one ssh -o argument, never re-split by a shell.
inline std::string proxyCommandFor(const ProxyConfig& proxy)
{
    std::string command = "nc -X ";
    command += ncProxyProtocolFlag(proxy.kind);
    command += " -x ";
    command += proxy.host;
    command += ':';
    command += std::to_string(proxy.port);
    command += " %h %p";
    return command;
}

} // namespace detail

// Safe to use: enabled and the proxy host passes the same argv-injection guard
// as the destination (it too reaches ssh as part of a -o value).
inline bool isUsableProxy(const ProxyConfig& proxy) { return proxy.enabled() && Transport::validateSSHHost(proxy.host); }

// Assemble the argv for a native `ssh` session. Returns an empty vector (== don't
// launch) when `sshPath` is empty or `host` is unsafe.
// Layout: ssh [-A] [-o ProxyCommand=…] -t [user@]host
//   -A forwards the agent (opt-in). -t forces a remote pty so full-screen apps work.
[[nodiscard]] inline std::vector<std::string> buildSSHArguments(std::string_view sshPath, std::string_view host, const RemoteOptions& options = {})
{
    std::vector<std::string> argv;
    if (sshPath.empty() || !Transport::validateSSHHost(host))
        return argv;
    argv.emplace_back(sshPath);
    if (options.forwardAgent)
        argv.emplace_back("-A");
    if (isUsableProxy(options.proxy)) {
        argv.emplace_back("-o");
        argv.emplace_back("ProxyCommand=" + detail::proxyCommandFor(options.proxy));
    }
    argv.emplace_back("-t"); // force a pty for the interactive remote shell
    argv.emplace_back(std::string(host));
    return argv;
}

// Assemble the argv for a native `mosh` session. mosh has no ProxyCommand of its
// own; a set proxy is passed through mosh's `--ssh` bootstrap so the ssh leg honors
// it (the UDP data plane still needs a reachable server). Empty vector on empty path / unsafe host.
// Layout: mosh [--ssh="ssh -A -o ProxyCommand=…"] [user@]host
[[nodiscard]] inline std::vector<std::string> buildMoshArguments(std::string_view moshPath, std::string_view host, const RemoteOptions& options = {})
{
    std::vector<std::string> argv;
    if (moshPath.empty() || !Transport::validateSSHHost(host))
        return argv;
    argv.emplace_back(moshPath);
    // Build the bootstrap-ssh override only when it differs from mosh's default
    // (an agent-forward or a proxy is requested). mosh runs this string as the
    // login transport.
    if (options.forwardAgent || isUsableProxy(options.proxy)) {
        std::string sshSpec = "ssh";
        if (options.forwardAgent)
            sshSpec += " -A";
        if (isUsableProxy(options.proxy)) {
            // ProxyCommand contains spaces; quote it so mosh's space-tokenised
            // --ssh (shell-quoting aware) keeps it one ssh -o argument.
            sshSpec += " -o \"ProxyCommand=" + detail::proxyCommandFor(options.proxy) + "\"";
        }
        argv.emplace_back("--ssh=" + sshSpec);
    }
    argv.emplace_back(std::string(host));
    return argv;
}

/// Why a profile's `startup_host` was refused, or ``None`` when it was accepted.
///
/// A `startup_host` names a host and nothing else. It is not a command line: a
/// profile is a shareable artifact — the config template presents profiles as
/// bundles to copy, the config watcher re-reads the file a synced checkout
/// writes, and profile inheritance carries an ancestor's `startup_host` into a
/// profile the user trusts. A value that could name a program would therefore run
/// on whichever machine opened the profile.
enum class StartupHostRejection : uint8_t {
    None, // accepted
    Empty, // nothing set
    UnknownProgram, // named a program other than ssh / mosh
    ExtraArguments, // flags or extra words — a port or an option needs its own field
    InvalidHost, // failed Transport::validateSSHHost
    MissingBinary, // the frontend could not resolve ssh / mosh
};

/// The argv a `startup_host` resolved to, with the reason when it resolved to none.
///
/// Empty `arguments` means the caller launches its normal local shell. The
/// rejection is carried rather than dropped so a frontend can say which profile
/// was refused and why: a setting that stops working silently is indistinguishable
/// from a broken feature.
struct StartupHostLaunch {
    std::vector<std::string> arguments; // empty ⇒ do not launch; fall back to the local shell
    StartupHostRejection rejection { StartupHostRejection::None };

    /// Reports whether a launch should happen.
    ///
    /// - Returns: True when a host was accepted and an argv was built.
    [[nodiscard]] bool accepted() const { return rejection == StartupHostRejection::None && !arguments.empty(); }
};

/// Returns a one-line reason a `startup_host` was refused, for a log or a
/// diagnostic. Never null.
///
/// - Parameter rejection: The reason `buildStartupHostArguments` reported.
/// - Returns: A short human-readable phrase.
[[nodiscard]] inline const char* startupHostRejectionReason(StartupHostRejection rejection)
{
    switch (rejection) {
    case StartupHostRejection::None:
        return "accepted";
    case StartupHostRejection::Empty:
        return "startup_host is empty";
    case StartupHostRejection::UnknownProgram:
        return "startup_host must name a host, optionally prefixed with ssh or mosh";
    case StartupHostRejection::ExtraArguments:
        return "startup_host takes one host and no options";
    case StartupHostRejection::InvalidHost:
        return "startup_host is not a valid [user@]host";
    case StartupHostRejection::MissingBinary:
        return "ssh or mosh could not be found";
    }
    return "startup_host was refused";
}

/// Assembles the argv a profile's `startup_host` should launch.
///
/// Accepts a bare `[user@]host`, or the documented `ssh [user@]host` /
/// `mosh [user@]host` form, and nothing else. The host is validated by
/// ``Transport::validateSSHHost`` and the argv is built by the same
/// ``buildSSHArguments`` / ``buildMoshArguments`` the interactive New-SSH sheet
/// uses, so one connect path carries one set of guarantees.
///
/// Options and flags are refused rather than forwarded. `-oProxyCommand=…` is an
/// ssh option that names a program to run, so a `startup_host` permitted to carry
/// flags is one permitted to run anything. Agent forwarding and proxies come from
/// `options`, which is config the user set themselves, not from this string.
///
/// - Parameters:
///   - sshPath: Absolute path the frontend resolved for ssh; empty if absent.
///   - moshPath: Absolute path the frontend resolved for mosh; empty if absent.
///   - startupHost: The profile's resolved `startup_host`.
///   - options: Agent forwarding and proxy settings, from the user's own config.
/// - Returns: The argv and a rejection reason. Empty arguments mean do not launch.
[[nodiscard]] inline StartupHostLaunch buildStartupHostArguments(std::string_view sshPath, std::string_view moshPath, std::string_view startupHost, const RemoteOptions& options = {})
{
    StartupHostLaunch launch;

    const auto firstNonSpace = startupHost.find_first_not_of(" \t");
    if (firstNonSpace == std::string_view::npos) {
        launch.rejection = StartupHostRejection::Empty;
        return launch;
    }
    const auto lastNonSpace = startupHost.find_last_not_of(" \t");
    std::string_view rest = startupHost.substr(firstNonSpace, lastNonSpace - firstNonSpace + 1);

    // An optional leading program word. The config template documents
    // `startup_host = "ssh prod.example.com"`, so the prefixed form has to keep
    // working; a bare host is the shape the field's own name describes.
    bool useMosh = false;
    const auto space = rest.find_first_of(" \t");
    if (space != std::string_view::npos) {
        const std::string_view word = rest.substr(0, space);
        if (word == "ssh")
            useMosh = false;
        else if (word == "mosh")
            useMosh = true;
        else {
            launch.rejection = StartupHostRejection::UnknownProgram;
            return launch;
        }
        const auto hostStart = rest.find_first_not_of(" \t", space);
        if (hostStart == std::string_view::npos) {
            launch.rejection = StartupHostRejection::InvalidHost;
            return launch;
        }
        rest = rest.substr(hostStart);
        // Exactly one host word may follow. A second word is a flag or an
        // argument, and neither belongs in a field that names a host.
        if (rest.find_first_of(" \t") != std::string_view::npos) {
            launch.rejection = StartupHostRejection::ExtraArguments;
            return launch;
        }
    }

    if (!Transport::validateSSHHost(rest)) {
        launch.rejection = StartupHostRejection::InvalidHost;
        return launch;
    }

    const std::string_view binaryPath = useMosh ? moshPath : sshPath;
    if (binaryPath.empty()) {
        launch.rejection = StartupHostRejection::MissingBinary;
        return launch;
    }

    launch.arguments = useMosh ? buildMoshArguments(binaryPath, rest, options) : buildSSHArguments(binaryPath, rest, options);
    if (launch.arguments.empty())
        launch.rejection = StartupHostRejection::InvalidHost; // the builder re-checks as a floor
    return launch;
}

} // namespace Ubiquitty::Network
