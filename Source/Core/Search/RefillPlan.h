// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/Search/LineCounts.h"
#include "Core/Search/Search.h"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace Ubiquitty::Search {

/// Locates a bounded hit window in exact line counts without reading the model.
/// The job owns the counts throughout planning. Each step visits a bounded
/// number of logical lines, including lines that contain no matches.
class RefillPlan {
public:
    /// One completed line intersecting the requested hit interval.
    struct Line {
        std::uint64_t number = 0;
        std::uint64_t newerCount = 0;
        std::uint64_t count = 0;
    };

    /// Starts planning a half-open interval in newest-first match order.
    /// Storage is reused; the caller holds no model lock.
    /// - Parameters:
    ///   - counts: Exact retained history counts, unchanged until planning ends.
    ///   - first: The first match to retain.
    ///   - end: One past the last match, at most maxStoredHits past first.
    ///   - screenCount: Matches in the screen and its open history prefix.
    void begin(const LineCounts& counts, std::uint64_t first, std::uint64_t end, std::uint64_t screenCount)
    {
        assert(first <= end && end - first <= maxStoredHits);
        m_counts = &counts;
        m_first = first;
        m_end = end;
        m_nextLine = counts.endLine();
        m_newerCount = screenCount;
        m_targetLine.reset();
        m_lines.clear();
    }

    /// Locates one retained line and the number of matches newer than it.
    /// - Parameters:
    ///   - counts: Exact retained history counts, unchanged until planning ends.
    ///   - line: The completed logical line to locate.
    ///   - screenCount: Matches newer than completed history.
    void beginLine(const LineCounts& counts, std::uint64_t line, std::uint64_t screenCount)
    {
        begin(counts, 0, 0, screenCount);
        if (line >= counts.firstLine() && line < counts.endLine())
            m_targetLine = line;
    }

    /// Visits one bounded batch of counts, without a model lock.
    /// - Parameter lineBudget: The most logical lines this call visits.
    /// - Returns: Whether more counts remain before the window is located.
    bool step(std::size_t lineBudget)
    {
        assert(m_counts && lineBudget);
        const auto hasMore = [&] {
            return m_nextLine > m_counts->firstLine() && (m_targetLine || (m_first < m_end && m_newerCount < m_end));
        };
        while (hasMore() && lineBudget) {
            --lineBudget;
            const std::uint64_t number = --m_nextLine;
            const auto count = static_cast<std::uint64_t>(m_counts->count(number));
            if (m_targetLine && number == *m_targetLine) {
                m_lines.push_back({ .number = number, .newerCount = m_newerCount, .count = count });
                m_targetLine.reset();
            } else if (!m_targetLine && count && m_newerCount + count > m_first)
                m_lines.push_back({ .number = number, .newerCount = m_newerCount, .count = count });
            m_newerCount += count;
        }
        return hasMore();
    }

    /// Returns intersecting history lines in newest-first order.
    [[nodiscard]] std::span<const Line> lines() const { return m_lines; }

private:
    const LineCounts* m_counts = nullptr;
    std::uint64_t m_first = 0;
    std::uint64_t m_end = 0;
    std::uint64_t m_nextLine = 0;
    std::uint64_t m_newerCount = 0;
    std::optional<std::uint64_t> m_targetLine;
    std::vector<Line> m_lines;
};

} // namespace Ubiquitty::Search
