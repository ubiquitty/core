// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace Ubiquitty::Host {

class Daemon;

/// Reports only completed transfers and explicitly refused predecessor attempts.
struct HandoverStartupReport {
    std::size_t movedSessions = 0;
    std::size_t refusedPredecessors = 0;
};

/// Routes authenticated local handover outside the ordinary remote protocols.
struct HandoverStartup {
    /// Pins the original install path before a daemon can outlive its executable.
    /// - Parameters:
    ///   - daemon: The not-yet-serving daemon.
    ///   - installPath: Its resolved absolute executable path, not a peer field.
    static void initialize(Daemon& daemon, std::string_view installPath);

    /// Selects a new local client's first-record route without blocking on input.
    /// Ordinary, greeted and QUIC clients cannot enter the handover transaction.
    /// An admitted request waits as the daemon's pending handover; a second
    /// arriving while one waits is refused.
    /// - Parameters:
    ///   - daemon: The holder and exclusive reactor owner.
    ///   - clientIndex: The current physical client's index.
    ///   - nowMilliseconds: Current monotonic time.
    ///   - deadlineMilliseconds: The original pre-HELLO deadline for this client.
    /// - Returns: True only when the caller should continue ordinary reads.
    [[nodiscard]] static bool route(Daemon& daemon, std::size_t clientIndex,
        std::uint64_t nowMilliseconds, std::uint64_t deadlineMilliseconds);

    /// Starts the pending handover's transaction once this host owes its
    /// clients nothing and nothing else blocks it, or once its wait is over,
    /// when the transaction refuses with what still blocks it.
    ///
    /// The reactor calls this after each pass has written to its clients.
    /// - Parameters:
    ///   - daemon: The holder and exclusive reactor owner.
    ///   - nowMilliseconds: Current monotonic time.
    static void servicePending(Daemon& daemon, std::uint64_t nowMilliseconds);

    /// Requests older sibling state before the new daemon accepts any client.
    /// Refusals preserve the predecessor; this never terminates another daemon.
    /// - Parameters:
    ///   - daemon: Started only far enough to bind its private versioned listener.
    ///   - timeoutMilliseconds: Positive bound for the complete sibling walk,
    ///     capped by the transaction's maximum deadline.
    /// - Returns: Counts from completed transaction results, not attempted moves.
    [[nodiscard]] static HandoverStartupReport takeOverSiblings(Daemon& daemon, int timeoutMilliseconds);
};

} // namespace Ubiquitty::Host
