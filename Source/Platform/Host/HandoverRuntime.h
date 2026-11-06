// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/Runtime/FrameCoherence.h"
#include "Core/Runtime/RemoteInput.h"

#include <expected>

namespace Ubiquitty::Host {

/// Receiver-owned ceilings for fixed scheduling and paste metadata.
struct HandoverRuntimeLimits {
    std::size_t maxSectionBytes = 0;
    std::size_t maxPastes = 0;
};

/// Why a private runtime section cannot be admitted.
enum class HandoverRuntimeError : std::uint8_t {
    NewerVersion,
    Malformed,
    OverCapacity,
    DigestMismatch,
};

/// Exact runtime state, separate from terminal state and accepted PTY bytes.
struct HandoverRuntime {
    Runtime::FrameCoherence::Continuation coherence;
    Runtime::RemoteInputEncoder::Continuation input;
    bool operator==(const HandoverRuntime&) const = default;
};

/// Encodes the original clocks and active paste prefix without changing either.
///
/// The bounded output vector retains its normal STL allocation behavior. No
/// terminal, sink, clock or authority owner is consulted. Unused paste slots do
/// not travel; all five normalizer bytes within each active slot do.
/// - Parameters:
///   - state: A frozen continuation captured from the two runtime owners.
///   - limits: Independent receiver ceilings, never supplied by the record.
/// - Returns: One complete versioned section, or refusal without side effects.
[[nodiscard]] std::expected<std::vector<std::uint8_t>, HandoverRuntimeError> encodeHandoverRuntime(
    const HandoverRuntime& state, const HandoverRuntimeLimits& limits);

/// Decodes into fixed storage, validating with the runtime owners' own rules.
///
/// The digest detects corruption, not sender authority. The caller authenticates
/// the predecessor and restores terminal modes/counters, input ledger and PTY
/// suffix in the same unpublished transaction. It must retain the same monotonic
/// clock and receiver tuning; decoding neither rebases clocks nor emits bytes.
/// - Parameters:
///   - bytes: Exactly one section, with no trailing bytes.
///   - limits: Independent byte and paste-count ceilings.
/// - Returns: A private candidate, or refusal without allocating variable owners.
[[nodiscard]] std::expected<HandoverRuntime, HandoverRuntimeError> decodeHandoverRuntime(
    std::span<const std::uint8_t> bytes, const HandoverRuntimeLimits& limits);

} // namespace Ubiquitty::Host
