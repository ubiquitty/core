// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Session/ReclaimTransaction.h"
#ifndef UBIQUITTY_TEST_WITHOUT_CORE
#define UBIQUITTY_TEST_WITHOUT_CORE
#endif
#include "Harness.h"
#include "ScopedNothrowAllocationRefusal.h"

#include <array>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <type_traits>
#include <utility>

void* operator new(std::size_t size, const std::nothrow_t&) noexcept
{
    if (UbiquittyTest::ScopedNothrowAllocationRefusal::refuses(size))
        return nullptr;
    return ::operator new(size);
}

void operator delete(void* pointer, const std::nothrow_t&) noexcept { ::operator delete(pointer); }

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept
{
    if (UbiquittyTest::ScopedNothrowAllocationRefusal::refuses(size))
        return nullptr;
    return ::operator new[](size);
}

void operator delete[](void* pointer, const std::nothrow_t&) noexcept { ::operator delete[](pointer); }

namespace {

using namespace Ubiquitty::Session;
constexpr std::uint64_t daemonInstance = 17;
constexpr AdoptOperation adoption { { 151, 161 }, daemonInstance };
constexpr AdoptOperation operation { { 51, 61 }, daemonInstance };
constexpr Ubiquitty::Protocol::Control::ReclaimTarget target {
    .handle = 3,
    .sessionID = 11,
    .daemonInstanceID = daemonInstance,
    .hostInstanceID = { 21, 31 },
    .attachmentID = { 41, 51 },
    .paneID = { 11, 21 },
};
constexpr ReclaimInputBarrier barrier { { 71, 81 }, 12 };
constexpr ReclaimTransactionSelector selector { adoption, operation, target, barrier };
constexpr ReclaimTransactionLimits limits { { 2, 3, 10 }, 4096 };

static_assert(!std::is_convertible_v<ReclaimAuthorityLease, AdoptAuthorityLease>);
static_assert(!std::is_convertible_v<AdoptAuthorityLease, ReclaimAuthorityLease>);

AdoptAuthority authority(std::uint64_t birth = 7)
{
    AdoptAuthority value;
    value.processID = 42;
    value.birth.low = birth;
    value.executableSHA256[0] = 8;
    return value;
}

ReclaimWitness witness(std::uint64_t number = operation.operationID.low)
{
    ReclaimWitness value { .selector = selector, .capsuleBytes = 1000 };
    value.selector.operation.operationID.low = number;
    value.capsuleSHA256[0] = 9;
    value.capsuleSHA256[31] = 29;
    return value;
}

struct Counts {
    unsigned preparations = 0;
    unsigned stagedDestructions = 0;
    unsigned activeDestructions = 0;
    unsigned reservations = 0;
    unsigned activations = 0;
    unsigned verifications = 0;
    unsigned reentrantRefusals = 0;
};

class Candidate final : public AdoptCandidate {
public:
    Candidate(Counts& counts, Candidate*& observed, AdoptTarget target)
        : identity(target)
        , m_counts(counts)
        , m_observed(observed)
    {
        m_observed = this;
    }
    ~Candidate() override
    {
        if (isActive)
            ++m_counts.activeDestructions;
        else
            ++m_counts.stagedDestructions;
        if (m_observed == this)
            m_observed = nullptr;
    }
    AdoptTarget target() const override { return identity; }
    AdoptTarget identity;
    bool isActive = false;

private:
    Counts& m_counts;
    Candidate*& m_observed;
};

class Reservation final : public AdoptCommitReservation { };

/// No OS graph: all authority/lifetime evidence is an explicit fake value. The
/// fixture delegates real state transitions to the production AdoptTransaction.
class Driver final : public AdoptTransactionDriver {
public:
    explicit Driver(Counts& counts)
        : counts(counts)
    {
    }

