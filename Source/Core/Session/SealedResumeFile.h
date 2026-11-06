// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/Session/ResumeRecordFormat.h"
#include "PAL/RestoreAuthorization.h"
#include "PAL/SealedStore.h"

#include <string>

namespace Ubiquitty::Session {

/// A storage refusal, without a partial record or plaintext fallback.
enum class ResumeFileError {
    Unavailable,
    TooLarge,
    AllocationFailed,
    FileChanged,
    IOFailed,
    DurabilityUnknown,
    AuthenticationFailed,
    RecordRefused,
    IdentityMismatch,
};

/// Owns authenticated plaintext and its borrowed, framed record view. Moving
/// the owner preserves every span; destroying it invalidates them. Pictures
/// still require semantic checkpoint validation before terminal mutation.
struct OpenedResumeRecord {
    PAL::SealedStoreBytes plaintext;
    ResumeRecordView record;
};

/// Builds the resume domain from verified identities, independent of filenames.
///
/// - Parameters:
///   - hostInstance: The verified host incarnation.
///   - principal: The digest of the authenticated principal.
///   - clientInstance: The stable identity of this client window.
/// - Returns: The exact scope required by the file reader and writer.
[[nodiscard]] PAL::SealScope resumeSealScope(Protocol::Sync::ID128 hostInstance, std::array<std::uint8_t, 32> principal, Protocol::Sync::ID128 clientInstance);

/// Validates and seals before writing any file. Durable replacement stores only
/// the ciphertext envelope; seal or encode refusal leaves an old file intact.
/// The caller serializes access to this path and its exact key scope.
/// The parent directory must already exist durably. DurabilityUnknown means
/// replacement happened but its directory flush failed; input must stay held.
///
/// - Parameters:
///   - path: A trusted application-owned destination, never a peer path.
///   - scope: Verified host/principal/window identity with purpose Resume.
///   - sealer: Explicit platform cryptography; an unavailable driver refuses.
///   - record: The record whose host and client identities must match scope.
///   - maxPlaintextBytes: The complete plaintext budget, including metadata.
/// - Returns: Success only after durable ciphertext replacement, or a refusal.
[[nodiscard]] std::expected<void, ResumeFileError> writeSealedResumeFile(const char* path, const PAL::SealScope& scope, const PAL::SealedStore& sealer, const Protocol::Sync::ResumeRecord& record, std::size_t maxPlaintextBytes);

/// Reads within the envelope budget, authenticates, then validates the record.
/// No attachment, terminal, or input queue is created or changed by this call.
///
/// - Parameters:
///   - path: The trusted application-owned file to read.
///   - scope: The verified identity expected independently of file contents.
///   - sealer: Explicit platform cryptography, with no key creation on open.
///   - maxPlaintextBytes: The complete plaintext budget.
/// - Returns: An owner and view bound to scope, or a refusal without plaintext.
[[nodiscard]] std::expected<OpenedResumeRecord, ResumeFileError> readSealedResumeFile(const char* path, const PAL::SealScope& scope, const PAL::SealedStore& sealer, std::size_t maxPlaintextBytes);

/// Retires this exact key and removes its file best-effort, even when key
/// retirement is refused. Callers must retain the failure for a later retry;
/// removing a file alone is not cryptographic erasure of copied ciphertext.
///
/// - Parameters:
///   - path: The trusted file to remove.
///   - scope: The exact verified resume key to retire.
///   - sealer: Platform key owner.
/// - Returns: Whether the platform confirms this key is absent, not file removal.
[[nodiscard]] bool forgetSealedResumeFile(const char* path, const PAL::SealScope& scope, const PAL::SealedStore& sealer);

/// A synchronous resume store bound to a verified scope and certificate pin.
/// The attachment must be destroyed before this owner. Its path is supplied by
/// the application after creating the private parent durably, not by the peer.
/// No constructor operation reads a file or creates a key.
/// Authorization and encryption use independent protected records. A missing
/// authorization never permits cold reads. Every file/key mutation first
/// requires a persistent blocked intent; a failed intent write mutates neither.
/// Callers serialize this entire transaction across every owner of the scope.
class SealedResumeRecordStore final : public Protocol::Sync::ResumeRecordStore {
public:
    /// Captures principal, client, certificate authority and storage bounds.
    ///
    /// - Parameters:
    ///   - path: An application-owned private absolute destination.
    ///   - scope: Independently verified host/principal/client identity.
    ///   - certificatePin: The pin verified by the transport, not record data.
    ///   - sealer: The explicit platform driver, with no fallback.
    ///   - authorization: Protected restart-safe read authorization and retirement intent.
    ///   - maxPlaintextBytes: The encoded record budget.
    ///   - maxMaterializedBytes: The complete decoded record storage budget.
    SealedResumeRecordStore(std::string path, PAL::SealScope scope, std::array<std::uint8_t, 32> certificatePin, PAL::SealedStore sealer, PAL::RestoreAuthorization authorization, std::size_t maxPlaintextBytes, std::size_t maxMaterializedBytes);

    /// Verifies the current authenticated peer against this scope. An exact
    /// predecessor permits changing only its host incarnation: both scopes are
    /// blocked and the old key retired before the new scope is adopted. A new
    /// record must still commit before input or cold reads are permitted.
    ///
    /// - Parameters:
    ///   - welcome: The authenticated host and principal.
    ///   - clientInstance: The stable identity this connection greeted as.
    ///   - certificatePin: The pin verified by the current transport.
    /// - Returns: Bound for the exact peer or its authenticated successor;
    ///   Refused for a scope mismatch or an unconfirmed retirement.
    [[nodiscard]] Protocol::Sync::ResumeBindingOutcome bindWelcome(const Protocol::Sync::Welcome& welcome, Protocol::Sync::ID128 clientInstance, const std::array<std::uint8_t, 32>& certificatePin) override;

    /// Seals and durably replaces only a record matching the configured peer.
    ///
    /// - Parameter record: The current authenticated attachment state.
    /// - Returns: The exact durable commit outcome; no partial success.
    [[nodiscard]] Protocol::Sync::ResumeWriteOutcome write(const Protocol::Sync::ResumeRecord& record) override;

    /// Opens, validates and materializes this scope's record without callbacks.
    ///
    /// - Returns: Independent state, or null on absence, refusal or pending purge.
    [[nodiscard]] std::optional<Protocol::Sync::ResumeRecord> read() override;

    /// Retires the exact key before this store may read or replace a record.
    ///
    /// - Returns: Purged only after blocked intent and key absence are confirmed.
    [[nodiscard]] Protocol::Sync::ResumePurgeOutcome purge() override;

private:
    std::string m_path;
    PAL::SealScope m_scope;
    std::array<std::uint8_t, 32> m_certificatePin;
    PAL::SealedStore m_sealer;
    PAL::RestoreAuthorization m_authorization;
    std::size_t m_maxPlaintextBytes;
    std::size_t m_maxMaterializedBytes;
    bool m_hasPendingPurge = false;
};

} // namespace Ubiquitty::Session
