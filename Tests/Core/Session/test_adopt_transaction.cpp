// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Session/AdoptTransaction.h"

#ifndef UBIQUITTY_TEST_WITHOUT_CORE
#define UBIQUITTY_TEST_WITHOUT_CORE
#endif
#include "Harness.h"

#include <array>
#include <limits>
#include <memory>
#include <optional>
#include <utility>

namespace {

using namespace Ubiquitty::Session;

struct Counts {
    int preparations = 0;
    int stagedDestructions = 0;
    int activeDestructions = 0;
    int replyReservations = 0;
    int replyDestructions = 0;
    int activations = 0;
    int verifications = 0;
    int reentrantRefusals = 0;
};

AdoptAuthority owner(std::uint64_t birth = 7)
{
    AdoptAuthority result;
    result.processID = 42;
    result.birth.low = birth;
    result.executableSHA256[0] = 8;
    return result;
}

AdoptPreparation request(std::uint64_t number = 1)
{
    AdoptPreparation result;
    result.operation.operationID = { .low = number, .high = 3 };
    result.operation.daemonInstanceID = 99;
    result.bodyBytes = 100;
    result.bodySHA256[0] = 9;
    return result;
}

class Candidate final : public AdoptCandidate {
public:
    Candidate(Counts& counts, Candidate*& observed)
        : m_counts(counts)
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
    AdoptTarget identity { .sessionID = 11, .paneID = { .low = 12, .high = 99 } };
    bool isActive = false;

private:
    Counts& m_counts;
    // The fixture driver outlives both table-staged and driver-active owners.
    // Destruction clears this borrow before it can outlive the candidate.
    Candidate*& m_observed;
};

class ReplyReservation final : public AdoptCommitReservation {
public:
    ReplyReservation(Counts& counts, AdoptOutcome outcome)
        : outcome(outcome)
        , m_counts(counts)
    {
    }
    ~ReplyReservation() override { ++m_counts.replyDestructions; }
    const AdoptOutcome outcome;

private:
    Counts& m_counts;
};

class Driver final : public AdoptTransactionDriver {
public:
    explicit Driver(Counts& counts)
        : counts(counts)
    {
    }

    std::expected<void, AdoptTransactionError> verifyAuthority(const AdoptAuthority& proposed) override
    {
        ++counts.verifications;
        if (!isPeerLive || proposed != liveOwner)
            return std::unexpected(AdoptTransactionError::Unauthorised);
        return {};
    }
    std::expected<std::unique_ptr<AdoptCandidate>, AdoptRejection> prepareCandidate(const AdoptPreparation& preparation) override
    {
        ++counts.preparations;
        CHECK(table && table->outcomeCount() > 0);
        if (shouldReenter)
            checkReentry(preparation.operation);
        if (preparationRejection)
            return std::unexpected(*preparationRejection);
        if (shouldReturnNullCandidate)
            return std::unique_ptr<AdoptCandidate> {};
        auto candidate = std::make_unique<Candidate>(counts, observed);
        candidate->identity.sessionID = nextSessionID++;
        candidate->identity.paneID.low = candidate->identity.sessionID;
        if (shouldReturnInvalidTarget)
            candidate->identity = {};
        if (shouldRevokeDuringPreparation)
            isPeerLive = false;
        return std::unique_ptr<AdoptCandidate>(std::move(candidate));
    }
    std::expected<std::unique_ptr<AdoptCommitReservation>, AdoptTransactionError> reserveCommittedReply(const AdoptOutcome& outcome, const AdoptCandidate& candidate) override
    {
        CHECK(&candidate == observed);
        CHECK(candidate.target() == outcome.target);
        ++counts.replyReservations;
        if (shouldReenter)
            checkReentry(outcome.operation);
        if (shouldRefuseEndpoint)
            return std::unexpected(AdoptTransactionError::Unauthorised);
        if (shouldRefuseReply)
            return std::unexpected(AdoptTransactionError::NoMemory);
        if (shouldRevokeDuringReservation)
            isPeerLive = false;
        if (shouldRetargetDuringReservation && observed)
            ++observed->identity.sessionID;
        return std::unique_ptr<AdoptCommitReservation>(std::make_unique<ReplyReservation>(counts, outcome));
    }
    void activate(std::unique_ptr<AdoptCandidate> candidate, std::unique_ptr<AdoptCommitReservation> reservation) noexcept override
    {
        auto* const staged = static_cast<Candidate*>(candidate.get());
        auto* const reply = static_cast<ReplyReservation*>(reservation.get());
        CHECK(staged && reply);
        if (!staged || !reply)
            return;
        CHECK(reply->outcome.disposition == AdoptDisposition::Committed);
        CHECK(reply->outcome.target == staged->target());
        if (shouldReenter)
            checkReentry(reply->outcome.operation);
        staged->isActive = true;
        ++counts.activations;
        // This fake host, not the transaction table, owns the published object.
        for (auto& slot : active) {
            if (!slot) {
                slot = std::move(candidate);
                return;
            }
        }
        CHECK_MSG(false, "fixture active-owner storage must cover every activation");
    }
    AdoptRetirementEvidence retirementEvidence(const AdoptAuthority& proposed) override
    {
        if (isPeerLive || proposed != liveOwner)
            return AdoptRetirementEvidence::Unproven;
        return retirement;
    }

