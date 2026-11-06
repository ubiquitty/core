// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdio>
#include <string>
#include <string_view>

namespace Ubiquitty {
namespace Host {

/// The header line for a grant file of this kind.
///
/// Every grant file opens with it. It names the kind, so one kind's records
/// cannot be read as another's, and the one layout this build reads, so a file
/// in any other is refused whole rather than read under a guessed meaning.
///
/// - Parameters:
///   - kind: Which file.
/// - Returns: The line to write first, newline included.
inline std::string grantFileHeader(std::string_view kind)
{
    return "# ubiquitty " + std::string(kind) + " v1\n";
}

/// Answers whether a file's first line is the header of a `kind` file.
///
/// - Parameters:
///   - line: The file's first line, as read, line end included or not.
///   - kind: The kind the caller expects.
/// - Returns: Whether it is exactly ``grantFileHeader(kind)``.
inline bool isGrantFileHeader(std::string_view line, std::string_view kind)
{
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
        line.remove_suffix(1);
    const std::string expected = grantFileHeader(kind);
    return line == std::string_view(expected).substr(0, expected.size() - 1);
}

} // namespace Host
} // namespace Ubiquitty
