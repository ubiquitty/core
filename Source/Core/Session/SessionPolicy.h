// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
// The close-confirmation decision: should the frontend warn before tearing a
// session down, and what would the warning be about? Pure logic over facts the
// caller reads off its live process handle and its own window. No platform
// types.

#include "Base/SwiftInterop.h"
#include "PAL/Process.h"

#include <cstdint>
#include <span>
#include <string_view>

namespace Ubiquitty::Session {

// The three facts the close-confirmation decision is taken over. The caller reads
// these off its live process handle (macOS: PAL::Stream::process()) right before
// the decision — `hasRunningChild` / `foregroundIsShell` reap a dead child when
// read, so they must be sampled fresh, not cached.
struct ProcessFacts {
    // A live PTY stream backs this session. False for a render-only / detached
    // session that owns no process to terminate.
    bool streamPresent;
    // The session's shell has a child process still running under it (a real
    // foreground job, not a pristine prompt).
    bool hasRunningChild;
    // The foreground process is the session's shell (i.e. the user is sitting at
    // the prompt, not inside a job). When true, closing kills nothing the user
    // would miss, so no confirmation.
    bool foregroundIsShell;
};

// Whether closing this session should prompt first (the close would kill a real
// running job). True only when all hold: a live stream, a running child, and the
// foreground process is not the shell itself.
inline bool needsCloseConfirmation(const ProcessFacts& facts) { return facts.streamPresent && facts.hasRunningChild && !facts.foregroundIsShell; }

/// Matches an executable basename against comma- or newline-separated names.
[[nodiscard]] inline bool isProcessListed(std::string_view name, std::string_view names)
{
    if (const auto slash = name.rfind('/'); slash != std::string_view::npos) {
        name.remove_prefix(slash + 1);
    }
    while (!names.empty()) {
        const auto end = names.find_first_of(",\n");
        auto candidate = names.substr(0, end);
        const auto first = candidate.find_first_not_of(" \t\r");
        const auto last = candidate.find_last_not_of(" \t\r");
        if (first != std::string_view::npos && !name.empty() && candidate.substr(first, last - first + 1) == name) {
            return true;
        }
        if (end == std::string_view::npos) {
            break;
        }
        names.remove_prefix(end + 1);
    }
    return false;
}

/// Whether closing ignores a process: ``isProcessListed`` against
/// `[terminal] close_ignored_processes`.
[[nodiscard]] inline bool isCloseIgnoredProcess(std::string_view name, std::string_view names) { return isProcessListed(name, names); }

/// Applies the configured close policy to the foreground job's processes.
[[nodiscard]] inline bool needsCloseConfirmation(const ProcessFacts& facts, std::string_view when,
    std::string_view ignoredProcesses, std::span<const PAL::Process::Record> processes, std::string_view foregroundName)
{
    if (when == "never" || !facts.streamPresent) {
        return false;
    }
    if (when == "always") {
        return true;
    }
    if (!needsCloseConfirmation(facts)) {
        return false;
    }
    if (!isCloseIgnoredProcess(foregroundName, ignoredProcesses)) {
        return true;
    }
    bool foundForegroundProcess = false;
    for (const auto& process : processes) {
        if (!process.isForeground || process.isSessionLeader || process.state == PAL::Process::State::Zombie) {
            continue;
        }
        foundForegroundProcess = true;
        if (!isCloseIgnoredProcess(process.name, ignoredProcesses)) {
            return true;
        }
    }
    return !foundForegroundProcess && !isCloseIgnoredProcess(foregroundName, ignoredProcesses);
}

/// What a close would be confirming.
///
/// The frontend words its prompt from this rather than deciding what the
/// close is about itself, so two shells asking the same question ask it the
/// same way.
enum class CloseConfirmation : std::uint8_t {
    /// Nothing a reader would miss: close without asking.
    None UBIQUITTY_SWIFT_NAME(none),
    /// The tab was pinned against exactly this.
    PinnedTab UBIQUITTY_SWIFT_NAME(pinnedTab),
    /// A foreground job would be killed.
    RunningJob UBIQUITTY_SWIFT_NAME(runningJob),
};

/// The facts a close is decided over.
///
/// The process side arrives already decided, because the two shells reach it by
/// different routes: one asks ``needsCloseConfirmation`` for its single session,
/// the other asks its workspace model across every pane of every tab. Both
/// answer the same question, and this takes the answer rather than the inputs.
struct CloseFacts {
    bool wouldKillARunningJob;
    bool isPinned;
};

/// What closing this session should confirm, if anything.
///
/// A pin outranks a running job because it is the claim the reader made
/// themselves: they said this tab is not to go by accident, and a prompt that
/// answered with something else would be answering a question they did not
/// ask. One prompt is asked and never two: a reader who has answered has
/// authorised the close, and a second question behind the first would be
/// asking them to authorise it again.
///
/// - Parameter facts: The process facts and the pin.
/// - Returns: What to confirm, or `None` to close.
[[nodiscard]] inline CloseConfirmation closeConfirmation(const CloseFacts& facts)
{
    if (facts.isPinned)
        return CloseConfirmation::PinnedTab;
    return facts.wouldKillARunningJob ? CloseConfirmation::RunningJob : CloseConfirmation::None;
}

} // namespace Ubiquitty::Session
