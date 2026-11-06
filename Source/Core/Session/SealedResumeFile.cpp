// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Session/SealedResumeFile.h"
#include "Core/Crypto/HMAC.h"
#include "Core/Crypto/SHA256.h"

#include "PAL/BoundedFile.h"
#include "PAL/DurableFile.h"
#include "PAL/PAL.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace Ubiquitty::Session {

namespace {

std::array<std::uint8_t, 16> identityBytes(Protocol::Sync::ID128 identity)
{
    std::array<std::uint8_t, 16> bytes;
    for (std::size_t index = 0; index < 8; ++index) {
        bytes[index] = static_cast<std::uint8_t>(identity.low >> (index * 8));
        bytes[index + 8] = static_cast<std::uint8_t>(identity.high >> (index * 8));
    }
    return bytes;
}

bool isValidScope(const PAL::SealScope& scope)
{
    const auto isNonzero = [](const auto& bytes) { return std::ranges::any_of(bytes, [](auto byte) { return byte != 0; }); };
    return scope.purpose == PAL::SealScope::Purpose::Resume && isNonzero(scope.hostInstance) && isNonzero(scope.principal) && isNonzero(scope.recordID);
}

bool matches(const PAL::SealScope& scope, Protocol::Sync::ID128 host, Protocol::Sync::ID128 client)
{
    return scope.hostInstance == identityBytes(host) && scope.recordID == identityBytes(client);
}

ResumeFileError sealError(PAL::SealError error)
{
    switch (error) {
    case PAL::SealError::Unavailable:
        return ResumeFileError::Unavailable;
    case PAL::SealError::TooLarge:
        return ResumeFileError::TooLarge;
    case PAL::SealError::AllocationFailed:
        return ResumeFileError::AllocationFailed;
    case PAL::SealError::Refused:
        return ResumeFileError::AuthenticationFailed;
    }
    return ResumeFileError::AuthenticationFailed;
}

ResumeFileError recordError(ResumeRecordError error)
{
    switch (error) {
    case ResumeRecordError::TooLarge:
        return ResumeFileError::TooLarge;
    case ResumeRecordError::AllocationFailed:
        return ResumeFileError::AllocationFailed;
    case ResumeRecordError::Malformed:
    case ResumeRecordError::VersionMismatch:
        return ResumeFileError::RecordRefused;
    }
    return ResumeFileError::RecordRefused;
}

std::expected<void, ResumeFileError> writePlaintext(const char* path, const PAL::SealScope& scope, const PAL::SealedStore& sealer, std::span<const std::uint8_t> plaintext, std::size_t maxPlaintextBytes)
{
    const auto ciphertext = sealer.seal(scope, plaintext, maxPlaintextBytes);
    if (!ciphertext)
        return std::unexpected(sealError(ciphertext.error()));
    switch (PAL::storageWriteDurable(path, std::as_bytes(ciphertext->view()))) {
    case PAL::DurableFileResult::NotCommitted:
        return std::unexpected(ResumeFileError::IOFailed);
    case PAL::DurableFileResult::DurabilityUnknown:
        return std::unexpected(ResumeFileError::DurabilityUnknown);
    case PAL::DurableFileResult::Durable:
        return {};
    }
    return std::unexpected(ResumeFileError::IOFailed);
}

} // namespace

PAL::SealScope resumeSealScope(Protocol::Sync::ID128 hostInstance, std::array<std::uint8_t, 32> principal, Protocol::Sync::ID128 clientInstance)
{
    return { .hostInstance = identityBytes(hostInstance), .principal = principal, .recordID = identityBytes(clientInstance), .purpose = PAL::SealScope::Purpose::Resume };
}

std::expected<void, ResumeFileError> writeSealedResumeFile(const char* path, const PAL::SealScope& scope, const PAL::SealedStore& sealer, const Protocol::Sync::ResumeRecord& record, std::size_t maxPlaintextBytes)
{
    if (!isValidScope(scope) || !matches(scope, record.hostInstanceID, record.clientInstance))
        return std::unexpected(ResumeFileError::IdentityMismatch);
    if (!path || !*path)
        return std::unexpected(ResumeFileError::IOFailed);
    const auto plaintext = encodeResumeRecord(record, maxPlaintextBytes);
    if (!plaintext)
        return std::unexpected(recordError(plaintext.error()));
    return writePlaintext(path, scope, sealer, plaintext->view(), maxPlaintextBytes);
}

