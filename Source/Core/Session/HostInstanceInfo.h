// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/Codec/JSON.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace Ubiquitty::Session {

/// A live same-user daemon found by the host, with its own attach executable.
/// Numeric workspace and session IDs are meaningful only within this endpoint.
struct HostInstanceInfo {
    std::string socketName;
    std::string executable;
    std::uint64_t startedUnixSeconds = 0;
    /// The running process's version, not the executable currently at its path.
    std::string appVersion {};
};

/// An owned endpoint list for language bridges; never borrows a changing tree.
struct HostInstanceList {
    std::vector<HostInstanceInfo> entries;
    [[nodiscard]] std::size_t count() const { return entries.size(); }
    [[nodiscard]] HostInstanceInfo instanceAt(std::size_t index) const { return index < entries.size() ? entries[index] : HostInstanceInfo {}; }
};

inline constexpr std::size_t maxDiscoveredHostInstances = 32;

/// Whether a version description is bounded printable text suitable for a notice.
[[nodiscard]] inline bool isValidDaemonVersion(std::string_view version)
{
    return version.size() <= 128 && std::ranges::all_of(version, [](unsigned char byte) { return byte >= 32 && byte < 127; });
}

/// Reads a complete startup line for the socket's authenticated peer process.
///
/// The caller bounds the log read. Other processes' startup lines and incomplete
/// lines cannot describe this peer; the on-disk executable may have been replaced.
/// - Parameters:
///   - log: A bounded prefix of the daemon's own log.
///   - process: The process authenticated by the socket.
/// - Returns: Its reported version, empty when the log does not establish one.
[[nodiscard]] inline std::string daemonVersionFromLog(std::string_view log, std::uint64_t process)
{
    constexpr std::string_view prefix = "ubiquitty-host: daemon ";
    const std::string identity = ", process " + std::to_string(process) + ", started ";
    std::string version;
    while (process && !log.empty()) {
        const auto end = log.find('\n');
        if (end == std::string_view::npos)
            break;
        const auto line = log.substr(0, end);
        log.remove_prefix(end + 1);
        const auto boundary = line.find(" serving protocol ");
        if (!line.starts_with(prefix) || boundary == std::string_view::npos || boundary < prefix.size()
            || line.find(identity, boundary) == std::string_view::npos)
            continue;
        const auto candidate = line.substr(prefix.size(), boundary - prefix.size());
        if (!candidate.empty() && isValidDaemonVersion(candidate))
            version = candidate;
    }
    return version;
}

/// Checks endpoint names before any frontend uses them in a transport command.
[[nodiscard]] inline bool isValidHostInstance(const HostInstanceInfo& instance)
{
    const auto& socket = instance.socketName;
    return socket.size() == 26 && socket.starts_with("host-") && socket.ends_with(".sock")
        && std::ranges::all_of(socket.substr(5, 16), [](char digit) { return (digit >= '0' && digit <= '9') || (digit >= 'a' && digit <= 'f'); })
        && !instance.executable.empty() && instance.executable.size() <= 4096 && instance.executable.front() == '/'
        && instance.executable.ends_with("/ubiquitty-host")
        && std::ranges::none_of(instance.executable, [](unsigned char byte) { return byte < 32 || byte == 127; })
        && isValidDaemonVersion(instance.appVersion);
}

/// Reads only complete bounded endpoint records, ignoring duplicate socket names.
[[nodiscard]] inline std::vector<HostInstanceInfo> readHostInstances(const Codec::JSON::Value* value)
{
    std::vector<HostInstanceInfo> result;
    if (!value || !value->isArray() || value->items().size() > maxDiscoveredHostInstances)
        return result;
    for (const auto& item : value->items()) {
        HostInstanceInfo instance { item.stringOr("socket"), item.stringOr("executable"), 0 };
        instance.appVersion = item.stringOr("appVersion");
        const auto started = item.numberOr("startedUnixSeconds");
        if (!(started >= 0 && started <= 9007199254740991.0) || std::floor(started) != started || !isValidHostInstance(instance))
            continue;
        instance.startedUnixSeconds = static_cast<std::uint64_t>(started);
        if (std::ranges::find(result, instance.socketName, &HostInstanceInfo::socketName) == result.end())
            result.push_back(std::move(instance));
    }
    return result;
}

} // namespace Ubiquitty::Session
