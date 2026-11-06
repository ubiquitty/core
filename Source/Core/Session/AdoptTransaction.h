// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// One serial owner retains inactive adoption candidates and exact outcomes.
// The platform supplies fresh kernel authority, bounded candidate construction,
// and a reserved, infallible activation; this module performs no I/O or parsing.

#include "Core/Protocol/Sync/Messages.h"
#include "PAL/PTYFacts.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>

namespace Ubiquitty::Session {

/// The live local process and executable established by the platform verifier.
/// This is not a wire credential; no request may manufacture these facts.
struct AdoptAuthority {
    PAL::ProcessID processID = PAL::noProcess;
    PAL::ProcessBirthIdentity birth;
    std::array<std::uint8_t, 32> executableSHA256 {};
    bool operator==(const AdoptAuthority&) const = default;
};

/// Pins a random operation independently of a connection's correlation number.
struct AdoptOperation {
    Protocol::Sync::ID128 operationID;
    std::uint64_t daemonInstanceID = 0;
    bool operator==(const AdoptOperation&) const = default;
};

/// Binds duplicate Prepare to the same completely validated payload bytes.
struct AdoptPreparation {
    AdoptOperation operation;
    std::array<std::uint8_t, 32> bodySHA256 {};
    std::size_t bodyBytes = 0;
    bool operator==(const AdoptPreparation&) const = default;
};

/// The reserved routing identity, unpublished until candidate activation.
struct AdoptTarget {
    std::uint32_t sessionID = 0;
    Protocol::Sync::ID128 paneID {};
    bool operator==(const AdoptTarget&) const = default;
};

/// A retained disposition, not an inference from transport success or silence.
enum class AdoptDisposition : std::uint8_t {
    Unknown,
    Prepared,
    Committed,
    Aborted,
    Expired,
    Rejected,
};

/// Why candidate preparation refused without activating an owner.
enum class AdoptRejection : std::uint8_t {
    InvalidPayload,
    NotOwner,
    NoMemory,
    InvalidCandidate,
    /// The host already holds as many sessions as it serves; nothing about the
    /// candidate was wrong, and a host with room would take it.
    HostFull,
};

/// The exact fixed-size result stored before any irreversible activation.
struct AdoptOutcome {
    AdoptOperation operation;
    AdoptDisposition disposition = AdoptDisposition::Unknown;
    AdoptTarget target {};
    std::optional<AdoptRejection> rejection {};
    bool operator==(const AdoptOutcome&) const = default;
};

/// Why an operation was not admitted; no error authorizes local resumption.
enum class AdoptTransactionError : std::uint8_t {
    InvalidIdentity,
    InvalidPolicy,
    NoMemory,
    Busy,
    StaleAuthority,
    Unauthorised,
    Conflict,
    Reentrant,
    ReplyUnavailable,
    InvalidTime,
    GenerationExhausted,
    ActiveCandidate,
    RetirementUnproven,
};

/// Evidence supplied only after exact kernel lifetime and channel retirement.
enum class AdoptRetirementEvidence : std::uint8_t {
    Unproven,
    ProcessGoneOnly,
    ProcessGoneAndAllChannelsAndOperationsRevokedAndDrained,
};

/// An inactive, privately owned candidate with no child termination authority.
/// Destruction releases only staged resources; it never publishes or ingests.
class AdoptCandidate {
public:
    virtual ~AdoptCandidate() = default;
    /// Returns its reserved immutable identity, without activation or allocation.
    /// - Returns: A nonzero reserved session and pane identity.
    [[nodiscard]] virtual AdoptTarget target() const = 0;
};

/// Owns complete admitted output and activation resources until they are used.
class AdoptCommitReservation {
public:
    virtual ~AdoptCommitReservation() = default;
};

/// The trusted, synchronous platform boundary; callbacks cannot destroy or reenter
/// this table or driver, write a carrier, or retain a borrowed table argument.
class AdoptTransactionDriver {
public:
    virtual ~AdoptTransactionDriver() = default;
    /// Rechecks the current same-user local ungreeted peer, trusted executable,
    /// and exact live kernel process birth. Cached verification is insufficient.
    /// - Parameter authority: Facts compared with the current physical peer.
    /// - Returns: Fresh verification, or a refusal with no ownership inference.
    [[nodiscard]] virtual std::expected<void, AdoptTransactionError> verifyAuthority(const AdoptAuthority& authority) = 0;
    /// Reserves and constructs an inactive candidate under the host's complete
    /// candidate-memory/descriptor/session-slot admission. No session is visible.
    /// - Parameter preparation: The exact payload admitted by the local decoder.
    /// - Returns: A complete private owner, or a definite inactive rejection.
    [[nodiscard]] virtual std::expected<std::unique_ptr<AdoptCandidate>, AdoptRejection> prepareCandidate(const AdoptPreparation& preparation) = 0;
    /// Reserves the complete correlated result and every remaining activation
    /// resource, without publication or carrier writes. Refusal changes nothing.
    /// - Parameter outcome: The exact immutable result that activation will use.
    /// - Parameter candidate: The same private owner, borrowed only for fresh
    ///   endpoint validation before recording irreversible ownership.
    /// - Returns: All admitted resources, or refusal before irreversible mutation.
    [[nodiscard]] virtual std::expected<std::unique_ptr<AdoptCommitReservation>, AdoptTransactionError> reserveCommittedReply(const AdoptOutcome& outcome, const AdoptCandidate& candidate) = 0;
    /// Consumes the prepared owner and reservation exactly once, without failure
    /// or further allocation. The record is already Committed; publication and
    /// delivery must occur outside decoder borrows in the platform owner.
    /// - Parameters:
    ///   - candidate: The inactive owner to install, without retargeting.
    ///   - reservation: The complete result and activation admission to consume.
    virtual void activate(std::unique_ptr<AdoptCandidate> candidate, std::unique_ptr<AdoptCommitReservation> reservation) noexcept = 0;
    /// Proves both exact lifetime death and all admitted channel/operation drain.
    /// Timeout, ESRCH alone, disconnect, and stale cached proof are insufficient.
    /// - Parameter authority: The exact kernel lifetime whose channels retired.
    /// - Returns: Explicit complete evidence, never an inferred timeout verdict.
    [[nodiscard]] virtual AdoptRetirementEvidence retirementEvidence(const AdoptAuthority& authority) = 0;
};

/// An opaque admission lease; reuse of a table slot never revives an old lease.
class AdoptAuthorityLease {
public:
    AdoptAuthorityLease() = default;
    bool operator==(const AdoptAuthorityLease&) const = default;

private:
    friend class AdoptTransaction;
    AdoptAuthorityLease(std::size_t slot, std::uint64_t generation)
        : m_slot(slot)
        , m_generation(generation)
    {
    }
    std::size_t m_slot = 0;
    std::uint64_t m_generation = 0;
};

/// Caller-selected admission, with no implicit timer or capacity policy.
struct AdoptTransactionLimits {
    std::size_t authorityCapacity = 0;
    std::size_t outcomeCapacity = 0;
    std::uint64_t preparedLeaseMicroseconds = 0;
};

/// Owns bounded preallocated metadata and candidates on one non-reentrant thread.
/// Terminal outcomes never expire or evict for a live authority. Only explicit
/// proven authority retirement reclaims slots, without touching committed owners.
class AdoptTransaction {
public:
    /// Allocates all metadata before admission, refusing invalid policy or allocation.
    /// - Parameters:
    ///   - daemonInstanceID: The immutable serving incarnation.
    ///   - limits: Explicit metadata capacity and Prepared lease policy.
    /// - Returns: A complete empty owner, or refusal without partial admission.
    [[nodiscard]] static std::expected<std::unique_ptr<AdoptTransaction>, AdoptTransactionError> create(std::uint64_t daemonInstanceID, AdoptTransactionLimits limits);
    ~AdoptTransaction();
    AdoptTransaction(const AdoptTransaction&) = delete;
    AdoptTransaction& operator=(const AdoptTransaction&) = delete;
    AdoptTransaction(AdoptTransaction&&) = delete;
    AdoptTransaction& operator=(AdoptTransaction&&) = delete;

