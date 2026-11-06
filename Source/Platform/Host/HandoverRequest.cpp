// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Platform/Host/HandoverRequest.h"

#include "Core/Protocol/Sync/Wire.h"
#include "Platform/POSIX/ReceivedDescriptor.h"

#include <algorithm>
#include <cerrno>
#include <sys/socket.h>
#include <utility>

namespace Ubiquitty::Host {

HandoverRequestState HandoverRequestReader::receive(int channel, std::uint64_t nowMilliseconds, std::uint64_t deadlineMilliseconds)
{
    if (m_state != HandoverRequestState::Awaiting)
        return m_state;
    const auto refuse = [&] {
        m_listener.reset();
        m_request.reset();
        return m_state = HandoverRequestState::Refused;
    };
    // An ordinary first byte selects its route whenever it arrives; the
    // deadline bounds only a handover prefix and a silent peer.
    if (!m_received) {
        std::uint8_t first = 0;
        const auto count = ::recv(channel, &first, 1, MSG_PEEK | MSG_DONTWAIT);
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
            return nowMilliseconds >= deadlineMilliseconds ? refuse() : m_state;
        if (count != 1)
            return refuse();
        if (first != Protocol::Sync::handoverPreamble.front())
            return m_state = HandoverRequestState::Ordinary;
    }
    if (nowMilliseconds >= deadlineMilliseconds)
        return refuse();
    // Each read stops at a protocol boundary, preserving the descriptor's first
    // record byte and leaving any following transaction record in the socket.
    for (unsigned pass = 0; pass < 3; ++pass) {
        const std::size_t boundary = m_received < preambleBytes ? preambleBytes
            : m_received < preambleBytes + lengthBytes          ? preambleBytes + lengthBytes
                                                                : m_bytes.size();
        iovec part { m_bytes.data() + m_received, boundary - m_received };
        alignas(cmsghdr) std::array<char, POSIX::receivedDescriptorControlCapacity> control {};
        msghdr message {};
        message.msg_iov = &part;
        message.msg_iovlen = 1;
        message.msg_control = control.data();
        message.msg_controllen = control.size();
        const auto count = ::recvmsg(channel, &message, MSG_DONTWAIT | POSIX::receivedDescriptorFlags);
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
            return m_state;
        if (count <= 0)
            return refuse();
        auto descriptor = POSIX::takeReceivedDescriptor(message);
        if (!descriptor)
            return refuse();
        if (m_received == preambleBytes) {
            if (!*descriptor)
                return refuse();
            m_listener = std::move(*descriptor);
        } else if (*descriptor)
            return refuse();
        m_received += static_cast<std::size_t>(count);
        const auto seenPreamble = std::min(m_received, preambleBytes);
        if (!std::equal(m_bytes.begin(), m_bytes.begin() + seenPreamble, Protocol::Sync::handoverPreamble.begin()))
            return refuse();
        if (m_received == preambleBytes + lengthBytes) {
            Protocol::Sync::WireReader reader(std::span { m_bytes }.subspan(preambleBytes, lengthBytes));
            if (reader.u32() != requestBytes)
                return refuse();
        }
        if (m_received == m_bytes.size()) {
            const auto decoded = Protocol::Sync::decodeHandoverMessage(std::span { m_bytes }.subspan(preambleBytes + lengthBytes), {});
            const auto* request = decoded ? std::get_if<Protocol::Sync::HandoverRequest>(&*decoded) : nullptr;
            if (!request || !m_listener)
                return refuse();
            m_request = *request;
            return m_state = HandoverRequestState::Ready;
        }
    }
    return m_state;
}

std::optional<ReceivedHandoverRequest> HandoverRequestReader::take()
{
    if (m_state != HandoverRequestState::Ready || !m_request || !m_listener)
        return std::nullopt;
    m_state = HandoverRequestState::Consumed;
    return ReceivedHandoverRequest { *std::exchange(m_request, std::nullopt), std::move(m_listener) };
}

} // namespace Ubiquitty::Host
