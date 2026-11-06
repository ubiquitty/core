// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// The shell-integration scripts the host binary carries, and their place in
// the control directory. A host copied to a machine on its own has no
// Resources directory beside it, so the scripts travel inside the binary and
// are written out where every shell it starts can find them through
// $UBIQUITTY_INTEGRATION_DIR.

#pragma once

#include <span>
#include <string>
#include <string_view>

namespace Ubiquitty::Host {

/// One script from Resources/shell-integration, as the binary carries it.
struct ShellIntegrationScript {
    /// The file name a shell's rc sources: `bash.sh`, `zsh.sh` or `fish.fish`.
    std::string_view fileName;
    /// The script's bytes, exactly as the resource file holds them.
    std::string_view contents;
};

/// The scripts this binary carries, generated from Resources/shell-integration.
///
/// - Returns: One entry per shell the integration supports.
[[nodiscard]] std::span<const ShellIntegrationScript> shellIntegrationScripts();

/// Writes the carried scripts into `<controlDirectory>/shell-integration`.
///
/// The directory is created owner-only and must be a directory this user owns;
/// a file already holding the same bytes is left alone, so a running shell
/// never sources a half-replaced script.
///
/// - Parameter controlDirectory: The host's control directory.
/// - Returns: The directory holding every script, or empty when any could not
///   be written.
[[nodiscard]] std::string installShellIntegration(std::string_view controlDirectory);

} // namespace Ubiquitty::Host
