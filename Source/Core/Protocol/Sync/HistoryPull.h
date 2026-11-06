// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

//
// Sync/HistoryPull.h — the one rule a client follows to pull a pane's history
// as its reader scrolls (Docs/sync.md §6.6 "The client pulls on scroll").
//
// A frontend reports where its view sits; the client owning the pane's cache
// asks this what the view owes. A view within one screen of the oldest row
// the replica projects owes one `Viewport` want for the gaps in the two
// screens of lines just older than the newest run the cache holds. The range
// depends on the run, not on where in that screen the view is, so a view
// moving there owes nothing more until the lines arrive and the run grows; a
// host holding nothing older is not asked again. A replica whose ring is full
// owes nothing, since the projection keeps only the newest rows it can hold.
//
// Every client on a Sync connection routes through this, whatever owns its
// carrier, so the rule exists once.
//

#include "Core/Protocol/Sync/HistoryCache.h"
#include "Core/Protocol/Sync/Messages.h"
#include "Core/Terminal.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <vector>

namespace Ubiquitty::Protocol::Sync {

/// What one pane's reader has asked of its history, and what a view owes.
class HistoryPull {
public:
    /// A want a view owes: the range it reached, keyed by epoch, and the
    /// parts of it the cache does not hold.
    struct Want {
        std::uint32_t epoch = 0;
        LineRange reached;
        std::vector<LineRange> gaps;
    };

    /// Decides the want a view this deep owes.
    ///
    /// - Parameters:
    ///   - history: The pane's cache.
    ///   - replica: The terminal the cache projects into.
    ///   - rowsAboveTail: Rows the view shows above the live tail.
    /// - Returns: The want, or `nullopt` when the view is not near the oldest
    ///   row, the replica's ring is full, nothing older can be sent, or the
    ///   same range was already asked for in this epoch.
    [[nodiscard]] std::optional<Want> wantFor(const HistoryCache& history, const Ubiquitty::Terminal& replica, int rowsAboveTail) const
    {
        const int rows = replica.rows;
        const int held = replica.scrollbackCount;
        if (rows <= 0 || rowsAboveTail <= 0 || held - rowsAboveTail >= rows)
            return std::nullopt;
        if (replica.scrollbackLimit > 0 && held >= replica.scrollbackLimit)
            return std::nullopt;
        const std::vector<LineRange> runs = history.cachedRanges();
        const auto newest = std::ranges::find_if(runs.rbegin(), runs.rend(), [&](const LineRange& run) { return run.from < history.head(); });
        if (newest == runs.rend() || newest->from == 0)
            return std::nullopt;
        const std::uint64_t lines = 2 * static_cast<std::uint64_t>(rows);
        Want want { .epoch = history.key().epoch, .reached = { .from = newest->from > lines ? newest->from - lines : 0, .to = newest->from }, .gaps = {} };
        if (m_sent && m_sent->epoch == want.epoch && m_sent->reached == want.reached)
            return std::nullopt;
        want.gaps = history.gaps(want.reached);
        if (want.gaps.empty() || want.gaps.size() > maxHistoryWantRanges)
            return std::nullopt;
        return want;
    }

    /// Records that a want reached the connection, so a view moving within
    /// the same range asks nothing more.
    ///
    /// - Parameter want: The want the connection admitted.
    void noteSent(const Want& want) { m_sent = Sent { .epoch = want.epoch, .reached = want.reached }; }

    /// Forgets the want sent: the host's wants are a connection's, and a new
    /// connection holds none.
    void reset() { m_sent.reset(); }

private:
    /// The range the last admitted want reached, and its epoch.
    struct Sent {
        std::uint32_t epoch = 0;
        LineRange reached;
    };
    std::optional<Sent> m_sent;
};

} // namespace Ubiquitty::Protocol::Sync
