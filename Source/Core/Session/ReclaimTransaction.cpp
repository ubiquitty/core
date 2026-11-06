// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Session/ReclaimTransaction.h"

#include "Core/Crypto/SHA256.h"

#include <cassert>
#include <limits>
#include <new>
#include <utility>

namespace Ubiquitty::Session {
namespace Local = Protocol::Local;
namespace {

constexpr std::array<std::uint8_t, 14> witnessDomain { 'U', 'B', 'Q', 'R', 'E', 'C', 'L', 'A', 'I', 'M', 'T', 'X', 0, 1 };

/// Serializes witness mutation around the existing table and its callbacks.
class WitnessScope {
public:
    explicit WitnessScope(bool& isHandling) // bool-ok: scoped reentrancy guard, not an operation choice.
        : m_isHandling(isHandling)
    {
        m_isHandling = true;
    }
    ~WitnessScope() { m_isHandling = false; }

private:
    bool& m_isHandling;
};

AdoptTarget targetOf(const ReclaimTransactionSelector& selector)
{
    return { .sessionID = selector.target.sessionID, .paneID = selector.target.paneID };
}

bool isEmpty(AdoptTarget target)
{
    return !target.sessionID && target.paneID == Protocol::Sync::ID128 {};
}

} // namespace

struct ReclaimTransaction::Record {
    ReclaimAuthorityLease authority;
    ReclaimWitness witness;
    bool isOccupied = false;
};

/// Adapts only proof and candidate bounds; all transaction transitions remain in
/// AdoptTransaction. The original adoption proof is rechecked at each existing
/// fresh-authority boundary, including the check after commit reservation.
class ReclaimTransaction::Driver final : public AdoptTransactionDriver {
public:
    Driver(ReclaimTransaction& owner, ReclaimAuthorityLease lease, const ReclaimTransactionSelector& selector, AdoptTransactionDriver& delegate)
        : m_owner(owner)
        , m_lease(lease)
        , m_selector(selector)
        , m_delegate(delegate)
    {
    }

    std::expected<void, AdoptTransactionError> verifyAuthority(const AdoptAuthority& authority) override
    {
        if (const auto physical = m_delegate.verifyAuthority(authority); !physical)
            return std::unexpected(physical.error());
        const auto original = m_owner.m_originalAdoptions.status(m_lease.m_adoption, m_selector.adoption, m_delegate);
        if (!original)
            return std::unexpected(original.error());
        if (original->disposition != AdoptDisposition::Committed || original->target != targetOf(m_selector))
            return std::unexpected(AdoptTransactionError::Unauthorised);
        return {};
    }

    std::expected<std::unique_ptr<AdoptCandidate>, AdoptRejection> prepareCandidate(const AdoptPreparation& preparation) override
    {
        if (preparation.operation != m_selector.operation)
            return std::unexpected(AdoptRejection::InvalidCandidate);
        auto candidate = m_delegate.prepareCandidate(preparation);
        if (!candidate)
            return std::unexpected(candidate.error());
        if (!*candidate || (*candidate)->target() != targetOf(m_selector))
            return std::unexpected(AdoptRejection::InvalidCandidate);
        return std::move(*candidate);
    }

    std::expected<std::unique_ptr<AdoptCommitReservation>, AdoptTransactionError> reserveCommittedReply(
        const AdoptOutcome& outcome, const AdoptCandidate& candidate) override
    {
        if (outcome.operation != m_selector.operation || outcome.disposition != AdoptDisposition::Committed
            || outcome.target != targetOf(m_selector) || candidate.target() != targetOf(m_selector))
            return std::unexpected(AdoptTransactionError::Conflict);
        return m_delegate.reserveCommittedReply(outcome, candidate);
    }

    void activate(std::unique_ptr<AdoptCandidate> candidate, std::unique_ptr<AdoptCommitReservation> reservation) noexcept override
    {
        assert(candidate && candidate->target() == targetOf(m_selector));
        m_delegate.activate(std::move(candidate), std::move(reservation));
    }

