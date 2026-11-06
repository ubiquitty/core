// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

//
// Sync/HistoryWants.h — the history lines one attachment's client wants
// (Docs/sync.md §6.6, §7.9), one set per `HistoryPriority`.
//
// The invariants:
//
// - A want replaces the set at its priority; it never adds to it. A client
//   flicking through a transcript leaves the newest viewport, not a backlog.
// - A set is sorted, disjoint and coalesced, and lies within
//   `[max(oldest, clearedThrough), head)` as the history stood when the want
//   arrived: a line below a clear is never wanted, and one the host no longer
//   holds is never owed.
// - A want that would take the sets together past the cap is refused whole,
//   and every set stands. Taking out the lines of a transfer still in flight
//   can split a range in two, so the sets hold at most the cap plus one range
//   per transfer in flight.
// - A want of another epoch is ignored.
// - Lines leave a set when they are sent, so a set is what is still owed.
//

#include "Core/Protocol/Sync/Messages.h"
#include "Core/Screen/HistoryLines.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace Ubiquitty::Protocol::Sync {

/// What `HistoryWants::replace` did with a want.
enum class WantOutcome : std::uint8_t {
    /// The set at the want's priority is the want's ranges, clipped.
    Accepted,
    /// The ranges would take the sets past the cap; every set stands.
    RefusedOverLimit,
    /// The want names another epoch; every set stands.
    StaleEpoch,
};

/// The history lines a client wants and has not been sent.
class HistoryWants {
public:
    /// Creates empty sets.
    ///
    /// - Parameter maxPendingRanges: The most ranges the sets hold together.
    explicit HistoryWants(std::size_t maxPendingRanges);

    /// Replaces the set at a want's priority.
    ///
    /// - Parameters:
    ///   - want: The want.
    ///   - address: Where the pane's history stands.
    /// - Returns: What became of the want.
    [[nodiscard]] WantOutcome replace(const HistoryWant& want, const Ubiquitty::Screen::HistoryAddress& address);

    /// The next lines to send at a priority, without taking them from the set:
    /// the first range's first lines for `Viewport` and `Search`, and the
    /// last range's last lines for `Backfill`, which goes newest first.
    ///
    /// - Parameters:
    ///   - priority: The priority.
    ///   - maxLines: The most lines.
    /// - Returns: The lines, or `nullopt` when the set is empty.
    [[nodiscard]] std::optional<LineRange> nextSlice(HistoryPriority priority, std::size_t maxLines) const;

    /// Takes lines out of every set: they were sent, or are in flight.
    ///
    /// - Parameter range: The lines.
    void exclude(LineRange range);

    /// Whether any set wants a line of a range.
    ///
    /// - Parameter range: The lines.
    [[nodiscard]] bool intersectsAny(LineRange range) const;

    /// The set at a priority, ascending.
    [[nodiscard]] std::span<const LineRange> ranges(HistoryPriority priority) const { return m_sets[index(priority)]; }

    /// The ranges the sets hold together.
    [[nodiscard]] std::size_t pendingRanges() const;

    /// Empties every set, as a clear does.
    void clear();

private:
    [[nodiscard]] static constexpr std::size_t index(HistoryPriority priority) { return static_cast<std::size_t>(priority); }

    std::size_t m_maxPendingRanges;
    std::array<std::vector<LineRange>, 3> m_sets;
};

} // namespace Ubiquitty::Protocol::Sync
