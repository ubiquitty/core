// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
// Portable SSH-host validation + /bin/sh single-quoting. Two distinct concerns:
//   1. shellSingleQuote(s) closes SHELL injection — the host ends up as one
//      /bin/sh word, so metacharacters can't break out.
//   2. validateSSHHost(s) closes ARGV injection — shell-quoting does not stop a
//      host like `-oProxyCommand=…`, which ssh reads as an OPTION. The guard is
//      the leading-'-' check on the whole string and each '@'-delimited part, not
//      the characterSet. The characterSet admits IPv6 forms (':' '[' ']' '%') because none
//      can START an ssh option, so mid-string they keep argv injection closed.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace Ubiquitty::Transport {

// Wrap `s` in single quotes for /bin/sh, escaping any embedded single quote.
// The classic POSIX idiom: close the quote, emit an escaped literal quote
// (`'\''`), reopen — `it's` → `'it'\''s'`. Result is exactly one shell word.
inline std::string shellSingleQuote(std::string_view s)
{
    std::string out;
    out.reserve(s.size() + 2);
    out.push_back('\'');
    for (char c : s) {
        if (c == '\'')
            out += "'\\''";
        else
            out.push_back(c);
    }
    out.push_back('\'');
    return out;
}

/// Runs a script under /bin/sh through an SSH server's login shell.
///
/// The login shell parses only exec and one quoted argument. Backslashes stay
/// outside quotes because fish and POSIX shells differ inside single quotes.
/// - Parameter script: The complete POSIX shell script, before SSH encoding.
/// - Returns: The remote exec request, preserving every byte of the script.
[[nodiscard]] inline std::string remoteShellCommand(std::string_view script)
{
    std::string command = "exec /bin/sh -c '";
    for (const char character : script) {
        if (character == '\'')
            command += "'\\''";
        else if (character == '\\')
            command += "'\\\\'";
        else
            command.push_back(character);
    }
    command.push_back('\'');
    return command;
}

// One char of the conservative SSH-host characterSet: ASCII letters/digits plus the
// punctuation real `[user@]host` strings (incl. IPv6 literals) use. See the
// header comment for why ':' '[' ']' '%' are safe to admit mid-string.
/// Reports whether `c` may appear in a `[user@]host` string.
///
/// - Parameter c: One byte of a candidate host string.
/// - Returns: True when the byte is admissible.
inline bool isSSHHostCharacter(char c)
{
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
        return true;
    switch (c) {
    case '.':
    case '-':
    case '_':
    case '@':
    case ':':
    case '[':
    case ']':
    case '%':
        return true;
    default:
        return false;
    }
}

/// The most bytes of a `[user@]host` handed to `ssh`.
///
/// What a host's layout record keeps of a session's remote host: a longer
/// string would come back from a restart as another machine's name.
inline constexpr std::size_t maxSSHHostLength = 255;

// Validate a user-entered `[user@]host` before it reaches `ssh`. Returns false
// (reject) when:
//   * the string is empty, or starts with '-' (would be read as an ssh option);
//   * any character is outside the conservative characterSet (isSSHHostCharacter);
//   * any '@'-delimited part is empty or starts with '-' (so neither the user
//     nor the host half can smuggle an option past the leading-'-' guard);
//   * the string is longer than maxSSHHostLength.
inline bool validateSSHHost(std::string_view host)
{
    if (host.empty() || host.front() == '-' || host.size() > maxSSHHostLength)
        return false;
    for (char c : host) {
        if (!isSSHHostCharacter(c))
            return false;
    }
    // Each '@'-delimited part must be non-empty and not start with '-'.
    size_t start = 0;
    for (;;) {
        size_t at = host.find('@', start);
        std::string_view part = (at == std::string_view::npos) ? host.substr(start) : host.substr(start, at - start);
        if (part.empty() || part.front() == '-')
            return false;
        if (at == std::string_view::npos)
            break;
        start = at + 1;
    }
    return true;
}

/// The kind of connection a typed address asks for by its prefix.
///
/// The values are explicit because the Swift mirrors pin them.
enum class ConnectKindHint : std::uint8_t {
    /// No prefix: the address names a host, and what kind of connection is
    /// the person's choice.
    None = 0,
    /// `ssh host`.
    SSH = 1,
    /// `mosh host`.
    Mosh = 2,
    /// `tmux host[:session]`.
    Tmux = 3,
    /// A `/dev/…` path, or `serial /dev/…`.
    Serial = 4,
};

/// What one typed address means, as facts.
///
/// One parse every surface asks — the search field, the Add Host sheet, the
/// inspector — so a string that one of them accepts is a string all of them
/// accept, and the reason one refuses is the reason all of them show.
struct ConnectAddress {
    /// The kind the prefix asked for.
    ConnectKindHint kindHint { ConnectKindHint::None };
    /// The login name before `@`, empty when none was typed.
    std::string user;
    /// The host, without brackets around an IPv6 literal; empty for a serial
    /// device.
    std::string host;
    /// The port after `:`, 0 when none was typed.
    std::uint16_t port { 0 };
    /// The tmux session after `host:` for a `tmux` address, empty otherwise.
    std::string tmuxSession;
    /// The device path for a serial address, empty otherwise.
    std::string devicePath;
    /// Whether the address can be dialled as typed.
    bool isValid { false };
    /// Why the address cannot be dialled, as a sentence a person reads under
    /// the field; empty when it can.
    std::string reason;

    /// The `[user@]host` string ssh is handed, IPv6 brackets restored; empty
    /// for a serial address.
    [[nodiscard]] std::string destination() const;
};

/// Reads a typed address.
///
/// Accepts `[user@]host[:port]`, `[user@][v6]:port`, a bare IPv6 literal, a
/// `/dev/…` device path, and the prefixes `ssh `, `mosh `, `tmux ` and
/// `serial `. A `tmux` address reads the part after `:` as the session name
/// rather than a port. Surrounding whitespace is ignored; whitespace inside the
/// address, a leading `-`, a shell metacharacter, and a port outside 1–65535
/// each refuse with a sentence naming the problem.
///
/// - Parameter text: What the person typed.
/// - Returns: The facts, with ``ConnectAddress::isValid`` and
///   ``ConnectAddress::reason`` set either way.
[[nodiscard]] ConnectAddress parseConnectAddress(std::string_view text);

} // namespace Ubiquitty::Transport
