// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/Protocol/Sync/HandoverChannel.h"
#include "PAL/UnixFileDescriptor.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace Ubiquitty::Host {

/// Selects one physical stream's first-record route permanently.
enum class HandoverRequestState : std::uint8_t {
    Awaiting,
    Ordinary,
    Ready,
    Refused,
    Consumed,
};

/// Owns an untrusted request and its sole received listener descriptor.
struct ReceivedHandoverRequest {
    Protocol::Sync::HandoverRequest request;
    PAL::UnixFileDescriptor listener;
};

/// Reads only a local stream's first handover preamble and fixed-size Request.
///
/// An ordinary first byte is never consumed and permanently selects Ordinary.
/// A handover prefix is consumed incrementally, so a stalled prefix cannot keep
/// a level-triggered reactor readable. Request and descriptor admission remain
/// separate: Ready grants no authority and never enters the ordinary decoder.
/// The caller owns the channel exclusively and invokes this before any HELLO.
class HandoverRequestReader {
public:
    /// Reads immediately available bytes, never waiting or allocating a body.
    ///
    /// The caller also invokes this at its existing pre-HELLO deadline when no
    /// bytes arrive. Every refusal closes a received listener but not the channel.
    /// - Parameters:
    ///   - channel: Borrowed local stream, with no preceding ordinary bytes.
    ///   - nowMilliseconds: Current monotonic time.
    ///   - deadlineMilliseconds: Original connection admission deadline.
    /// - Returns: The permanent route or Awaiting for an incomplete request.
    [[nodiscard]] HandoverRequestState receive(int channel, std::uint64_t nowMilliseconds, std::uint64_t deadlineMilliseconds);

    /// Transfers one complete request to its separate authentication gate.
    /// - Returns: The sole owned request once, or nothing unless Ready.
    [[nodiscard]] std::optional<ReceivedHandoverRequest> take();

private:
    static constexpr std::size_t preambleBytes = Protocol::Sync::handoverPreamble.size();
    static constexpr std::size_t lengthBytes = sizeof(std::uint32_t);
    static constexpr std::size_t requestBytes = 1 + 2 + 2 + 16;
    std::array<std::uint8_t, preambleBytes + lengthBytes + requestBytes> m_bytes {};
    std::size_t m_received = 0;
    HandoverRequestState m_state = HandoverRequestState::Awaiting;
    PAL::UnixFileDescriptor m_listener;
    std::optional<Protocol::Sync::HandoverRequest> m_request;
};

} // namespace Ubiquitty::Host