    std::expected<void, AdoptTransactionError> verifyAuthority(const AdoptAuthority& proposed) override
    {
        ++counts.verifications;
        if (verificationsBeforeRefusal && !*verificationsBeforeRefusal)
            return std::unexpected(AdoptTransactionError::Unauthorised);
        if (verificationsBeforeRefusal)
            --*verificationsBeforeRefusal;
        if (!isPeerAuthorised || proposed != physicalAuthority)
            return std::unexpected(AdoptTransactionError::Unauthorised);
        return {};
    }
    std::expected<std::unique_ptr<AdoptCandidate>, AdoptRejection> prepareCandidate(const AdoptPreparation& preparation) override
    {
        ++counts.preparations;
        lastPreparation = preparation;
        if (ledger) {
            CHECK(ledger->witnessCount() > 0);
            if (shouldReenter) {
                const auto reentered = ledger->abort(lease, current.selector, current.capsuleSHA256, *this);
                CHECK(!reentered);
                if (!reentered && reentered.error() == AdoptTransactionError::Reentrant)
                    ++counts.reentrantRefusals;
            }
        }
        if (rejection)
            return std::unexpected(*rejection);
        auto identity = AdoptTarget { .sessionID = target.sessionID, .paneID = target.paneID };
        if (shouldReturnWrongTarget)
            ++identity.sessionID;
        std::unique_ptr<AdoptCandidate> candidate(new (std::nothrow) Candidate(counts, observed, identity));
        if (!candidate)
            return std::unexpected(AdoptRejection::NoMemory);
        if (shouldRevokeDuringPreparation)
            isPeerAuthorised = false;
        return candidate;
    }
    std::expected<std::unique_ptr<AdoptCommitReservation>, AdoptTransactionError> reserveCommittedReply(
        const AdoptOutcome& outcome, const AdoptCandidate& candidate) override
    {
        ++counts.reservations;
        CHECK(candidate.target() == outcome.target);
        if (shouldRefuseReservation)
            return std::unexpected(AdoptTransactionError::NoMemory);
        if (shouldRevokeDuringReservation)
            isPeerAuthorised = false;
        if (shouldExhaustOriginalProofDuringReservation)
            verificationsBeforeRefusal = 1;
        if (shouldRetargetDuringReservation && observed)
            ++observed->identity.sessionID;
        std::unique_ptr<AdoptCommitReservation> reservation(new (std::nothrow) Reservation);
        if (!reservation)
            return std::unexpected(AdoptTransactionError::NoMemory);
        return reservation;
    }
    void activate(std::unique_ptr<AdoptCandidate> candidate, std::unique_ptr<AdoptCommitReservation>) noexcept override
    {
        ++counts.activations;
        static_cast<Candidate*>(candidate.get())->isActive = true;
        for (auto& owner : active) {
            if (!owner) {
                owner = std::move(candidate);
                return;
            }
        }
        CHECK_MSG(false, "fixed fake active-owner storage must cover every activation");
    }
    AdoptRetirementEvidence retirementEvidence(const AdoptAuthority& proposed) override
    {
        return isProcessGone && proposed == physicalAuthority ? retirement : AdoptRetirementEvidence::Unproven;
    }

    Counts& counts;
    AdoptAuthority physicalAuthority = authority();
    ReclaimTransaction* ledger = nullptr;
    ReclaimAuthorityLease lease;
    ReclaimWitness current = witness();
    Candidate* observed = nullptr;
    std::array<std::unique_ptr<AdoptCandidate>, 4> active;
    std::optional<AdoptPreparation> lastPreparation;
    std::optional<AdoptRejection> rejection;
    std::optional<unsigned> verificationsBeforeRefusal;
    AdoptRetirementEvidence retirement = AdoptRetirementEvidence::Unproven;
    bool isPeerAuthorised = true;
    bool isProcessGone = false;
    bool shouldReturnWrongTarget = false;
    bool shouldRevokeDuringPreparation = false;
    bool shouldRefuseReservation = false;
    bool shouldRevokeDuringReservation = false;
    bool shouldExhaustOriginalProofDuringReservation = false;
    bool shouldRetargetDuringReservation = false;
    bool shouldReenter = false;
};

enum class OriginalState {
    Prepared,
    Committed
};

struct Fixture {
    Counts originalCounts;
    Counts counts;
    Driver originalDriver { originalCounts };
    Driver driver { counts };
    std::unique_ptr<AdoptTransaction> originals;
    std::unique_ptr<ReclaimTransaction> ledger;
    std::optional<AdoptAuthorityLease> originalLease;
    std::optional<ReclaimAuthorityLease> lease;

