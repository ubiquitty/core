// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
// Where ssh(1) says it connected (Docs/sync.md §6.9).
//
// A QUIC carrier dials the daemon whose bootstrap an ssh run just reached: the
// one-time token and the certificate pin that run printed are that daemon's
// and no other's. Resolving the host's name a second time can name another
// machine, so the address is not resolved again. It is read from what ssh
// reports of its own connection under `-v`:
//
//     debug1: Connecting to <name> [<address>] port <port>.
//     debug1: Connection established.
//
// ssh prints one connect line for each address it tries, so the address is
// the one whose attempt the `Connection established.` line follows. Nothing
// about how ssh dials is changed to learn it: its fallback across a name's
// addresses and its host-key lookup stay its own.
//
// A run through a proxy, or over a connection another ssh already holds
// (ControlMaster), makes no connection of its own and says so in other lines;
// each is told apart, because what the caller does next differs. A shared
// connection's address is learned by one probe and kept while that connection
// lives (`SharedConnectionReaches`).
//
// The transcript is ssh's standard error, which carries text the remote end
// chose (its banner): only whole lines in ssh's own diagnostic form are read,
// and an address is taken only if it is a numeric literal. A hostile line can
// at worst name another address, and a carrier that dials it is refused by
// the certificate pin before it presents anything.

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace Ubiquitty::Session {

/// How an ssh run reached its host, as its transcript shows.
enum class SSHReachKind : std::uint8_t {
    /// It connected to an address itself.
    Direct,
    /// It went through a proxy (ProxyJump or ProxyCommand): the host is not
    /// reachable from here at any address the run could name.
    Proxied,
    /// It used a connection another ssh already holds (ControlMaster), and
    /// made none of its own.
    Multiplexed,
    /// The transcript shows none of these.
    Unrecognised,
};

/// Where an ssh run connected.
struct SSHReach {
    SSHReachKind kind = SSHReachKind::Unrecognised;
    /// The numeric address, for `Direct`; empty otherwise.
    std::string address;
    /// The port ssh connected to, for `Direct`. It is ssh's: a QUIC carrier's
    /// port comes from the bootstrap line.
    std::uint16_t port = 0;
    /// The control socket of the connection that was shared, for
    /// `Multiplexed`, as ssh expanded its ControlPath: what names the master.
    std::string sharedConnectionPath {};

    friend bool operator==(const SSHReach&, const SSHReach&) = default;
};

/// Reports whether text is a numeric address: IPv4 dotted, or IPv6 with an
/// optional zone (`fe80::1%en0`). A name is not.
///
/// - Parameter text: The candidate.
/// - Returns: Whether a carrier may dial it without resolving anything.
[[nodiscard]] inline bool isNumericAddress(std::string_view text)
{
    if (text.empty() || text.size() > 100)
        return false;
    const std::size_t zoneAt = text.find('%');
    const std::string_view address = text.substr(0, zoneAt);
    const std::string_view zone = zoneAt == std::string_view::npos ? std::string_view {} : text.substr(zoneAt + 1);
    const auto isDigit = [](char c) { return c >= '0' && c <= '9'; };
    const auto isHex = [&](char c) { return isDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); };
    if (address.find(':') != std::string_view::npos) {
        // IPv6, possibly ending in a dotted IPv4 tail.
        if (address.size() < 2)
            return false;
        for (const char c : address) {
            if (!isHex(c) && c != ':' && c != '.')
                return false;
        }
        if (zoneAt != std::string_view::npos) {
            if (zone.empty())
                return false;
            for (const char c : zone) {
                if (!isDigit(c) && !(c >= 'a' && c <= 'z') && !(c >= 'A' && c <= 'Z') && c != '.' && c != '-' && c != '_')
                    return false;
            }
        }
        return true;
    }
    // IPv4: four decimal parts, each at most 255, and no zone.
    if (zoneAt != std::string_view::npos)
        return false;
    int parts = 0;
    std::size_t at = 0;
    while (at <= address.size()) {
        const std::size_t dot = address.find('.', at);
        const std::string_view part = address.substr(at, dot == std::string_view::npos ? std::string_view::npos : dot - at);
        if (part.empty() || part.size() > 3)
            return false;
        int value = 0;
        for (const char c : part) {
            if (!isDigit(c))
                return false;
            value = value * 10 + (c - '0');
        }
        if (value > 255)
            return false;
        ++parts;
        if (dot == std::string_view::npos)
            break;
        at = dot + 1;
    }
    return parts == 4;
}

