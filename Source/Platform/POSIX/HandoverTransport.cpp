// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Platform/POSIX/HandoverTransport.h"

#include "Platform/POSIX/ReceivedDescriptor.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <limits>
#include <poll.h>
#include <sys/socket.h>

namespace Ubiquitty::POSIX {

namespace {

using Clock = std::chrono::steady_clock;

constexpr std::size_t recordHeaderBytes = sizeof(std::uint32_t);
constexpr std::size_t maxTransferBytes = 256 * 1024;

class NonblockingScope {
public:
    explicit NonblockingScope(int socket)
        : m_socket(socket)
        , m_originalFlags(::fcntl(socket, F_GETFL))
        , m_valid(m_originalFlags >= 0 && ::fcntl(socket, F_SETFL, m_originalFlags | O_NONBLOCK) == 0)
    {
    }

    NonblockingScope(const NonblockingScope&) = delete;
    NonblockingScope& operator=(const NonblockingScope&) = delete;

    ~NonblockingScope()
    {
        (void)restore();
    }

    bool valid() const { return m_valid; }

    [[nodiscard]] bool restore()
    {
        if (!m_valid)
            return true;
        int result;
        do {
            result = ::fcntl(m_socket, F_SETFL, m_originalFlags);
        } while (result < 0 && errno == EINTR);
        if (result < 0)
            return false;
        m_valid = false;
        return true;
    }

private:
    int m_socket;
    int m_originalFlags;
    bool m_valid;
};

bool isLocalStream(int socket)
{
    sockaddr_storage address {};
    socklen_t addressBytes = sizeof(address);
    int type = 0;
    socklen_t typeBytes = sizeof(type);
    return ::getsockname(socket, reinterpret_cast<sockaddr*>(&address), &addressBytes) == 0
        && address.ss_family == AF_UNIX
        && ::getsockopt(socket, SOL_SOCKET, SO_TYPE, &type, &typeBytes) == 0 && type == SOCK_STREAM;
}

std::expected<void, HandoverChannelError> awaitReady(int socket, short events, Clock::time_point deadline)
{
    for (;;) {
        const auto now = Clock::now();
        if (now >= deadline)
            return std::unexpected(HandoverChannelError::TimedOut);
        const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - now).count();
        pollfd descriptor { socket, events, 0 };
        const int result = ::poll(&descriptor, 1, static_cast<int>(std::min<std::int64_t>(remaining, std::numeric_limits<int>::max())));
        if (result > 0) {
            if (descriptor.revents & POLLNVAL)
                return std::unexpected(HandoverChannelError::InvalidArgument);
            // HUP/ERR are consumed by recv/send so buffered bytes remain readable.
            return {};
        }
        if (!result)
            return std::unexpected(HandoverChannelError::TimedOut);
        if (errno != EINTR)
            return std::unexpected(HandoverChannelError::Fatal);
    }
}

HandoverChannelError socketError(int error)
{
    if (error == EPIPE || error == ECONNRESET || error == ENOTCONN)
        return HandoverChannelError::Closed;
    return HandoverChannelError::Fatal;
}

std::expected<int, HandoverChannelError> sendFlags(int socket)
{
#ifdef MSG_NOSIGNAL
    (void)socket;
    return MSG_DONTWAIT | MSG_NOSIGNAL;
#elif defined(SO_NOSIGPIPE)
    const int suppressSignal = 1;
    if (::setsockopt(socket, SOL_SOCKET, SO_NOSIGPIPE, &suppressSignal, sizeof(suppressSignal)) == 0)
        return MSG_DONTWAIT;
    return std::unexpected(HandoverChannelError::Fatal);
#else
    (void)socket;
    return std::unexpected(HandoverChannelError::Fatal);
#endif
}

} // namespace