    explicit Fixture(OriginalState state = OriginalState::Committed, ReclaimTransactionLimits policy = limits)
    {
        auto original = AdoptTransaction::create(daemonInstance, { .authorityCapacity = 2, .outcomeCapacity = 3, .preparedLeaseMicroseconds = 10 });
        if (!original)
            return;
        originals = std::move(*original);
        const auto admitted = originals->admitAuthority(originalDriver.physicalAuthority, originalDriver);
        if (!admitted)
            return;
        originalLease = *admitted;
        const auto prepared = originals->prepare(*originalLease, { .operation = adoption, .bodyBytes = 100 }, originalDriver);
        if (!prepared || prepared->disposition != AdoptDisposition::Prepared)
            return;
        if (state == OriginalState::Committed) {
            const auto committed = originals->commit(*originalLease, adoption, originalDriver);
            if (!committed || committed->disposition != AdoptDisposition::Committed)
                return;
        }
        auto created = ReclaimTransaction::create(daemonInstance, *originals, policy);
        if (!created)
            return;
        ledger = std::move(*created);
        const auto reverse = ledger->admitAuthority(driver.physicalAuthority, *originalLease, driver);
        if (!reverse)
            return;
        lease = *reverse;
        driver.ledger = ledger.get();
        driver.lease = *lease;
    }

    bool isReady() const { return originals && originalLease && ledger && lease; }

    std::expected<ReclaimTransactionOutcome, AdoptTransactionError> prepare(ReclaimWitness value = witness())
    {
        driver.current = value;
        return ledger->prepare(*lease, value, driver);
    }
};

} // namespace

TEST(reclaimLedgerReservesDurableWitnessBeforeCandidateAndReusesExactDuplicate)
{
    Fixture fixture;
    REQUIRE(fixture.isReady());
    const auto prepared = fixture.prepare();
    REQUIRE(prepared);
    CHECK(prepared->outcome.disposition == AdoptDisposition::Prepared);
    CHECK(prepared->selector == selector);
    REQUIRE(prepared->capsuleSHA256);
    CHECK(*prepared->capsuleSHA256 == witness().capsuleSHA256);
    CHECK_EQ(prepared->capsuleBytes, witness().capsuleBytes);
    CHECK_EQ(fixture.ledger->witnessCount(), 1u);
    CHECK_EQ(fixture.ledger->outcomeCount(), 1u);
    CHECK_EQ(fixture.counts.preparations, 1u);
    const auto duplicate = fixture.prepare();
    REQUIRE(duplicate);
    CHECK(duplicate->outcome == prepared->outcome);
    CHECK(duplicate->capsuleSHA256 == prepared->capsuleSHA256);
    CHECK_EQ(fixture.counts.preparations, 1u);
    const auto canonical = reclaimWitnessPreparation(witness(), limits.maxCapsuleBytes);
    REQUIRE(canonical && fixture.driver.lastPreparation);
    CHECK(*canonical == *fixture.driver.lastPreparation);
}

TEST(reclaimLedgerPreparationHashBindsEverySelectorActualDigestAndLength)
{
    const auto original = reclaimWitnessPreparation(witness(), limits.maxCapsuleBytes);
    REQUIRE(original);
    const auto changed = [&](auto mutate) {
        auto value = witness();
        mutate(value);
        const auto prepared = reclaimWitnessPreparation(value, limits.maxCapsuleBytes);
        CHECK(prepared.has_value());
        if (prepared)
            CHECK(prepared->bodySHA256 != original->bodySHA256);
    };
    changed([](auto& value) { ++value.selector.adoption.operationID.low; });
    changed([](auto& value) { ++value.selector.adoption.operationID.high; });
    changed([](auto& value) { ++value.selector.operation.operationID.low; });
    changed([](auto& value) { ++value.selector.operation.operationID.high; });
    changed([](auto& value) { ++value.selector.target.handle; });
    changed([](auto& value) { ++value.selector.target.sessionID; });
    changed([](auto& value) { ++value.selector.target.hostInstanceID.low; });
    changed([](auto& value) { ++value.selector.target.hostInstanceID.high; });
    changed([](auto& value) { ++value.selector.target.attachmentID.low; });
    changed([](auto& value) { ++value.selector.target.attachmentID.high; });
    changed([](auto& value) { ++value.selector.target.paneID.low; });
    changed([](auto& value) { ++value.selector.target.paneID.high; });
    changed([](auto& value) { ++value.selector.barrier.clientInstance.low; });
    changed([](auto& value) { ++value.selector.barrier.clientInstance.high; });
    changed([](auto& value) { value.selector.barrier.finalInputSeq = 0; });
    changed([](auto& value) { ++value.capsuleBytes; });
    for (std::size_t index = 0; index < 32; ++index)
        changed([&](auto& value) { value.capsuleSHA256[index] ^= 1; });
    changed([](auto& value) {
        ++value.selector.adoption.daemonInstanceID;
        ++value.selector.operation.daemonInstanceID;
        ++value.selector.target.daemonInstanceID;
    });
    CHECK(!reclaimWitnessPreparation(witness(), 0));
    CHECK(!reclaimWitnessPreparation(witness(), witness().capsuleBytes - 1));
    auto empty = witness();
    empty.capsuleBytes = 0;
    CHECK(!reclaimWitnessPreparation(empty, limits.maxCapsuleBytes));
}