    Counts& counts;
    AdoptAuthority liveOwner = owner();
    AdoptTransaction* table = nullptr;
    AdoptAuthorityLease lease;
    Candidate* observed = nullptr;
    std::array<std::unique_ptr<AdoptCandidate>, 4> active;
    std::uint32_t nextSessionID = 11;
    std::optional<AdoptRejection> preparationRejection;
    AdoptRetirementEvidence retirement = AdoptRetirementEvidence::Unproven;
    bool isPeerLive = true;
    bool shouldRefuseEndpoint = false;
    bool shouldRefuseReply = false;
    bool shouldReturnNullCandidate = false;
    bool shouldReturnInvalidTarget = false;
    bool shouldRevokeDuringPreparation = false;
    bool shouldRevokeDuringReservation = false;
    bool shouldRetargetDuringReservation = false;
    bool shouldReenter = false;

private:
    void checkReentry(AdoptOperation operation)
    {
        const auto reentry = table->abort(lease, operation, *this);
        CHECK(!reentry);
        if (!reentry) {
            CHECK(reentry.error() == AdoptTransactionError::Reentrant);
            counts.reentrantRefusals += reentry.error() == AdoptTransactionError::Reentrant;
        }
    }
};

struct Fixture {
    Counts counts;
    Driver driver { counts };
    std::unique_ptr<AdoptTransaction> table;
    std::optional<AdoptAuthorityLease> lease;

    Fixture()
    {
        // All bounds and clock units here are fixture policy, not product tuning.
        auto created = AdoptTransaction::create(99, { .authorityCapacity = 2, .outcomeCapacity = 3, .preparedLeaseMicroseconds = 10 });
        if (!created)
            return;
        table = std::move(*created);
        driver.table = table.get();
        auto admitted = table->admitAuthority(driver.liveOwner, driver);
        if (admitted) {
            lease = *admitted;
            driver.lease = *admitted;
        }
    }
    bool isReady() const { return table && lease.has_value(); }
};

} // namespace

TEST(adoptPrepareOwnsAnInactiveCandidateAndReservesItsOutcomeFirst)
{
    Fixture fixture;
    REQUIRE(fixture.isReady());
    const auto prepared = fixture.table->prepare(*fixture.lease, request(), fixture.driver);
    REQUIRE(prepared);
    CHECK(prepared->disposition == AdoptDisposition::Prepared);
    CHECK_EQ(fixture.table->outcomeCount(), 1u);
    CHECK_EQ(fixture.counts.preparations, 1);
    CHECK_EQ(fixture.counts.activations, 0);
    CHECK_EQ(fixture.counts.replyReservations, 0);
    CHECK_EQ(fixture.counts.stagedDestructions, 0);
    REQUIRE(fixture.driver.observed);
    CHECK(!fixture.driver.observed->isActive);
    const auto duplicate = fixture.table->prepare(*fixture.lease, request(), fixture.driver);
    REQUIRE(duplicate);
    CHECK(*duplicate == *prepared);
    CHECK_EQ(fixture.counts.preparations, 1);
    const auto competing = fixture.table->prepare(*fixture.lease, request(2), fixture.driver);
    REQUIRE(!competing);
    CHECK(competing.error() == AdoptTransactionError::Busy);
}

TEST(adoptCommitActivatesExactlyOnceAndRetainsItsLostReplyOutcome)
{
    Fixture fixture;
    REQUIRE(fixture.isReady());
    auto prepared = fixture.table->prepare(*fixture.lease, request(), fixture.driver);
    REQUIRE(prepared);
    AdoptClientAttempt client(request().operation);
    REQUIRE(client.observe(*prepared));
    REQUIRE(client.beginCommit());
    CHECK(!client.mayResumeLocal());
    // The host commits but the sender does not receive this first result.
    const auto committed = fixture.table->commit(*fixture.lease, request().operation, fixture.driver);
    REQUIRE(committed);
    CHECK(committed->disposition == AdoptDisposition::Committed);
    CHECK(!client.mayResumeLocal());
    const auto duplicate = fixture.table->commit(*fixture.lease, request().operation, fixture.driver);
    const auto status = fixture.table->status(*fixture.lease, request().operation, fixture.driver);
    const auto tooLateAbort = fixture.table->abort(*fixture.lease, request().operation, fixture.driver);
    REQUIRE(duplicate && status && tooLateAbort);
    CHECK(*duplicate == *committed);
    CHECK(*status == *committed);
    CHECK(*tooLateAbort == *committed);
    CHECK_EQ(fixture.counts.activations, 1);
    CHECK_EQ(fixture.counts.replyReservations, 1);
    CHECK_EQ(fixture.counts.activeDestructions, 0);
    REQUIRE(client.observe(*status));
    CHECK(client.isCommitted());
    CHECK(!client.mayResumeLocal());
}

TEST(adoptAbortWinsBeforeLateCommitAndDisposesOnlyTheStagedOwner)
{
    Fixture fixture;
    REQUIRE(fixture.isReady());
    const auto prepared = fixture.table->prepare(*fixture.lease, request(), fixture.driver);
    REQUIRE(prepared);
    AdoptClientAttempt client(request().operation);
    REQUIRE(client.observe(*prepared));
    REQUIRE(client.beginCommit());
    const auto aborted = fixture.table->abort(*fixture.lease, request().operation, fixture.driver);
    REQUIRE(aborted);
    CHECK(aborted->disposition == AdoptDisposition::Aborted);
    const auto late = fixture.table->commit(*fixture.lease, request().operation, fixture.driver);
    const auto repeated = fixture.table->prepare(*fixture.lease, request(), fixture.driver);
    REQUIRE(late && repeated);
    CHECK(*late == *aborted);
    CHECK(*repeated == *aborted);
    CHECK_EQ(fixture.counts.stagedDestructions, 1);
    CHECK_EQ(fixture.counts.activeDestructions, 0);
    CHECK_EQ(fixture.counts.activations, 0);
    CHECK_EQ(fixture.counts.preparations, 1);
    REQUIRE(client.observe(*aborted));
    CHECK(client.mayResumeLocal());
    CHECK(!client.beginCommit());
}

