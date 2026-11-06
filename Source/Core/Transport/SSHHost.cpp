// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// SSHHost.cpp — see SSHHost.h.

#include "Core/Transport/SSHHost.h"

#include <array>
#include <charconv>
#include <utility>

namespace Ubiquitty::Transport {

namespace {

/// A word an address may open with, and the kind it asks for.
struct KindPrefix {
    std::string_view word;
    ConnectKindHint kind;
};

constexpr std::array<KindPrefix, 4> kindPrefixes { {
    { "ssh", ConnectKindHint::SSH },
    { "mosh", ConnectKindHint::Mosh },
    { "tmux", ConnectKindHint::Tmux },
    { "serial", ConnectKindHint::Serial },
} };

constexpr std::string_view devicePrefix = "/dev/";

[[nodiscard]] bool isSpace(char character)
{
    return character == ' ' || character == '\t' || character == '\n' || character == '\r' || character == '\f' || character == '\v';
}

[[nodiscard]] std::string_view trimmed(std::string_view text)
{
    while (!text.empty() && isSpace(text.front()))
        text.remove_prefix(1);
    while (!text.empty() && isSpace(text.back()))
        text.remove_suffix(1);
    return text;
}

[[nodiscard]] bool isDevicePathCharacter(char character)
{
    return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') || (character >= '0' && character <= '9')
        || character == '.' || character == '-' || character == '_' || character == '/';
}

/// The address refused, for the reason given.
///
/// - Parameters:
///   - address: The facts read so far.
///   - reason: The sentence.
/// - Returns: The address, marked invalid.
[[nodiscard]] ConnectAddress refused(ConnectAddress address, std::string reason)
{
    address.isValid = false;
    address.reason = std::move(reason);
    return address;
}

[[nodiscard]] std::string leadingDashReason() { return "ssh reads a leading “-” as an option."; }

/// Reads a serial device path.
///
/// - Parameters:
///   - address: The facts read so far.
///   - path: The typed path.
/// - Returns: The address.
[[nodiscard]] ConnectAddress parseDevicePath(ConnectAddress address, std::string_view path)
{
    address.kindHint = ConnectKindHint::Serial;
    if (!path.starts_with(devicePrefix) || path.size() == devicePrefix.size())
        return refused(std::move(address), "A serial port is a device path, like /dev/cu.usbserial.");
    for (const char character : path) {
        if (isSpace(character))
            return refused(std::move(address), "A device path can’t contain spaces.");
        if (!isDevicePathCharacter(character))
            return refused(std::move(address), "“" + std::string(1, character) + "” can’t appear in a device path.");
    }
    address.devicePath = std::string(path);
    address.isValid = true;
    return address;
}

/// Reads a port.
///
/// - Parameter text: The digits after `:`.
/// - Returns: The port, or 0 when the text is not a port.
[[nodiscard]] std::uint16_t parsePort(std::string_view text)
{
    if (text.empty() || text.size() > 5)
        return 0;
    unsigned value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc() || end != text.data() + text.size() || value == 0 || value > 65535)
        return 0;
    return static_cast<std::uint16_t>(value);
}

} // namespace

std::string ConnectAddress::destination() const
{
    if (host.empty())
        return {};
    // A literal with ':' in it is IPv6, which ssh reads unambiguously only in
    // brackets once a port or a user sits beside it.
    const std::string bracketed = host.find(':') != std::string::npos ? "[" + host + "]" : host;
    return user.empty() ? bracketed : user + "@" + bracketed;
}

ConnectAddress parseConnectAddress(std::string_view text)
{
    ConnectAddress address;
    std::string_view rest = trimmed(text);
    if (rest.empty())
        return refused(std::move(address), "Enter an address, like you@server.example.");

    for (const KindPrefix& prefix : kindPrefixes) {
        if (rest.size() > prefix.word.size() && rest.starts_with(prefix.word) && isSpace(rest[prefix.word.size()])) {
            address.kindHint = prefix.kind;
            rest = trimmed(rest.substr(prefix.word.size()));
            break;
        }
    }
    if (rest.empty())
        return refused(std::move(address), "Enter a host after the connection kind.");

    if (rest.front() == '/' || address.kindHint == ConnectKindHint::Serial) {
        if (address.kindHint != ConnectKindHint::None && address.kindHint != ConnectKindHint::Serial)
            return refused(std::move(address), "A device path opens a serial port, not a remote shell.");
        return parseDevicePath(std::move(address), rest);
    }

    for (const char character : rest) {
        if (isSpace(character))
            return refused(std::move(address), "An address can’t contain spaces.");
    }
    // The option reading first: `-o…=…` is an option before it is a string
    // with an '=' in it, and the sentence should name what ssh would do.
    if (rest.front() == '-')
        return refused(std::move(address), leadingDashReason());
    if (const std::size_t at = rest.rfind('@'); at != std::string_view::npos && at + 1 < rest.size() && rest[at + 1] == '-')
        return refused(std::move(address), leadingDashReason());
    for (const char character : rest) {
        if (!isSSHHostCharacter(character))
            return refused(std::move(address), "“" + std::string(1, character) + "” can’t appear in an address.");
    }

    std::string_view hostAndPort = rest;
    if (const std::size_t at = rest.find('@'); at != std::string_view::npos) {
        if (rest.find('@', at + 1) != std::string_view::npos)
            return refused(std::move(address), "An address has one “@”, between the user and the host.");
        const std::string_view user = rest.substr(0, at);
        hostAndPort = rest.substr(at + 1);
        if (user.empty())
            return refused(std::move(address), "Enter a user name before “@”.");
        address.user = std::string(user);
        if (hostAndPort.empty())
            return refused(std::move(address), "Enter a host after “@”.");
        if (hostAndPort.front() == '-')
            return refused(std::move(address), leadingDashReason());
    }

    std::string_view host = hostAndPort;
    std::string_view suffix;
    if (hostAndPort.front() == '[') {
        const std::size_t close = hostAndPort.find(']');
        if (close == std::string_view::npos)
            return refused(std::move(address), "An IPv6 address needs its closing “]”.");
        host = hostAndPort.substr(1, close - 1);
        const std::string_view after = hostAndPort.substr(close + 1);
        if (!after.empty()) {
            if (after.front() != ':')
                return refused(std::move(address), "Only a port can follow an IPv6 address, after “:”.");
            suffix = after.substr(1);
            if (suffix.empty())
                return refused(std::move(address), "Enter a port after “:”.");
        }
    } else if (const std::size_t colon = hostAndPort.find(':'); colon != std::string_view::npos
        && hostAndPort.find(':', colon + 1) == std::string_view::npos) {
        // One colon is host:port; more is a bare IPv6 literal with no port.
        host = hostAndPort.substr(0, colon);
        suffix = hostAndPort.substr(colon + 1);
        if (suffix.empty())
            return refused(std::move(address), address.kindHint == ConnectKindHint::Tmux ? "Enter a session name after “:”." : "Enter a port after “:”.");
    }
    if (host.empty())
        return refused(std::move(address), "Enter a host.");
    if (host.front() == '-')
        return refused(std::move(address), leadingDashReason());
    address.host = std::string(host);

    if (!suffix.empty()) {
        if (address.kindHint == ConnectKindHint::Tmux) {
            address.tmuxSession = std::string(suffix);
        } else {
            address.port = parsePort(suffix);
            if (address.port == 0)
                return refused(std::move(address), "A port is a number from 1 to 65535.");
        }
    }
    address.isValid = true;
    return address;
}

} // namespace Ubiquitty::Transport