TEST(reclaimLedgerRejectsConflictingDuplicateSelectorsBeforeFactoryFastPath)
{
    Fixture fixture;
    REQUIRE(fixture.isReady());
    REQUIRE(fixture.prepare());
    const auto changed = [&](auto mutate) {
        auto value = witness();
        mutate(value);
        const auto duplicate = fixture.prepare(value);
        CHECK(!duplicate);
        CHECK_EQ(fixture.counts.preparations, 1u);
        CHECK_EQ(fixture.ledger->witnessCount(), 1u);
        CHECK_EQ(fixture.ledger->outcomeCount(), 1u);
    };
    changed([](auto& value) { ++value.selector.adoption.operationID.low; });
    changed([](auto& value) { ++value.selector.target.handle; });
    changed([](auto& value) { ++value.selector.target.sessionID; });
    changed([](auto& value) { ++value.selector.target.hostInstanceID.high; });
    changed([](auto& value) { ++value.selector.target.attachmentID.low; });
    changed([](auto& value) { ++value.selector.target.paneID.high; });
    changed([](auto& value) { ++value.selector.barrier.clientInstance.low; });
    changed([](auto& value) { ++value.selector.barrier.finalInputSeq; });
    changed([](auto& value) { ++value.capsuleBytes; });
    changed([](auto& value) { value.capsuleSHA256[31] ^= 1; });
    fixture.driver.current = witness();
    const auto status = fixture.ledger->status(*fixture.lease, selector, witness().capsuleSHA256, fixture.driver);
    REQUIRE(status);
    CHECK(status->outcome.disposition == AdoptDisposition::Prepared);
}

TEST(reclaimLedgerRequiresOriginalCommittedPhysicalOwnerBeforeAnyCandidateWork)
{
    Fixture uncommitted(OriginalState::Prepared);
    REQUIRE(uncommitted.isReady());
    const auto refused = uncommitted.prepare();
    REQUIRE(!refused);
    CHECK(refused.error() == AdoptTransactionError::Unauthorised);
    CHECK_EQ(uncommitted.counts.preparations, 0u);
    CHECK_EQ(uncommitted.ledger->witnessCount(), 0u);
    Fixture fixture;
    REQUIRE(fixture.isReady());
    fixture.driver.isPeerAuthorised = false;
    CHECK(!fixture.prepare());
    fixture.driver.isPeerAuthorised = true;
    ++fixture.driver.physicalAuthority.birth.low;
    CHECK(!fixture.prepare());
    CHECK_EQ(fixture.counts.preparations, 0u);
    fixture.driver.physicalAuthority = authority();
    CHECK(!fixture.ledger->admitAuthority(fixture.driver.physicalAuthority, AdoptAuthorityLease {}, fixture.driver));
    auto differentTable = ReclaimTransaction::create(daemonInstance, *fixture.originals, limits);
    REQUIRE(differentTable);
    CHECK(!(*differentTable)->status(*fixture.lease, selector, std::nullopt, fixture.driver));
}

TEST(reclaimLedgerUnknownNeverInventsWitnessOrReleaseFromProvidedDigest)
{
    Fixture fixture;
    REQUIRE(fixture.isReady());
    const auto attempts = UbiquittyTest::ScopedNothrowAllocationRefusal::attemptCount();
    {
        UbiquittyTest::ScopedNothrowAllocationRefusal refuse;
        for (const auto digest : { std::optional<std::array<std::uint8_t, 32>> {}, std::optional { witness().capsuleSHA256 } }) {
            const auto status = fixture.ledger->status(*fixture.lease, selector, digest, fixture.driver);
            const auto aborted = fixture.ledger->abort(*fixture.lease, selector, digest, fixture.driver);
            REQUIRE(status && aborted);
            CHECK(status->outcome.disposition == AdoptDisposition::Unknown);
            CHECK(aborted->outcome.disposition == AdoptDisposition::Unknown);
            CHECK(!status->capsuleSHA256 && !aborted->capsuleSHA256);
            CHECK_EQ(status->capsuleBytes, 0u);
        }
        const auto committed = fixture.ledger->commit(*fixture.lease, selector, witness().capsuleSHA256, fixture.driver);
        REQUIRE(committed);
        CHECK(committed->outcome.disposition == AdoptDisposition::Unknown);
        CHECK(!committed->capsuleSHA256);
    }
    CHECK_EQ(UbiquittyTest::ScopedNothrowAllocationRefusal::attemptCount(), attempts);
    CHECK_EQ(fixture.ledger->outcomeCount(), 0u);
    CHECK_EQ(fixture.ledger->witnessCount(), 0u);
    CHECK_EQ(fixture.counts.preparations, 0u);
}