TEST(adoptPreparedExpiryIsTerminalAndNeverMakesRoomForReprepare)
{
    Fixture fixture;
    REQUIRE(fixture.isReady());
    REQUIRE(fixture.table->prepare(*fixture.lease, request(), fixture.driver));
    REQUIRE(fixture.table->advanceTime(9));
    auto before = fixture.table->status(*fixture.lease, request().operation, fixture.driver);
    REQUIRE(before);
    CHECK(before->disposition == AdoptDisposition::Prepared);
    REQUIRE(fixture.table->advanceTime(10));
    const auto expired = fixture.table->status(*fixture.lease, request().operation, fixture.driver);
    REQUIRE(expired);
    CHECK(expired->disposition == AdoptDisposition::Expired);
    REQUIRE(fixture.table->advanceTime(std::numeric_limits<std::uint64_t>::max()));
    const auto late = fixture.table->commit(*fixture.lease, request().operation, fixture.driver);
    const auto duplicate = fixture.table->prepare(*fixture.lease, request(), fixture.driver);
    REQUIRE(late && duplicate);
    CHECK(*late == *expired);
    CHECK(*duplicate == *expired);
    CHECK_EQ(fixture.counts.preparations, 1);
    CHECK_EQ(fixture.counts.stagedDestructions, 1);
    CHECK_EQ(fixture.table->outcomeCount(), 1u);
}

TEST(adoptCapacityRefusesBeforeConstructingOrEvictingAnyCandidate)
{
    Fixture fixture;
    REQUIRE(fixture.isReady());
    for (std::uint64_t number = 1; number <= 3; ++number) {
        REQUIRE(fixture.table->prepare(*fixture.lease, request(number), fixture.driver));
        REQUIRE(fixture.table->abort(*fixture.lease, request(number).operation, fixture.driver));
    }
    const auto full = fixture.table->prepare(*fixture.lease, request(4), fixture.driver);
    REQUIRE(!full);
    CHECK(full.error() == AdoptTransactionError::Busy);
    CHECK_EQ(fixture.counts.preparations, 3);
    CHECK_EQ(fixture.counts.stagedDestructions, 3);
    CHECK_EQ(fixture.table->outcomeCount(), 3u);
    const auto first = fixture.table->status(*fixture.lease, request().operation, fixture.driver);
    REQUIRE(first);
    CHECK(first->disposition == AdoptDisposition::Aborted);
}

TEST(adoptSameOperationCannotChangeBodyOwnerOrServingDaemon)
{
    Fixture fixture;
    REQUIRE(fixture.isReady());
    REQUIRE(fixture.table->prepare(*fixture.lease, request(), fixture.driver));
    auto changed = request();
    ++changed.bodyBytes;
    auto conflict = fixture.table->prepare(*fixture.lease, changed, fixture.driver);
    REQUIRE(!conflict);
    CHECK(conflict.error() == AdoptTransactionError::Conflict);
    changed = request();
    ++changed.bodySHA256[0];
    conflict = fixture.table->prepare(*fixture.lease, changed, fixture.driver);
    REQUIRE(!conflict);
    CHECK(conflict.error() == AdoptTransactionError::Conflict);
    Driver other(fixture.counts);
    other.liveOwner = owner(8);
    other.table = fixture.table.get();
    const auto otherLease = fixture.table->admitAuthority(other.liveOwner, other);
    REQUIRE(otherLease);
    conflict = fixture.table->prepare(*otherLease, request(), other);
    REQUIRE(!conflict);
    CHECK(conflict.error() == AdoptTransactionError::Conflict);
    changed = request();
    ++changed.operation.daemonInstanceID;
    conflict = fixture.table->prepare(*fixture.lease, changed, fixture.driver);
    REQUIRE(!conflict);
    CHECK(conflict.error() == AdoptTransactionError::InvalidIdentity);
    CHECK_EQ(fixture.counts.preparations, 1);
}

TEST(adoptPreparationRefusalRetainsARealRejectedOutcomeWithoutActivation)
{
    Fixture fixture;
    REQUIRE(fixture.isReady());
    fixture.driver.preparationRejection = AdoptRejection::NotOwner;
    const auto rejected = fixture.table->prepare(*fixture.lease, request(), fixture.driver);
    REQUIRE(rejected);
    CHECK(rejected->disposition == AdoptDisposition::Rejected);
    CHECK(rejected->rejection == AdoptRejection::NotOwner);
    const auto later = fixture.table->commit(*fixture.lease, request().operation, fixture.driver);
    const auto repeated = fixture.table->prepare(*fixture.lease, request(), fixture.driver);
    REQUIRE(later && repeated);
    CHECK(*later == *rejected);
    CHECK(*repeated == *rejected);
    CHECK_EQ(fixture.counts.preparations, 1);
    CHECK_EQ(fixture.counts.activations, 0);
    CHECK_EQ(fixture.counts.replyReservations, 0);
    fixture.driver.preparationRejection = AdoptRejection::NoMemory;
    const auto noMemory = fixture.table->prepare(*fixture.lease, request(2), fixture.driver);
    REQUIRE(noMemory);
    CHECK(noMemory->disposition == AdoptDisposition::Rejected);
    CHECK(noMemory->rejection == AdoptRejection::NoMemory);
    CHECK_EQ(fixture.counts.activations, 0);
    // A full host is its own reason, not folded into a bad candidate.
    fixture.driver.preparationRejection = AdoptRejection::HostFull;
    const auto full = fixture.table->prepare(*fixture.lease, request(3), fixture.driver);
    REQUIRE(full);
    CHECK(full->disposition == AdoptDisposition::Rejected);
    CHECK(full->rejection == AdoptRejection::HostFull);
    CHECK_EQ(fixture.counts.activations, 0);
}

