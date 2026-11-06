// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Session/AdoptTransactionWire.h"

#ifndef UBIQUITTY_TEST_WITHOUT_CORE
#define UBIQUITTY_TEST_WITHOUT_CORE
#endif
#include "Harness.h"

#include <algorithm>
#include <array>
#include <limits>
#include <optional>
#include <utility>

namespace {

namespace Session = Ubiquitty::Session;
namespace Local = Ubiquitty::Protocol::Local;

Session::AdoptOperation operation()
{
    return { .operationID = { .low = 0x123456789ABCDEF0, .high = 0xFEDCBA9876543210 },
        .daemonInstanceID = 0x8877665544332211 };
}

constexpr std::uint64_t correlation = 0x1122334455667788;

Session::AdoptTarget target()
{
    return { .sessionID = 0xAABBCCDD, .paneID = { .low = 0x1234432112344321, .high = 0x5678876556788765 } };
}

Session::AdoptOutcome outcome()
{
    return { .operation = operation(), .disposition = Session::AdoptDisposition::Prepared, .target = target() };
}

constexpr std::array dispositionPairs {
    std::pair { Session::AdoptDisposition::Unknown, Local::AdoptWireDisposition::Unknown },
    std::pair { Session::AdoptDisposition::Prepared, Local::AdoptWireDisposition::Prepared },
    std::pair { Session::AdoptDisposition::Committed, Local::AdoptWireDisposition::Committed },
    std::pair { Session::AdoptDisposition::Aborted, Local::AdoptWireDisposition::Aborted },
    std::pair { Session::AdoptDisposition::Expired, Local::AdoptWireDisposition::Expired },
    std::pair { Session::AdoptDisposition::Rejected, Local::AdoptWireDisposition::Rejected },
};

constexpr std::array rejectionPairs {
    std::pair { Session::AdoptRejection::InvalidPayload, Local::AdoptWireRejection::InvalidPayload },
    std::pair { Session::AdoptRejection::NotOwner, Local::AdoptWireRejection::NotOwner },
    std::pair { Session::AdoptRejection::NoMemory, Local::AdoptWireRejection::NoMemory },
    std::pair { Session::AdoptRejection::InvalidCandidate, Local::AdoptWireRejection::InvalidCandidate },
    std::pair { Session::AdoptRejection::HostFull, Local::AdoptWireRejection::HostFull },
};

constexpr std::array refusalPairs {
    std::pair { Session::AdoptTransactionError::InvalidIdentity, Local::AdoptWireRefusal::InvalidIdentity },
    std::pair { Session::AdoptTransactionError::InvalidPolicy, Local::AdoptWireRefusal::InvalidPolicy },
    std::pair { Session::AdoptTransactionError::NoMemory, Local::AdoptWireRefusal::NoMemory },
    std::pair { Session::AdoptTransactionError::Busy, Local::AdoptWireRefusal::Busy },
    std::pair { Session::AdoptTransactionError::StaleAuthority, Local::AdoptWireRefusal::StaleAuthority },
    std::pair { Session::AdoptTransactionError::Unauthorised, Local::AdoptWireRefusal::Unauthorised },
    std::pair { Session::AdoptTransactionError::Conflict, Local::AdoptWireRefusal::Conflict },
    std::pair { Session::AdoptTransactionError::Reentrant, Local::AdoptWireRefusal::Reentrant },
    std::pair { Session::AdoptTransactionError::ReplyUnavailable, Local::AdoptWireRefusal::ReplyUnavailable },
    std::pair { Session::AdoptTransactionError::InvalidTime, Local::AdoptWireRefusal::InvalidTime },
    std::pair { Session::AdoptTransactionError::GenerationExhausted, Local::AdoptWireRefusal::GenerationExhausted },
    std::pair { Session::AdoptTransactionError::ActiveCandidate, Local::AdoptWireRefusal::ActiveCandidate },
    std::pair { Session::AdoptTransactionError::RetirementUnproven, Local::AdoptWireRefusal::RetirementUnproven },
};

template<typename Domain, typename Wire, std::size_t count, typename FromWire, typename ToWire>
void checkEnumMapping(const std::array<std::pair<Domain, Wire>, count>& pairs, FromWire fromWire, ToWire toWire)
{
    for (unsigned value = 0; value <= 255; ++value) {
        const auto domain = static_cast<Domain>(value);
        const auto wire = static_cast<Wire>(value);
        const auto domainPair = std::ranges::find_if(pairs, [domain](auto pair) { return pair.first == domain; });
        const auto wirePair = std::ranges::find_if(pairs, [wire](auto pair) { return pair.second == wire; });
        const auto exported = toWire(domain);
        const auto imported = fromWire(wire);
        CHECK_EQ(exported.has_value(), domainPair != pairs.end());
        CHECK_EQ(imported.has_value(), wirePair != pairs.end());
        if (exported && domainPair != pairs.end())
            CHECK(*exported == domainPair->second);
        if (imported && wirePair != pairs.end())
            CHECK(*imported == wirePair->first);
    }
}

} // namespace