TEST(reclaimLedgerCommittedWitnessSurvivesCandidateAndHostedSessionDeletion)
{
    Fixture fixture;
    REQUIRE(fixture.isReady());
    REQUIRE(fixture.prepare());
    const auto committed = fixture.ledger->commit(*fixture.lease, selector, witness().capsuleSHA256, fixture.driver);
    REQUIRE(committed);
    CHECK(committed->outcome.disposition == AdoptDisposition::Committed);
    CHECK_EQ(fixture.counts.activations, 1u);
    // The fake hosted object and every candidate owner go away. Outcome proof
    // lives in the original table and fixed reverse witness, not those objects.
    for (auto& owner : fixture.driver.active)
        owner.reset();
    for (auto& owner : fixture.originalDriver.active)
        owner.reset();
    CHECK_EQ(fixture.counts.activeDestructions, 1u);
    CHECK_EQ(fixture.originalCounts.activeDestructions, 1u);
    REQUIRE(fixture.ledger->advanceTime(10000));
    const auto attempts = UbiquittyTest::ScopedNothrowAllocationRefusal::attemptCount();
    {
        UbiquittyTest::ScopedNothrowAllocationRefusal refuse;
        const auto status = fixture.ledger->status(*fixture.lease, selector, witness().capsuleSHA256, fixture.driver);
        const auto repeated = fixture.ledger->commit(*fixture.lease, selector, witness().capsuleSHA256, fixture.driver);
        const auto lateAbort = fixture.ledger->abort(*fixture.lease, selector, witness().capsuleSHA256, fixture.driver);
        REQUIRE(status && repeated && lateAbort);
        CHECK(status->outcome == committed->outcome);
        CHECK(repeated->outcome == committed->outcome);
        CHECK(lateAbort->outcome == committed->outcome);
        CHECK(status->capsuleSHA256 == committed->capsuleSHA256);
        CHECK_EQ(status->capsuleBytes, 1000u);
        CHECK(!fixture.ledger->status(*fixture.lease, selector, std::nullopt, fixture.driver));
        CHECK(!fixture.ledger->abort(*fixture.lease, selector, std::nullopt, fixture.driver));
    }
    CHECK_EQ(UbiquittyTest::ScopedNothrowAllocationRefusal::attemptCount(), attempts);
    CHECK_EQ(fixture.counts.activations, 1u);
    CHECK_EQ(fixture.ledger->witnessCount(), 1u);
}

TEST(reclaimLedgerEveryCommandChecksRetainedSelectorAndActualDigest)
{
    Fixture fixture;
    REQUIRE(fixture.isReady());
    REQUIRE(fixture.prepare());
    const auto changed = [&](auto mutate) {
        auto value = selector;
        mutate(value);
        CHECK(!fixture.ledger->status(*fixture.lease, value, witness().capsuleSHA256, fixture.driver));
        CHECK(!fixture.ledger->commit(*fixture.lease, value, witness().capsuleSHA256, fixture.driver));
        CHECK(!fixture.ledger->abort(*fixture.lease, value, witness().capsuleSHA256, fixture.driver));
    };
    changed([](auto& value) { ++value.adoption.operationID.low; });
    changed([](auto& value) { ++value.target.handle; });
    changed([](auto& value) { ++value.target.hostInstanceID.high; });
    changed([](auto& value) { ++value.target.attachmentID.low; });
    changed([](auto& value) { ++value.target.paneID.high; });
    changed([](auto& value) { ++value.barrier.clientInstance.low; });
    changed([](auto& value) { ++value.barrier.finalInputSeq; });
    for (std::size_t index = 0; index < 32; ++index) {
        auto digest = witness().capsuleSHA256;
        digest[index] ^= 1;
        CHECK(!fixture.ledger->status(*fixture.lease, selector, digest, fixture.driver));
        CHECK(!fixture.ledger->commit(*fixture.lease, selector, digest, fixture.driver));
        CHECK(!fixture.ledger->abort(*fixture.lease, selector, digest, fixture.driver));
    }
    CHECK_EQ(fixture.counts.activations, 0u);
    const auto recovered = fixture.ledger->status(*fixture.lease, selector, std::nullopt, fixture.driver);
    REQUIRE(recovered && recovered->capsuleSHA256);
    CHECK(*recovered->capsuleSHA256 == witness().capsuleSHA256);
    CHECK(recovered->outcome.disposition == AdoptDisposition::Prepared);
}