std::expected<OpenedResumeRecord, ResumeFileError> readSealedResumeFile(const char* path, const PAL::SealScope& scope, const PAL::SealedStore& sealer, std::size_t maxPlaintextBytes)
{
    if (!isValidScope(scope))
        return std::unexpected(ResumeFileError::IdentityMismatch);
    if (maxPlaintextBytes > std::numeric_limits<std::size_t>::max() - PAL::SealedStore::overhead)
        return std::unexpected(ResumeFileError::TooLarge);
    const auto ciphertext = PAL::storageReadBounded(path, maxPlaintextBytes + PAL::SealedStore::overhead);
    if (!ciphertext) {
        switch (ciphertext.error()) {
        case PAL::FileReadError::Unavailable:
            return std::unexpected(ResumeFileError::IOFailed);
        case PAL::FileReadError::TooLarge:
            return std::unexpected(ResumeFileError::TooLarge);
        case PAL::FileReadError::AllocationFailed:
            return std::unexpected(ResumeFileError::AllocationFailed);
        case PAL::FileReadError::Changed:
            return std::unexpected(ResumeFileError::FileChanged);
        }
    }
    auto plaintext = sealer.open(scope, ciphertext->view(), maxPlaintextBytes);
    if (!plaintext)
        return std::unexpected(sealError(plaintext.error()));
    const auto record = decodeResumeRecord(plaintext->view(), maxPlaintextBytes);
    if (!record)
        return std::unexpected(recordError(record.error()));
    if (!matches(scope, record->hostInstanceID, record->clientInstance))
        return std::unexpected(ResumeFileError::IdentityMismatch);
    return OpenedResumeRecord { std::move(*plaintext), *record };
}

bool forgetSealedResumeFile(const char* path, const PAL::SealScope& scope, const PAL::SealedStore& sealer)
{
    if (!isValidScope(scope))
        return false;
    const bool isAbsent = sealer.forget(scope);
    PAL::storageRemoveFile(path);
    return isAbsent;
}

SealedResumeRecordStore::SealedResumeRecordStore(std::string path, PAL::SealScope scope, std::array<std::uint8_t, 32> certificatePin, PAL::SealedStore sealer, PAL::RestoreAuthorization authorization, std::size_t maxPlaintextBytes, std::size_t maxMaterializedBytes)
    : m_path(std::move(path))
    , m_scope(scope)
    , m_certificatePin(certificatePin)
    , m_sealer(sealer)
    , m_authorization(authorization)
    , m_maxPlaintextBytes(maxPlaintextBytes)
    , m_maxMaterializedBytes(maxMaterializedBytes)
{
}

Protocol::Sync::ResumeBindingOutcome SealedResumeRecordStore::bindWelcome(const Protocol::Sync::Welcome& welcome, Protocol::Sync::ID128 clientInstance, const std::array<std::uint8_t, 32>& certificatePin)
{
    std::array<std::uint8_t, 32> principal {};
    Crypto::sha256({}, { reinterpret_cast<const std::uint8_t*>(welcome.principal.data()), welcome.principal.size() }, principal);
    if (welcome.principal.empty() || !isValidScope(m_scope)
        || m_scope.recordID != identityBytes(clientInstance)
        || m_scope.principal != principal || m_certificatePin != certificatePin)
        return Protocol::Sync::ResumeBindingOutcome::Refused;
    if (m_scope.hostInstance == identityBytes(welcome.hostInstanceID))
        return Protocol::Sync::ResumeBindingOutcome::Bound;
    if (welcome.hostInstanceID == Protocol::Sync::ID128 {} || !welcome.predecessorHostInstanceID
        || m_scope.hostInstance != identityBytes(*welcome.predecessorHostInstanceID))
        return Protocol::Sync::ResumeBindingOutcome::Refused;

    auto successor = m_scope;
    successor.hostInstance = identityBytes(welcome.hostInstanceID);
    // Neither scope may expose an old receipt after the host consumes its
    // secret. Record both deny intents before retiring the predecessor; a
    // crash before the successor's durable write leaves no readable record.
    if (!m_authorization.block(successor) || purge() != Protocol::Sync::ResumePurgeOutcome::Purged)
        return Protocol::Sync::ResumeBindingOutcome::Refused;
    m_scope = successor;
    m_hasPendingPurge = true;
    return Protocol::Sync::ResumeBindingOutcome::Bound;
}

