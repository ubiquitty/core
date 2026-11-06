// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/Protocol/Local/Control.h"
#include "Core/Protocol/Sync/EffectCodec.h"

#include <functional>
#include <memory>
#include <string_view>

namespace Ubiquitty::Protocol::Local {

/// A helper may inspect routing without reading the selected client's clipboard.
enum class LocalClipboardKind : std::uint8_t {
    Types = 0,
    Item = 1,
    Route = 2,
};

/// No session, principal or process identity is accepted from helper bytes.
struct LocalClipboardRequest {
    LocalClipboardKind kind = LocalClipboardKind::Types;
    std::string_view mime;
};

/// A correlated local result. Payload views borrow the complete reply's owner.
struct LocalClipboardReply {
    std::uint64_t correlation = 0;
    std::uint64_t daemonInstanceID = 0;
    Sync::EffectClipboardStatus status = Sync::EffectClipboardStatus::Empty;
    bool isHostSelected = false;
    Sync::EffectClipboardBundle bundle {};
};

inline constexpr std::size_t localClipboardReplyHeaderBytes = localControlReplyBytes + 1 + sizeof(std::uint32_t);

/// Encodes one complete F9 request, not a remote Sync frame or greeting.
/// - Parameters:
///   - correlation: Nonzero identity for this one local exchange.
///   - request: An inventory, exact item, or route-only probe.
/// - Returns: Bounded owned bytes, or a refusal before allocation.
[[nodiscard]] std::expected<Sync::EffectCodecBytes, LocalControlError> encodeLocalClipboardRequest(std::uint64_t correlation, const LocalClipboardRequest& request);

/// Measures the complete reply before its output owner is reserved.
/// - Parameter reply: One result with nonzero correlation and serving identity.
/// - Returns: Exact bytes, including the header, or a refusal.
[[nodiscard]] std::expected<std::size_t, LocalControlError> localClipboardReplyBytes(const LocalClipboardReply& reply);

/// Writes a whole reply directly into its already admitted output owner.
/// - Parameters:
///   - reply: Validated metadata and borrowed canonical clipboard items.
///   - output: Exactly localClipboardReplyBytes bytes of stable storage.
/// - Returns: A refusal before writing if metadata or storage is invalid.
[[nodiscard]] std::expected<void, LocalControlError> writeLocalClipboardReply(const LocalClipboardReply& reply, std::span<std::uint8_t> output);

/// Reads the bounded request body only after the transport establishes authority.
class LocalClipboardRequestReader {
public:
    /// Tracks one bounded body through validation and one-consumer delivery.
    enum class State : std::uint8_t {
        Awaiting = 0,
        Ready = 1,
        Refused = 2,
        Consumed = 3
    };

    /// Refuses contradictory length/kind/MIME headers before allocating a body.
    ///
    /// - Parameters:
    ///   - bytes: The next suffix of this operation's body, without its prefix.
    ///   - admitsAllocation: Optional owner-budget check before the sole MIME allocation.
    /// - Returns: The body's permanent parsing or delivery state.
    [[nodiscard]] State consume(std::span<const std::uint8_t> bytes, const std::function<bool(std::size_t)>& admitsAllocation = {});
    /// Returns one request whose MIME borrows this reader until destruction.
    /// - Returns: The complete request once, or nothing before validation or after delivery.
    [[nodiscard]] std::optional<LocalClipboardRequest> take();
    /// Reports whether the selected operation still awaits a complete body.
    [[nodiscard]] bool hasPendingRequest() const { return m_state == State::Awaiting; }
    /// Reports retained header and actual MIME storage, including after delivery.
    [[nodiscard]] std::size_t retainedBytes() const { return m_headerReceived + (m_mime ? m_mimeLength : 0); }

private:
    std::array<std::uint8_t, 7> m_header {};
    std::unique_ptr<char[]> m_mime;
    std::size_t m_headerReceived = 0;
    std::size_t m_mimeLength = 0;
    std::size_t m_mimeReceived = 0;
    State m_state = State::Awaiting;
};

/// Reads one reply with exact correlation, canonical payload and no trailing data.
class LocalClipboardReplyReader {
public:
    /// Pins the one nonzero request correlation before accepting any reply byte.
    /// - Parameter correlation: The identity sent on this local connection.
    explicit LocalClipboardReplyReader(std::uint64_t correlation)
        : m_correlation(correlation)
        , m_state(correlation ? LocalClipboardRequestReader::State::Awaiting : LocalClipboardRequestReader::State::Refused)
    {
    }
    /// Validates a reply header before allocating its bounded canonical payload.
    /// - Parameter bytes: The next part of this connection's sole reply.
    /// - Returns: Its permanent parsing or delivery state.
    [[nodiscard]] LocalClipboardRequestReader::State consume(std::span<const std::uint8_t> bytes);
    /// Returns views that borrow this reader's retained payload owner.
    /// - Returns: One validated result, or nothing before validation or after delivery.
    [[nodiscard]] std::optional<LocalClipboardReply> take();

private:
    std::array<std::uint8_t, localClipboardReplyHeaderBytes> m_header {};
    std::unique_ptr<std::uint8_t[]> m_body;
    std::uint64_t m_correlation = 0;
    std::size_t m_headerReceived = 0;
    std::size_t m_bodyLength = 0;
    std::size_t m_bodyReceived = 0;
    LocalClipboardReply m_reply;
    LocalClipboardRequestReader::State m_state;
};

} // namespace Ubiquitty::Protocol::Local
