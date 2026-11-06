// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
//
// Source/Platform/POSIX/DatagramBatch.h — several datagrams per syscall, both
// directions.
//
// A QUIC client is the bulk receiver: checkpoints and updates stream down to
// it. A checkpoint moves at roughly 1350 bytes a packet, so a mebibyte is about
// 775 datagrams, and one syscall each is 775 crossings on the thread that also
// decrypts and delivers them.
//
// Linux reads a whole array in one `recvmmsg`. Everywhere else the same array
// is filled a datagram at a time. The difference is confined to the fill: what
// a caller receives, and every decision it then makes, is identical either way,
// so the packet-handling loop above it exists once rather than once per
// platform.
//
// The batch is not a queue. One fill takes what the socket has, up to the
// array's size, and reports how many; a caller loops until a fill comes back
// empty, exactly as it looped on single reads.
//
// The send side is the mirror, and it is the host's direction: a daemon
// streaming a snapshot writes the packets a client reads. Each one carries its
// own destination rather than a cached peer, because during migration a
// PATH_CHALLENGE goes to the new path while data still flows on the old one,
// and a batch that assumed one peer would send the challenge to the wrong
// address.

#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include <sys/socket.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace Ubiquitty::POSIX {

/// How many datagrams one fill may take.
///
/// A keyframe burst is hundreds of packets, so this is what decides how many
/// share a syscall. Past this the arrays cost more stack than the calls they
/// save are worth.
inline constexpr int datagramBatchSize = 16;

/// The largest datagram a slot holds.
///
/// Above any path MTU a QUIC connection negotiates, so a datagram is never
/// truncated into one -- a truncated packet fails authentication and is
/// dropped, which reads as a lossy link rather than as a buffer that was too
/// small.
inline constexpr std::size_t datagramSlotCapacity = 2048;

/// One fill's worth of received datagrams.
struct DatagramBatch {
    /// Each slot's bytes.
    std::array<std::array<std::uint8_t, datagramSlotCapacity>, datagramBatchSize> storage;
    /// Each slot's sender.
    std::array<sockaddr_storage, datagramBatchSize> from;
    /// Each slot's length, valid up to the count a fill returned.
    std::array<std::size_t, datagramBatchSize> length;
};

/// Datagrams held for one send.
///
/// Packets are copied in rather than referenced: ngtcp2 writes each one into
/// the same scratch buffer, so a batch holding pointers would send the last
/// packet ``count`` times. A copy of at most an MTU is what a syscall costs
/// many times over.
struct DatagramSendBatch {
    /// Each queued packet's bytes.
    std::array<std::array<std::uint8_t, datagramSlotCapacity>, datagramBatchSize> storage;
    /// Each queued packet's destination.
    std::array<sockaddr_storage, datagramBatchSize> to;
    /// Each destination's length, as the address family needs.
    std::array<socklen_t, datagramBatchSize> toLength;
    /// Each queued packet's length.
    std::array<std::size_t, datagramBatchSize> length;
    /// How many slots are queued.
    int count = 0;

    /// Whether another packet would not fit.
    [[nodiscard]] bool isFull() const { return count >= datagramBatchSize; }
};

/// Queues one packet for the next send.
///
/// A packet too large for a slot is dropped rather than truncated: a truncated
/// QUIC packet fails authentication at the peer, which reads as a lossy link
/// rather than as a buffer that was too small.
///
/// - Parameters:
///   - batch: The batch to append to; must not be full.
///   - packet: The packet's bytes, copied.
///   - destination: Where it goes.
///   - destinationLength: The address's length.
/// - Returns: Whether the packet was queued.
bool queueDatagram(DatagramSendBatch& batch, std::span<const std::uint8_t> packet,
    const sockaddr* destination, socklen_t destinationLength);

/// Sends every queued packet and empties the batch.
///
/// On Linux that is one ``sendmmsg``; elsewhere the same packets go one at a
/// time. Either way the batch is empty on return, so a caller that flushes on
/// a full batch and again at the end never sends a packet twice.
///
/// A partial send is not retried here. QUIC's own loss recovery resends what
/// the peer does not acknowledge, and a retry loop inside a flush would block
/// the write pump on a socket that has just said it is full.
///
/// - Parameters:
///   - socketDescriptor: The UDP socket.
///   - batch: The queued packets; emptied.
/// - Returns: How many packets the kernel accepted.
int flushDatagramBatch(int socketDescriptor, DatagramSendBatch& batch);

/// Takes up to ``datagramBatchSize`` datagrams from a non-blocking socket.
///
/// - Parameters:
///   - socketDescriptor: The bound UDP socket, non-blocking.
///   - batch: Filled from slot zero upward.
/// - Returns: How many datagrams were read, or -1 when the socket had none.
///   Zero is never returned, so a caller's drain loop has one exit.
[[nodiscard]] int fillDatagramBatch(int socketDescriptor, DatagramBatch& batch);

} // namespace Ubiquitty::POSIX
