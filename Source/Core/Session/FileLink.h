// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "Base/SwiftInterop.h"
#include "Core/Protocol/Sync/Wire.h"

#include <algorithm>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace Ubiquitty::Session {

/// Selects literal path parsing or file-URI decoding.
enum class FileLinkSyntax : std::uint8_t {
    Path,
    URI
};
/// Identifies why a link cannot resolve on the selected host.
enum class FileLinkError : std::uint8_t {
    Invalid,
    ForeignHost,
    NoDirectory
};

/// Which byte sequences a decoded path may hold.
enum class FilePathEncoding : std::uint8_t {
    /// Well-formed UTF-8 only: every name on a macOS volume is, and a path
    /// sent to a host travels as text.
    UTF8 UBIQUITTY_SWIFT_NAME(utf8),
    /// Any bytes but controls: a Linux file name is bytes, and one written in
    /// Latin-1 is as much a file as one written in UTF-8.
    Bytes UBIQUITTY_SWIFT_NAME(bytes),
};

/// Compares ASCII URI scheme and host names without locale-dependent folding.
///
/// - Parameters:
///   - left: The first name.
///   - right: The second name.
/// - Returns: Whether the names match under ASCII case folding.
inline bool fileLinkNamesEqual(std::string_view left, std::string_view right)
{
    const auto folded = [](unsigned char character) { return character >= 'A' && character <= 'Z' ? character + ('a' - 'A') : character; };
    return left.size() == right.size() && std::equal(left.begin(), left.end(), right.begin(), [&](auto a, auto b) { return folded(a) == folded(b); });
}

/// Recognizes the file scheme, including malformed file URIs that must not reach a handler.
///
/// - Parameter value: The link text before URI decoding.
/// - Returns: Whether the text starts with the case-insensitive file scheme.
inline bool isFileLinkURI(std::string_view value)
{
    return value.size() >= 5 && fileLinkNamesEqual(value.substr(0, 5), "file:");
}

/// Resolves a file link on the already selected host; an authority never selects a connection.
///
/// The working directory and host name are captured at the click. URI paths are
/// decoded once; path candidates are literal. Filesystem confinement remains the
/// receiving daemon's responsibility.
///
/// A URI is `file://` and an authority, or `file:/` and a path with none, the
/// form Java's `File.toURI()` writes.
///
/// - Parameters:
///   - candidate: The literal path or encoded file URI.
///   - syntax: The interpretation of candidate.
///   - directory: The absolute working directory for relative paths.
///   - host: The selected host, matched against a nonempty URI authority.
///   - encoding: Which byte sequences the resolved path may hold.
/// - Returns: The resolved path, or a parsing or host-selection error.
[[nodiscard]] inline std::expected<std::string, FileLinkError> resolveFileLink(std::string_view candidate,
    FileLinkSyntax syntax, std::string_view directory, std::string_view host, FilePathEncoding encoding = FilePathEncoding::UTF8)
{
    constexpr std::size_t maxFileLinkBytes = 4096;
    const auto isPrintable = [](std::string_view value) {
        return std::ranges::all_of(value, [](unsigned char character) { return character >= 0x20 && character != 0x7f; });
    };
    if (candidate.empty() || candidate.size() > maxFileLinkBytes || !isPrintable(candidate))
        return std::unexpected(FileLinkError::Invalid);
    std::string path;
    if (syntax == FileLinkSyntax::URI) {
        if (!isFileLinkURI(candidate) || !candidate.substr(5).starts_with('/'))
            return std::unexpected(FileLinkError::Invalid);
        candidate.remove_prefix(5);
        // Only `//` begins an authority; `file:/path` has none and names this
        // machine, as an empty one does.
        if (candidate.starts_with("//")) {
            candidate.remove_prefix(2);
            const auto slash = candidate.find('/');
            if (slash == std::string_view::npos)
                return std::unexpected(FileLinkError::Invalid);
            auto authority = candidate.substr(0, slash);
            if (authority.find_first_of("@%?#\\") != std::string_view::npos)
                return std::unexpected(FileLinkError::Invalid);
            if (authority.starts_with('[')) {
                if (!authority.ends_with(']') || authority.size() < 4)
                    return std::unexpected(FileLinkError::Invalid);
                authority = authority.substr(1, authority.size() - 2);
                if (host.starts_with('[') && host.ends_with(']'))
                    host = host.substr(1, host.size() - 2);
            } else if (authority.find(':') != std::string_view::npos)
                return std::unexpected(FileLinkError::Invalid);
            if (authority.find_first_of("[]") != std::string_view::npos)
                return std::unexpected(FileLinkError::Invalid);
            if (!authority.empty() && !fileLinkNamesEqual(authority, "localhost") && !fileLinkNamesEqual(authority, host))
                return std::unexpected(FileLinkError::ForeignHost);
            candidate.remove_prefix(slash);
        }
        if (candidate.find_first_of("?#") != std::string_view::npos)
            return std::unexpected(FileLinkError::Invalid);
        const auto hex = [](unsigned char character) -> int {
            if (character >= '0' && character <= '9')
                return character - '0';
            if (character >= 'a' && character <= 'f')
                return character - 'a' + 10;
            if (character >= 'A' && character <= 'F')
                return character - 'A' + 10;
            return -1;
        };
        for (std::size_t index = 0; index < candidate.size(); ++index) {
            if (candidate[index] != '%') {
                path.push_back(candidate[index]);
                continue;
            }
            if (candidate.size() - index < 3)
                return std::unexpected(FileLinkError::Invalid);
            const int high = hex(candidate[index + 1]);
            const int low = hex(candidate[index + 2]);
            if (high < 0 || low < 0)
                return std::unexpected(FileLinkError::Invalid);
            path.push_back(static_cast<char>((high << 4) | low));
            index += 2;
        }
    } else {
        path = candidate;
        if (path.front() != '/' && path.front() != '~') {
            if (directory.empty() || directory.front() != '/')
                return std::unexpected(FileLinkError::NoDirectory);
            if (directory.size() > maxFileLinkBytes || !isPrintable(directory))
                return std::unexpected(FileLinkError::Invalid);
            path = std::string(directory) + (directory.back() == '/' ? "" : "/") + path;
        }
    }
    if (path.empty() || path.size() > maxFileLinkBytes || !isPrintable(path))
        return std::unexpected(FileLinkError::Invalid);
    if (encoding == FilePathEncoding::UTF8
        && !Protocol::Sync::isValidUTF8(std::span { reinterpret_cast<const std::uint8_t*>(path.data()), path.size() }))
        return std::unexpected(FileLinkError::Invalid);
    return path;
}