TEST(adoptTransactionWireOperationsPreserveEveryIdentityBit)
{
    for (const auto identity : { operation().operationID, Ubiquitty::Protocol::Sync::ID128 { 0, 1 },
             Ubiquitty::Protocol::Sync::ID128 { 1, 0 } }) {
        auto expected = operation();
        expected.operationID = identity;
        const auto wire = Session::adoptOperationToWire(expected);
        REQUIRE(wire.has_value());
        CHECK(wire->operationID == identity);
        CHECK_EQ(wire->daemonInstanceID, expected.daemonInstanceID);
        const auto restored = Session::adoptOperationFromWire(*wire);
        REQUIRE(restored.has_value());
        CHECK(*restored == expected);
    }
}

TEST(adoptTransactionWireRejectsEmptyOperationOrDaemonIdentityInBothDirections)
{
    for (unsigned field = 0; field < 2; ++field) {
        auto invalid = operation();
        if (!field)
            invalid.operationID = {};
        else
            invalid.daemonInstanceID = 0;
        CHECK(!Session::adoptOperationToWire(invalid));
        CHECK(!Session::adoptOperationFromWire({ .operationID = invalid.operationID, .daemonInstanceID = invalid.daemonInstanceID }));
    }
}

TEST(adoptTransactionWirePreparationPreservesDigestLengthAndOperationExactly)
{
    Session::AdoptPreparation expected { .operation = operation(), .bodyBytes = 1234567 };
    for (std::size_t index = 0; index < expected.bodySHA256.size(); ++index)
        expected.bodySHA256[index] = static_cast<std::uint8_t>(index * 7);
    const auto wire = Session::adoptPreparationToWire(expected);
    REQUIRE(wire.has_value());
    CHECK_EQ(wire->bodyBytes, expected.bodyBytes);
    CHECK(wire->bodySHA256 == expected.bodySHA256);
    const auto restored = Session::adoptPreparationFromWire(*wire);
    REQUIRE(restored.has_value());
    CHECK(*restored == expected);
    auto invalid = expected;
    invalid.bodyBytes = 0;
    CHECK(!Session::adoptPreparationToWire(invalid));
    auto invalidWire = *wire;
    invalidWire.bodyBytes = 0;
    CHECK(!Session::adoptPreparationFromWire(invalidWire));
    invalid = expected;
    invalid.operation.daemonInstanceID = 0;
    CHECK(!Session::adoptPreparationToWire(invalid));
    invalidWire = *wire;
    invalidWire.operation.operationID = {};
    CHECK(!Session::adoptPreparationFromWire(invalidWire));
}

TEST(adoptTransactionWirePreparationChecksRepresentationWithoutInventingAnAllowance)
{
    Local::AdoptWirePreparation wire { .operation = { .operationID = operation().operationID,
                                           .daemonInstanceID = operation().daemonInstanceID },
        .bodyBytes = std::numeric_limits<std::uint64_t>::max() };
    wire.bodySHA256.fill(0xFF);
    const auto imported = Session::adoptPreparationFromWire(wire);
    CHECK_EQ(imported.has_value(), std::in_range<std::size_t>(wire.bodyBytes));
    if (imported) {
        const auto exported = Session::adoptPreparationToWire(*imported);
        REQUIRE(exported.has_value());
        CHECK(*exported == wire);
    } else
        CHECK(imported.error() == Session::AdoptTransactionWireError::UnrepresentableLength);
    Session::AdoptPreparation domain { .operation = operation(), .bodyBytes = std::numeric_limits<std::size_t>::max() };
    const auto exported = Session::adoptPreparationToWire(domain);
    CHECK_EQ(exported.has_value(), std::in_range<std::uint64_t>(domain.bodyBytes));
    if (exported) {
        const auto restored = Session::adoptPreparationFromWire(*exported);
        REQUIRE(restored.has_value());
        CHECK(*restored == domain);
    }
}