TEST(adoptReplyReservationFailureLeavesTheOriginalPreparedCandidateInactive)
{
    Fixture fixture;
    REQUIRE(fixture.isReady());
    const auto prepared = fixture.table->prepare(*fixture.lease, request(), fixture.driver);
    REQUIRE(prepared);
    auto* const original = fixture.driver.observed;
    fixture.driver.shouldRefuseReply = true;
    const auto refused = fixture.table->commit(*fixture.lease, request().operation, fixture.driver);
    REQUIRE(!refused);
    CHECK(refused.error() == AdoptTransactionError::ReplyUnavailable);
    CHECK(fixture.driver.observed == original);
    CHECK_EQ(fixture.counts.activations, 0);
    CHECK_EQ(fixture.counts.stagedDestructions, 0);
    const auto status = fixture.table->status(*fixture.lease, request().operation, fixture.driver);
    REQUIRE(status);
    CHECK(*status == *prepared);
}

TEST(adoptCommitEndpointRefusalKeepsTheSamePreparedCandidateUntilExactAbort)
{
    Fixture fixture;
    REQUIRE(fixture.isReady());
    const auto prepared = fixture.table->prepare(*fixture.lease, request(), fixture.driver);
    REQUIRE(prepared);
    auto* const original = fixture.driver.observed;
    REQUIRE(original);
    AdoptClientAttempt client(request().operation);
    REQUIRE(client.observe(*prepared));
    REQUIRE(client.beginCommit());

    // The fake platform refuses the exact borrowed candidate before reserving
    // output. This tests table ownership policy, not a kernel endpoint check.
    fixture.driver.shouldRefuseEndpoint = true;
    const auto refused = fixture.table->commit(*fixture.lease, request().operation, fixture.driver);
    REQUIRE(!refused);
    CHECK(refused.error() == AdoptTransactionError::ReplyUnavailable);
    CHECK(fixture.driver.observed == original);
    CHECK_EQ(fixture.counts.replyReservations, 1);
    CHECK_EQ(fixture.counts.replyDestructions, 0);
    CHECK_EQ(fixture.counts.stagedDestructions, 0);
    CHECK_EQ(fixture.counts.activations, 0);
    CHECK_EQ(fixture.table->outcomeCount(), 1u);
    CHECK(!client.mayResumeLocal());

    const auto retained = fixture.table->status(*fixture.lease, request().operation, fixture.driver);
    REQUIRE(retained);
    CHECK(*retained == *prepared);
    CHECK(fixture.driver.observed == original);
    REQUIRE(client.observe(*retained));
    CHECK(!client.isCommitted());
    CHECK(!client.mayResumeLocal());

    const auto aborted = fixture.table->abort(*fixture.lease, request().operation, fixture.driver);
    REQUIRE(aborted);
    CHECK(aborted->disposition == AdoptDisposition::Aborted);
    CHECK(aborted->target == prepared->target);
    REQUIRE(client.observe(*aborted));
    CHECK(client.mayResumeLocal());
    CHECK(!client.isCommitted());
    CHECK(fixture.driver.observed == nullptr);
    CHECK_EQ(fixture.counts.stagedDestructions, 1);
    CHECK_EQ(fixture.counts.activeDestructions, 0);
    CHECK_EQ(fixture.counts.activations, 0);
}

TEST(adoptCommitRevalidatesAuthorityAfterReplyReservation)
{
    Fixture fixture;
    REQUIRE(fixture.isReady());
    REQUIRE(fixture.table->prepare(*fixture.lease, request(), fixture.driver));
    fixture.driver.shouldRevokeDuringReservation = true;
    const auto refused = fixture.table->commit(*fixture.lease, request().operation, fixture.driver);
    REQUIRE(!refused);
    CHECK(refused.error() == AdoptTransactionError::Unauthorised);
    CHECK_EQ(fixture.counts.replyReservations, 1);
    CHECK_EQ(fixture.counts.replyDestructions, 1);
    CHECK_EQ(fixture.counts.activations, 0);
    CHECK_EQ(fixture.counts.stagedDestructions, 0);
    fixture.driver.isPeerLive = true;
    const auto status = fixture.table->status(*fixture.lease, request().operation, fixture.driver);
    REQUIRE(status);
    CHECK(status->disposition == AdoptDisposition::Prepared);
}

TEST(adoptCommitRejectsCandidateRetargetingEvenInsideReplyReservation)
{
    Fixture fixture;
    REQUIRE(fixture.isReady());
    const auto prepared = fixture.table->prepare(*fixture.lease, request(), fixture.driver);
    REQUIRE(prepared);
    fixture.driver.shouldRetargetDuringReservation = true;
    const auto refused = fixture.table->commit(*fixture.lease, request().operation, fixture.driver);
    REQUIRE(refused);
    CHECK(refused->disposition == AdoptDisposition::Rejected);
    CHECK(refused->target == prepared->target);
    CHECK(refused->rejection == AdoptRejection::InvalidCandidate);
    CHECK_EQ(fixture.counts.activations, 0);
    CHECK_EQ(fixture.counts.stagedDestructions, 1);
    CHECK_EQ(fixture.counts.replyDestructions, 1);
}