std::expected<void, HandoverChannelError> sendHandoverRecord(int socket, std::span<const std::uint8_t> bytes, std::span<const int> descriptors, int timeoutMilliseconds)
{
    if (timeoutMilliseconds <= 0 || !isLocalStream(socket))
        return std::unexpected(HandoverChannelError::InvalidArgument);
    if (bytes.size() > std::numeric_limits<std::uint32_t>::max())
        return std::unexpected(HandoverChannelError::Oversize);
    if (descriptors.size() > maxHandoverDescriptors)
        return std::unexpected(HandoverChannelError::DescriptorCount);
    for (const int descriptor : descriptors) {
        if (descriptor < 0)
            return std::unexpected(HandoverChannelError::InvalidArgument);
    }
    // MSG_DONTWAIT alone does not bound a large Unix-stream send on every
    // supported kernel. The exclusive channel owner also sets O_NONBLOCK.
    NonblockingScope nonblocking(socket);
    if (!nonblocking.valid())
        return std::unexpected(HandoverChannelError::Fatal);
    const auto flags = sendFlags(socket);
    if (!flags)
        return std::unexpected(flags.error());
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMilliseconds);
    std::array<std::uint8_t, recordHeaderBytes> header;
    for (std::size_t index = 0; index < header.size(); ++index)
        header[index] = static_cast<std::uint8_t>(bytes.size() >> (8 * index));
    union Control {
        char bytes[CMSG_SPACE(maxHandoverDescriptors * sizeof(int))];
        cmsghdr alignment;
    } control {};
    std::size_t offset = 0;
    // No allocation or payload-size addition: header and body are separate spans.
    for (const std::span<const std::uint8_t> part : { std::span<const std::uint8_t>(header), bytes }) {
        offset = 0;
        while (offset < part.size()) {
            auto ready = awaitReady(socket, POLLOUT, deadline);
            if (!ready)
                return ready;
            iovec buffer { const_cast<std::uint8_t*>(part.data() + offset), std::min(part.size() - offset, maxTransferBytes) };
            msghdr message {};
            message.msg_iov = &buffer;
            message.msg_iovlen = 1;
            if (part.data() == header.data() && !offset && !descriptors.empty()) {
                message.msg_control = control.bytes;
                // Each field's own type: socklen_t on Darwin, size_t on Linux.
                message.msg_controllen = static_cast<decltype(message.msg_controllen)>(CMSG_SPACE(descriptors.size_bytes()));
                auto* ancillary = CMSG_FIRSTHDR(&message);
                ancillary->cmsg_level = SOL_SOCKET;
                ancillary->cmsg_type = SCM_RIGHTS;
                ancillary->cmsg_len = static_cast<decltype(ancillary->cmsg_len)>(CMSG_LEN(descriptors.size_bytes()));
                std::memcpy(CMSG_DATA(ancillary), descriptors.data(), descriptors.size_bytes());
            }
            const auto sent = ::sendmsg(socket, &message, *flags);
            if (sent > 0) {
                offset += static_cast<std::size_t>(sent);
                continue;
            }
            if (sent < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
                continue;
            return std::unexpected(sent ? socketError(errno) : HandoverChannelError::Closed);
        }
    }
    if (!nonblocking.restore())
        return std::unexpected(HandoverChannelError::Fatal);
    return {};
}

std::expected<HandoverRecord, HandoverChannelError> receiveHandoverRecord(int socket, std::size_t maxBytes, std::size_t expectedDescriptors, int timeoutMilliseconds)
{
    if (timeoutMilliseconds <= 0 || !isLocalStream(socket))
        return std::unexpected(HandoverChannelError::InvalidArgument);
    if (expectedDescriptors > maxHandoverDescriptors)
        return std::unexpected(HandoverChannelError::DescriptorCount);
    NonblockingScope nonblocking(socket);
    if (!nonblocking.valid())
        return std::unexpected(HandoverChannelError::Fatal);
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMilliseconds);
    HandoverRecord record;
    std::array<std::uint8_t, recordHeaderBytes> header {};
    bool isFirstRead = true;
    auto receivePart = [&](std::span<std::uint8_t> bytes) -> std::expected<void, HandoverChannelError> {
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            auto ready = awaitReady(socket, POLLIN, deadline);
            if (!ready)
                return ready;
            // The full kernel limit matters even for a request expecting zero:
            // some kernels install handles beyond a truncated control buffer.
            union Control {
                char bytes[receivedDescriptorControlCapacity];
                cmsghdr alignment;
            } control {};
            iovec buffer { bytes.data() + offset, std::min(bytes.size() - offset, maxTransferBytes) };
            msghdr message {};
            message.msg_iov = &buffer;
            message.msg_iovlen = 1;
            message.msg_control = control.bytes;
            message.msg_controllen = sizeof(control.bytes);
            const auto received = ::recvmsg(socket, &message, MSG_DONTWAIT | receivedDescriptorFlags);
            if (received < 0) {
                if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                    continue;
                return std::unexpected(socketError(errno));
            }
            auto destination = std::span(record.descriptors).first(isFirstRead ? expectedDescriptors : 0);
            const auto count = takeReceivedDescriptors(message, destination);
            if (!received)
                return std::unexpected(HandoverChannelError::Closed);
            if (!count || (isFirstRead && *count != expectedDescriptors))
                return std::unexpected(HandoverChannelError::DescriptorCount);
            if (isFirstRead)
                record.descriptorCount = *count;
            isFirstRead = false;
            offset += static_cast<std::size_t>(received);
        }
        return {};
    };
    auto received = receivePart(header);
    if (!received)
        return std::unexpected(received.error());
    for (std::size_t index = 0; index < header.size(); ++index)
        record.length |= static_cast<std::size_t>(header[index]) << (8 * index);
    if (record.length > maxBytes)
        return std::unexpected(HandoverChannelError::Oversize);
    if (record.length) {
        record.storage.reset(new (std::nothrow) std::uint8_t[record.length]);
        if (!record.storage)
            return std::unexpected(HandoverChannelError::AllocationRefused);
        received = receivePart({ record.storage.get(), record.length });
        if (!received)
            return std::unexpected(received.error());
    }
    if (!nonblocking.restore())
        return std::unexpected(HandoverChannelError::Fatal);
    return record;
}

} // namespace Ubiquitty::POSIX
