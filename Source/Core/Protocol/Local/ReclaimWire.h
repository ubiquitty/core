// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// These records select an exact reverse transfer, never authenticate it. The
// operation-5 transport must independently verify the original physical owner
// and its retained Committed adoption. No reclaim record carries a descriptor.
// Records start with kind:u8, bodyBytes:u32, then fixed little-endian fields.
// Retained outcome/refusal bodies embed the existing canonical AdoptWire record.

#include "Core/Protocol/Control/Reclaim.h"
#include "Core/Protocol/Local/AdoptWire.h"
#include "Core/Protocol/Local/Control.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>

namespace Ubiquitty::Protocol::Local {

enum class ReclaimWireMessage : std::uint8_t {
    Prepare = 1,
    Commit = 2,
    Abort = 3,
    Status = 4,
    Capsule = 0x81,
    Outcome = 0x82,
    Refused = 0x83,
};

/// Final numbering frozen by the exact requesting attachment, not authority.
struct ReclaimWireBarrier {
    Sync::ID128 clientInstance;
    std::uint64_t finalInputSeq = 0;
    bool operator==(const ReclaimWireBarrier&) const = default;
};

/// Original adoption proof and new transaction identity are distinct selectors.
/// Their bytes confer no authority; both operations pin the same daemon.
struct ReclaimWireSelector {
    AdoptWireOperation adoption;
    AdoptWireOperation operation;
    Control::ReclaimTarget target;
    ReclaimWireBarrier barrier;
    bool operator==(const ReclaimWireSelector&) const = default;
};

/// Prepare has no digest yet; Commit requires one. An absent Status/Abort digest
/// is solely a lost-Prepare recovery selector, never evidence of released state.
struct ReclaimWireCommand {
    ReclaimWireMessage kind = ReclaimWireMessage::Status;
    std::uint64_t correlation = 0;
    ReclaimWireSelector selector;
    std::optional<std::array<std::uint8_t, 32>> capsuleSHA256 {};
    bool operator==(const ReclaimWireCommand&) const = default;
};

/// Queue-local ownership partition, not attachment identity or authorization.
struct ReclaimWireRun {
    std::uint64_t owner = 0;
    std::uint64_t bytes = 0;
    bool operator==(const ReclaimWireRun&) const = default;
};

/// Borrows a complete immutable offer. A missing receipt is not a zero receipt;
/// the installation layer separately admits its exact queue-acceptance barrier.
struct ReclaimWireCapsule {
    ReclaimWireSelector selector;
    AdoptWireOutcome prepared;
    std::array<std::uint8_t, 32> capsuleSHA256 {};
    std::optional<std::uint64_t> inputWritten;
    std::span<const ReclaimWireRun> acceptedRuns;
    std::span<const std::uint8_t> terminalBody;
    std::span<const std::uint8_t> acceptedPTY;
};

/// Unknown/negative evidence may lack a digest only if no capsule was prepared.
/// Prepared and Committed always bind the retained actual capsule digest.
struct ReclaimWireOutcome {
    ReclaimWireSelector selector;
    std::optional<std::array<std::uint8_t, 32>> capsuleSHA256;
    AdoptWireOutcome outcome {};
    bool operator==(const ReclaimWireOutcome&) const = default;
};

/// Admission refusal remains distinct from every retained transaction outcome.
struct ReclaimWireRefused {
    ReclaimWireSelector selector;
    std::optional<std::array<std::uint8_t, 32>> capsuleSHA256;
    AdoptWireRefused refusal {};
    bool operator==(const ReclaimWireRefused&) const = default;
};

/// Wire-work bounds, independent of complete-terminal installation admission.
struct ReclaimWireLimits {
    std::size_t maxRecordBytes = 0;
    std::size_t maxTerminalBytes = 0;
    std::size_t maxAcceptedPTYBytes = 0;
    std::size_t maxRuns = 0;
};

enum class ReclaimWireReadyStatus : std::uint8_t {
    Ready,
    Refused,
};

struct ReclaimWireReady {
    std::uint64_t correlation = 0;
    std::uint64_t daemonInstanceID = 0;
    ReclaimWireReadyStatus status = ReclaimWireReadyStatus::Refused;
    bool operator==(const ReclaimWireReady&) const = default;
};

inline constexpr std::size_t reclaimWireFrameHeaderBytes = 5;
inline constexpr std::size_t reclaimWireSelectorBytes = 24 + 24 + 64 + 24;
inline constexpr std::size_t reclaimWireOptionalDigestBytes = 1 + 32;
inline constexpr std::size_t reclaimWireCommandBytes = reclaimWireFrameHeaderBytes + 8 + reclaimWireSelectorBytes + reclaimWireOptionalDigestBytes;
inline constexpr std::size_t reclaimWireOutcomeBytes = reclaimWireFrameHeaderBytes + reclaimWireSelectorBytes + reclaimWireOptionalDigestBytes + adoptWireOutcomeBytes;
inline constexpr std::size_t reclaimWireRefusedBytes = reclaimWireFrameHeaderBytes + reclaimWireSelectorBytes + reclaimWireOptionalDigestBytes + adoptWireRefusedBytes;
inline constexpr std::size_t reclaimWireCapsuleHeaderBytes = reclaimWireFrameHeaderBytes + reclaimWireSelectorBytes + 32 + 1 + 8 + 8 + 8 + 2 + adoptWireOutcomeBytes;
inline constexpr std::size_t reclaimWireMaxRuns = 128;
inline constexpr std::size_t reclaimWireMaxCapsulePrefixBytes = reclaimWireCapsuleHeaderBytes + reclaimWireMaxRuns * 16;

struct ReclaimWireHeader {
    ReclaimWireMessage kind = ReclaimWireMessage::Status;
    std::uint32_t bodyBytes = 0;
    bool operator==(const ReclaimWireHeader&) const = default;
};

/// Owns only metadata. The two body owners must prevent mutation through every
/// alias and outlive the complete send. Send prefix, terminal, suffix in order.
struct EncodedReclaimWireCapsule {
    std::array<std::uint8_t, reclaimWireMaxCapsulePrefixBytes> bytes {};
    std::size_t length = 0;
    std::span<const std::uint8_t> terminalBody;
    std::span<const std::uint8_t> acceptedPTY;