TEST(adoptCallbacksCannotReenterTheSerialTransactionOwner)
{
    Fixture fixture;
    REQUIRE(fixture.isReady());
    fixture.driver.shouldReenter = true;
    REQUIRE(fixture.table->prepare(*fixture.lease, request(), fixture.driver));
    const auto committed = fixture.table->commit(*fixture.lease, request().operation, fixture.driver);
    REQUIRE(committed);
    CHECK(committed->disposition == AdoptDisposition::Committed);
    CHECK_EQ(fixture.counts.reentrantRefusals, 3);
    CHECK_EQ(fixture.counts.activations, 1);
}

TEST(adoptAuthorityRetirementRequiresCompleteProofAndNoLiveCandidate)
{
    Fixture fixture;
    REQUIRE(fixture.isReady());
    REQUIRE(fixture.table->prepare(*fixture.lease, request(), fixture.driver));
    auto retired = fixture.table->retireAuthority(*fixture.lease, fixture.driver);
    REQUIRE(!retired);
    CHECK(retired.error() == AdoptTransactionError::ActiveCandidate);
    REQUIRE(fixture.table->commit(*fixture.lease, request().operation, fixture.driver));
    retired = fixture.table->retireAuthority(*fixture.lease, fixture.driver);
    REQUIRE(!retired);
    CHECK(retired.error() == AdoptTransactionError::RetirementUnproven);
    fixture.driver.isPeerLive = false;
    fixture.driver.retirement = AdoptRetirementEvidence::ProcessGoneOnly;
    retired = fixture.table->retireAuthority(*fixture.lease, fixture.driver);
    REQUIRE(!retired);
    CHECK(retired.error() == AdoptTransactionError::RetirementUnproven);
    fixture.driver.retirement = AdoptRetirementEvidence::ProcessGoneAndAllChannelsAndOperationsRevokedAndDrained;
    REQUIRE(fixture.table->retireAuthority(*fixture.lease, fixture.driver));
    CHECK_EQ(fixture.table->outcomeCount(), 0u);
    CHECK_EQ(fixture.counts.activations, 1);
    CHECK_EQ(fixture.counts.activeDestructions, 0);
    REQUIRE(fixture.driver.active[0]);
}

TEST(adoptRetiredSlotsReuseFreshBirthsButNeverTheirStaleLeasesOrProofs)
{
    Fixture fixture;
    REQUIRE(fixture.isReady());
    const auto stale = *fixture.lease;
    const auto oldOwner = fixture.driver.liveOwner;
    REQUIRE(fixture.table->prepare(stale, request(), fixture.driver));
    REQUIRE(fixture.table->abort(stale, request().operation, fixture.driver));
    fixture.driver.isPeerLive = false;
    fixture.driver.retirement = AdoptRetirementEvidence::ProcessGoneAndAllChannelsAndOperationsRevokedAndDrained;
    REQUIRE(fixture.table->retireAuthority(stale, fixture.driver));
    const auto deadAdmission = fixture.table->admitAuthority(oldOwner, fixture.driver);
    REQUIRE(!deadAdmission);
    CHECK(deadAdmission.error() == AdoptTransactionError::Unauthorised);
    fixture.driver.liveOwner = owner(8);
    fixture.driver.isPeerLive = true;
    const auto fresh = fixture.table->admitAuthority(fixture.driver.liveOwner, fixture.driver);
    REQUIRE(fresh);
    CHECK(*fresh != stale);
    const int verifications = fixture.counts.verifications;
    const auto staleStatus = fixture.table->status(stale, request().operation, fixture.driver);
    const auto stalePrepare = fixture.table->prepare(stale, request(), fixture.driver);
    const auto staleRetire = fixture.table->retireAuthority(stale, fixture.driver);
    REQUIRE(!staleStatus && !stalePrepare && !staleRetire);
    CHECK(staleStatus.error() == AdoptTransactionError::StaleAuthority);
    CHECK(stalePrepare.error() == AdoptTransactionError::StaleAuthority);
    CHECK(staleRetire.error() == AdoptTransactionError::StaleAuthority);
    CHECK_EQ(fixture.counts.verifications, verifications);
    const auto oldFactsWithNewLease = fixture.table->admitAuthority(oldOwner, fixture.driver);
    REQUIRE(!oldFactsWithNewLease);
    CHECK(oldFactsWithNewLease.error() == AdoptTransactionError::Unauthorised);
    REQUIRE(fixture.table->prepare(*fresh, request(2), fixture.driver));
    CHECK_EQ(fixture.table->outcomeCount(), 1u);
}