TEST(reclaimLedgerPreReservationRefusalDoesNotLeakWitnessAndTerminalCapacityNeverEvicts)
{
    Fixture fixture(OriginalState::Committed, { { 2, 2, 10 }, 4096 });
    REQUIRE(fixture.isReady());
    REQUIRE(fixture.prepare());
    const auto competing = fixture.prepare(witness(52));
    REQUIRE(!competing);
    CHECK(competing.error() == AdoptTransactionError::Busy);
    CHECK_EQ(fixture.ledger->witnessCount(), 1u);
    REQUIRE(fixture.ledger->abort(*fixture.lease, selector, std::nullopt, fixture.driver));
    const auto secondWitness = witness(52);
    REQUIRE(fixture.prepare(secondWitness));
    REQUIRE(fixture.ledger->abort(*fixture.lease, secondWitness.selector, secondWitness.capsuleSHA256, fixture.driver));
    REQUIRE(fixture.ledger->advanceTime(10000));
    CHECK(!fixture.prepare(witness(53)));
    CHECK_EQ(fixture.ledger->witnessCount(), 2u);
    CHECK_EQ(fixture.ledger->outcomeCount(), 2u);
    const auto first = fixture.ledger->commit(*fixture.lease, selector, witness().capsuleSHA256, fixture.driver);
    REQUIRE(first);
    CHECK(first->outcome.disposition == AdoptDisposition::Aborted);
    CHECK_EQ(fixture.counts.activations, 0u);
}

TEST(reclaimLedgerRetainsWitnessWhenFreshAuthorityFailsAfterOutcomeReservation)
{
    Fixture fixture;
    REQUIRE(fixture.isReady());
    fixture.driver.shouldRevokeDuringPreparation = true;
    CHECK(!fixture.prepare());
    CHECK_EQ(fixture.ledger->outcomeCount(), 1u);
    CHECK_EQ(fixture.ledger->witnessCount(), 1u);
    CHECK_EQ(fixture.counts.stagedDestructions, 1u);
    fixture.driver.shouldRevokeDuringPreparation = false;
    fixture.driver.isPeerAuthorised = true;
    const auto retained = fixture.ledger->status(*fixture.lease, selector, witness().capsuleSHA256, fixture.driver);
    REQUIRE(retained);
    CHECK(retained->outcome.disposition == AdoptDisposition::Rejected);
    REQUIRE(retained->outcome.rejection);
    CHECK(*retained->outcome.rejection == AdoptRejection::NotOwner);
    REQUIRE(retained->capsuleSHA256);
    CHECK(*retained->capsuleSHA256 == witness().capsuleSHA256);
}

TEST(reclaimLedgerExpiredWitnessRejectsLateCommitWithoutEvictionOrActivation)
{
    Fixture fixture;
    REQUIRE(fixture.isReady());
    REQUIRE(fixture.prepare());
    REQUIRE(fixture.ledger->advanceTime(10));
    const auto expired = fixture.ledger->status(*fixture.lease, selector, std::nullopt, fixture.driver);
    const auto lateCommit = fixture.ledger->commit(*fixture.lease, selector, witness().capsuleSHA256, fixture.driver);
    REQUIRE(expired && lateCommit && expired->capsuleSHA256);
    CHECK(expired->outcome.disposition == AdoptDisposition::Expired);
    CHECK(lateCommit->outcome == expired->outcome);
    CHECK(*expired->capsuleSHA256 == witness().capsuleSHA256);
    CHECK_EQ(fixture.counts.stagedDestructions, 1u);
    CHECK_EQ(fixture.counts.activations, 0u);
    CHECK_EQ(fixture.ledger->witnessCount(), 1u);
}

