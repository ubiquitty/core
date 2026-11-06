// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/Protocol/Control/Reclaim.h"
#include "Core/Protocol/Sync/HandoverBody.h"
#include "Core/Session/AdoptTransaction.h"
#include "PAL/OutputQueue.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>

namespace Ubiquitty::Session {

/// The requesting attachment's final numbered event before reclaim freezes input.
/// The client instance is replay identity, not authorization to reclaim a pane.
struct ReclaimInputBarrier {
    Protocol::Sync::ID128 clientInstance;
    std::uint64_t finalInputSeq = 0;
    bool operator==(const ReclaimInputBarrier&) const = default;
};

/// Borrows one frozen, complete offer; no field authenticates its sender.
/// The transport owner must verify the exact existing local attachment and keep
/// the host terminal, input admission, encoder and stream drains frozen together.
struct ReclaimCapsuleView {
    Protocol::Control::ReclaimTarget target;
    AdoptOperation operation;
    ReclaimInputBarrier barrier;
    /// The actual ledger receipt. Absence is never converted to zero.
    /// Written means accepted by the stream queue, not delivered to the kernel.
    std::optional<std::uint64_t> inputWritten;
    std::span<const std::uint8_t> terminalBody;
    std::span<const char> acceptedPTY;
    std::span<const PAL::OutputQueue::Run> acceptedRuns;
    std::array<std::uint8_t, 32> capsuleSHA256 {};
};

/// Independent receiver policy. Capsule bytes bound the canonical digest input
/// plus its digest, not decoded-terminal memory or peak preparation allocation.
struct ReclaimTransferLimits {
    Protocol::Sync::HandoverTerminalLimits terminal;
    std::size_t maxAcceptedPTYBytes = 0;
    std::size_t maxCapsuleBytes = 0;
};

/// A refusal leaves every live owner and any existing prepared transfer unchanged.
enum class ReclaimTransferError : std::uint8_t {
    InvalidPolicy,
    InvalidIdentity,
    MismatchedIdentity,
    InputBarrier,
    InvalidPartition,
    OverCapacity,
    DigestFailure,
    DigestMismatch,
    TerminalRefused,
    NoMemory,
    Conflict,
};

/// Hashes a versioned field-wise representation without retaining borrowed data.
/// Includes every identity, receipt, length, ownership run and payload byte;
/// excludes only capsuleSHA256 itself. Digest validity does not grant authority.
/// - Parameters:
///   - capsule: Immutable metadata and bytes held by their serial owner.
///   - limits: Receiver policy, independent of the offered record.
/// - Returns: The complete digest, or refusal before materializing a terminal.
[[nodiscard]] std::expected<std::array<std::uint8_t, 32>, ReclaimTransferError> reclaimCapsuleSHA256(
    const ReclaimCapsuleView& capsule, const ReclaimTransferLimits& limits);

/// Exact evidence received through the same freshly authenticated authority.
/// Committed means the daemon released this capsule, not that a thin replica is
/// safe to resume. Correlation and live authority verification precede observe().
struct ReclaimTransferEvidence {
    Protocol::Control::ReclaimTarget target;
    ReclaimInputBarrier barrier;
    std::array<std::uint8_t, 32> capsuleSHA256 {};
    AdoptOutcome outcome;
};

/// Owns the complete terminal and accepted-byte suffix before any possible Commit.
/// Queue owner tokens remain queue-local data. Reserve their namespace before
/// future owned submissions; tokens neither identify nor authorize an attachment.
/// Native callbacks, stream-install capacity and all external borrowers still
/// require an independent pre-Commit installation reservation by the live owner.
struct ReclaimInstallation {
    Protocol::Control::ReclaimTarget target;
    AdoptOperation operation;
    ReclaimInputBarrier barrier;
    std::uint64_t inputWritten = 0;
    Protocol::Sync::HandoverTerminal terminal;
    PAL::OutputQueue::Unwritten acceptedPTY;