TEST(adoptUnknownAndPreparedAfterCommitNeverAuthoriseLocalResume)
{
    Fixture fixture;
    REQUIRE(fixture.isReady());
    const auto prepared = fixture.table->prepare(*fixture.lease, request(), fixture.driver);
    REQUIRE(prepared);
    AdoptClientAttempt client(request().operation);
    REQUIRE(client.observe(*prepared));
    CHECK(client.mayResumeLocal());
    REQUIRE(client.beginCommit());
    REQUIRE(client.observe(*prepared));
    CHECK(!client.mayResumeLocal());
    AdoptOutcome missing;
    missing.operation = request().operation;
    REQUIRE(client.observe(missing));
    CHECK(!client.mayResumeLocal());
    auto wrong = *prepared;
    ++wrong.operation.daemonInstanceID;
    CHECK(!client.observe(wrong));
    wrong = *prepared;
    ++wrong.target.sessionID;
    CHECK(!client.observe(wrong));
    CHECK(!client.mayResumeLocal());
    const auto aborted = fixture.table->abort(*fixture.lease, request().operation, fixture.driver);
    REQUIRE(aborted);
    REQUIRE(client.observe(*aborted));
    CHECK(client.mayResumeLocal());
    CHECK(!client.observe(*prepared));
}

TEST(adoptInvalidPolicyAndClockCannotAllocateOrWrapAPreparedLease)
{
    CHECK(!AdoptTransaction::create(99, {}));
    CHECK(!AdoptTransaction::create(0, { .authorityCapacity = 1, .outcomeCapacity = 1, .preparedLeaseMicroseconds = 10 }));
    CHECK(!AdoptTransaction::create(99, { .authorityCapacity = std::numeric_limits<std::size_t>::max(), .outcomeCapacity = 1, .preparedLeaseMicroseconds = 10 }));
    Fixture fixture;
    REQUIRE(fixture.isReady());
    REQUIRE(fixture.table->advanceTime(std::numeric_limits<std::uint64_t>::max() - 5));
    const auto overflow = fixture.table->prepare(*fixture.lease, request(), fixture.driver);
    REQUIRE(!overflow);
    CHECK(overflow.error() == AdoptTransactionError::InvalidTime);
    CHECK_EQ(fixture.counts.preparations, 0);
    CHECK_EQ(fixture.table->outcomeCount(), 0u);
    const auto backward = fixture.table->advanceTime(1);
    REQUIRE(!backward);
    CHECK(backward.error() == AdoptTransactionError::InvalidTime);
    const auto missing = fixture.table->status(*fixture.lease, request().operation, fixture.driver);
    REQUIRE(missing);
    CHECK(missing->disposition == AdoptDisposition::Unknown);
}

TEST(adoptInvalidCandidatesAndLostPreparationAuthorityNeverBecomePrepared)
{
    Fixture fixture;
    REQUIRE(fixture.isReady());
    fixture.driver.shouldReturnNullCandidate = true;
    auto rejected = fixture.table->prepare(*fixture.lease, request(), fixture.driver);
    REQUIRE(rejected);
    CHECK(rejected->disposition == AdoptDisposition::Rejected);
    CHECK(rejected->rejection == AdoptRejection::InvalidCandidate);
    fixture.driver.shouldReturnNullCandidate = false;
    fixture.driver.shouldReturnInvalidTarget = true;
    rejected = fixture.table->prepare(*fixture.lease, request(2), fixture.driver);
    REQUIRE(rejected);
    CHECK(rejected->disposition == AdoptDisposition::Rejected);
    CHECK(rejected->rejection == AdoptRejection::InvalidCandidate);
    CHECK_EQ(fixture.counts.stagedDestructions, 1);
    fixture.driver.shouldReturnInvalidTarget = false;
    fixture.driver.shouldRevokeDuringPreparation = true;
    const auto lost = fixture.table->prepare(*fixture.lease, request(3), fixture.driver);
    REQUIRE(!lost);
    CHECK(lost.error() == AdoptTransactionError::Unauthorised);
    CHECK_EQ(fixture.counts.stagedDestructions, 2);
    CHECK_EQ(fixture.counts.activations, 0);
    fixture.driver.isPeerLive = true;
    const auto retained = fixture.table->status(*fixture.lease, request(3).operation, fixture.driver);
    REQUIRE(retained);
    CHECK(retained->disposition == AdoptDisposition::Rejected);
    CHECK(retained->rejection == AdoptRejection::NotOwner);
}

TEST(adoptEachPhaseRequiresFreshExactLiveAuthorityRatherThanACachedLease)
{
    Fixture fixture;
    REQUIRE(fixture.isReady());
    REQUIRE(fixture.table->prepare(*fixture.lease, request(), fixture.driver));
    const auto original = fixture.driver.liveOwner;
    ++fixture.driver.liveOwner.executableSHA256[0];
    const auto changedBinary = fixture.table->admitAuthority(fixture.driver.liveOwner, fixture.driver);
    REQUIRE(!changedBinary);
    CHECK(changedBinary.error() == AdoptTransactionError::Conflict);
    const auto status = fixture.table->status(*fixture.lease, request().operation, fixture.driver);
    const auto commit = fixture.table->commit(*fixture.lease, request().operation, fixture.driver);
    const auto abort = fixture.table->abort(*fixture.lease, request().operation, fixture.driver);
    const auto prepare = fixture.table->prepare(*fixture.lease, request(2), fixture.driver);
    REQUIRE(!status && !commit && !abort && !prepare);
    CHECK(status.error() == AdoptTransactionError::Unauthorised);
    CHECK(commit.error() == AdoptTransactionError::Unauthorised);
    CHECK(abort.error() == AdoptTransactionError::Unauthorised);
    CHECK(prepare.error() == AdoptTransactionError::Unauthorised);
    CHECK_EQ(fixture.counts.activations, 0);
    CHECK_EQ(fixture.counts.preparations, 1);
    fixture.driver.liveOwner = original;
    const auto stillPrepared = fixture.table->status(*fixture.lease, request().operation, fixture.driver);
    REQUIRE(stillPrepared);
    CHECK(stillPrepared->disposition == AdoptDisposition::Prepared);
}