TEST(reclaimLedgerConstrainsCandidateAndRechecksOwnerAfterCommitReservation)
{
    Fixture wrong;
    REQUIRE(wrong.isReady());
    wrong.driver.shouldReturnWrongTarget = true;
    const auto rejected = wrong.prepare();
    REQUIRE(rejected);
    CHECK(rejected->outcome.disposition == AdoptDisposition::Rejected);
    CHECK_EQ(wrong.counts.stagedDestructions, 1u);
    CHECK_EQ(wrong.counts.activations, 0u);
    Fixture fixture;
    REQUIRE(fixture.isReady());
    REQUIRE(fixture.prepare());
    fixture.driver.shouldRevokeDuringReservation = true;
    CHECK(!fixture.ledger->commit(*fixture.lease, selector, witness().capsuleSHA256, fixture.driver));
    CHECK_EQ(fixture.counts.activations, 0u);
    fixture.driver.shouldRevokeDuringReservation = false;
    fixture.driver.isPeerAuthorised = true;
    const auto prepared = fixture.ledger->status(*fixture.lease, selector, witness().capsuleSHA256, fixture.driver);
    REQUIRE(prepared);
    CHECK(prepared->outcome.disposition == AdoptDisposition::Prepared);
    fixture.driver.shouldRetargetDuringReservation = true;
    const auto invalidated = fixture.ledger->commit(*fixture.lease, selector, witness().capsuleSHA256, fixture.driver);
    REQUIRE(invalidated);
    CHECK(invalidated->outcome.disposition == AdoptDisposition::Rejected);
    CHECK_EQ(fixture.counts.activations, 0u);
}

TEST(reclaimLedgerRechecksOriginalProofAfterReservationAndBeforeDuplicateFastPath)
{
    Fixture fixture;
    REQUIRE(fixture.isReady());
    REQUIRE(fixture.prepare());
    fixture.driver.shouldExhaustOriginalProofDuringReservation = true;
    // After reply reservation, the reverse table's physical check succeeds;
    // the immediately following original-table proof recheck refuses. A
    // cached original Committed result would incorrectly allow activation.
    CHECK(!fixture.ledger->commit(*fixture.lease, selector, witness().capsuleSHA256, fixture.driver));
    CHECK_EQ(fixture.counts.activations, 0u);
    fixture.driver.shouldExhaustOriginalProofDuringReservation = false;
    fixture.driver.verificationsBeforeRefusal.reset();
    const auto prepared = fixture.ledger->status(*fixture.lease, selector, witness().capsuleSHA256, fixture.driver);
    REQUIRE(prepared);
    CHECK(prepared->outcome.disposition == AdoptDisposition::Prepared);
    fixture.driver.verificationsBeforeRefusal = 1;
    CHECK(!fixture.prepare());
    CHECK_EQ(fixture.counts.preparations, 1u);
    CHECK_EQ(fixture.ledger->witnessCount(), 1u);
}

TEST(reclaimLedgerRetiresDirectionLeasesIndependentlyWithoutStaleFirstRetry)
{
    Fixture fixture;
    REQUIRE(fixture.isReady());
    REQUIRE(fixture.prepare());
    CHECK(!fixture.ledger->retireAuthority(*fixture.lease, fixture.driver));
    fixture.driver.isPeerAuthorised = false;
    fixture.driver.isProcessGone = true;
    fixture.driver.retirement = AdoptRetirementEvidence::ProcessGoneAndAllChannelsAndOperationsRevokedAndDrained;
    // Original is terminal and can retire first. Its optional is cleared now,
    // even though the independent reverse table still owns a Prepared candidate.
    REQUIRE(fixture.originals->retireAuthority(*fixture.originalLease, fixture.driver));
    fixture.originalLease.reset();
    const auto held = fixture.ledger->retireAuthority(*fixture.lease, fixture.driver);
    REQUIRE(!held);
    CHECK(held.error() == AdoptTransactionError::ActiveCandidate);
    CHECK_EQ(fixture.ledger->witnessCount(), 1u);
    REQUIRE(fixture.ledger->advanceTime(10));
    REQUIRE(fixture.ledger->retireAuthority(*fixture.lease, fixture.driver));
    const auto oldLease = *fixture.lease;
    fixture.lease.reset();
    CHECK_EQ(fixture.ledger->outcomeCount(), 0u);
    CHECK_EQ(fixture.ledger->witnessCount(), 0u);
    CHECK_EQ(fixture.originals->outcomeCount(), 0u);
    CHECK(!fixture.ledger->status(oldLease, selector, witness().capsuleSHA256, fixture.driver));

    // A new physical birth reuses capacity, not either old table generation.
    fixture.driver.physicalAuthority = authority(8);
    fixture.originalDriver.physicalAuthority = authority(8);
    fixture.driver.isPeerAuthorised = true;
    fixture.driver.isProcessGone = false;
    const auto newOriginalLease = fixture.originals->admitAuthority(authority(8), fixture.originalDriver);
    REQUIRE(newOriginalLease);
    fixture.originalLease = *newOriginalLease;
    auto newWitness = witness(55);
    ++newWitness.selector.adoption.operationID.low;
    REQUIRE(fixture.originals->prepare(*newOriginalLease, { .operation = newWitness.selector.adoption, .bodyBytes = 100 }, fixture.originalDriver));
    REQUIRE(fixture.originals->commit(*newOriginalLease, newWitness.selector.adoption, fixture.originalDriver));
    const auto newLease = fixture.ledger->admitAuthority(authority(8), *newOriginalLease, fixture.driver);
    REQUIRE(newLease);
    fixture.lease = *newLease;
    fixture.driver.lease = *newLease;
    REQUIRE(fixture.prepare(newWitness));
    CHECK(!fixture.ledger->status(oldLease, selector, witness().capsuleSHA256, fixture.driver));
    REQUIRE(fixture.ledger->status(*newLease, newWitness.selector, newWitness.capsuleSHA256, fixture.driver));
}

