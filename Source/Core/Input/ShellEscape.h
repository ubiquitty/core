// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Backslash-escape a dropped-file path so spaces and shell metacharacters survive
// word splitting at the prompt. Every byte outside [A-Za-z0-9_./-] gains a
// backslash; multibyte UTF-8 bytes (>= 0x80) are word chars and pass through verbatim.

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Ubiquitty::Input {

// Whether `byte` is a shell-unreserved byte that needs no backslash: the
// ASCII word set [A-Za-z0-9], the path-safe punctuation `_ . / -`, and any
// continuation/lead byte of a multibyte UTF-8 sequence (>= 0x80).
inline bool isShellUnreservedByte(unsigned char byte) { return (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9') || byte == '_' || byte == '.' || byte == '/' || byte == '-' || byte >= 0x80; }

// Backslash-escape `path` (UTF-8) for safe insertion at a shell prompt. Each
// reserved byte gains a leading backslash; unreserved bytes pass through. An
// empty input yields an empty result.
inline std::string shellEscapedPath(std::string_view path)
{
    std::string escaped;
    escaped.reserve(path.size() + 8);
    for (char rawByte : path) {
        const unsigned char byte = static_cast<unsigned char>(rawByte);
        if (!isShellUnreservedByte(byte))
            escaped.push_back('\\');
        escaped.push_back(rawByte);
    }
    return escaped;
}

/// The text a set of dropped paths inserts at a prompt.
///
/// Space-separated with a trailing space, so what lands reads as ready-to-use
/// arguments and the next thing typed does not run into the last path. An
/// empty path is skipped rather than contributing a bare separator, which
/// would land as an argument nobody dropped.
///
/// - Parameter paths: The dropped paths, already absolute.
/// - Returns: The text to insert; empty when nothing was droppable, which a
///   caller reads as "this drop inserted nothing" rather than a lone space.
inline std::string shellEscapedDropText(std::span<const std::string> paths)
{
    std::string text;
    for (const std::string& path : paths) {
        if (path.empty())
            continue;
        text.append(shellEscapedPath(path));
        text.push_back(' ');
    }
    return text;
}

/// The paths one drop carried, in a type Swift can both name and build.
///
/// A span is what the rule wants and what the C++ callers pass; Swift's
/// importer needs a named container to construct, and a vector is the one it
/// has.
using DroppedPaths = std::vector<std::string>;

/// The vector form of ``shellEscapedDropText``, for a caller holding one.
///
/// - Parameter paths: The dropped paths.
/// - Returns: The text to insert, empty when nothing was droppable.
inline std::string shellEscapedDropText(const DroppedPaths& paths)
{
    return shellEscapedDropText(std::span<const std::string>(paths));
}

} // namespace Ubiquitty::Input
