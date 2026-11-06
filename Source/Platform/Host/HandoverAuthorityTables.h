// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/Protocol/Sync/HandoverBody.h"
#include "Core/Protocol/Sync/Scope.h"
#include "Platform/Host/HostCapabilityPolicy.h"

namespace Ubiquitty::Host {

/// An unconsumed QUIC bootstrap bearer and the exact authority that minted it.
struct HandoverQUICToken {
    std::string token;
    std::uint64_t deadlineMilliseconds = 0;
    std::uint32_t mintingClientID = 0;
    Protocol::Sync::ScopeSet scope;
    std::string tokenID;
    bool isInteractiveUser = false;
};

/// Host-wide policy and consent state, separate from per-session policy copies.
///
/// No pending question crosses: one belongs to its recipients and to its
/// process's clock, so capture waits until none is pending, and a record
/// naming one is refused as not at ground. QUIC minter IDs remain exact.
struct HandoverAuthorityTables {
    CapabilityDecisionTable decisions;
    std::vector<std::pair<CapabilityGrantKey, GrantRecord>> grants;
    std::vector<CapabilityGrantKey> pendingConsent;
    std::vector<CapabilityGrantKey> newlyPendingConsent;
    std::vector<HandoverQUICToken> quicTokens;
    std::vector<std::string> alwaysAllowedAgents;
    std::uint32_t nextConsentRequestID = 1;
    std::uint32_t nextClipboardRequestID = 0;
    bool isTokenRequired = false;
    bool didFailManagementHistory = false;
};

/// Receiver-owned resource ceilings for private authority tables.
struct HandoverAuthorityLimits {
    std::size_t maxSectionBytes = 0;
    std::size_t maxGrants = 0;
    std::size_t maxConsentEntries = 0;
    std::size_t maxQUICTokens = 0;
    std::size_t maxStandingAgents = 0;
    std::size_t maxStringBytes = 0;
};

/// Encodes frozen authority without consuming one-shot grants or bootstrap tokens.
/// - Parameters:
///   - state: Exact host-wide tables; physical clipboard operations must be absent.
///   - limits: Independent aggregate ceilings chosen by the successor.
/// - Returns: Exact section bytes or refusal without changing authority.
[[nodiscard]] std::expected<Protocol::Sync::HandoverBytes, Protocol::Sync::HandoverBodyError> encodeHandoverAuthority(
    const HandoverAuthorityTables& state, const HandoverAuthorityLimits& limits);

/// Reconstructs private authority for an unpublished candidate daemon.
/// The caller must revalidate token roots and session bindings, retain the current
/// trusted executable set, and bind its own admitted token-store path before commit.
/// - Parameters:
///   - bytes: Exactly one predecessor section from an authenticated local channel.
///   - limits: Receiver ceilings, never fields trusted from the record.
/// - Returns: Owned authority or refusal, without granting or consuming anything.
[[nodiscard]] std::expected<HandoverAuthorityTables, Protocol::Sync::HandoverBodyError> decodeHandoverAuthority(
    std::span<const std::uint8_t> bytes, const HandoverAuthorityLimits& limits);

} // namespace Ubiquitty::Host
