// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
// A file this terminal fetched from somewhere else lands the way a browser
// download lands: under a name that cannot escape the directory it was written
// into, tagged so Gatekeeper still gets a say over it. Both are policy rather
// than mechanism — the same answer has to hold on every frontend, or one of
// them is the loose one — so the strings live here and the filesystem call
// stays with the platform that has one. Pure text, no platform types.

#include <cstdint>
#include <string>
#include <string_view>

namespace Ubiquitty::Session {

// The extended attribute LaunchServices and Gatekeeper read before opening a
// file, and therefore the one a fetched file carries.
inline constexpr std::string_view quarantineAttributeName = "com.apple.quarantine";

// The flags field of a browser download's quarantine value: kLSQuarantineTypeWebDownload
// with the "not yet approved by the user" bit still set, which is what makes
// the first open ask.
inline constexpr std::string_view quarantineDownloadFlags = "0083";

// What this terminal calls itself in a quarantine record, the field a person
// reads back in the "downloaded from" sentence.
inline constexpr std::string_view quarantineAgentName = "Ubiquitty";

/// Builds the `com.apple.quarantine` value for a file this terminal fetched.
///
/// Four semicolon-separated fields — flags, the time as eight lower-case hex
/// digits, the agent, and an event id — which is exactly what Safari and
/// `curl --xattr` write. Written in that shape so LaunchServices treats a file
/// fetched off a remote host as a browser download and not as something a
/// trusted local program produced.
///
/// - Parameters:
///   - unixSeconds: When the file landed. Only the low 32 bits are recorded,
///     which is the width the field has.
///   - eventIdentifier: A UUID the caller minted. The caller's, because a UUID
///     has no portable source and a record whose id repeats is a record two
///     files share.
/// - Returns: The attribute's value.
inline std::string quarantineValue(std::uint64_t unixSeconds, std::string_view eventIdentifier)
{
    char timestamp[9] = {};
    for (int digit = 7; digit >= 0; --digit) {
        timestamp[digit] = "0123456789abcdef"[unixSeconds & 0xF];
        unixSeconds >>= 4;
    }
    return std::string(quarantineDownloadFlags) + ';' + timestamp + ';' + std::string(quarantineAgentName) + ';'
        + std::string(eventIdentifier);
}

/// Reduces an untrusted suggestion to a file name that cannot escape its
/// directory.
///
/// The suggestion comes from the other end — a path a remote host echoed back,
/// or a name a sender chose — so only its last component is used: a name
/// carrying `..` or a leading slash would otherwise place the write outside
/// the fresh directory it was meant to be confined to.
///
/// - Parameter suggestedName: What the other end called the file.
/// - Returns: The name to write under. A suggestion that is all separators, or
///   names a directory, leaves nothing to write under and yields a fixed name.
inline std::string safeDownloadName(std::string_view suggestedName)
{
    const std::size_t lastSeparator = suggestedName.find_last_of('/');
    const std::string_view lastComponent
        = lastSeparator == std::string_view::npos ? suggestedName : suggestedName.substr(lastSeparator + 1);
    if (lastComponent.empty() || lastComponent == "." || lastComponent == "..")
        return "download";
    return std::string(lastComponent);
}

/// Resolves the path on a host that a clicked candidate names.
///
/// A path a program printed on a host names a file there, so a relative one is
/// joined against what that host last reported as its directory rather than
/// against anything on this machine. A `~` is left for the host's own shell to
/// expand, because this machine's home is not the host's.
///
/// - Parameters:
///   - candidate: The text the link run matched.
///   - hostDirectory: What the host last reported as its directory.
/// - Returns: The path to ask the host for. Empty when none can be derived —
///   a relative candidate with no directory reported — so the click does
///   nothing, which is what a local click on a path resolving to no file does.
inline std::string hostAbsolutePath(std::string_view candidate, std::string_view hostDirectory)
{
    if (candidate.empty())
        return {};
    if (candidate.front() == '/' || candidate.front() == '~')
        return std::string(candidate);
    if (hostDirectory.empty())
        return {};
    std::string joined(hostDirectory);
    if (joined.back() != '/')
        joined.push_back('/');
    joined += candidate;
    return joined;
}

} // namespace Ubiquitty::Session
