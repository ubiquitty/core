// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
//
// Source/Core/Session/ClosePrompt.h — the words a close asks before it kills
// something.
//
// `SessionPolicy.h` decides whether to ask (`needsCloseConfirmation`,
// `closeConfirmation`). This decides what the question says, which is a
// separate judgment and one that has to come out the same on every surface: a
// reader closing a window and a reader closing a tab are being asked about the
// same kind of loss, and two frontends wording it for themselves is how they
// come to disagree about it.
//
#include "Base/SwiftInterop.h"
#include "PAL/Process.h"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Ubiquitty::Session {

/// What a close is about to end.
///
/// The surface changes the noun and nothing else, so a frontend that gains a
/// fourth kind of surface adds a case here rather than a fourth wording.
enum class CloseScope : std::uint8_t {
    /// A window, with everything in it.
    Window UBIQUITTY_SWIFT_NAME(window),
    /// One tab of a window.
    Tab UBIQUITTY_SWIFT_NAME(tab),
    /// One pane of a split.
    Pane UBIQUITTY_SWIFT_NAME(pane),
};

/// The noun a sentence uses for a scope.
[[nodiscard]] inline std::string_view closeScopeNoun(CloseScope scope)
{
    switch (scope) {
    case CloseScope::Window:
        return "window";
    case CloseScope::Tab:
        return "tab";
    case CloseScope::Pane:
        return "pane";
    }
    return "window";
}

/// Names live descendants without exposing the session launcher, login shell, or zombies.
///
/// - Parameter processes: The session's process-tree records.
/// - Returns: Nonempty process display names in traversal order.
[[nodiscard]] inline std::vector<std::string> runningProcessNamesForClose(std::span<const PAL::Process::Record> processes)
{
    std::vector<std::string> names;
    const auto* loginShell = PAL::Process::effectiveShellLeader(processes);
    for (const auto& process : processes) {
        if (&process == loginShell || process.isSessionLeader || process.state == PAL::Process::State::Zombie) {
            continue;
        }
        std::string name { PAL::Process::shellCommandDisplayName(process) };
        if (!name.empty()) {
            names.push_back(std::move(name));
        }
    }
    return names;
}

/// The question a close asks.
///
/// - Parameter scope: What is being closed.
/// - Returns: The alert's title.
[[nodiscard]] inline std::string closeConfirmationTitle(CloseScope scope)
{
    std::string out = "Do you want to terminate running processes in this ";
    out.append(closeScopeNoun(scope));
    out.push_back('?');
    return out;
}

/// The sentence naming what the close would take.
///
/// Empty when nothing is running, which is a caller's cue that it should not
/// have asked at all — the decision to ask is `needsCloseConfirmation`'s, and a
/// prompt with nothing to name is a prompt with no reason behind it.
///
/// - Parameters:
///   - scope: What is being closed.
///   - processNames: What is running, in the order the walk found it.
/// - Returns: The alert's body.
[[nodiscard]] inline std::string closeConfirmationBody(CloseScope scope, std::span<const std::string> processNames)
{
    if (processNames.empty())
        return {};
    std::string out = "Closing this ";
    out.append(closeScopeNoun(scope));
    if (processNames.size() == 1) {
        out.append(" will terminate the running process ");
        out.append(processNames[0]);
        out.push_back('.');
        return out;
    }
    out.append(" will terminate the running processes: ");
    // Commas throughout, with no "and" before the last. A reader is counting
    // what they are about to lose, and a list reads as a count; a conjunction
    // makes the last one look like a different kind of thing from the rest.
    for (std::size_t index = 0; index < processNames.size(); ++index) {
        if (index != 0)
            out.append(", ");
        out.append(processNames[index]);
    }
    out.push_back('.');
    return out;
}

/// The running programs a close would end, gathered a name at a time by a
/// caller whose language hands no list across: a frontend that reads them
/// off a host's pane facts rather than off a local process walk.
struct CloseProcessNames {
    std::vector<std::string> names;

    /// Adds one program's name.
    ///
    /// - Parameter name: The name, as the pane's host reports it.
    void append(const std::string& name) { names.push_back(name); }
};

/// ``closeConfirmationBody`` over names gathered one at a time.
///
/// - Parameters:
///   - scope: What is being closed.
///   - processNames: What is running, in the order the panes stand.
/// - Returns: The alert's body; empty when nothing is running.
[[nodiscard]] inline std::string closeConfirmationBody(CloseScope scope, const CloseProcessNames& processNames)
{
    return closeConfirmationBody(scope, std::span<const std::string>(processNames.names));
}

/// The button that goes through with it.
///
/// Named for the consequence rather than for the gesture: "Close" describes
/// what the reader clicked a moment ago, and what they are being warned about
/// is what happens to the processes.
[[nodiscard]] inline std::string_view closeConfirmationVerb() { return "Terminate"; }

} // namespace Ubiquitty::Session
