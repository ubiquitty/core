// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Protocol/Sync/HandoverChannel.h"
#include "Core/Protocol/Sync/Wire.h"

#include <algorithm>
#include <limits>
#include <type_traits>

namespace Ubiquitty::Protocol::Sync {

bool isHandoverPreamble(std::span<const std::uint8_t> bytes)
{
    return std::ranges::equal(bytes, handoverPreamble);
}

std::vector<std::uint8_t> encodeHandoverMessage(const HandoverMessage& message)
{
    std::vector<std::uint8_t> bytes;
    WireWriter writer(bytes);
    const auto identity = [&](ID128 value) {
        writer.u64(value.low);
        writer.u64(value.high);
    };
    const auto kind = [&](HandoverMessageKind value) { writer.u8(static_cast<std::uint8_t>(value)); };
    std::visit([&](const auto& value) {
        using Value = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Value, HandoverRequest>) {
            kind(HandoverMessageKind::Request);
            writer.u16(handoverChannelVersion);
            writer.u16(value.maxBodyVersion);
            identity(value.requesterInstance);
        } else if constexpr (std::is_same_v<Value, HandoverOffer>) {
            kind(HandoverMessageKind::Offer);
            writer.u16(value.bodyVersion);
            identity(value.predecessorInstance);
            writer.u16(value.paneCount);
            writer.u32(value.tableBytes);
            writer.u8(value.listeners);
            writer.u8(value.datagramSockets);
        } else if constexpr (std::is_same_v<Value, HandoverPaneFollows>) {
            kind(HandoverMessageKind::PaneFollows);
            writer.u16(value.index);
            identity(value.paneID);
            writer.u32(value.recordBytes);
            writer.u32(value.sessionLeader);
            writer.u32(value.leaderParent);
        } else if constexpr (std::is_same_v<Value, HandoverRefuse>) {
            kind(HandoverMessageKind::Refuse);
            writer.u8(static_cast<std::uint8_t>(value.reason));
        } else if constexpr (std::is_same_v<Value, HandoverAccept>)
            kind(HandoverMessageKind::Accept);
        else if constexpr (std::is_same_v<Value, HandoverRelease>)
            kind(HandoverMessageKind::Release);
        else if constexpr (std::is_same_v<Value, HandoverServing>)
            kind(HandoverMessageKind::Serving);
    },
        message);
    return bytes;
}

std::expected<HandoverMessage, HandoverDecodeError> decodeHandoverMessage(std::span<const std::uint8_t> bytes, const HandoverChannelLimits& limits)
{
    WireReader reader(bytes);
    const auto identity = [&] { return ID128 { reader.u64(), reader.u64() }; };
    HandoverMessage message;
    switch (static_cast<HandoverMessageKind>(reader.u8())) {
    case HandoverMessageKind::Request: {
        const auto version = reader.u16();
        if (reader.ok() && version > handoverChannelVersion)
            return std::unexpected(HandoverDecodeError::NewerChannel);
        if (version != handoverChannelVersion)
            return std::unexpected(HandoverDecodeError::Malformed);
        HandoverRequest request { reader.u16(), identity() };
        if (!request.maxBodyVersion || request.requesterInstance == ID128 {})
            return std::unexpected(HandoverDecodeError::Malformed);
        message = request;
        break;
    }
    case HandoverMessageKind::Offer: {
        const auto version = reader.u16();
        if (reader.ok() && version > limits.maxBodyVersion)
            return std::unexpected(HandoverDecodeError::NewerBody);
        HandoverOffer offer { version, identity(), reader.u16(), reader.u32(), reader.u8(), reader.u8() };
        if (!version || offer.predecessorInstance == ID128 {} || !offer.listeners || offer.listeners > maxHandoverListeners
            || offer.datagramSockets > 1)
            return std::unexpected(HandoverDecodeError::Malformed);
        if (offer.paneCount > limits.maxPanes || offer.tableBytes > limits.maxTableBytes)
            return std::unexpected(HandoverDecodeError::OverCapacity);
        message = offer;
        break;
    }
    case HandoverMessageKind::PaneFollows: {
        HandoverPaneFollows pane { reader.u16(), identity(), reader.u32(), reader.u32(), reader.u32() };
        // A leader is a real process other than init; its parent may be init,
        // which adopts a leader whose original holder has exited.
        if (pane.paneID == ID128 {} || !pane.recordBytes || pane.sessionLeader <= 1 || !pane.leaderParent
            || pane.sessionLeader > static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max())
            || pane.leaderParent > static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max()))
            return std::unexpected(HandoverDecodeError::Malformed);
        if (pane.index >= limits.maxPanes || pane.recordBytes > limits.maxPaneBytes)
            return std::unexpected(HandoverDecodeError::OverCapacity);
        message = pane;
        break;
    }
    case HandoverMessageKind::Accept:
        message = HandoverAccept {};
        break;
    case HandoverMessageKind::Refuse: {
        const auto reason = reader.u8();
        if (reason < static_cast<std::uint8_t>(HandoverRefuseReason::NotSuccessor) || reason > static_cast<std::uint8_t>(lastHandoverRefuseReason))
            return std::unexpected(HandoverDecodeError::Malformed);
        message = HandoverRefuse { static_cast<HandoverRefuseReason>(reason) };
        break;
    }
    case HandoverMessageKind::Release:
        message = HandoverRelease {};
        break;
    case HandoverMessageKind::Serving:
        message = HandoverServing {};
        break;
    default:
        return std::unexpected(HandoverDecodeError::Malformed);
    }
    if (!reader.ok() || !reader.isAtEnd())
        return std::unexpected(HandoverDecodeError::Malformed);
    return message;
}

} // namespace Ubiquitty::Protocol::Sync
