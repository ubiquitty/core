// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/Protocol/Sync/Records.h"
#include "PAL/TerminalClients.h"

#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace Ubiquitty::Session {

/// A retained event ready for a platform notification service.
struct BackgroundRefreshNotification {
    std::string identifier;
    std::string title;
    std::string body;
    std::string paneID;
    std::uint32_t sessionID = 0;
    std::string kind;
    std::string statusID;
    std::string notificationID;
    NotificationOccasion occasion = NotificationOccasion::Always;
    std::uint64_t sequence = 0;
    std::uint64_t createdUnixMilliseconds = 0;
    bool isReady = false;
};

/// The latest cached facts and optional screen for a remote pane.
struct BackgroundRefreshPane {
    std::string paneID;
    std::uint32_t sessionID = 0;
    std::string title;
    std::string statusText;
    std::string cachedScreenText;
    Protocol::Sync::PaneFacts facts;
    Protocol::Sync::StatusRecords records;
    std::string screenRevision;
    std::vector<std::uint8_t> checkpoint;
};

/// Owns passive catch-up cursors, cached state, and unpresented notifications.
///
/// A caller stores `saveRecord()` atomically before presenting notifications.
/// Successful presentation retires an identifier and stores the record again.
/// Replaying an interrupted delivery uses the same platform notification ID.
/// Fetching never acknowledges human attention or changes a live attachment.
class BackgroundRefreshState {
public:
    static constexpr std::size_t maxResponseBytes = 1024 * 1024;
    static constexpr std::size_t maxRecordBytes = 4 * 1024 * 1024;
    static constexpr std::size_t maxCheckpointBytes = 256 * 1024;
    static constexpr std::size_t maxCachedPanes = 256;
    static constexpr std::size_t maxPendingNotifications = 256;
    static constexpr std::size_t maxSeenPanes = 1024;

    /// Returns the cache byte limit through a Swift-importable method.
    [[nodiscard]] static constexpr std::size_t recordByteLimit() { return maxRecordBytes; }

    /// Starts a bounded refresh pass without discarding committed progress.
    void beginRefresh();
    /// Builds a correlated control-only request, optionally prefetching a pane.
    [[nodiscard]] std::string makeRequest(const std::string& screenPaneID = {});
    /// Validates and atomically applies the outstanding request's answer.
    [[nodiscard]] bool applyResponse(const std::string&);
    /// Restores a bounded record; failure leaves the held state unchanged.
    [[nodiscard]] bool restoreRecord(const std::string&);
    /// Serializes the cursor, pending deliveries, and cached state together.
    [[nodiscard]] std::string saveRecord() const;
    /// Retires an event only after the platform has accepted its notification.
    ///
    /// - Parameter identifier: The exact stable identifier from `notificationAt`.
    void markDelivered(const std::string& identifier);
    /// Whether another page or the optional screen remains in this pass.
    [[nodiscard]] bool hasMore() const;
    [[nodiscard]] std::size_t pendingNotificationCount() const;
    [[nodiscard]] BackgroundRefreshNotification notificationAt(std::size_t) const;
    [[nodiscard]] std::size_t paneCount() const { return m_panes.size(); }
    [[nodiscard]] BackgroundRefreshPane paneAt(std::size_t) const;
    [[nodiscard]] bool hadRetentionGap() const { return m_hadRetentionGap; }
    [[nodiscard]] const std::string& error() const { return m_error; }

private:
    std::string m_epoch;
    std::string m_inventory;
    std::uint64_t m_cursor = 0;
    std::uint64_t m_hostNowUnixMilliseconds = 0;
    std::uint64_t m_requestNumber = 0;
    std::string m_requestID;
    std::string m_requestedScreenPane;
    std::size_t m_nextPaneOffset = 0;
    std::set<std::string> m_seenPanes;
    std::vector<BackgroundRefreshNotification> m_pending;
    std::vector<BackgroundRefreshPane> m_panes;
    bool m_hasMoreEvents = false;
    bool m_didAttemptScreen = false;
    bool m_hadRetentionGap = false;
    bool m_isMetadataIncomplete = false;
    std::string m_error;
};

} // namespace Ubiquitty::Session
