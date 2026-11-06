// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/Protocol/Sync/Attachment.h"
#include "Core/Protocol/Sync/Scope.h"

#include <cstdint>
#include <string>
#include <vector>

namespace Ubiquitty::Host {

/// Verified token authority retained with one attachment, not a new credential.
/// Socket grants retain revocation identity without acquiring proof eligibility.
struct AttachmentGrant {
    Protocol::Sync::ID128 attachmentID;
    Protocol::Sync::ScopeSet scope;
    std::string tokenID;
    std::uint64_t tokenExpiryUnix = 0;
    bool hasBootstrapToken = false;
    bool allowsProof = false;
    bool isInteractiveUser = false;
};

/// The two independent identities of an authenticated host process.
/// The carrier pin is not a truncation or encoding of the full secret issuer.
struct AttachmentHostIdentity {
    Protocol::Sync::ID128 issuer;
    std::uint64_t daemonInstanceID = 0;
};

/// Owned in-memory authority from a frozen, authenticated predecessor.
/// No listening socket, active client, token-store contents or PTY is transferred.
struct AttachmentHandover {
    Protocol::Sync::AttachmentTable::Continuation attachments;
    std::vector<AttachmentGrant> grants;
    /// Oldest ancestor first, with the exporting host last.
    std::vector<AttachmentHostIdentity> hostIdentities;
};

/// Privately admitted authority, still unreachable from the serving daemon.
/// Its table uses the destination's stable secret source, but restoring it never
/// changes the destination's active table or consumes a retained resume secret.
struct PreparedAttachmentAuthority {
    std::unique_ptr<Protocol::Sync::AttachmentTable> attachments;
    std::vector<AttachmentGrant> grants;
    std::vector<AttachmentHostIdentity> hostIdentities;
};

/// Why carried authority cannot be installed before serving successor clients.
enum class AttachmentHandoverRefusal : std::uint8_t {
    DestinationInUse,
    InvalidTable,
    MissingGrant,
    InvalidGrant,
    ExpiredGrant,
    RevokedGrant,
};

} // namespace Ubiquitty::Host
