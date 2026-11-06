// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Session/ReclaimTransfer.h"

#include "Core/Crypto/SHA256.h"
#include "Core/Terminal.h"

#include <limits>
#include <new>
#include <utility>

namespace Ubiquitty::Session {
namespace {

using Error = ReclaimTransferError;
using Identity = Protocol::Sync::ID128;

// Domain and version belong to the digest contract, not a remote frame grammar.
constexpr std::array<std::uint8_t, 12> capsuleDomain { 'U', 'B', 'Q', 'R', 'E', 'C', 'L', 'A', 'I', 'M', 0, 1 };
constexpr std::size_t capsuleFixedBytes = capsuleDomain.size() + 24 + 64 + 24 + 1 + 8 + 8 + 8 + 2 + 32;

bool isValid(Identity identity)
{
    return identity.low || identity.high;
}

bool isValid(const Protocol::Control::ReclaimTarget& target)
{
    return target.handle && target.sessionID && target.daemonInstanceID
        && isValid(target.hostInstanceID) && isValid(target.attachmentID) && isValid(target.paneID);
}

bool isValid(AdoptOperation operation)
{
    return isValid(operation.operationID) && operation.daemonInstanceID;
}

std::expected<void, Error> admit(const ReclaimCapsuleView& capsule, const ReclaimTransferLimits& limits)
{
    if (!limits.terminal.maxRecordBytes || !limits.maxCapsuleBytes)
        return std::unexpected(Error::InvalidPolicy);
    if (!isValid(capsule.target) || !isValid(capsule.operation) || !isValid(capsule.barrier.clientInstance)
        || capsule.operation.daemonInstanceID != capsule.target.daemonInstanceID)
        return std::unexpected(Error::InvalidIdentity);
    if (!capsule.inputWritten || *capsule.inputWritten != capsule.barrier.finalInputSeq)
        return std::unexpected(Error::InputBarrier);
    if (capsule.terminalBody.empty())
        return std::unexpected(Error::TerminalRefused);
    if (capsule.terminalBody.size() > limits.terminal.maxRecordBytes
        || capsule.acceptedPTY.size() > limits.maxAcceptedPTYBytes
        || capsule.acceptedPTY.size() > PAL::OutputQueue::maxPending
        || capsule.acceptedRuns.size() > PAL::OutputQueue::maxPendingRuns)
        return std::unexpected(Error::OverCapacity);
    std::size_t covered = 0;
    for (std::size_t index = 0; index < capsule.acceptedRuns.size(); ++index) {
        const auto& run = capsule.acceptedRuns[index];
        if (!run.bytes || run.bytes > capsule.acceptedPTY.size() - covered
            || (index && capsule.acceptedRuns[index - 1].owner == run.owner))
            return std::unexpected(Error::InvalidPartition);
        covered += run.bytes;
    }
    if (covered != capsule.acceptedPTY.size())
        return std::unexpected(Error::InvalidPartition);
    std::size_t remaining = limits.maxCapsuleBytes;
    const auto charge = [&](std::size_t bytes) {
        if (bytes > remaining)
            return false;
        remaining -= bytes;
        return true;
    };
    static_assert(PAL::OutputQueue::maxPendingRuns <= (std::numeric_limits<std::size_t>::max() - capsuleFixedBytes) / 16);
    if (!charge(capsuleFixedBytes) || !charge(capsule.acceptedRuns.size() * 16)
        || !charge(capsule.terminalBody.size()) || !charge(capsule.acceptedPTY.size()))
        return std::unexpected(Error::OverCapacity);
    return {};
}

/// Feeds fixed-width little-endian fields into the existing bounded hasher.
class CapsuleDigest {
public:
    void bytes(std::span<const std::uint8_t> bytes)
    {
        if (m_ok && !m_hash.update(bytes))
            m_ok = false;
    }

    void scalar(std::uint64_t value, std::size_t width)
    {
        std::array<std::uint8_t, 8> bytes;
        for (std::size_t index = 0; index < width; ++index)
            bytes[index] = static_cast<std::uint8_t>(value >> (index * 8));
        this->bytes(std::span(bytes).first(width));
    }

    void identity(Identity identity)
    {
        scalar(identity.low, 8);
        scalar(identity.high, 8);
    }