TEST(adoptTransactionWireTargetsPreserveEmptyOrCompletePairsAndRejectPartialOnes)
{
    const std::array<Session::AdoptTarget, 4> targets { Session::AdoptTarget {}, target(),
        Session::AdoptTarget { .sessionID = 1 }, Session::AdoptTarget { .paneID = { 0, 1 } } };
    for (std::size_t index = 0; index < targets.size(); ++index) {
        const auto wire = Session::adoptTargetToWire(targets[index]);
        CHECK_EQ(wire.has_value(), index < 2);
        const auto imported = Session::adoptTargetFromWire({ .sessionID = targets[index].sessionID, .paneID = targets[index].paneID });
        CHECK_EQ(imported.has_value(), index < 2);
        if (imported)
            CHECK(*imported == targets[index]);
    }
}

TEST(adoptTransactionWireMapsEveryDispositionWithoutNumericCoupling)
{
    checkEnumMapping(dispositionPairs, Session::adoptDispositionFromWire, Session::adoptDispositionToWire);
}

TEST(adoptTransactionWireMapsEveryRejectionAndRejectsTheWireSentinel)
{
    checkEnumMapping(rejectionPairs, Session::adoptRejectionFromWire, Session::adoptRejectionToWire);
    CHECK(!Session::adoptRejectionFromWire(static_cast<Local::AdoptWireRejection>(0)));
}

TEST(adoptTransactionWireMapsAllThirteenAdmissionErrorsSeparatelyFromOutcomes)
{
    checkEnumMapping(refusalPairs, Session::adoptRefusalFromWire, Session::adoptRefusalToWire);
    CHECK(!Session::adoptRefusalFromWire(static_cast<Local::AdoptWireRefusal>(0)));
}

TEST(adoptTransactionWireOutcomeConversionsShareTheExactCodecShapeGrammar)
{
    const std::array<Session::AdoptTarget, 4> targets { Session::AdoptTarget {}, target(),
        Session::AdoptTarget { .sessionID = 1 }, Session::AdoptTarget { .paneID = { 2, 3 } } };
    for (const auto& disposition : dispositionPairs) {
        for (std::size_t targetIndex = 0; targetIndex < targets.size(); ++targetIndex) {
            for (std::size_t rejectionIndex = 0; rejectionIndex <= rejectionPairs.size(); ++rejectionIndex) {
                Session::AdoptOutcome domain { .operation = operation(), .disposition = disposition.first, .target = targets[targetIndex] };
                Local::AdoptWireOutcome wire { .correlation = correlation,
                    .operation = { .operationID = operation().operationID, .daemonInstanceID = operation().daemonInstanceID },
                    .disposition = disposition.second,
                    .target = { .sessionID = domain.target.sessionID, .paneID = domain.target.paneID } };
                if (rejectionIndex) {
                    domain.rejection = rejectionPairs[rejectionIndex - 1].first;
                    wire.rejection = rejectionPairs[rejectionIndex - 1].second;
                }
                const bool isCodecShape = Local::encodeAdoptWireOutcome(wire).has_value();
                const auto exported = Session::adoptOutcomeToWire(domain, correlation);
                const auto imported = Session::adoptOutcomeFromWire(wire, operation(), correlation);
                CHECK_EQ(exported.has_value(), isCodecShape);
                CHECK_EQ(imported.has_value(), isCodecShape);
                if (exported)
                    CHECK(*exported == wire);
                if (imported)
                    CHECK(*imported == domain);
            }
        }
    }
}

TEST(adoptTransactionWireRejectsUnknownOutcomeValuesWithoutChangingInputs)
{
    const auto original = outcome();
    auto invalid = original;
    invalid.disposition = static_cast<Session::AdoptDisposition>(255);
    CHECK(!Session::adoptOutcomeToWire(invalid, correlation));
    invalid = original;
    invalid.rejection = static_cast<Session::AdoptRejection>(255);
    const auto unchanged = invalid;
    CHECK(!Session::adoptOutcomeToWire(invalid, correlation));
    CHECK(invalid == unchanged);
    const auto valid = Session::adoptOutcomeToWire(original, correlation);
    REQUIRE(valid.has_value());
    auto wire = *valid;
    wire.disposition = static_cast<Local::AdoptWireDisposition>(255);
    CHECK(!Session::adoptOutcomeFromWire(wire, operation(), correlation));
    wire = *valid;
    wire.rejection = static_cast<Local::AdoptWireRejection>(255);
    const auto unchangedWire = wire;
    CHECK(!Session::adoptOutcomeFromWire(wire, operation(), correlation));
    CHECK(wire == unchangedWire);
}

