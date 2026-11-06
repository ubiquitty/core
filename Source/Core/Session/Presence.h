// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/Protocol/Sync/Attachment.h"

#include <algorithm>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace Ubiquitty::Session {

// Local presentation bits only; these are not another wire grammar.
inline constexpr std::uint8_t presenceHuman = 0x01;
inline constexpr std::uint8_t presenceSelf = 0x02;
inline constexpr std::uint8_t presenceQUIC = 0x04;
inline constexpr std::uint8_t grantNone = 0;
inline constexpr std::uint8_t grantSession = 1;
inline constexpr std::uint8_t grantAlways = 2;

/// A pane-specific native projection of one canonical attachments row.
struct PaneAttendee {
    Protocol::Sync::ID128 attachmentID {};
    std::uint32_t clientID {}; // Display only; never a revoke selector.
    std::uint32_t peerProcessID {};
    std::uint8_t flags {};
    std::uint8_t grantScope = UINT8_MAX; // Unknown when optional metadata is absent.
    std::string label {}; // Claimed and never authority.
    std::string process {}; // Verified process facts; empty when unknown.
    std::string grantIdentity {}; // Separate daemon-derived grant selector.
};

/// A local projection only. The sole network roster is attachments Notify.
struct PanePresenceSnapshot {
    std::uint32_t sessionID {};
    std::vector<PaneAttendee> attendees;
};

/// Projects the one canonical roster for a native pane, without treating any
/// display field as authority. Lost and unrelated rows are not live viewers.
/// - Parameters:
///   - attachments: The rows of one complete strictly decoded notification.
///   - paneID: This pane's exact bound identity, including its original owner.
///   - sessionID: The native pane's local session label.
/// - Returns: Local display values; never another network representation.
[[nodiscard]] inline PanePresenceSnapshot presenceForPane(std::span<const Protocol::Sync::AttachmentEntry> attachments,
    Protocol::Sync::ID128 paneID, std::uint32_t sessionID)
{
    using namespace Protocol::Sync;
    PanePresenceSnapshot result { sessionID, {} };
    for (const auto& entry : attachments) {
        if (entry.state != AttachmentState::Live || std::ranges::find(entry.panes, paneID) == entry.panes.end())
            continue;
        PaneAttendee row {
            .attachmentID = entry.attachmentID,
            .flags = static_cast<std::uint8_t>((entry.clientKind != ClientKind::Agent ? presenceHuman : 0) | (entry.isSelf ? presenceSelf : 0)),
            .label = {},
            .process = {},
            .grantIdentity = {},
        };
        if (entry.presence) {
            const auto& facts = *entry.presence;
            row.clientID = facts.clientID;
            row.peerProcessID = facts.processID;
            row.label = facts.claimedLabel;
            row.process = facts.process;
            row.grantIdentity = facts.grantIdentity;
            if (facts.isQUIC)
                row.flags |= presenceQUIC;
            const auto grant = std::ranges::find(facts.sharedPanes, paneID, &SharedPaneGrant::paneID);
            if (grant != facts.sharedPanes.end()) {
                switch (grant->grant) {
                case PresenceGrant::None:
                    row.grantScope = grantNone;
                    break;
                case PresenceGrant::Session:
                    row.grantScope = grantSession;
                    break;
                case PresenceGrant::Always:
                    row.grantScope = grantAlways;
                    break;
                }
            }
        }
        result.attendees.push_back(std::move(row));
    }
    return result;
}

} // namespace Ubiquitty::Session