    /// Issues or reuses a lease only after fresh platform verification.
    /// - Parameters:
    ///   - authority: Kernel-derived facts, never request-provided credentials.
    ///   - driver: The live local platform verifier.
    /// - Returns: An opaque current lease, or a refusal before candidate work.
    [[nodiscard]] std::expected<AdoptAuthorityLease, AdoptTransactionError> admitAuthority(const AdoptAuthority& authority, AdoptTransactionDriver& driver);
    /// Reclaims terminal metadata only after all operations and channels retired.
    /// Old leases stay invalid; fresh admissions must validate a live birth again.
    /// - Parameters:
    ///   - lease: The exact authority generation to retire.
    ///   - driver: The source of complete kernel-lifetime/channel-drain evidence.
    /// - Returns: Retired metadata, or refusal without touching active owners.
    [[nodiscard]] std::expected<void, AdoptTransactionError> retireAuthority(AdoptAuthorityLease lease, AdoptTransactionDriver& driver);
    /// Advances the owner's monotonic clock; absolute Prepared expiry becomes a
    /// retained Expired outcome that rejects every late Commit for that operation.
    /// The serial owner advances this before command admission and on its timer.
    /// - Parameter nowMicroseconds: The injected current monotonic reading.
    /// - Returns: Clock advancement, or refusal without wrapping or moving back.
    [[nodiscard]] std::expected<void, AdoptTransactionError> advanceTime(std::uint64_t nowMicroseconds);
    /// Reserves an outcome slot before calling any candidate constructor.
    /// - Parameters:
    ///   - lease: The current verified authority generation.
    ///   - preparation: The exact bounded payload identity.
    ///   - driver: The inactive-candidate factory and current peer verifier.
    /// - Returns: The retained Prepared/terminal result, or admission refusal.
    [[nodiscard]] std::expected<AdoptOutcome, AdoptTransactionError> prepare(AdoptAuthorityLease lease, const AdoptPreparation& preparation, AdoptTransactionDriver& driver);
    /// Revalidates exact authority and output admission before single activation.
    /// - Parameters:
    ///   - lease: The original authority generation, freshly revalidated.
    ///   - operation: The exact prepared operation and serving incarnation.
    ///   - driver: The reservation/activation owner and current peer verifier.
    /// - Returns: An exact retained result, Unknown, or refusal; never a rollback.
    [[nodiscard]] std::expected<AdoptOutcome, AdoptTransactionError> commit(AdoptAuthorityLease lease, AdoptOperation operation, AdoptTransactionDriver& driver);
    /// Serializes terminal Abort against Commit; late Commit returns the tombstone.
    /// - Parameters:
    ///   - lease: The original authority generation, freshly revalidated.
    ///   - operation: The exact operation to abort, never another selector.
    ///   - driver: The current physical peer verifier.
    /// - Returns: The winning retained outcome, Unknown, or admission refusal.
    [[nodiscard]] std::expected<AdoptOutcome, AdoptTransactionError> abort(AdoptAuthorityLease lease, AdoptOperation operation, AdoptTransactionDriver& driver);
    /// Reads the exact retained outcome without refreshing a lease or mutating it.
    /// - Parameters:
    ///   - lease: The original authority generation, freshly revalidated.
    ///   - operation: The exact operation whose retained state is requested.
    ///   - driver: The current physical peer verifier.
    /// - Returns: A retained result, genuinely missing Unknown, or refusal.
    [[nodiscard]] std::expected<AdoptOutcome, AdoptTransactionError> status(AdoptAuthorityLease lease, AdoptOperation operation, AdoptTransactionDriver& driver);
    /// Reports occupied outcome storage, including terminal tombstones.
    /// - Returns: The count within the preallocated outcome capacity.
    [[nodiscard]] std::size_t outcomeCount() const { return m_outcomeCount; }

private:
    struct Authority;
    struct Record;
    AdoptTransaction(std::uint64_t, AdoptTransactionLimits);
    [[nodiscard]] std::expected<Authority*, AdoptTransactionError> verified(AdoptAuthorityLease, AdoptTransactionDriver&);
    [[nodiscard]] std::expected<Record*, AdoptTransactionError> find(AdoptAuthorityLease, AdoptOperation);
    void endPrepared(Record&, AdoptDisposition);

