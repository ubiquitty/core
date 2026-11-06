// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Session/AdoptTransactionWire.h"

#include <utility>

namespace Ubiquitty::Session {
namespace Local = Protocol::Local;

namespace {

bool isValid(AdoptOperation operation)
{
    return operation.daemonInstanceID && (operation.operationID.low || operation.operationID.high);
}

bool isCompleteOrEmpty(AdoptTarget target)
{
    const bool hasPane = target.paneID.low || target.paneID.high;
    return static_cast<bool>(target.sessionID) == hasPane;
}

AdoptTransactionWireError conversionError(Local::AdoptWireError error)
{
    switch (error) {
    case Local::AdoptWireError::InvalidIdentity:
        return AdoptTransactionWireError::InvalidIdentity;
    case Local::AdoptWireError::TooLong:
        return AdoptTransactionWireError::UnrepresentableLength;
    case Local::AdoptWireError::Malformed:
    case Local::AdoptWireError::InvalidPayload:
    case Local::AdoptWireError::DigestMismatch:
        return AdoptTransactionWireError::InvalidValue;
    }
    return AdoptTransactionWireError::InvalidValue;
}

std::expected<void, AdoptTransactionWireError> matchesRequest(const Local::AdoptWireOperation& operation, std::uint64_t correlation,
    const AdoptOperation& expectedOperation, std::uint64_t expectedCorrelation)
{
    const auto converted = adoptOperationFromWire(operation);
    if (!converted)
        return std::unexpected(converted.error());
    if (!isValid(expectedOperation) || !correlation || !expectedCorrelation)
        return std::unexpected(AdoptTransactionWireError::InvalidIdentity);
    if (*converted != expectedOperation)
        return std::unexpected(AdoptTransactionWireError::MismatchedOperation);
    if (correlation != expectedCorrelation)
        return std::unexpected(AdoptTransactionWireError::MismatchedCorrelation);
    return {};
}

} // namespace

std::expected<AdoptOperation, AdoptTransactionWireError> adoptOperationFromWire(const Local::AdoptWireOperation& operation)
{
    AdoptOperation converted { .operationID = operation.operationID, .daemonInstanceID = operation.daemonInstanceID };
    if (!isValid(converted))
        return std::unexpected(AdoptTransactionWireError::InvalidIdentity);
    return converted;
}

std::expected<Local::AdoptWireOperation, AdoptTransactionWireError> adoptOperationToWire(const AdoptOperation& operation)
{
    if (!isValid(operation))
        return std::unexpected(AdoptTransactionWireError::InvalidIdentity);
    return Local::AdoptWireOperation { .operationID = operation.operationID, .daemonInstanceID = operation.daemonInstanceID };
}

std::expected<AdoptPreparation, AdoptTransactionWireError> adoptPreparationFromWire(const Local::AdoptWirePreparation& preparation)
{
    const auto operation = adoptOperationFromWire(preparation.operation);
    if (!operation)
        return std::unexpected(operation.error());
    if (!preparation.bodyBytes)
        return std::unexpected(AdoptTransactionWireError::InvalidValue);
    if (!std::in_range<std::size_t>(preparation.bodyBytes))
        return std::unexpected(AdoptTransactionWireError::UnrepresentableLength);
    return AdoptPreparation { .operation = *operation, .bodySHA256 = preparation.bodySHA256, .bodyBytes = static_cast<std::size_t>(preparation.bodyBytes) };
}

std::expected<Local::AdoptWirePreparation, AdoptTransactionWireError> adoptPreparationToWire(const AdoptPreparation& preparation)
{
    const auto operation = adoptOperationToWire(preparation.operation);
    if (!operation)
        return std::unexpected(operation.error());
    if (!preparation.bodyBytes)
        return std::unexpected(AdoptTransactionWireError::InvalidValue);
    if (!std::in_range<std::uint64_t>(preparation.bodyBytes))
        return std::unexpected(AdoptTransactionWireError::UnrepresentableLength);
    return Local::AdoptWirePreparation { .operation = *operation, .bodyBytes = static_cast<std::uint64_t>(preparation.bodyBytes), .bodySHA256 = preparation.bodySHA256 };
}

std::expected<AdoptTarget, AdoptTransactionWireError> adoptTargetFromWire(const Local::AdoptWireTarget& target)
{
    AdoptTarget converted { .sessionID = target.sessionID, .paneID = target.paneID };
    if (!isCompleteOrEmpty(converted))
        return std::unexpected(AdoptTransactionWireError::InvalidIdentity);
    return converted;
}

std::expected<Local::AdoptWireTarget, AdoptTransactionWireError> adoptTargetToWire(const AdoptTarget& target)
{
    if (!isCompleteOrEmpty(target))
        return std::unexpected(AdoptTransactionWireError::InvalidIdentity);
    return Local::AdoptWireTarget { .sessionID = target.sessionID, .paneID = target.paneID };
}

std::expected<AdoptDisposition, AdoptTransactionWireError> adoptDispositionFromWire(Local::AdoptWireDisposition disposition)
{
    switch (disposition) {
    case Local::AdoptWireDisposition::Unknown:
        return AdoptDisposition::Unknown;
    case Local::AdoptWireDisposition::Prepared:
        return AdoptDisposition::Prepared;
    case Local::AdoptWireDisposition::Committed:
        return AdoptDisposition::Committed;
    case Local::AdoptWireDisposition::Aborted:
        return AdoptDisposition::Aborted;
    case Local::AdoptWireDisposition::Expired:
        return AdoptDisposition::Expired;
    case Local::AdoptWireDisposition::Rejected:
        return AdoptDisposition::Rejected;
    }
    return std::unexpected(AdoptTransactionWireError::InvalidValue);
}

std::expected<Local::AdoptWireDisposition, AdoptTransactionWireError> adoptDispositionToWire(AdoptDisposition disposition)
{
    switch (disposition) {
    case AdoptDisposition::Unknown:
        return Local::AdoptWireDisposition::Unknown;
    case AdoptDisposition::Prepared:
        return Local::AdoptWireDisposition::Prepared;
    case AdoptDisposition::Committed:
        return Local::AdoptWireDisposition::Committed;
    case AdoptDisposition::Aborted:
        return Local::AdoptWireDisposition::Aborted;
    case AdoptDisposition::Expired:
        return Local::AdoptWireDisposition::Expired;
    case AdoptDisposition::Rejected:
        return Local::AdoptWireDisposition::Rejected;
    }
    return std::unexpected(AdoptTransactionWireError::InvalidValue);
}

std::expected<AdoptRejection, AdoptTransactionWireError> adoptRejectionFromWire(Local::AdoptWireRejection rejection)
{
    switch (rejection) {
    case Local::AdoptWireRejection::InvalidPayload:
        return AdoptRejection::InvalidPayload;
    case Local::AdoptWireRejection::NotOwner:
        return AdoptRejection::NotOwner;
    case Local::AdoptWireRejection::NoMemory:
        return AdoptRejection::NoMemory;
    case Local::AdoptWireRejection::InvalidCandidate:
        return AdoptRejection::InvalidCandidate;
    case Local::AdoptWireRejection::HostFull:
        return AdoptRejection::HostFull;
    }
    return std::unexpected(AdoptTransactionWireError::InvalidValue);
}

std::expected<Local::AdoptWireRejection, AdoptTransactionWireError> adoptRejectionToWire(AdoptRejection rejection)
{
    switch (rejection) {
    case AdoptRejection::InvalidPayload:
        return Local::AdoptWireRejection::InvalidPayload;
    case AdoptRejection::NotOwner:
        return Local::AdoptWireRejection::NotOwner;
    case AdoptRejection::NoMemory:
        return Local::AdoptWireRejection::NoMemory;
    case AdoptRejection::InvalidCandidate:
        return Local::AdoptWireRejection::InvalidCandidate;
    case AdoptRejection::HostFull:
        return Local::AdoptWireRejection::HostFull;
    }
    return std::unexpected(AdoptTransactionWireError::InvalidValue);
}

std::expected<AdoptTransactionError, AdoptTransactionWireError> adoptRefusalFromWire(Local::AdoptWireRefusal refusal)
{
    switch (refusal) {
    case Local::AdoptWireRefusal::InvalidIdentity:
        return AdoptTransactionError::InvalidIdentity;
    case Local::AdoptWireRefusal::InvalidPolicy:
        return AdoptTransactionError::InvalidPolicy;
    case Local::AdoptWireRefusal::NoMemory:
        return AdoptTransactionError::NoMemory;
    case Local::AdoptWireRefusal::Busy:
        return AdoptTransactionError::Busy;
    case Local::AdoptWireRefusal::StaleAuthority:
        return AdoptTransactionError::StaleAuthority;
    case Local::AdoptWireRefusal::Unauthorised:
        return AdoptTransactionError::Unauthorised;
    case Local::AdoptWireRefusal::Conflict:
        return AdoptTransactionError::Conflict;
    case Local::AdoptWireRefusal::Reentrant:
        return AdoptTransactionError::Reentrant;
    case Local::AdoptWireRefusal::ReplyUnavailable:
        return AdoptTransactionError::ReplyUnavailable;
    case Local::AdoptWireRefusal::InvalidTime:
        return AdoptTransactionError::InvalidTime;
    case Local::AdoptWireRefusal::GenerationExhausted:
        return AdoptTransactionError::GenerationExhausted;
    case Local::AdoptWireRefusal::ActiveCandidate:
        return AdoptTransactionError::ActiveCandidate;
    case Local::AdoptWireRefusal::RetirementUnproven:
        return AdoptTransactionError::RetirementUnproven;
    }
    return std::unexpected(AdoptTransactionWireError::InvalidValue);
}

std::expected<Local::AdoptWireRefusal, AdoptTransactionWireError> adoptRefusalToWire(AdoptTransactionError refusal)
{
    switch (refusal) {
    case AdoptTransactionError::InvalidIdentity:
        return Local::AdoptWireRefusal::InvalidIdentity;
    case AdoptTransactionError::InvalidPolicy:
        return Local::AdoptWireRefusal::InvalidPolicy;
    case AdoptTransactionError::NoMemory:
        return Local::AdoptWireRefusal::NoMemory;
    case AdoptTransactionError::Busy:
        return Local::AdoptWireRefusal::Busy;
    case AdoptTransactionError::StaleAuthority:
        return Local::AdoptWireRefusal::StaleAuthority;
    case AdoptTransactionError::Unauthorised:
        return Local::AdoptWireRefusal::Unauthorised;
    case AdoptTransactionError::Conflict:
        return Local::AdoptWireRefusal::Conflict;
    case AdoptTransactionError::Reentrant:
        return Local::AdoptWireRefusal::Reentrant;
    case AdoptTransactionError::ReplyUnavailable:
        return Local::AdoptWireRefusal::ReplyUnavailable;
    case AdoptTransactionError::InvalidTime:
        return Local::AdoptWireRefusal::InvalidTime;
    case AdoptTransactionError::GenerationExhausted:
        return Local::AdoptWireRefusal::GenerationExhausted;
    case AdoptTransactionError::ActiveCandidate:
        return Local::AdoptWireRefusal::ActiveCandidate;
    case AdoptTransactionError::RetirementUnproven:
        return Local::AdoptWireRefusal::RetirementUnproven;
    }
    return std::unexpected(AdoptTransactionWireError::InvalidValue);
}

std::expected<Local::AdoptWireOutcome, AdoptTransactionWireError> adoptOutcomeToWire(const AdoptOutcome& outcome, std::uint64_t correlation)
{
    if (!correlation)
        return std::unexpected(AdoptTransactionWireError::InvalidIdentity);
    const auto operation = adoptOperationToWire(outcome.operation);
    const auto target = adoptTargetToWire(outcome.target);
    const auto disposition = adoptDispositionToWire(outcome.disposition);
    if (!operation)
        return std::unexpected(operation.error());
    if (!target)
        return std::unexpected(target.error());
    if (!disposition)
        return std::unexpected(disposition.error());
    Local::AdoptWireOutcome converted { .correlation = correlation, .operation = *operation, .disposition = *disposition, .target = *target };
    if (outcome.rejection) {
        const auto rejection = adoptRejectionToWire(*outcome.rejection);
        if (!rejection)
            return std::unexpected(rejection.error());
        converted.rejection = *rejection;
    }
    // The allocation-free codec owns the outcome-shape grammar in both directions.
    if (const auto encoded = Local::encodeAdoptWireOutcome(converted); !encoded)
        return std::unexpected(conversionError(encoded.error()));
    return converted;
}

std::expected<AdoptOutcome, AdoptTransactionWireError> adoptOutcomeFromWire(const Local::AdoptWireOutcome& outcome,
    const AdoptOperation& expectedOperation, std::uint64_t expectedCorrelation)
{
    if (const auto match = matchesRequest(outcome.operation, outcome.correlation, expectedOperation, expectedCorrelation); !match)
        return std::unexpected(match.error());
    if (const auto encoded = Local::encodeAdoptWireOutcome(outcome); !encoded)
        return std::unexpected(conversionError(encoded.error()));
    const auto operation = adoptOperationFromWire(outcome.operation);
    const auto target = adoptTargetFromWire(outcome.target);
    const auto disposition = adoptDispositionFromWire(outcome.disposition);
    if (!operation)
        return std::unexpected(operation.error());
    if (!target)
        return std::unexpected(target.error());
    if (!disposition)
        return std::unexpected(disposition.error());
    AdoptOutcome converted { .operation = *operation, .disposition = *disposition, .target = *target };
    if (outcome.rejection) {
        const auto rejection = adoptRejectionFromWire(*outcome.rejection);
        if (!rejection)
            return std::unexpected(rejection.error());
        converted.rejection = *rejection;
    }
    return converted;
}

std::expected<Local::AdoptWireRefused, AdoptTransactionWireError> adoptRefusedToWire(const AdoptOperation& operation,
    AdoptTransactionError error, std::uint64_t correlation)
{
    if (!correlation)
        return std::unexpected(AdoptTransactionWireError::InvalidIdentity);
    const auto convertedOperation = adoptOperationToWire(operation);
    if (!convertedOperation)
        return std::unexpected(convertedOperation.error());
    const auto reason = adoptRefusalToWire(error);
    if (!reason)
        return std::unexpected(reason.error());
    return Local::AdoptWireRefused { .correlation = correlation, .operation = *convertedOperation, .reason = *reason };
}

std::expected<AdoptTransactionError, AdoptTransactionWireError> adoptRefusedFromWire(const Local::AdoptWireRefused& refused,
    const AdoptOperation& expectedOperation, std::uint64_t expectedCorrelation)
{
    if (const auto match = matchesRequest(refused.operation, refused.correlation, expectedOperation, expectedCorrelation); !match)
        return std::unexpected(match.error());
    return adoptRefusalFromWire(refused.reason);
}

} // namespace Ubiquitty::Session