    [[nodiscard]] std::span<const std::uint8_t> prefix() const { return std::span(bytes).first(length); }
};

/// Readiness pins a selected operation-5 channel; it does not grant release.
[[nodiscard]] std::expected<std::array<std::uint8_t, localControlReplyBytes>, AdoptWireError> encodeLocalReclaimReady(const ReclaimWireReady&);
[[nodiscard]] std::expected<ReclaimWireReady, AdoptWireError> decodeLocalReclaimReady(std::span<const std::uint8_t>);

/// Validates a complete selector without consulting any physical authority.
[[nodiscard]] bool isValidReclaimWireSelector(const ReclaimWireSelector&);
/// Reads exactly five framing bytes before any body allocation or hash work.
[[nodiscard]] std::expected<ReclaimWireHeader, AdoptWireError> decodeReclaimWireHeader(std::span<const std::uint8_t>, std::size_t maxRecordBytes);

[[nodiscard]] std::expected<std::array<std::uint8_t, reclaimWireCommandBytes>, AdoptWireError> encodeReclaimWireCommand(const ReclaimWireCommand&);
[[nodiscard]] std::expected<ReclaimWireCommand, AdoptWireError> decodeReclaimWireCommand(std::span<const std::uint8_t>);

/// Structural validation does not decode a Terminal or grant its memory budget.
/// The Session adapter additionally verifies the existing canonical capsule hash.
[[nodiscard]] std::expected<EncodedReclaimWireCapsule, AdoptWireError> encodeReclaimWireCapsule(const ReclaimWireCapsule&, const ReclaimWireLimits&);
/// Refuses bounds, lengths and partitions before publishing any run scratch.
/// The returned runs borrow runStorage; both storage and record bytes must
/// outlive every use of the returned view. No partial result escapes on failure.
[[nodiscard]] std::expected<ReclaimWireCapsule, AdoptWireError> decodeReclaimWireCapsule(std::span<const std::uint8_t>, const ReclaimWireLimits&, std::span<ReclaimWireRun> runStorage);

[[nodiscard]] std::expected<std::array<std::uint8_t, reclaimWireOutcomeBytes>, AdoptWireError> encodeReclaimWireOutcome(const ReclaimWireOutcome&);
[[nodiscard]] std::expected<ReclaimWireOutcome, AdoptWireError> decodeReclaimWireOutcome(std::span<const std::uint8_t>);
[[nodiscard]] std::expected<std::array<std::uint8_t, reclaimWireRefusedBytes>, AdoptWireError> encodeReclaimWireRefused(const ReclaimWireRefused&);
[[nodiscard]] std::expected<ReclaimWireRefused, AdoptWireError> decodeReclaimWireRefused(std::span<const std::uint8_t>);

} // namespace Ubiquitty::Protocol::Local