TEST(adoptProvenRetirementReusesBoundedStorageAcrossFreshProcessBirths)
{
    Fixture fixture;
    REQUIRE(fixture.isReady());
    auto lease = *fixture.lease;
    for (std::uint64_t birth = 7; birth < 17; ++birth) {
        REQUIRE(fixture.table->prepare(lease, request(birth), fixture.driver));
        REQUIRE(fixture.table->abort(lease, request(birth).operation, fixture.driver));
        fixture.driver.isPeerLive = false;
        fixture.driver.retirement = AdoptRetirementEvidence::ProcessGoneAndAllChannelsAndOperationsRevokedAndDrained;
        REQUIRE(fixture.table->retireAuthority(lease, fixture.driver));
        CHECK_EQ(fixture.table->outcomeCount(), 0u);
        fixture.driver.liveOwner = owner(birth + 1);
        fixture.driver.isPeerLive = true;
        const auto admitted = fixture.table->admitAuthority(fixture.driver.liveOwner, fixture.driver);
        REQUIRE(admitted);
        CHECK(*admitted != lease);
        lease = *admitted;
    }
    CHECK_EQ(fixture.counts.preparations, 10);
    CHECK_EQ(fixture.counts.stagedDestructions, 10);
    CHECK_EQ(fixture.counts.activations, 0);
}

TEST(adoptMalformedIdentitiesCannotReserveOrConstructAnOutcome)
{
    Fixture fixture;
    REQUIRE(fixture.isReady());
    auto malformed = request();
    malformed.operation.operationID = {};
    auto refused = fixture.table->prepare(*fixture.lease, malformed, fixture.driver);
    REQUIRE(!refused);
    CHECK(refused.error() == AdoptTransactionError::InvalidIdentity);
    malformed = request();
    malformed.bodyBytes = 0;
    refused = fixture.table->prepare(*fixture.lease, malformed, fixture.driver);
    REQUIRE(!refused);
    CHECK(refused.error() == AdoptTransactionError::InvalidIdentity);
    auto absent = owner();
    absent.processID = Ubiquitty::PAL::noProcess;
    const auto admission = fixture.table->admitAuthority(absent, fixture.driver);
    REQUIRE(!admission);
    CHECK(admission.error() == AdoptTransactionError::InvalidIdentity);
    CHECK_EQ(fixture.counts.preparations, 0);
    CHECK_EQ(fixture.table->outcomeCount(), 0u);
}

TEST(adoptClientRejectsMalformedDispositionAndRejectionCombinations)
{
    Fixture fixture;
    REQUIRE(fixture.isReady());
    const auto prepared = fixture.table->prepare(*fixture.lease, request(), fixture.driver);
    REQUIRE(prepared);

    for (const auto disposition : { AdoptDisposition::Unknown, AdoptDisposition::Prepared,
             AdoptDisposition::Committed, AdoptDisposition::Aborted, AdoptDisposition::Expired }) {
        AdoptClientAttempt client(request().operation);
        REQUIRE(client.observe(*prepared));
        REQUIRE(client.beginCommit());
        auto malformed = *prepared;
        malformed.disposition = disposition;
        malformed.rejection = AdoptRejection::NotOwner;
        const auto refused = client.observe(malformed);
        REQUIRE(!refused);
        CHECK(refused.error() == AdoptTransactionError::Conflict);
        CHECK(!client.mayResumeLocal());
        CHECK(!client.isCommitted());
        REQUIRE(client.observe(*prepared));
        CHECK(!client.mayResumeLocal());
    }

    std::array<AdoptOutcome, 3> malformed { *prepared, *prepared, *prepared };
    malformed[0].disposition = static_cast<AdoptDisposition>(0xff);
    malformed[1].disposition = AdoptDisposition::Rejected;
    malformed[1].rejection.reset();
    malformed[2].disposition = AdoptDisposition::Rejected;
    malformed[2].rejection = static_cast<AdoptRejection>(0xff);
    for (const auto& outcome : malformed) {
        AdoptClientAttempt client(request().operation);
        REQUIRE(client.observe(*prepared));
        REQUIRE(client.beginCommit());
        const auto refused = client.observe(outcome);
        REQUIRE(!refused);
        CHECK(refused.error() == AdoptTransactionError::Conflict);
        CHECK(!client.mayResumeLocal());
        CHECK(!client.isCommitted());
        REQUIRE(client.observe(*prepared));
        CHECK(!client.mayResumeLocal());
    }
}

TEST(adoptClientRejectsEmptyAndPartialTargetsWithoutRelaxingCommitUncertainty)
{
    Fixture fixture;
    REQUIRE(fixture.isReady());
    const auto prepared = fixture.table->prepare(*fixture.lease, request(), fixture.driver);
    REQUIRE(prepared);
    std::array<AdoptTarget, 3> targets {};
    targets[1].sessionID = prepared->target.sessionID;
    targets[2].paneID = prepared->target.paneID;
    for (const auto disposition : { AdoptDisposition::Unknown, AdoptDisposition::Prepared,
             AdoptDisposition::Committed, AdoptDisposition::Aborted, AdoptDisposition::Expired, AdoptDisposition::Rejected }) {
        for (const auto& target : targets) {
            AdoptClientAttempt client(request().operation);
            REQUIRE(client.observe(*prepared));
            REQUIRE(client.beginCommit());
            auto outcome = *prepared;
            outcome.disposition = disposition;
            outcome.target = target;
            if (disposition == AdoptDisposition::Rejected)
                outcome.rejection = AdoptRejection::InvalidCandidate;
            const auto observed = client.observe(outcome);
            if (disposition == AdoptDisposition::Unknown && target == AdoptTarget {}) {
                CHECK_MSG(observed.has_value(), "a well-shaped missing result is accepted, never a rollback");
            } else {
                REQUIRE(!observed);
                CHECK(observed.error() == AdoptTransactionError::Conflict);
            }
            CHECK(!client.mayResumeLocal());
            CHECK(!client.isCommitted());
            REQUIRE(client.observe(*prepared));
            CHECK(!client.mayResumeLocal());
        }
    }
}

