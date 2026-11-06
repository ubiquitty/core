// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// The daemon's outbound caps, derived from the Sync accounting rather than a
// frame-size constant (Docs/sync.md §9). The daemon enforces them; the host
// stanzas read the same values instead of restating a formula.

#include "Core/Protocol/Sync/Envelope.h"
#include "Core/Protocol/Sync/NativeAttachment.h"

#include <cstddef>
#include <cstdint>

namespace Ubiquitty::Host {

/// The most a client's output holds of its screen plane: one Sync frame. The
/// screen plane queues nothing a slow client cannot take: its connection is
/// window-paced from the client's acknowledgements and marks the client owed
/// when the window is full, and the daemon moves its frames into the output
/// only while the output holds less than this. The lanes choose what leaves
/// next only among frames still in the connection, so a frame handed to the
/// output early is one an interactive frame queued after it cannot overtake.
inline constexpr std::size_t screenOutputLowWaterBytes = Protocol::Sync::frameHeaderLength + Protocol::Sync::maxEnvelopeHeaderBytes + Protocol::Sync::maxFrameBodyBytes;

/// Per-client cap on pending output, which bounds the memory a stalled reader
/// can pin in this persistent daemon: a client whose output would pass it is
/// closed with `Bye{OutputBacklog}`. Only frames outside the screen plane can
/// reach it, and what those retain is the connection's catalog allowance
/// (`Protocol::Sync::maxCatalogMessageBytes`: image sources, samples, effects
/// and clipboard payloads), which the client's receive owner admits in the
/// same amount. The cap holds that allowance in its envelope beside the one
/// screen frame the output may hold.
inline constexpr std::size_t clientBufferHighWater = Protocol::Sync::frameHeaderLength + sizeof(std::uint32_t) + Protocol::Sync::maxCatalogMessageBytes + screenOutputLowWaterBytes;

/// Host-wide cap on every client's pending output: four clients at the
/// per-client cap, the ratio the inbound aggregate uses.
inline constexpr std::size_t maxAggregateOutboundBytes = 4u * clientBufferHighWater;

} // namespace Ubiquitty::Host
