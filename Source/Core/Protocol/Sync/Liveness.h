// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// How a client judges its link to the host (Docs/sync.md §6.9 "Stalled is
/// not lost", §6.12): two indicators a frontend shows, and the one bit that
/// marks the pane.
///
/// - **No contact**: the host is expected to speak, by its probe schedule,
///   and has been silent for longer than that schedule allows. Any frame, a
///   `Probe` included, is contact.
/// - **Input not delivered**: input has waited for its receipt for longer
///   than the stall bound. Only a receipt clears it.
///
/// Neither is `Lost`: a link that stalls is shown as stalled, and only the
/// liveness probe going unanswered declares the transport dead.

#include <cstdint>
#include <optional>

namespace Ubiquitty::Protocol::Sync {

/// What a client knows of its link, from which its state is judged.
struct LinkFacts {
    /// When the last frame of any kind arrived, absent before the first.
    std::optional<std::uint64_t> lastHeardMicroseconds;
    /// Since when the oldest input not yet receipted has waited, absent when
    /// every event sent is receipted.
    std::optional<std::uint64_t> unreceiptedSinceMicroseconds;
    /// The longest the host may be silent while it probes this client, absent
    /// when it does not: an unfocused client, one with no `Live` pane, or a
    /// host that probes nobody.
    std::optional<std::uint64_t> silenceBoundMicroseconds;
    /// How long input waits for its receipt before the client probes.
    std::uint64_t stallBoundMicroseconds = 0;
};

/// A client's link as its frontend shows it.
struct LinkState {
    /// When the last frame arrived, absent before the first.
    std::optional<std::uint64_t> lastHeardMicroseconds {};
    /// How long the oldest input not yet receipted has waited.
    std::optional<std::uint64_t> oldestUnreceiptedAgeMicroseconds {};
    /// The host has been silent past the silence it may keep.
    bool isSilent = false;
    /// Input has waited past the stall bound for its receipt.
    bool isInputUndelivered = false;

    /// Whether either indicator shows: the bit a frontend marks the pane by.
    [[nodiscard]] bool isStalled() const { return isSilent || isInputUndelivered; }
};

/// Judges a link from what the client knows of it.
///
/// - Parameters:
///   - facts: What the client knows.
///   - now: The client clock.
/// - Returns: The link's state.
[[nodiscard]] LinkState judgeLink(const LinkFacts& facts, std::uint64_t now);

/// When a link turns stalled if nothing more arrives: the earliest time at
/// which an indicator that does not show yet would. A frontend's owner wakes
/// then to show it, since nothing arriving is the event.
///
/// - Parameters:
///   - facts: What the client knows.
///   - now: The client clock.
/// - Returns: The time, or `nullopt` when no indicator is waiting to turn.
[[nodiscard]] std::optional<std::uint64_t> nextStallMicroseconds(const LinkFacts& facts, std::uint64_t now);

} // namespace Ubiquitty::Protocol::Sync