TEST(reclaimLedgerPositiveRetirementEvidenceIsRequiredAndOriginalRemainsIndependent)
{
    Fixture fixture;
    REQUIRE(fixture.isReady());
    REQUIRE(fixture.prepare());
    REQUIRE(fixture.ledger->abort(*fixture.lease, selector, witness().capsuleSHA256, fixture.driver));
    for (const auto evidence : { AdoptRetirementEvidence::Unproven, AdoptRetirementEvidence::ProcessGoneOnly }) {
        fixture.driver.isPeerAuthorised = false;
        fixture.driver.isProcessGone = true;
        fixture.driver.retirement = evidence;
        const auto refused = fixture.ledger->retireAuthority(*fixture.lease, fixture.driver);
        REQUIRE(!refused);
        CHECK(refused.error() == AdoptTransactionError::RetirementUnproven);
        CHECK_EQ(fixture.ledger->witnessCount(), 1u);
    }
    fixture.driver.retirement = AdoptRetirementEvidence::ProcessGoneAndAllChannelsAndOperationsRevokedAndDrained;
    REQUIRE(fixture.ledger->retireAuthority(*fixture.lease, fixture.driver));
    fixture.lease.reset();
    CHECK_EQ(fixture.ledger->witnessCount(), 0u);
    CHECK_EQ(fixture.originals->outcomeCount(), 1u);
    REQUIRE(fixture.originals->retireAuthority(*fixture.originalLease, fixture.driver));
    fixture.originalLease.reset();
    CHECK_EQ(fixture.originals->outcomeCount(), 0u);
}

TEST(reclaimLedgerRejectsReentryAndAllocationRefusalWithoutMutatingOriginalOwner)
{
    Fixture fixture;
    REQUIRE(fixture.isReady());
    fixture.driver.shouldReenter = true;
    REQUIRE(fixture.prepare());
    CHECK_EQ(fixture.counts.reentrantRefusals, 1u);
    const auto originalCount = fixture.originals->outcomeCount();
    const auto originalActivations = fixture.originalCounts.activations;
    {
        UbiquittyTest::ScopedNothrowAllocationRefusal refuse;
        const auto refused = ReclaimTransaction::create(daemonInstance, *fixture.originals, limits);
        REQUIRE(!refused);
        CHECK(refused.error() == AdoptTransactionError::NoMemory);
    }
    CHECK_EQ(fixture.originals->outcomeCount(), originalCount);
    CHECK_EQ(fixture.originalCounts.activations, originalActivations);
    CHECK(!ReclaimTransaction::create(0, *fixture.originals, limits));
    CHECK(!ReclaimTransaction::create(daemonInstance, *fixture.originals, { {}, 0 }));
    auto excessive = limits;
    excessive.transactions.outcomeCapacity = std::numeric_limits<std::size_t>::max();
    CHECK(!ReclaimTransaction::create(daemonInstance, *fixture.originals, excessive));
}

UBIQUITTY_TEST_MAIN()
