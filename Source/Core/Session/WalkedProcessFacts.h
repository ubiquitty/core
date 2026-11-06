// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Source/Core/Session/WalkedProcessFacts.h — what a title reads from one walk
// of a session's process subtree.
//
// One reading, for both owners of a session's process: the frontend's runtime,
// which titles a local pane, and the host daemon, which sends a hosted pane's
// facts to every attachment. A host pane is titled from what the host read, so
// the two readings have to be the same function: a host that named the
// foreground by its executable's file name put "zsh" where the same shell,
// local, reads "-zsh".

#include "Core/Session/ProcessState.h"
#include "PAL/Process.h"

#include <span>
#include <string>

namespace Ubiquitty::Session {

/// The process-derived title facts one walk yields.
struct WalkedProcessFacts {
    /// The foreground chain, as a title shows it ("-zsh", "vim ◂ -zsh").
    std::string processChain;
    /// The same chain spelled with each program's arguments.
    std::string processChainWithArguments;
    /// The session shell's name, without its login dash.
    std::string shellCommandName;
    /// The tty the session leader holds, as its basename.
    std::string leaderTTYName;
};

/// Reads the title facts out of one walk.
///
/// The shell is the effective leader: on macOS the session leader is login(1),
/// and a segment that named it would say "login" where the reader is looking
/// for their shell. The tty is the session leader's, which is the device the
/// PTY belongs to. A chain that names this process's own image is the
/// pre-exec instant of a child that has forked and not yet exec'd, and is
/// dropped rather than titling the session after the terminal.
///
/// - Parameter walked: The records one walk found, root to leaf.
/// - Returns: The facts; every field empty for an empty walk.
[[nodiscard]] inline WalkedProcessFacts walkedProcessFacts(std::span<const PAL::Process::Record> walked)
{
    WalkedProcessFacts facts;
    if (walked.empty())
        return facts;
    if (const PAL::Process::Record* shell = PAL::Process::effectiveShellLeader(walked))
        facts.shellCommandName = std::string(PAL::Process::shellCommandDisplayName(*shell));
    if (const PAL::Process::Record* leader = PAL::Process::sessionLeader(walked))
        facts.leaderTTYName = leader->tty;
    facts.processChain = PAL::Process::formatChain(walked);
    facts.processChainWithArguments = PAL::Process::formatChain(walked, PAL::Process::ChainArguments::IncludeArguments);
    if (facts.processChain == PAL::Process::hostProcessName()) {
        facts.processChain.clear();
        facts.processChainWithArguments.clear();
    }
    return facts;
}

} // namespace Ubiquitty::Session
