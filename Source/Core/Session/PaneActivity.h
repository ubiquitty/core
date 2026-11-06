// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
//
// Source/Core/Session/PaneActivity.h — when a pane's program last wrote and
// when someone last typed into it.
//
// Two instants on the host's wall clock, kept apart because they answer
// different questions: "output two minutes ago" is the program, "typed an hour
// ago" is a person. The host stamps each once per batch (a drain pass of the
// PTY, a pass of client input), never per byte, and a listing shows a pane's
// own and, for a tab, a window or a workspace, the newest beneath it.
//
// Wall time, because the reader of a listing is on another machine at another
// moment: a monotonic stamp means nothing outside the process that took it.
// Pure: the caller hands in the time.
//

#include <algorithm>
#include <cstdint>
#include <span>

namespace Ubiquitty::Session {

/// When a pane last had output and last had typed input.
///
/// Milliseconds since the Unix epoch on the host's wall clock; zero for never.
struct PaneActivity {
    std::uint64_t lastOutputUnixMilliseconds = 0;
    std::uint64_t lastInputUnixMilliseconds = 0;

    /// Records a batch of the program's output.
    ///
    /// - Parameter nowUnixMilliseconds: The host's wall clock.
    constexpr void noteOutput(std::uint64_t nowUnixMilliseconds) { lastOutputUnixMilliseconds = nowUnixMilliseconds; }

    /// Records a batch of input someone typed.
    ///
    /// - Parameter nowUnixMilliseconds: The host's wall clock.
    constexpr void noteInput(std::uint64_t nowUnixMilliseconds) { lastInputUnixMilliseconds = nowUnixMilliseconds; }

    bool operator==(const PaneActivity&) const = default;
};

/// A run of activities, named so it crosses into Swift.
using PaneActivitySpan = std::span<const PaneActivity>;

/// What a node above panes shows: its newest child, each instant on its own.
///
/// The same fold from panes to a tab, tabs to a window and windows to a
/// workspace, and independent of how the panes are grouped. The newest output
/// and the newest input may come from different children.
///
/// - Parameter children: The node's children.
/// - Returns: The newest output and the newest input among them; never, for none.
[[nodiscard]] constexpr PaneActivity newestActivity(PaneActivitySpan children)
{
    PaneActivity newest;
    for (const PaneActivity& child : children) {
        newest.lastOutputUnixMilliseconds = std::max(newest.lastOutputUnixMilliseconds, child.lastOutputUnixMilliseconds);
        newest.lastInputUnixMilliseconds = std::max(newest.lastInputUnixMilliseconds, child.lastInputUnixMilliseconds);
    }
    return newest;
}

} // namespace Ubiquitty::Session
