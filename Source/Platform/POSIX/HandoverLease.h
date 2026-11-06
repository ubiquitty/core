// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/Protocol/Sync/Messages.h"
#include "PAL/UnixFileDescriptor.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <sys/types.h>

namespace Ubiquitty::POSIX {

inline constexpr std::size_t maxHandoverLeaseOfferBytes = 4096;

/// Distinguishes validation, contention and transport failures without granting service.
enum class HandoverLeaseError : std::uint8_t {
    InvalidArgument,
    UnsafeDescriptor,
    WrongProcess,
    WrongPredecessor,
    NotHeld,
    Busy,
    SystemFailure,
    TransferFailed,
    InvalidMetadata,
    AlreadyBound,
    WrongSuccessor,
    SuccessorClaimed,
    AlreadyClaimed,
    SuccessorCancelled,
};

/// The authenticated process and request permitted to make the one-way claim.
struct HandoverLeaseClaim {
    pid_t successor = 0;
    Protocol::Sync::ID128 requester;
    friend bool operator==(const HandoverLeaseClaim&, const HandoverLeaseClaim&) = default;
};

/// Owns the sole local descriptor for an anonymous process-associated service lock.
///
/// A predecessor may resume only after successful `tryAcquire` and, once an
/// offer has been exposed, successful `cancelSuccessor`. A successor
/// must also complete `claimSuccessor` before committing state or servicing PTYs.
/// A successful claim permanently prevents predecessor rollback, even when the
/// successor dies before acknowledging that it is serving.
/// Service must stop before `release` or destruction. The caller serializes all
/// methods and authenticates the peer independently; this lock is not admission.
///
/// POSIX record locks are process-associated: closing *any* local descriptor for
/// this inode releases them. This owner never duplicates or exposes its handle;
/// the receiver must move the sole received descriptor into `accept`, without
/// retaining a duplicate. No code in this process may duplicate/reopen the inode
/// or send it back to this process. Forked copies cannot acquire or release.
/// SCM_RIGHTS shares the open-file description, so flock/OFD locks would not
/// exclude the two participating processes.
class HandoverLease {
public:
    HandoverLease(HandoverLease&&) noexcept = default;
    HandoverLease& operator=(HandoverLease&&) noexcept = default;
    HandoverLease(const HandoverLease&) = delete;
    HandoverLease& operator=(const HandoverLease&) = delete;

    /// Creates, unlinks, and acquires a private fixed-size metadata file.
    ///
    /// - Parameter privateDirectory: Borrowed owner-only directory descriptor.
    /// - Returns: The held lease, or a refusal without a retained descriptor.
    [[nodiscard]] static std::expected<HandoverLease, HandoverLeaseError> create(int privateDirectory);

    /// Binds the held lease once to the authenticated successor request.
    /// - Parameter claim: Exact successor PID and nonzero request identity.
    /// - Returns: Completion, or refusal without permitting another claim.
    [[nodiscard]] std::expected<void, HandoverLeaseError> bindSuccessor(HandoverLeaseClaim claim);

    /// Adopts one received descriptor while the authenticated predecessor holds it.
    ///
    /// This must finish before the receiver accepts the handover and before the
    /// predecessor releases. It does not acquire permission to serve. Every
    /// refusal consumes and closes the incoming handle.
    ///
    /// - Parameters:
    ///   - descriptor: Sole local close-on-exec owner received through SCM_RIGHTS.
    ///   - predecessor: Positive PID from authenticated live peer admission.
    ///   - claim: Exact request sent by this process to the predecessor.
    /// - Returns: A non-serving owner, or the exact validation refusal.
    [[nodiscard]] static std::expected<HandoverLease, HandoverLeaseError> accept(PAL::UnixFileDescriptor descriptor, pid_t predecessor, HandoverLeaseClaim claim);

    /// Sends the held descriptor without making a local duplicate or releasing it.
    ///
    /// A failed transfer can have delivered a prefix; abandon that channel. The
    /// lease remains held. Sending does not change either peer's service authority.
    ///
    /// - Parameters:
    ///   - socket: Exclusively owned authenticated Unix stream channel.
    ///   - payload: Offer metadata, at most `maxHandoverLeaseOfferBytes` bytes.
    ///   - timeoutMilliseconds: Positive bound for the complete send.
    /// - Returns: Completion, or refusal with the lease still locally owned.
    [[nodiscard]] std::expected<void, HandoverLeaseError> sendOffer(int socket, std::span<const std::uint8_t> payload, int timeoutMilliseconds);

    /// Attempts a whole-file process lock without waiting for another holder.
    ///
    /// Successor preparation and predecessor rollback require this operation.
    /// A claimed marker refuses predecessor rollback and releases any acquired
    /// lock; acquiring a lock after its successor dies never clears that fence.
    /// A refusal never permits service, even if an earlier attempt succeeded.
    /// - Returns: Exclusive ownership, Busy, or an unrecoverable local refusal.
    [[nodiscard]] std::expected<void, HandoverLeaseError> tryAcquire();

    /// Irrevocably fences the predecessor while this accepted successor holds the lock.
    /// A caller commits or services PTYs only after this succeeds. Failed or
    /// partial metadata writes permit no service; corrupt metadata refuses rollback.
    /// - Returns: Completion, or refusal without granting service authority.
    [[nodiscard]] std::expected<void, HandoverLeaseError> claimSuccessor();

    /// Permanently cancels an exposed pending offer while its predecessor holds the lock.
    ///
    /// Only the original authenticated owner may cancel its exact bound request.
    /// Success permits predecessor service while the lock remains held; a late
    /// successor cannot acquire or claim after cancellation, even if it received
    /// Release before cancellation and outlives the predecessor's lease owner.
    /// Failed or partial writes permit no service and must not resume rollback.
    /// A new handover requires a fresh anonymous lease, never rebinding this one.
    /// - Returns: Completion, or refusal without granting service authority.
    [[nodiscard]] std::expected<void, HandoverLeaseError> cancelSuccessor();

    /// Releases the process lock after the caller has stopped all service.
    ///
    /// - Returns: Successful release, or a refusal that never permits service.
    [[nodiscard]] std::expected<void, HandoverLeaseError> release();

private:
    friend struct HandoverLeaseAccess;
    explicit HandoverLease(PAL::UnixFileDescriptor descriptor);

    PAL::UnixFileDescriptor m_descriptor;
    pid_t m_process = 0;
    bool m_isHeld = false;
    bool m_isSuccessor = false;
    pid_t m_predecessor = 0;
    HandoverLeaseClaim m_claim;
};

} // namespace Ubiquitty::POSIX
