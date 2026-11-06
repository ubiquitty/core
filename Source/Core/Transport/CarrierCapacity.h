// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
// The cross-carrier outbound sizing contract. Protocol/Sync promises payloads
// its carriers must carry, and each carrier caps what it will accept; those
// numbers live in different modules on purpose (Transport knows nothing of
// frames, Protocol knows nothing of queues), so nothing but this header keeps
// them together — a protocol ceiling above a carrier cap makes the protocol
// promise a payload that ends the channel instead of carrying it.
//
// This header is the one place the relationship is written down: every capped
// carrier static_asserts its cap honours the guarantee, and Protocol/Sync
// static_asserts its frame ceilings fit under it. Raising any side is a build
// error until the number is re-decided here.

#include <cstddef>

namespace Ubiquitty::Transport {

/// The largest single outbound frame every carrier that declares an outbound
/// cap guarantees to accept on an idle queue.
///
/// The minimum over the declared carrier caps — today PollChannel's pending-out
/// cap, which bounds the SSH/subprocess fallback taken when QUIC has already
/// failed. A carrier that cannot state a capacity answers -1 from
/// ``Channel::outboundCapacityBytes`` and makes no promise here; momentary room
/// on a real queue stays a runtime question the caller pre-flights. Raising
/// this number raises the unsent queue every PollChannel holds, so a daemon
/// serving many wedged peers pins that much more memory per peer; lowering it
/// strands any Protocol/Sync ceiling pinned at it.
inline constexpr std::size_t guaranteedOutboundFrameBytes = 4u * 1024u * 1024u;

} // namespace Ubiquitty::Transport
