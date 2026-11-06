// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Protocol/Sync/HistoryWants.h"

#include <algorithm>

namespace Ubiquitty::Protocol::Sync {

HistoryWants::HistoryWants(std::size_t maxPendingRanges)
    : m_maxPendingRanges(maxPendingRanges)
{
}

WantOutcome HistoryWants::replace(const HistoryWant& want, const Ubiquitty::Screen::HistoryAddress& address)
{
    if (want.historyEpoch != address.epoch)
        return WantOutcome::StaleEpoch;
    const std::uint64_t floor = std::max(address.oldest, address.clearedThrough);
    std::vector<LineRange> clipped;
    clipped.reserve(want.ranges.size());
    for (const LineRange& range : want.ranges) {
        const LineRange kept { std::max(range.from, floor), std::min(range.to, address.head) };
        if (kept.from >= kept.to)
            continue;
        // A decoded want is sorted and disjoint; ranges that touch merge.
        if (!clipped.empty() && kept.from <= clipped.back().to)
            clipped.back().to = std::max(clipped.back().to, kept.to);
        else
            clipped.push_back(kept);
    }
    std::size_t others = 0;
    for (std::size_t priority = 0; priority < m_sets.size(); ++priority)
        others += priority == index(want.priority) ? 0 : m_sets[priority].size();
    if (others + clipped.size() > m_maxPendingRanges)
        return WantOutcome::RefusedOverLimit;
    m_sets[index(want.priority)] = std::move(clipped);
    return WantOutcome::Accepted;
}

std::optional<LineRange> HistoryWants::nextSlice(HistoryPriority priority, std::size_t maxLines) const
{
    const std::vector<LineRange>& set = m_sets[index(priority)];
    if (set.empty() || maxLines == 0)
        return std::nullopt;
    if (priority == HistoryPriority::Backfill) {
        const LineRange& last = set.back();
        return LineRange { last.to - std::min<std::uint64_t>(maxLines, last.to - last.from), last.to };
    }
    const LineRange& first = set.front();
    return LineRange { first.from, first.from + std::min<std::uint64_t>(maxLines, first.to - first.from) };
}

void HistoryWants::exclude(LineRange range)
{
    if (range.from >= range.to)
        return;
    for (std::vector<LineRange>& set : m_sets) {
        std::vector<LineRange> kept;
        kept.reserve(set.size() + 1);
        for (const LineRange& held : set) {
            if (held.to <= range.from || held.from >= range.to) {
                kept.push_back(held);
                continue;
            }
            if (held.from < range.from)
                kept.push_back({ held.from, range.from });
            if (held.to > range.to)
                kept.push_back({ range.to, held.to });
        }
        set = std::move(kept);
    }
}

bool HistoryWants::intersectsAny(LineRange range) const
{
    return std::ranges::any_of(m_sets, [&](const std::vector<LineRange>& set) {
        return std::ranges::any_of(set, [&](const LineRange& held) { return held.from < range.to && range.from < held.to; });
    });
}

std::size_t HistoryWants::pendingRanges() const
{
    std::size_t count = 0;
    for (const std::vector<LineRange>& set : m_sets)
        count += set.size();
    return count;
}

void HistoryWants::clear()
{
    for (std::vector<LineRange>& set : m_sets)
        set.clear();
}

} // namespace Ubiquitty::Protocol::Sync