TEST(adoptClientRefusesUnsolicitedCommittedUntilItsOwnCommitAttempt)
{
    Fixture fixture;
    REQUIRE(fixture.isReady());
    const auto prepared = fixture.table->prepare(*fixture.lease, request(), fixture.driver);
    REQUIRE(prepared);
    const auto committed = fixture.table->commit(*fixture.lease, request().operation, fixture.driver);
    REQUIRE(committed);
    REQUIRE(committed->disposition == AdoptDisposition::Committed);
    AdoptClientAttempt client(request().operation);
    CHECK(!client.beginCommit());
    auto unsolicited = client.observe(*committed);
    REQUIRE(!unsolicited);
    CHECK(unsolicited.error() == AdoptTransactionError::Conflict);
    CHECK(!client.isCommitted());

    REQUIRE(client.observe(*prepared));
    REQUIRE(client.observe(*prepared));
    unsolicited = client.observe(*committed);
    REQUIRE(!unsolicited);
    CHECK(unsolicited.error() == AdoptTransactionError::Conflict);
    CHECK(!client.isCommitted());
    REQUIRE(client.beginCommit());
    CHECK(!client.mayResumeLocal());
    REQUIRE(client.observe(*committed));
    REQUIRE(client.observe(*committed));
    CHECK(client.isCommitted());
    CHECK(!client.mayResumeLocal());
    CHECK_EQ(fixture.counts.activations, 1);
}

TEST(adoptClientAcceptsExactTerminalReplayAndRejectsEveryChangedTerminal)
{
    for (const auto disposition : { AdoptDisposition::Committed, AdoptDisposition::Aborted,
             AdoptDisposition::Expired, AdoptDisposition::Rejected }) {
        Fixture fixture;
        REQUIRE(fixture.isReady());
        const auto prepared = fixture.table->prepare(*fixture.lease, request(), fixture.driver);
        REQUIRE(prepared);
        AdoptClientAttempt client(request().operation);
        REQUIRE(client.observe(*prepared));
        REQUIRE(client.beginCommit());
        auto terminal = fixture.table->status(*fixture.lease, request().operation, fixture.driver);
        switch (disposition) {
        case AdoptDisposition::Committed:
            terminal = fixture.table->commit(*fixture.lease, request().operation, fixture.driver);
            break;
        case AdoptDisposition::Aborted:
            terminal = fixture.table->abort(*fixture.lease, request().operation, fixture.driver);
            break;
        case AdoptDisposition::Expired:
            REQUIRE(fixture.table->advanceTime(10));
            terminal = fixture.table->status(*fixture.lease, request().operation, fixture.driver);
            break;
        case AdoptDisposition::Rejected:
            fixture.driver.shouldRetargetDuringReservation = true;
            terminal = fixture.table->commit(*fixture.lease, request().operation, fixture.driver);
            break;
        default:
            REQUIRE(false);
        }
        REQUIRE(terminal);
        REQUIRE(terminal->disposition == disposition);
        REQUIRE(client.observe(*terminal));
        REQUIRE(client.observe(*terminal));
        const bool wasCommitted = disposition == AdoptDisposition::Committed;
        CHECK(client.isCommitted() == wasCommitted);
        CHECK(client.mayResumeLocal() != wasCommitted);

        std::array<AdoptOutcome, 5> changed { *terminal, *terminal, *terminal, *terminal, *terminal };
        ++changed[0].target.sessionID;
        ++changed[1].target.paneID.low;
        ++changed[2].operation.operationID.low;
        ++changed[3].operation.daemonInstanceID;
        changed[4].disposition = wasCommitted ? AdoptDisposition::Aborted : AdoptDisposition::Committed;
        changed[4].rejection.reset();
        for (std::size_t index = 0; index < changed.size(); ++index) {
            const auto refused = client.observe(changed[index]);
            REQUIRE(!refused);
            CHECK(refused.error() == (index == 2 || index == 3 ? AdoptTransactionError::InvalidIdentity : AdoptTransactionError::Conflict));
            CHECK(client.isCommitted() == wasCommitted);
            CHECK(client.mayResumeLocal() != wasCommitted);
            REQUIRE(client.observe(*terminal));
        }
        if (disposition == AdoptDisposition::Rejected) {
            auto otherReason = *terminal;
            REQUIRE(otherReason.rejection == AdoptRejection::InvalidCandidate);
            otherReason.rejection = AdoptRejection::NotOwner;
            const auto refused = client.observe(otherReason);
            REQUIRE(!refused);
            CHECK(refused.error() == AdoptTransactionError::Conflict);
            REQUIRE(client.observe(*terminal));
            CHECK(client.mayResumeLocal());
        }
        CHECK(!client.beginCommit());
    }
}

UBIQUITTY_TEST_MAIN()
