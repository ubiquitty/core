// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/Protocol/Sync/Messages.h"

#include <array>
#include <expected>
#include <variant>

namespace Ubiquitty::Protocol::Sync {

/// Identifies the local successor channel before any remote frame decoder runs.
inline constexpr std::array<std::uint8_t, 16> handoverPreamble { 0xF8, 'U', 'B', 'Q', 'H', 'A', 'N', 'D', 'O', 'V', 'E', 'R', 0, 0, 0, 1 };
inline constexpr std::uint16_t handoverChannelVersion = 1;

/// Discriminates records on an authenticated, exclusively owned local channel.
enum class HandoverMessageKind : std::uint8_t {
    Request = 1,
    Offer,
    PaneFollows,
    Accept,
    Refuse,
    Release,
    Serving
};
/// Identifies why the predecessor must retain its panes and continue serving.
/// What it names reaches the person running the install, so each blocker the
/// holder knows is told apart from a pane that did not reach ground.
enum class HandoverRefuseReason : std::uint8_t {
    NotSuccessor = 1,
    NewerBody,
    /// A pane did not reach ground in time.
    PaneBusy,
    Malformed,
    Geometry,
    Ancestry,
    /// An agent's change to a workspace's tree waits for a person's approval.
    ApprovalPending,
    /// A capability question waits for a person's answer.
    GrantPending,
    /// A client has not yet been sent everything the holder owes it.
    OutputUnsent,
    /// A client's request is part-way: a frame half read, a call, a consent,
    /// a clipboard read or a local request still unanswered.
    RequestPending,
    /// A local session adoption is under way or its outcome unreported.
    AdoptPending,
};

/// The last reason a `HandoverRefuse` carries.
inline constexpr HandoverRefuseReason lastHandoverRefuseReason = HandoverRefuseReason::AdoptPending;
/// Reports a record rejected before its announced state body can be allocated.
enum class HandoverDecodeError : std::uint8_t {
    Malformed,
    NewerChannel,
    NewerBody,
    OverCapacity
};

/// Names the requester and the newest state format it can read.
struct HandoverRequest {
    std::uint16_t maxBodyVersion = 0;
    ID128 requesterInstance;
    bool operator==(const HandoverRequest&) const = default;
};

/// The most listening sockets one Release carries: the holder's own and the
/// predecessors' it serves, so a path a client pinned keeps an owner across a
/// chain of handovers. Past the bound the holder keeps the one its records
/// are kept beside and lets the oldest of the others go.
inline constexpr std::uint8_t maxHandoverListeners = 8;

/// Announces bounded state from the predecessor without transferring ownership.
///
/// `listeners` and `datagramSockets` name exactly how many descriptors the
/// eventual Release carries, listening sockets first, so the receiver bounds
/// them before any arrives.
struct HandoverOffer {
    std::uint16_t bodyVersion = 0;
    ID128 predecessorInstance;
    std::uint16_t paneCount = 0;
    std::uint32_t tableBytes = 0;
    std::uint8_t listeners = 0;
    std::uint8_t datagramSockets = 0;
    bool operator==(const HandoverOffer&) const = default;
};

/// Associates the following state record and descriptor with one offered pane.
///
/// The holder reads `sessionLeader` from its own master (`tcgetsid`) and
/// `leaderParent` from the kernel's record of that leader. They name what the
/// receiver must corroborate against the received master before it owns the
/// PTY; they grant nothing by themselves.
struct HandoverPaneFollows {
    std::uint16_t index = 0;
    ID128 paneID;
    std::uint32_t recordBytes = 0;
    std::uint32_t sessionLeader = 0;
    std::uint32_t leaderParent = 0;
    bool operator==(const HandoverPaneFollows&) const = default;
};

/// Confirms that the requester has restored every offered pane without reading PTYs.
struct HandoverAccept {
    bool operator==(const HandoverAccept&) const = default;
};
/// Refuses the transaction without granting serving authority to the requester.
struct HandoverRefuse {
    HandoverRefuseReason reason = HandoverRefuseReason::Malformed;
    bool operator==(const HandoverRefuse&) const = default;
};
/// Permits the restored requester to begin serving its inherited descriptors.
/// It carries the listening and datagram sockets its Offer announced.
struct HandoverRelease {
    bool operator==(const HandoverRelease&) const = default;
};
/// Confirms successor service so the predecessor can retire its descriptors.
struct HandoverServing {
    bool operator==(const HandoverServing&) const = default;
};

using HandoverMessage = std::variant<HandoverRequest, HandoverOffer, HandoverPaneFollows, HandoverAccept, HandoverRefuse, HandoverRelease, HandoverServing>;

/// Receiver-owned allocation ceilings, applied before any announced body is read.
struct HandoverChannelLimits {
    std::uint16_t maxBodyVersion = 0;
    std::uint16_t maxPanes = 0;
    std::uint32_t maxPaneBytes = 0;
    std::uint32_t maxTableBytes = 0;
};

/// Matches exactly one complete preamble; a prefix is not an admitted channel.
///
/// - Parameter bytes: The complete candidate preamble.
/// - Returns: Whether every byte matches and no suffix remains.
[[nodiscard]] bool isHandoverPreamble(std::span<const std::uint8_t>);

/// Encodes one control record, without the preamble or the transport length.
/// The exclusive authenticated local socket scopes the transaction. These bytes
/// neither authenticate its peer nor grant PTY ownership or permission to serve.
///
/// - Parameter message: The control record, validated by its transaction owner.
/// - Returns: Its field-wise, little-endian bytes.
[[nodiscard]] std::vector<std::uint8_t> encodeHandoverMessage(const HandoverMessage&);

/// Decodes one complete control record without allocating its announced body.
/// The owner separately verifies peer authority, ordering, indexes and identities
/// against its current transaction before receiving descriptors or changing state.
///
/// - Parameters:
///   - bytes: Exactly one control record.
///   - limits: The receiver's supported version and allocation ceilings.
/// - Returns: The decoded record or its refusal, with no state body allocated.
[[nodiscard]] std::expected<HandoverMessage, HandoverDecodeError> decodeHandoverMessage(std::span<const std::uint8_t>, const HandoverChannelLimits&);

} // namespace Ubiquitty::Protocol::Sync