Protocol::Sync::ResumeWriteOutcome SealedResumeRecordStore::write(const Protocol::Sync::ResumeRecord& record)
{
    using namespace Protocol::Sync;
    if (!isValidScope(m_scope) || !matches(m_scope, record.hostInstanceID, record.clientInstance)
        || record.certificatePin != m_certificatePin)
        return ResumeWriteOutcome::Unchanged;
    const auto plaintext = encodeResumeRecord(record, m_maxPlaintextBytes);
    if (!plaintext)
        return ResumeWriteOutcome::Unchanged;
    const auto decoded = decodeResumeRecord(plaintext->view(), m_maxPlaintextBytes);
    if (!decoded || decoded->materializedBytes > m_maxMaterializedBytes)
        return ResumeWriteOutcome::Unchanged;
    const auto authority = m_authorization.read(m_scope);
    if (authority.state == PAL::RestoreAuthorization::State::Unavailable)
        return ResumeWriteOutcome::Unchanged;
    if ((m_hasPendingPurge || authority.state == PAL::RestoreAuthorization::State::Blocked)
        && purge() != ResumePurgeOutcome::Purged)
        return ResumeWriteOutcome::Unchanged;
    // A crash from here through activation must never expose the old receipt
    // or an uncertain new one. Refusal to record intent precedes all mutation.
    if (!m_authorization.block(m_scope))
        return ResumeWriteOutcome::Unchanged;
    m_hasPendingPurge = true;
    const auto result = writePlaintext(m_path.c_str(), m_scope, m_sealer, plaintext->view(), m_maxPlaintextBytes);
    if (result) {
        std::array<std::uint8_t, 32> digest;
        Crypto::sha256(plaintext->view(), digest);
        if (!m_authorization.activate(m_scope, digest))
            return ResumeWriteOutcome::DurabilityUnknown;
        m_hasPendingPurge = false;
        return ResumeWriteOutcome::Committed;
    }
    if (result.error() == ResumeFileError::DurabilityUnknown) {
        m_hasPendingPurge = true;
        return ResumeWriteOutcome::DurabilityUnknown;
    }
    return ResumeWriteOutcome::Unchanged;
}

std::optional<Protocol::Sync::ResumeRecord> SealedResumeRecordStore::read()
{
    if (m_hasPendingPurge)
        return std::nullopt;
    const auto authority = m_authorization.read(m_scope);
    if (authority.state != PAL::RestoreAuthorization::State::Active)
        return std::nullopt;
    const auto opened = readSealedResumeFile(m_path.c_str(), m_scope, m_sealer, m_maxPlaintextBytes);
    if (!opened || opened->record.certificatePin != m_certificatePin)
        return std::nullopt;
    std::array<std::uint8_t, 32> digest;
    Crypto::sha256(opened->plaintext.view(), digest);
    if (!Crypto::constantTimeEqual(digest, authority.digest))
        return std::nullopt;
    auto record = materializeResumeRecord(opened->plaintext.view(), m_maxPlaintextBytes, m_maxMaterializedBytes);
    if (!record)
        return std::nullopt;
    return std::move(*record);
}

Protocol::Sync::ResumePurgeOutcome SealedResumeRecordStore::purge()
{
    m_hasPendingPurge = true;
    if (!isValidScope(m_scope) || !m_authorization.block(m_scope))
        return Protocol::Sync::ResumePurgeOutcome::Refused;
    m_hasPendingPurge = !forgetSealedResumeFile(m_path.c_str(), m_scope, m_sealer);
    return m_hasPendingPurge ? Protocol::Sync::ResumePurgeOutcome::Refused : Protocol::Sync::ResumePurgeOutcome::Purged;
}

} // namespace Ubiquitty::Session
