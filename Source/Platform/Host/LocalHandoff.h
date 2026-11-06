// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/Protocol/Local/AdoptWire.h"
#include "Platform/Host/LocalControl.h"

#include <cstddef>
#include <cstdint>

namespace Ubiquitty::Host {

/// Facts about the exact byte position of the selected descriptor-bearing lane.
struct LocalHandoffDescriptorPosition {
    bool isSelectedHandoff = false;
    bool hasReceivedRecord = false;
    bool hasPendingDescriptor = false;
    std::size_t pendingRecordBytes = 0;
};

/// Requires a one-byte recvmsg at the first record byte, with its sole descriptor.
/// A larger read could combine preceding plain bytes with later ancillary data.
[[nodiscard]] constexpr bool awaitsLocalHandoffDescriptor(const LocalHandoffDescriptorPosition& position)
{
    return position.isSelectedHandoff && !position.hasReceivedRecord
        && !position.hasPendingDescriptor && !position.pendingRecordBytes;
}

/// Rejects both a missing first-byte descriptor and any descriptor arriving later.
[[nodiscard]] constexpr bool admitsLocalHandoffDescriptorPosition(const LocalHandoffDescriptorPosition& position, bool hasReceivedDescriptor)
{
    return awaitsLocalHandoffDescriptor(position) == hasReceivedDescriptor;
}

/// Isolates the kind byte of every record on the selected adoption exchange.
/// Prior records do not suppress the boundary needed by a later Prepare.
[[nodiscard]] constexpr bool awaitsLocalAdoptRecordKind(const LocalHandoffDescriptorPosition& position)
{
    return position.isSelectedHandoff && !position.pendingRecordBytes;
}

/// Checks descriptors against the actual isolated kind byte, never a guessed
/// first-operation role. Ancillary decoding separately owns and closes all FDs.
[[nodiscard]] constexpr bool admitsLocalAdoptDescriptorPosition(const LocalHandoffDescriptorPosition& position,
    std::uint8_t firstRecordByte, std::size_t receivedDescriptorCount)
{
    if (!position.isSelectedHandoff)
        return false;
    if (!awaitsLocalAdoptRecordKind(position))
        return !receivedDescriptorCount;
    if (position.hasPendingDescriptor)
        return false;
    switch (static_cast<Protocol::Local::AdoptWireMessage>(firstRecordByte)) {
    case Protocol::Local::AdoptWireMessage::Prepare:
        return receivedDescriptorCount == 1;
    case Protocol::Local::AdoptWireMessage::Commit:
    case Protocol::Local::AdoptWireMessage::Abort:
    case Protocol::Local::AdoptWireMessage::Status:
        return !receivedDescriptorCount;
    case Protocol::Local::AdoptWireMessage::Outcome:
    case Protocol::Local::AdoptWireMessage::Refused:
        return false;
    }
    return false;
}

/// Host-established facts for the separately selected descriptor-bearing lane.
struct LocalAdoptAuthority {
    LocalControlPeer peer;
    bool isSelectedHandoff = false;
    bool hasVerifiedProcess = false;
    bool hasCurrentProcessInstance = false;
};

/// Admits adoption only through the current trusted local process, never a token.
[[nodiscard]] constexpr bool admitsLocalAdopt(const LocalAdoptAuthority& authority)
{
    return authority.isSelectedHandoff && authority.hasVerifiedProcess && authority.hasCurrentProcessInstance
        && authority.peer.isSameUserLocalStream && authority.peer.isTrustedOwnerBinary && !authority.peer.hasRemoteGreeting;
}

/// Facts rederived from the exact live attachment and adopted pane at release.
struct LocalReclaimAuthority {
    LocalControlPeer peer;
    bool hasLiveAttachment = false;
    bool isGrantedHuman = false;
    bool hasCurrentAuthority = false;
    bool hasExactAdoptedTarget = false;
};

/// Keeps Observe-level attached-human policy without admitting fresh local IDs.
[[nodiscard]] constexpr bool admitsLocalReclaim(const LocalReclaimAuthority& authority)
{
    return authority.peer.isSameUserLocalStream && authority.peer.isTrustedOwnerBinary
        && authority.peer.hasRemoteGreeting && !authority.peer.hasDescriptors
        && authority.hasLiveAttachment && authority.isGrantedHuman && authority.hasCurrentAuthority
        && authority.hasExactAdoptedTarget;
}

} // namespace Ubiquitty::Host