/// Validates a local file URI without allowing its authority to select a remote machine.
///
/// - Parameters:
///   - value: The encoded file URI.
///   - localHost: The local host name permitted in its authority.
/// - Returns: Whether the URI resolves on the local host.
inline bool isLocalFileLinkURI(std::string_view value, std::string_view localHost)
{
    return resolveFileLink(value, FileLinkSyntax::URI, {}, localHost).has_value();
}

/// The line a file link's fragment names, and the column when it names one.
struct FileLinkLine {
    /// The line, counted from 1; 0 when the link names none.
    std::uint32_t line = 0;
    /// The column, counted from 1; 0 when the link names none.
    std::uint32_t column = 0;
};

namespace FileLinkDetail {

/// Reads a fragment as a line reference, the way editors and the tools that
/// print links into them spell one.
///
/// A reference is one or more positions joined by `:`, `,` or `-`, each a
/// number of at most nine digits, optionally after `L` or `l`: `12`, `L12`,
/// `L12-L20`, `12:5`, `12,14`. The first position is the line. A position
/// after a `:` that directly follows the line is its column, so `12:5` is
/// line 12, column 5, and `12-20` is line 12 with no column. A position of 0
/// names no line, so `0` is not a reference.
///
/// - Parameter fragment: The fragment, without its `#`.
/// - Returns: The line and column, or nothing when the fragment is anything else.
[[nodiscard]] inline std::optional<FileLinkLine> parseLineFragment(std::string_view fragment)
{
    constexpr std::size_t maxDigits = 9;
    FileLinkLine reference;
    std::size_t index = 0;
    for (int position = 0;; ++position) {
        if (index < fragment.size() && (fragment[index] == 'L' || fragment[index] == 'l'))
            ++index;
        std::uint32_t number = 0;
        const std::size_t start = index;
        while (index < fragment.size() && fragment[index] >= '0' && fragment[index] <= '9' && index - start < maxDigits) {
            number = number * 10 + static_cast<std::uint32_t>(fragment[index] - '0');
            ++index;
        }
        if (index == start || number == 0 || (index < fragment.size() && fragment[index] >= '0' && fragment[index] <= '9'))
            return std::nullopt;
        if (position == 0)
            reference.line = number;
        else if (position == 1 && fragment[start - 1] == ':')
            reference.column = number;
        if (index == fragment.size())
            return reference;
        const char separator = fragment[index];
        if (separator != ':' && separator != ',' && separator != '-')
            return std::nullopt;
        ++index;
    }
}

/// A file URI with a fragment that names a line set aside.
///
/// - Parameter value: The encoded file URI.
/// - Returns: The URI before its `#` when the fragment names a line, the URI
///   whole when it has no fragment, and nothing when its fragment names
///   anything else.
[[nodiscard]] inline std::optional<std::string_view> withoutLineFragment(std::string_view value)
{
    const std::size_t hash = value.find('#');
    if (hash == std::string_view::npos)
        return value;
    if (!parseLineFragment(value.substr(hash + 1)))
        return std::nullopt;
    return value.substr(0, hash);
}

} // namespace FileLinkDetail

