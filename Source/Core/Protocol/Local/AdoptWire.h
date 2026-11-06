// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// These records carry adoption intent and retained outcomes, never authority.
// The selected local transport establishes the physical peer and descriptor
// provenance; the session transaction owns candidate admission and activation.
// Every record starts with kind:u8 and bodyBytes:u32, little-endian. Its common
// body is correlation:u64, operationID:(low:u64,high:u64), daemonInstanceID:u64.
// Prepare adds terminalBytes:u64, SHA256:[32], and the complete Terminal bytes.
// Outcome adds disposition:u8, session:u32, pane:(low:u64,high:u64), rejection:u8.
// Refused adds reason:u8; Commit, Abort, and Status have no further fields.

#include "Core/Protocol/Sync/Messages.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>

namespace Ubiquitty::Protocol::Local {

/// Names exact records on the separately selected local adoption exchange.
enum class AdoptWireMessage : std::uint8_t {
    Prepare = 1,
    Commit = 2,
    Abort = 3,
    Status = 4,
    Outcome = 0x81,
    Refused = 0x82,
};

/// Pins an independently random operation to one serving daemon incarnation.
/// The caller mints the complete 128-bit ID; a correlation number is not an ID.
struct AdoptWireOperation {
    Sync::ID128 operationID;
    std::uint64_t daemonInstanceID = 0;
    bool operator==(const AdoptWireOperation&) const = default;
};

/// Binds duplicate Prepare to the same whole-Terminal bytes, without parsing them.
struct AdoptWirePreparation {
    AdoptWireOperation operation;
    std::uint64_t bodyBytes = 0;
    std::array<std::uint8_t, 32> bodySHA256 {};
    bool operator==(const AdoptWirePreparation&) const = default;
};

/// Names the session and pane reserved by an inactive adoption candidate.
struct AdoptWireTarget {
    std::uint32_t sessionID = 0;
    Sync::ID128 paneID {};
    bool operator==(const AdoptWireTarget&) const = default;
};

/// Reports retained transaction evidence, independently of carrier success.
enum class AdoptWireDisposition : std::uint8_t {
    Unknown = 0,
    Prepared = 1,
    Committed = 2,
    Aborted = 3,
    Expired = 4,
    Rejected = 5,
};

/// Explains a retained Rejected result; zero on the wire means no rejection.
enum class AdoptWireRejection : std::uint8_t {
    InvalidPayload = 1,
    NotOwner = 2,
    NoMemory = 3,
    InvalidCandidate = 4,
    HostFull = 5,
};

/// Reports admission refusal, not a retained outcome or permission to resume.
/// Domain conversion must explicitly handle every enumerator in both types.
enum class AdoptWireRefusal : std::uint8_t {
    InvalidIdentity = 1,
    InvalidPolicy = 2,
    NoMemory = 3,
    Busy = 4,
    StaleAuthority = 5,
    Unauthorised = 6,
    Conflict = 7,
    Reentrant = 8,
    ReplyUnavailable = 9,
    InvalidTime = 10,
    GenerationExhausted = 11,
    ActiveCandidate = 12,
    RetirementUnproven = 13,
};

/// Borrows the complete immutable Terminal body until decoding or sending ends.
/// No checkpoint-only interpretation, truncation, allocation, or restoration occurs.
struct AdoptWirePrepare {
    std::uint64_t correlation = 0;
    AdoptWirePreparation preparation;
    std::span<const std::uint8_t> terminalBody;
};

/// Carries Commit, Abort, or Status for one exact operation, without a payload.
struct AdoptWireCommand {
    AdoptWireMessage kind = AdoptWireMessage::Status;
    std::uint64_t correlation = 0;
    AdoptWireOperation operation;
    bool operator==(const AdoptWireCommand&) const = default;
};

/// Carries the exact retained result associated with one request correlation.
/// Target continuity against previous replies belongs to the client transaction.
struct AdoptWireOutcome {
    std::uint64_t correlation = 0;
    AdoptWireOperation operation;
    AdoptWireDisposition disposition = AdoptWireDisposition::Unknown;
    AdoptWireTarget target;
    std::optional<AdoptWireRejection> rejection {};
    bool operator==(const AdoptWireOutcome&) const = default;
};

/// A correlated nonterminal refusal that cannot be observed as an Outcome.
struct AdoptWireRefused {
    std::uint64_t correlation = 0;
    AdoptWireOperation operation;
    AdoptWireRefusal reason = AdoptWireRefusal::InvalidIdentity;
    bool operator==(const AdoptWireRefused&) const = default;
};

/// Why one exact record could not be encoded or decoded.
enum class AdoptWireError : std::uint8_t {
    Malformed,
    TooLong,
    InvalidIdentity,
    InvalidPayload,
    DigestMismatch,
};

inline constexpr std::size_t adoptWireFrameHeaderBytes = 5;
inline constexpr std::size_t adoptWireCommandBytes = adoptWireFrameHeaderBytes + 32;
inline constexpr std::size_t adoptWirePrepareHeaderBytes = adoptWireCommandBytes + 8 + 32;
inline constexpr std::size_t adoptWireOutcomeBytes = adoptWireCommandBytes + 1 + 4 + 16 + 1;
inline constexpr std::size_t adoptWireRefusedBytes = adoptWireCommandBytes + 1;

/// Contains validated framing, not permission to allocate or accept a descriptor.
struct AdoptWireHeader {
    AdoptWireMessage kind = AdoptWireMessage::Status;
    std::uint32_t bodyBytes = 0;
    bool operator==(const AdoptWireHeader&) const = default;
};

/// Owns only the prefix; the body owner prevents mutation through every alias
/// and retains those exact bytes until the complete transport send finishes.
/// Send `header` followed by `terminalBody` as one record, never the prefix alone.
struct EncodedAdoptWirePrepare {
    std::array<std::uint8_t, adoptWirePrepareHeaderBytes> header {};
    std::span<const std::uint8_t> terminalBody;
};

/// Validates exactly five framing bytes against an explicit whole-record allowance.
/// The allowance includes the frame, metadata, and body. It is a wire-work bound,
/// not decoded Terminal admission; the transport must separately reserve ownership.
/// - Parameters:
///   - bytes: Exactly the framing header, excluding its body.
///   - maxRecordBytes: The caller's explicit allowance, with no shipping default.
/// - Returns: Valid framing, or refusal before any allocation or payload hash work.
[[nodiscard]] std::expected<AdoptWireHeader, AdoptWireError> decodeAdoptWireHeader(std::span<const std::uint8_t> bytes, std::size_t maxRecordBytes);

/// Encodes a complete Prepare prefix only after length, identity, and digest validation.
/// - Parameters:
///   - prepare: Exact metadata and the borrowed immutable whole-Terminal bytes.
///   - maxRecordBytes: Explicit allowance for the complete prefix and payload.
/// - Returns: A fixed prefix and the same borrowed body, or refusal with no output.
[[nodiscard]] std::expected<EncodedAdoptWirePrepare, AdoptWireError> encodeAdoptWirePrepare(const AdoptWirePrepare& prepare, std::size_t maxRecordBytes);

/// Borrows a whole Prepare body after bounds and exact digest validation.
/// - Parameters:
///   - bytes: Exactly one record; its owner must outlive the returned borrow.
///   - maxRecordBytes: Explicit allowance for the complete prefix and payload.
/// - Returns: Complete metadata and a borrowed Terminal body, or no partial result.
[[nodiscard]] std::expected<AdoptWirePrepare, AdoptWireError> decodeAdoptWirePrepare(std::span<const std::uint8_t> bytes, std::size_t maxRecordBytes);

/// Encodes one exact payload-free command without allocation or partial output.
[[nodiscard]] std::expected<std::array<std::uint8_t, adoptWireCommandBytes>, AdoptWireError> encodeAdoptWireCommand(const AdoptWireCommand&);
/// Decodes one exact Commit, Abort, or Status, refusing trailing bytes and bad IDs.
[[nodiscard]] std::expected<AdoptWireCommand, AdoptWireError> decodeAdoptWireCommand(std::span<const std::uint8_t>);

/// Encodes only a valid complete retained-outcome shape, without allocation.
[[nodiscard]] std::expected<std::array<std::uint8_t, adoptWireOutcomeBytes>, AdoptWireError> encodeAdoptWireOutcome(const AdoptWireOutcome&);
/// Decodes a retained outcome, never coercing malformed or refused records to Unknown.
[[nodiscard]] std::expected<AdoptWireOutcome, AdoptWireError> decodeAdoptWireOutcome(std::span<const std::uint8_t>);

/// Encodes an explicit admission refusal, distinct from terminal transaction evidence.
[[nodiscard]] std::expected<std::array<std::uint8_t, adoptWireRefusedBytes>, AdoptWireError> encodeAdoptWireRefused(const AdoptWireRefused&);
/// Decodes exactly one correlated refusal, with every reason checked exhaustively.
[[nodiscard]] std::expected<AdoptWireRefused, AdoptWireError> decodeAdoptWireRefused(std::span<const std::uint8_t>);

} // namespace Ubiquitty::Protocol::Local