    ReclaimInstallation();
    ~ReclaimInstallation();
    ReclaimInstallation(const ReclaimInstallation&) = delete;
    ReclaimInstallation& operator=(const ReclaimInstallation&) = delete;
    ReclaimInstallation(ReclaimInstallation&&) noexcept;
    ReclaimInstallation& operator=(ReclaimInstallation&&) noexcept;
};

/// The proven serving owner, never inferred from a carrier error or EOF.
enum class ReclaimOwnership : std::uint8_t {
    Daemon,
    Local,
    Unresolved,
};

/// Owns an admitted private capsule and adapts the existing irreversible gate.
/// AdoptClientAttempt is reused directly, with direction reversed: its
/// mayResumeLocal() means the predecessor daemon still owns this transfer;
/// isCommitted() means the receiving app may install the complete local owner.
/// No display replica or reset/retarget API participates in this decision.
class ReclaimTransfer {
public:
    /// Stages the complete terminal and suffix atomically, without live mutation.
    /// Exact selector shapes, barrier, component limits, partition and digest are
    /// admitted before materialization. The canonical full-terminal decoder still
    /// has bounded STL fallibility gaps; this is not an aggregate OOM guarantee.
    /// - Parameters:
    ///   - target: The original exact existing attachment and adopted pane.
    ///   - operation: Independently random operation on that daemon, not a credential.
    ///   - barrier: The client's frozen final numbering, not a new input event.
    ///   - capsule: A complete unchanged offer, including the actual ledger receipt.
    ///   - limits: Explicit receiver policy; no transport cap becomes a memory budget.
    ///   - decodeMemory: The receiving process's decode owner, which the
    ///     staged terminal charges until it is installed or dropped.
    /// - Returns: A complete inactive owner, or refusal preserving all live state.
    [[nodiscard]] static std::expected<std::unique_ptr<ReclaimTransfer>, ReclaimTransferError> prepare(
        const Protocol::Control::ReclaimTarget& target, AdoptOperation operation, ReclaimInputBarrier barrier,
        const ReclaimCapsuleView& capsule, const ReclaimTransferLimits& limits, std::shared_ptr<Protocol::Sync::MemoryBudget> decodeMemory = nullptr);
    ~ReclaimTransfer();
    ReclaimTransfer(const ReclaimTransfer&) = delete;
    ReclaimTransfer& operator=(const ReclaimTransfer&) = delete;
    ReclaimTransfer(ReclaimTransfer&&) = delete;
    ReclaimTransfer& operator=(ReclaimTransfer&&) = delete;

    /// Marks uncertainty before the first possible Commit byte. The caller must
    /// already retain this entire owner and the original master in bounded escrow,
    /// with every remaining native/stream installation resource pre-reserved.
    /// - Returns: The existing gate's decision; no I/O or allocation occurs.
    [[nodiscard]] std::expected<void, AdoptTransactionError> beginCommit();

    /// Accepts only evidence for the original full identity, barrier and capsule.
    /// Missing/refused evidence requires no call: uncertainty and owners persist.
    /// Unknown or Prepared after Commit never grants either side permission to
    /// reactivate. A changed daemon cannot retarget or discard this attempt.
    /// - Parameter evidence: Authenticated and correlated retained host outcome.
    /// - Returns: Accepted evidence, or refusal without relaxing the gate.
    [[nodiscard]] std::expected<void, ReclaimTransferError> observe(const ReclaimTransferEvidence& evidence);

    [[nodiscard]] ReclaimOwnership ownership() const;
    [[nodiscard]] bool hasPreparedInstallation() const { return m_installation != nullptr; }
    [[nodiscard]] const Protocol::Control::ReclaimTarget& target() const { return m_target; }
    [[nodiscard]] const AdoptOperation& operation() const { return m_attempt.operation(); }
    [[nodiscard]] const ReclaimInputBarrier& barrier() const { return m_barrier; }
    [[nodiscard]] const std::array<std::uint8_t, 32>& capsuleSHA256() const { return m_capsuleSHA256; }

    /// Transfers the already allocated complete owner once, only after release.
    /// No allocation, decoding, callbacks, stream writes or installation occurs.
    /// - Returns: The prepared owner, or null while uncommitted/already consumed.
    [[nodiscard]] std::unique_ptr<ReclaimInstallation> takeCommitted() noexcept;

private:
    ReclaimTransfer(const Protocol::Control::ReclaimTarget&, AdoptOperation, ReclaimInputBarrier,
        std::array<std::uint8_t, 32>, std::unique_ptr<ReclaimInstallation>);

    const Protocol::Control::ReclaimTarget m_target;
    const ReclaimInputBarrier m_barrier;
    const std::array<std::uint8_t, 32> m_capsuleSHA256;
    AdoptClientAttempt m_attempt;
    std::unique_ptr<ReclaimInstallation> m_installation;
};

} // namespace Ubiquitty::Session
