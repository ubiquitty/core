// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/Protocol/Local/ReclaimWire.h"
#include "Core/Session/AdoptTransactionWire.h"
#include "Core/Session/ReclaimTransfer.h"

#include <array>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>

namespace Ubiquitty::Session {

/// Exact immutable selectors, not a substitute for physical owner verification.
struct ReclaimTransactionSelector {
    AdoptOperation adoption;
    AdoptOperation operation;
    Protocol::Control::ReclaimTarget target;
    ReclaimInputBarrier barrier;
    bool operator==(const ReclaimTransactionSelector&) const = default;
};

enum class ReclaimTransactionWireError : std::uint8_t {
    InvalidIdentity,
    InvalidValue,
    MismatchedSelector,
    MismatchedCorrelation,
    MissingDigest,
    DigestMismatch,
    OverCapacity,
    InvalidCapsule,
};

[[nodiscard]] std::expected<Protocol::Local::ReclaimWireSelector, ReclaimTransactionWireError> reclaimSelectorToWire(const ReclaimTransactionSelector&);
[[nodiscard]] std::expected<ReclaimTransactionSelector, ReclaimTransactionWireError> reclaimSelectorFromWire(const Protocol::Local::ReclaimWireSelector&);

/// Verifies the existing canonical capsule hash, without decoding a Terminal or
/// changing any live owner. Run storage is published only after full admission;
/// it and the capsule's immutable body owners must outlive the returned borrow.
[[nodiscard]] std::expected<Protocol::Local::ReclaimWireCapsule, ReclaimTransactionWireError> reclaimCapsuleToWire(
    const AdoptOperation& adoption, std::uint64_t correlation, const ReclaimCapsuleView&, const ReclaimTransferLimits&,
    std::span<Protocol::Local::ReclaimWireRun> runStorage);

/// Imports only an exact correlated Prepared offer with a matching full capsule
/// hash. Missing written evidence is never synthesized as a zero receipt. The
/// caller separately materializes ReclaimTransfer before any possible Commit.
/// Both runStorage and the original wire-body owners must outlive the view.
[[nodiscard]] std::expected<ReclaimCapsuleView, ReclaimTransactionWireError> reclaimCapsuleFromWire(
    const Protocol::Local::ReclaimWireCapsule&, const ReclaimTransactionSelector& expectedSelector,
    std::uint64_t expectedCorrelation, const ReclaimTransferLimits&, std::span<PAL::OutputQueue::Run> runStorage);

[[nodiscard]] std::expected<Protocol::Local::ReclaimWireOutcome, ReclaimTransactionWireError> reclaimOutcomeToWire(
    const AdoptOperation& adoption, const ReclaimTransferEvidence&, std::uint64_t correlation);

/// Requires the already retained actual capsule digest, even for Unknown or
/// negative evidence. A digest-free lost-Prepare response is never imported as
/// release evidence. The existing transaction gate decides direction ownership.
[[nodiscard]] std::expected<ReclaimTransferEvidence, ReclaimTransactionWireError> reclaimEvidenceFromWire(
    const Protocol::Local::ReclaimWireOutcome&, const ReclaimTransactionSelector& expectedSelector,
    std::uint64_t expectedCorrelation, const std::array<std::uint8_t, 32>& expectedDigest);

/// Refusal conversion never creates a retained outcome or changes ownership.
[[nodiscard]] std::expected<Protocol::Local::ReclaimWireRefused, ReclaimTransactionWireError> reclaimRefusedToWire(
    const ReclaimTransactionSelector&, AdoptTransactionError, std::uint64_t correlation,
    const std::optional<std::array<std::uint8_t, 32>>& digest);
[[nodiscard]] std::expected<AdoptTransactionError, ReclaimTransactionWireError> reclaimRefusedFromWire(
    const Protocol::Local::ReclaimWireRefused&, const ReclaimTransactionSelector& expectedSelector,
    std::uint64_t expectedCorrelation, const std::optional<std::array<std::uint8_t, 32>>& expectedDigest);

} // namespace Ubiquitty::Session
