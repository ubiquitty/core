// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <optional>
#include <string>

namespace Ubiquitty::Preferences {

/// The directory the preferences store lives in.
///
/// `$XDG_CONFIG_HOME/ubiquitty` when that variable holds an absolute path,
/// otherwise `<home>/.config/ubiquitty`, where `<home>` is whatever
/// `PAL::systemHomeDirectory` resolves: `$HOME` first, then the account
/// database. Every frontend and the POSIX store resolve through here, so a
/// process tree that mixes them never splits across two directories. The result
/// carries no trailing slash.
///
/// - Returns: The directory path, or `std::nullopt` when no home resolves.
[[nodiscard]] std::optional<std::string> directoryPath();

/// The file a person edits: `config.toml` inside ``directoryPath``.
///
/// - Returns: The file path, or `std::nullopt` when no directory resolves.
[[nodiscard]] std::optional<std::string> filePath();

} // namespace Ubiquitty::Preferences
