// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Ubiquitty::Protocol::Sync {

/// Consent's verified identity retains its existing uint16 byte bound. A
/// complete serialized Call must additionally fit the connection frame bound.
inline constexpr std::size_t maxConsentIdentityBytes = UINT16_MAX;
/// The size of a resolved-question `Notify` body: the kind's one-byte varint,
/// then `kind:u8 request:u32 session:u32`.
inline constexpr std::size_t maxConsentResolutionBytes = 1 + 1 + 4 + 4;

/// The operation whose exact retained question a human is deciding.
enum class ConsentKind : std::uint8_t {
    Attach = 0,
    Terminate = 1,
    Capability = 2,
    Structure = 3,
};

/// The human's lifetime choice; per-use policy can narrow a requested lifetime.
enum class ConsentChoice : std::uint8_t {
    Deny = 0,
    AllowOnce = 1,
    AllowSession = 2,
    AllowAlways = 3,
    /// Not an answer: the recipient could not show the question to anyone. It
    /// stays with the others it was asked of, and is refused only once no
    /// recipient is left to show it.
    Unshown = 4,
};

/// Independent ways to withdraw existing session authority.
enum class ConsentRevocationKind : std::uint8_t {
    DisconnectNow = 0,
    StopAlways = 1,
    SessionGrant = 2,
    DisconnectAll = 3,
};

/// A host-owned question. Verified process facts and a claimed label are kept
/// separate; neither the label nor a response supplies operation authority.
struct ConsentQuestion {
    std::uint32_t requestID = 0;
    std::uint32_t sessionID = 0;
    std::uint32_t peerProcessID = 0;
    std::string peerProcess;
    std::string agentLabel;
    /// The ConsentKind tag; structure questions retain their exact operation
    /// in StructureConsent rather than these capability fields.
    std::uint8_t action = 0;
    std::uint16_t capabilityCode = 0;
    std::uint8_t capabilityOrigin = 0;
    bool isRecipientsOwnRequest = false;
};

/// The one question presentation retired after an answer or authority change.
struct ConsentResolution {
    ConsentKind kind = ConsentKind::Attach;
    std::uint32_t requestID = 0;
    std::uint32_t sessionID = 0;

    friend bool operator==(const ConsentResolution&, const ConsentResolution&) = default;
};

/// Encodes one exact human question as a `ConsentQuestion` `Notify` body:
///
///     kind:u8 {Attach, Terminate, Capability} request:u32 session:u32
///     peerProcessID:u32 peerProcess:utf8 agentLabel:utf8
///     [capabilityCode:u16 capabilityOrigin:u8 isRecipientsOwnRequest:u8 when Capability]
///
/// - Parameter question: One nonzero host-owned attach, terminate or capability question.
/// - Returns: The whole body, or nothing for an invalid or oversized value.
[[nodiscard]] std::optional<std::vector<std::uint8_t>> consentQuestionNotify(const ConsentQuestion& question);

/// Reads a `ConsentQuestion` record, refusing a structure kind, text with a
/// NUL or past its bound, a process name without a process, and trailing bytes.
/// - Parameter record: A `ConsentQuestion` `Notify`'s record.
/// - Returns: A question, or nothing for a malformed record.
[[nodiscard]] std::optional<ConsentQuestion> decodeConsentQuestion(std::span<const std::uint8_t> record);

/// Encodes one retired question as a `ConsentResolved` `Notify` body:
/// `kind:u8 request:u32 session:u32`, without new authority.
/// - Parameter resolution: The actual requested question being retired.
/// - Returns: The whole body, or nothing for invalid identifiers or kind.
[[nodiscard]] std::optional<std::vector<std::uint8_t>> consentResolvedNotify(const ConsentResolution& resolution);

/// Reads a `ConsentResolved` record for any known question kind.
/// - Parameter record: A `ConsentResolved` `Notify`'s record.
/// - Returns: The exact presentation to retire, or nothing for a malformed record.
[[nodiscard]] std::optional<ConsentResolution> decodeConsentResolved(std::span<const std::uint8_t> record);

} // namespace Ubiquitty::Protocol::Sync