    std::expected<std::array<std::uint8_t, 32>, Error> finish()
    {
        std::array<std::uint8_t, 32> digest;
        if (!m_ok || !m_hash.finalize(digest))
            return std::unexpected(Error::DigestFailure);
        return digest;
    }

private:
    Crypto::SHA256Hasher m_hash;
    bool m_ok = true;
};

} // namespace

std::expected<std::array<std::uint8_t, 32>, Error> reclaimCapsuleSHA256(const ReclaimCapsuleView& capsule, const ReclaimTransferLimits& limits)
{
    if (auto result = admit(capsule, limits); !result)
        return std::unexpected(result.error());
    CapsuleDigest hash;
    hash.bytes(capsuleDomain);
    hash.identity(capsule.operation.operationID);
    hash.scalar(capsule.operation.daemonInstanceID, 8);
    hash.scalar(capsule.target.handle, 4);
    hash.scalar(capsule.target.sessionID, 4);
    hash.scalar(capsule.target.daemonInstanceID, 8);
    hash.identity(capsule.target.hostInstanceID);
    hash.identity(capsule.target.attachmentID);
    hash.identity(capsule.target.paneID);
    hash.identity(capsule.barrier.clientInstance);
    hash.scalar(capsule.barrier.finalInputSeq, 8);
    hash.scalar(capsule.inputWritten.has_value(), 1);
    hash.scalar(*capsule.inputWritten, 8);
    hash.scalar(capsule.terminalBody.size(), 8);
    hash.scalar(capsule.acceptedPTY.size(), 8);
    hash.scalar(capsule.acceptedRuns.size(), 2);
    for (const auto& run : capsule.acceptedRuns) {
        hash.scalar(run.owner, 8);
        hash.scalar(run.bytes, 8);
    }
    hash.bytes(capsule.terminalBody);
    hash.bytes({ reinterpret_cast<const std::uint8_t*>(capsule.acceptedPTY.data()), capsule.acceptedPTY.size() });
    return hash.finish();
}

ReclaimInstallation::ReclaimInstallation() = default;
ReclaimInstallation::~ReclaimInstallation() = default;
ReclaimInstallation::ReclaimInstallation(ReclaimInstallation&&) noexcept = default;
ReclaimInstallation& ReclaimInstallation::operator=(ReclaimInstallation&&) noexcept = default;

ReclaimTransfer::ReclaimTransfer(const Protocol::Control::ReclaimTarget& target, AdoptOperation operation,
    ReclaimInputBarrier barrier, std::array<std::uint8_t, 32> digest, std::unique_ptr<ReclaimInstallation> installation)
    : m_target(target)
    , m_barrier(barrier)
    , m_capsuleSHA256(digest)
    , m_attempt(operation)
    , m_installation(std::move(installation))
{
}

ReclaimTransfer::~ReclaimTransfer() = default;

std::expected<std::unique_ptr<ReclaimTransfer>, Error> ReclaimTransfer::prepare(
    const Protocol::Control::ReclaimTarget& target, AdoptOperation operation, ReclaimInputBarrier barrier,
    const ReclaimCapsuleView& capsule, const ReclaimTransferLimits& limits, std::shared_ptr<Protocol::Sync::MemoryBudget> decodeMemory)
{
    if (!isValid(target) || !isValid(operation) || !isValid(barrier.clientInstance)
        || operation.daemonInstanceID != target.daemonInstanceID)
        return std::unexpected(Error::InvalidIdentity);
    if (target != capsule.target || operation != capsule.operation || barrier != capsule.barrier)
        return std::unexpected(Error::MismatchedIdentity);
    auto digest = reclaimCapsuleSHA256(capsule, limits);
    if (!digest)
        return std::unexpected(digest.error());
    if (*digest != capsule.capsuleSHA256)
        return std::unexpected(Error::DigestMismatch);
    auto terminal = Protocol::Sync::decodeHandoverTerminal(capsule.terminalBody, limits.terminal, std::move(decodeMemory));
    if (!terminal)
        return std::unexpected(terminal.error() == Protocol::Sync::HandoverBodyError::AllocationRefused ? Error::NoMemory : Error::TerminalRefused);
    auto accepted = PAL::OutputQueue::Unwritten::copyOf(capsule.acceptedPTY, capsule.acceptedRuns);
    if (!accepted)
        return std::unexpected(Error::NoMemory);
    std::unique_ptr<ReclaimInstallation> installation(new (std::nothrow) ReclaimInstallation);
    if (!installation)
        return std::unexpected(Error::NoMemory);
    installation->target = target;
    installation->operation = operation;
    installation->barrier = barrier;
    installation->inputWritten = *capsule.inputWritten;
    installation->terminal = std::move(*terminal);
    installation->acceptedPTY = std::move(*accepted);
    std::unique_ptr<ReclaimTransfer> result(new (std::nothrow) ReclaimTransfer(target, operation, barrier, *digest, std::move(installation)));
    if (!result)
        return std::unexpected(Error::NoMemory);
    if (!result->m_attempt.observe({ .operation = operation, .disposition = AdoptDisposition::Prepared, .target = { .sessionID = target.sessionID, .paneID = target.paneID } }))
        return std::unexpected(Error::Conflict);
    return result;
}

std::expected<void, AdoptTransactionError> ReclaimTransfer::beginCommit()
{
    if (!m_installation)
        return std::unexpected(AdoptTransactionError::Conflict);
    return m_attempt.beginCommit();
}

std::expected<void, Error> ReclaimTransfer::observe(const ReclaimTransferEvidence& evidence)
{
    if (evidence.target != m_target || evidence.barrier != m_barrier || evidence.outcome.operation != m_attempt.operation())
        return std::unexpected(Error::MismatchedIdentity);
    if (evidence.capsuleSHA256 != m_capsuleSHA256)
        return std::unexpected(Error::DigestMismatch);
    if (!m_attempt.observe(evidence.outcome))
        return std::unexpected(Error::Conflict);
    return {};
}

ReclaimOwnership ReclaimTransfer::ownership() const
{
    if (m_attempt.isCommitted())
        return ReclaimOwnership::Local;
    return m_attempt.mayResumeLocal() ? ReclaimOwnership::Daemon : ReclaimOwnership::Unresolved;
}

std::unique_ptr<ReclaimInstallation> ReclaimTransfer::takeCommitted() noexcept
{
    return m_attempt.isCommitted() ? std::move(m_installation) : nullptr;
}

} // namespace Ubiquitty::Session
