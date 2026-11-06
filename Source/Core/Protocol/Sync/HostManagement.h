// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Base/Compiler.h"
#include "Core/Protocol/Sync/NativeAttachment.h"

#include <cstddef>

namespace Ubiquitty::Protocol::Sync {

/// Bounds a complete host-administration payload, including a typed response's JSON envelope.
inline constexpr std::size_t maxManagementPayload = 4 * 1024 * 1024;
/// Bounds each host-administration collection before any record is read.
inline constexpr std::size_t maxManagementEntries = 1024;
/// Bounds one host-owned administration string.
inline constexpr std::size_t maxManagementString = 4096;

/// Identifies a host administration operation.
enum class UBIQUITTY_CLOSED_ENUM ManagementOperation : std::uint8_t {
    List,
    DisconnectClient,
    RevokeGrant,
    IssueToken,
    RevokeToken,
    TerminateSession
};

/// Identifies the outcome of an administration request.
enum class UBIQUITTY_CLOSED_ENUM ManagementStatus : std::uint8_t {
    Success,
    Refused,
    NotFound,
    Invalid,
    Failed
};

/// Names an operation and the daemon incarnation authorized to perform it.
struct ManagementRequest {
    std::uint32_t requestID = 0;
    std::uint64_t daemonInstanceID = 0;
    ManagementOperation operation = ManagementOperation::List;
    std::uint32_t clientID = 0;
    std::uint32_t sessionID = 0;
    std::string identity;
    std::string label;
    ScopeSet scopes;
    std::uint64_t expiryUnix = 0;
};

/// Describes one admitted attachment without exposing its credential.
struct ManagedClient {
    std::uint32_t clientID = 0;
    std::uint32_t sessionID = 0;
    std::uint64_t attachedUnix = 0;
    std::string principal;
    std::string label;
    std::string transport;
    std::string identity;
};

/// Describes one revocable authorization decision.
struct ManagedGrant {
    std::string identifier;
    std::string capability;
    std::string origin;
    std::string identity;
    std::string lifetime;
    std::uint32_t sessionID = 0;
    std::uint64_t grantedUnix = 0;
    std::string grantedBy;
};

/// Describes a stored token without its bearer secret.
struct ManagedToken {
    std::string fingerprint;
    std::string label;
    ScopeSet scopes;
    std::uint64_t expiryUnix = 0;
    std::uint64_t createdUnix = 0;
    std::uint64_t lastUsedUnix = 0;
    std::string lastUsedBy;
};

/// Holds an authoritative snapshot or a correlated mutation result.
struct ManagementReply {
    std::uint32_t requestID = 0;
    std::uint64_t daemonInstanceID = 0;
    ManagementStatus status = ManagementStatus::Failed;
    std::string detail;
    std::vector<ManagedClient> clients;
    std::vector<ManagedGrant> grants;
    std::vector<ManagedToken> tokens;
    /// Contains a secret only in the successful reply to an explicit Issue operation.
    std::string issuedToken;
};

} // namespace Ubiquitty::Protocol::Sync
