// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Wire values and transaction values have distinct ownership and enum contracts.
// Conversion establishes neither kernel authority nor candidate admission; it
// preserves identities and refuses unrepresentable or contradictory evidence.

#include "Core/Protocol/Local/AdoptWire.h"
#include "Core/Session/AdoptTransaction.h"

#include <cstdint>
#include <expected>

namespace Ubiquitty::Session {

/// Why a value could not cross the wire/domain boundary without losing meaning.
enum class AdoptTransactionWireError : std::uint8_t {
    InvalidIdentity,
    InvalidValue,
    UnrepresentableLength,
    MismatchedOperation,
    MismatchedCorrelation,
};

/// Converts one nonzero operation and daemon pin without narrowing either ID half.
[[nodiscard]] std::expected<AdoptOperation, AdoptTransactionWireError> adoptOperationFromWire(const Protocol::Local::AdoptWireOperation&);
[[nodiscard]] std::expected<Protocol::Local::AdoptWireOperation, AdoptTransactionWireError> adoptOperationToWire(const AdoptOperation&);

/// Copies exact nonempty preparation metadata after checking length representation.
/// The Local codec must separately validate the whole borrowed body and digest;
/// this conversion neither reads payload bytes nor chooses a resource allowance.
[[nodiscard]] std::expected<AdoptPreparation, AdoptTransactionWireError> adoptPreparationFromWire(const Protocol::Local::AdoptWirePreparation&);
[[nodiscard]] std::expected<Protocol::Local::AdoptWirePreparation, AdoptTransactionWireError> adoptPreparationToWire(const AdoptPreparation&);

/// Preserves an empty sentinel or a complete session/pane pair, never a partial ID.
[[nodiscard]] std::expected<AdoptTarget, AdoptTransactionWireError> adoptTargetFromWire(const Protocol::Local::AdoptWireTarget&);
[[nodiscard]] std::expected<Protocol::Local::AdoptWireTarget, AdoptTransactionWireError> adoptTargetToWire(const AdoptTarget&);

/// Maps each disposition explicitly, refusing every unknown value.
[[nodiscard]] std::expected<AdoptDisposition, AdoptTransactionWireError> adoptDispositionFromWire(Protocol::Local::AdoptWireDisposition);
[[nodiscard]] std::expected<Protocol::Local::AdoptWireDisposition, AdoptTransactionWireError> adoptDispositionToWire(AdoptDisposition);

/// Maps each rejection explicitly; wire zero is not a rejection enumerator.
[[nodiscard]] std::expected<AdoptRejection, AdoptTransactionWireError> adoptRejectionFromWire(Protocol::Local::AdoptWireRejection);
[[nodiscard]] std::expected<Protocol::Local::AdoptWireRejection, AdoptTransactionWireError> adoptRejectionToWire(AdoptRejection);

/// Maps admission errors separately from retained outcomes or resumption evidence.
[[nodiscard]] std::expected<AdoptTransactionError, AdoptTransactionWireError> adoptRefusalFromWire(Protocol::Local::AdoptWireRefusal);
[[nodiscard]] std::expected<Protocol::Local::AdoptWireRefusal, AdoptTransactionWireError> adoptRefusalToWire(AdoptTransactionError);

/// Imports only the exact request's structurally valid retained result.
/// The client transaction still checks target continuity and ownership uncertainty.
[[nodiscard]] std::expected<AdoptOutcome, AdoptTransactionWireError> adoptOutcomeFromWire(const Protocol::Local::AdoptWireOutcome&, const AdoptOperation& expectedOperation, std::uint64_t expectedCorrelation);
/// Exports a complete retained result with an explicit nonzero request correlation.
[[nodiscard]] std::expected<Protocol::Local::AdoptWireOutcome, AdoptTransactionWireError> adoptOutcomeToWire(const AdoptOutcome&, std::uint64_t correlation);

/// Imports only the exact request's refusal; the result is not a terminal outcome.
[[nodiscard]] std::expected<AdoptTransactionError, AdoptTransactionWireError> adoptRefusedFromWire(const Protocol::Local::AdoptWireRefused&, const AdoptOperation& expectedOperation, std::uint64_t expectedCorrelation);
/// Exports an admission refusal without fabricating a disposition or target.
[[nodiscard]] std::expected<Protocol::Local::AdoptWireRefused, AdoptTransactionWireError> adoptRefusedToWire(const AdoptOperation&, AdoptTransactionError, std::uint64_t correlation);

} // namespace Ubiquitty::Session
