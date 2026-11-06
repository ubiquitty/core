// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Plumbing shared between the Values translation units. Nothing outside
// Source/Core/Preferences/ includes this header.

#include <optional>
#include <string>

namespace Ubiquitty::Preferences {

/// Reads the whole file at `path` into `body`.
///
/// Wraps the PAL storage seam's two-call protocol — a sizing call with no buffer, then the
/// read — so a caller cannot get the order or the resize wrong. A file that exists but is
/// empty succeeds with `body` empty, which is distinct from a file that is absent.
///
/// - Parameters:
///   - path: The file to read.
///   - body: Receives the contents; left in an unspecified state when the read fails.
/// - Returns: `true` when the file was read, `false` when it is absent or unreadable.
/// Reads a whole file into memory.
///
/// - Parameter path: The file to read.
/// - Returns: Its bytes, or nothing when it cannot be read. A file that
///   exists and is empty reads as an empty string, which is not nothing.
[[nodiscard]] std::optional<std::string> readWholeFile(const std::string& path);

} // namespace Ubiquitty::Preferences