/// The line a file URI's fragment names, for a frontend that can open a file there.
///
/// - Parameter value: The encoded file URI.
/// - Returns: The line and column, both 0 when the URI has no fragment or one that names no line.
[[nodiscard]] inline FileLinkLine fileLinkLine(std::string_view value)
{
    const std::size_t hash = value.find('#');
    if (hash == std::string_view::npos)
        return {};
    return FileLinkDetail::parseLineFragment(value.substr(hash + 1)).value_or(FileLinkLine {});
}

/// The local path a file URI names, for a frontend about to open it.
///
/// A fragment that names a line, as `rg --hyperlink-format` and `delta` write
/// one (a line number, `#L12-L20`, `#L12:5`), is set aside first, and
/// `fileLinkLine` reads it: the path is the same file whatever line it is opened at. The
/// rest is the reading `resolveFileLink` gives, which refuses a query, any
/// other fragment and an authority that names another machine, returned as a
/// plain string so a Swift caller can hold the answer. A fragment that is not
/// a line is refused because no file has one, and a program could mean it for
/// whichever handler reads it.
///
/// - Parameters:
///   - value: The encoded file URI.
///   - localHost: The local host name permitted in its authority, besides
///     `localhost`.
///   - encoding: Which byte sequences a file name may hold on this platform.
/// - Returns: The decoded path, or empty when the URI names nothing on this machine.
[[nodiscard]] inline std::string localFileLinkPath(std::string_view value, std::string_view localHost, FilePathEncoding encoding)
{
    const auto withoutLine = FileLinkDetail::withoutLineFragment(value);
    if (!withoutLine)
        return {};
    return resolveFileLink(*withoutLine, FileLinkSyntax::URI, {}, localHost, encoding).value_or(std::string {});
}

/// What a click on a file link in a pane attached to a host asks the host for.
struct HostFileLinkTarget {
    /// The path on the host.
    std::string path;
    /// The line the link names, which the file opens at once it has landed.
    FileLinkLine line;
};

/// The host file a clicked link names on a thin client, and the line it names.
///
/// A file link in a pane attached to a host names that host's file, so the
/// connected host's name stands where this machine's would, and nothing on
/// this machine is consulted. A URI's fragment that names a line is set aside
/// first, as `localFileLinkPath` sets it aside, so `#L12` asks the host for the
/// same file as no fragment does; the rest is `resolveFileLink`'s reading,
/// which refuses a query, any other fragment and an authority naming a third
/// machine. A path candidate is literal, so a `#` in it is part of a name.
///
/// `TerminalSession`'s `prepareRemoteFileDownload` asks this of a click, and
/// `hostFileLinkPath` asks it for the plate, so the two read a link alike.
///
/// - Parameters:
///   - candidate: The literal path or encoded file URI.
///   - syntax: The interpretation of candidate.
///   - directory: The host's working directory, for a relative path.
///   - host: The connected host's name, as the connection names it.
/// - Returns: The path and line, or why the host is asked for nothing.
[[nodiscard]] inline std::expected<HostFileLinkTarget, FileLinkError> resolveHostFileLink(std::string_view candidate,
    FileLinkSyntax syntax, std::string_view directory, std::string_view host)
{
    if (syntax == FileLinkSyntax::Path)
        return resolveFileLink(candidate, syntax, directory, host).transform([](std::string path) { return HostFileLinkTarget { std::move(path), {} }; });
    const auto withoutLine = FileLinkDetail::withoutLineFragment(candidate);
    if (!withoutLine)
        return std::unexpected(FileLinkError::Invalid);
    return resolveFileLink(*withoutLine, syntax, directory, host).transform([&](std::string path) {
        return HostFileLinkTarget { std::move(path), fileLinkLine(candidate) };
    });
}

/// The host path a file URI names on a thin client, read as its click asks the host for it.
///
/// `resolveHostFileLink`'s path for the URI, for the plate: a line fragment
/// names the same file, and a query, any other fragment, an authority naming a
/// third machine and a connection with no host name ask for nothing.
///
/// - Parameters:
///   - value: The encoded file URI.
///   - host: The connected host's name, as the connection names it.
/// - Returns: The decoded path, or empty when a click would ask the host for nothing.
[[nodiscard]] inline std::string hostFileLinkPath(std::string_view value, std::string_view host)
{
    if (host.empty())
        return {};
    return resolveHostFileLink(value, FileLinkSyntax::URI, {}, host).transform([](HostFileLinkTarget target) { return std::move(target.path); }).value_or(std::string {});
}

} // namespace Ubiquitty::Session
