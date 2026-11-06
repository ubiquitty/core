// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
// Portable assembly of the remote bootstrap command the `ubiquitty ssh`/`mosh`
// wrapper runs at connect to compile Ubiquitty's terminfo into the remote
// ~/.terminfo, so $TERM=xterm-ubiquitty (and its Ms clipboard cap) resolves remotely.
//
// SECURITY — the remote command must be:
//   - TERMINFO-only: runs only `tic` over our source. No arbitrary code, no rc mutation.
//   - IDEMPOTENT: skips when xterm-ubiquitty already resolves.
//   - FAIL-OPEN: the final `exec "$SHELL" -l` runs UNCONDITIONALLY, so a missing/
//     failing tic never blocks the login.
//   - ARGV/SHELL-SAFE: host floored by validateSSHHost; source shellSingleQuote'd into one word.
// The source string is passed in by the caller, keeping this free of the terminfo module.

#include "Core/Network/RemoteLaunch.h"
#include "Core/Transport/SSHHost.h"

#include <string>
#include <string_view>
#include <vector>

namespace Ubiquitty::Network {

// The canonical terminfo entry name the remote command probes and installs.
inline constexpr std::string_view terminfoEntryName = "xterm-ubiquitty";

// Build the remote /bin/sh command that installs `terminfoSource` idempotently
// and then execs the login shell. One line, shape:
//
//   if command -v tic >/dev/null 2>&1 && ! infocmp xterm-ubiquitty >/dev/null 2>&1; then
//     mkdir -p "$HOME/.terminfo" 2>/dev/null &&
//     printf %s '<source>' | tic -x -o "$HOME/.terminfo" - >/dev/null 2>&1;
//   fi; exec "${SHELL:-/bin/sh}" -l
//
//   - `command -v tic` gate => no tic, no install (fail-open); exec still runs.
//   - `! infocmp xterm-ubiquitty` => skip when already resolvable (idempotent).
//   - `printf %s '<single-quoted source>'` feeds the source verbatim to tic's stdin
//     (source has no single quote; %s takes the arg literally, backslashes survive).
//   - trailing `exec` is UNCONDITIONAL, so an install failure degrades to a normal login.
[[nodiscard]] inline std::string buildRemoteTerminfoInstallCommand(std::string_view terminfoSource)
{
    std::string command;
    command.reserve(terminfoSource.size() + 160);
    command += "if command -v tic >/dev/null 2>&1 && ! infocmp ";
    command += terminfoEntryName;
    command += " >/dev/null 2>&1; then mkdir -p \"$HOME/.terminfo\" 2>/dev/null && printf %s ";
    command += Transport::shellSingleQuote(terminfoSource);
    command += " | tic -x -o \"$HOME/.terminfo\" - >/dev/null 2>&1; fi; exec \"${SHELL:-/bin/sh}\" -l";
    return command;
}

// Assemble the `ubiquitty ssh` argv: the plain ssh argv with the terminfo-install
// command appended as the remote command (install-then-login-shell). Empty vector
// (=> don't launch) when the base argv is empty; the caller falls back to plain ssh.
[[nodiscard]] inline std::vector<std::string> buildSSHArgumentsWithTerminfoInstall(std::string_view sshPath, std::string_view host, std::string_view terminfoSource, const RemoteOptions& options = {})
{
    std::vector<std::string> argv = buildSSHArguments(sshPath, host, options);
    if (argv.empty())
        return argv;
    argv.emplace_back(Transport::remoteShellCommand(buildRemoteTerminfoInstallCommand(terminfoSource)));
    return argv;
}

} // namespace Ubiquitty::Network
