// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Ubiquitty::Protocol::Sync {

/// A consent question holds no more than one frame's half in operation
/// parameters. Even a 4096-byte name escaped six bytes per byte fits.
inline constexpr std::size_t maxStructureConsentParametersBytes = 32 * 1024;
/// The daemon holds one question per requesting client and this many in all.
inline constexpr std::size_t maxPendingStructureConsents = 16;
/// A question expires after thirty seconds, whether a person saw it or not.
inline constexpr std::uint64_t structureConsentTimeoutMilliseconds = 30'000;

/// Whether a method changes the host's session tree.
///
/// - Parameter method: Its registry name.
/// - Returns: Whether this tree operation requires agent consent.
[[nodiscard]] constexpr bool isStructureMethod(std::string_view method)
{
    return method == "label.set" || method == "tags.set" || method == "workspace.rename"
        || method == "tab.rename" || method == "tab.move" || method == "window.close"
        || method == "window.activate" || method == "tab.activate" || method == "pane.focus"
        || method == "window.moveFocus" || method == "pane.zoom" || method == "pane.move" || method == "pane.swap"
        || method == "pane.extractTab" || method == "pane.extractWindow" || method == "tab.extractWindow"
        || method == "pane.moveToTab" || method == "tab.moveToPane" || method == "pane.moveToNewTab"
        || method == "tab.balance" || method == "pane.adjustRatio" || method == "divider.setRatio"
        || method == "workspace.addWindow" || method == "workspace.addTab" || method == "workspace.split" || method == "workspace.closeTab" || method == "workspace.closePane";
}

/// Whether a tree operation is bound to an observed structure revision.
/// - Parameter method: The registry method.
/// - Returns: Whether stale structure must refuse the operation.
[[nodiscard]] constexpr bool requiresStructureRevision(std::string_view method)
{
    return method == "tab.balance" || method == "pane.adjustRatio" || method == "divider.setRatio"
        || method == "workspace.addWindow" || method == "window.close" || method == "pane.zoom" || method == "window.moveFocus"
        || method == "pane.move" || method == "pane.swap"
        || method == "pane.extractTab" || method == "pane.extractWindow" || method == "tab.extractWindow"
        || method == "pane.moveToTab" || method == "tab.moveToPane" || method == "pane.moveToNewTab";
}

/// Whether a revision a tree operation was asked under still describes its
/// target.
///
/// A structure revises for what it says of each pane, its cells and who is
/// attached, as well as for its arrangement. A layout operation asked about
/// the arrangement that still stands means what it meant, so it stays current
/// until the arrangement next changes: a person's unzoom is not refused
/// because the pane resized under a link that had not yet said so, and an
/// approval that took seconds is not refused because a shell printed. Closing
/// a window, a tab or a pane ends the sessions its question named, which the
/// arrangement does not hold, so it stays bound to the exact revision.
///
/// - Parameters:
///   - method: The registry method.
///   - asked: The revision the requester observed; zero is never current.
///   - current: The structure's revision now.
///   - arrangedAt: The revision at which its arrangement last changed.
/// - Returns: Whether the operation may proceed against the current target.
[[nodiscard]] constexpr bool isStructureRevisionCurrent(std::string_view method, std::uint64_t asked, std::uint64_t current, std::uint64_t arrangedAt)
{
    if (asked == 0 || asked > current)
        return false;
    if (method == "window.close" || method == "workspace.closeTab" || method == "workspace.closePane")
        return asked == current;
    return asked >= arrangedAt;
}

/// Whether a tree operation needs the requester's session-creation scope.
///
/// - Parameter method: The registry method.
/// - Returns: Whether the operation creates a session, in addition to editing.
[[nodiscard]] constexpr bool createsSessionForStructure(std::string_view method)
{
    return method == "workspace.addWindow" || method == "workspace.addTab" || method == "workspace.split";
}

/// The ordinary host-call wait, including a bounded human decision where the
/// operation requires one. The five-second margin lets the host's timeout
/// refusal arrive instead of a client abandoning the question first.
///
/// - Parameter method: The registry method.
/// - Returns: Milliseconds, without changing an explicitly configured wait.
[[nodiscard]] constexpr int hostCallWaitMilliseconds(std::string_view method)
{
    return isStructureMethod(method) ? static_cast<int>(structureConsentTimeoutMilliseconds) + 5000 : 5000;
}

/// One exact tree operation awaiting an attached person's approval.
/// The parameters describe the question only: the host executes its own
/// immutable original request, never fields returned by the frontend.
struct StructureConsent {
    std::uint32_t requestID = 0;
    /// One of the recipient's attached sessions, for routing to its window.
    std::uint32_t sessionID = 0;
    std::uint32_t workspaceID = 0;
    std::uint64_t revision = 0;
    std::string workspaceName;
    std::string method;
    /// The validated method parameters, encoded as one JSON object.
    std::string parameters;
    /// The requester's claimed label, not a verified identity.
    std::string agentLabel;
    /// The process the daemon verified, or its remote-peer description.
    std::string peerProcess;
    std::uint32_t peerProcessID = 0;

    friend bool operator==(const StructureConsent&, const StructureConsent&) = default;
};

/// Encodes a question as a `StructureConsent` `Notify` body:
///
///     request:u32 session:u32 workspace:u32 revision:u64 workspaceName:utf8
///     method:utf8 parameters:utf8 agentLabel:utf8 peerProcess:utf8 peerProcessID:u32
///
/// `parameters` is the operation's JSON object, as the method's schema reads it.
///
/// - Parameter request: The question.
/// - Returns: The whole body, or nothing for invalid or oversized data.
[[nodiscard]] std::optional<std::vector<std::uint8_t>> structureConsentNotify(const StructureConsent& request);

/// Reads a `StructureConsent` record, refusing an unknown method, parameters
/// the method's schema rejects and malformed fields before the frontend can
/// ask a misleading question.
///
/// - Parameter record: A `StructureConsent` `Notify`'s record.
/// - Returns: The question, or nothing if it is not a valid structure consent.
[[nodiscard]] std::optional<StructureConsent> decodeStructureConsent(std::span<const std::uint8_t> record);

} // namespace Ubiquitty::Protocol::Sync