    AdoptRetirementEvidence retirementEvidence(const AdoptAuthority& authority) override
    {
        return m_delegate.retirementEvidence(authority);
    }

private:
    ReclaimTransaction& m_owner;
    const ReclaimAuthorityLease m_lease;
    const ReclaimTransactionSelector& m_selector;
    AdoptTransactionDriver& m_delegate;
};

std::expected<AdoptPreparation, AdoptTransactionError> reclaimWitnessPreparation(const ReclaimWitness& witness, std::size_t maxCapsuleBytes)
{
    if (!maxCapsuleBytes)
        return std::unexpected(AdoptTransactionError::InvalidPolicy);
    if (!witness.capsuleBytes)
        return std::unexpected(AdoptTransactionError::InvalidIdentity);
    if (witness.capsuleBytes > maxCapsuleBytes || !std::in_range<std::uint64_t>(witness.capsuleBytes))
        return std::unexpected(AdoptTransactionError::Busy);
    const auto selector = reclaimSelectorToWire(witness.selector);
    if (!selector)
        return std::unexpected(AdoptTransactionError::InvalidIdentity);
    // A fixed nonzero correlation canonicalizes the existing selector codec;
    // no live connection correlation participates in duplicate identity.
    const auto metadata = Local::encodeReclaimWireCommand({ .kind = Local::ReclaimWireMessage::Prepare,
        .correlation = 1,
        .selector = *selector,
        .capsuleSHA256 = std::nullopt });
    if (!metadata)
        return std::unexpected(AdoptTransactionError::InvalidIdentity);
    std::array<std::uint8_t, 8> length;
    const auto capsuleBytes = static_cast<std::uint64_t>(witness.capsuleBytes);
    for (std::size_t index = 0; index < length.size(); ++index)
        length[index] = static_cast<std::uint8_t>(capsuleBytes >> (index * 8));
    AdoptPreparation preparation { .operation = witness.selector.operation, .bodyBytes = witness.capsuleBytes };
    Crypto::SHA256Hasher hash;
    if (!hash.update(witnessDomain) || !hash.update(*metadata) || !hash.update(witness.capsuleSHA256)
        || !hash.update(length) || !hash.finalize(preparation.bodySHA256))
        return std::unexpected(AdoptTransactionError::InvalidPolicy);
    return preparation;
}

ReclaimTransaction::ReclaimTransaction(AdoptTransaction& originalAdoptions, ReclaimTransactionLimits limits)
    : m_originalAdoptions(originalAdoptions)
    , m_limits(limits)
{
}

ReclaimTransaction::~ReclaimTransaction()
{
    m_isHandling = true;
    m_transactions.reset();
}

std::expected<std::unique_ptr<ReclaimTransaction>, AdoptTransactionError> ReclaimTransaction::create(
    std::uint64_t daemonInstanceID, AdoptTransaction& originalAdoptions, ReclaimTransactionLimits limits)
{
    if (!limits.maxCapsuleBytes || limits.transactions.outcomeCapacity > static_cast<std::size_t>(std::numeric_limits<std::ptrdiff_t>::max()) / sizeof(Record))
        return std::unexpected(AdoptTransactionError::InvalidPolicy);
    auto transactions = AdoptTransaction::create(daemonInstanceID, limits.transactions);
    if (!transactions)
        return std::unexpected(transactions.error());
    std::unique_ptr<ReclaimTransaction> owner(new (std::nothrow) ReclaimTransaction(originalAdoptions, limits));
    if (!owner)
        return std::unexpected(AdoptTransactionError::NoMemory);
    owner->m_records.reset(new (std::nothrow) Record[limits.transactions.outcomeCapacity]);
    if (!owner->m_records)
        return std::unexpected(AdoptTransactionError::NoMemory);
    owner->m_transactions = std::move(*transactions);
    return owner;
}

bool ReclaimTransaction::owns(ReclaimAuthorityLease lease) const
{
    return lease.m_owner == this;
}

std::expected<ReclaimAuthorityLease, AdoptTransactionError> ReclaimTransaction::admitAuthority(
    const AdoptAuthority& physicalAuthority, AdoptAuthorityLease originalLease, AdoptTransactionDriver& driver)
{
    if (m_isHandling)
        return std::unexpected(AdoptTransactionError::Reentrant);
    const WitnessScope scope(m_isHandling);
    const auto original = m_originalAdoptions.admitAuthority(physicalAuthority, driver);
    if (!original)
        return std::unexpected(original.error());
    if (*original != originalLease)
        return std::unexpected(AdoptTransactionError::Conflict);
    const auto reverse = m_transactions->admitAuthority(physicalAuthority, driver);
    if (!reverse)
        return std::unexpected(reverse.error());
    return ReclaimAuthorityLease(*this, originalLease, *reverse);
}

std::expected<void, AdoptTransactionError> ReclaimTransaction::advanceTime(std::uint64_t nowMicroseconds)
{
    if (m_isHandling)
        return std::unexpected(AdoptTransactionError::Reentrant);
    const WitnessScope scope(m_isHandling);
    return m_transactions->advanceTime(nowMicroseconds);
}

std::expected<ReclaimTransaction::Record*, AdoptTransactionError> ReclaimTransaction::find(
    ReclaimAuthorityLease lease, const ReclaimTransactionSelector& selector)
{
    for (std::size_t index = 0; index < m_limits.transactions.outcomeCapacity; ++index) {
        auto& record = m_records[index];
        if (!record.isOccupied || record.witness.selector.operation != selector.operation)
            continue;
        if (record.authority != lease || record.witness.selector != selector)
            return std::unexpected(AdoptTransactionError::Conflict);
        return &record;
    }
    return nullptr;
}

ReclaimTransactionOutcome ReclaimTransaction::result(const ReclaimTransactionSelector& selector, const Record* record, AdoptOutcome outcome) const
{
    if (!record)
        return { .selector = selector, .capsuleSHA256 = std::nullopt, .capsuleBytes = 0, .outcome = outcome };
    return { .selector = record->witness.selector, .capsuleSHA256 = record->witness.capsuleSHA256, .capsuleBytes = record->witness.capsuleBytes, .outcome = outcome };
}

std::expected<ReclaimTransactionOutcome, AdoptTransactionError> ReclaimTransaction::prepare(
    ReclaimAuthorityLease lease, const ReclaimWitness& witness, AdoptTransactionDriver& delegate)
{
    if (m_isHandling)
        return std::unexpected(AdoptTransactionError::Reentrant);
    const WitnessScope scope(m_isHandling);
    if (!owns(lease))
        return std::unexpected(AdoptTransactionError::StaleAuthority);
    const auto preparation = reclaimWitnessPreparation(witness, m_limits.maxCapsuleBytes);
    if (!preparation)
        return std::unexpected(preparation.error());
    Driver driver(*this, lease, witness.selector, delegate);
    const auto observed = m_transactions->status(lease.m_reclaim, witness.selector.operation, driver);
    if (!observed)
        return std::unexpected(observed.error());
    const auto found = find(lease, witness.selector);
    if (!found)
        return std::unexpected(found.error());
    Record* record = *found;
    const bool isNew = !record;
    if (record && record->witness != witness)
        return std::unexpected(AdoptTransactionError::Conflict);
    if (!record) {
        if (observed->disposition != AdoptDisposition::Unknown)
            return std::unexpected(AdoptTransactionError::Conflict);
        for (std::size_t index = 0; index < m_limits.transactions.outcomeCapacity; ++index) {
            if (!m_records[index].isOccupied) {
                record = &m_records[index];
                break;
            }
        }
        if (!record)
            return std::unexpected(AdoptTransactionError::Busy);
        record->authority = lease;
        record->witness = witness;
        record->isOccupied = true;
        ++m_witnessCount;
    }
    const auto before = m_transactions->outcomeCount();
    const auto prepared = m_transactions->prepare(lease.m_reclaim, *preparation, driver);
    // A base refusal before reserving an outcome must not consume witness slots.
    // A refusal after reservation retains both records, including Rejected
    // outcomes hidden by a fresh-authority failure after candidate construction.
    if (isNew && m_transactions->outcomeCount() == before) {
        *record = Record {};
        --m_witnessCount;
        if (prepared)
            return std::unexpected(AdoptTransactionError::Conflict);
    }
    if (!prepared)
        return std::unexpected(prepared.error());
    if (!isEmpty(prepared->target) && prepared->target != targetOf(witness.selector))
        return std::unexpected(AdoptTransactionError::Conflict);
    return result(witness.selector, record, *prepared);
}

std::expected<ReclaimTransactionOutcome, AdoptTransactionError> ReclaimTransaction::command(Local::ReclaimWireMessage kind,
    ReclaimAuthorityLease lease, const ReclaimTransactionSelector& selector,
    const std::optional<std::array<std::uint8_t, 32>>& digest, AdoptTransactionDriver& delegate)
{
    if (m_isHandling)
        return std::unexpected(AdoptTransactionError::Reentrant);
    const WitnessScope scope(m_isHandling);
    if (!owns(lease))
        return std::unexpected(AdoptTransactionError::StaleAuthority);
    if (!reclaimSelectorToWire(selector) || (kind == Local::ReclaimWireMessage::Commit && !digest))
        return std::unexpected(AdoptTransactionError::InvalidIdentity);
    if (kind != Local::ReclaimWireMessage::Status && kind != Local::ReclaimWireMessage::Commit && kind != Local::ReclaimWireMessage::Abort)
        return std::unexpected(AdoptTransactionError::InvalidIdentity);
    Driver driver(*this, lease, selector, delegate);
    const auto observed = m_transactions->status(lease.m_reclaim, selector.operation, driver);
    if (!observed)
        return std::unexpected(observed.error());
    const auto found = find(lease, selector);
    if (!found)
        return std::unexpected(found.error());
    const Record* record = *found;
    if (!record) {
        if (observed->disposition != AdoptDisposition::Unknown)
            return std::unexpected(AdoptTransactionError::Conflict);
        return result(selector, nullptr, *observed);
    }
    if (observed->disposition == AdoptDisposition::Unknown || (digest && *digest != record->witness.capsuleSHA256)
        || (!digest && observed->disposition == AdoptDisposition::Committed))
        return std::unexpected(AdoptTransactionError::Conflict);
    if (!isEmpty(observed->target) && observed->target != targetOf(selector))
        return std::unexpected(AdoptTransactionError::Conflict);
    if (kind == Local::ReclaimWireMessage::Status)
        return result(selector, record, *observed);
    const auto outcome = kind == Local::ReclaimWireMessage::Commit
        ? m_transactions->commit(lease.m_reclaim, selector.operation, driver)
        : m_transactions->abort(lease.m_reclaim, selector.operation, driver);
    if (!outcome)
        return std::unexpected(outcome.error());
    return result(selector, record, *outcome);
}

std::expected<ReclaimTransactionOutcome, AdoptTransactionError> ReclaimTransaction::commit(ReclaimAuthorityLease lease,
    const ReclaimTransactionSelector& selector, const std::array<std::uint8_t, 32>& digest, AdoptTransactionDriver& driver)
{
    return command(Local::ReclaimWireMessage::Commit, lease, selector, digest, driver);
}

std::expected<ReclaimTransactionOutcome, AdoptTransactionError> ReclaimTransaction::status(ReclaimAuthorityLease lease,
    const ReclaimTransactionSelector& selector, const std::optional<std::array<std::uint8_t, 32>>& digest, AdoptTransactionDriver& driver)
{
    return command(Local::ReclaimWireMessage::Status, lease, selector, digest, driver);
}

std::expected<ReclaimTransactionOutcome, AdoptTransactionError> ReclaimTransaction::abort(ReclaimAuthorityLease lease,
    const ReclaimTransactionSelector& selector, const std::optional<std::array<std::uint8_t, 32>>& digest, AdoptTransactionDriver& driver)
{
    return command(Local::ReclaimWireMessage::Abort, lease, selector, digest, driver);
}

std::expected<void, AdoptTransactionError> ReclaimTransaction::retireAuthority(ReclaimAuthorityLease lease, AdoptTransactionDriver& driver)
{
    if (m_isHandling)
        return std::unexpected(AdoptTransactionError::Reentrant);
    const WitnessScope scope(m_isHandling);
    if (!owns(lease))
        return std::unexpected(AdoptTransactionError::StaleAuthority);
    if (const auto retired = m_transactions->retireAuthority(lease.m_reclaim, driver); !retired)
        return std::unexpected(retired.error());
    for (std::size_t index = 0; index < m_limits.transactions.outcomeCapacity; ++index) {
        auto& record = m_records[index];
        if (record.isOccupied && record.authority == lease) {
            record = Record {};
            --m_witnessCount;
        }
    }
    return {};
}

std::size_t ReclaimTransaction::outcomeCount() const
{
    return m_transactions ? m_transactions->outcomeCount() : 0;
}

} // namespace Ubiquitty::Session