/// Reads where an ssh run connected from its `-v` transcript.
///
/// - Parameter transcript: The run's standard error, possibly cut short.
/// - Returns: The address of the attempt that was established, when the run
///   connected itself; otherwise which other way it went, or `Unrecognised`.
[[nodiscard]] inline SSHReach readSSHReach(std::string_view transcript)
{
    constexpr std::string_view connecting = "debug1: Connecting to ";
    constexpr std::string_view established = "debug1: Connection established.";
    constexpr std::string_view proxyCommand = "debug1: Executing proxy command: ";
    constexpr std::string_view proxyJump = "debug1: Setting implicit ProxyCommand from ProxyJump: ";
    constexpr std::string_view multiplexed = "debug1: auto-mux: Trying existing master at '";

    std::optional<SSHReach> attempt;
    std::optional<SSHReach> reached;
    bool isProxied = false;
    bool isMultiplexed = false;
    std::string_view sharedConnectionPath;
    std::size_t at = 0;
    while (at < transcript.size()) {
        const std::size_t end = transcript.find('\n', at);
        std::string_view line = transcript.substr(at, end == std::string_view::npos ? std::string_view::npos : end - at);
        at = end == std::string_view::npos ? transcript.size() : end + 1;
        if (!line.empty() && line.back() == '\r')
            line.remove_suffix(1);
        if (line == established) {
            // The attempt this follows is the one that connected. The first
            // stands: ssh connects once, before the remote end can show any
            // text of its own.
            if (attempt && !reached)
                reached = attempt;
            continue;
        }
        if (line.starts_with(proxyCommand) || line.starts_with(proxyJump)) {
            isProxied = true;
            continue;
        }
        if (line.starts_with(multiplexed) && line.ends_with('\'') && line.size() > multiplexed.size() + 1) {
            // ssh says this once it has connected to a live master's socket.
            // The first stands, as the first established attempt does.
            if (!isMultiplexed)
                sharedConnectionPath = line.substr(multiplexed.size(), line.size() - multiplexed.size() - 1);
            isMultiplexed = true;
            continue;
        }
        if (!line.starts_with(connecting))
            continue;
        // "<name> [<address>] port <port>." Each connect line opens a new
        // attempt; one that names no numeric address opens none.
        attempt.reset();
        const std::size_t open = line.find(" [", connecting.size());
        const std::size_t close = open == std::string_view::npos ? std::string_view::npos : line.find("] port ", open);
        if (close == std::string_view::npos || !line.ends_with('.'))
            continue;
        const std::string_view address = line.substr(open + 2, close - open - 2);
        const std::string_view portText = line.substr(close + 7, line.size() - close - 8);
        std::uint32_t port = 0;
        bool isPort = !portText.empty() && portText.size() <= 5;
        for (const char c : portText) {
            isPort = isPort && c >= '0' && c <= '9';
            port = port * 10 + static_cast<std::uint32_t>(c - '0');
        }
        if (!isPort || port == 0 || port > 0xFFFF || !isNumericAddress(address))
            continue;
        attempt = SSHReach { .kind = SSHReachKind::Direct, .address = std::string(address), .port = static_cast<std::uint16_t>(port), .sharedConnectionPath = {} };
    }
    // A proxied run's connect lines are its proxy's own, to the proxy: the
    // proxy is what keeps a carrier from reaching the host, so it is the
    // answer whatever else the transcript holds.
    if (isProxied)
        return { .kind = SSHReachKind::Proxied, .address = {}, .port = 0, .sharedConnectionPath = {} };
    if (reached)
        return *reached;
    if (isMultiplexed)
        return { .kind = SSHReachKind::Multiplexed, .address = {}, .port = 0, .sharedConnectionPath = std::string(sharedConnectionPath) };
    return {};
}