    const std::uint64_t m_daemonInstanceID;
    const AdoptTransactionLimits m_limits;
    std::unique_ptr<Authority[]> m_authorities;
    std::unique_ptr<Record[]> m_records;
    std::size_t m_outcomeCount = 0;
    std::uint64_t m_nextAuthorityGeneration = 1;
    std::uint64_t m_nowMicroseconds = 0;
    bool m_isHandling = false;
};

/// The sender's non-resettable ownership gate for exactly one operation.
/// It marks uncertainty before Commit is sent. Prepared or Unknown after that
/// never restores local ownership; only an exact retained terminal outcome can.
class AdoptClientAttempt {
public:
    /// Pins one attempt for its entire lifetime, without a reset or retarget API.
    /// - Parameter operation: The independently minted operation and daemon pin.
    explicit AdoptClientAttempt(AdoptOperation operation)
        : m_operation(operation)
    {
    }
    AdoptClientAttempt(const AdoptClientAttempt&) = delete;
    AdoptClientAttempt& operator=(const AdoptClientAttempt&) = delete;
    AdoptClientAttempt(AdoptClientAttempt&&) = delete;
    AdoptClientAttempt& operator=(AdoptClientAttempt&&) = delete;

    /// Accepts only exact operation/daemon/target results with valid shapes.
    /// - Parameter outcome: The complete authenticated result to validate.
    /// - Returns: Accepted evidence, or refusal without relaxing ownership.
    [[nodiscard]] std::expected<void, AdoptTransactionError> observe(const AdoptOutcome& outcome);
    /// Records uncertainty before the first possible Commit byte is written.
    /// - Returns: A recorded attempt, or refusal without issuing any transport work.
    [[nodiscard]] std::expected<void, AdoptTransactionError> beginCommit();
    /// Reports whether the inactive-Prepare protocol proves local ownership.
    /// - Returns: False after an unresolved Commit or confirmed host activation.
    [[nodiscard]] bool mayResumeLocal() const;
    /// Reports whether the exact committed target was confirmed.
    /// - Returns: Whether valid matching Committed evidence was accepted.
    [[nodiscard]] bool isCommitted() const { return m_disposition == AdoptDisposition::Committed; }
    /// Returns the immutable operation retained across every resolution channel.
    [[nodiscard]] const AdoptOperation& operation() const { return m_operation; }
    /// Returns the exact reserved target, or empty before Prepared was observed.
    [[nodiscard]] AdoptTarget target() const { return m_target; }
    /// Returns why the host rejected the candidate.
    /// - Returns: The accepted Rejected outcome's reason, or empty for any
    ///   other disposition.
    [[nodiscard]] std::optional<AdoptRejection> rejection() const { return m_rejection; }

private:
    const AdoptOperation m_operation;
    AdoptTarget m_target;
    AdoptDisposition m_disposition = AdoptDisposition::Unknown;
    std::optional<AdoptRejection> m_rejection;
    bool m_didAttemptCommit = false;
};

} // namespace Ubiquitty::Session
