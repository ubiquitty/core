// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace Ubiquitty::Session {

/// One successful disclosure of a pane's screen or retained text.
struct ScreenAccess {
    std::uint64_t cursor = 0;
    std::uint64_t unixMilliseconds = 0;
    std::string claimedLabel;
    std::string verifiedProcess;
};

/// A pane's bounded access history and the exact position its reader reviewed.
class ScreenAccessHistory {
public:
    static constexpr std::size_t maxRecords = 32;
    static constexpr std::size_t maxIdentityBytes = 1024;

    /// Retains a newly ordered record; replayed or oversized records change nothing.
    /// - Parameter access: Host-reported identity and event position, without screen contents.
    /// - Returns: Whether the pane gained an unread record.
    bool record(ScreenAccess access)
    {
        if (!access.cursor || access.cursor <= m_latestCursor
            || access.claimedLabel.size() > maxIdentityBytes || access.verifiedProcess.size() > maxIdentityBytes)
            return false;
        if (m_records.size() == maxRecords)
            m_records.erase(m_records.begin());
        m_latestCursor = access.cursor;
        m_records.push_back(std::move(access));
        return true;
    }

    /// Marks only the records shown when the activity list opened as reviewed.
    /// - Parameter cursor: The displayed list's last position; later reads remain unread.
    void acknowledgeThrough(std::uint64_t cursor)
    {
        m_reviewedCursor = std::max(m_reviewedCursor, std::min(cursor, m_latestCursor));
    }

    /// Clears both the history and its positions when changing to another pane.
    void reset()
    {
        m_records.clear();
        m_latestCursor = 0;
        m_reviewedCursor = 0;
    }

    /// Returns the retained records in arrival order.
    [[nodiscard]] std::span<const ScreenAccess> records() const { return m_records; }
    /// Returns the last accepted event position, including records already reviewed.
    [[nodiscard]] std::uint64_t latestCursor() const { return m_latestCursor; }
    /// Returns whether a capture arrived after the last reviewed position.
    [[nodiscard]] bool hasUnread() const { return m_latestCursor > m_reviewedCursor; }

private:
    std::vector<ScreenAccess> m_records;
    std::uint64_t m_latestCursor = 0;
    std::uint64_t m_reviewedCursor = 0;
};

} // namespace Ubiquitty::Session
