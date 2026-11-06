// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/Codec/JSON.h"
#include "Core/OSC/Activity.h"
#include "Core/OSC/KittyNotification.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Ubiquitty::Session {

/// One notification or unresolved program question retained for sleeping clients.
struct BackgroundEvent {
    std::uint64_t sequence = 0;
    std::uint64_t createdUnixMilliseconds = 0;
    std::string paneID;
    std::uint32_t sessionID = 0;
    std::string title;
    std::string body;
    /// Empty for a notification, otherwise the OSC 7501 identifier.
    std::string statusID;
    /// The program notification replacement key, scoped to the pane.
    std::string notificationID;
    NotificationOccasion occasion = NotificationOccasion::Always;
    bool isCurrent = true;

    friend bool operator==(const BackgroundEvent&, const BackgroundEvent&) = default;
};

/// Holds an independently acknowledged, bounded inbox without owning a session attachment.
class BackgroundEvents {
public:
    /// Exact journal counters and retained event order, without disk-load revision changes.
    struct Continuation {
        std::uint64_t epoch = 0;
        std::uint64_t latest = 0;
        std::uint64_t droppedThrough = 0;
        std::uint64_t revision = 0;
        std::vector<BackgroundEvent> events;
    };

    /// Copies retained state without pruning, changing its epoch or advancing revision.
    /// - Returns: Original clocks, cursors and events.
    [[nodiscard]] Continuation continuation() const { return { m_epoch, m_latest, m_droppedThrough, m_revision, m_events }; }

    /// Validates journal state without pruning or changing its owners.
    /// - Parameter state: Exact predecessor counters and retained events.
    /// - Returns: Whether the bounded state can continue under its original epoch.
    [[nodiscard]] static bool isValidContinuation(const Continuation& state);

    /// Restores a fresh candidate constructed with the predecessor's journal epoch.
    /// - Parameter state: Exact bounded journal, consumed only on success.
    /// - Returns: Whether every event and counter is valid for that epoch.
    [[nodiscard]] bool restoreContinuation(Continuation&& state);

    static constexpr std::size_t capacity = 256;
    static constexpr std::size_t pageCapacity = 64;
    static constexpr std::size_t titleCapacity = 192;
    static constexpr std::size_t bodyCapacity = 2048;
    static constexpr std::size_t statusCapacity = 128;
    static constexpr std::size_t notificationIDCapacity = OSC::kittyNotificationIdentifierCapacity;
    static constexpr std::size_t maxFileBytes = 4 * 1024 * 1024;
    static constexpr std::uint64_t lifetimeMilliseconds = 7 * 24 * 60 * 60 * 1000ULL;

    explicit BackgroundEvents(std::uint64_t epoch)
        : m_epoch(epoch)
    {
    }

    /// Retains an admitted notification independently of live banner delivery.
    ///
    /// - Parameters:
    ///   - paneID: The canonical host-scoped pane identity.
    ///   - sessionID: The live session owning the notification.
    ///   - request: The admitted notification and replacement key.
    ///   - now: The creation time in Unix milliseconds.
    void appendNotification(std::string paneID, std::uint32_t sessionID, const NotificationRequest& request, std::uint64_t now);
    /// Reconciles authoritative status snapshots; an unchanged question cannot be reissued after journal eviction.
    ///
    /// - Parameters:
    ///   - paneID: The canonical host-scoped pane identity.
    ///   - sessionID: The live session owning the records.
    ///   - previous: The preceding authoritative status snapshot.
    ///   - records: The replacement authoritative status snapshot.
    ///   - now: The creation time for new questions in Unix milliseconds.
    void updateQuestions(std::string_view paneID, std::uint32_t sessionID, std::span<const OSC::ProgramStatus> previous, std::span<const OSC::ProgramStatus> records, std::uint64_t now);
    /// Withdraws a removed pane's questions, retaining informational notifications.
    ///
    /// - Parameter paneID: The canonical identity of the removed pane.
    void removePane(std::string_view paneID);

    /// One bounded scan result, including withdrawn-record and retention progress.
    struct Page {
        std::uint64_t cursor = 0;
        bool hasMore = false;
        bool hasGap = false;
        std::vector<BackgroundEvent> events;
    };

    /// Scans up to one page of deliverable events; the cursor advances across withdrawn records too.
    [[nodiscard]] Page since(std::uint64_t after, std::uint64_t now) const;
    [[nodiscard]] std::uint64_t epoch() const { return m_epoch; }
    [[nodiscard]] std::uint64_t revision() const { return m_revision; }
    [[nodiscard]] Codec::JSON::Value persistentState() const;
    /// Restores only a complete valid bounded journal; refusal leaves this log unchanged.
    [[nodiscard]] bool restore(const Codec::JSON::Value&);

private:
    /// Appends a validated event with a fresh sequence, evicting the oldest at capacity.
    ///
    /// - Parameter event: The event whose text and identity have been bounded by the caller.
    void append(BackgroundEvent event);
    std::uint64_t m_epoch = 0;
    std::uint64_t m_latest = 0;
    std::uint64_t m_droppedThrough = 0;
    std::uint64_t m_revision = 0;
    std::vector<BackgroundEvent> m_events;
};

/// Repairs invalid UTF-8 and truncates event text without splitting a sequence.
[[nodiscard]] std::string backgroundEventText(std::string_view, std::size_t capacity);
/// Parses the notification occasion in the passive refresh representation.
[[nodiscard]] std::optional<NotificationOccasion> backgroundNotificationOccasion(std::string_view);
/// Writes the canonical notification occasion name.
[[nodiscard]] std::string_view backgroundNotificationOccasionName(NotificationOccasion);
/// Parses a lossless decimal unsigned JSON string, refusing signs and trailing bytes.
[[nodiscard]] std::optional<std::uint64_t> backgroundSequence(std::string_view);
/// Formats a pane identity in the passive refresh contract's canonical hex form.
[[nodiscard]] std::string backgroundPaneID(std::uint64_t high, std::uint64_t low);
/// Validates the canonical lowercase 128-bit pane identity.
[[nodiscard]] bool isValidBackgroundPaneID(std::string_view);
/// Writes an event in the passive refresh response's representation.
[[nodiscard]] Codec::JSON::Value backgroundEventJSON(const BackgroundEvent&);

} // namespace Ubiquitty::Session
