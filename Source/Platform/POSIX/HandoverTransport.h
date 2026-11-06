// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "PAL/UnixFileDescriptor.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>

namespace Ubiquitty::POSIX {

inline constexpr std::size_t maxHandoverDescriptors = 32;

/// Why a local record transfer did not finish.
enum class HandoverChannelError : std::uint8_t {
    Closed,
    Oversize,
    DescriptorCount,
    TimedOut,
    AllocationRefused,
    InvalidArgument,
    Fatal,
};

/// Owns a complete record and all descriptors received with its first byte.
struct HandoverRecord {
    std::unique_ptr<std::uint8_t[]> storage;
    std::size_t length = 0;
    std::array<PAL::UnixFileDescriptor, maxHandoverDescriptors> descriptors;
    std::size_t descriptorCount = 0;

    /// Returns the complete payload, without its four-byte length prefix.
    [[nodiscard]] std::span<const std::uint8_t> bytes() const { return { storage.get(), length }; }
};

/// Sends one length-prefixed record on an exclusively owned Unix stream socket.
///
/// The descriptors remain the caller's. The first successful send carries them
/// exactly once; partial writes finish on the same absolute deadline. The caller
/// authenticates the peer before calling and closes the channel after any error:
/// a failed transfer may have sent a prefix and cannot safely be retried in place.
/// No child, PTY, or serving authority changes merely by sending a record.
/// The socket is temporarily nonblocking; no other thread may use any duplicate
/// of this channel during the call. Success preserves its original file status
/// flags. An error attempts restoration, but the caller must abandon the socket.
///
/// - Parameters:
///   - socket: A connected AF_UNIX/SOCK_STREAM descriptor with one I/O owner.
///   - bytes: Payload, representable by a little-endian 32-bit length.
///   - descriptors: At most `maxHandoverDescriptors` borrowed descriptors.
///   - timeoutMilliseconds: Positive bound for the entire record.
/// - Returns: Completion, or the reason the channel must be abandoned.
[[nodiscard]] std::expected<void, HandoverChannelError> sendHandoverRecord(int socket, std::span<const std::uint8_t> bytes, std::span<const int> descriptors, int timeoutMilliseconds);

/// Receives one bounded record without consuming the following record's bytes.
///
/// Received descriptors are close-on-exec and every refusal closes all handles
/// already delivered. Descriptors after the first receive are refused. Length is
/// checked before allocating payload storage; allocation failure is recoverable.
/// The caller authenticates the peer and abandons the socket on every refusal.
/// As with sending, this temporarily sets nonblocking mode under exclusive
/// ownership of the channel and its duplicates. Success preserves the original
/// flags; an error attempts restoration before the caller abandons the socket.
///
/// - Parameters:
///   - socket: A connected AF_UNIX/SOCK_STREAM descriptor with one I/O owner.
///   - maxBytes: The caller's admitted payload bound.
///   - expectedDescriptors: Exact handle count, at most `maxHandoverDescriptors`.
///   - timeoutMilliseconds: Positive bound for the entire record.
/// - Returns: A complete owned record, or a refusal with no retained handles.
[[nodiscard]] std::expected<HandoverRecord, HandoverChannelError> receiveHandoverRecord(int socket, std::size_t maxBytes, std::size_t expectedDescriptors, int timeoutMilliseconds);

} // namespace Ubiquitty::POSIX