TEST(adoptTransactionWireReplyImportsRequireTheExactOperationDaemonAndCorrelation)
{
    const auto wireOutcome = Session::adoptOutcomeToWire(outcome(), correlation);
    const auto wireRefused = Session::adoptRefusedToWire(operation(), Session::AdoptTransactionError::Busy, correlation);
    REQUIRE(wireOutcome && wireRefused);
    for (unsigned field = 0; field < 4; ++field) {
        auto expected = operation();
        auto expectedCorrelation = correlation;
        if (!field)
            expected.operationID.low ^= 1;
        else if (field == 1)
            expected.operationID.high ^= 1;
        else if (field == 2)
            expected.daemonInstanceID ^= 1;
        else
            expectedCorrelation ^= 1;
        const auto importedOutcome = Session::adoptOutcomeFromWire(*wireOutcome, expected, expectedCorrelation);
        const auto importedRefused = Session::adoptRefusedFromWire(*wireRefused, expected, expectedCorrelation);
        REQUIRE(!importedOutcome && !importedRefused);
        const auto error = field == 3 ? Session::AdoptTransactionWireError::MismatchedCorrelation
                                      : Session::AdoptTransactionWireError::MismatchedOperation;
        CHECK(importedOutcome.error() == error);
        CHECK(importedRefused.error() == error);
    }
    CHECK(!Session::adoptOutcomeFromWire(*wireOutcome, operation(), 0));
    CHECK(!Session::adoptRefusedFromWire(*wireRefused, operation(), 0));
    CHECK(!Session::adoptOutcomeFromWire(*wireOutcome, {}, correlation));
    CHECK(!Session::adoptRefusedFromWire(*wireRefused, {}, correlation));
    CHECK(!Session::adoptOutcomeToWire(outcome(), 0));
    CHECK(!Session::adoptRefusedToWire(operation(), Session::AdoptTransactionError::Busy, 0));
}

TEST(adoptTransactionWireRefusedRecordsPreserveEveryErrorAndOriginalIdentity)
{
    for (const auto& pair : refusalPairs) {
        const auto wire = Session::adoptRefusedToWire(operation(), pair.first, correlation);
        REQUIRE(wire.has_value());
        CHECK(wire->reason == pair.second);
        CHECK_EQ(wire->correlation, correlation);
        const auto encoded = Local::encodeAdoptWireRefused(*wire);
        REQUIRE(encoded.has_value());
        const auto restored = Session::adoptRefusedFromWire(*wire, operation(), correlation);
        REQUIRE(restored.has_value());
        CHECK(*restored == pair.first);
    }
    CHECK(!Session::adoptRefusedToWire(operation(), static_cast<Session::AdoptTransactionError>(255), correlation));
    auto invalid = Session::adoptRefusedToWire(operation(), Session::AdoptTransactionError::Busy, correlation);
    REQUIRE(invalid.has_value());
    invalid->reason = static_cast<Local::AdoptWireRefusal>(255);
    CHECK(!Session::adoptRefusedFromWire(*invalid, operation(), correlation));
}

TEST(adoptTransactionWireConversionsDoNotResolveOwnershipUncertainty)
{
    Session::AdoptClientAttempt attempt(operation());
    REQUIRE(attempt.observe(outcome()).has_value());
    REQUIRE(attempt.beginCommit().has_value());
    CHECK(!attempt.mayResumeLocal());
    for (const auto& pair : refusalPairs) {
        const auto wire = Session::adoptRefusedToWire(operation(), pair.first, correlation);
        REQUIRE(wire.has_value());
        const auto refused = Session::adoptRefusedFromWire(*wire, operation(), correlation);
        REQUIRE(refused.has_value());
        CHECK(!attempt.mayResumeLocal());
    }
    auto prepared = Session::adoptOutcomeToWire(outcome(), correlation);
    REQUIRE(prepared.has_value());
    const auto imported = Session::adoptOutcomeFromWire(*prepared, operation(), correlation);
    REQUIRE(imported.has_value());
    REQUIRE(attempt.observe(*imported).has_value());
    CHECK(!attempt.mayResumeLocal());
    prepared->target.paneID.high ^= 1;
    const auto retargeted = Session::adoptOutcomeFromWire(*prepared, operation(), correlation);
    REQUIRE(retargeted.has_value());
    CHECK(!attempt.observe(*retargeted));
    CHECK(!attempt.mayResumeLocal());
}

UBIQUITTY_TEST_MAIN()
