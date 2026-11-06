// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Platform/Host/LocalControl.h"

namespace Ubiquitty::Host {

/// Facts established from filesystem metadata and kernel peer credentials.
struct LocalControlEndpoint {
    bool hasPrivateOwnerDirectory = false;
    bool hasPrivateOwnerSocket = false;
    bool hasSameUserPeer = false;
};

/// One bounded reload exchange, with no transport or clock of its own.
class LocalReloadExchange {
public:
    enum class State : std::uint8_t {
        AwaitingAuthority,
        Sending,
        AwaitingReply,
        Reloaded,
        Refused
    };

    /// Pins one correlation and the original absolute monotonic deadline.
    LocalReloadExchange(std::uint64_t correlation, std::uint64_t deadlineMilliseconds)
        : m_request(encodeLocalReloadRequest(correlation))
        , m_correlation(correlation)
        , m_deadlineMilliseconds(deadlineMilliseconds)
    {
        if (!correlation || !deadlineMilliseconds)
            m_state = State::Refused;
    }

    /// Establishes authority before the request can expose any outgoing byte.
    void authorize(const LocalControlEndpoint& endpoint, std::uint64_t nowMilliseconds)
    {
        if (state(nowMilliseconds) != State::AwaitingAuthority)
            return;
        m_state = endpoint.hasPrivateOwnerDirectory && endpoint.hasPrivateOwnerSocket && endpoint.hasSameUserPeer
            ? State::Sending
            : State::Refused;
    }

    /// The unconsumed request suffix; empty before authority or after refusal.
    [[nodiscard]] std::span<const std::uint8_t> outgoing(std::uint64_t nowMilliseconds)
    {
        return state(nowMilliseconds) == State::Sending ? std::span { m_request }.subspan(m_sent) : std::span<const std::uint8_t> {};
    }

    /// Records actual write progress, never a requested write's length.
    void didSend(std::size_t count, std::uint64_t nowMilliseconds)
    {
        if (state(nowMilliseconds) != State::Sending)
            return;
        if (!count || count > m_request.size() - m_sent) {
            m_state = State::Refused;
            return;
        }
        m_sent += count;
        if (m_sent == m_request.size())
            m_state = State::AwaitingReply;
    }

    /// Accepts exactly one matching reply only after every request byte left.
    void receive(std::span<const std::uint8_t> bytes, std::uint64_t nowMilliseconds)
    {
        const auto current = state(nowMilliseconds);
        if (current == State::Reloaded || current == State::Refused)
            return;
        if (current != State::AwaitingReply || bytes.size() > m_reply.size() - m_received) {
            m_state = State::Refused;
            return;
        }
        std::ranges::copy(bytes, m_reply.begin() + m_received);
        m_received += bytes.size();
        if (m_received != m_reply.size())
            return;
        const auto reply = decodeLocalControlReply(m_reply);
        m_state = reply && reply->correlation == m_correlation && reply->status == LocalControlStatus::Reloaded
            ? State::Reloaded
            : State::Refused;
    }

    /// Ends an incomplete exchange after EOF or a transport error.
    void refuse()
    {
        if (m_state != State::Reloaded)
            m_state = State::Refused;
    }

    /// Expiry is absolute: partial progress never renews the request's budget.
    [[nodiscard]] State state(std::uint64_t nowMilliseconds)
    {
        if (m_state != State::Reloaded && nowMilliseconds >= m_deadlineMilliseconds)
            m_state = State::Refused;
        return m_state;
    }

private:
    std::array<std::uint8_t, localControlRequestBytes> m_request;
    std::array<std::uint8_t, localControlReplyBytes> m_reply {};
    std::uint64_t m_correlation = 0;
    std::uint64_t m_deadlineMilliseconds = 0;
    std::size_t m_sent = 0;
    std::size_t m_received = 0;
    State m_state = State::AwaitingAuthority;
};

} // namespace Ubiquitty::Host
