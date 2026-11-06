// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/Session/ReclaimTransactionWire.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>

namespace Ubiquitty::Session {

class ReclaimTransaction;

/// Direction-specific lease; the underlying reverse-table lease never escapes.
/// This nonowning value is valid only while its original table owner is alive.
/// The owner identity is compared, never dereferenced; a destroyed owner and
/// every lease it issued must be retired together before its storage is reused.
class ReclaimAuthorityLease {
public:
    ReclaimAuthorityLease() = default;
    bool operator==(const ReclaimAuthorityLease&) const = default;

private:
    friend class ReclaimTransaction;
    ReclaimAuthorityLease(const ReclaimTransaction& owner, AdoptAuthorityLease adoption, AdoptAuthorityLease reclaim)
        : m_owner(&owner)
        , m_adoption(adoption)
        , m_reclaim(reclaim)
    {
    }

    const ReclaimTransaction* m_owner = nullptr;
    AdoptAuthorityLease m_adoption;
    AdoptAuthorityLease m_reclaim;
};

/// Fixed retained evidence, independent of the candidate and hosted Session.
/// Capsule validation, full input freeze and installation admission precede this
/// call. Neither operation ID nor digest is a credential or kernel death proof.
struct ReclaimWitness {
    ReclaimTransactionSelector selector;
    std::array<std::uint8_t, 32> capsuleSHA256 {};
    std::size_t capsuleBytes = 0;
    bool operator==(const ReclaimWitness&) const = default;
};

struct ReclaimTransactionLimits {
    AdoptTransactionLimits transactions;
    std::size_t maxCapsuleBytes = 0;
};

/// Unknown has no fabricated capsule witness, even if a query offered a digest.
/// Known outcomes carry the actual immutable witness retained before Prepare.
struct ReclaimTransactionOutcome {
    ReclaimTransactionSelector selector;
    std::optional<std::array<std::uint8_t, 32>> capsuleSHA256;
    std::size_t capsuleBytes = 0;
    AdoptOutcome outcome;
};

/// Binds the complete selector, actual capsule hash and admitted length to the
/// existing table's preparation identity, independently of request correlation.
/// Hashing this bounded metadata does not authenticate or validate a capsule.
[[nodiscard]] std::expected<AdoptPreparation, AdoptTransactionError> reclaimWitnessPreparation(
    const ReclaimWitness&, std::size_t maxCapsuleBytes);

/// Retains fixed witnesses beside a separate instance of the existing adoption
/// transaction implementation. It does not copy the state machine or touch a
/// Terminal, stream, clock, physical channel, process or credential store.
///
/// The original adoption table must outlive this owner. A shared platform kernel
/// lifetime protects both tables; retiring this owner retires only its reverse
/// lease. The platform independently clears each successful table retirement,
/// and releases the kernel watch only after both directions and all channels,
/// operations, callbacks and decoder borrows are positively retired and drained.
class ReclaimTransaction {
public:
    [[nodiscard]] static std::expected<std::unique_ptr<ReclaimTransaction>, AdoptTransactionError> create(
        std::uint64_t daemonInstanceID, AdoptTransaction& originalAdoptions, ReclaimTransactionLimits);
    ~ReclaimTransaction();
    ReclaimTransaction(const ReclaimTransaction&) = delete;
    ReclaimTransaction& operator=(const ReclaimTransaction&) = delete;
    ReclaimTransaction(ReclaimTransaction&&) = delete;
    ReclaimTransaction& operator=(ReclaimTransaction&&) = delete;

    /// The caller has already reserved the exact physical lifetime owner. Fresh
    /// original admission must equal its supplied original-table lease before a
    /// reverse lease is admitted. Partial refusal never retires that original.
    [[nodiscard]] std::expected<ReclaimAuthorityLease, AdoptTransactionError> admitAuthority(
        const AdoptAuthority& physicalAuthority, AdoptAuthorityLease originalLease, AdoptTransactionDriver&);

    /// Advances only the existing table's injected monotonic time. Expiry keeps
    /// every witness and terminal outcome until proven physical retirement.
    [[nodiscard]] std::expected<void, AdoptTransactionError> advanceTime(std::uint64_t nowMicroseconds);

    /// Reserves fixed witness storage before candidate construction. Duplicate
    /// Prepare validates every selector and hash before the base table can take
    /// its duplicate fast path. Candidate bodies remain privately platform-owned.
    [[nodiscard]] std::expected<ReclaimTransactionOutcome, AdoptTransactionError> prepare(
        ReclaimAuthorityLease, const ReclaimWitness&, AdoptTransactionDriver&);

    /// Commit requires the retained actual digest. The adapter freshly verifies
    /// the exact original Committed adoption and target both before reservation
    /// and again before the existing table records its irreversible outcome.
    [[nodiscard]] std::expected<ReclaimTransactionOutcome, AdoptTransactionError> commit(
        ReclaimAuthorityLease, const ReclaimTransactionSelector&, const std::array<std::uint8_t, 32>& digest,
        AdoptTransactionDriver&);

    /// An absent digest is only a lost-Prepare query. It may discover Prepared or
    /// a noncommitted negative, but never authorizes a Committed release reply.
    [[nodiscard]] std::expected<ReclaimTransactionOutcome, AdoptTransactionError> status(
        ReclaimAuthorityLease, const ReclaimTransactionSelector&,
        const std::optional<std::array<std::uint8_t, 32>>& digest, AdoptTransactionDriver&);
    [[nodiscard]] std::expected<ReclaimTransactionOutcome, AdoptTransactionError> abort(
        ReclaimAuthorityLease, const ReclaimTransactionSelector&,
        const std::optional<std::array<std::uint8_t, 32>>& digest, AdoptTransactionDriver&);

    /// Clears only this reverse table and its fixed witnesses after complete
    /// exact lifetime/channel/operation retirement evidence. A successful result
    /// must be cleared independently of the original table's retirement result.
    [[nodiscard]] std::expected<void, AdoptTransactionError> retireAuthority(ReclaimAuthorityLease, AdoptTransactionDriver&);

    [[nodiscard]] std::size_t outcomeCount() const;
    [[nodiscard]] std::size_t witnessCount() const { return m_witnessCount; }

private:
    struct Record;
    class Driver;
    ReclaimTransaction(AdoptTransaction&, ReclaimTransactionLimits);
    [[nodiscard]] bool owns(ReclaimAuthorityLease) const;
    [[nodiscard]] std::expected<Record*, AdoptTransactionError> find(ReclaimAuthorityLease, const ReclaimTransactionSelector&);
    [[nodiscard]] ReclaimTransactionOutcome result(const ReclaimTransactionSelector&, const Record*, AdoptOutcome) const;
    [[nodiscard]] std::expected<ReclaimTransactionOutcome, AdoptTransactionError> command(
        Protocol::Local::ReclaimWireMessage, ReclaimAuthorityLease, const ReclaimTransactionSelector&,
        const std::optional<std::array<std::uint8_t, 32>>&, AdoptTransactionDriver&);

    AdoptTransaction& m_originalAdoptions;
    const ReclaimTransactionLimits m_limits;
    std::unique_ptr<AdoptTransaction> m_transactions;
    std::unique_ptr<Record[]> m_records;
    std::size_t m_witnessCount = 0;
    bool m_isHandling = false;
};

} // namespace Ubiquitty::Session
