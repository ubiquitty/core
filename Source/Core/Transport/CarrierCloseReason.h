// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
// The words a poll-backed carrier closes with, and the part of them a person
// may read.
//
// A poll channel names its own end of the pipe — "the connection closed" for
// a peer that closed its output — and appends whatever the child wrote on its
// standard error. The first half says only that the pipe ended, which a
// failure's title already says; the second is ssh's own account of what happened, which is the
// detail a failure shows.
//
// Portable: no platform types. Exception/rtti-free (WebKit style).

#include <array>
#include <string>
#include <string_view>

namespace Ubiquitty::Transport {

/// The reason a poll channel reports when its peer closed the read end.
inline constexpr std::string_view peerClosedReason = "the connection closed";

/// Every reason a poll channel reports in its own words.
inline constexpr std::array<std::string_view, 6> pollChannelReasons {
    peerClosedReason,
    "the connection closed while sending",
    "the connection failed while sending",
    "the connection failed while receiving",
    "the connection stalled with too much unsent data",
    "the connection was lost",
};

/// Separates a poll channel's reason from the standard error it appends.
inline constexpr std::string_view closeDiagnosticSeparator = ": ";

/// Reads what a carrier's child said about its close, without the channel's
/// own words.
///
/// - Parameter closeReason: The reason a channel reported.
/// - Returns: The child's standard error, trimmed and with its line endings as
///   newlines; empty when the reason is a poll channel's own words alone.
///   A reason no poll channel wrote is returned whole.
[[nodiscard]] inline std::string closeDiagnostic(std::string_view closeReason)
{
    std::string_view said = closeReason;
    for (const std::string_view reason : pollChannelReasons) {
        if (closeReason == reason)
            return {};
        if (closeReason.starts_with(reason) && closeReason.substr(reason.size()).starts_with(closeDiagnosticSeparator)) {
            said = closeReason.substr(reason.size() + closeDiagnosticSeparator.size());
            break;
        }
    }
    std::string text;
    text.reserve(said.size());
    for (const char character : said) {
        if (character != '\r')
            text.push_back(character);
    }
    constexpr std::string_view blank = " \t\n";
    const auto first = text.find_first_not_of(blank);
    if (first == std::string::npos)
        return {};
    const auto last = text.find_last_not_of(blank);
    return text.substr(first, last - first + 1);
}

} // namespace Ubiquitty::Transport
