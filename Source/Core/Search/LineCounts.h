// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
#include <bit>
#include <cassert>
#include <cstdint>
#include <vector>

namespace Ubiquitty::Search {

/// Exact counts for retained logical lines, independent of stored hit capacity.
/// Circular slots are reused as history advances; only growth of the retained
/// interval needs more storage. One search job owns every access.
class LineCounts {
public:
    /// Clears the interval and total while retaining allocated storage.
    void clear()
    {
        m_first = 0;
        m_end = 0;
        m_total = 0;
    }

    /// Makes room for a retained interval, preserving its existing counts.
    /// Storage growth and initialization run without the model lock.
    /// - Parameters:
    ///   - first: The first retained completed line.
    ///   - end: One past the last completed line.
    void prepare(std::uint64_t first, std::uint64_t end)
    {
        assert(first <= end);
        trimBefore(first);
        if (m_first == m_end)
            m_first = m_end = end;
        const std::uint64_t nextFirst = std::min(first, m_first);
        const std::uint64_t nextEnd = std::max(end, m_end);
        const auto required = static_cast<std::size_t>(nextEnd - nextFirst);
        if (required > m_counts.size()) {
            std::vector<std::int64_t> counts(std::bit_ceil(required), 0);
            for (std::uint64_t line = m_first; line < m_end; ++line)
                counts[static_cast<std::size_t>(line % counts.size())] = count(line);
            m_counts.swap(counts);
        } else {
            for (std::uint64_t line = nextFirst; line < m_first; ++line)
                m_counts[static_cast<std::size_t>(line % m_counts.size())] = 0;
            for (std::uint64_t line = m_end; line < nextEnd; ++line)
                m_counts[static_cast<std::size_t>(line % m_counts.size())] = 0;
        }
        m_first = nextFirst;
        m_end = nextEnd;
    }

    /// Drops evicted lines from the total without moving retained slots.
    /// - Parameter first: The first line whose beginning remains retained.
    void trimBefore(std::uint64_t first)
    {
        if (first >= m_end) {
            m_first = m_end = first;
            m_total = 0;
            return;
        }
        while (m_first < first) {
            m_total -= count(m_first);
            ++m_first;
        }
    }

    /// Replaces one prepared line's count, including a count of zero.
    /// - Parameters:
    ///   - line: A line in the prepared interval.
    ///   - count: Its exact match count.
    void replace(std::uint64_t line, std::int64_t count)
    {
        assert(line >= m_first && line < m_end && count >= 0);
        auto& slot = m_counts[static_cast<std::size_t>(line % m_counts.size())];
        m_total += count - slot;
        slot = count;
    }

    /// Reads a retained line's count, or zero outside the prepared interval.
    /// - Parameter line: The logical line number.
    /// - Returns: Its recorded count.
    [[nodiscard]] std::int64_t count(std::uint64_t line) const
    {
        return line >= m_first && line < m_end ? m_counts[static_cast<std::size_t>(line % m_counts.size())] : 0;
    }

    /// Returns the exact sum of the retained line counts.
    [[nodiscard]] std::int64_t total() const { return m_total; }

    /// Returns the first line in the retained count interval.
    [[nodiscard]] std::uint64_t firstLine() const { return m_first; }

    /// Returns one past the last line in the retained count interval.
    [[nodiscard]] std::uint64_t endLine() const { return m_end; }

private:
    std::vector<std::int64_t> m_counts;
    std::uint64_t m_first = 0;
    std::uint64_t m_end = 0;
    std::int64_t m_total = 0;
};

} // namespace Ubiquitty::Search