/// Says why a reach that named no address offers no QUIC carrier, for the
/// transport's diagnostics.
///
/// - Parameter kind: How the run reached its host.
/// - Returns: The reason, empty for `Direct`.
[[nodiscard]] inline std::string_view sshReachRefusal(SSHReachKind kind)
{
    switch (kind) {
    case SSHReachKind::Direct:
        return {};
    case SSHReachKind::Proxied:
        return "ssh reaches the host through a proxy, so no address here leads to it";
    case SSHReachKind::Multiplexed:
        return "ssh reused a shared connection, and a probe did not say in time where the host is";
    case SSHReachKind::Unrecognised:
        return "ssh did not say which address it connected to";
    }
    return {};
}

/// What tells one shared connection from another: its control socket, and
/// when that socket was made. A master that went and came back, perhaps to
/// another of its host's addresses, binds a new socket at the same path.
struct SharedConnectionIdentity {
    std::string controlPath;
    /// The socket file's modification time, in its clock's ticks.
    std::int64_t madeAt = 0;

    friend bool operator==(const SharedConnectionIdentity&, const SharedConnectionIdentity&) = default;
};

/// Identifies the shared connection behind a control socket.
///
/// - Parameter controlPath: The socket's path, as ssh named it.
/// - Returns: The identity, or `nullopt` when nothing is at the path: the
///   master is gone, and what was learned of it holds no longer.
[[nodiscard]] inline std::optional<SharedConnectionIdentity> identifySharedConnection(std::string_view controlPath)
{
    if (controlPath.empty())
        return std::nullopt;
    std::error_code error;
    const std::filesystem::file_time_type madeAt = std::filesystem::last_write_time(std::filesystem::path(controlPath), error);
    if (error)
        return std::nullopt;
    return SharedConnectionIdentity { .controlPath = std::string(controlPath), .madeAt = static_cast<std::int64_t>(madeAt.time_since_epoch().count()) };
}

/// Where each shared connection reaches its host, learned by one probe and
/// kept while that connection lives, for every session that uses it: a probe
/// is a connection the host sees, so it is made once for a master and not
/// once for a dial.
///
/// Safe from any thread: each session's bootstrap runs on a thread of its
/// own.
class SharedConnectionReaches {
public:
    /// The masters remembered; past it the one noted longest ago is
    /// forgotten.
    static constexpr std::size_t maxReaches = 32;

    /// Finds the address a shared connection reaches.
    ///
    /// - Parameter identity: The connection.
    /// - Returns: The address, or `nullopt` when it was never learned, or
    ///   was learned of an earlier master at the same path.
    [[nodiscard]] std::optional<std::string> find(const SharedConnectionIdentity& identity) const
    {
        const std::scoped_lock lock(m_lock);
        const auto found = std::ranges::find(m_reaches, identity, &Reach::identity);
        return found == m_reaches.end() ? std::nullopt : std::optional { found->address };
    }

    /// Records the address a shared connection reaches, in place of anything
    /// kept for its control path.
    ///
    /// - Parameters:
    ///   - identity: The connection.
    ///   - address: The numeric address a probe found.
    void note(const SharedConnectionIdentity& identity, std::string address)
    {
        const std::scoped_lock lock(m_lock);
        std::erase_if(m_reaches, [&](const Reach& reach) { return reach.identity.controlPath == identity.controlPath; });
        if (m_reaches.size() >= maxReaches)
            m_reaches.erase(m_reaches.begin());
        m_reaches.push_back({ .identity = identity, .address = std::move(address) });
    }

private:
    /// One master and where it reaches.
    struct Reach {
        SharedConnectionIdentity identity;
        std::string address;
    };

    mutable std::mutex m_lock;
    std::vector<Reach> m_reaches;
};

} // namespace Ubiquitty::Session
