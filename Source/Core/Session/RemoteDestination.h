// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// RemoteDestination.h — the name a frontend shows for the machine on the far
// side of a remote session, derived from the command that opened it.
//
// A remote session is launched by a shell command, and the only place the
// destination appears is inside it. Every frontend that reports progress
// against the host — a paste crossing the wire, a transfer, a reconnect
// notice — needs the same name, so the derivation is here rather than in
// each of them: two frontends parsing one command line is two answers to
// "where am I pasting".

#include <string>
#include <string_view>

namespace Ubiquitty::Session {

/// The destination a remote session's command addresses.
///
/// Reads the first non-flag operand after `ssh`, which is where a
/// destination sits in every spelling of the command, and drops a `user@`
/// prefix — the machine is what a progress line names, not the account. A
/// command naming no host, or one that is not an ssh command at all,
/// answers empty: a caller says "the host" rather than a wrong name.
///
/// - Parameter command: The shell command the session was launched with.
/// - Returns: The destination, or empty when the command names none.
[[nodiscard]] inline std::string remoteDestinationLabel(std::string_view command)
{
    // The flags that take a separate value; the operand after one of these
    // is the flag's argument, never the destination.
    constexpr std::string_view valueFlags = "bcDEeFIiJLlmOoPpQRSWw";
    bool sawSSH = false;
    bool skipNext = false;
    std::size_t at = 0;
    while (at < command.size()) {
        while (at < command.size() && (command[at] == ' ' || command[at] == '\t'))
            ++at;
        const std::size_t start = at;
        while (at < command.size() && command[at] != ' ' && command[at] != '\t')
            ++at;
        if (start == at)
            break;
        std::string_view token = command.substr(start, at - start);
        if (skipNext) {
            skipNext = false;
            continue;
        }
        if (!sawSSH) {
            // The command may be a path: /usr/bin/ssh counts, and so does a
            // leading environment assignment the shell would consume.
            const std::size_t slash = token.rfind('/');
            const std::string_view basename = slash == std::string_view::npos ? token : token.substr(slash + 1);
            sawSSH = basename == "ssh";
            continue;
        }
        if (token.starts_with('-')) {
            // A bundled short flag ending in one that takes a value takes the
            // next token with it: `-tp 2222` is `-t` plus `-p 2222`.
            if (token.size() > 1 && valueFlags.find(token.back()) != std::string_view::npos)
                skipNext = true;
            continue;
        }
        const std::size_t atSign = token.rfind('@');
        if (atSign != std::string_view::npos)
            token = token.substr(atSign + 1);
        return std::string { token };
    }
    return {};
}

} // namespace Ubiquitty::Session
