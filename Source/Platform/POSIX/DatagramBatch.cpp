// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Platform/POSIX/DatagramBatch.h"

#include <array>
#include <cstring>

#include <sys/types.h>

namespace Ubiquitty::POSIX {

bool queueDatagram(DatagramSendBatch& batch, std::span<const std::uint8_t> packet,
    const sockaddr* destination, socklen_t destinationLength)
{
    if (batch.isFull() || packet.empty() || packet.size() > datagramSlotCapacity)
        return false;
    if (destination == nullptr || destinationLength == 0
        || static_cast<std::size_t>(destinationLength) > sizeof(sockaddr_storage))
        return false;
    const int slot = batch.count;
    std::memcpy(batch.storage[slot].data(), packet.data(), packet.size());
    std::memcpy(&batch.to[slot], destination, destinationLength);
    batch.toLength[slot] = destinationLength;
    batch.length[slot] = packet.size();
    batch.count = slot + 1;
    return true;
}

int flushDatagramBatch(int socketDescriptor, DatagramSendBatch& batch)
{
    const int queued = batch.count;
    batch.count = 0; // emptied whatever the kernel does, so no packet is sent twice
    if (queued <= 0)
        return 0;
#if defined(__linux__)
    std::array<mmsghdr, datagramBatchSize> messages {};
    std::array<iovec, datagramBatchSize> vectors {};
    for (int slot = 0; slot < queued; ++slot) {
        vectors[slot].iov_base = batch.storage[slot].data();
        vectors[slot].iov_len = batch.length[slot];
        messages[slot].msg_hdr.msg_iov = &vectors[slot];
        messages[slot].msg_hdr.msg_iovlen = 1;
        messages[slot].msg_hdr.msg_name = &batch.to[slot];
        messages[slot].msg_hdr.msg_namelen = batch.toLength[slot];
    }
    const int sent = ::sendmmsg(socketDescriptor, messages.data(), static_cast<unsigned>(queued), 0);
    return sent < 0 ? 0 : sent;
#else
    int sent = 0;
    for (int slot = 0; slot < queued; ++slot) {
        const ssize_t bytes = ::sendto(socketDescriptor, batch.storage[slot].data(), batch.length[slot], 0,
            reinterpret_cast<const sockaddr*>(&batch.to[slot]), batch.toLength[slot]);
        if (bytes < 0)
            break;
        ++sent;
    }
    return sent;
#endif
}

int fillDatagramBatch(int socketDescriptor, DatagramBatch& batch)
{
#if defined(__linux__)
    std::array<mmsghdr, datagramBatchSize> messages {};
    std::array<iovec, datagramBatchSize> vectors {};
    for (int slot = 0; slot < datagramBatchSize; ++slot) {
        vectors[slot].iov_base = batch.storage[slot].data();
        vectors[slot].iov_len = datagramSlotCapacity;
        messages[slot].msg_hdr.msg_iov = &vectors[slot];
        messages[slot].msg_hdr.msg_iovlen = 1;
        messages[slot].msg_hdr.msg_name = &batch.from[slot];
        messages[slot].msg_hdr.msg_namelen = sizeof(batch.from[slot]);
    }
    const int received = ::recvmmsg(socketDescriptor, messages.data(), datagramBatchSize, 0, nullptr);
    if (received <= 0)
        return -1; // EAGAIN, or a socket with nothing on it
    for (int slot = 0; slot < received; ++slot)
        batch.length[slot] = messages[slot].msg_len;
    return received;
#else
    // One at a time, into the same array. The loop stops at the first empty
    // read rather than at the array's end, so a caller waiting on a quiet
    // socket does not spin through fifteen more failing calls to learn that.
    int received = 0;
    while (received < datagramBatchSize) {
        socklen_t fromLength = sizeof(batch.from[received]);
        const ssize_t bytes = ::recvfrom(socketDescriptor, batch.storage[received].data(), datagramSlotCapacity, 0,
            reinterpret_cast<sockaddr*>(&batch.from[received]), &fromLength);
        if (bytes < 0)
            break;
        batch.length[received] = static_cast<std::size_t>(bytes);
        ++received;
    }
    return received > 0 ? received : -1;
#endif
}

} // namespace Ubiquitty::POSIX
