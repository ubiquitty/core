// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "AdoptTransaction.h"

#include <limits>
#include <new>
#include <utility>

namespace Ubiquitty::Session {

namespace {

bool isValid(Protocol::Sync::ID128 identity)
{
    return identity.low || identity.high;
}

bool isValid(AdoptOperation operation)
{
    return isValid(operation.operationID) && operation.daemonInstanceID;
}

bool isValid(AdoptTarget target)
{
    return target.sessionID && isValid(target.paneID);
}

bool isEmpty(AdoptTarget target)
{
    return !target.sessionID && !isValid(target.paneID);
}

bool isValid(const AdoptAuthority& authority)
{
    return authority.processID > 0;
}

AdoptOutcome unknownOutcome(AdoptOperation operation)
{
    AdoptOutcome outcome;
    outcome.operation = operation;
    return outcome;
}

bool isTerminal(AdoptDisposition disposition)
{
    return disposition == AdoptDisposition::Committed || disposition == AdoptDisposition::Aborted
        || disposition == AdoptDisposition::Expired || disposition == AdoptDisposition::Rejected;
}

// Named rather than compared against the last enumerator, so a reason added
// later is refused here until it is listed.
bool isKnownRejection(AdoptRejection rejection)
{
    switch (rejection) {
    case AdoptRejection::InvalidPayload:
    case AdoptRejection::NotOwner:
    case AdoptRejection::NoMemory:
    case AdoptRejection::InvalidCandidate:
    case AdoptRejection::HostFull:
        return true;
    }
    return false;
}

class HandlingScope {
public:
    explicit HandlingScope(bool& isHandling) // bool-ok: borrows reentrancy data, not an operation choice.
        : m_isHandling(isHandling)
    {
        m_isHandling = true;
    }
    ~HandlingScope() { m_isHandling = false; }

private:
    bool& m_isHandling;
};

} // namespace

struct AdoptTransaction::Authority {
    AdoptAuthority owner;
    std::uint64_t generation = 0;
};

struct AdoptTransaction::Record {
    AdoptAuthorityLease authority;
    AdoptPreparation preparation;
    AdoptOutcome outcome;
    std::unique_ptr<AdoptCandidate> candidate;
    std::uint64_t preparedUntilMicroseconds = 0;
    bool isOccupied = false;
};

AdoptTransaction::AdoptTransaction(std::uint64_t daemonInstanceID, AdoptTransactionLimits limits)
    : m_daemonInstanceID(daemonInstanceID)
    , m_limits(limits)
{
}

AdoptTransaction::~AdoptTransaction() = default;

std::expected<std::unique_ptr<AdoptTransaction>, AdoptTransactionError> AdoptTransaction::create(std::uint64_t daemonInstanceID, AdoptTransactionLimits limits)
{
    if (!daemonInstanceID || !limits.authorityCapacity || !limits.outcomeCapacity || !limits.preparedLeaseMicroseconds
        || limits.authorityCapacity > static_cast<std::size_t>(std::numeric_limits<std::ptrdiff_t>::max()) / sizeof(Authority)
        || limits.outcomeCapacity > static_cast<std::size_t>(std::numeric_limits<std::ptrdiff_t>::max()) / sizeof(Record))
        return std::unexpected(AdoptTransactionError::InvalidPolicy);
    std::unique_ptr<AdoptTransaction> table(new (std::nothrow) AdoptTransaction(daemonInstanceID, limits));
    if (!table)
        return std::unexpected(AdoptTransactionError::NoMemory);
    table->m_authorities.reset(new (std::nothrow) Authority[limits.authorityCapacity]);
    table->m_records.reset(new (std::nothrow) Record[limits.outcomeCapacity]);
    if (!table->m_authorities || !table->m_records)
        return std::unexpected(AdoptTransactionError::NoMemory);
    return table;
}

std::expected<AdoptAuthorityLease, AdoptTransactionError> AdoptTransaction::admitAuthority(const AdoptAuthority& owner, AdoptTransactionDriver& driver)
{
    if (m_isHandling)
        return std::unexpected(AdoptTransactionError::Reentrant);
    const HandlingScope handling(m_isHandling);
    if (!isValid(owner))
        return std::unexpected(AdoptTransactionError::InvalidIdentity);
    if (const auto proof = driver.verifyAuthority(owner); !proof)
        return std::unexpected(proof.error());
    Authority* available = nullptr;
    for (std::size_t index = 0; index < m_limits.authorityCapacity; ++index) {
        auto& authority = m_authorities[index];
        if (!authority.generation) {
            if (!available)
                available = &authority;
            continue;
        }
        if (authority.owner.processID == owner.processID && authority.owner.birth == owner.birth) {
            if (authority.owner != owner)
                return std::unexpected(AdoptTransactionError::Conflict);
            return AdoptAuthorityLease(index, authority.generation);
        }
    }
    if (!available)
        return std::unexpected(AdoptTransactionError::Busy);
    if (!m_nextAuthorityGeneration)
        return std::unexpected(AdoptTransactionError::GenerationExhausted);
    const auto slot = static_cast<std::size_t>(available - m_authorities.get());
    available->owner = owner;
    available->generation = m_nextAuthorityGeneration;
    m_nextAuthorityGeneration = m_nextAuthorityGeneration == std::numeric_limits<std::uint64_t>::max() ? 0 : m_nextAuthorityGeneration + 1;
    return AdoptAuthorityLease(slot, available->generation);
}

std::expected<AdoptTransaction::Authority*, AdoptTransactionError> AdoptTransaction::verified(AdoptAuthorityLease lease, AdoptTransactionDriver& driver)
{
    if (!lease.m_generation || lease.m_slot >= m_limits.authorityCapacity)
        return std::unexpected(AdoptTransactionError::StaleAuthority);
    auto& authority = m_authorities[lease.m_slot];
    if (authority.generation != lease.m_generation)
        return std::unexpected(AdoptTransactionError::StaleAuthority);
    if (const auto proof = driver.verifyAuthority(authority.owner); !proof)
        return std::unexpected(proof.error());
    return &authority;
}

std::expected<AdoptTransaction::Record*, AdoptTransactionError> AdoptTransaction::find(AdoptAuthorityLease lease, AdoptOperation operation)
{
    if (!isValid(operation) || operation.daemonInstanceID != m_daemonInstanceID)
        return std::unexpected(AdoptTransactionError::InvalidIdentity);
    for (std::size_t index = 0; index < m_limits.outcomeCapacity; ++index) {
        auto& record = m_records[index];
        if (!record.isOccupied || record.outcome.operation != operation)
            continue;
        if (record.authority != lease)
            return std::unexpected(AdoptTransactionError::Conflict);
        return &record;
    }
    return nullptr;
}

void AdoptTransaction::endPrepared(Record& record, AdoptDisposition disposition)
{
    record.outcome.disposition = disposition;
    record.candidate.reset();
}

std::expected<void, AdoptTransactionError> AdoptTransaction::advanceTime(std::uint64_t nowMicroseconds)
{
    if (m_isHandling)
        return std::unexpected(AdoptTransactionError::Reentrant);
    const HandlingScope handling(m_isHandling);
    if (nowMicroseconds < m_nowMicroseconds)
        return std::unexpected(AdoptTransactionError::InvalidTime);
    m_nowMicroseconds = nowMicroseconds;
    for (std::size_t index = 0; index < m_limits.outcomeCapacity; ++index) {
        auto& record = m_records[index];
        if (record.isOccupied && record.outcome.disposition == AdoptDisposition::Prepared && nowMicroseconds >= record.preparedUntilMicroseconds)
            endPrepared(record, AdoptDisposition::Expired);
    }
    return {};
}

std::expected<AdoptOutcome, AdoptTransactionError> AdoptTransaction::prepare(AdoptAuthorityLease lease, const AdoptPreparation& preparation, AdoptTransactionDriver& driver)
{
    if (m_isHandling)
        return std::unexpected(AdoptTransactionError::Reentrant);
    const HandlingScope handling(m_isHandling);
    if (const auto proof = verified(lease, driver); !proof)
        return std::unexpected(proof.error());
    if (!preparation.bodyBytes)
        return std::unexpected(AdoptTransactionError::InvalidIdentity);
    const auto found = find(lease, preparation.operation);
    if (!found)
        return std::unexpected(found.error());
    if (*found) {
        if ((*found)->preparation != preparation)
            return std::unexpected(AdoptTransactionError::Conflict);
        return (*found)->outcome;
    }
    Record* available = nullptr;
    for (std::size_t index = 0; index < m_limits.outcomeCapacity; ++index) {
        auto& record = m_records[index];
        if (!record.isOccupied && !available)
            available = &record;
        if (record.isOccupied && record.authority == lease && record.outcome.disposition == AdoptDisposition::Prepared)
            return std::unexpected(AdoptTransactionError::Busy);
    }
    if (!available)
        return std::unexpected(AdoptTransactionError::Busy);
    if (m_limits.preparedLeaseMicroseconds > std::numeric_limits<std::uint64_t>::max() - m_nowMicroseconds)
        return std::unexpected(AdoptTransactionError::InvalidTime);

    available->authority = lease;
    available->preparation = preparation;
    available->outcome.operation = preparation.operation;
    available->preparedUntilMicroseconds = m_nowMicroseconds + m_limits.preparedLeaseMicroseconds;
    available->isOccupied = true;
    ++m_outcomeCount;
    auto candidate = driver.prepareCandidate(preparation);
    if (!candidate || !*candidate) {
        available->outcome.disposition = AdoptDisposition::Rejected;
        available->outcome.rejection = AdoptRejection::InvalidCandidate;
        if (!candidate && isKnownRejection(candidate.error()))
            available->outcome.rejection = candidate.error();
        return available->outcome;
    }
    const auto target = (*candidate)->target();
    if (!isValid(target)) {
        available->outcome.disposition = AdoptDisposition::Rejected;
        available->outcome.rejection = AdoptRejection::InvalidCandidate;
        return available->outcome;
    }
    if (const auto proof = verified(lease, driver); !proof) {
        available->outcome.disposition = AdoptDisposition::Rejected;
        available->outcome.rejection = AdoptRejection::NotOwner;
        return std::unexpected(proof.error());
    }
    available->outcome.target = target;
    available->candidate = std::move(*candidate);
    available->outcome.disposition = AdoptDisposition::Prepared;
    return available->outcome;
}

std::expected<AdoptOutcome, AdoptTransactionError> AdoptTransaction::commit(AdoptAuthorityLease lease, AdoptOperation operation, AdoptTransactionDriver& driver)
{
    if (m_isHandling)
        return std::unexpected(AdoptTransactionError::Reentrant);
    const HandlingScope handling(m_isHandling);
    if (const auto proof = verified(lease, driver); !proof)
        return std::unexpected(proof.error());
    const auto found = find(lease, operation);
    if (!found)
        return std::unexpected(found.error());
    if (!*found)
        return unknownOutcome(operation);
    auto& record = **found;
    if (record.outcome.disposition != AdoptDisposition::Prepared)
        return record.outcome;
    if (!record.candidate || record.candidate->target() != record.outcome.target) {
        record.outcome.rejection = AdoptRejection::InvalidCandidate;
        endPrepared(record, AdoptDisposition::Rejected);
        return record.outcome;
    }
    auto committed = record.outcome;
    committed.disposition = AdoptDisposition::Committed;
    auto reservation = driver.reserveCommittedReply(committed, *record.candidate);
    if (!reservation || !*reservation)
        return std::unexpected(AdoptTransactionError::ReplyUnavailable);
    if (const auto proof = verified(lease, driver); !proof)
        return std::unexpected(proof.error());
    if (record.candidate->target() != record.outcome.target) {
        record.outcome.rejection = AdoptRejection::InvalidCandidate;
        endPrepared(record, AdoptDisposition::Rejected);
        return record.outcome;
    }
    record.outcome = committed;
    driver.activate(std::move(record.candidate), std::move(*reservation));
    return record.outcome;
}

std::expected<AdoptOutcome, AdoptTransactionError> AdoptTransaction::abort(AdoptAuthorityLease lease, AdoptOperation operation, AdoptTransactionDriver& driver)
{
    if (m_isHandling)
        return std::unexpected(AdoptTransactionError::Reentrant);
    const HandlingScope handling(m_isHandling);
    if (const auto proof = verified(lease, driver); !proof)
        return std::unexpected(proof.error());
    const auto found = find(lease, operation);
    if (!found)
        return std::unexpected(found.error());
    if (!*found)
        return unknownOutcome(operation);
    auto& record = **found;
    if (record.outcome.disposition == AdoptDisposition::Prepared)
        endPrepared(record, AdoptDisposition::Aborted);
    return record.outcome;
}

std::expected<AdoptOutcome, AdoptTransactionError> AdoptTransaction::status(AdoptAuthorityLease lease, AdoptOperation operation, AdoptTransactionDriver& driver)
{
    if (m_isHandling)
        return std::unexpected(AdoptTransactionError::Reentrant);
    const HandlingScope handling(m_isHandling);
    if (const auto proof = verified(lease, driver); !proof)
        return std::unexpected(proof.error());
    const auto found = find(lease, operation);
    if (!found)
        return std::unexpected(found.error());
    return *found ? (*found)->outcome : unknownOutcome(operation);
}

std::expected<void, AdoptTransactionError> AdoptTransaction::retireAuthority(AdoptAuthorityLease lease, AdoptTransactionDriver& driver)
{
    if (m_isHandling)
        return std::unexpected(AdoptTransactionError::Reentrant);
    const HandlingScope handling(m_isHandling);
    if (!lease.m_generation || lease.m_slot >= m_limits.authorityCapacity
        || m_authorities[lease.m_slot].generation != lease.m_generation)
        return std::unexpected(AdoptTransactionError::StaleAuthority);
    for (std::size_t index = 0; index < m_limits.outcomeCapacity; ++index) {
        const auto& record = m_records[index];
        if (record.isOccupied && record.authority == lease && !isTerminal(record.outcome.disposition))
            return std::unexpected(AdoptTransactionError::ActiveCandidate);
    }
    auto& authority = m_authorities[lease.m_slot];
    if (driver.retirementEvidence(authority.owner) != AdoptRetirementEvidence::ProcessGoneAndAllChannelsAndOperationsRevokedAndDrained)
        return std::unexpected(AdoptTransactionError::RetirementUnproven);
    for (std::size_t index = 0; index < m_limits.outcomeCapacity; ++index) {
        auto& record = m_records[index];
        if (record.isOccupied && record.authority == lease) {
            record = Record {};
            --m_outcomeCount;
        }
    }
    authority = Authority {};
    return {};
}

std::expected<void, AdoptTransactionError> AdoptClientAttempt::beginCommit()
{
    if (!isValid(m_operation))
        return std::unexpected(AdoptTransactionError::InvalidIdentity);
    if (m_disposition != AdoptDisposition::Prepared && !(m_didAttemptCommit && m_disposition == AdoptDisposition::Unknown))
        return std::unexpected(AdoptTransactionError::Conflict);
    m_didAttemptCommit = true;
    m_disposition = AdoptDisposition::Unknown;
    return {};
}

std::expected<void, AdoptTransactionError> AdoptClientAttempt::observe(const AdoptOutcome& outcome)
{
    if (!isValid(m_operation) || outcome.operation != m_operation)
        return std::unexpected(AdoptTransactionError::InvalidIdentity);
    if ((outcome.disposition == AdoptDisposition::Rejected) != outcome.rejection.has_value())
        return std::unexpected(AdoptTransactionError::Conflict);
    if (outcome.rejection && !isKnownRejection(*outcome.rejection))
        return std::unexpected(AdoptTransactionError::Conflict);
    if (outcome.disposition != AdoptDisposition::Unknown && outcome.disposition != AdoptDisposition::Prepared && !isTerminal(outcome.disposition))
        return std::unexpected(AdoptTransactionError::Conflict);
    if (outcome.disposition == AdoptDisposition::Unknown) {
        if (!isEmpty(outcome.target))
            return std::unexpected(AdoptTransactionError::Conflict);
        return {};
    }
    if (isTerminal(m_disposition)) {
        if (outcome.disposition != m_disposition || outcome.target != m_target || outcome.rejection != m_rejection)
            return std::unexpected(AdoptTransactionError::Conflict);
        return {};
    }
    if (outcome.disposition == AdoptDisposition::Prepared || outcome.disposition == AdoptDisposition::Committed) {
        if (!isValid(outcome.target) || (!isEmpty(m_target) && outcome.target != m_target)
            || (outcome.disposition == AdoptDisposition::Committed && !m_didAttemptCommit))
            return std::unexpected(AdoptTransactionError::Conflict);
        m_target = outcome.target;
        if (outcome.disposition == AdoptDisposition::Prepared && m_didAttemptCommit)
            return {};
    } else {
        if ((!isEmpty(m_target) && outcome.target != m_target) || (!isEmpty(outcome.target) && !isValid(outcome.target)))
            return std::unexpected(AdoptTransactionError::Conflict);
        m_target = outcome.target;
    }
    m_disposition = outcome.disposition;
    m_rejection = outcome.rejection;
    return {};
}

bool AdoptClientAttempt::mayResumeLocal() const
{
    return isValid(m_operation) && m_disposition != AdoptDisposition::Committed
        && (!m_didAttemptCommit || m_disposition == AdoptDisposition::Aborted || m_disposition == AdoptDisposition::Expired || m_disposition == AdoptDisposition::Rejected);
}

} // namespace Ubiquitty::Session
