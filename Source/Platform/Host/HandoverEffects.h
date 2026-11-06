// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Platform/Host/HostCapabilityPolicy.h"
#include "Platform/Host/HostEffects.h"

#include <expected>

namespace Ubiquitty::Host {

/// Bounds the private host section independently of the incoming record.
struct HandoverEffectsLimits {
    std::size_t maxSectionBytes = 0;
    std::size_t maxIdentityBytes = 0;
    std::size_t maxNotificationBytes = 0;
    std::size_t maxClipboardBytes = 0;
};

/// Why held effects cannot be admitted into an unpublished successor.
enum class HandoverEffectsError : std::uint8_t {
    NewerVersion,
    Malformed,
    OverCapacity,
    DigestMismatch,
    AllocationRefused,
};

/// Owns private policy/clipboard delivery state, never host-wide authority.
struct HandoverEffects {
    HostCapabilityPolicy::Continuation policy;
    std::vector<HostEffectQueue::Effect> effects;
};

/// Encodes a frozen policy and its undelivered effects without consuming either.
///
/// Notification recorder callbacks and grant-store pointers do not travel.
/// The caller keeps both source owners unchanged throughout the operation.
/// Bounded strings/vectors retain their normal STL allocation behavior.
/// - Parameters:
///   - policy: The session's current policy and exact-operation consent owner.
///   - effects: The session's held deliveries, with their selected recipients.
///   - limits: The receiver's independent resource ceilings.
/// - Returns: Complete private section bytes or refusal without side effects.
[[nodiscard]] std::expected<std::vector<std::uint8_t>, HandoverEffectsError> encodeHandoverEffects(
    const HostCapabilityPolicy& policy, const HostEffectQueue& effects, const HandoverEffectsLimits& limits);

/// Admits all fields before allocating payload owners or constructing state.
///
/// The digest detects corruption, not sender authority. The caller authenticates
/// the local predecessor and must also carry the host-wide consent/grant tables.
/// The returned objects remain private until the complete handover is admitted.
/// - Parameters:
///   - bytes: Exactly one versioned host-effects section.
///   - limits: The receiver's independent ceilings.
/// - Returns: Owned policy and effects, or an explicit refusal.
[[nodiscard]] std::expected<HandoverEffects, HandoverEffectsError> decodeHandoverEffects(
    std::span<const std::uint8_t> bytes, const HandoverEffectsLimits& limits);

} // namespace Ubiquitty::Host
