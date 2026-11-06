// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "PAL/UnixFileDescriptor.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <fcntl.h>
#include <optional>
#include <span>
#include <sys/socket.h>

namespace Ubiquitty::POSIX {

// XNU permits at most 512 descriptors per control message and can install
// descriptors beyond a truncated receive buffer. Receive its entire limit;
// Linux's SCM_MAX_FD limit is smaller.
inline constexpr std::size_t receivedDescriptorControlCapacity = CMSG_SPACE(512 * sizeof(int));

#ifdef MSG_CMSG_CLOEXEC
inline constexpr int receivedDescriptorFlags = MSG_CMSG_CLOEXEC;
#else
inline constexpr int receivedDescriptorFlags = 0;
#endif

/// Takes ownership of a bounded batch from a completed `recvmsg` call.
///
/// The destination contains empty owners. A refusal closes every delivered
/// handle, including handles beyond the destination's capacity. An empty
/// optional rejects truncation, malformed ancillary data, and close-on-exec
/// failure; a successful count may be zero.
[[nodiscard]] inline std::optional<std::size_t> takeReceivedDescriptors(msghdr& message, std::span<PAL::UnixFileDescriptor> descriptors)
{
    std::size_t count = 0;
    bool invalid = (message.msg_flags & MSG_CTRUNC) != 0;
    for (cmsghdr* header = CMSG_FIRSTHDR(&message); header; header = CMSG_NXTHDR(&message, header)) {
        if (header->cmsg_level != SOL_SOCKET || header->cmsg_type != SCM_RIGHTS) {
            continue;
        }
        if (header->cmsg_len < CMSG_LEN(0)) {
            invalid = true;
            continue;
        }
        const auto* control = static_cast<const unsigned char*>(message.msg_control);
        // Both addresses name bytes within the kernel-filled control buffer.
        const auto* headerBytes = reinterpret_cast<const unsigned char*>(header);
        const std::size_t available = message.msg_controllen - static_cast<std::size_t>(headerBytes - control);
        const std::size_t headerLength = std::min<std::size_t>(header->cmsg_len, available);
        invalid |= header->cmsg_len > available;
        if (headerLength < CMSG_LEN(0)) {
            invalid = true;
            continue;
        }
        const std::size_t length = headerLength - CMSG_LEN(0);
        invalid |= length % sizeof(int) != 0;
        // CMSG_SPACE includes alignment padding that can hold extra installed
        // descriptors, even when the receive buffer was sized for one.
        for (std::size_t offset = 0; offset + sizeof(int) <= length; offset += sizeof(int)) {
            int received = -1;
            std::memcpy(&received, CMSG_DATA(header) + offset, sizeof(received));
            PAL::UnixFileDescriptor owner(received);
            if (!owner || count == descriptors.size()) {
                invalid = true;
                continue;
            }
            if (::fcntl(owner.get(), F_SETFD, FD_CLOEXEC) < 0) {
                invalid = true;
                continue;
            }
            descriptors[count++] = std::move(owner);
        }
    }
    if (invalid) {
        for (auto& descriptor : descriptors.first(count))
            descriptor.reset();
        return std::nullopt;
    }
    return count;
}

/// Takes ownership of at most one descriptor from a completed `recvmsg` call.
///
/// An empty optional rejects truncated or multiple descriptors after closing
/// every delivered descriptor. An engaged, empty owner means none arrived.
[[nodiscard]] inline std::optional<PAL::UnixFileDescriptor> takeReceivedDescriptor(msghdr& message)
{
    PAL::UnixFileDescriptor descriptor;
    if (!takeReceivedDescriptors(message, std::span { &descriptor, 1 }))
        return std::nullopt;
    return descriptor;
}

}
