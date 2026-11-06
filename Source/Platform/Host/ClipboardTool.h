// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <span>
#include <string>
#include <vector>

namespace Ubiquitty::Host {

/// Runs a clipboard tool alias against its owning host session.
///
/// - Parameters:
///   - arguments: The complete command-line arguments.
/// - Returns: The process exit status.
int runClipboardTool(std::span<char*> arguments);

/// Installs private tool aliases and returns the child environment overrides.
///
/// - Parameters:
///   - socketPath: The live daemon's absolute socket path.
///   - shellPath: The program receiving the startup environment.
/// - Returns: PATH and endpoint overrides, or empty on setup failure or a
///   relative socket path.
std::vector<std::string> clipboardToolEnvironment(const std::string& socketPath, const std::string& shellPath);

/// Prints shell environment assignments after the daemon verifies session ownership.
///
/// - Parameter socketPath: The absolute live daemon socket path.
/// - Returns: Zero when assignments were emitted, or a failure status.
int runClipboardEnvironment(const std::string& socketPath);

}
