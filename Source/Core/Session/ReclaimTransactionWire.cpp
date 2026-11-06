// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Session/ReclaimTransactionWire.h"

#include <algorithm>
#include <utility>

namespace Ubiquitty::Session {
namespace Local = Protocol::Local;
namespace {

using Error = ReclaimTransactionWireError;
static_assert(Local::reclaimWireMaxRuns == PAL::OutputQueue::maxPendingRuns);

Error conversionError(AdoptTransactionWireError error)
{
    switch (error) {
    case AdoptTransactionWireError::InvalidIdentity:
        return Error::InvalidIdentity;
    case AdoptTransactionWireError::InvalidValue:
        return Error::InvalidValue;
    case AdoptTransactionWireError::UnrepresentableLength:
        return Error::OverCapacity;
    case AdoptTransactionWireError::MismatchedOperation:
        return Error::MismatchedSelector;
    case AdoptTransactionWireError::MismatchedCorrelation:
        return Error::MismatchedCorrelation;
    }
    return Error::InvalidValue;
}

Error conversionError(ReclaimTransferError error)
{
    switch (error) {
    case ReclaimTransferError::InvalidIdentity:
        return Error::InvalidIdentity;
    case ReclaimTransferError::MismatchedIdentity:
        return Error::MismatchedSelector;
    case ReclaimTransferError::OverCapacity:
        return Error::OverCapacity;
    case ReclaimTransferError::DigestMismatch:
        return Error::DigestMismatch;
    case ReclaimTransferError::InvalidPolicy:
    case ReclaimTransferError::InputBarrier:
    case ReclaimTransferError::InvalidPartition:
    case ReclaimTransferError::DigestFailure:
    case ReclaimTransferError::TerminalRefused:
    case ReclaimTransferError::NoMemory:
    case ReclaimTransferError::Conflict:
        return Error::InvalidCapsule;
    }
    return Error::InvalidCapsule;
}

std::expected<void, Error> matchesSelector(const Local::ReclaimWireSelector& offered, const ReclaimTransactionSelector& expected)
{
    const auto converted = reclaimSelectorFromWire(offered);
    if (!converted)
        return std::unexpected(converted.error());
    if (const auto valid = reclaimSelectorToWire(expected); !valid)
        return std::unexpected(valid.error());
    if (*converted != expected)
        return std::unexpected(Error::MismatchedSelector);
    return {};
}

} // namespace

std::expected<Local::ReclaimWireSelector, Error> reclaimSelectorToWire(const ReclaimTransactionSelector& selector)
{
    const auto adoption = adoptOperationToWire(selector.adoption);
    const auto operation = adoptOperationToWire(selector.operation);
    if (!adoption || !operation)
        return std::unexpected(Error::InvalidIdentity);
    Local::ReclaimWireSelector converted { .adoption = *adoption, .operation = *operation, .target = selector.target, .barrier = { .clientInstance = selector.barrier.clientInstance, .finalInputSeq = selector.barrier.finalInputSeq } };
    if (!Local::isValidReclaimWireSelector(converted))
        return std::unexpected(Error::InvalidIdentity);
    return converted;
}

std::expected<ReclaimTransactionSelector, Error> reclaimSelectorFromWire(const Local::ReclaimWireSelector& selector)
{
    if (!Local::isValidReclaimWireSelector(selector))
        return std::unexpected(Error::InvalidIdentity);
    const auto adoption = adoptOperationFromWire(selector.adoption);
    const auto operation = adoptOperationFromWire(selector.operation);
    if (!adoption || !operation)
        return std::unexpected(Error::InvalidIdentity);
    return ReclaimTransactionSelector { .adoption = *adoption, .operation = *operation, .target = selector.target, .barrier = { .clientInstance = selector.barrier.clientInstance, .finalInputSeq = selector.barrier.finalInputSeq } };
}

std::expected<Local::ReclaimWireCapsule, Error> reclaimCapsuleToWire(const AdoptOperation& adoption, std::uint64_t correlation,
    const ReclaimCapsuleView& capsule, const ReclaimTransferLimits& limits, std::span<Local::ReclaimWireRun> runStorage)
{
    const auto selector = reclaimSelectorToWire({ .adoption = adoption, .operation = capsule.operation, .target = capsule.target, .barrier = capsule.barrier });
    if (!selector)
        return std::unexpected(selector.error());
    if (capsule.acceptedRuns.size() > runStorage.size() || capsule.acceptedRuns.size() > Local::reclaimWireMaxRuns)
        return std::unexpected(Error::OverCapacity);
    const auto digest = reclaimCapsuleSHA256(capsule, limits);
    if (!digest)
        return std::unexpected(conversionError(digest.error()));
    if (*digest != capsule.capsuleSHA256)
        return std::unexpected(Error::DigestMismatch);
    const auto prepared = adoptOutcomeToWire({ .operation = capsule.operation, .disposition = AdoptDisposition::Prepared, .target = { .sessionID = capsule.target.sessionID, .paneID = capsule.target.paneID } },
        correlation);
    if (!prepared)
        return std::unexpected(conversionError(prepared.error()));
    for (const auto& run : capsule.acceptedRuns) {
        if (!std::in_range<std::uint64_t>(run.bytes))
            return std::unexpected(Error::OverCapacity);
    }
    for (std::size_t index = 0; index < capsule.acceptedRuns.size(); ++index) {
        const auto& run = capsule.acceptedRuns[index];
        runStorage[index] = { .owner = run.owner, .bytes = static_cast<std::uint64_t>(run.bytes) };
    }
    return Local::ReclaimWireCapsule { .selector = *selector, .prepared = *prepared, .capsuleSHA256 = capsule.capsuleSHA256, .inputWritten = capsule.inputWritten, .acceptedRuns = runStorage.first(capsule.acceptedRuns.size()), .terminalBody = capsule.terminalBody, .acceptedPTY = { reinterpret_cast<const std::uint8_t*>(capsule.acceptedPTY.data()), capsule.acceptedPTY.size() } };
}

std::expected<ReclaimCapsuleView, Error> reclaimCapsuleFromWire(const Local::ReclaimWireCapsule& capsule,
    const ReclaimTransactionSelector& expectedSelector, std::uint64_t expectedCorrelation,
    const ReclaimTransferLimits& limits, std::span<PAL::OutputQueue::Run> runStorage)
{
    if (const auto valid = matchesSelector(capsule.selector, expectedSelector); !valid)
        return std::unexpected(valid.error());
    const auto prepared = adoptOutcomeFromWire(capsule.prepared, expectedSelector.operation, expectedCorrelation);
    if (!prepared)
        return std::unexpected(conversionError(prepared.error()));
    if (prepared->disposition != AdoptDisposition::Prepared
        || prepared->target != AdoptTarget { .sessionID = expectedSelector.target.sessionID, .paneID = expectedSelector.target.paneID })
        return std::unexpected(Error::InvalidValue);
    if (capsule.acceptedRuns.size() > runStorage.size() || capsule.acceptedRuns.size() > PAL::OutputQueue::maxPendingRuns)
        return std::unexpected(Error::OverCapacity);
    std::array<PAL::OutputQueue::Run, PAL::OutputQueue::maxPendingRuns> candidateRuns;
    for (std::size_t index = 0; index < capsule.acceptedRuns.size(); ++index) {
        const auto& run = capsule.acceptedRuns[index];
        if (!std::in_range<std::size_t>(run.bytes))
            return std::unexpected(Error::OverCapacity);
        candidateRuns[index] = { .owner = run.owner, .bytes = static_cast<std::size_t>(run.bytes) };
    }
    ReclaimCapsuleView converted { .target = expectedSelector.target, .operation = expectedSelector.operation, .barrier = expectedSelector.barrier, .inputWritten = capsule.inputWritten, .terminalBody = capsule.terminalBody, .acceptedPTY = { reinterpret_cast<const char*>(capsule.acceptedPTY.data()), capsule.acceptedPTY.size() }, .acceptedRuns = std::span(candidateRuns).first(capsule.acceptedRuns.size()), .capsuleSHA256 = capsule.capsuleSHA256 };
    const auto digest = reclaimCapsuleSHA256(converted, limits);
    if (!digest)
        return std::unexpected(conversionError(digest.error()));
    if (*digest != capsule.capsuleSHA256)
        return std::unexpected(Error::DigestMismatch);
    std::ranges::copy(converted.acceptedRuns, runStorage.begin());
    converted.acceptedRuns = runStorage.first(capsule.acceptedRuns.size());
    return converted;
}

std::expected<Local::ReclaimWireOutcome, Error> reclaimOutcomeToWire(const AdoptOperation& adoption,
    const ReclaimTransferEvidence& evidence, std::uint64_t correlation)
{
    const auto selector = reclaimSelectorToWire({ .adoption = adoption, .operation = evidence.outcome.operation, .target = evidence.target, .barrier = evidence.barrier });
    if (!selector)
        return std::unexpected(selector.error());
    const auto outcome = adoptOutcomeToWire(evidence.outcome, correlation);
    if (!outcome)
        return std::unexpected(conversionError(outcome.error()));
    Local::ReclaimWireOutcome converted { .selector = *selector, .capsuleSHA256 = evidence.capsuleSHA256, .outcome = *outcome };
    if (!Local::encodeReclaimWireOutcome(converted))
        return std::unexpected(Error::InvalidValue);
    return converted;
}

std::expected<ReclaimTransferEvidence, Error> reclaimEvidenceFromWire(const Local::ReclaimWireOutcome& evidence,
    const ReclaimTransactionSelector& expectedSelector, std::uint64_t expectedCorrelation,
    const std::array<std::uint8_t, 32>& expectedDigest)
{
    if (const auto valid = matchesSelector(evidence.selector, expectedSelector); !valid)
        return std::unexpected(valid.error());
    if (!Local::encodeReclaimWireOutcome(evidence))
        return std::unexpected(Error::InvalidValue);
    if (!evidence.capsuleSHA256)
        return std::unexpected(Error::MissingDigest);
    if (*evidence.capsuleSHA256 != expectedDigest)
        return std::unexpected(Error::DigestMismatch);
    const auto outcome = adoptOutcomeFromWire(evidence.outcome, expectedSelector.operation, expectedCorrelation);
    if (!outcome)
        return std::unexpected(conversionError(outcome.error()));
    return ReclaimTransferEvidence { .target = expectedSelector.target, .barrier = expectedSelector.barrier, .capsuleSHA256 = *evidence.capsuleSHA256, .outcome = *outcome };
}

std::expected<Local::ReclaimWireRefused, Error> reclaimRefusedToWire(const ReclaimTransactionSelector& selector,
    AdoptTransactionError reason, std::uint64_t correlation, const std::optional<std::array<std::uint8_t, 32>>& digest)
{
    const auto converted = reclaimSelectorToWire(selector);
    if (!converted)
        return std::unexpected(converted.error());
    const auto refusal = adoptRefusedToWire(selector.operation, reason, correlation);
    if (!refusal)
        return std::unexpected(conversionError(refusal.error()));
    return Local::ReclaimWireRefused { .selector = *converted, .capsuleSHA256 = digest, .refusal = *refusal };
}

std::expected<AdoptTransactionError, Error> reclaimRefusedFromWire(const Local::ReclaimWireRefused& refused,
    const ReclaimTransactionSelector& expectedSelector, std::uint64_t expectedCorrelation,
    const std::optional<std::array<std::uint8_t, 32>>& expectedDigest)
{
    if (const auto valid = matchesSelector(refused.selector, expectedSelector); !valid)
        return std::unexpected(valid.error());
    if (refused.capsuleSHA256 != expectedDigest)
        return std::unexpected(Error::DigestMismatch);
    const auto reason = adoptRefusedFromWire(refused.refusal, expectedSelector.operation, expectedCorrelation);
    if (!reason)
        return std::unexpected(conversionError(reason.error()));
    return *reason;
}

} // namespace Ubiquitty::Session
